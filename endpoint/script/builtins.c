#include "script_internal.h"
#include "gfx.h"
#include <math.h>

// The standard functions of AegisScript.

#define ARG(i) (i < n ? a[i] : script_nil())
#define NEED(count, name) \
    if (n < count) \
        return script_fail(s, "%s needs %d argument%s", name, count, count == 1 ? "" : "s")

static double num_arg(struct script *s, struct script_value *a, int n, int i, const char *fn)
{
    if (i >= n || a[i].type != S_NUM) {
        script_fail(s, "%s: argument %d must be a number", fn, i + 1);
        return 0;
    }
    return a[i].n;
}

static const char *str_arg(struct script *s, struct script_value *a, int n, int i, const char *fn)
{
    if (i >= n || a[i].type != S_STR) {
        script_fail(s, "%s: argument %d must be a string", fn, i + 1);
        return "";
    }
    return script_text(a[i]);
}

static struct script_value b_print(struct script *s, struct script_value *a, int n)
{
    (void)s;
    for (int i = 0; i < n; i++) {
        char *t = script_to_string(a[i]);

        dprintf(STDOUT_FILENO, "%s%s", i ? " " : "", t);
        free(t);
    }
    dprintf(STDOUT_FILENO, "\n");
    return script_nil();
}

static struct script_value b_len(struct script *s, struct script_value *a, int n)
{
    NEED(1, "len");
    if (a[0].type == S_STR) {
        int count = 0;

        for (const char *p = script_text(a[0]); *p; p += utf8_next(p, 0))
            count++;
        return script_num(count);
    }
    if (a[0].type == S_LIST)
        return script_num(a[0].l->n);
    if (a[0].type == S_MAP)
        return script_num(a[0].m->n);
    return script_num(0);
}

static struct script_value b_str(struct script *s, struct script_value *a, int n)
{
    char *t;
    struct script_value v;

    (void)s;
    t = script_to_string(ARG(0));
    v = script_str(t);
    free(t);
    return v;
}

static struct script_value b_num(struct script *s, struct script_value *a, int n)
{
    char *end;
    double v;

    (void)s;
    if (ARG(0).type == S_NUM)
        return a[0];
    if (ARG(0).type == S_BOOL)
        return script_num(a[0].b);
    if (ARG(0).type != S_STR)
        return script_nil();
    v = strtod(script_text(a[0]), &end);
    while (*end == ' ')
        end++;
    return end == script_text(a[0]) || *end ? script_nil() : script_num(v);
}

static struct script_value b_int(struct script *s, struct script_value *a, int n)
{
    struct script_value v = b_num(s, a, n);

    return v.type == S_NUM ? script_num(trunc(v.n)) : v;
}

static struct script_value b_type(struct script *s, struct script_value *a, int n)
{
    static const char *const names[] = { "nil", "bool", "number", "string", "list", "map", "function",
                                         "function", "widget" };

    (void)s;
    return script_str(names[ARG(0).type]);
}

// ---- Lists and maps ----

static struct script_value b_push(struct script *s, struct script_value *a, int n)
{
    NEED(2, "push");
    if (a[0].type != S_LIST)
        return script_fail(s, "push: the first argument must be a list");
    for (int i = 1; i < n; i++)
        script_list_push(a[0], a[i]);
    return script_num(a[0].l->n);
}

static struct script_value b_pop(struct script *s, struct script_value *a, int n)
{
    NEED(1, "pop");
    if (a[0].type != S_LIST)
        return script_fail(s, "pop: the argument must be a list");
    if (!a[0].l->n)
        return script_nil();
    return a[0].l->items[--a[0].l->n];        // the list's reference moves to the caller
}

static struct script_value b_insert(struct script *s, struct script_value *a, int n)
{
    int at;

    NEED(3, "insert");
    if (a[0].type != S_LIST)
        return script_fail(s, "insert: the first argument must be a list");
    at = (int)num_arg(s, a, n, 1, "insert");
    script_list_push(a[0], a[2]);
    if (at < 0)
        at = 0;
    if (at < a[0].l->n - 1) {
        struct script_value v = a[0].l->items[a[0].l->n - 1];

        memmove(&a[0].l->items[at + 1], &a[0].l->items[at], (a[0].l->n - 1 - at) * sizeof(v));
        a[0].l->items[at] = v;
    }
    return script_nil();
}

static struct script_value b_remove(struct script *s, struct script_value *a, int n)
{
    int at;
    struct script_value v;

    NEED(2, "remove");
    if (a[0].type != S_LIST)
        return script_fail(s, "remove: the first argument must be a list");
    at = (int)num_arg(s, a, n, 1, "remove");
    if (at < 0)
        at += a[0].l->n;
    if (at < 0 || at >= a[0].l->n)
        return script_nil();
    v = a[0].l->items[at];
    memmove(&a[0].l->items[at], &a[0].l->items[at + 1], (a[0].l->n - at - 1) * sizeof(v));
    a[0].l->n--;
    return v;
}

static int compare_values(const void *x, const void *y)
{
    const struct script_value *a = x, *b = y;

    if (a->type == S_NUM && b->type == S_NUM)
        return a->n < b->n ? -1 : a->n > b->n;
    if (a->type == S_STR && b->type == S_STR)
        return strcasecmp(script_text(*a), script_text(*b));
    return (int)a->type - (int)b->type;
}

static struct script_value b_sort(struct script *s, struct script_value *a, int n)
{
    NEED(1, "sort");
    if (a[0].type != S_LIST)
        return script_fail(s, "sort: the argument must be a list");
    qsort(a[0].l->items, a[0].l->n, sizeof(struct script_value), compare_values);
    script_retain(a[0]);
    return a[0];
}

static struct script_value b_reverse(struct script *s, struct script_value *a, int n)
{
    NEED(1, "reverse");
    if (a[0].type != S_LIST)
        return script_fail(s, "reverse: the argument must be a list");
    for (int i = 0, j = a[0].l->n - 1; i < j; i++, j--) {
        struct script_value t = a[0].l->items[i];

        a[0].l->items[i] = a[0].l->items[j];
        a[0].l->items[j] = t;
    }
    script_retain(a[0]);
    return a[0];
}

static struct script_value b_range(struct script *s, struct script_value *a, int n)
{
    double from = 0, to, step = 1;
    struct script_value l;

    NEED(1, "range");
    if (n == 1) {
        to = num_arg(s, a, n, 0, "range");
    } else {
        from = num_arg(s, a, n, 0, "range");
        to = num_arg(s, a, n, 1, "range");
        if (n > 2)
            step = num_arg(s, a, n, 2, "range");
    }
    if (!step)
        return script_fail(s, "range: the step cannot be 0");
    l = script_list();
    for (double v = from; (step > 0 ? v < to : v > to); v += step) {
        if (l.l->n > 1000000)
            break;
        script_list_push(l, script_num(v));
    }
    return l;
}

static struct script_value b_keys(struct script *s, struct script_value *a, int n)
{
    struct script_value l;

    NEED(1, "keys");
    if (a[0].type != S_MAP)
        return script_fail(s, "keys: the argument must be a map");
    l = script_list();
    for (int i = 0; i < a[0].m->n; i++) {
        struct script_value k = script_str(a[0].m->keys[i]->data);

        script_list_push(l, k);
        script_release(k);
    }
    return l;
}

static struct script_value b_has(struct script *s, struct script_value *a, int n)
{
    NEED(2, "has");
    if (a[0].type == S_MAP) {
        char *k = script_to_string(a[1]);
        bool found = false;

        for (int i = 0; i < a[0].m->n && !found; i++)
            found = !strcmp(a[0].m->keys[i]->data, k);
        free(k);
        return script_bool(found);
    }
    if (a[0].type == S_LIST) {
        for (int i = 0; i < a[0].l->n; i++) {
            struct script_value x = a[0].l->items[i];

            if (x.type == a[1].type && ((x.type == S_NUM && x.n == a[1].n)
                                        || (x.type == S_STR && !strcmp(script_text(x), script_text(a[1])))))
                return script_bool(true);
        }
        return script_bool(false);
    }
    if (a[0].type == S_STR)
        return script_bool(strstr(script_text(a[0]), str_arg(s, a, n, 1, "has")) != NULL);
    return script_bool(false);
}

// ---- Strings ----

static struct script_value b_split(struct script *s, struct script_value *a, int n)
{
    const char *str = str_arg(s, a, n, 0, "split"), *sep = n > 1 ? str_arg(s, a, n, 1, "split") : " ";
    struct script_value l = script_list();
    size_t sl = strlen(sep);

    if (!sl) {
        for (const char *p = str; *p; p += utf8_next(p, 0)) {
            struct script_value c = script_str_len(p, utf8_next(p, 0));

            script_list_push(l, c);
            script_release(c);
        }
        return l;
    }
    for (const char *p = str;;) {
        const char *hit = strstr(p, sep);
        struct script_value part = script_str_len(p, hit ? (size_t)(hit - p) : strlen(p));

        script_list_push(l, part);
        script_release(part);
        if (!hit)
            break;
        p = hit + sl;
    }
    return l;
}

static struct script_value b_join(struct script *s, struct script_value *a, int n)
{
    const char *sep = n > 1 ? str_arg(s, a, n, 1, "join") : "";
    size_t cap = 64, len = 0, sl = strlen(sep);
    char *out = malloc(cap);
    struct script_value r;

    NEED(1, "join");
    if (a[0].type != S_LIST || !out) {
        free(out);
        return script_fail(s, "join: the first argument must be a list");
    }
    out[0] = 0;
    for (int i = 0; i < a[0].l->n; i++) {
        char *t = script_to_string(a[0].l->items[i]);
        size_t tl = strlen(t);

        if (len + tl + sl + 1 > cap) {
            char *g = realloc(out, cap = (len + tl + sl + 1) * 2);

            if (!g) {
                free(t);
                break;
            }
            out = g;
        }
        if (i) {
            memcpy(out + len, sep, sl);
            len += sl;
        }
        memcpy(out + len, t, tl);
        len += tl;
        out[len] = 0;
        free(t);
    }
    r = script_str_len(out, len);
    free(out);
    return r;
}

static struct script_value change_case(struct script *s, struct script_value *a, int n, bool up)
{
    const char *str = str_arg(s, a, n, 0, up ? "upper" : "lower");
    struct script_value r = script_str(str);

    if (r.type == S_STR)
        for (size_t i = 0; i < r.s->len; i++)
            r.s->data[i] = up ? toupper((unsigned char)r.s->data[i]) : tolower((unsigned char)r.s->data[i]);
    return r;
}

static struct script_value b_upper(struct script *s, struct script_value *a, int n)
{
    return change_case(s, a, n, true);
}

static struct script_value b_lower(struct script *s, struct script_value *a, int n)
{
    return change_case(s, a, n, false);
}

static struct script_value b_trim(struct script *s, struct script_value *a, int n)
{
    const char *str = str_arg(s, a, n, 0, "trim"), *end;

    while (isspace((unsigned char)*str))
        str++;
    end = str + strlen(str);
    while (end > str && isspace((unsigned char)end[-1]))
        end--;
    return script_str_len(str, end - str);
}

static struct script_value b_find(struct script *s, struct script_value *a, int n)
{
    const char *str = str_arg(s, a, n, 0, "find"), *what = str_arg(s, a, n, 1, "find");
    const char *hit = strstr(str, what);
    int chars = 0;

    if (!hit)
        return script_num(-1);
    for (const char *p = str; p < hit; p += utf8_next(p, 0))
        chars++;
    return script_num(chars);
}

static struct script_value b_replace(struct script *s, struct script_value *a, int n)
{
    const char *str = str_arg(s, a, n, 0, "replace"), *from = str_arg(s, a, n, 1, "replace");
    const char *to = str_arg(s, a, n, 2, "replace");
    size_t fl = strlen(from), tl = strlen(to), cap = strlen(str) + 1, len = 0;
    char *out = malloc(cap);
    struct script_value r;

    if (!out)
        return script_nil();
    if (!fl) {
        free(out);
        return script_str(str);
    }
    for (const char *p = str; *p;) {
        if (!strncmp(p, from, fl)) {
            if (len + tl + 1 > cap && !(out = realloc(out, cap = (len + tl + 1) * 2)))
                return script_nil();
            memcpy(out + len, to, tl);
            len += tl;
            p += fl;
        } else {
            if (len + 2 > cap && !(out = realloc(out, cap *= 2)))
                return script_nil();
            out[len++] = *p++;
        }
    }
    r = script_str_len(out, len);
    free(out);
    return r;
}

// substr(text, start[, count]) in characters.
static struct script_value b_substr(struct script *s, struct script_value *a, int n)
{
    const char *str = str_arg(s, a, n, 0, "substr"), *p = str, *q;
    int start = (int)num_arg(s, a, n, 1, "substr"), count = n > 2 ? (int)num_arg(s, a, n, 2, "substr") : 1 << 30;
    int total = 0;

    for (const char *x = str; *x; x += utf8_next(x, 0))
        total++;
    if (start < 0)
        start = MAX(0, total + start);
    for (int i = 0; i < start && *p; i++)
        p += utf8_next(p, 0);
    q = p;
    for (int i = 0; i < count && *q; i++)
        q += utf8_next(q, 0);
    return script_str_len(p, q - p);
}

static struct script_value b_starts(struct script *s, struct script_value *a, int n)
{
    const char *str = str_arg(s, a, n, 0, "starts"), *pre = str_arg(s, a, n, 1, "starts");

    return script_bool(!strncmp(str, pre, strlen(pre)));
}

static struct script_value b_format(struct script *s, struct script_value *a, int n)
{
    // format(number, decimals)
    char buf[64];
    double v = num_arg(s, a, n, 0, "format");
    int d = n > 1 ? (int)num_arg(s, a, n, 1, "format") : 2;

    snprintf(buf, sizeof(buf), "%.*f", MIN(MAX(d, 0), 20), v);
    return script_str(buf);
}

// ---- Numbers ----

#define MATH1(name, expr) \
    static struct script_value b_##name(struct script *s, struct script_value *a, int n) \
    { \
        double x = num_arg(s, a, n, 0, #name); \
        return script_num(expr); \
    }
MATH1(floor, floor(x))
MATH1(ceil, ceil(x))
MATH1(round, round(x))
MATH1(abs, fabs(x))
MATH1(sqrt, sqrt(x))
MATH1(sin, sin(x))
MATH1(cos, cos(x))
MATH1(tan, tan(x))
MATH1(log, log(x))
MATH1(exp, exp(x))

static struct script_value b_pow(struct script *s, struct script_value *a, int n)
{
    return script_num(pow(num_arg(s, a, n, 0, "pow"), num_arg(s, a, n, 1, "pow")));
}

static struct script_value minmax(struct script *s, struct script_value *a, int n, bool want_max)
{
    struct script_value *items = a;
    int count = n;
    double best;

    if (n == 1 && a[0].type == S_LIST) {
        items = a[0].l->items;
        count = a[0].l->n;
    }
    if (!count)
        return script_nil();
    best = num_arg(s, items, count, 0, want_max ? "max" : "min");
    for (int i = 1; i < count; i++) {
        double v = num_arg(s, items, count, i, want_max ? "max" : "min");

        best = want_max ? fmax(best, v) : fmin(best, v);
    }
    return script_num(best);
}

static struct script_value b_min(struct script *s, struct script_value *a, int n)
{
    return minmax(s, a, n, false);
}

static struct script_value b_max(struct script *s, struct script_value *a, int n)
{
    return minmax(s, a, n, true);
}

static uint64_t rng_state;

static struct script_value b_random(struct script *s, struct script_value *a, int n)
{
    double r;

    (void)s;
    if (!rng_state) {
        int fd = open("/osystem/devices/urandom", O_RDONLY);

        if (fd < 0 || read(fd, &rng_state, 8) != 8)
            rng_state = uptime_ms() | 1;
        if (fd >= 0)
            close(fd);
    }
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    r = (rng_state >> 11) * (1.0 / 9007199254740992.0);
    // random() in [0, 1); random(n) an integer below n; random(a, b) from a to b.
    if (n == 1 && a[0].type == S_NUM)
        return script_num(floor(r * a[0].n));
    if (n >= 2 && a[0].type == S_NUM && a[1].type == S_NUM)
        return script_num(a[0].n + floor(r * (a[1].n - a[0].n + 1)));
    return script_num(r);
}

// ---- Time ----

static struct script_value b_now(struct script *s, struct script_value *a, int n)
{
    (void)s;
    (void)a;
    (void)n;
    return script_num((double)time(NULL));
}

static struct script_value b_clock(struct script *s, struct script_value *a, int n)
{
    (void)s;
    (void)a;
    (void)n;
    return script_num((double)uptime_ms());
}

static struct script_value b_date(struct script *s, struct script_value *a, int n)
{
    const char *fmt = n > 0 ? str_arg(s, a, n, 0, "date") : "%Y-%m-%d %H:%M";
    int64_t t = n > 1 ? (int64_t)num_arg(s, a, n, 1, "date") : time(NULL);
    struct tm tm;
    char buf[128];

    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), fmt, &tm);
    return script_str(buf);
}

// ---- Files and programs ----

static char *slurp(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    struct aegis_stat st;
    char *buf;
    ssize_t got = 0, r;

    if (fd < 0 || fstat(fd, &st) < 0 || !(buf = malloc(st.size + 1))) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    while (got < (ssize_t)st.size && (r = read(fd, buf + got, st.size - got)) > 0)
        got += r;
    close(fd);
    buf[got] = 0;
    *len = got;
    return buf;
}

static char *expand_home(const char *path, char *out, size_t size)
{
    const char *home = getenv("HOME");

    if (path[0] == '~' && (path[1] == '/' || !path[1]) && home)
        snprintf(out, size, "%s%s", home, path + 1);
    else
        snprintf(out, size, "%s", path);
    return out;
}

static struct script_value b_read(struct script *s, struct script_value *a, int n)
{
    char path[512];
    size_t len;
    char *data = slurp(expand_home(str_arg(s, a, n, 0, "read"), path, sizeof(path)), &len);
    struct script_value v;

    if (!data)
        return script_nil();
    v = script_str_len(data, len);
    free(data);
    return v;
}

static struct script_value write_file(struct script *s, struct script_value *a, int n, bool append)
{
    char path[512], *text;
    int fd;
    size_t len;
    bool ok;

    expand_home(str_arg(s, a, n, 0, append ? "append" : "write"), path, sizeof(path));
    if ((fd = open(path, O_WRONLY | O_CREAT | (append ? 0 : O_TRUNC), 0644)) < 0)
        return script_bool(false);
    if (append)
        lseek(fd, 0, SEEK_END);
    text = script_to_string(ARG(1));
    len = strlen(text);
    ok = write(fd, text, len) == (ssize_t)len;
    free(text);
    close(fd);
    return script_bool(ok);
}

static struct script_value b_write(struct script *s, struct script_value *a, int n)
{
    return write_file(s, a, n, false);
}

static struct script_value b_append(struct script *s, struct script_value *a, int n)
{
    return write_file(s, a, n, true);
}

static struct script_value b_exists(struct script *s, struct script_value *a, int n)
{
    char path[512];
    struct aegis_stat st;

    return script_bool(stat(expand_home(str_arg(s, a, n, 0, "exists"), path, sizeof(path)), &st) == 0);
}

static struct script_value b_files(struct script *s, struct script_value *a, int n)
{
    char path[512];
    struct dir_stream *d = opendir(expand_home(n ? str_arg(s, a, n, 0, "files") : ".", path, sizeof(path)));
    struct aegis_dirent *e;
    struct script_value l = script_list();

    if (!d)
        return l;
    while ((e = readdir(d))) {
        struct script_value name;

        if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
            continue;
        name = script_str(e->name);
        script_list_push(l, name);
        script_release(name);
    }
    closedir(d);
    qsort(l.l->items, l.l->n, sizeof(struct script_value), compare_values);
    return l;
}

// run(command): runs a shell command and returns its output.
static struct script_value b_run(struct script *s, struct script_value *a, int n)
{
    const char *cmd = str_arg(s, a, n, 0, "run");
    char *argv[] = { "terminal", "-c", (char *)cmd, NULL }, buf[4096];
    int fds[2], saved, pid;
    struct sb_out {
        char *s;
        size_t len, cap;
    } out = { 0 };
    ssize_t r;
    struct script_value v;

    if (pipe(fds) < 0)
        return script_nil();
    saved = dup(STDOUT_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    pid = spawn("/sysapps/terminal", argv, environ);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    close(fds[1]);
    while ((r = read(fds[0], buf, sizeof(buf))) > 0) {
        if (out.len + r + 1 > out.cap) {
            char *g = realloc(out.s, out.cap = (out.len + r + 1) * 2);

            if (!g)
                break;
            out.s = g;
        }
        memcpy(out.s + out.len, buf, r);
        out.len += r;
    }
    close(fds[0]);
    if (pid > 0)
        waitpid(pid, NULL, 0);
    v = script_str_len(out.s ? out.s : "", out.len);
    free(out.s);
    return v;
}

static struct script_value b_env(struct script *s, struct script_value *a, int n)
{
    const char *v = getenv(str_arg(s, a, n, 0, "env"));

    return v ? script_str(v) : script_nil();
}

void script_builtins(struct script *s)
{
    static const struct {
        const char *name;
        script_native fn;
    } fns[] = {
        { "print", b_print }, { "len", b_len }, { "str", b_str }, { "num", b_num }, { "int", b_int },
        { "type", b_type }, { "push", b_push }, { "pop", b_pop }, { "insert", b_insert }, { "remove", b_remove },
        { "sort", b_sort }, { "reverse", b_reverse }, { "range", b_range }, { "keys", b_keys }, { "has", b_has },
        { "split", b_split }, { "join", b_join }, { "upper", b_upper }, { "lower", b_lower }, { "trim", b_trim },
        { "find", b_find }, { "replace", b_replace }, { "substr", b_substr }, { "starts", b_starts },
        { "format", b_format }, { "floor", b_floor }, { "ceil", b_ceil }, { "round", b_round }, { "abs", b_abs },
        { "sqrt", b_sqrt }, { "sin", b_sin }, { "cos", b_cos }, { "tan", b_tan }, { "log", b_log },
        { "exp", b_exp }, { "pow", b_pow }, { "min", b_min }, { "max", b_max }, { "random", b_random },
        { "now", b_now }, { "clock", b_clock }, { "date", b_date }, { "read", b_read }, { "write", b_write },
        { "append", b_append }, { "exists", b_exists }, { "files", b_files }, { "run", b_run }, { "env", b_env },
    };

    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
        script_define(s, fns[i].name, fns[i].fn);
    script_set_global(s, "PI", script_num(M_PI));
}
