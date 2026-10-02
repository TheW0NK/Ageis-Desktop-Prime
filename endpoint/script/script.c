#include "script.h"
#include "gfx.h"
#include <math.h>

// The AegisScript interpreter: lexer, parser to a syntax tree, and a
// tree-walking evaluator. Heap values are reference counted.

#include "script_internal.h"

struct env {
    int refs;
    struct env *parent;
    int n, cap;
    const char **names;
    struct script_value *vals;
};

// ---- Syntax tree ----

enum kind {
    N_NUM, N_STR, N_NAME, N_LIST, N_MAP, N_NIL, N_TRUE, N_FALSE,
    N_UNARY, N_BINARY, N_AND, N_OR, N_CALL, N_INDEX, N_MEMBER, N_ASSIGN, N_FN,
    N_LET, N_BLOCK, N_IF, N_WHILE, N_FOR, N_RETURN, N_BREAK, N_CONTINUE, N_EXPR, N_FNDEF,
};

struct node {
    enum kind kind;
    int line;
    int op;
    double num;
    char *str;                      // names, string literals, function names
    struct node *a, *b, *c;
    struct node **list;             // arguments, statements, elements, parameters (as N_NAME)
    int nlist;
    struct node **list2;            // map values
};

struct script {
    struct env *globals;
    char error[300];
    const char *source_name;
    // Unwinding.
    enum { RUN_NORMAL, RUN_BREAK, RUN_CONTINUE, RUN_RETURN, RUN_ERROR } state;
    struct script_value ret;
    int depth;
    // Every tree ever parsed lives until the script is freed.
    struct node **trees;
    int ntrees;
    // Names are interned so they outlive their trees.
    char **names;
    int nnames;
};

static struct script_value nil_value = { .type = S_NIL };

// ---- Values ----

struct script_value script_nil(void)
{
    return nil_value;
}

struct script_value script_num(double n)
{
    struct script_value v = { .type = S_NUM };

    v.n = n;
    return v;
}

struct script_value script_bool(bool b)
{
    struct script_value v = { .type = S_BOOL };

    v.b = b;
    return v;
}

struct script_value script_str_len(const char *text, size_t len)
{
    struct script_value v = { .type = S_STR };
    struct s_str *s = malloc(sizeof(*s) + len + 1);

    if (!s)
        return nil_value;
    s->refs = 1;
    s->len = len;
    memcpy(s->data, text, len);
    s->data[len] = 0;
    v.s = s;
    return v;
}

struct script_value script_str(const char *text)
{
    return script_str_len(text ? text : "", text ? strlen(text) : 0);
}

struct script_value script_list(void)
{
    struct script_value v = { .type = S_LIST };

    v.l = calloc(1, sizeof(*v.l));
    if (!v.l)
        return nil_value;
    v.l->refs = 1;
    return v;
}

static struct script_value new_map(void)
{
    struct script_value v = { .type = S_MAP };

    v.m = calloc(1, sizeof(*v.m));
    if (!v.m)
        return nil_value;
    v.m->refs = 1;
    return v;
}

struct script_value script_widget(struct widget *w)
{
    struct script_value v = { .type = w ? S_WIDGET : S_NIL };

    v.w = w;
    return v;
}

void script_retain(struct script_value v)
{
    switch (v.type) {
    case S_STR: v.s->refs++; break;
    case S_LIST: v.l->refs++; break;
    case S_MAP: v.m->refs++; break;
    case S_FN: v.f->refs++; break;
    case S_NATIVE: v.nf->refs++; break;
    default: break;
    }
}

static void env_release(struct env *e);

void script_release(struct script_value v)
{
    switch (v.type) {
    case S_STR:
        if (--v.s->refs == 0)
            free(v.s);
        break;
    case S_LIST:
        if (--v.l->refs == 0) {
            for (int i = 0; i < v.l->n; i++)
                script_release(v.l->items[i]);
            free(v.l->items);
            free(v.l);
        }
        break;
    case S_MAP:
        if (--v.m->refs == 0) {
            for (int i = 0; i < v.m->n; i++) {
                if (--v.m->keys[i]->refs == 0)
                    free(v.m->keys[i]);
                script_release(v.m->vals[i]);
            }
            free(v.m->keys);
            free(v.m->vals);
            free(v.m);
        }
        break;
    case S_FN:
        if (--v.f->refs == 0) {
            env_release(v.f->closure);
            free(v.f);
        }
        break;
    case S_NATIVE:
        if (--v.nf->refs == 0)
            free(v.nf);
        break;
    default:
        break;
    }
}

void script_list_push(struct script_value list, struct script_value v)
{
    struct s_list *l = list.l;

    if (list.type != S_LIST)
        return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        struct script_value *items = realloc(l->items, cap * sizeof(*items));

        if (!items)
            return;
        l->items = items;
        l->cap = cap;
    }
    script_retain(v);
    l->items[l->n++] = v;
}

const char *script_text(struct script_value v)
{
    return v.type == S_STR ? v.s->data : "";
}

bool script_truthy(struct script_value v)
{
    switch (v.type) {
    case S_NIL: return false;
    case S_BOOL: return v.b;
    case S_NUM: return v.n != 0;
    case S_STR: return v.s->len != 0;
    case S_LIST: return v.l->n != 0;
    default: return true;
    }
}

static void format_num(double n, char *buf, size_t size)
{
    if (n == (int64_t)n && fabs(n) < 1e15)
        snprintf(buf, size, "%ld", (long)(int64_t)n);
    else
        snprintf(buf, size, "%.14g", n);
}

// A growing string buffer.
struct sb {
    char *s;
    size_t len, cap;
};

static void sb_add(struct sb *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = MAX(b->cap * 2, b->len + n + 32);
        char *t = realloc(b->s, cap);

        if (!t)
            return;
        b->s = t;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void to_string(struct script_value v, struct sb *b, bool quote, int depth)
{
    char buf[64];

    if (depth > 8) {
        sb_add(b, "...", 3);
        return;
    }
    switch (v.type) {
    case S_NIL: sb_add(b, "nil", 3); break;
    case S_BOOL: sb_add(b, v.b ? "true" : "false", v.b ? 4 : 5); break;
    case S_NUM: format_num(v.n, buf, sizeof(buf)); sb_add(b, buf, strlen(buf)); break;
    case S_STR:
        if (quote)
            sb_add(b, "\"", 1);
        sb_add(b, v.s->data, v.s->len);
        if (quote)
            sb_add(b, "\"", 1);
        break;
    case S_LIST:
        sb_add(b, "[", 1);
        for (int i = 0; i < v.l->n; i++) {
            if (i)
                sb_add(b, ", ", 2);
            to_string(v.l->items[i], b, true, depth + 1);
        }
        sb_add(b, "]", 1);
        break;
    case S_MAP:
        sb_add(b, "{", 1);
        for (int i = 0; i < v.m->n; i++) {
            if (i)
                sb_add(b, ", ", 2);
            sb_add(b, v.m->keys[i]->data, v.m->keys[i]->len);
            sb_add(b, ": ", 2);
            to_string(v.m->vals[i], b, true, depth + 1);
        }
        sb_add(b, "}", 1);
        break;
    case S_FN: sb_add(b, "<function>", 10); break;
    case S_NATIVE: sb_add(b, "<built-in>", 10); break;
    case S_WIDGET: sb_add(b, "<widget>", 8); break;
    }
}

char *script_to_string(struct script_value v)
{
    struct sb b = { 0 };

    to_string(v, &b, false, 0);
    return b.s ? b.s : strdup("");
}

// ---- Maps ----

static int map_find(struct s_map *m, const char *key)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->keys[i]->data, key))
            return i;
    return -1;
}

static struct script_value map_get(struct s_map *m, const char *key)
{
    int i = map_find(m, key);

    return i >= 0 ? m->vals[i] : nil_value;
}

static void map_set(struct s_map *m, const char *key, struct script_value v)
{
    int i = map_find(m, key);

    script_retain(v);
    if (i >= 0) {
        script_release(m->vals[i]);
        m->vals[i] = v;
        return;
    }
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        struct s_str **k = realloc(m->keys, cap * sizeof(*k));
        struct script_value *vals;

        if (!k)
            return;
        m->keys = k;
        if (!(vals = realloc(m->vals, cap * sizeof(*vals))))
            return;
        m->vals = vals;
        m->cap = cap;
    }
    {
        struct script_value ks = script_str(key);

        m->keys[m->n] = ks.s;
        m->vals[m->n++] = v;
    }
}

void script_map_set(struct script_value map, const char *key, struct script_value v)
{
    if (map.type == S_MAP)
        map_set(map.m, key, v);
}

// ---- Environments ----

static struct env *env_new(struct env *parent)
{
    struct env *e = calloc(1, sizeof(*e));

    if (!e)
        return NULL;
    e->refs = 1;
    e->parent = parent;
    if (parent)
        parent->refs++;
    return e;
}

static void env_release(struct env *e)
{
    while (e && --e->refs == 0) {
        struct env *p = e->parent;

        for (int i = 0; i < e->n; i++)
            script_release(e->vals[i]);
        free(e->names);
        free(e->vals);
        free(e);
        e = p;
    }
}

static struct script_value *env_lookup(struct env *e, const char *name)
{
    for (; e; e = e->parent)
        for (int i = 0; i < e->n; i++)
            if (e->names[i] == name || !strcmp(e->names[i], name))
                return &e->vals[i];
    return NULL;
}

// Defines name in e (replacing a binding of the same scope).
static void env_define(struct env *e, const char *name, struct script_value v)
{
    for (int i = 0; i < e->n; i++) {
        if (!strcmp(e->names[i], name)) {
            script_retain(v);
            script_release(e->vals[i]);
            e->vals[i] = v;
            return;
        }
    }
    if (e->n == e->cap) {
        int cap = e->cap ? e->cap * 2 : 8;
        const char **names = realloc(e->names, cap * sizeof(char *));
        struct script_value *vals;

        if (!names)
            return;
        e->names = names;
        if (!(vals = realloc(e->vals, cap * sizeof(*vals))))
            return;
        e->vals = vals;
        e->cap = cap;
    }
    script_retain(v);
    e->names[e->n] = name;
    e->vals[e->n++] = v;
}

// ---- Errors ----

static void set_error(struct script *s, int line, const char *fmt, va_list ap)
{
    int n;

    if (s->state == RUN_ERROR)
        return;
    n = snprintf(s->error, sizeof(s->error), "%s:%d: ", s->source_name ? s->source_name : "script", line);
    vsnprintf(s->error + n, sizeof(s->error) - n, fmt, ap);
    s->state = RUN_ERROR;
}

static void error_at(struct script *s, int line, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    set_error(s, line, fmt, ap);
    va_end(ap);
}

static int native_line;

struct script_value script_fail(struct script *s, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    set_error(s, native_line, fmt, ap);
    va_end(ap);
    return nil_value;
}

const char *script_error(struct script *s)
{
    return s->error;
}

// ---- Lexer ----

enum tok {
    T_EOF, T_NUM, T_STR, T_NAME, T_PUNCT,
};

struct lexer {
    struct script *s;
    const char *p;
    int line;
    enum tok tok;
    char text[256];                 // names, punctuation, string contents
    char *str;                      // long strings
    double num;
    int tok_line;
};

static const char *intern(struct script *s, const char *name)
{
    char **names;

    for (int i = 0; i < s->nnames; i++)
        if (!strcmp(s->names[i], name))
            return s->names[i];
    if (!(names = realloc(s->names, (s->nnames + 1) * sizeof(char *))))
        return "?";
    s->names = names;
    s->names[s->nnames] = strdup(name);
    return s->names[s->nnames++] ? s->names[s->nnames - 1] : "?";
}

static void next(struct lexer *L)
{
    const char *p = L->p;

    free(L->str);
    L->str = NULL;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (*p == '\n')
                L->line++;
            p++;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n')
                    L->line++;
                p++;
            }
            if (*p)
                p += 2;
            continue;
        }
        break;
    }
    L->tok_line = L->line;
    if (!*p) {
        L->tok = T_EOF;
        L->text[0] = 0;
    } else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
        char *end;

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            L->num = (double)strtoul(p + 2, &end, 16);
        } else {
            L->num = strtod(p, &end);
        }
        p = end;
        L->tok = T_NUM;
    } else if (isalpha((unsigned char)*p) || *p == '_' || (unsigned char)*p >= 0x80) {
        int n = 0;

        while ((isalnum((unsigned char)*p) || *p == '_' || (unsigned char)*p >= 0x80) && n < 255)
            L->text[n++] = *p++;
        L->text[n] = 0;
        L->tok = T_NAME;
    } else if (*p == '"' || *p == '\'') {
        char q = *p++;
        struct sb b = { 0 };

        while (*p && *p != q) {
            char c = *p++;

            if (c == '\n')
                L->line++;
            if (c == '\\' && *p) {
                c = *p++;
                switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': c = 0; break;
                case 'u': {
                    char hex[5] = { 0 }, enc[4];
                    uint32_t cp;

                    for (int i = 0; i < 4 && isxdigit((unsigned char)*p); i++)
                        hex[i] = *p++;
                    cp = strtoul(hex, NULL, 16);
                    sb_add(&b, enc, utf8_encode(cp, enc));
                    continue;
                }
                }
            }
            sb_add(&b, &c, 1);
        }
        if (*p == q)
            p++;
        else
            error_at(L->s, L->line, "a string is not closed");
        L->str = b.s ? b.s : strdup("");
        L->tok = T_STR;
    } else {
        static const char *const two[] = { "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=",
                                           "=>", NULL };

        L->tok = T_PUNCT;
        L->text[0] = *p;
        L->text[1] = 0;
        for (int i = 0; two[i]; i++) {
            if (p[0] == two[i][0] && p[1] == two[i][1]) {
                L->text[1] = p[1];
                L->text[2] = 0;
                p++;
                break;
            }
        }
        p++;
    }
    L->p = p;
}

static bool is(struct lexer *L, const char *punct)
{
    return (L->tok == T_PUNCT || L->tok == T_NAME) && !strcmp(L->text, punct);
}

static bool take(struct lexer *L, const char *punct)
{
    if (!is(L, punct))
        return false;
    next(L);
    return true;
}

static void expect(struct lexer *L, const char *punct)
{
    if (!take(L, punct)) {
        error_at(L->s, L->tok_line, "expected \"%s\" but found \"%s\"", punct,
                 L->tok == T_EOF ? "the end" : L->tok == T_STR ? "a string" : L->tok == T_NUM ? "a number" : L->text);
    }
}

// ---- Parser ----

static struct node *mk(struct lexer *L, enum kind k)
{
    struct node *n = calloc(1, sizeof(*n));

    if (!n) {
        error_at(L->s, L->tok_line, "out of memory");
        return NULL;
    }
    n->kind = k;
    n->line = L->tok_line;
    return n;
}

static void push_node(struct node ***list, int *n, struct node *x)
{
    struct node **l = realloc(*list, (*n + 1) * sizeof(*l));

    if (!l)
        return;
    *list = l;
    l[(*n)++] = x;
}

static bool failed(struct lexer *L)
{
    return L->s->state == RUN_ERROR;
}

static struct node *expression(struct lexer *L);
static struct node *statement(struct lexer *L);
static struct node *block(struct lexer *L);

static const char *const keywords[] = { "let", "fn", "if", "else", "while", "for", "in", "return", "break",
                                        "continue", "true", "false", "nil", NULL };

static bool keyword(const char *s)
{
    for (int i = 0; keywords[i]; i++)
        if (!strcmp(keywords[i], s))
            return true;
    return false;
}

static struct node *function_rest(struct lexer *L, struct node *f)
{
    expect(L, "(");
    while (!failed(L) && !is(L, ")")) {
        struct node *p;

        if (L->tok != T_NAME || keyword(L->text)) {
            error_at(L->s, L->tok_line, "expected a parameter name");
            return f;
        }
        p = mk(L, N_NAME);
        if (!p)
            return f;
        p->str = (char *)intern(L->s, L->text);
        push_node(&f->list, &f->nlist, p);
        next(L);
        if (!take(L, ","))
            break;
    }
    expect(L, ")");
    f->a = block(L);
    return f;
}

static struct node *primary(struct lexer *L)
{
    struct node *n = NULL;

    if (failed(L))
        return NULL;
    if (L->tok == T_NUM) {
        n = mk(L, N_NUM);
        if (n)
            n->num = L->num;
        next(L);
    } else if (L->tok == T_STR) {
        n = mk(L, N_STR);
        if (n) {
            n->str = L->str;
            L->str = NULL;
        }
        next(L);
    } else if (take(L, "(")) {
        n = expression(L);
        expect(L, ")");
    } else if (take(L, "[")) {
        n = mk(L, N_LIST);
        while (n && !failed(L) && !is(L, "]")) {
            push_node(&n->list, &n->nlist, expression(L));
            if (!take(L, ","))
                break;
        }
        expect(L, "]");
    } else if (take(L, "{")) {
        int nv = 0;

        n = mk(L, N_MAP);
        while (n && !failed(L) && !is(L, "}")) {
            struct node *k = mk(L, N_STR);

            if (!k)
                break;
            if (L->tok == T_STR) {
                k->str = L->str;
                L->str = NULL;
            } else if (L->tok == T_NAME || L->tok == T_NUM) {
                char buf[64];

                if (L->tok == T_NUM)
                    format_num(L->num, buf, sizeof(buf));
                k->str = strdup(L->tok == T_NUM ? buf : L->text);
            } else {
                error_at(L->s, L->tok_line, "expected a key");
                break;
            }
            next(L);
            expect(L, ":");
            push_node(&n->list, &n->nlist, k);
            push_node(&n->list2, &nv, expression(L));
            if (!take(L, ","))
                break;
        }
        expect(L, "}");
    } else if (L->tok == T_NAME) {
        if (take(L, "true")) {
            n = mk(L, N_TRUE);
        } else if (take(L, "false")) {
            n = mk(L, N_FALSE);
        } else if (take(L, "nil")) {
            n = mk(L, N_NIL);
        } else if (take(L, "fn")) {
            n = mk(L, N_FN);
            if (n)
                function_rest(L, n);
        } else if (keyword(L->text)) {
            error_at(L->s, L->tok_line, "\"%s\" cannot be used here", L->text);
        } else {
            n = mk(L, N_NAME);
            if (n)
                n->str = (char *)intern(L->s, L->text);
            next(L);
        }
    } else {
        error_at(L->s, L->tok_line, "unexpected \"%s\"", L->tok == T_EOF ? "end of the program" : L->text);
    }
    return n;
}

static struct node *postfix(struct lexer *L)
{
    struct node *n = primary(L);

    while (n && !failed(L)) {
        if (take(L, "(")) {
            struct node *c = mk(L, N_CALL);

            if (!c)
                break;
            c->a = n;
            while (!failed(L) && !is(L, ")")) {
                push_node(&c->list, &c->nlist, expression(L));
                if (!take(L, ","))
                    break;
            }
            expect(L, ")");
            n = c;
        } else if (take(L, "[")) {
            struct node *x = mk(L, N_INDEX);

            if (!x)
                break;
            x->a = n;
            x->b = expression(L);
            expect(L, "]");
            n = x;
        } else if (take(L, ".")) {
            struct node *m = mk(L, N_MEMBER);

            if (!m)
                break;
            if (L->tok != T_NAME) {
                error_at(L->s, L->tok_line, "expected a name after \".\"");
                break;
            }
            m->a = n;
            m->str = (char *)intern(L->s, L->text);
            next(L);
            n = m;
        } else {
            break;
        }
    }
    return n;
}

static struct node *unary(struct lexer *L)
{
    if (is(L, "-") || is(L, "!")) {
        struct node *n = mk(L, N_UNARY);

        if (!n)
            return NULL;
        n->op = L->text[0];
        next(L);
        n->a = unary(L);
        return n;
    }
    return postfix(L);
}

static int precedence(struct lexer *L, int *op)
{
    static const struct {
        const char *t;
        int prec, op;
    } ops[] = {
        { "||", 1, 'o' }, { "&&", 2, 'a' }, { "==", 3, 'e' }, { "!=", 3, 'n' }, { "<", 4, '<' },
        { "<=", 4, 'l' }, { ">", 4, '>' }, { ">=", 4, 'g' }, { "+", 5, '+' }, { "-", 5, '-' },
        { "*", 6, '*' }, { "/", 6, '/' }, { "%", 6, '%' },
    };

    if (L->tok != T_PUNCT)
        return 0;
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (!strcmp(L->text, ops[i].t)) {
            *op = ops[i].op;
            return ops[i].prec;
        }
    return 0;
}

static struct node *binary(struct lexer *L, int min)
{
    struct node *left = unary(L);
    int op, prec;

    while (left && !failed(L) && (prec = precedence(L, &op)) >= min && prec) {
        struct node *n = mk(L, op == 'o' ? N_OR : op == 'a' ? N_AND : N_BINARY);

        if (!n)
            break;
        next(L);
        n->op = op;
        n->a = left;
        n->b = binary(L, prec + 1);
        left = n;
    }
    return left;
}

static struct node *expression(struct lexer *L)
{
    struct node *left = binary(L, 1);

    if (left && (is(L, "=") || is(L, "+=") || is(L, "-=") || is(L, "*=") || is(L, "/=") || is(L, "%="))) {
        struct node *n = mk(L, N_ASSIGN);

        if (!n)
            return left;
        if (left->kind != N_NAME && left->kind != N_INDEX && left->kind != N_MEMBER) {
            error_at(L->s, L->tok_line, "this cannot be assigned to");
            return left;
        }
        n->op = L->text[0] == '=' ? '=' : L->text[0];
        next(L);
        n->a = left;
        n->b = expression(L);
        return n;
    }
    return left;
}

static struct node *block(struct lexer *L)
{
    struct node *b = mk(L, N_BLOCK);

    expect(L, "{");
    while (b && !failed(L) && !is(L, "}") && L->tok != T_EOF)
        push_node(&b->list, &b->nlist, statement(L));
    expect(L, "}");
    return b;
}

static struct node *statement(struct lexer *L)
{
    struct node *n = NULL;

    if (failed(L))
        return NULL;
    if (is(L, "{"))
        return block(L);
    if (take(L, "let")) {
        n = mk(L, N_LET);
        if (!n)
            return NULL;
        if (L->tok != T_NAME || keyword(L->text)) {
            error_at(L->s, L->tok_line, "expected a name after \"let\"");
            return n;
        }
        n->str = (char *)intern(L->s, L->text);
        next(L);
        if (take(L, "="))
            n->a = expression(L);
        take(L, ";");
        return n;
    }
    if (is(L, "fn") && L->p && (isalpha((unsigned char)*L->p) || *L->p == ' ')) {
        // fn name(...) { } at statement level defines a function.
        const char *save = L->p;
        int line = L->line;

        next(L);
        if (L->tok == T_NAME && !keyword(L->text)) {
            n = mk(L, N_FNDEF);
            if (!n)
                return NULL;
            n->str = (char *)intern(L->s, L->text);
            next(L);
            return function_rest(L, n);
        }
        // An anonymous function used as a statement expression.
        L->p = save;
        L->line = line;
        strcpy(L->text, "fn");
        L->tok = T_NAME;
    }
    if (take(L, "if")) {
        n = mk(L, N_IF);
        if (!n)
            return NULL;
        expect(L, "(");
        n->a = expression(L);
        expect(L, ")");
        n->b = statement(L);
        if (take(L, "else"))
            n->c = statement(L);
        return n;
    }
    if (take(L, "while")) {
        n = mk(L, N_WHILE);
        if (!n)
            return NULL;
        expect(L, "(");
        n->a = expression(L);
        expect(L, ")");
        n->b = statement(L);
        return n;
    }
    if (take(L, "for")) {
        n = mk(L, N_FOR);
        if (!n)
            return NULL;
        expect(L, "(");
        take(L, "let");
        if (L->tok != T_NAME || keyword(L->text)) {
            error_at(L->s, L->tok_line, "expected a name after \"for (\"");
            return n;
        }
        n->str = (char *)intern(L->s, L->text);
        next(L);
        expect(L, "in");
        n->a = expression(L);
        expect(L, ")");
        n->b = statement(L);
        return n;
    }
    if (take(L, "return")) {
        n = mk(L, N_RETURN);
        if (n && !is(L, ";") && !is(L, "}"))
            n->a = expression(L);
        take(L, ";");
        return n;
    }
    if (take(L, "break")) {
        take(L, ";");
        return mk(L, N_BREAK);
    }
    if (take(L, "continue")) {
        take(L, ";");
        return mk(L, N_CONTINUE);
    }
    n = mk(L, N_EXPR);
    if (n)
        n->a = expression(L);
    take(L, ";");
    return n;
}

// ---- Evaluation ----

static struct script_value eval(struct script *s, struct node *n, struct env *e);
static void exec(struct script *s, struct node *n, struct env *e);

static bool equal(struct script_value a, struct script_value b)
{
    if (a.type != b.type)
        return false;
    switch (a.type) {
    case S_NIL: return true;
    case S_BOOL: return a.b == b.b;
    case S_NUM: return a.n == b.n;
    case S_STR: return a.s->len == b.s->len && !memcmp(a.s->data, b.s->data, a.s->len);
    case S_LIST: return a.l == b.l;
    case S_MAP: return a.m == b.m;
    case S_FN: return a.f == b.f;
    case S_NATIVE: return a.nf == b.nf;
    case S_WIDGET: return a.w == b.w;
    }
    return false;
}

static const char *type_name(struct script_value v)
{
    static const char *const names[] = { "nil", "bool", "number", "string", "list", "map", "function",
                                         "function", "widget" };

    return names[v.type];
}

static struct script_value binop(struct script *s, struct node *n, struct script_value a, struct script_value b)
{
    struct script_value r = nil_value;

    switch (n->op) {
    case 'e': return script_bool(equal(a, b));
    case 'n': return script_bool(!equal(a, b));
    case '+':
        if (a.type == S_NUM && b.type == S_NUM)
            return script_num(a.n + b.n);
        if (a.type == S_LIST && b.type == S_LIST) {
            r = script_list();
            for (int i = 0; i < a.l->n; i++)
                script_list_push(r, a.l->items[i]);
            for (int i = 0; i < b.l->n; i++)
                script_list_push(r, b.l->items[i]);
            return r;
        }
        if (a.type == S_STR || b.type == S_STR) {
            char *x = script_to_string(a), *y = script_to_string(b);
            size_t lx = strlen(x), ly = strlen(y);
            char *both = malloc(lx + ly + 1);

            if (both) {
                memcpy(both, x, lx);
                memcpy(both + lx, y, ly + 1);
                r = script_str_len(both, lx + ly);
            }
            free(x);
            free(y);
            free(both);
            return r;
        }
        break;
    case '<': case 'l': case '>': case 'g':
        if (a.type == S_NUM && b.type == S_NUM) {
            bool v = n->op == '<' ? a.n < b.n : n->op == 'l' ? a.n <= b.n : n->op == '>' ? a.n > b.n : a.n >= b.n;

            return script_bool(v);
        }
        if (a.type == S_STR && b.type == S_STR) {
            int c = strcmp(a.s->data, b.s->data);

            return script_bool(n->op == '<' ? c < 0 : n->op == 'l' ? c <= 0 : n->op == '>' ? c > 0 : c >= 0);
        }
        break;
    case '-': case '*': case '/': case '%':
        if (a.type == S_NUM && b.type == S_NUM) {
            switch (n->op) {
            case '-': return script_num(a.n - b.n);
            case '*': return script_num(a.n * b.n);
            case '/': return script_num(a.n / b.n);
            case '%': return script_num(fmod(a.n, b.n));
            }
        }
        if (n->op == '*' && a.type == S_STR && b.type == S_NUM) {
            struct sb buf = { 0 };

            for (int i = 0; i < (int)b.n && i < 100000; i++)
                sb_add(&buf, a.s->data, a.s->len);
            r = script_str_len(buf.s ? buf.s : "", buf.len);
            free(buf.s);
            return r;
        }
        break;
    }
    error_at(s, n->line, "cannot apply this operator to a %s and a %s", type_name(a), type_name(b));
    return nil_value;
}

// Widget members are provided by the UI library.
struct script_value (*script_widget_get)(struct script *s, struct widget *w, const char *name);
bool (*script_widget_set)(struct script *s, struct widget *w, const char *name, struct script_value v);
struct script_value (*script_widget_call)(struct script *s, struct widget *w, const char *name,
                                          struct script_value *args, int nargs);

static struct script_value member_get(struct script *s, struct node *n, struct script_value obj, const char *name)
{
    if (obj.type == S_MAP) {
        struct script_value v = map_get(obj.m, name);

        script_retain(v);
        return v;
    }
    if (!strcmp(name, "length")) {
        if (obj.type == S_STR)
            {
                int count = 0;

                for (const char *p = obj.s->data; *p; p += utf8_next(p, 0))
                    count++;
                return script_num(count);
            }
        if (obj.type == S_LIST)
            return script_num(obj.l->n);
    }
    if (obj.type == S_WIDGET && script_widget_get)
        return script_widget_get(s, obj.w, name);
    error_at(s, n->line, "a %s has no \"%s\"", type_name(obj), name);
    return nil_value;
}

static struct script_value call_value(struct script *s, int line, struct script_value fnv, struct script_value *args,
                                      int nargs)
{
    struct script_value r = nil_value;

    if (fnv.type == S_NATIVE) {
        native_line = line;
        r = fnv.nf->fn(s, args, nargs);
        return r;
    }
    if (fnv.type != S_FN) {
        error_at(s, line, "a %s cannot be called", type_name(fnv));
        return nil_value;
    }
    if (++s->depth > 200) {
        error_at(s, line, "too many nested calls");
        s->depth--;
        return nil_value;
    }
    {
        struct node *def = fnv.f->def;
        struct env *local = env_new(fnv.f->closure);

        if (!local) {
            s->depth--;
            return nil_value;
        }
        for (int i = 0; i < def->nlist; i++)
            env_define(local, def->list[i]->str, i < nargs ? args[i] : nil_value);
        exec(s, def->a, local);
        if (s->state == RUN_RETURN) {
            r = s->ret;
            s->ret = nil_value;
            s->state = RUN_NORMAL;
        } else if (s->state == RUN_BREAK || s->state == RUN_CONTINUE) {
            s->state = RUN_NORMAL;
        }
        env_release(local);
    }
    s->depth--;
    return r;
}

static struct script_value call(struct script *s, struct node *n, struct env *e)
{
    struct script_value args[16], r = nil_value;
    int nargs = MIN(n->nlist, 16);

    if (n->nlist > 16) {
        error_at(s, n->line, "too many arguments");
        return nil_value;
    }
    // obj.method(...) on widgets and maps of functions.
    if (n->a->kind == N_MEMBER) {
        struct script_value obj = eval(s, n->a->a, e);

        if (s->state == RUN_ERROR) {
            script_release(obj);
            return nil_value;
        }
        for (int i = 0; i < nargs; i++)
            args[i] = eval(s, n->list[i], e);
        if (s->state != RUN_ERROR) {
            if (obj.type == S_WIDGET && script_widget_call) {
                native_line = n->line;
                r = script_widget_call(s, obj.w, n->a->str, args, nargs);
            } else {
                struct script_value fnv = member_get(s, n->a, obj, n->a->str);

                if (s->state != RUN_ERROR)
                    r = call_value(s, n->line, fnv, args, nargs);
                script_release(fnv);
            }
        }
        for (int i = 0; i < nargs; i++)
            script_release(args[i]);
        script_release(obj);
        return r;
    }
    {
        struct script_value fnv = eval(s, n->a, e);

        for (int i = 0; i < nargs && s->state != RUN_ERROR; i++)
            args[i] = eval(s, n->list[i], e);
        if (s->state == RUN_ERROR) {
            script_release(fnv);
            return nil_value;
        }
        r = call_value(s, n->line, fnv, args, nargs);
        for (int i = 0; i < nargs; i++)
            script_release(args[i]);
        script_release(fnv);
    }
    return r;
}

static void assign(struct script *s, struct node *target, struct script_value v, struct env *e)
{
    if (target->kind == N_NAME) {
        struct script_value *slot = env_lookup(e, target->str);

        if (!slot) {
            error_at(s, target->line, "\"%s\" is not defined (use let first)", target->str);
            return;
        }
        script_retain(v);
        script_release(*slot);
        *slot = v;
    } else if (target->kind == N_INDEX) {
        struct script_value obj = eval(s, target->a, e), idx = eval(s, target->b, e);

        if (s->state != RUN_ERROR) {
            if (obj.type == S_LIST && idx.type == S_NUM) {
                int i = (int)idx.n;

                if (i < 0)
                    i += obj.l->n;
                if (i >= 0 && i < obj.l->n) {
                    script_retain(v);
                    script_release(obj.l->items[i]);
                    obj.l->items[i] = v;
                } else if (i == obj.l->n) {
                    script_list_push(obj, v);
                } else {
                    error_at(s, target->line, "index %d is outside the list (length %d)", i, obj.l->n);
                }
            } else if (obj.type == S_MAP) {
                char *key = script_to_string(idx);

                map_set(obj.m, key, v);
                free(key);
            } else {
                error_at(s, target->line, "a %s cannot be indexed", type_name(obj));
            }
        }
        script_release(obj);
        script_release(idx);
    } else if (target->kind == N_MEMBER) {
        struct script_value obj = eval(s, target->a, e);

        if (s->state != RUN_ERROR) {
            if (obj.type == S_MAP)
                map_set(obj.m, target->str, v);
            else if (obj.type == S_WIDGET && script_widget_set) {
                native_line = target->line;
                if (!script_widget_set(s, obj.w, target->str, v) && s->state != RUN_ERROR)
                    error_at(s, target->line, "widgets have no \"%s\" to set", target->str);
            } else
                error_at(s, target->line, "cannot set \"%s\" on a %s", target->str, type_name(obj));
        }
        script_release(obj);
    }
}

static struct script_value eval(struct script *s, struct node *n, struct env *e)
{
    struct script_value a, b, r = nil_value;

    if (!n || s->state == RUN_ERROR)
        return nil_value;
    switch (n->kind) {
    case N_NUM: return script_num(n->num);
    case N_STR: return script_str(n->str);
    case N_NIL: return nil_value;
    case N_TRUE: return script_bool(true);
    case N_FALSE: return script_bool(false);
    case N_NAME: {
        struct script_value *slot = env_lookup(e, n->str);

        if (!slot) {
            error_at(s, n->line, "\"%s\" is not defined", n->str);
            return nil_value;
        }
        script_retain(*slot);
        return *slot;
    }
    case N_LIST:
        r = script_list();
        for (int i = 0; i < n->nlist && s->state != RUN_ERROR; i++) {
            a = eval(s, n->list[i], e);
            script_list_push(r, a);
            script_release(a);
        }
        return r;
    case N_MAP:
        r = new_map();
        for (int i = 0; i < n->nlist && s->state != RUN_ERROR; i++) {
            a = eval(s, n->list2[i], e);
            map_set(r.m, n->list[i]->str, a);
            script_release(a);
        }
        return r;
    case N_UNARY:
        a = eval(s, n->a, e);
        if (n->op == '!')
            r = script_bool(!script_truthy(a));
        else if (a.type == S_NUM)
            r = script_num(-a.n);
        else if (s->state != RUN_ERROR)
            error_at(s, n->line, "cannot negate a %s", type_name(a));
        script_release(a);
        return r;
    case N_AND:
        a = eval(s, n->a, e);
        if (!script_truthy(a))
            return a;
        script_release(a);
        return eval(s, n->b, e);
    case N_OR:
        a = eval(s, n->a, e);
        if (script_truthy(a))
            return a;
        script_release(a);
        return eval(s, n->b, e);
    case N_BINARY:
        a = eval(s, n->a, e);
        b = eval(s, n->b, e);
        if (s->state != RUN_ERROR)
            r = binop(s, n, a, b);
        script_release(a);
        script_release(b);
        return r;
    case N_CALL:
        return call(s, n, e);
    case N_INDEX:
        a = eval(s, n->a, e);
        b = eval(s, n->b, e);
        if (s->state != RUN_ERROR) {
            if (a.type == S_LIST && b.type == S_NUM) {
                int i = (int)b.n;

                if (i < 0)
                    i += a.l->n;
                if (i >= 0 && i < a.l->n) {
                    r = a.l->items[i];
                    script_retain(r);
                }
            } else if (a.type == S_STR && b.type == S_NUM) {
                // Characters (UTF-8 aware).
                int i = (int)b.n, k = 0;
                const char *p = a.s->data;

                while (*p && k < i) {
                    p += utf8_next(p, 0);
                    k++;
                }
                if (*p && i >= 0)
                    r = script_str_len(p, utf8_next(p, 0));
            } else if (a.type == S_MAP) {
                char *key = script_to_string(b);

                r = map_get(a.m, key);
                script_retain(r);
                free(key);
            } else {
                error_at(s, n->line, "a %s cannot be indexed", type_name(a));
            }
        }
        script_release(a);
        script_release(b);
        return r;
    case N_MEMBER:
        a = eval(s, n->a, e);
        if (s->state != RUN_ERROR)
            r = member_get(s, n, a, n->str);
        script_release(a);
        return r;
    case N_ASSIGN:
        if (n->op == '=') {
            r = eval(s, n->b, e);
        } else {
            struct node fake = *n;

            a = eval(s, n->a, e);
            b = eval(s, n->b, e);
            fake.op = n->op;
            if (s->state != RUN_ERROR)
                r = binop(s, &fake, a, b);
            script_release(a);
            script_release(b);
        }
        if (s->state != RUN_ERROR)
            assign(s, n->a, r, e);
        return r;
    case N_FN: {
        struct script_value f = { .type = S_FN };

        f.f = calloc(1, sizeof(*f.f));
        if (!f.f)
            return nil_value;
        f.f->refs = 1;
        f.f->def = n;
        f.f->closure = e;
        e->refs++;
        return f;
    }
    default:
        error_at(s, n->line, "this is a statement, not a value");
        return nil_value;
    }
}

static void exec(struct script *s, struct node *n, struct env *e)
{
    struct script_value v;

    if (!n || s->state != RUN_NORMAL)
        return;
    switch (n->kind) {
    case N_BLOCK: {
        struct env *inner = env_new(e);

        if (!inner)
            return;
        for (int i = 0; i < n->nlist && s->state == RUN_NORMAL; i++)
            exec(s, n->list[i], inner);
        env_release(inner);
        break;
    }
    case N_LET:
        v = n->a ? eval(s, n->a, e) : nil_value;
        if (s->state != RUN_ERROR)
            env_define(e, n->str, v);
        script_release(v);
        break;
    case N_FNDEF: {
        struct node fake = *n;

        fake.kind = N_FN;
        // The node outlives this call: point the function at the real one.
        v = eval(s, &fake, e);
        if (v.type == S_FN)
            v.f->def = n;
        env_define(e, n->str, v);
        script_release(v);
        break;
    }
    case N_IF:
        v = eval(s, n->a, e);
        if (s->state != RUN_ERROR)
            exec(s, script_truthy(v) ? n->b : n->c, e);
        script_release(v);
        break;
    case N_WHILE:
        for (int guard = 0; s->state == RUN_NORMAL; guard++) {
            v = eval(s, n->a, e);
            if (!script_truthy(v)) {
                script_release(v);
                break;
            }
            script_release(v);
            exec(s, n->b, e);
            if (s->state == RUN_BREAK) {
                s->state = RUN_NORMAL;
                break;
            }
            if (s->state == RUN_CONTINUE)
                s->state = RUN_NORMAL;
        }
        break;
    case N_FOR: {
        struct script_value coll = eval(s, n->a, e);
        int count;

        if (s->state == RUN_ERROR)
            break;
        if (coll.type == S_NUM) {
            count = (int)coll.n;
        } else if (coll.type == S_LIST) {
            count = coll.l->n;
        } else if (coll.type == S_MAP) {
            count = coll.m->n;
        } else if (coll.type == S_STR) {
            count = (int)coll.s->len;
        } else {
            error_at(s, n->line, "cannot loop over a %s", type_name(coll));
            script_release(coll);
            break;
        }
        for (int i = 0, pos = 0; i < count && s->state == RUN_NORMAL; i++) {
            struct env *inner = env_new(e);
            struct script_value item = nil_value;

            if (!inner)
                break;
            if (coll.type == S_NUM) {
                item = script_num(i);
            } else if (coll.type == S_LIST) {
                if (i >= coll.l->n)
                    break;
                item = coll.l->items[i];
                script_retain(item);
            } else if (coll.type == S_MAP) {
                if (i >= coll.m->n)
                    break;
                item = script_str(coll.m->keys[i]->data);
            } else {
                int len;

                if (pos >= (int)coll.s->len)
                    break;
                len = utf8_next(coll.s->data + pos, 0);
                item = script_str_len(coll.s->data + pos, len);
                pos += len;
                count = (int)coll.s->len + 1;
            }
            env_define(inner, n->str, item);
            script_release(item);
            exec(s, n->b, inner);
            env_release(inner);
            if (s->state == RUN_BREAK) {
                s->state = RUN_NORMAL;
                break;
            }
            if (s->state == RUN_CONTINUE)
                s->state = RUN_NORMAL;
        }
        script_release(coll);
        break;
    }
    case N_RETURN:
        v = n->a ? eval(s, n->a, e) : nil_value;
        if (s->state == RUN_ERROR) {
            script_release(v);
            break;
        }
        script_release(s->ret);
        s->ret = v;
        s->state = RUN_RETURN;
        break;
    case N_BREAK:
        s->state = RUN_BREAK;
        break;
    case N_CONTINUE:
        s->state = RUN_CONTINUE;
        break;
    case N_EXPR:
        script_release(eval(s, n->a, e));
        break;
    default:
        script_release(eval(s, n, e));
        break;
    }
}

// ---- Public interface ----

void script_define(struct script *s, const char *name, script_native fn)
{
    struct script_value v = { .type = S_NATIVE };

    v.nf = calloc(1, sizeof(*v.nf));
    if (!v.nf)
        return;
    v.nf->refs = 1;
    strlcpy(v.nf->name, name, sizeof(v.nf->name));
    v.nf->fn = fn;
    env_define(s->globals, intern(s, name), v);
    script_release(v);
}

void script_set_global(struct script *s, const char *name, struct script_value v)
{
    env_define(s->globals, intern(s, name), v);
}

void script_builtins(struct script *s);

struct script *script_new(void)
{
    struct script *s = calloc(1, sizeof(*s));

    if (!s || !(s->globals = env_new(NULL))) {
        free(s);
        return NULL;
    }
    script_builtins(s);
    return s;
}

static void free_tree(struct node *n)
{
    if (!n)
        return;
    free_tree(n->a);
    free_tree(n->b);
    free_tree(n->c);
    for (int i = 0; i < n->nlist; i++)
        free_tree(n->list[i]);
    if (n->kind == N_MAP)
        for (int i = 0; i < n->nlist; i++)
            free_tree(n->list2[i]);
    free(n->list);
    free(n->list2);
    if (n->kind == N_STR)
        free(n->str);
    free(n);
}

void script_free(struct script *s)
{
    if (!s)
        return;
    script_release(s->ret);
    env_release(s->globals);
    for (int i = 0; i < s->ntrees; i++)
        free_tree(s->trees[i]);
    free(s->trees);
    for (int i = 0; i < s->nnames; i++)
        free(s->names[i]);
    free(s->names);
    free(s);
}

bool script_run(struct script *s, const char *source, const char *name)
{
    struct lexer L = { s, source, 1, T_EOF, { 0 }, NULL, 0, 1 };
    struct node *prog;

    s->state = RUN_NORMAL;
    s->error[0] = 0;
    s->source_name = name;
    next(&L);
    if (!(prog = calloc(1, sizeof(*prog))))
        return false;
    prog->kind = N_BLOCK;
    while (L.tok != T_EOF && s->state == RUN_NORMAL)
        push_node(&prog->list, &prog->nlist, statement(&L));
    free(L.str);
    {
        struct node **t = realloc(s->trees, (s->ntrees + 1) * sizeof(*t));

        if (t) {
            s->trees = t;
            s->trees[s->ntrees++] = prog;
        }
    }
    if (s->state == RUN_ERROR)
        return false;
    // The top level runs in the global scope (not a block of its own).
    for (int i = 0; i < prog->nlist && s->state == RUN_NORMAL; i++)
        exec(s, prog->list[i], s->globals);
    if (s->state == RUN_RETURN || s->state == RUN_BREAK || s->state == RUN_CONTINUE)
        s->state = RUN_NORMAL;
    return s->state != RUN_ERROR;
}

bool script_has_function(struct script *s, const char *function)
{
    struct script_value *v = env_lookup(s->globals, function);

    return v && (v->type == S_FN || v->type == S_NATIVE);
}

bool script_call(struct script *s, const char *function, struct script_value *args, int nargs,
                 struct script_value *result)
{
    struct script_value *fn = env_lookup(s->globals, function), r;

    s->state = RUN_NORMAL;
    s->error[0] = 0;
    if (!fn) {
        snprintf(s->error, sizeof(s->error), "there is no function \"%s\"", function);
        return false;
    }
    {
        struct script_value f = *fn;

        script_retain(f);
        r = call_value(s, 0, f, args, nargs);
        script_release(f);
    }
    if (result)
        *result = r;
    else
        script_release(r);
    if (s->state == RUN_ERROR)
        return false;
    s->state = RUN_NORMAL;
    return true;
}
