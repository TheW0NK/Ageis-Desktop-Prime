#include "dom.h"

static int next_id = 1;

static const char *const containers[] = {
    "window", "vbox", "hbox", "box", "grid", "group", "card", "scroll", "stack", "tabs", "tab", "toolbar",
    "statusbar", "menubar", "menu", "list", "table", "dropdown", "div", "row", "column", NULL,
};

bool dom_is_container(const char *tag)
{
    for (int i = 0; containers[i]; i++)
        if (!strcmp(containers[i], tag))
            return true;
    return false;
}

struct node *dom_new(const char *tag)
{
    struct node *n = calloc(1, sizeof(*n));

    if (!n)
        return NULL;
    strlcpy(n->tag, tag, sizeof(n->tag));
    n->id = next_id++;
    return n;
}

void dom_free(struct node *n)
{
    struct node *c, *next;

    if (!n)
        return;
    for (c = n->first; c; c = next) {
        next = c->next;
        dom_free(c);
    }
    for (int i = 0; i < n->nattrs; i++) {
        free(n->names[i]);
        free(n->values[i]);
    }
    free(n);
}

struct node *dom_clone(const struct node *n)
{
    struct node *c = dom_new(n->tag);

    if (!c)
        return NULL;
    for (int i = 0; i < n->nattrs; i++)
        dom_set(c, n->names[i], n->values[i]);
    for (const struct node *k = n->first; k; k = k->next)
        dom_append(c, dom_clone(k));
    return c;
}

const char *dom_get(const struct node *n, const char *name)
{
    for (int i = 0; i < n->nattrs; i++)
        if (!strcmp(n->names[i], name))
            return n->values[i];
    return NULL;
}

void dom_set(struct node *n, const char *name, const char *value)
{
    for (int i = 0; i < n->nattrs; i++) {
        if (!strcmp(n->names[i], name)) {
            char *v = strdup(value);

            if (v) {
                free(n->values[i]);
                n->values[i] = v;
            }
            return;
        }
    }
    if (n->nattrs == DOM_MAX_ATTRS)
        return;
    n->names[n->nattrs] = strdup(name);
    n->values[n->nattrs] = strdup(value);
    if (n->names[n->nattrs] && n->values[n->nattrs])
        n->nattrs++;
}

void dom_unset(struct node *n, const char *name)
{
    for (int i = 0; i < n->nattrs; i++) {
        if (!strcmp(n->names[i], name)) {
            free(n->names[i]);
            free(n->values[i]);
            memmove(&n->names[i], &n->names[i + 1], (n->nattrs - i - 1) * sizeof(char *));
            memmove(&n->values[i], &n->values[i + 1], (n->nattrs - i - 1) * sizeof(char *));
            n->nattrs--;
            return;
        }
    }
}

void dom_append(struct node *parent, struct node *child)
{
    struct node **pp;

    if (!parent || !child)
        return;
    child->parent = parent;
    child->next = NULL;
    for (pp = &parent->first; *pp; pp = &(*pp)->next)
        ;
    *pp = child;
}

void dom_insert_after(struct node *ref, struct node *child)
{
    if (!ref || !child)
        return;
    child->parent = ref->parent;
    child->next = ref->next;
    ref->next = child;
}

struct node *dom_prev(struct node *n)
{
    struct node *p;

    if (!n->parent || n->parent->first == n)
        return NULL;
    for (p = n->parent->first; p && p->next != n; p = p->next)
        ;
    return p;
}

void dom_detach(struct node *n)
{
    struct node *prev;

    if (!n->parent)
        return;
    if ((prev = dom_prev(n)))
        prev->next = n->next;
    else
        n->parent->first = n->next;
    n->parent = n->next = NULL;
}

struct node *dom_find(struct node *root, int id)
{
    if (!root)
        return NULL;
    if (root->id == id)
        return root;
    for (struct node *c = root->first; c; c = c->next) {
        struct node *f = dom_find(c, id);

        if (f)
            return f;
    }
    return NULL;
}

// ---- Parsing ----

struct parser {
    const char *p;
    int line;
    char *err;
    size_t errsize;
    bool failed;
};

static void fail(struct parser *P, const char *msg, const char *arg)
{
    if (P->failed)
        return;
    P->failed = true;
    snprintf(P->err, P->errsize, "line %d: %s%s%s", P->line, msg, arg ? " " : "", arg ? arg : "");
}

static void adv(struct parser *P)
{
    if (*P->p == '\n')
        P->line++;
    if (*P->p)
        P->p++;
}

static void skip_ws(struct parser *P)
{
    while (isspace((unsigned char)*P->p))
        adv(P);
}

static bool name_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':' || c == '.';
}

static void read_entity(struct parser *P, char *out, size_t *len, size_t size)
{
    static const struct {
        const char *name, *text;
    } ents[] = { { "lt", "<" }, { "gt", ">" }, { "amp", "&" }, { "quot", "\"" }, { "apos", "'" },
                 { "nbsp", "\xC2\xA0" } };
    const char *semi = strchr(P->p, ';');
    char name[16];
    size_t n;

    if (!semi || (n = semi - P->p - 1) == 0 || n >= sizeof(name)) {
        if (*len + 1 < size)
            out[(*len)++] = '&';
        adv(P);
        return;
    }
    memcpy(name, P->p + 1, n);
    name[n] = 0;
    if (name[0] == '#') {
        uint32_t cp = name[1] == 'x' ? strtoul(name + 2, NULL, 16) : strtoul(name + 1, NULL, 10);
        char enc[4];
        int k = 0;

        // UTF-8 by hand (no gfx dependency here).
        if (cp < 0x80) enc[k++] = cp;
        else if (cp < 0x800) { enc[k++] = 0xC0 | cp >> 6; enc[k++] = 0x80 | (cp & 63); }
        else if (cp < 0x10000) { enc[k++] = 0xE0 | cp >> 12; enc[k++] = 0x80 | (cp >> 6 & 63); enc[k++] = 0x80 | (cp & 63); }
        else { enc[k++] = 0xF0 | cp >> 18; enc[k++] = 0x80 | (cp >> 12 & 63); enc[k++] = 0x80 | (cp >> 6 & 63); enc[k++] = 0x80 | (cp & 63); }
        for (int i = 0; i < k && *len + 1 < size; i++)
            out[(*len)++] = enc[i];
    } else {
        bool found = false;

        for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++)
            if (!strcmp(ents[i].name, name)) {
                for (const char *t = ents[i].text; *t && *len + 1 < size; t++)
                    out[(*len)++] = *t;
                found = true;
            }
        if (!found) {
            if (*len + 1 < size)
                out[(*len)++] = '&';
            adv(P);
            return;
        }
    }
    for (size_t i = 0; i < n + 2; i++)
        adv(P);
}

static bool skip_special(struct parser *P)
{
    if (!strncmp(P->p, "<!--", 4)) {
        const char *end = strstr(P->p, "-->");

        while (*P->p && (!end || P->p < end + 3))
            adv(P);
        return true;
    }
    if (!strncmp(P->p, "<?", 2) || !strncmp(P->p, "<!", 2)) {
        while (*P->p && *P->p != '>')
            adv(P);
        adv(P);
        return true;
    }
    return false;
}

static const char *const void_tags[] = { "input", "password", "spin", "separator", "spacer", "image", "img",
                                         "progress", "slider", "checkbox", "radio", "toggle", "canvas", "br",
                                         "hr", NULL };

static bool is_void(const char *tag)
{
    for (int i = 0; void_tags[i]; i++)
        if (!strcmp(void_tags[i], tag))
            return true;
    return false;
}

static struct node *element(struct parser *P);

static void content(struct parser *P, struct node *n)
{
    char text[4096];
    size_t len = 0;

    while (*P->p && !P->failed) {
        if (*P->p == '<') {
            if (skip_special(P))
                continue;
            if (P->p[1] == '/') {
                char name[32];
                int k = 0;

                adv(P);
                adv(P);
                while (name_char(*P->p) && k < 31) {
                    name[k++] = tolower((unsigned char)*P->p);
                    adv(P);
                }
                name[k] = 0;
                skip_ws(P);
                if (*P->p == '>')
                    adv(P);
                if (strcmp(name, n->tag)) {
                    if (is_void(name))
                        continue;
                    fail(P, "unexpected closing tag", name);
                }
                break;
            }
            adv(P);
            {
                struct node *c = element(P);

                if (c)
                    dom_append(n, c);
            }
            continue;
        }
        if (*P->p == '&') {
            read_entity(P, text, &len, sizeof(text));
            continue;
        }
        if (len + 1 < sizeof(text))
            text[len++] = *P->p;
        adv(P);
    }
    text[len] = 0;
    if (!dom_get(n, "text")) {
        bool raw = !strcmp(n->tag, "textarea");
        char *t = text;

        if (!raw) {
            // Collapse white space as AUI does.
            char *d = text;
            bool space = false;

            for (char *s = text; *s; s++) {
                if (isspace((unsigned char)*s)) {
                    space = d != text;
                    continue;
                }
                if (space)
                    *d++ = ' ';
                space = false;
                *d++ = *s;
            }
            *d = 0;
        } else if (*t == '\n') {
            t++;
        }
        if (*t)
            dom_set(n, "text", t);
    }
}

static struct node *element(struct parser *P)
{
    char tag[32];
    int k = 0;
    struct node *n;
    bool self = false;

    while (name_char(*P->p) && k < 31) {
        tag[k++] = tolower((unsigned char)*P->p);
        adv(P);
    }
    tag[k] = 0;
    if (!k) {
        fail(P, "expected an element name", NULL);
        return NULL;
    }
    if (!(n = dom_new(tag)))
        return NULL;
    while (*P->p && !P->failed) {
        char name[48], value[2048];
        size_t vl = 0;
        int nl = 0;

        skip_ws(P);
        if (*P->p == '>') {
            adv(P);
            break;
        }
        if (P->p[0] == '/' && P->p[1] == '>') {
            adv(P);
            adv(P);
            self = true;
            break;
        }
        while (name_char(*P->p) && nl < 47) {
            name[nl++] = tolower((unsigned char)*P->p);
            adv(P);
        }
        name[nl] = 0;
        if (!nl) {
            fail(P, "bad attribute in", tag);
            break;
        }
        skip_ws(P);
        if (*P->p == '=') {
            char q = 0;

            adv(P);
            skip_ws(P);
            if (*P->p == '"' || *P->p == '\'') {
                q = *P->p;
                adv(P);
            }
            while (*P->p && (q ? *P->p != q : !isspace((unsigned char)*P->p) && *P->p != '>' && *P->p != '/')) {
                if (*P->p == '&') {
                    read_entity(P, value, &vl, sizeof(value));
                    continue;
                }
                if (vl + 1 < sizeof(value))
                    value[vl++] = *P->p;
                adv(P);
            }
            if (q && *P->p == q)
                adv(P);
            value[vl] = 0;
        } else {
            strcpy(value, "true");
        }
        dom_set(n, name, value);
    }
    if (!self && !is_void(tag) && !P->failed)
        content(P, n);
    return n;
}

struct node *dom_parse(const char *text, char *err, size_t size)
{
    struct parser P = { text, 1, err, size, false };
    struct node *root = NULL;

    while (*P.p && !P.failed) {
        skip_ws(&P);
        if (!*P.p)
            break;
        if (*P.p != '<') {
            fail(&P, "text outside an element", NULL);
            break;
        }
        if (skip_special(&P))
            continue;
        adv(&P);
        if (root) {
            fail(&P, "only one <window> element is allowed", NULL);
            break;
        }
        root = element(&P);
    }
    if (P.failed) {
        dom_free(root);
        return NULL;
    }
    if (!root) {
        snprintf(err, size, "the file is empty");
        return NULL;
    }
    if (strcmp(root->tag, "window")) {
        // Wrap fragments in a window.
        struct node *w = dom_new("window");

        dom_append(w, root);
        root = w;
    }
    return root;
}

// ---- Writing ----

struct out {
    char *s;
    size_t len, cap;
};

static void put(struct out *o, const char *s)
{
    size_t n = strlen(s);

    if (o->len + n + 1 > o->cap) {
        size_t cap = MAX(o->cap * 2, o->len + n + 256);
        char *t = realloc(o->s, cap);

        if (!t)
            return;
        o->s = t;
        o->cap = cap;
    }
    memcpy(o->s + o->len, s, n + 1);
    o->len += n;
}

static void put_escaped(struct out *o, const char *s)
{
    char one[2] = { 0, 0 };

    for (; *s; s++) {
        switch (*s) {
        case '&': put(o, "&amp;"); break;
        case '<': put(o, "&lt;"); break;
        case '>': put(o, "&gt;"); break;
        case '"': put(o, "&quot;"); break;
        default: one[0] = *s; put(o, one);
        }
    }
}

static void write_node(struct out *o, const struct node *n, int depth, bool preview)
{
    char num[16];

    for (int i = 0; i < depth; i++)
        put(o, "  ");
    put(o, "<");
    put(o, n->tag);
    for (int i = 0; i < n->nattrs; i++) {
        if (preview && !strncmp(n->names[i], "on", 2) && n->names[i][2])
            continue;
        if (preview && !strcmp(n->names[i], "hidden"))
            continue;       // show everything while designing
        put(o, " ");
        put(o, n->names[i]);
        put(o, "=\"");
        put_escaped(o, n->values[i]);
        put(o, "\"");
    }
    if (preview) {
        snprintf(num, sizeof(num), "%d", n->id);
        put(o, " __wb=\"");
        put(o, num);
        put(o, "\"");
    }
    if (!n->first) {
        put(o, "/>\n");
        return;
    }
    put(o, ">\n");
    for (const struct node *c = n->first; c; c = c->next)
        write_node(o, c, depth + 1, preview);
    for (int i = 0; i < depth; i++)
        put(o, "  ");
    put(o, "</");
    put(o, n->tag);
    put(o, ">\n");
}

char *dom_serialize(const struct node *root, bool preview)
{
    struct out o = { 0 };

    if (preview) {
        for (const struct node *c = root->first; c; c = c->next)
            write_node(&o, c, 0, true);
    } else {
        write_node(&o, root, 0, false);
    }
    return o.s ? o.s : strdup("");
}
