#include "browser.h"

// A forgiving HTML parser. It builds a tree the way browsers do for the
// common cases: void elements, implied end tags (p, li, td, ...), raw text
// in script and style, character references, and stray end tags ignored.

static const char *const void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source",
    "track", "wbr", "keygen", NULL,
};

// Starting one of these closes an open <p>.
static const char *const closes_p[] = {
    "address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl",
    "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
    "hgroup", "hr", "main", "menu", "nav", "ol", "p", "pre", "section", "summary", "table", "ul", "li",
    "dd", "dt", "listing", "plaintext", "xmp", NULL,
};

static bool in_list(const char *tag, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(tag, list[i]))
            return true;
    return false;
}

const char *node_attr(const struct node *n, const char *name)
{
    if (!n || n->type != NODE_ELEMENT)
        return NULL;
    for (int i = 0; i < n->nattrs; i++)
        if (!strcmp(n->attrs[i].name, name))
            return n->attrs[i].value;
    return NULL;
}

bool node_is(const struct node *n, const char *tag)
{
    return n && n->type == NODE_ELEMENT && !strcmp(n->tag, tag);
}

struct node *node_walk(struct node *n, const struct node *root)
{
    if (n->first)
        return n->first;
    while (n && n != root) {
        if (n->next)
            return n->next;
        n = n->parent;
    }
    return NULL;
}

struct node *node_find(struct node *root, const char *tag)
{
    for (struct node *n = root; n; n = node_walk(n, root))
        if (node_is(n, tag))
            return n;
    return NULL;
}

char *node_text(const struct node *root)
{
    size_t len = 0, cap = 64;
    char *out = malloc(cap);
    bool space = true;

    if (!out)
        return NULL;
    for (const struct node *n = root; n; n = node_walk((struct node *)n, root)) {
        if (n->type != NODE_TEXT)
            continue;
        for (const char *p = n->text; *p; p++) {
            bool ws = isspace((unsigned char)*p);

            if (ws && space)
                continue;
            if (len + 2 >= cap) {
                char *m = realloc(out, cap *= 2);

                if (!m)
                    break;
                out = m;
            }
            out[len++] = ws ? ' ' : *p;
            space = ws;
        }
    }
    while (len && out[len - 1] == ' ')
        len--;
    out[len] = 0;
    return out;
}

void node_free(struct node *n)
{
    struct node *c, *next;

    if (!n)
        return;
    for (c = n->first; c; c = next) {
        next = c->next;
        node_free(c);
    }
    for (int i = 0; i < n->nattrs; i++) {
        free(n->attrs[i].name);
        free(n->attrs[i].value);
    }
    free(n->attrs);
    free(n->tag);
    free(n->text);
    free(n->value);
    style_free(n->style);
    if (n->image)
        surface_destroy(n->image);
    free(n);
}

static struct node *new_node(int type)
{
    struct node *n = calloc(1, sizeof(*n));

    if (n) {
        n->type = type;
        n->selected = -1;
    }
    return n;
}

static void append(struct node *parent, struct node *child)
{
    child->parent = parent;
    child->prev = parent->last;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
}

// ---- Character references ----

static const struct {
    const char *name;
    uint32_t cp;
} entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "middot", 0xB7 }, { "bull", 0x2022 },
    { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "times", 0xD7 },
    { "divide", 0xF7 }, { "deg", 0xB0 }, { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 },
    { "cent", 0xA2 }, { "sect", 0xA7 }, { "para", 0xB6 }, { "plusmn", 0xB1 }, { "frac12", 0xBD },
    { "frac14", 0xBC }, { "frac34", 0xBE }, { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 },
    { "darr", 0x2193 }, { "harr", 0x2194 }, { "rArr", 0x21D2 }, { "lArr", 0x21D0 }, { "hearts", 0x2665 },
    { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "shy", 0xAD }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
    { "thinsp", 0x2009 }, { "zwnj", 0x200C }, { "zwj", 0x200D }, { "prime", 0x2032 }, { "Prime", 0x2033 },
    { "le", 0x2264 }, { "ge", 0x2265 }, { "ne", 0x2260 }, { "minus", 0x2212 }, { "infin", 0x221E },
    { "sum", 0x2211 }, { "check", 0x2713 }, { "star", 0x2606 }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
    { "ecirc", 0xEA }, { "agrave", 0xE0 }, { "aacute", 0xE1 }, { "acirc", 0xE2 }, { "auml", 0xE4 },
    { "ouml", 0xF6 }, { "uuml", 0xFC }, { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC },
    { "szlig", 0xDF }, { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "oacute", 0xF3 }, { "iacute", 0xED },
    { "uacute", 0xFA }, { "Eacute", 0xC9 }, { "aring", 0xE5 }, { "oslash", 0xF8 }, { "aelig", 0xE6 },
    { "alpha", 0x3B1 }, { "beta", 0x3B2 }, { "gamma", 0x3B3 }, { "delta", 0x3B4 }, { "pi", 0x3C0 },
    { "mu", 0x3BC }, { "lambda", 0x3BB }, { "sigma", 0x3C3 }, { "omega", 0x3C9 }, { "Omega", 0x3A9 },
};

// Decodes the reference at *p (just after '&'); returns the code point and
// advances *p, or 0 if it is not one.
static uint32_t entity(const char **p, const char *end)
{
    const char *s = *p;
    uint32_t cp = 0;

    if (s < end && *s == '#') {
        int base = 10, digits = 0;

        s++;
        if (s < end && (*s == 'x' || *s == 'X')) {
            base = 16;
            s++;
        }
        while (s < end && digits < 8) {
            int d = isdigit((unsigned char)*s) ? *s - '0'
                    : base == 16 && isxdigit((unsigned char)*s) ? (tolower(*s) - 'a' + 10) : -1;

            if (d < 0)
                break;
            cp = cp * base + d;
            digits++;
            s++;
        }
        if (!digits)
            return 0;
        if (s < end && *s == ';')
            s++;
        *p = s;
        if (!cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = 0xFFFD;
        // Windows-1252 codes that pages write as numbers.
        if (cp == 0x91 || cp == 0x92)
            cp = cp == 0x91 ? 0x2018 : 0x2019;
        else if (cp == 0x93 || cp == 0x94)
            cp = cp == 0x93 ? 0x201C : 0x201D;
        else if (cp == 0x96 || cp == 0x97)
            cp = cp == 0x96 ? 0x2013 : 0x2014;
        return cp;
    }
    for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
        size_t n = strlen(entities[i].name);

        if ((size_t)(end - s) >= n && !strncmp(s, entities[i].name, n)
            && (s + n == end || !isalnum((unsigned char)s[n]))) {
            s += n;
            if (s < end && *s == ';')
                s++;
            *p = s;
            return entities[i].cp;
        }
    }
    return 0;
}

struct buf {
    char *s;
    size_t len, cap;
};

static void buf_put(struct buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = MAX(b->cap * 2, b->len + n + 64);
        char *m = realloc(b->s, cap);

        if (!m)
            return;
        b->s = m;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

// Copies text, decoding references and turning stray bytes that are not
// UTF-8 into the Latin-1 characters they most likely were.
static char *decode(const char *s, const char *end, bool refs)
{
    struct buf b = { 0 };

    buf_put(&b, "", 0);
    while (s < end) {
        unsigned char c = *s;

        if (c == '&' && refs) {
            const char *p = s + 1;
            uint32_t cp = entity(&p, end);

            if (cp) {
                char u[4];

                buf_put(&b, u, utf8_encode(cp, u));
                s = p;
                continue;
            }
        }
        if (c == '\r') {
            buf_put(&b, "\n", 1);
            s += (s + 1 < end && s[1] == '\n') ? 2 : 1;
            continue;
        }
        if (c >= 0x80) {
            int need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : -1;
            bool ok = need > 0 && s + need < end + 0;

            for (int i = 1; ok && i <= need; i++)
                ok = s + i < end && (s[i] & 0xC0) == 0x80;
            if (ok) {
                buf_put(&b, s, need + 1);
                s += need + 1;
            } else {
                char u[4];

                buf_put(&b, u, utf8_encode(c, u));
                s++;
            }
            continue;
        }
        buf_put(&b, s, 1);
        s++;
    }
    return b.s;
}

// ---- Tree building ----

struct parser {
    struct node *doc, *cur;
};

static bool open_in_scope(struct parser *p, const char *tag, const char *const *stop)
{
    for (struct node *n = p->cur; n && n->type == NODE_ELEMENT; n = n->parent) {
        if (!strcmp(n->tag, tag))
            return true;
        if (stop && in_list(n->tag, stop))
            return false;
    }
    return false;
}

// Closes elements up to and including the nearest `tag`.
static void close_to(struct parser *p, const char *tag)
{
    for (struct node *n = p->cur; n && n->type == NODE_ELEMENT; n = n->parent) {
        if (!strcmp(n->tag, tag)) {
            p->cur = n->parent;
            return;
        }
    }
}

static const char *const list_scope[] = { "ul", "ol", "menu", "table", "div", "body", NULL };
static const char *const table_scope[] = { "table", NULL };
static const char *const block_scope[] = { "div", "table", "body", "td", "th", "li", "button", "blockquote",
                                           "section", "article", "form", NULL };

static void implied_ends(struct parser *p, const char *tag)
{
    if (in_list(tag, closes_p) && open_in_scope(p, "p", block_scope))
        close_to(p, "p");
    if (!strcmp(tag, "li") && open_in_scope(p, "li", list_scope))
        close_to(p, "li");
    if ((!strcmp(tag, "dt") || !strcmp(tag, "dd"))) {
        static const char *const dl_scope[] = { "dl", NULL };

        if (open_in_scope(p, "dd", dl_scope))
            close_to(p, "dd");
        if (open_in_scope(p, "dt", dl_scope))
            close_to(p, "dt");
    }
    if (!strcmp(tag, "tr") || !strcmp(tag, "tbody") || !strcmp(tag, "thead") || !strcmp(tag, "tfoot")) {
        if (open_in_scope(p, "td", table_scope))
            close_to(p, "td");
        if (open_in_scope(p, "th", table_scope))
            close_to(p, "th");
        if (open_in_scope(p, "tr", table_scope))
            close_to(p, "tr");
    }
    if (strcmp(tag, "tr") && (!strcmp(tag, "tbody") || !strcmp(tag, "thead") || !strcmp(tag, "tfoot"))) {
        static const char *const sections[] = { "tbody", "thead", "tfoot", NULL };

        for (int i = 0; sections[i]; i++)
            if (open_in_scope(p, sections[i], table_scope))
                close_to(p, sections[i]);
    }
    if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
        static const char *const row_scope[] = { "tr", "table", NULL };

        if (open_in_scope(p, "td", row_scope))
            close_to(p, "td");
        if (open_in_scope(p, "th", row_scope))
            close_to(p, "th");
    }
    if (!strcmp(tag, "option") || !strcmp(tag, "optgroup")) {
        static const char *const sel_scope[] = { "select", "datalist", NULL };

        if (open_in_scope(p, "option", sel_scope))
            close_to(p, "option");
    }
    // Headings do not nest.
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2] && p->cur->type == NODE_ELEMENT
        && p->cur->tag[0] == 'h' && p->cur->tag[1] >= '1' && p->cur->tag[1] <= '6' && !p->cur->tag[2])
        p->cur = p->cur->parent;
}

static void add_text(struct parser *p, const char *s, const char *end, bool refs)
{
    struct node *n;
    char *text;

    if (s >= end)
        return;
    text = decode(s, end, refs);
    if (!text)
        return;
    // Merge with a text node just before.
    if (p->cur->last && p->cur->last->type == NODE_TEXT) {
        struct node *t = p->cur->last;
        size_t a = strlen(t->text), b = strlen(text);
        char *m = realloc(t->text, a + b + 1);

        if (m) {
            memcpy(m + a, text, b + 1);
            t->text = m;
        }
        free(text);
        return;
    }
    if (!(n = new_node(NODE_TEXT))) {
        free(text);
        return;
    }
    n->type = NODE_TEXT;
    n->text = text;
    append(p->cur, n);
}

static bool name_char(char c)
{
    return c && !isspace((unsigned char)c) && c != '>' && c != '/' && c != '=';
}

// Parses a start tag at s (just after '<'); returns where it ends.
static const char *start_tag(struct parser *p, const char *s, const char *end, const char **raw_end_tag)
{
    const char *q = s;
    struct node *n;
    char tag[32];
    size_t tl = 0;
    bool self_closing = false;

    while (q < end && name_char(*q)) {
        if (tl + 1 < sizeof(tag))
            tag[tl++] = tolower((unsigned char)*q);
        q++;
    }
    tag[tl] = 0;
    if (!(n = new_node(NODE_ELEMENT)))
        return q;
    n->type = NODE_ELEMENT;
    n->tag = strdup(tag);
    n->selected = -1;
    // Attributes.
    for (;;) {
        const char *ns, *vs = NULL, *ve = NULL;
        char *name;

        while (q < end && (isspace((unsigned char)*q) || (*q == '/' && q + 1 < end && q[1] != '>')))
            q++;
        if (q >= end)
            break;
        if (*q == '>') {
            q++;
            break;
        }
        if (*q == '/') {
            self_closing = true;
            q++;
            continue;
        }
        ns = q;
        while (q < end && name_char(*q))
            q++;
        if (q == ns) {
            q++;
            continue;
        }
        name = strndup(ns, q - ns);
        while (q < end && isspace((unsigned char)*q))
            q++;
        if (q < end && *q == '=') {
            q++;
            while (q < end && isspace((unsigned char)*q))
                q++;
            if (q < end && (*q == '"' || *q == '\'')) {
                char quote = *q++;

                vs = q;
                while (q < end && *q != quote)
                    q++;
                ve = q;
                if (q < end)
                    q++;
            } else {
                vs = q;
                while (q < end && !isspace((unsigned char)*q) && *q != '>')
                    q++;
                ve = q;
            }
        }
        if (name) {
            struct attr *a = realloc(n->attrs, (n->nattrs + 1) * sizeof(*a));

            for (char *c = name; *c; c++)
                *c = tolower((unsigned char)*c);
            if (a && !node_attr(n, name)) {
                n->attrs = a;
                a[n->nattrs].name = name;
                a[n->nattrs].value = vs ? decode(vs, ve, true) : strdup("");
                n->nattrs++;
            } else {
                if (a)
                    n->attrs = a;
                free(name);
            }
        }
    }
    if (!strcmp(tag, "html") || !strcmp(tag, "body") || !strcmp(tag, "head")) {
        // Only the first counts; later ones add their attributes.
        struct node *old = node_find(p->doc, tag);

        if (old) {
            for (int i = 0; i < n->nattrs; i++)
                if (!node_attr(old, n->attrs[i].name)) {
                    struct attr *a = realloc(old->attrs, (old->nattrs + 1) * sizeof(*a));

                    if (a) {
                        old->attrs = a;
                        a[old->nattrs++] = n->attrs[i];
                        n->attrs[i].name = n->attrs[i].value = NULL;
                    }
                }
            for (int i = 0; i < n->nattrs; i++) {
                free(n->attrs[i].name);
                free(n->attrs[i].value);
            }
            n->nattrs = 0;
            node_free(n);
            if (!strcmp(tag, "body"))
                p->cur = old;
            return q;
        }
        if (!strcmp(tag, "body")) {
            struct node *head = node_find(p->doc, "head");

            if (head && open_in_scope(p, "head", NULL))
                p->cur = head->parent;
        }
    }
    implied_ends(p, tag);
    append(p->cur, n);
    if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "textarea") || !strcmp(tag, "title")
        || !strcmp(tag, "xmp") || !strcmp(tag, "noembed") || !strcmp(tag, "iframe")) {
        *raw_end_tag = n->tag;
        p->cur = n;
        return q;
    }
    if (!in_list(tag, void_tags) && !(self_closing && (!strcmp(tag, "svg") || !strcmp(tag, "math"))))
        p->cur = n;
    return q;
}

static const char *end_tag(struct parser *p, const char *s, const char *end)
{
    char tag[32];
    size_t tl = 0;
    const char *q = s;

    while (q < end && name_char(*q)) {
        if (tl + 1 < sizeof(tag))
            tag[tl++] = tolower((unsigned char)*q);
        q++;
    }
    tag[tl] = 0;
    while (q < end && *q != '>')
        q++;
    if (q < end)
        q++;
    if (!strcmp(tag, "body") || !strcmp(tag, "html"))
        return q;               // content after </body> still belongs to the body
    if (!strcmp(tag, "p") && !open_in_scope(p, "p", block_scope)) {
        // A stray </p> makes an empty paragraph.
        struct node *n = new_node(NODE_ELEMENT);

        if (n) {
            n->type = NODE_ELEMENT;
            n->tag = strdup("p");
            append(p->cur, n);
        }
        return q;
    }
    if (!strcmp(tag, "br")) {
        struct node *n = new_node(NODE_ELEMENT);

        if (n) {
            n->type = NODE_ELEMENT;
            n->tag = strdup("br");
            append(p->cur, n);
        }
        return q;
    }
    if (open_in_scope(p, tag, NULL))
        close_to(p, tag);
    return q;
}

static void unlink_node(struct node *n)
{
    if (n->prev)
        n->prev->next = n->next;
    else if (n->parent)
        n->parent->first = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else if (n->parent)
        n->parent->last = n->prev;
    n->parent = n->prev = n->next = NULL;
}

static struct node *element(const char *tag)
{
    struct node *n = new_node(NODE_ELEMENT);

    if (n)
        n->tag = strdup(tag);
    return n;
}

static bool head_content(const struct node *n)
{
    static const char *const tags[] = { "head", "title", "meta", "link", "style", "base", NULL };

    return n->type == NODE_ELEMENT && in_list(n->tag, tags);
}

// Like browsers, makes sure the document is <html> with a <head> and a
// <body>, so html and body styles apply to pages that leave them out.
static void implied_structure(struct node *doc)
{
    struct node *html = NULL, *body = NULL, *c, *next;

    for (c = doc->first; c; c = c->next)
        if (node_is(c, "html"))
            html = c;
    if (!html) {
        if (!(html = element("html")))
            return;
        for (c = doc->first; c; c = next) {
            next = c->next;
            unlink_node(c);
            append(html, c);
        }
        append(doc, html);
    } else {
        // Anything outside <html> (comments aside) goes inside it.
        for (c = doc->first; c; c = next) {
            next = c->next;
            if (c != html && (c->type == NODE_ELEMENT || (c->type == NODE_TEXT && c->text[strspn(c->text, " \t\r\n")]))) {
                unlink_node(c);
                append(html, c);
            }
        }
    }
    for (c = html->first; c; c = c->next)
        if (node_is(c, "body"))
            body = c;
    if (body)
        return;
    if (!(body = element("body")))
        return;
    for (c = html->first; c; c = next) {
        next = c->next;
        if (head_content(c))
            continue;
        unlink_node(c);
        append(body, c);
    }
    append(html, body);
}

struct node *html_parse(const char *src, size_t len)
{
    struct parser p;
    const char *s = src, *end = src + len, *text = src, *raw = NULL;

    if (!(p.doc = new_node(NODE_DOCUMENT)))
        return NULL;
    p.doc->type = NODE_DOCUMENT;
    p.cur = p.doc;
    // A byte order mark.
    if (len >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3))
        s = text = src + 3;
    while (s < end) {
        if (raw) {
            // Inside script/style/textarea/title: only the matching end tag counts.
            size_t rl = strlen(raw);

            if (*s == '<' && s + 2 + rl <= end && s[1] == '/' && !strncasecmp(s + 2, raw, rl)
                && (s + 2 + rl == end || !name_char(s[2 + rl]))) {
                add_text(&p, text, s, !strcmp(raw, "textarea") || !strcmp(raw, "title"));
                s = end_tag(&p, s + 2, end);
                text = s;
                raw = NULL;
            } else {
                s++;
            }
            continue;
        }
        if (*s != '<' || s + 1 >= end) {
            s++;
            continue;
        }
        if (s[1] == '!') {
            add_text(&p, text, s, true);
            if (s + 4 <= end && !strncmp(s, "<!--", 4)) {
                const char *c = strstr(s + 4, "-->");

                s = c && c < end ? c + 3 : end;
            } else if (s + 9 <= end && !strncmp(s, "<![CDATA[", 9)) {
                const char *c = strstr(s + 9, "]]>");

                add_text(&p, s + 9, c && c < end ? c : end, false);
                s = c && c < end ? c + 3 : end;
            } else {
                while (s < end && *s != '>')
                    s++;
                s += s < end;
            }
            text = s;
        } else if (s[1] == '?') {
            add_text(&p, text, s, true);
            while (s < end && *s != '>')
                s++;
            s += s < end;
            text = s;
        } else if (s[1] == '/' && s + 2 < end && isalpha((unsigned char)s[2])) {
            add_text(&p, text, s, true);
            s = end_tag(&p, s + 2, end);
            text = s;
        } else if (isalpha((unsigned char)s[1])) {
            add_text(&p, text, s, true);
            s = start_tag(&p, s + 1, end, &raw);
            text = s;
        } else {
            s++;
        }
    }
    add_text(&p, text, end, !raw);
    implied_structure(p.doc);
    return p.doc;
}
