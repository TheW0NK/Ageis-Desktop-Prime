#include "script_internal.h"
#include "ui.h"

// The UI library for scripts: ui.* functions and widget members.

static struct script *app;
struct ui_window *script_window;

extern struct script_value (*script_widget_get)(struct script *s, struct widget *w, const char *name);
extern bool (*script_widget_set)(struct script *s, struct widget *w, const char *name, struct script_value v);
extern struct script_value (*script_widget_call)(struct script *s, struct widget *w, const char *name,
                                                 struct script_value *args, int nargs);

static void report(void)
{
    // Errors in handlers are shown, then the app carries on.
    if (*script_error(app)) {
        dprintf(STDERR_FILENO, "%s\n", script_error(app));
        ui_message(script_window, "Script error", script_error(app), "OK");
    }
}

// ---- Handlers ----

struct bound {
    struct script_value fn;         // a function value, or nil to call by name
    char name[64];
};

static void run_handler(struct widget *w, void *user)
{
    struct bound *b = user;
    struct script_value arg = script_widget(w), r;
    bool ok;

    if (b->fn.type == S_FN || b->fn.type == S_NATIVE) {
        struct script_value fn = b->fn;

        script_retain(fn);
        // Call through a temporary global so script_call can find it.
        script_set_global(app, "__handler", fn);
        script_release(fn);
        ok = script_call(app, "__handler", &arg, 1, &r);
    } else {
        ok = script_call(app, b->name, &arg, 1, &r);
    }
    if (ok)
        script_release(r);
    else
        report();
}

static ui_handler resolve(const char *name, void **user)
{
    struct bound *b;

    if (!app || !script_has_function(app, name) || !(b = calloc(1, sizeof(*b))))
        return NULL;
    strlcpy(b->name, name, sizeof(b->name));
    *user = b;
    return run_handler;
}

// ---- Widget members ----

static struct script_value widget_get(struct script *s, struct widget *w, const char *name)
{
    (void)s;
    if (!strcmp(name, "text"))
        return script_str(ui_text(w));
    if (!strcmp(name, "value") || !strcmp(name, "checked"))
        return !strcmp(name, "checked") ? script_bool(ui_value(w) != 0) : script_num(ui_value(w));
    if (!strcmp(name, "selected"))
        return script_num(ui_list_selected(w));
    if (!strcmp(name, "count"))
        return script_num(ui_list_count(w));
    if (!strcmp(name, "visible"))
        return script_bool(ui_visible(w));
    if (!strcmp(name, "id"))
        return script_str(ui_id(w) ? ui_id(w) : "");
    if (!strcmp(name, "item")) {
        const char *it = ui_list_item(w, ui_list_selected(w));

        return it ? script_str(it) : script_nil();
    }
    {
        const char *a = ui_attr(w, name);

        return a ? script_str(a) : script_nil();
    }
}

static bool widget_set(struct script *s, struct widget *w, const char *name, struct script_value v)
{
    char *t;

    (void)s;
    if (!strcmp(name, "text")) {
        t = script_to_string(v);
        ui_set_text(w, t);
        free(t);
    } else if (!strcmp(name, "value")) {
        if (v.type == S_NUM)
            ui_set_value(w, v.n);
    } else if (!strcmp(name, "checked")) {
        ui_set_value(w, script_truthy(v));
    } else if (!strcmp(name, "selected")) {
        if (v.type == S_NUM)
            ui_list_select(w, (int)v.n);
    } else if (!strcmp(name, "enabled")) {
        ui_set_enabled(w, script_truthy(v));
    } else if (!strcmp(name, "visible")) {
        ui_set_visible(w, script_truthy(v));
    } else {
        // Any other attribute, as text.
        t = script_to_string(v);
        ui_set_attr(w, name, t);
        free(t);
    }
    return true;
}

static struct script_value widget_call(struct script *s, struct widget *w, const char *name,
                                       struct script_value *args, int nargs)
{
    if (!strcmp(name, "add")) {
        for (int i = 0; i < nargs; i++) {
            char *t = script_to_string(args[i]);

            ui_list_add(w, t);
            free(t);
        }
        return script_num(ui_list_count(w));
    }
    if (!strcmp(name, "clear")) {
        ui_list_clear(w);
        return script_nil();
    }
    if (!strcmp(name, "remove") && nargs && args[0].type == S_NUM) {
        ui_list_remove(w, (int)args[0].n);
        return script_nil();
    }
    if (!strcmp(name, "get") && nargs && args[0].type == S_NUM) {
        const char *it = ui_list_item(w, (int)args[0].n);

        return it ? script_str(it) : script_nil();
    }
    if (!strcmp(name, "set") && nargs > 1 && args[0].type == S_NUM) {
        char *t = script_to_string(args[1]);

        ui_list_set_item(w, (int)args[0].n, t);
        free(t);
        return script_nil();
    }
    if (!strcmp(name, "focus")) {
        ui_focus(w);
        return script_nil();
    }
    if (!strcmp(name, "insert") && nargs) {
        char *t = script_to_string(args[0]);

        ui_textarea_insert(w, t);
        free(t);
        return script_nil();
    }
    if (!strcmp(name, "on") && nargs > 1 && args[0].type == S_STR) {
        struct bound *b = calloc(1, sizeof(*b));

        if (!b)
            return script_nil();
        b->fn = args[1];
        script_retain(b->fn);
        ui_set_handler(w, script_text(args[0]), run_handler, b);
        return script_nil();
    }
    return script_fail(s, "widgets have no \"%s\"", name);
}

// ---- ui.* ----

static const char *text_arg(struct script_value *a, int n, int i, const char *def)
{
    return i < n && a[i].type == S_STR ? script_text(a[i]) : def;
}

static struct script_value u_get(struct script *s, struct script_value *a, int n)
{
    struct widget *w;

    if (!n || a[0].type != S_STR)
        return script_fail(s, "ui.get needs an id");
    if (!(w = ui_get(script_window, script_text(a[0]))))
        return script_fail(s, "there is no element with id \"%s\"", script_text(a[0]));
    return script_widget(w);
}

static struct script_value u_message(struct script *s, struct script_value *a, int n)
{
    char *t = script_to_string(n ? a[0] : script_nil());
    int r;

    (void)s;
    r = ui_message(script_window, text_arg(a, n, 1, "Message"), t, text_arg(a, n, 2, "OK"));
    free(t);
    return script_num(r);
}

static struct script_value u_confirm(struct script *s, struct script_value *a, int n)
{
    char *t = script_to_string(n ? a[0] : script_nil());
    int r;

    (void)s;
    r = ui_message(script_window, text_arg(a, n, 1, "Question"), t, "Yes|No");
    free(t);
    return script_bool(r == 0);
}

static struct script_value u_ask(struct script *s, struct script_value *a, int n)
{
    char *r = ui_prompt(script_window, text_arg(a, n, 2, "Question"), text_arg(a, n, 0, ""), text_arg(a, n, 1, ""));
    struct script_value v;

    (void)s;
    if (!r)
        return script_nil();
    v = script_str(r);
    free(r);
    return v;
}

static struct script_value file_dialog(struct script_value *a, int n, bool save)
{
    char *r = ui_file_dialog_filtered(script_window, text_arg(a, n, 0, save ? "Save" : "Open"), NULL, save,
                                      text_arg(a, n, 1, NULL), text_arg(a, n, 2, NULL));
    struct script_value v;

    if (!r)
        return script_nil();
    v = script_str(r);
    free(r);
    return v;
}

static struct script_value u_open_file(struct script *s, struct script_value *a, int n)
{
    (void)s;
    return file_dialog(a, n, false);
}

static struct script_value u_save_file(struct script *s, struct script_value *a, int n)
{
    (void)s;
    return file_dialog(a, n, true);
}

struct timer_cb {
    struct script_value fn;
};

static bool timer_fired(void *user)
{
    struct timer_cb *t = user;
    struct script_value r;
    bool keep = true;

    script_set_global(app, "__timer", t->fn);
    if (script_call(app, "__timer", NULL, 0, &r)) {
        // Returning false stops the timer.
        keep = !(r.type == S_BOOL && !r.b);
        script_release(r);
    } else {
        report();
        keep = false;
    }
    if (!keep) {
        script_release(t->fn);
        free(t);
    }
    return keep;
}

static struct script_value u_timer(struct script *s, struct script_value *a, int n)
{
    struct timer_cb *t;

    if (n < 2 || a[0].type != S_NUM || (a[1].type != S_FN && a[1].type != S_NATIVE))
        return script_fail(s, "ui.timer needs milliseconds and a function");
    if (!(t = calloc(1, sizeof(*t))))
        return script_nil();
    t->fn = a[1];
    script_retain(t->fn);
    return script_num(ui_timer((uint64_t)a[0].n, timer_fired, t));
}

static struct script_value u_cancel(struct script *s, struct script_value *a, int n)
{
    (void)s;
    if (n && a[0].type == S_NUM)
        ui_timer_cancel((int)a[0].n);
    return script_nil();
}

static struct script_value u_quit(struct script *s, struct script_value *a, int n)
{
    (void)s;
    ui_quit(n && a[0].type == S_NUM ? (int)a[0].n : 0);
    return script_nil();
}

static struct script_value u_title(struct script *s, struct script_value *a, int n)
{
    (void)s;
    ui_window_set_title(script_window, text_arg(a, n, 0, ""));
    return script_nil();
}

static struct script_value u_clipboard(struct script *s, struct script_value *a, int n)
{
    (void)s;
    if (n && a[0].type == S_STR)
        ui_clipboard_set(script_text(a[0]));
    return script_str(ui_clipboard_get());
}

static struct script_value u_open(struct script *s, struct script_value *a, int n)
{
    struct app_info info;

    (void)s;
    // ui.open(path): open a file or folder with its app.
    if (!n || a[0].type != S_STR)
        return script_bool(false);
    if (app_for_file(script_text(a[0]), &info) == 0)
        return script_bool(app_launch(&info, script_text(a[0])) > 0);
    if (app_find("files", &info) == 0)
        return script_bool(app_launch(&info, script_text(a[0])) > 0);
    return script_bool(false);
}

void script_add_ui(struct script *s)
{
    static const struct {
        const char *name;
        script_native fn;
    } fns[] = {
        { "get", u_get }, { "message", u_message }, { "confirm", u_confirm }, { "ask", u_ask },
        { "open_file", u_open_file }, { "save_file", u_save_file }, { "timer", u_timer },
        { "cancel", u_cancel }, { "quit", u_quit }, { "title", u_title }, { "clipboard", u_clipboard },
        { "open", u_open },
    };
    struct script_value ui = { .type = S_MAP };

    app = s;
    script_widget_get = widget_get;
    script_widget_set = widget_set;
    script_widget_call = widget_call;
    ui.m = calloc(1, sizeof(*ui.m));
    if (!ui.m)
        return;
    ui.m->refs = 1;
    script_set_global(s, "ui", ui);
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        // ui.name is a native function stored in the map.
        struct script_value f = { .type = S_NATIVE };
        struct script_value key;

        f.nf = calloc(1, sizeof(*f.nf));
        if (!f.nf)
            continue;
        f.nf->refs = 1;
        strlcpy(f.nf->name, fns[i].name, sizeof(f.nf->name));
        f.nf->fn = fns[i].fn;
        key = script_str(fns[i].name);
        script_map_set(ui, script_text(key), f);
        script_release(key);
        script_release(f);
    }
    script_release(ui);
    ui_set_handler_resolver(resolve);
}
