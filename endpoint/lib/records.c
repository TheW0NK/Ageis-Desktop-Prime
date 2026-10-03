#include "aegis.h"

// Aegis record files: the format of accounts (.aacc) and settings (.aset).
//
//   aegis accounts 1           the first line: "aegis", the file's type, version
//   # a comment
//   theme: dark                a setting outside any block
//
//   account alex               a block: its kind and name
//       id: 1000               the block's settings, indented
//       display: Alex
//
// Blank lines and comments are not kept when a file is saved.

#define MAGIC "aegis"

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static struct rec_block *add_block(struct records *r, const char *kind, const char *name)
{
    struct rec_block *b;

    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 8;
        struct rec_block *nb = realloc(r->b, cap * sizeof(*nb));

        if (!nb)
            return NULL;
        r->b = nb;
        r->cap = cap;
    }
    b = &r->b[r->n++];
    memset(b, 0, sizeof(*b));
    strlcpy(b->kind, kind, sizeof(b->kind));
    strlcpy(b->name, name, sizeof(b->name));
    return b;
}

void records_init(struct records *r, const char *type)
{
    memset(r, 0, sizeof(*r));
    strlcpy(r->type, type, sizeof(r->type));
    add_block(r, "", "");           // block 0: settings outside any block
}

int records_load(const char *path, const char *type, struct records *r)
{
    char line[1024];
    struct rec_block *cur;
    int fd = open(path, O_RDONLY);
    bool first = true;

    records_init(r, type);
    if (fd < 0)
        return -1;
    cur = &r->b[0];
    while (read_line(fd, line, sizeof(line)) >= 0) {
        bool indented = line[0] == ' ' || line[0] == '\t';
        char *s = trim(line), *colon;

        if (first) {
            char want[48];

            first = false;
            snprintf(want, sizeof(want), MAGIC " %s ", type);
            if (strncmp(s, want, strlen(want))) {
                close(fd);
                records_free(r);
                records_init(r, type);
                errno = EINVAL;
                return -1;
            }
            continue;
        }
        if (!*s || *s == '#')
            continue;
        colon = strstr(s, ": ");
        if (!colon && s[strlen(s) - 1] == ':')
            colon = s + strlen(s) - 1;
        if (!indented && !colon) {
            // "kind name" starts a block.
            char *sp = strchr(s, ' ');

            if (!sp)
                continue;
            *sp = 0;
            if (!(cur = add_block(r, s, trim(sp + 1))))
                break;
            continue;
        }
        if (!colon)
            continue;
        *colon = 0;
        if (!indented)
            cur = &r->b[0];
        rec_set(cur, trim(s), trim(colon + 1));
    }
    close(fd);
    return 0;
}

void records_free(struct records *r)
{
    for (int i = 0; i < r->n; i++) {
        for (int k = 0; k < r->b[i].n; k++) {
            free(r->b[i].f[k].key);
            free(r->b[i].f[k].value);
        }
        free(r->b[i].f);
    }
    free(r->b);
    memset(r, 0, sizeof(*r));
}

int records_save(const struct records *r, const char *path, uint32_t mode)
{
    char tmp[300];
    struct aegis_stat st;
    bool had = stat(path, &st) == 0;
    int fd;

    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, had ? st.mode & 0777 : mode)) < 0)
        return -1;
    dprintf(fd, MAGIC " %s 1\n", r->type);
    for (int i = 0; i < r->n; i++) {
        const struct rec_block *b = &r->b[i];

        if (i == 0 && !b->n)
            continue;
        if (i > 0)
            dprintf(fd, "\n%s %s\n", b->kind, b->name);
        for (int k = 0; k < b->n; k++)
            dprintf(fd, "%s%s: %s\n", i ? "    " : "", b->f[k].key, b->f[k].value);
    }
    if (had) {
        chmod(tmp, st.mode & 0777);
        chown(tmp, st.uid, st.gid);
    }
    close(fd);
    sync();
    return rename(tmp, path);
}

struct rec_block *records_top(struct records *r)
{
    return &r->b[0];
}

struct rec_block *records_find(struct records *r, const char *kind, const char *name)
{
    for (int i = 1; i < r->n; i++)
        if (!strcmp(r->b[i].kind, kind) && !strcmp(r->b[i].name, name))
            return &r->b[i];
    return NULL;
}

struct rec_block *records_add(struct records *r, const char *kind, const char *name)
{
    struct rec_block *b = records_find(r, kind, name);

    return b ? b : add_block(r, kind, name);
}

void records_remove(struct records *r, const char *kind, const char *name)
{
    struct rec_block *b = records_find(r, kind, name);
    int i;

    if (!b)
        return;
    i = b - r->b;
    for (int k = 0; k < b->n; k++) {
        free(b->f[k].key);
        free(b->f[k].value);
    }
    free(b->f);
    memmove(&r->b[i], &r->b[i + 1], (r->n - i - 1) * sizeof(*b));
    r->n--;
}

const char *rec_get(const struct rec_block *b, const char *key)
{
    for (int k = 0; b && k < b->n; k++)
        if (!strcmp(b->f[k].key, key))
            return b->f[k].value;
    return NULL;
}

int rec_set(struct rec_block *b, const char *key, const char *value)
{
    char *v;

    for (int k = 0; k < b->n; k++)
        if (!strcmp(b->f[k].key, key)) {
            if (!(v = strdup(value)))
                return -1;
            free(b->f[k].value);
            b->f[k].value = v;
            return 0;
        }
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 8;
        struct rec_field *nf = realloc(b->f, cap * sizeof(*nf));

        if (!nf)
            return -1;
        b->f = nf;
        b->cap = cap;
    }
    b->f[b->n].key = strdup(key);
    b->f[b->n].value = strdup(value);
    if (!b->f[b->n].key || !b->f[b->n].value)
        return -1;
    b->n++;
    return 0;
}

void rec_unset(struct rec_block *b, const char *key)
{
    for (int k = 0; k < b->n; k++)
        if (!strcmp(b->f[k].key, key)) {
            free(b->f[k].key);
            free(b->f[k].value);
            memmove(&b->f[k], &b->f[k + 1], (b->n - k - 1) * sizeof(b->f[0]));
            b->n--;
            return;
        }
}

// Lists ("user, alex"): is item in it, and adding or removing it.
bool rec_list_has(const char *list, const char *item)
{
    size_t n = strlen(item);

    for (const char *p = list; p && *p;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        while (len && *p == ' ')
            p++, len--;
        while (len && p[len - 1] == ' ')
            len--;
        if (len == n && !strncmp(p, item, n))
            return true;
        p = comma ? comma + 1 : NULL;
    }
    return false;
}

void rec_list_edit(struct rec_block *b, const char *key, const char *item, bool present)
{
    const char *list = rec_get(b, key);
    char out[1024] = "";

    for (const char *p = list; p && *p;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char one[128];

        while (len && *p == ' ')
            p++, len--;
        while (len && p[len - 1] == ' ')
            len--;
        snprintf(one, sizeof(one), "%.*s", (int)len, p);
        if (*one && strcmp(one, item)) {
            if (*out)
                strlcat(out, ", ", sizeof(out));
            strlcat(out, one, sizeof(out));
        }
        p = comma ? comma + 1 : NULL;
    }
    if (present) {
        if (*out)
            strlcat(out, ", ", sizeof(out));
        strlcat(out, item, sizeof(out));
    }
    rec_set(b, key, out);
}

// One setting of a settings file (.aset) outside any block.
int aset_get(const char *path, const char *key, char *buf, size_t size)
{
    struct records r;
    const char *v;
    int n = -1;

    if (size && records_load(path, "settings", &r) == 0 && (v = rec_get(records_top(&r), key))) {
        strlcpy(buf, v, size);
        n = strlen(buf);
    }
    records_free(&r);
    return n;
}

int aset_set(const char *path, const char *key, const char *value, uint32_t mode)
{
    struct records r;
    int ret;

    records_load(path, "settings", &r);
    ret = rec_set(records_top(&r), key, value);
    if (ret == 0)
        ret = records_save(&r, path, mode);
    records_free(&r);
    return ret;
}
