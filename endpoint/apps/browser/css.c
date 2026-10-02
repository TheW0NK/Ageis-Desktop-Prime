#include "browser.h"
#include <math.h>

// CSS: style sheets are parsed into rules (one per selector), matched
// right to left against elements, and cascaded by importance, origin,
// specificity and order into each element's computed style.

int css_viewport_width = 1024, css_viewport_height = 768;

// Border colours not given follow the text colour, once that is known.
#define CURRENT_COLOR 0x00010203U

#define MAX_PARTS 12

enum {
    PSEUDO_FIRST_CHILD = 1, PSEUDO_LAST_CHILD = 2, PSEUDO_ONLY_CHILD = 4, PSEUDO_LINK = 8, PSEUDO_ROOT = 16,
    PSEUDO_EMPTY = 32, PSEUDO_CHECKED = 64, PSEUDO_DISABLED = 128, PSEUDO_FIRST_OF_TYPE = 256,
    PSEUDO_LAST_OF_TYPE = 512,
};

struct attrsel {
    char *name, *value;
    char op;                        // 0 exists, '=' equal, '~' word, '^' prefix, '$' suffix, '*' contains, '|' lang
};

struct compound {
    char *tag;                      // NULL: any
    char *id;
    char **classes;
    int nclasses;
    struct attrsel *attrs;
    int nattrs;
    uint16_t pseudo;
    int nth_a, nth_b;               // :nth-child(an+b) when has_nth
    bool has_nth;
    struct compound *not_;          // :not(simple)
    bool never;                     // has something that never matches here (:hover, ::before, ...)
};

struct selector {
    struct compound parts[MAX_PARTS];   // left to right
    char comb[MAX_PARTS];           // comb[i]: between parts[i-1] and parts[i] (' ', '>', '+', '~')
    int n;
    int spec;
};

struct decl {
    char *prop, *value;
    bool important;
};

struct block {
    struct decl *decls;
    int n;
};

struct rule {
    struct selector sel;
    struct block *block;
    const char *media;              // NULL: always
    int order;
};

struct sheet {
    struct rule *rules;
    int n, cap;
    struct block **blocks;
    int nblocks;
    char **medias;
    int nmedias;
    int origin;                     // 0 user agent, 1 author
};

static int rule_counter;

// ---- Small helpers ----

static char *trim_dup(const char *s, const char *e)
{
    while (s < e && isspace((unsigned char)*s))
        s++;
    while (e > s && isspace((unsigned char)e[-1]))
        e--;
    return strndup(s, e - s);
}

static bool ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80 || c == '\\';
}

static char *read_ident(const char **p)
{
    const char *s = *p;
    char out[128];
    size_t n = 0;

    while (*s && ident_char(*s)) {
        if (*s == '\\' && s[1]) {
            s++;
            if (isxdigit((unsigned char)*s)) {
                // An escaped code point: keep it simple and skip it.
                while (isxdigit((unsigned char)*s))
                    s++;
                if (*s == ' ')
                    s++;
                continue;
            }
        }
        if (n + 1 < sizeof(out))
            out[n++] = *s;
        s++;
    }
    out[n] = 0;
    *p = s;
    return strdup(out);
}

static void compound_free(struct compound *c)
{
    free(c->tag);
    free(c->id);
    for (int i = 0; i < c->nclasses; i++)
        free(c->classes[i]);
    free(c->classes);
    for (int i = 0; i < c->nattrs; i++) {
        free(c->attrs[i].name);
        free(c->attrs[i].value);
    }
    free(c->attrs);
    if (c->not_) {
        compound_free(c->not_);
        free(c->not_);
    }
}

// ---- Selectors ----

static void parse_nth(const char *s, int *a, int *b)
{
    while (isspace((unsigned char)*s))
        s++;
    if (!strncasecmp(s, "odd", 3)) {
        *a = 2;
        *b = 1;
    } else if (!strncasecmp(s, "even", 4)) {
        *a = 2;
        *b = 0;
    } else if (strchr(s, 'n')) {
        const char *n = strchr(s, 'n');

        *a = n == s || (n == s + 1 && *s == '+') ? 1 : (n == s + 1 && *s == '-') ? -1 : atoi(s);
        s = n + 1;
        while (isspace((unsigned char)*s))
            s++;
        if (*s == '+' || *s == '-') {
            int sign = *s == '-' ? -1 : 1;

            s++;
            while (isspace((unsigned char)*s))
                s++;
            *b = sign * atoi(s);
        } else {
            *b = 0;
        }
    } else {
        *a = 0;
        *b = atoi(s);
    }
}

// Parses one compound selector; returns false if it is not valid.
static bool parse_compound(const char **pp, struct compound *c, int *spec)
{
    const char *p = *pp;
    bool any = false;

    memset(c, 0, sizeof(*c));
    if (*p == '*') {
        p++;
        any = true;
    } else if (ident_char(*p)) {
        c->tag = read_ident(&p);
        for (char *t = c->tag; *t; t++)
            *t = tolower((unsigned char)*t);
        *spec += 1;
        any = true;
    }
    for (;;) {
        if (*p == '#') {
            p++;
            free(c->id);
            c->id = read_ident(&p);
            *spec += 10000;
        } else if (*p == '.') {
            char **m = realloc(c->classes, (c->nclasses + 1) * sizeof(char *));

            p++;
            if (!m)
                return false;
            c->classes = m;
            c->classes[c->nclasses++] = read_ident(&p);
            *spec += 100;
        } else if (*p == '[') {
            struct attrsel a = { 0 };
            struct attrsel *m;

            p++;
            while (isspace((unsigned char)*p))
                p++;
            a.name = read_ident(&p);
            for (char *t = a.name; *t; t++)
                *t = tolower((unsigned char)*t);
            while (isspace((unsigned char)*p))
                p++;
            if (*p == '=' || (strchr("~^$*|", *p) && *p && p[1] == '=')) {
                a.op = *p;
                p += *p == '=' ? 1 : 2;
                while (isspace((unsigned char)*p))
                    p++;
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    const char *s = p;

                    while (*p && *p != q)
                        p++;
                    a.value = strndup(s, p - s);
                    if (*p)
                        p++;
                } else {
                    a.value = read_ident(&p);
                }
                while (*p && *p != ']')
                    p++;
            }
            if (*p == ']')
                p++;
            if (!(m = realloc(c->attrs, (c->nattrs + 1) * sizeof(*m))))
                return false;
            c->attrs = m;
            c->attrs[c->nattrs++] = a;
            *spec += 100;
        } else if (*p == ':') {
            bool element = p[1] == ':';
            char *name;

            p += element ? 2 : 1;
            name = read_ident(&p);
            for (char *t = name; *t; t++)
                *t = tolower((unsigned char)*t);
            if (*p == '(') {
                // Arguments up to the matching parenthesis.
                const char *s = ++p;
                int depth = 1;
                char *arg;

                while (*p && depth) {
                    depth += *p == '(' ? 1 : *p == ')' ? -1 : 0;
                    p++;
                }
                arg = strndup(s, p - s - (depth ? 0 : 1));
                if (!strcmp(name, "not")) {
                    const char *q = arg;
                    int ignored = 0;

                    while (isspace((unsigned char)*q))
                        q++;
                    c->not_ = calloc(1, sizeof(*c->not_));
                    if (!c->not_ || !parse_compound(&q, c->not_, &ignored) || (*q && !isspace((unsigned char)*q)))
                        c->never = true;
                    *spec += 100;
                } else if (!strcmp(name, "nth-child")) {
                    parse_nth(arg, &c->nth_a, &c->nth_b);
                    c->has_nth = true;
                    *spec += 100;
                } else if (!strcmp(name, "is") || !strcmp(name, "where") || !strcmp(name, "matches")) {
                    // Only the simple case of one compound.
                    const char *q = arg;
                    struct compound inner;
                    int sp = 0;

                    while (isspace((unsigned char)*q))
                        q++;
                    if (!strchr(arg, ',') && parse_compound(&q, &inner, &sp) && !*q) {
                        // Merge into this one.
                        if (inner.tag && !c->tag)
                            c->tag = strdup(inner.tag);
                        for (int i = 0; i < inner.nclasses; i++) {
                            char **m = realloc(c->classes, (c->nclasses + 1) * sizeof(char *));

                            if (m) {
                                c->classes = m;
                                c->classes[c->nclasses++] = strdup(inner.classes[i]);
                            }
                        }
                        if (inner.id && !c->id)
                            c->id = strdup(inner.id);
                        c->pseudo |= inner.pseudo;
                        c->never |= inner.never;
                        if (strcmp(name, "where"))
                            *spec += sp;
                    } else {
                        c->never = true;
                    }
                    compound_free(&inner);
                } else {
                    c->never = true;    // :lang(), :has(), :nth-of-type(), ...
                }
                free(arg);
            } else if (element) {
                c->never = true;        // ::before, ::after, ::marker, ...
            } else if (!strcmp(name, "first-child")) {
                c->pseudo |= PSEUDO_FIRST_CHILD;
            } else if (!strcmp(name, "last-child")) {
                c->pseudo |= PSEUDO_LAST_CHILD;
            } else if (!strcmp(name, "only-child")) {
                c->pseudo |= PSEUDO_ONLY_CHILD;
            } else if (!strcmp(name, "first-of-type")) {
                c->pseudo |= PSEUDO_FIRST_OF_TYPE;
            } else if (!strcmp(name, "last-of-type")) {
                c->pseudo |= PSEUDO_LAST_OF_TYPE;
            } else if (!strcmp(name, "link") || !strcmp(name, "any-link")) {
                c->pseudo |= PSEUDO_LINK;
            } else if (!strcmp(name, "root")) {
                c->pseudo |= PSEUDO_ROOT;
            } else if (!strcmp(name, "empty")) {
                c->pseudo |= PSEUDO_EMPTY;
            } else if (!strcmp(name, "checked")) {
                c->pseudo |= PSEUDO_CHECKED;
            } else if (!strcmp(name, "disabled")) {
                c->pseudo |= PSEUDO_DISABLED;
            } else if (strcmp(name, "enabled")) {
                // :visited never matches (nothing is remembered), and
                // neither do the dynamic ones (:hover, :focus, :active).
                c->never = true;
            }
            free(name);
            if (!element)
                *spec += 100;
            else
                *spec += 1;
        } else {
            break;
        }
        any = true;
    }
    *pp = p;
    return any;
}

static bool parse_selector(const char *s, struct selector *sel)
{
    const char *p = s;
    char comb = 0;

    memset(sel, 0, sizeof(*sel));
    while (*p) {
        while (isspace((unsigned char)*p)) {
            p++;
            if (!comb)
                comb = ' ';
        }
        if (!*p)
            break;
        if (*p == '>' || *p == '+' || *p == '~') {
            comb = *p++;
            while (isspace((unsigned char)*p))
                p++;
        }
        if (sel->n == MAX_PARTS)
            return false;
        if (!parse_compound(&p, &sel->parts[sel->n], &sel->spec)) {
            compound_free(&sel->parts[sel->n]);
            return false;
        }
        sel->comb[sel->n] = sel->n ? (comb ? comb : ' ') : 0;
        sel->n++;
        comb = 0;
        if (*p && !isspace((unsigned char)*p) && !strchr(">+~", *p))
            return false;
    }
    return sel->n > 0;
}

static bool has_word(const char *list, const char *word)
{
    size_t wl = strlen(word);

    if (!list || !wl)
        return false;
    for (const char *p = list; *p;) {
        while (isspace((unsigned char)*p))
            p++;
        const char *s = p;

        while (*p && !isspace((unsigned char)*p))
            p++;
        if ((size_t)(p - s) == wl && !strncmp(s, word, wl))
            return true;
    }
    return false;
}

static struct node *prev_element(struct node *n)
{
    for (n = n->prev; n; n = n->prev)
        if (n->type == NODE_ELEMENT)
            return n;
    return NULL;
}

static struct node *next_element(struct node *n)
{
    for (n = n->next; n; n = n->next)
        if (n->type == NODE_ELEMENT)
            return n;
    return NULL;
}

static bool match_compound(const struct compound *c, struct node *n)
{
    if (c->never || n->type != NODE_ELEMENT)
        return false;
    if (c->tag && strcmp(c->tag, n->tag))
        return false;
    if (c->id) {
        const char *id = node_attr(n, "id");

        if (!id || strcmp(id, c->id))
            return false;
    }
    if (c->nclasses) {
        const char *cls = node_attr(n, "class");

        for (int i = 0; i < c->nclasses; i++)
            if (!has_word(cls, c->classes[i]))
                return false;
    }
    for (int i = 0; i < c->nattrs; i++) {
        const struct attrsel *a = &c->attrs[i];
        const char *v = node_attr(n, a->name);
        size_t vl, al;

        if (!v)
            return false;
        if (!a->op)
            continue;
        vl = strlen(v);
        al = strlen(a->value);
        switch (a->op) {
        case '=':
            if (strcmp(v, a->value) && !(!strcmp(a->name, "type") && !strcasecmp(v, a->value)))
                return false;
            break;
        case '~':
            if (!has_word(v, a->value))
                return false;
            break;
        case '^':
            if (!al || strncmp(v, a->value, al))
                return false;
            break;
        case '$':
            if (!al || vl < al || strcmp(v + vl - al, a->value))
                return false;
            break;
        case '*':
            if (!al || !strstr(v, a->value))
                return false;
            break;
        case '|':
            if (strncmp(v, a->value, al) || (v[al] && v[al] != '-'))
                return false;
            break;
        }
    }
    if (c->pseudo) {
        if ((c->pseudo & (PSEUDO_FIRST_CHILD | PSEUDO_ONLY_CHILD)) && prev_element(n))
            return false;
        if ((c->pseudo & (PSEUDO_LAST_CHILD | PSEUDO_ONLY_CHILD)) && next_element(n))
            return false;
        if (c->pseudo & PSEUDO_FIRST_OF_TYPE)
            for (struct node *s = prev_element(n); s; s = prev_element(s))
                if (!strcmp(s->tag, n->tag))
                    return false;
        if (c->pseudo & PSEUDO_LAST_OF_TYPE)
            for (struct node *s = next_element(n); s; s = next_element(s))
                if (!strcmp(s->tag, n->tag))
                    return false;
        if ((c->pseudo & PSEUDO_LINK) && !((node_is(n, "a") || node_is(n, "area")) && node_attr(n, "href")))
            return false;
        if ((c->pseudo & PSEUDO_ROOT) && !(n->parent && n->parent->type == NODE_DOCUMENT))
            return false;
        if ((c->pseudo & PSEUDO_EMPTY) && n->first)
            return false;
        if ((c->pseudo & PSEUDO_CHECKED) && !n->checked)
            return false;
        if ((c->pseudo & PSEUDO_DISABLED) && !node_attr(n, "disabled"))
            return false;
    }
    if (c->has_nth) {
        int index = 1;

        for (struct node *s = prev_element(n); s; s = prev_element(s))
            index++;
        if (c->nth_a == 0) {
            if (index != c->nth_b)
                return false;
        } else if ((index - c->nth_b) % c->nth_a || (index - c->nth_b) / c->nth_a < 0) {
            return false;
        }
    }
    if (c->not_ && match_compound(c->not_, n))
        return false;
    return true;
}

// Does parts[0..i] match with parts[i] at n?
static bool match_from(const struct selector *sel, int i, struct node *n)
{
    if (!match_compound(&sel->parts[i], n))
        return false;
    if (i == 0)
        return true;
    switch (sel->comb[i]) {
    case '>':
        return n->parent && n->parent->type == NODE_ELEMENT && match_from(sel, i - 1, n->parent);
    case '+': {
        struct node *p = prev_element(n);

        return p && match_from(sel, i - 1, p);
    }
    case '~':
        for (struct node *p = prev_element(n); p; p = prev_element(p))
            if (match_from(sel, i - 1, p))
                return true;
        return false;
    default:
        for (struct node *p = n->parent; p && p->type == NODE_ELEMENT; p = p->parent)
            if (match_from(sel, i - 1, p))
                return true;
        return false;
    }
}

// ---- Declarations ----

static struct block *parse_block(const char *s, const char *e)
{
    struct block *b = calloc(1, sizeof(*b));

    if (!b)
        return NULL;
    while (s < e) {
        const char *start = s, *colon = NULL;
        int depth = 0;
        char quote = 0;

        while (s < e) {
            if (quote) {
                if (*s == '\\' && s + 1 < e)
                    s++;
                else if (*s == quote)
                    quote = 0;
            } else if (*s == '"' || *s == '\'') {
                quote = *s;
            } else if (*s == '(') {
                depth++;
            } else if (*s == ')') {
                depth--;
            } else if (*s == ':' && !colon && !depth) {
                colon = s;
            } else if (*s == ';' && !depth) {
                break;
            }
            s++;
        }
        if (colon) {
            char *prop = trim_dup(start, colon), *value = trim_dup(colon + 1, s);
            char *bang = value ? strrchr(value, '!') : NULL;
            bool important = false;
            struct decl *m;

            if (bang && !strncasecmp(bang + 1, "important", 9) && !strchr(bang, ')')) {
                important = true;
                *bang = 0;
                while (bang > value && isspace((unsigned char)bang[-1]))
                    *--bang = 0;
            }
            if (prop && value && *prop && (m = realloc(b->decls, (b->n + 1) * sizeof(*m)))) {
                if (prop[0] != '-' || prop[1] != '-')
                    for (char *t = prop; *t; t++)
                        *t = tolower((unsigned char)*t);
                b->decls = m;
                b->decls[b->n++] = (struct decl){ prop, value, important };
            } else {
                free(prop);
                free(value);
            }
        }
        if (s < e)
            s++;
    }
    return b;
}

static void block_free(struct block *b)
{
    if (!b)
        return;
    for (int i = 0; i < b->n; i++) {
        free(b->decls[i].prop);
        free(b->decls[i].value);
    }
    free(b->decls);
    free(b);
}

// ---- Sheets ----

// Skips a {...} block starting at p (on '{'); returns just after it.
static const char *skip_block(const char *p)
{
    int depth = 0;
    char quote = 0;

    for (; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == quote)
                quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}' && --depth == 0) {
            return p + 1;
        }
    }
    return p;
}

static void add_rules(struct sheet *sh, const char *sels, struct block *b, const char *media)
{
    const char *p = sels;

    while (*p) {
        const char *s = p;
        int depth = 0;
        char *one;

        while (*p && (*p != ',' || depth)) {
            depth += *p == '(' ? 1 : *p == ')' ? -1 : 0;
            p++;
        }
        one = trim_dup(s, p);
        if (*p)
            p++;
        if (!one)
            continue;
        if (sh->n == sh->cap) {
            int cap = sh->cap ? sh->cap * 2 : 64;
            struct rule *m = realloc(sh->rules, cap * sizeof(*m));

            if (!m) {
                free(one);
                return;
            }
            sh->rules = m;
            sh->cap = cap;
        }
        if (parse_selector(one, &sh->rules[sh->n].sel)) {
            sh->rules[sh->n].block = b;
            sh->rules[sh->n].media = media;
            sh->rules[sh->n].order = rule_counter++;
            sh->n++;
        } else {
            for (int i = 0; i < MAX_PARTS; i++)
                compound_free(&sh->rules[sh->n].sel.parts[i]);
        }
        free(one);
    }
}

static void parse_rules(struct sheet *sh, const char *p, const char *end, const char *media);

static const char *keep_media(struct sheet *sh, const char *outer, const char *s, const char *e)
{
    char *m = trim_dup(s, e), **list;

    if (!m)
        return outer;
    if (outer) {
        // Nested @media: both must hold.
        char *both = malloc(strlen(outer) + strlen(m) + 8);

        if (both) {
            sprintf(both, "%s and %s", outer, m);
            free(m);
            m = both;
        }
    }
    if (!(list = realloc(sh->medias, (sh->nmedias + 1) * sizeof(char *)))) {
        free(m);
        return outer;
    }
    sh->medias = list;
    sh->medias[sh->nmedias++] = m;
    return m;
}

static void parse_rules(struct sheet *sh, const char *p, const char *end, const char *media)
{
    while (p < end) {
        const char *s, *open;

        while (p < end && (isspace((unsigned char)*p) || *p == ';' || *p == '}'))
            p++;
        if (p >= end)
            break;
        if (!strncmp(p, "<!--", 4) || !strncmp(p, "-->", 3)) {
            p += *p == '<' ? 4 : 3;
            continue;
        }
        s = p;
        // Up to '{' or (for statements like @import) ';'.
        while (p < end && *p != '{' && !(*s == '@' && *p == ';'))
            p++;
        if (p >= end)
            break;
        if (*p == ';') {
            p++;
            continue;
        }
        open = p;
        if (*s == '@') {
            const char *after = skip_block(open);

            if (!strncasecmp(s, "@media", 6)) {
                const char *m = keep_media(sh, media, s + 6, open);

                parse_rules(sh, open + 1, after - 1, m);
            } else if (!strncasecmp(s, "@supports", 9) || !strncasecmp(s, "@layer", 6)
                       || !strncasecmp(s, "@document", 9)) {
                parse_rules(sh, open + 1, after - 1, media);
            }
            p = after;
            continue;
        }
        {
            const char *close = skip_block(open);
            char *sels = trim_dup(s, open);
            struct block *b = parse_block(open + 1, close > open + 1 ? close - 1 : close);
            struct block **blocks = b ? realloc(sh->blocks, (sh->nblocks + 1) * sizeof(*blocks)) : NULL;

            if (blocks) {
                sh->blocks = blocks;
                sh->blocks[sh->nblocks++] = b;
                if (sels)
                    add_rules(sh, sels, b, media);
            } else {
                block_free(b);
            }
            free(sels);
            p = close;
        }
    }
}

struct sheet *css_parse(const char *src, int origin)
{
    struct sheet *sh = calloc(1, sizeof(*sh));
    char *clean;
    size_t n = 0;

    if (!sh || !src)
        return sh;
    sh->origin = origin;
    // Comments out first.
    if (!(clean = malloc(strlen(src) + 1)))
        return sh;
    for (const char *p = src; *p;) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");

            p = e ? e + 2 : p + strlen(p);
            clean[n++] = ' ';
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p;

            clean[n++] = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && p[1])
                    clean[n++] = *p++;
                clean[n++] = *p++;
            }
            if (*p)
                clean[n++] = *p++;
            continue;
        }
        clean[n++] = *p++;
    }
    clean[n] = 0;
    parse_rules(sh, clean, clean + n, NULL);
    free(clean);
    return sh;
}

void css_free(struct sheet *sh)
{
    if (!sh)
        return;
    for (int i = 0; i < sh->n; i++)
        for (int j = 0; j < sh->rules[i].sel.n; j++)
            compound_free(&sh->rules[i].sel.parts[j]);
    free(sh->rules);
    for (int i = 0; i < sh->nblocks; i++)
        block_free(sh->blocks[i]);
    free(sh->blocks);
    for (int i = 0; i < sh->nmedias; i++)
        free(sh->medias[i]);
    free(sh->medias);
    free(sh);
}

// ---- Media queries ----

static bool media_one(const char *q)
{
    // "only screen and (min-width: 600px) and (orientation: landscape)"
    char buf[256], *save = buf;
    bool ok = true, negate = false;

    strlcpy(buf, q, sizeof(buf));
    for (char *t = buf; *t; t++)
        *t = tolower((unsigned char)*t);
    while (*save) {
        char *word;

        while (isspace((unsigned char)*save))
            save++;
        if (!*save)
            break;
        word = save;
        if (*save == '(') {
            while (*save && *save != ')')
                save++;
            if (*save)
                *save++ = 0;
            word++;
            {
                char *colon = strchr(word, ':');
                float v;
                char *unit;

                if (!colon)
                    continue;
                *colon = 0;
                v = strtod(colon + 1, &unit);
                while (isspace((unsigned char)*unit))
                    unit++;
                if (!strncmp(unit, "em", 2) || !strncmp(unit, "rem", 3))
                    v *= 16;
                while (isspace((unsigned char)*word))
                    word++;
                if (!strncmp(word, "min-width", 9))
                    ok &= css_viewport_width >= v;
                else if (!strncmp(word, "max-width", 9))
                    ok &= css_viewport_width <= v;
                else if (!strncmp(word, "min-height", 10))
                    ok &= css_viewport_height >= v;
                else if (!strncmp(word, "max-height", 10))
                    ok &= css_viewport_height <= v;
                else if (!strncmp(word, "prefers-color-scheme", 20))
                    ok &= strstr(colon + 1, "light") != NULL;
                else if (!strncmp(word, "prefers-reduced-motion", 22))
                    ok &= strstr(colon + 1, "reduce") != NULL;
                else if (!strncmp(word, "orientation", 11))
                    ok &= strstr(colon + 1, css_viewport_width >= css_viewport_height ? "landscape" : "portrait")
                          != NULL;
                else if (!strncmp(word, "hover", 5) || !strncmp(word, "pointer", 7))
                    ok &= !strstr(colon + 1, "none") && !strstr(colon + 1, "coarse");
            }
            continue;
        }
        while (*save && !isspace((unsigned char)*save) && *save != '(')
            save++;
        if (*save && *save != '(')
            *save++ = 0;
        else if (*save == '(') {
            // "screen(" without a space: split here.
            memmove(save + 1, save, strlen(save) + 1);
            *save++ = 0;
        }
        if (!strcmp(word, "not"))
            negate = true;
        else if (!strcmp(word, "print") || !strcmp(word, "speech") || !strcmp(word, "tv")
                 || !strcmp(word, "handheld"))
            ok = false;
    }
    return negate ? !ok : ok;
}

static bool media_matches(const char *m)
{
    char buf[512];
    char *p = buf;

    if (!m)
        return true;
    strlcpy(buf, m, sizeof(buf));
    // A comma means "or". Nested queries were joined with "and".
    while (p) {
        char *comma = strchr(p, ',');

        if (comma)
            *comma = 0;
        if (media_one(p))
            return true;
        p = comma ? comma + 1 : NULL;
    }
    return false;
}

// ---- Values ----

static const struct {
    const char *name;
    uint32_t rgb;
} named_colors[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
    { "magenta", 0xFF00FF }, { "lime", 0x00FF00 }, { "olive", 0x808000 }, { "navy", 0x000080 },
    { "teal", 0x008080 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "orange", 0xFFA500 },
    { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "indigo", 0x4B0082 },
    { "violet", 0xEE82EE }, { "coral", 0xFF7F50 }, { "salmon", 0xFA8072 }, { "tomato", 0xFF6347 },
    { "crimson", 0xDC143C }, { "khaki", 0xF0E68C }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 },
    { "lavender", 0xE6E6FA }, { "plum", 0xDDA0DD }, { "orchid", 0xDA70D6 }, { "tan", 0xD2B48C },
    { "chocolate", 0xD2691E }, { "sienna", 0xA0522D }, { "peru", 0xCD853F }, { "wheat", 0xF5DEB3 },
    { "linen", 0xFAF0E6 }, { "snow", 0xFFFAFA }, { "azure", 0xF0FFFF }, { "mintcream", 0xF5FFFA },
    { "honeydew", 0xF0FFF0 }, { "aliceblue", 0xF0F8FF }, { "ghostwhite", 0xF8F8FF },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "lightgray", 0xD3D3D3 },
    { "lightgrey", 0xD3D3D3 }, { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "dimgray", 0x696969 },
    { "dimgrey", 0x696969 }, { "lightslategray", 0x778899 }, { "slategray", 0x708090 },
    { "darkslategray", 0x2F4F4F }, { "lightblue", 0xADD8E6 }, { "skyblue", 0x87CEEB },
    { "lightskyblue", 0x87CEFA }, { "deepskyblue", 0x00BFFF }, { "dodgerblue", 0x1E90FF },
    { "cornflowerblue", 0x6495ED }, { "steelblue", 0x4682B4 }, { "royalblue", 0x4169E1 },
    { "mediumblue", 0x0000CD }, { "darkblue", 0x00008B }, { "midnightblue", 0x191970 },
    { "cadetblue", 0x5F9EA0 }, { "powderblue", 0xB0E0E6 }, { "lightcyan", 0xE0FFFF },
    { "turquoise", 0x40E0D0 }, { "darkturquoise", 0x00CED1 }, { "aquamarine", 0x7FFFD4 },
    { "lightgreen", 0x90EE90 }, { "palegreen", 0x98FB98 }, { "springgreen", 0x00FF7F },
    { "seagreen", 0x2E8B57 }, { "forestgreen", 0x228B22 }, { "darkgreen", 0x006400 },
    { "limegreen", 0x32CD32 }, { "yellowgreen", 0x9ACD32 }, { "olivedrab", 0x6B8E23 },
    { "darkolivegreen", 0x556B2F }, { "lightyellow", 0xFFFFE0 }, { "lemonchiffon", 0xFFFACD },
    { "lightgoldenrodyellow", 0xFAFAD2 }, { "goldenrod", 0xDAA520 }, { "darkgoldenrod", 0xB8860B },
    { "darkorange", 0xFF8C00 }, { "orangered", 0xFF4500 }, { "lightsalmon", 0xFFA07A },
    { "darksalmon", 0xE9967A }, { "lightcoral", 0xF08080 }, { "indianred", 0xCD5C5C },
    { "firebrick", 0xB22222 }, { "darkred", 0x8B0000 }, { "hotpink", 0xFF69B4 }, { "deeppink", 0xFF1493 },
    { "lightpink", 0xFFB6C1 }, { "palevioletred", 0xDB7093 }, { "mediumvioletred", 0xC71585 },
    { "thistle", 0xD8BFD8 }, { "mediumpurple", 0x9370DB }, { "blueviolet", 0x8A2BE2 },
    { "darkviolet", 0x9400D3 }, { "darkorchid", 0x9932CC }, { "darkmagenta", 0x8B008B },
    { "rebeccapurple", 0x663399 }, { "slateblue", 0x6A5ACD }, { "darkslateblue", 0x483D8B },
    { "mistyrose", 0xFFE4E1 }, { "seashell", 0xFFF5EE }, { "oldlace", 0xFDF5E6 },
    { "floralwhite", 0xFFFAF0 }, { "cornsilk", 0xFFF8DC }, { "papayawhip", 0xFFEFD5 },
    { "blanchedalmond", 0xFFEBCD }, { "bisque", 0xFFE4C4 }, { "moccasin", 0xFFE4B5 },
    { "navajowhite", 0xFFDEAD }, { "peachpuff", 0xFFDAB9 }, { "antiquewhite", 0xFAEBD7 },
    { "burlywood", 0xDEB887 }, { "rosybrown", 0xBC8F8F }, { "saddlebrown", 0x8B4513 },
    { "darkkhaki", 0xBDB76B }, { "palegoldenrod", 0xEEE8AA }, { "lawngreen", 0x7CFC00 },
    { "chartreuse", 0x7FFF00 }, { "greenyellow", 0xADFF2F }, { "mediumseagreen", 0x3CB371 },
    { "mediumspringgreen", 0x00FA9A }, { "mediumaquamarine", 0x66CDAA }, { "lightseagreen", 0x20B2AA },
    { "darkcyan", 0x008B8B }, { "mediumturquoise", 0x48D1CC }, { "paleturquoise", 0xAFEEEE },
    { "lightsteelblue", 0xB0C4DE }, { "mediumslateblue", 0x7B68EE }, { "mediumorchid", 0xBA55D3 },
    { "darkseagreen", 0x8FBC8F }, { "lavenderblush", 0xFFF0F5 },
};

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

// Reads the numbers in "rgb(1, 2, 3 / 50%)"-style arguments; percentages
// are scaled by pct_scale[i].
static int color_args(const char *s, float *out, int max, bool *pct)
{
    int n = 0;

    while (*s && *s != ')' && n < max) {
        char *end;
        float v;

        while (*s && (isspace((unsigned char)*s) || *s == ',' || *s == '/'))
            s++;
        if (!*s || *s == ')')
            break;
        v = strtod(s, &end);
        if (end == s)
            return n;
        pct[n] = *end == '%';
        if (!strncmp(end, "deg", 3))
            end += 3;
        else if (!strncmp(end, "turn", 4))
            v *= 360, end += 4;
        out[n++] = v;
        s = end + (*end == '%');
    }
    return n;
}

static float hue(float p, float q, float t)
{
    if (t < 0)
        t += 1;
    if (t > 1)
        t -= 1;
    if (t < 1.0f / 6)
        return p + (q - p) * 6 * t;
    if (t < 0.5f)
        return q;
    if (t < 2.0f / 3)
        return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

bool css_color(const char *v, color_t *out)
{
    while (isspace((unsigned char)*v))
        v++;
    if (*v == '#') {
        char hex[9];
        size_t n = 0;
        uint32_t x;

        v++;
        while (isxdigit((unsigned char)v[n]) && n < 8)
            n++;
        if (n != 3 && n != 4 && n != 6 && n != 8)
            return false;
        if (n <= 4) {
            for (size_t i = 0; i < n; i++)
                hex[i * 2] = hex[i * 2 + 1] = v[i];
            n *= 2;
        } else {
            memcpy(hex, v, n);
        }
        hex[n] = 0;
        x = strtoul(hex, NULL, 16);
        *out = n == 8 ? ((x >> 8) | (x << 24)) : RGB(x);
        return true;
    }
    if (!strncasecmp(v, "rgb", 3)) {
        const char *p = strchr(v, '(');
        float a[4] = { 0, 0, 0, 1 };
        bool pct[4] = { 0 };
        int n;

        if (!p || (n = color_args(p + 1, a, 4, pct)) < 3)
            return false;
        for (int i = 0; i < 3; i++)
            a[i] = pct[i] ? a[i] * 2.55f : a[i];
        if (n == 4 && pct[3])
            a[3] /= 100;
        *out = ARGB((int)(clamp01(a[3]) * 255 + 0.5f), (int)MIN(MAX(a[0], 0), 255), (int)MIN(MAX(a[1], 0), 255),
                    (int)MIN(MAX(a[2], 0), 255));
        return true;
    }
    if (!strncasecmp(v, "hsl", 3)) {
        const char *p = strchr(v, '(');
        float a[4] = { 0, 0, 0, 1 }, h, s, l, q, pp;
        bool pct[4] = { 0 };
        int n;

        if (!p || (n = color_args(p + 1, a, 4, pct)) < 3)
            return false;
        if (n == 4 && pct[3])
            a[3] /= 100;
        h = fmodf(a[0], 360) / 360;
        if (h < 0)
            h += 1;
        s = clamp01(a[1] / 100);
        l = clamp01(a[2] / 100);
        q = l < 0.5f ? l * (1 + s) : l + s - l * s;
        pp = 2 * l - q;
        *out = ARGB((int)(clamp01(a[3]) * 255 + 0.5f), (int)(hue(pp, q, h + 1.0f / 3) * 255 + 0.5f),
                    (int)(hue(pp, q, h) * 255 + 0.5f), (int)(hue(pp, q, h - 1.0f / 3) * 255 + 0.5f));
        return true;
    }
    if (!strncasecmp(v, "transparent", 11)) {
        *out = 0;
        return true;
    }
    for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++) {
        size_t n = strlen(named_colors[i].name);

        if (!strncasecmp(v, named_colors[i].name, n) && !isalnum((unsigned char)v[n])) {
            *out = RGB(named_colors[i].rgb);
            return true;
        }
    }
    return false;
}

// Parses a length; em is relative to `em`. False if it is not one.
static bool parse_len(const char *v, float em, struct len *out)
{
    char *end;
    float n;

    while (isspace((unsigned char)*v))
        v++;
    if (!strncasecmp(v, "auto", 4)) {
        *out = (struct len){ 0, LEN_AUTO };
        return true;
    }
    if (!strncasecmp(v, "calc(", 5) || !strncasecmp(v, "min(", 4) || !strncasecmp(v, "max(", 4)
        || !strncasecmp(v, "clamp(", 6)) {
        // calc(A op B) with lengths or percentages; min/max/clamp take the
        // first argument that is a plain length.
        const char *p = strchr(v, '(') + 1;
        struct len a, b;
        char op = 0;

        if (tolower(*v) == 'c' && tolower(v[1]) == 'a') {
            if (!parse_len(p, em, &a))
                return false;
            while (*p && *p != ' ' && *p != ')')
                p++;
            while (*p == ' ')
                p++;
            if (*p == '+' || *p == '-' || *p == '*' || *p == '/') {
                op = *p++;
                if (op == '*' || op == '/') {
                    float k = strtod(p, NULL);

                    if (op == '/' && !k)
                        return false;
                    a.v = op == '*' ? a.v * k : a.v / k;
                    *out = a;
                    return true;
                }
                if (!parse_len(p, em, &b))
                    return false;
                if (a.unit == b.unit) {
                    a.v += op == '+' ? b.v : -b.v;
                } else if (a.unit == LEN_PCT && b.unit == LEN_PX) {
                    // Mixed: keep the percentage (close enough on wide pages).
                } else if (b.unit == LEN_PCT) {
                    a = b;
                }
            }
            *out = a;
            return true;
        }
        if (!strncasecmp(v, "clamp(", 6)) {
            // clamp(MIN, VAL, MAX): use VAL if it is simple, else MAX.
            const char *c1 = strchr(p, ',');

            if (c1 && parse_len(c1 + 1, em, out))
                return true;
        }
        return parse_len(p, em, out);
    }
    n = strtod(v, &end);
    if (end == v)
        return false;
    if (*end == '%') {
        *out = (struct len){ n, LEN_PCT };
        return true;
    }
    if (!strncasecmp(end, "px", 2) || !*end || isspace((unsigned char)*end) || *end == ')' || *end == ',')
        *out = (struct len){ n, LEN_PX };
    else if (!strncasecmp(end, "rem", 3))
        *out = (struct len){ n * 16, LEN_PX };
    else if (!strncasecmp(end, "em", 2))
        *out = (struct len){ n * em, LEN_PX };
    else if (!strncasecmp(end, "ex", 2) || !strncasecmp(end, "ch", 2))
        *out = (struct len){ n * em * 0.5f, LEN_PX };
    else if (!strncasecmp(end, "pt", 2))
        *out = (struct len){ n * 4 / 3, LEN_PX };
    else if (!strncasecmp(end, "pc", 2))
        *out = (struct len){ n * 16, LEN_PX };
    else if (!strncasecmp(end, "in", 2))
        *out = (struct len){ n * 96, LEN_PX };
    else if (!strncasecmp(end, "cm", 2))
        *out = (struct len){ n * 96 / 2.54f, LEN_PX };
    else if (!strncasecmp(end, "mm", 2))
        *out = (struct len){ n * 96 / 25.4f, LEN_PX };
    else if (!strncasecmp(end, "vw", 2))
        *out = (struct len){ n * css_viewport_width / 100, LEN_PX };
    else if (!strncasecmp(end, "vh", 2))
        *out = (struct len){ n * css_viewport_height / 100, LEN_PX };
    else if (!strncasecmp(end, "vmin", 4))
        *out = (struct len){ n * MIN(css_viewport_width, css_viewport_height) / 100, LEN_PX };
    else if (!strncasecmp(end, "vmax", 4))
        *out = (struct len){ n * MAX(css_viewport_width, css_viewport_height) / 100, LEN_PX };
    else if (!strncasecmp(end, "fr", 2))
        *out = (struct len){ 0, LEN_AUTO };
    else
        return false;
    return true;
}

static int len_px(struct len l)
{
    return l.unit == LEN_PX ? (int)lroundf(l.v) : 0;
}

// Splits a value into words, keeping parentheses together. Returns the count.
static int words(const char *v, char out[][96], int max)
{
    int n = 0;

    while (*v && n < max) {
        size_t len = 0;
        int depth = 0;
        char quote = 0;

        while (isspace((unsigned char)*v) || (*v == ',' && !n))
            v++;
        if (!*v)
            break;
        while (*v && (depth || quote || (!isspace((unsigned char)*v) && *v != ','))) {
            if (quote) {
                if (*v == quote)
                    quote = 0;
            } else if (*v == '"' || *v == '\'') {
                quote = *v;
            } else if (*v == '(') {
                depth++;
            } else if (*v == ')') {
                depth--;
            }
            if (len + 1 < 96)
                out[n][len++] = *v;
            v++;
        }
        out[n][len] = 0;
        if (*v == ',') {
            // Keep commas as their own word (font lists, backgrounds).
            n++;
            if (n < max)
                strcpy(out[n], ",");
            v++;
        }
        n++;
    }
    return MIN(n, max);
}

// ---- Applying declarations ----

static const char *const display_names[] = {
    [D_NONE] = "none", [D_INLINE] = "inline", [D_BLOCK] = "block", [D_INLINE_BLOCK] = "inline-block",
    [D_LIST_ITEM] = "list-item", [D_TABLE] = "table", [D_TABLE_ROW_GROUP] = "table-row-group",
    [D_TABLE_ROW] = "table-row", [D_TABLE_CELL] = "table-cell", [D_FLEX] = "flex",
    [D_INLINE_FLEX] = "inline-flex", [D_TABLE_CAPTION] = "table-caption",
};

static int keyword(const char *v, const char *const *names, int n, int fallback)
{
    for (int i = 0; i < n; i++)
        if (names[i] && !strcasecmp(v, names[i]))
            return i;
    return fallback;
}

static int border_style(const char *w)
{
    static const char *const names[] = { "none", "solid", "dashed", "dotted", "double", "inset", "outset" };

    if (!strcasecmp(w, "hidden"))
        return BS_NONE;
    if (!strcasecmp(w, "groove") || !strcasecmp(w, "ridge"))
        return BS_SOLID;
    return keyword(w, names, 7, -1);
}

static bool border_width(const char *w, float em, int *out)
{
    struct len l;

    if (!strcasecmp(w, "thin"))
        *out = 1;
    else if (!strcasecmp(w, "medium"))
        *out = 3;
    else if (!strcasecmp(w, "thick"))
        *out = 5;
    else if (parse_len(w, em, &l) && l.unit == LEN_PX)
        *out = l.v > 0 && l.v < 1 ? 1 : (int)lroundf(l.v);
    else
        return false;
    return true;
}

// "1px solid #ccc" onto the given sides.
static void apply_border(struct style *s, const char *v, int first, int last)
{
    char w[6][96];
    int n = words(v, w, 6), width = 3, bs = BS_NONE;
    color_t c = CURRENT_COLOR;
    bool has_style = false;

    for (int i = 0; i < n; i++) {
        int x;
        color_t col;

        if (border_width(w[i], s->font_size, &x))
            width = x;
        else if ((x = border_style(w[i])) >= 0) {
            bs = x;
            has_style = true;
        } else if (css_color(w[i], &col))
            c = col;
    }
    if (!has_style && n == 1 && !strcasecmp(w[0], "0"))
        bs = BS_NONE;
    for (int i = first; i <= last; i++) {
        s->border[i] = width;
        s->border_style[i] = bs;
        s->border_color[i] = c;
    }
}

// Applies "a b c d" in top, right, bottom, left order.
static void four(const char *v, float em, struct len out[4])
{
    char w[4][96];
    int n = words(v, w, 4);
    struct len l[4];

    for (int i = 0; i < n; i++)
        if (!parse_len(w[i], em, &l[i]))
            return;
    if (n == 1)
        l[1] = l[2] = l[3] = l[0];
    else if (n == 2) {
        l[2] = l[0];
        l[3] = l[1];
    } else if (n == 3)
        l[3] = l[1];
    if (n)
        memcpy(out, l, sizeof(l));
}

static void font_family(struct style *s, const char *v)
{
    char w[12][96];
    int n = words(v, w, 12);

    for (int i = 0; i < n; i++) {
        char *f = w[i];
        size_t len;

        if (*f == '"' || *f == '\'') {
            f++;
            len = strlen(f);
            if (len)
                f[len - 1] = 0;
        }
        for (char *t = f; *t; t++)
            *t = tolower((unsigned char)*t);
        if (strstr(f, "mono") || strstr(f, "courier") || strstr(f, "consol") || strstr(f, "menlo")
            || strstr(f, "code")) {
            s->family = FAM_MONO;
            return;
        }
        if (!strcmp(f, "serif") || strstr(f, "times") || strstr(f, "georgia") || strstr(f, "garamond")
            || strstr(f, "palatino") || strstr(f, "cambria") || strstr(f, "book")) {
            s->family = FAM_SERIF;
            return;
        }
        if (strstr(f, "sans") || strstr(f, "arial") || strstr(f, "helvetica") || strstr(f, "verdana")
            || strstr(f, "system") || strstr(f, "tahoma") || strstr(f, "segoe") || strstr(f, "roboto")
            || strstr(f, "inter") || strstr(f, "ui-") || strstr(f, "apple") || strstr(f, "ubuntu")) {
            s->family = FAM_SANS;
            return;
        }
    }
}

static void font_size(struct style *s, const struct style *parent, const char *v)
{
    static const struct {
        const char *name;
        int px;
    } sizes[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
        { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
    };
    int pf = parent ? parent->font_size : 16;
    struct len l;

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (!strcasecmp(v, sizes[i].name)) {
            s->font_size = sizes[i].px;
            return;
        }
    if (!strcasecmp(v, "smaller"))
        s->font_size = MAX(pf * 5 / 6, 8);
    else if (!strcasecmp(v, "larger"))
        s->font_size = pf * 6 / 5;
    else if (parse_len(v, pf, &l) && l.unit != LEN_AUTO)
        s->font_size = MAX(1, (int)lroundf(l.unit == LEN_PCT ? pf * l.v / 100 : l.v));
}

// The size inside the font shorthand: "bold 14px/1.5 serif".
static void font_shorthand_size(struct style *s, const struct style *parent, const char *v)
{
    char fw[12][96];
    int fn = words(v, fw, 12);

    for (int k = 0; k < fn; k++)
        if (isdigit((unsigned char)fw[k][0]) || fw[k][0] == '.') {
            char *slash = strchr(fw[k], '/');

            if (slash)
                *slash = 0;
            if (atoi(fw[k]) >= 100 && !strpbrk(fw[k], "pxemrt%"))
                continue;       // a weight like 700
            font_size(s, parent, fw[k]);
            return;
        }
}

static void line_height(struct style *s, const char *v)
{
    char *end;
    float n = strtod(v, &end);
    struct len l;

    if (!strcasecmp(v, "normal")) {
        s->lh_factor = 1.2f;
        s->line_height = s->font_size * 1.2f;
    } else if (end != v && (!*end || isspace((unsigned char)*end))) {
        s->lh_factor = n;
        s->line_height = s->font_size * n;
    } else if (parse_len(v, s->font_size, &l) && l.unit != LEN_AUTO) {
        s->lh_factor = 0;
        s->line_height = l.unit == LEN_PCT ? s->font_size * l.v / 100 : l.v;
    }
}

static void background(struct style *s, const char *v)
{
    char w[10][96];
    int n = words(v, w, 10), pos = 0;
    color_t c;

    for (int i = 0; i < n; i++) {
        if (!strcmp(w[i], ","))
            continue;
        if (!strncasecmp(w[i], "url(", 4)) {
            char *u = w[i] + 4, *e;

            while (*u == '"' || *u == '\'' || *u == ' ')
                u++;
            e = u + strlen(u);
            while (e > u && (e[-1] == ')' || e[-1] == '"' || e[-1] == '\'' || e[-1] == ' '))
                e--;
            free(s->background_url);
            s->background_url = strndup(u, e - u);
        } else if (!strcasecmp(w[i], "none")) {
            free(s->background_url);
            s->background_url = NULL;
        } else if (!strncasecmp(w[i], "linear-gradient", 15) || !strncasecmp(w[i], "radial-gradient", 15)) {
            // Approximated by its first colour.
            char inner[96], *p;

            strlcpy(inner, strchr(w[i], '(') + 1, sizeof(inner));
            for (p = inner; *p; p++) {
                if (css_color(p, &c) && (p == inner || p[-1] == ' ' || p[-1] == ',' || p[-1] == '(')) {
                    s->background = c;
                    break;
                }
            }
        } else if (!strcasecmp(w[i], "no-repeat")) {
            s->bg_repeat = BG_NO_REPEAT;
        } else if (!strcasecmp(w[i], "repeat-x")) {
            s->bg_repeat = BG_REPEAT_X;
        } else if (!strcasecmp(w[i], "repeat-y")) {
            s->bg_repeat = BG_REPEAT_Y;
        } else if (!strcasecmp(w[i], "repeat")) {
            s->bg_repeat = BG_REPEAT;
        } else if (!strcasecmp(w[i], "cover")) {
            s->bg_size = BG_SIZE_COVER;
        } else if (!strcasecmp(w[i], "contain")) {
            s->bg_size = BG_SIZE_CONTAIN;
        } else if (css_color(w[i], &c)) {
            s->background = c;
        } else {
            struct len l;

            if (!strcasecmp(w[i], "center"))
                l = (struct len){ 50, LEN_PCT };
            else if (!strcasecmp(w[i], "left") || !strcasecmp(w[i], "top"))
                l = (struct len){ 0, LEN_PCT };
            else if (!strcasecmp(w[i], "right") || !strcasecmp(w[i], "bottom"))
                l = (struct len){ 100, LEN_PCT };
            else if (!parse_len(w[i], s->font_size, &l))
                continue;
            if (!strcasecmp(w[i], "top") || !strcasecmp(w[i], "bottom"))
                s->bg_y = l;
            else if (pos++ == 0)
                s->bg_x = s->bg_y = l;
            else
                s->bg_y = l;
        }
    }
}

static const char *var_lookup(const struct style *s, const char *name, size_t len)
{
    for (struct cssvar *v = s->vars; v; v = v->next)
        if (strlen(v->name) == len && !strncmp(v->name, name, len))
            return v->value;
    return NULL;
}

// Replaces var(--x, fallback) in a value (malloc'd, or NULL if unchanged).
static char *substitute_vars(const struct style *s, const char *v, int depth)
{
    const char *p = strstr(v, "var(");
    struct buf {
        char *s;
        size_t len, cap;
    } b = { 0 };
    char *result;

    if (!p || depth > 8)
        return NULL;
    b.cap = strlen(v) + 64;
    if (!(b.s = malloc(b.cap)))
        return NULL;
#define PUT(src, n)                                                                                    \
    do {                                                                                               \
        size_t n_ = (n);                                                                               \
        if (b.len + n_ + 1 > b.cap) {                                                                  \
            char *m_ = realloc(b.s, b.cap = (b.len + n_ + 1) * 2);                                     \
            if (!m_) {                                                                                 \
                free(b.s);                                                                             \
                return NULL;                                                                           \
            }                                                                                          \
            b.s = m_;                                                                                  \
        }                                                                                              \
        memcpy(b.s + b.len, (src), n_);                                                                \
        b.len += n_;                                                                                   \
    } while (0)
    while ((p = strstr(v, "var("))) {
        const char *name = p + 4, *q, *close, *fallback = NULL;
        int depth2 = 1;
        const char *value;

        PUT(v, p - v);
        while (*name == ' ')
            name++;
        q = name;
        while (*q && *q != ',' && *q != ')' && *q != ' ')
            q++;
        close = q;
        while (*close && depth2) {
            if (*close == '(')
                depth2++;
            else if (*close == ')' && --depth2 == 0)
                break;
            else if (*close == ',' && depth2 == 1 && !fallback)
                fallback = close + 1;
            close++;
        }
        value = var_lookup(s, name, q - name);
        if (value) {
            PUT(value, strlen(value));
        } else if (fallback) {
            const char *f = fallback;

            while (*f == ' ')
                f++;
            PUT(f, close - f);
        }
        v = *close ? close + 1 : close;
    }
    PUT(v, strlen(v) + 1);
#undef PUT
    result = b.s;
    if (strstr(result, "var(")) {
        char *again = substitute_vars(s, result, depth + 1);

        if (again) {
            free(result);
            result = again;
        }
    }
    return result;
}

static void apply(struct style *s, const struct style *parent, const char *prop, const char *v)
{
    struct len l;
    color_t c;
    char w[4][96];
    int n;
    bool inherit = !strcasecmp(v, "inherit");

    if (inherit && parent) {
        // Only the common cases.
        if (!strcmp(prop, "color"))
            s->color = parent->color;
        else if (!strcmp(prop, "background-color") || !strcmp(prop, "background"))
            s->background = parent->background;
        else if (!strcmp(prop, "font-size"))
            s->font_size = parent->font_size;
        else if (!strcmp(prop, "text-decoration"))
            s->decoration = parent->decoration;
        return;
    }
    if (!strcasecmp(v, "initial") || !strcasecmp(v, "unset") || !strcasecmp(v, "revert"))
        return;
    switch (prop[0]) {
    case 'a':
        if (!strcmp(prop, "align-self")) {
            static const char *const names[] = { "stretch", "flex-start", "center", "flex-end", "baseline" };
            int k = keyword(v, names, 5, !strcasecmp(v, "start") ? AI_START : !strcasecmp(v, "end") ? AI_END : -1);

            s->align_self = k + 1;
        } else if (!strcmp(prop, "align-items")) {
            static const char *const names[] = { "stretch", "flex-start", "center", "flex-end", "baseline" };

            s->align_items = keyword(v, names, 5, !strcasecmp(v, "start") ? AI_START
                                                  : !strcasecmp(v, "end") ? AI_END : AI_STRETCH);
        }
        break;
    case 'b':
        if (!strcmp(prop, "background-color")) {
            if (css_color(v, &c))
                s->background = c;
        } else if (!strcmp(prop, "background") || !strcmp(prop, "background-image")) {
            if (!strcmp(prop, "background")) {
                s->background = 0;
                s->bg_repeat = BG_REPEAT;
            }
            background(s, v);
        } else if (!strcmp(prop, "background-repeat")) {
            background(s, v);
        } else if (!strcmp(prop, "background-size")) {
            if (!strcasecmp(v, "cover"))
                s->bg_size = BG_SIZE_COVER;
            else if (!strcasecmp(v, "contain"))
                s->bg_size = BG_SIZE_CONTAIN;
        } else if (!strcmp(prop, "background-position")) {
            background(s, v);
        } else if (!strcmp(prop, "border")) {
            apply_border(s, v, 0, 3);
        } else if (!strncmp(prop, "border-", 7)) {
            static const char *const sides[] = { "top", "right", "bottom", "left" };
            const char *rest = prop + 7;
            int side = -1;

            for (int i = 0; i < 4; i++) {
                size_t sl = strlen(sides[i]);

                if (!strncmp(rest, sides[i], sl) && (!rest[sl] || rest[sl] == '-')) {
                    side = i;
                    rest += sl;
                    break;
                }
            }
            if (!strncmp(rest, "inline", 6)) {
                // border-inline(-start/-end): left and right.
                rest += 6;
                if (!strncmp(rest, "-start", 6))
                    side = 3, rest += 6;
                else if (!strncmp(rest, "-end", 4))
                    side = 1, rest += 4;
                else
                    side = 5;
            } else if (!strncmp(rest, "block", 5)) {
                rest += 5;
                if (!strncmp(rest, "-start", 6))
                    side = 0, rest += 6;
                else if (!strncmp(rest, "-end", 4))
                    side = 2, rest += 4;
                else
                    side = 4;
            }
            if (side >= 0 && !*rest) {
                if (side == 4) {
                    apply_border(s, v, 0, 0);
                    apply_border(s, v, 2, 2);
                } else if (side == 5) {
                    apply_border(s, v, 1, 1);
                    apply_border(s, v, 3, 3);
                } else {
                    apply_border(s, v, side, side);
                }
            } else if (!strcmp(rest, "-width") || !strcmp(rest, "-style") || !strcmp(rest, "-color")) {
                int first = side >= 0 && side < 4 ? side : 0, last = side >= 0 && side < 4 ? side : 3;
                int vals[4], k = 0;
                color_t cols[4];
                int styles[4];

                n = words(v, w, 4);
                for (int i = 0; i < n; i++) {
                    if (rest[1] == 'w' && border_width(w[i], s->font_size, &vals[k]))
                        k++;
                    else if (rest[1] == 's' && (styles[k] = border_style(w[i])) >= 0)
                        k++;
                    else if (rest[1] == 'c' && css_color(w[i], &cols[k]))
                        k++;
                }
                for (int i = first; i <= last && k; i++) {
                    int j = side >= 0 && side < 4 ? 0
                            : k == 1 ? 0 : k == 2 ? i % 2 : k == 3 ? (i == 3 ? 1 : i) : i;

                    if (rest[1] == 'w')
                        s->border[i] = vals[j];
                    else if (rest[1] == 's')
                        s->border_style[i] = styles[j];
                    else
                        s->border_color[i] = cols[j];
                }
            } else if (!strcmp(prop, "border-radius")) {
                if (parse_len(v, s->font_size, &l))
                    s->radius = l.unit == LEN_PCT ? (l.v >= 50 ? 9999 : (int)l.v) : len_px(l);
            }
        } else if (!strcmp(prop, "border-spacing")) {
            if (parse_len(v, s->font_size, &l))
                s->border_spacing = len_px(l);
        } else if (!strcmp(prop, "border-collapse")) {
            s->border_collapse = !strcasecmp(v, "collapse");
        } else if (!strcmp(prop, "box-sizing")) {
            s->box_sizing_border = !strcasecmp(v, "border-box");
        }
        break;
    case 'c':
        if (!strcmp(prop, "color")) {
            if (css_color(v, &c))
                s->color = c;
        } else if (!strcmp(prop, "clear")) {
            s->clear = !strcasecmp(v, "left") ? 1 : !strcasecmp(v, "right") ? 2 : !strcasecmp(v, "both") ? 3 : 0;
        } else if (!strcmp(prop, "column-gap")) {
            if (parse_len(v, s->font_size, &l))
                s->gap = l;
        }
        break;
    case 'd':
        if (!strcmp(prop, "display")) {
            int d = keyword(v, display_names, sizeof(display_names) / sizeof(display_names[0]), -1);

            if (d >= 0)
                s->display = d;
            else if (!strcasecmp(v, "table-header-group") || !strcasecmp(v, "table-footer-group"))
                s->display = D_TABLE_ROW_GROUP;
            else if (!strcasecmp(v, "grid") || !strcasecmp(v, "flow-root") || !strcasecmp(v, "inline-table")
                     || !strcasecmp(v, "table-column-group") || !strncasecmp(v, "block ", 6))
                s->display = !strcasecmp(v, "inline-table") ? D_INLINE_BLOCK : D_BLOCK;
            else if (!strcasecmp(v, "inline-grid"))
                s->display = D_INLINE_BLOCK;
            else if (!strcasecmp(v, "contents"))
                s->display = D_INLINE;
        }
        break;
    case 'f':
        if (!strcmp(prop, "font-size")) {
            // Done first, in its own pass.
        } else if (!strcmp(prop, "font-weight")) {
            if (isdigit((unsigned char)*v))
                s->bold = atoi(v) >= 600;
            else
                s->bold = !strcasecmp(v, "bold") || !strcasecmp(v, "bolder");
        } else if (!strcmp(prop, "font-style")) {
            s->italic = !strcasecmp(v, "italic") || !strcasecmp(v, "oblique");
        } else if (!strcmp(prop, "font-family")) {
            font_family(s, v);
        } else if (!strcmp(prop, "font")) {
            // [style] [weight] size[/line-height] family
            char fw[12][96];
            int fn = words(v, fw, 12);

            s->bold = s->italic = false;
            for (int i = 0; i < fn; i++) {
                if (!strcasecmp(fw[i], "italic"))
                    s->italic = true;
                else if (!strcasecmp(fw[i], "bold") || atoi(fw[i]) >= 600)
                    s->bold = true;
                else if ((isdigit((unsigned char)fw[i][0]) || fw[i][0] == '.' || strchr(fw[i], '/'))
                         && !(atoi(fw[i]) >= 100 && !strpbrk(fw[i], "pxemrt%"))) {
                    // The size was taken in the first pass; the rest is the family.
                    char *slash = strchr(fw[i], '/');

                    if (slash)
                        line_height(s, slash + 1);
                    font_family(s, strstr(v, fw[i]) + strlen(fw[i]));
                    break;
                }
            }
        } else if (!strcmp(prop, "float")) {
            s->float_ = !strcasecmp(v, "left") ? FLOAT_LEFT : !strcasecmp(v, "right") ? FLOAT_RIGHT : FLOAT_NONE;
        } else if (!strcmp(prop, "flex-direction")) {
            s->flex_column = !strncasecmp(v, "column", 6);
        } else if (!strcmp(prop, "flex-wrap")) {
            s->flex_wrap = !strcasecmp(v, "wrap") || !strcasecmp(v, "wrap-reverse");
        } else if (!strcmp(prop, "flex-flow")) {
            s->flex_column = strstr(v, "column") != NULL;
            s->flex_wrap = strstr(v, "wrap") && !strstr(v, "nowrap");
        } else if (!strcmp(prop, "flex-grow")) {
            s->flex_grow = strtod(v, NULL);
        } else if (!strcmp(prop, "flex-shrink")) {
            s->flex_shrink = strtod(v, NULL);
        } else if (!strcmp(prop, "flex-basis")) {
            parse_len(v, s->font_size, &s->flex_basis);
        } else if (!strcmp(prop, "flex")) {
            n = words(v, w, 3);
            if (n == 1 && !strcasecmp(w[0], "none")) {
                s->flex_grow = s->flex_shrink = 0;
            } else if (n == 1 && !strcasecmp(w[0], "auto")) {
                s->flex_grow = s->flex_shrink = 1;
            } else if (n >= 1) {
                char *end;
                float g = strtod(w[0], &end);

                if (end != w[0] && !*end) {
                    s->flex_grow = g;
                    s->flex_shrink = n >= 2 ? strtod(w[1], NULL) : 1;
                    if (n == 1)
                        s->flex_basis = (struct len){ 0, LEN_PX };
                    else if (n == 3 || (n == 2 && !isdigit((unsigned char)w[1][0])))
                        parse_len(w[n - 1], s->font_size, &s->flex_basis);
                } else {
                    parse_len(w[0], s->font_size, &s->flex_basis);
                }
            }
        }
        break;
    case 'g':
        if (!strcmp(prop, "gap") || !strcmp(prop, "grid-gap")) {
            if (parse_len(v, s->font_size, &l))
                s->gap = l;
        }
        break;
    case 'h':
        if (!strcmp(prop, "height"))
            parse_len(v, s->font_size, &s->height);
        break;
    case 'j':
        if (!strcmp(prop, "justify-content")) {
            static const char *const names[] = { "flex-start", "center", "flex-end", "space-between",
                                                 "space-around", "space-evenly" };

            s->justify = keyword(v, names, 6, !strcasecmp(v, "end") || !strcasecmp(v, "right") ? JC_END : JC_START);
        }
        break;
    case 'l':
        if (!strcmp(prop, "line-height")) {
            line_height(s, v);
        } else if (!strcmp(prop, "list-style-type") || !strcmp(prop, "list-style")) {
            static const char *const names[] = { "none", "disc", "circle", "square", "decimal", "lower-alpha",
                                                 "upper-alpha", "lower-roman", "upper-roman" };

            n = words(v, w, 3);
            for (int i = 0; i < n; i++) {
                int k = keyword(w[i], names, 9, -1);

                if (k < 0 && !strcasecmp(w[i], "lower-latin"))
                    k = LS_LOWER_ALPHA;
                else if (k < 0 && !strcasecmp(w[i], "upper-latin"))
                    k = LS_UPPER_ALPHA;
                if (k >= 0)
                    s->list_style = k;
            }
        } else if (!strcmp(prop, "left")) {
            parse_len(v, s->font_size, &s->left);
        }
        break;
    case 'm':
        if (!strcmp(prop, "margin")) {
            four(v, s->font_size, s->margin);
        } else if (!strncmp(prop, "margin-", 7)) {
            const char *side = prop + 7;
            int i = !strcmp(side, "top") || !strcmp(side, "block-start") ? 0
                    : !strcmp(side, "right") || !strcmp(side, "inline-end") ? 1
                    : !strcmp(side, "bottom") || !strcmp(side, "block-end") ? 2
                    : !strcmp(side, "left") || !strcmp(side, "inline-start") ? 3 : -1;

            if (i >= 0)
                parse_len(v, s->font_size, &s->margin[i]);
            else if (!strcmp(side, "inline") || !strcmp(side, "block")) {
                struct len m[4] = { s->margin[0], s->margin[1], s->margin[2], s->margin[3] };
                int a = side[0] == 'i' ? 3 : 0, b = side[0] == 'i' ? 1 : 2;

                n = words(v, w, 2);
                if (n >= 1 && parse_len(w[0], s->font_size, &m[a])) {
                    m[b] = m[a];
                    if (n == 2)
                        parse_len(w[1], s->font_size, &m[b]);
                    memcpy(s->margin, m, sizeof(m));
                }
            }
        } else if (!strcmp(prop, "max-width")) {
            if (strcasecmp(v, "none"))
                parse_len(v, s->font_size, &s->max_width);
            else
                s->max_width = (struct len){ 0, LEN_AUTO };
        } else if (!strcmp(prop, "min-width")) {
            parse_len(v, s->font_size, &s->min_width);
        } else if (!strcmp(prop, "min-height")) {
            parse_len(v, s->font_size, &s->min_height);
        } else if (!strcmp(prop, "max-height")) {
            if (strcasecmp(v, "none"))
                parse_len(v, s->font_size, &s->max_height);
        }
        break;
    case 'o':
        if (!strcmp(prop, "overflow") || !strcmp(prop, "overflow-y") || !strcmp(prop, "overflow-x"))
            s->overflow_hidden = !strcasecmp(v, "hidden") || !strcasecmp(v, "clip");
        else if (!strcmp(prop, "order"))
            s->order = atoi(v);
        else if (!strcmp(prop, "opacity") && strtod(v, NULL) < 0.05)
            s->visibility_hidden = true;
        break;
    case 'p':
        if (!strcmp(prop, "padding")) {
            four(v, s->font_size, s->padding);
        } else if (!strncmp(prop, "padding-", 8)) {
            const char *side = prop + 8;
            int i = !strcmp(side, "top") || !strcmp(side, "block-start") ? 0
                    : !strcmp(side, "right") || !strcmp(side, "inline-end") ? 1
                    : !strcmp(side, "bottom") || !strcmp(side, "block-end") ? 2
                    : !strcmp(side, "left") || !strcmp(side, "inline-start") ? 3 : -1;

            if (i >= 0)
                parse_len(v, s->font_size, &s->padding[i]);
            else if (!strcmp(side, "inline") || !strcmp(side, "block")) {
                int a = side[0] == 'i' ? 3 : 0, b = side[0] == 'i' ? 1 : 2;

                n = words(v, w, 2);
                if (n >= 1 && parse_len(w[0], s->font_size, &s->padding[a])) {
                    s->padding[b] = s->padding[a];
                    if (n == 2)
                        parse_len(w[1], s->font_size, &s->padding[b]);
                }
            }
        } else if (!strcmp(prop, "position")) {
            static const char *const names[] = { "static", "relative", "absolute", "fixed", "sticky" };

            s->position = keyword(v, names, 5, POS_STATIC);
        }
        break;
    case 'r':
        if (!strcmp(prop, "row-gap") && parse_len(v, s->font_size, &l))
            s->gap = l;
        break;
    case 't':
        if (!strcmp(prop, "text-align")) {
            static const char *const names[] = { "left", "center", "right", "justify" };

            s->text_align = keyword(v, names, 4, !strcasecmp(v, "end") ? ALIGN_RIGHT
                                                 : !strcmp(v, "-webkit-center") ? ALIGN_CENTER : ALIGN_LEFT);
        } else if (!strcmp(prop, "text-decoration") || !strcmp(prop, "text-decoration-line")) {
            s->decoration = 0;
            if (strstr(v, "underline"))
                s->decoration |= DEC_UNDERLINE;
            if (strstr(v, "line-through"))
                s->decoration |= DEC_LINE_THROUGH;
            if (strstr(v, "overline"))
                s->decoration |= DEC_OVERLINE;
        } else if (!strcmp(prop, "text-transform")) {
            static const char *const names[] = { "none", "uppercase", "lowercase", "capitalize" };

            s->transform = keyword(v, names, 4, TT_NONE);
        } else if (!strcmp(prop, "text-indent")) {
            parse_len(v, s->font_size, &s->text_indent);
        } else if (!strcmp(prop, "top")) {
            parse_len(v, s->font_size, &s->top);
        }
        break;
    case 'v':
        if (!strcmp(prop, "visibility")) {
            s->visibility_hidden = !strcasecmp(v, "hidden") || !strcasecmp(v, "collapse");
        } else if (!strcmp(prop, "vertical-align")) {
            static const char *const names[] = { "baseline", "middle", "top", "bottom", "super", "sub" };

            s->vertical_align = keyword(v, names, 6, !strcasecmp(v, "text-top") ? VA_TOP
                                                     : !strcasecmp(v, "text-bottom") ? VA_BOTTOM : VA_BASELINE);
        }
        break;
    case 'w':
        if (!strcmp(prop, "width")) {
            parse_len(v, s->font_size, &s->width);
        } else if (!strcmp(prop, "white-space")) {
            static const char *const names[] = { "normal", "pre", "nowrap", "pre-wrap" };

            s->white_space = keyword(v, names, 4, !strcasecmp(v, "pre-line") ? WS_PRE_WRAP : WS_NORMAL);
        }
        break;
    }
}

// ---- The cascade ----

struct match {
    const struct block *b;
    int spec, order, origin;
};

static int by_priority(const void *a, const void *b)
{
    const struct match *x = a, *y = b;

    if (x->origin != y->origin)
        return x->origin - y->origin;
    if (x->spec != y->spec)
        return x->spec - y->spec;
    return x->order - y->order;
}

static void initial_style(struct style *s, const struct style *parent)
{
    memset(s, 0, sizeof(*s));
    // Lengths start at zero, except the sizes, which start as auto.
    for (int i = 0; i < 4; i++)
        s->margin[i] = s->padding[i] = (struct len){ 0, LEN_PX };
    s->text_indent = s->gap = s->top = s->left = (struct len){ 0, LEN_PX };
    s->display = D_INLINE;
    s->flex_shrink = 1;
    s->bg_x = s->bg_y = (struct len){ 0, LEN_PCT };
    if (parent) {
        s->color = parent->color;
        s->font_size = parent->font_size;
        s->bold = parent->bold;
        s->italic = parent->italic;
        s->family = parent->family;
        s->text_align = parent->text_align;
        s->white_space = parent->white_space;
        s->lh_factor = parent->lh_factor;
        s->line_height = parent->line_height;
        s->list_style = parent->list_style;
        s->transform = parent->transform;
        s->visibility_hidden = parent->visibility_hidden;
        s->decoration = parent->decoration;
        s->border_spacing = parent->border_spacing;
        s->border_collapse = parent->border_collapse;
        s->vars = parent->vars;
    } else {
        s->color = RGB(0x000000);
        s->font_size = 16;
        s->lh_factor = 1.2f;
        s->family = FAM_SANS;
        s->list_style = LS_DISC;
    }
    for (int i = 0; i < 4; i++)
        s->border_color[i] = CURRENT_COLOR;
}

void style_free(struct style *s)
{
    struct cssvar *v, *next;
    int i = 0;

    if (!s)
        return;
    for (v = s->vars; v && i < s->nvars; v = next, i++) {
        next = v->next;
        free(v->name);
        free(v->value);
        free(v);
    }
    free(s->background_url);
    free(s);
}

// Presentational HTML attributes, as CSS declarations.
static void presentational(struct node *n, char *css, size_t size)
{
    const char *v;

    css[0] = 0;
    if ((v = node_attr(n, "bgcolor")))
        snprintf(css + strlen(css), size - strlen(css), "background-color:%s%s;",
                 v[0] != '#' && strlen(v) == 6 && isxdigit((unsigned char)v[0]) ? "#" : "", v);
    if ((v = node_attr(n, "color")) && node_is(n, "font"))
        snprintf(css + strlen(css), size - strlen(css), "color:%s;", v);
    if ((v = node_attr(n, "text")) && node_is(n, "body"))
        snprintf(css + strlen(css), size - strlen(css), "color:%s;", v);
    if ((v = node_attr(n, "face")) && node_is(n, "font"))
        snprintf(css + strlen(css), size - strlen(css), "font-family:%s;", v);
    if ((v = node_attr(n, "size")) && node_is(n, "font")) {
        static const char *const sizes[] = { "x-small", "x-small", "small", "medium", "large", "x-large",
                                             "xx-large", "xxx-large" };
        int k = atoi(v);

        if (*v == '+' || *v == '-')
            k = 3 + k;
        k = MIN(MAX(k, 1), 7);
        snprintf(css + strlen(css), size - strlen(css), "font-size:%s;", sizes[k]);
    }
    if ((v = node_attr(n, "width")) && !node_is(n, "img") && !node_is(n, "input") && !node_is(n, "canvas"))
        snprintf(css + strlen(css), size - strlen(css), "width:%s%s;", v, strchr(v, '%') ? "" : "px");
    if ((v = node_attr(n, "height")) && (node_is(n, "td") || node_is(n, "th") || node_is(n, "tr")
                                         || node_is(n, "table")))
        snprintf(css + strlen(css), size - strlen(css), "height:%s%s;", v, strchr(v, '%') ? "" : "px");
    if ((v = node_attr(n, "align"))) {
        if (node_is(n, "table") && !strcasecmp(v, "center"))
            snprintf(css + strlen(css), size - strlen(css), "margin-left:auto;margin-right:auto;");
        else if (node_is(n, "img") && (!strcasecmp(v, "left") || !strcasecmp(v, "right")))
            snprintf(css + strlen(css), size - strlen(css), "float:%s;", v);
        else if (!node_is(n, "img") && !node_is(n, "table"))
            snprintf(css + strlen(css), size - strlen(css), "text-align:%s;", v);
    }
    if ((v = node_attr(n, "valign")))
        snprintf(css + strlen(css), size - strlen(css), "vertical-align:%s;", v);
    if (node_is(n, "table") && (v = node_attr(n, "border")) && atoi(v) > 0)
        snprintf(css + strlen(css), size - strlen(css), "border:%dpx outset gray;", atoi(v));
    if ((node_is(n, "td") || node_is(n, "th"))) {
        struct node *t = n->parent;

        while (t && !node_is(t, "table"))
            t = t->parent;
        if (t && (v = node_attr(t, "border")) && (atoi(v) > 0 || !*v))
            snprintf(css + strlen(css), size - strlen(css), "border:1px inset gray;");
        if (t && (v = node_attr(t, "cellpadding")))
            snprintf(css + strlen(css), size - strlen(css), "padding:%dpx;", atoi(v));
        if (node_attr(n, "nowrap"))
            snprintf(css + strlen(css), size - strlen(css), "white-space:nowrap;");
    }
    if (node_is(n, "body")) {
        if ((v = node_attr(n, "link")))
            (void)v;
        if ((v = node_attr(n, "background")))
            snprintf(css + strlen(css), size - strlen(css), "background-image:url(%s);", v);
        if ((v = node_attr(n, "leftmargin")) || (v = node_attr(n, "marginwidth")))
            snprintf(css + strlen(css), size - strlen(css), "margin-left:%dpx;margin-right:%dpx;", atoi(v),
                     atoi(v));
        if ((v = node_attr(n, "topmargin")) || (v = node_attr(n, "marginheight")))
            snprintf(css + strlen(css), size - strlen(css), "margin-top:%dpx;", atoi(v));
    }
    if (node_is(n, "hr") && (v = node_attr(n, "size")))
        snprintf(css + strlen(css), size - strlen(css), "height:%dpx;", atoi(v));
    if (node_attr(n, "hidden"))
        snprintf(css + strlen(css), size - strlen(css), "display:none;");
    if (node_is(n, "input") && (v = node_attr(n, "type")) && !strcasecmp(v, "hidden"))
        snprintf(css + strlen(css), size - strlen(css), "display:none;");
}

struct cascade {
    struct sheet **sheets;
    int nsheets;
    struct match *m;
    int cap;
};

static void style_element(struct cascade *cx, struct node *n, const struct style *parent)
{
    struct style *s = calloc(1, sizeof(*s));
    struct block *inline_block = NULL, *pres_block = NULL;
    const char *style_attr = node_attr(n, "style");
    char pres[512];
    int nm = 0;

    if (!s)
        return;
    style_free(n->style);
    n->style = s;
    initial_style(s, parent);
    presentational(n, pres, sizeof(pres));
    if (*pres)
        pres_block = parse_block(pres, pres + strlen(pres));
    if (style_attr)
        inline_block = parse_block(style_attr, style_attr + strlen(style_attr));
    for (int i = 0; i < cx->nsheets; i++) {
        struct sheet *sh = cx->sheets[i];

        if (!sh)
            continue;
        for (int r = 0; r < sh->n; r++) {
            struct rule *rule = &sh->rules[r];

            if (!match_from(&rule->sel, rule->sel.n - 1, n) || !media_matches(rule->media))
                continue;
            if (nm == cx->cap) {
                int cap = cx->cap ? cx->cap * 2 : 64;
                struct match *m = realloc(cx->m, cap * sizeof(*m));

                if (!m)
                    break;
                cx->m = m;
                cx->cap = cap;
            }
            cx->m[nm++] = (struct match){ rule->block, rule->sel.spec, rule->order, sh->origin * 2 };
        }
    }
    if (nm + 2 > cx->cap) {
        struct match *m = realloc(cx->m, (nm + 2) * sizeof(*m));

        if (m) {
            cx->m = m;
            cx->cap = nm + 2;
        }
    }
    if (pres_block && nm < cx->cap)
        cx->m[nm++] = (struct match){ pres_block, 0, -1, 1 };
    if (inline_block && nm < cx->cap)
        cx->m[nm++] = (struct match){ inline_block, 1 << 30, 0, 2 };
    qsort(cx->m, nm, sizeof(*cx->m), by_priority);
    // Custom properties first (they can be used by anything), then the
    // font size (em units depend on it), then everything; !important last.
    for (int pass = 0; pass < 4; pass++) {
        bool important = pass == 3;

        for (int i = 0; i < nm; i++) {
            const struct block *b = cx->m[i].b;

            for (int d = 0; d < b->n; d++) {
                const struct decl *dc = &b->decls[d];
                bool custom = dc->prop[0] == '-' && dc->prop[1] == '-';
                bool is_size = !strcmp(dc->prop, "font-size"), is_font = !strcmp(dc->prop, "font");
                char *value;
                const char *val;

                if (pass == 0) {
                    struct cssvar *v;

                    if (!custom || !(v = calloc(1, sizeof(*v))))
                        continue;
                    v->name = strdup(dc->prop);
                    v->value = strdup(dc->value);
                    v->next = s->vars;
                    s->vars = v;
                    s->nvars++;
                    continue;
                }
                if (custom || dc->important != important || (pass == 1 && !is_size && !is_font)
                    || (pass == 2 && is_size))
                    continue;
                value = substitute_vars(s, dc->value, 0);
                val = value ? value : dc->value;
                if (is_size) {
                    font_size(s, parent, val);
                } else if (is_font) {
                    if (pass != 2)
                        font_shorthand_size(s, parent, val);
                    if (pass != 1)
                        apply(s, parent, dc->prop, val);
                } else {
                    apply(s, parent, dc->prop, val);
                }
                free(value);
            }
        }
        if (pass == 1 && s->lh_factor)
            s->line_height = s->font_size * s->lh_factor;
    }
    block_free(inline_block);
    block_free(pres_block);
    // Fix-ups.
    if (s->lh_factor)
        s->line_height = s->font_size * s->lh_factor;
    for (int i = 0; i < 4; i++) {
        if (s->border_style[i] == BS_NONE)
            s->border[i] = 0;
        if (s->border_color[i] == CURRENT_COLOR)
            s->border_color[i] = s->color;
    }
    if ((s->float_ || s->position == POS_ABSOLUTE || s->position == POS_FIXED)
        && (s->display == D_INLINE || s->display == D_TABLE_CELL || s->display == D_TABLE_ROW))
        s->display = D_BLOCK;
    if (parent && (parent->display == D_FLEX || parent->display == D_INLINE_FLEX) && s->display == D_INLINE)
        s->display = D_BLOCK;
}

static void style_tree(struct cascade *cx, struct node *n, const struct style *parent)
{
    for (struct node *c = n->first; c; c = c->next) {
        if (c->type != NODE_ELEMENT)
            continue;
        style_element(cx, c, parent);
        if (c->style)
            style_tree(cx, c, c->style);
    }
}

void css_apply(struct node *root, struct sheet **sheets, int nsheets)
{
    struct cascade cx = { sheets, nsheets, NULL, 0 };

    style_tree(&cx, root, NULL);
    free(cx.m);
}

struct font *style_font(const struct style *s)
{
    char name[40];

    snprintf(name, sizeof(name), "%s%s%s", s->family == FAM_MONO ? "mono" : s->family == FAM_SERIF ? "serif" : "sans",
             s->bold ? "-bold" : "", s->italic ? "-italic" : "");
    return font_get(name, s->font_size);
}

// ---- The user agent's style sheet ----

static const char default_css[] =
    "html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr,"
    " legend, listing, main, p, plaintext, pre, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav,"
    " section, dl, dt, dd, ol, ul, menu, dir, fieldset, details, summary, address, frameset, frame, optgroup"
    " { display: block; }"
    "head, script, style, link, meta, title, template, noscript, datalist, param, source, track, area,"
    " base, iframe, object, embed, svg, canvas, video, audio, map, dialog:not([open]), select option,"
    " noembed, noframes, input[type=hidden] { display: none; }"
    "body { margin: 8px; line-height: 1.35; }"
    "p, blockquote, figure, dl, ul, ol, menu, pre, listing, xmp, plaintext, fieldset { margin: 1em 0; }"
    "h1 { font-size: 2em; margin: .67em 0; font-weight: bold; }"
    "h2 { font-size: 1.5em; margin: .83em 0; font-weight: bold; }"
    "h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold; }"
    "h4 { margin: 1.33em 0; font-weight: bold; }"
    "h5 { font-size: .83em; margin: 1.67em 0; font-weight: bold; }"
    "h6 { font-size: .67em; margin: 2.33em 0; font-weight: bold; }"
    "blockquote, figure { margin-left: 40px; margin-right: 40px; }"
    "ul, ol, menu, dir { padding-left: 40px; }"
    "ul ul, ol ul, ul ol, ol ol { margin-top: 0; margin-bottom: 0; }"
    "ul ul, ol ul { list-style-type: circle; }"
    "ul ul ul { list-style-type: square; }"
    "ol { list-style-type: decimal; }"
    "li { display: list-item; }"
    "dd { margin-left: 40px; }"
    "b, strong, th, dt { font-weight: bold; }"
    "i, em, cite, var, dfn, address { font-style: italic; }"
    "pre, code, kbd, samp, tt, listing, xmp, plaintext { font-family: monospace; }"
    "code, kbd, samp, tt { font-size: .9em; }"
    "pre, listing, xmp, plaintext { white-space: pre; font-size: .9em; }"
    "u, ins { text-decoration: underline; }"
    "s, strike, del { text-decoration: line-through; }"
    "big { font-size: larger; } small { font-size: smaller; }"
    "sub { vertical-align: sub; font-size: smaller; } sup { vertical-align: super; font-size: smaller; }"
    "mark { background-color: yellow; color: black; }"
    "a:link { color: #0645ad; text-decoration: underline; }"
    "center { text-align: center; }"
    "hr { margin: .5em auto; border: 1px inset #c0c0c0; }"
    "table { display: table; border-spacing: 2px; }"
    "caption { display: table-caption; text-align: center; }"
    "thead, tbody, tfoot { display: table-row-group; }"
    "tr { display: table-row; }"
    "td, th { display: table-cell; padding: 1px; vertical-align: middle; }"
    "th { text-align: center; }"
    "img { display: inline-block; }"
    "input, select, button, textarea { display: inline-block; font-size: 13px; }"
    "button { padding: 2px 8px; border: 1px solid #8f8f9d; background-color: #e9e9ed; border-radius: 3px; }"
    "fieldset { border: 2px groove #c0c0c0; padding: .35em .75em .6em; margin: 0 2px; }"
    "legend { padding: 0 2px; }"
    "abbr[title] { text-decoration: underline; }"
    "q:before { content: '\"'; }"
    "summary { font-weight: bold; }"
    "details:not([open]) > :not(summary) { display: none; }"
    "nobr { white-space: nowrap; }"
    "[hidden] { display: none; }";

struct sheet *css_default(void)
{
    static struct sheet *ua;

    if (!ua)
        ua = css_parse(default_css, 0);
    return ua;
}
