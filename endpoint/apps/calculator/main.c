#include "aegis.h"
#include "ui.h"
#include <math.h>

// Calculator: type or click an expression; Enter evaluates it.

static const char page[] =
    "<window title='Calculator' width='420' padding='12' spacing='8' resizable='false'>"
    "  <hbox spacing='6'>"
    "    <toggle id='sci' text='Scientific' onchange='mode'/>"
    "    <spacer/>"
    "    <dropdown id='angle' width='110'><option>Degrees</option><option>Radians</option></dropdown>"
    "  </hbox>"
    "  <label id='expr' textalign='right' dim='true' mono='true' text=' '/>"
    "  <label id='display' textalign='right' size='huge' text='0'/>"
    "  <grid id='scikeys' columns='5' spacing='6' stretch='0,1,2,3,4' hidden='true'>"
    "    <button focusable='false' text='sin' onclick='fn'/><button focusable='false' text='cos' onclick='fn'/><button focusable='false' text='tan' onclick='fn'/>"
    "    <button focusable='false' text='ln' onclick='fn'/><button focusable='false' text='log' onclick='fn'/>"
    "    <button focusable='false' text='asin' onclick='fn'/><button focusable='false' text='acos' onclick='fn'/><button focusable='false' text='atan' onclick='fn'/>"
    "    <button focusable='false' text='sqrt' onclick='fn'/><button focusable='false' text='^' onclick='key'/>"
    "    <button focusable='false' text='pi' onclick='key'/><button focusable='false' text='e' onclick='key'/><button focusable='false' text='!' onclick='key'/>"
    "    <button focusable='false' text='(' onclick='key'/><button focusable='false' text=')' onclick='key'/>"
    "  </grid>"
    "  <grid columns='4' spacing='6' stretch='0,1,2,3'>"
    "    <button focusable='false' text='MC' onclick='mem'/><button focusable='false' text='MR' onclick='mem'/>"
    "    <button focusable='false' text='M+' onclick='mem'/><button focusable='false' text='M-' onclick='mem'/>"
    "    <button focusable='false' text='C' onclick='clear'/><button focusable='false' text='&#x232B;' onclick='back'/>"
    "    <button focusable='false' text='%' onclick='key'/><button focusable='false' text='&#xF7;' onclick='key'/>"
    "    <button focusable='false' text='7' onclick='key'/><button focusable='false' text='8' onclick='key'/><button focusable='false' text='9' onclick='key'/>"
    "    <button focusable='false' text='&#xD7;' onclick='key'/>"
    "    <button focusable='false' text='4' onclick='key'/><button focusable='false' text='5' onclick='key'/><button focusable='false' text='6' onclick='key'/>"
    "    <button focusable='false' text='&#x2212;' onclick='key'/>"
    "    <button focusable='false' text='1' onclick='key'/><button focusable='false' text='2' onclick='key'/><button focusable='false' text='3' onclick='key'/>"
    "    <button focusable='false' text='+' onclick='key'/>"
    "    <button focusable='false' text='&#xB1;' onclick='negate'/><button focusable='false' text='0' onclick='key'/><button focusable='false' text='.' onclick='key'/>"
    "    <button focusable='false' text='=' primary='true' onclick='equals'/>"
    "  </grid>"
    "  <label id='memory' dim='true' text=''/>"
    "</window>";

static struct ui_window *win;
static char expr[256];
static double memory, last_answer;
static bool have_memory, just_evaluated;

// ---- Evaluation: recursive descent over doubles ----

struct parser {
    const char *p;
    bool error;
    bool radians;
};

static double expression(struct parser *P);

static void skip(struct parser *P)
{
    while (*P->p == ' ')
        P->p++;
}

static bool take(struct parser *P, const char *tok)
{
    size_t n = strlen(tok);

    skip(P);
    if (!strncmp(P->p, tok, n)) {
        P->p += n;
        return true;
    }
    return false;
}

static double factorial(double v)
{
    double r = 1;

    if (v < 0 || v != floor(v) || v > 170)
        return v > 170 ? INFINITY : NAN;
    for (int i = 2; i <= (int)v; i++)
        r *= i;
    return r;
}

static double primary(struct parser *P)
{
    static const struct {
        const char *name;
        int id;
    } fns[] = { { "asin", 1 }, { "acos", 2 }, { "atan", 3 }, { "sin", 4 }, { "cos", 5 }, { "tan", 6 },
                { "sqrt", 7 }, { "ln", 8 }, { "log", 9 }, { "abs", 10 }, { "exp", 11 } };
    double v;

    skip(P);
    if (take(P, "(")) {
        v = expression(P);
        if (!take(P, ")"))
            P->error = true;
        return v;
    }
    if (take(P, "pi") || take(P, "\xCF\x80"))
        return M_PI;
    if (take(P, "ans"))
        return last_answer;
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        if (take(P, fns[i].name)) {
            double a = primary(P), k = P->radians ? 1 : M_PI / 180;

            switch (fns[i].id) {
            case 1: return asin(a) / k;
            case 2: return acos(a) / k;
            case 3: return atan(a) / k;
            case 4: return sin(a * k);
            case 5: return cos(a * k);
            case 6: return tan(a * k);
            case 7: return sqrt(a);
            case 8: return log(a);
            case 9: return log10(a);
            case 10: return fabs(a);
            case 11: return exp(a);
            }
        }
    }
    if (take(P, "e") && !isalpha((unsigned char)*P->p))
        return M_E;
    if (isdigit((unsigned char)*P->p) || *P->p == '.') {
        char *end;

        v = strtod(P->p, &end);
        if (end == P->p)
            P->error = true;
        P->p = end;
        return v;
    }
    P->error = true;
    return 0;
}

static double postfix(struct parser *P)
{
    double v = primary(P);

    for (;;) {
        if (take(P, "!"))
            v = factorial(v);
        else if (take(P, "%"))
            v /= 100;
        else
            return v;
    }
}

static double unary(struct parser *P)
{
    if (take(P, "-") || take(P, "\xE2\x88\x92"))
        return -unary(P);
    if (take(P, "+"))
        return unary(P);
    return postfix(P);
}

static double power(struct parser *P)
{
    double base = unary(P);

    if (take(P, "^"))
        return pow(base, power(P));     // right associative
    return base;
}

static double term(struct parser *P)
{
    double v = power(P);

    for (;;) {
        if (take(P, "*") || take(P, "\xC3\x97"))
            v *= power(P);
        else if (take(P, "/") || take(P, "\xC3\xB7"))
            v /= power(P);
        else
            return v;
    }
}

static double expression(struct parser *P)
{
    double v = term(P);

    for (;;) {
        if (take(P, "+"))
            v += term(P);
        else if (take(P, "-") || take(P, "\xE2\x88\x92"))
            v -= term(P);
        else
            return v;
    }
}

static bool evaluate(const char *s, double *out)
{
    struct parser P = { s, false, ui_list_selected(ui_get(win, "angle")) == 1 };
    double v = expression(&P);

    skip(&P);
    if (P.error || *P.p)
        return false;
    *out = v;
    return true;
}

static void format(double v, char *buf, size_t size)
{
    if (isnan(v)) {
        strlcpy(buf, "Not a number", size);
        return;
    }
    if (isinf(v)) {
        strlcpy(buf, v > 0 ? "Infinity" : "-Infinity", size);
        return;
    }
    // Round away binary noise (0.1 + 0.2).
    if (fabs(v) < 1e-12)
        v = 0;
    snprintf(buf, size, "%.12g", v);
}

// ---- Display ----

static void show(void)
{
    double v;
    char buf[64];

    ui_set_text(ui_get(win, "display"), *expr ? expr : "0");
    // A live preview of the result while typing.
    if (*expr && !just_evaluated && evaluate(expr, &v)) {
        format(v, buf, sizeof(buf));
        ui_set_text(ui_get(win, "expr"), strcmp(buf, expr) ? buf : " ");
    } else if (!just_evaluated) {
        ui_set_text(ui_get(win, "expr"), " ");
    }
}

static void append(const char *s)
{
    if (just_evaluated) {
        // A digit starts afresh; an operator continues from the answer.
        if (isdigit((unsigned char)s[0]) || s[0] == '.' || s[0] == '(' || isalpha((unsigned char)s[0]))
            expr[0] = 0;
        just_evaluated = false;
    }
    if (strlen(expr) + strlen(s) < sizeof(expr) - 1)
        strcat(expr, s);
    show();
}

static void on_key(struct widget *w, void *u)
{
    (void)u;
    append(ui_text(w));
}

static void on_fn(struct widget *w, void *u)
{
    char buf[16];

    (void)u;
    snprintf(buf, sizeof(buf), "%s(", ui_text(w));
    append(buf);
}

static void on_clear(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    expr[0] = 0;
    just_evaluated = false;
    ui_set_text(ui_get(win, "expr"), " ");
    show();
}

static void on_back(struct widget *w, void *u)
{
    size_t n = strlen(expr);

    (void)w;
    (void)u;
    if (just_evaluated) {
        on_clear(NULL, NULL);
        return;
    }
    // Remove one character (UTF-8 aware) or a whole function name.
    if (n && expr[n - 1] == '(') {
        int i = n - 1;

        while (i > 0 && isalpha((unsigned char)expr[i - 1]))
            i--;
        expr[i] = 0;
    } else if (n) {
        while (n > 0 && (expr[n - 1] & 0xC0) == 0x80)
            n--;
        expr[n ? n - 1 : 0] = 0;
    }
    show();
}

static void on_equals(struct widget *w, void *u)
{
    double v;
    char buf[64], line[300];

    (void)w;
    (void)u;
    if (!*expr)
        return;
    // Close any parentheses left open.
    {
        int depth = 0;

        for (char *p = expr; *p; p++)
            depth += (*p == '(') - (*p == ')');
        while (depth-- > 0 && strlen(expr) < sizeof(expr) - 1)
            strcat(expr, ")");
    }
    if (!evaluate(expr, &v)) {
        ui_set_text(ui_get(win, "expr"), "That expression is not complete.");
        return;
    }
    format(v, buf, sizeof(buf));
    snprintf(line, sizeof(line), "%s =", expr);
    ui_set_text(ui_get(win, "expr"), line);
    last_answer = v;
    strlcpy(expr, isfinite(v) ? buf : "", sizeof(expr));
    just_evaluated = true;
    ui_set_text(ui_get(win, "display"), buf);
}

static void on_negate(struct widget *w, void *u)
{
    char buf[260];

    (void)w;
    (void)u;
    if (!*expr)
        return;
    if (expr[0] == '-' && expr[1] == '(' && expr[strlen(expr) - 1] == ')') {
        memmove(expr, expr + 2, strlen(expr) - 2);
        expr[strlen(expr) - 3] = 0;
    } else {
        snprintf(buf, sizeof(buf), "-(%s)", expr);
        strlcpy(expr, buf, sizeof(expr));
    }
    just_evaluated = false;
    show();
}

static void on_mem(struct widget *w, void *u)
{
    const char *t = ui_text(w);
    double v;
    char buf[64], label[96];

    (void)u;
    if (!strcmp(t, "MC")) {
        memory = 0;
        have_memory = false;
    } else if (!strcmp(t, "MR")) {
        if (have_memory) {
            format(memory, buf, sizeof(buf));
            append(buf);
        }
    } else if (evaluate(*expr ? expr : "0", &v)) {
        memory += !strcmp(t, "M+") ? v : -v;
        have_memory = true;
    }
    if (have_memory) {
        format(memory, buf, sizeof(buf));
        snprintf(label, sizeof(label), "Memory: %s", buf);
    } else {
        label[0] = 0;
    }
    ui_set_text(ui_get(win, "memory"), label);
}

static void on_mode(struct widget *w, void *u)
{
    (void)u;
    ui_set_visible(ui_get(win, "scikeys"), ui_value(w) != 0);
    ui_window_fit(win);
}

static void keyboard(struct ui_window *wnd, struct wm_event *ev, void *u)
{
    (void)wnd;
    (void)u;
    if (!ev->value)
        return;
    if (ev->key == KEY_ENTER || ev->key == KEY_KPENTER || (ev->text[0] == '=' && !ev->text[1]))
        on_equals(NULL, NULL);
    else if (ev->key == KEY_BACKSPACE)
        on_back(NULL, NULL);
    else if (ev->key == KEY_ESC || ev->key == KEY_DELETE)
        on_clear(NULL, NULL);
    else if ((ev->mods & MOD_CTRL) && ev->key == KEY_A + 2) {
        ui_clipboard_set(*expr ? expr : "0");
    } else if ((ev->mods & MOD_CTRL) && ev->key == KEY_A + 21) {
        append(ui_clipboard_get());
    } else if (ev->text[0] && !(ev->mods & (MOD_CTRL | MOD_ALT)) && strchr("0123456789.+-*/^()%!", ev->text[0])) {
        const char *s = ev->text;

        if (*s == '*')
            s = "\xC3\x97";
        else if (*s == '/')
            s = "\xC3\xB7";
        append(s);
    } else if (ev->text[0] && isalpha((unsigned char)ev->text[0]) && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        append(ev->text);   // function names and constants can be typed
    }
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "key", on_key }, { "fn", on_fn }, { "clear", on_clear }, { "back", on_back }, { "equals", on_equals },
        { "negate", on_negate }, { "mem", on_mem }, { "mode", on_mode }, { NULL, NULL },
    };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "calculator")))
        return 1;
    ui_on_key(win, keyboard, NULL);
    ui_window_show(win);
    return ui_run();
}
