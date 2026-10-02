#include "ui_internal.h"

// The AUI markup parser. AUI is HTML-shaped: elements with attributes and
// text, comments, character entities, self-closing tags. There is no
// styling; elements name widgets and attributes set their properties.

struct parser {
    const char *p, *src;
    int line;
    struct ui_window *win;
    bool error;
};

static const char *const void_tags[] = {
    "input", "password", "spin", "separator", "spacer", "image", "img", "br", "hr", "progress", "slider",
    "checkbox", "radio", "toggle", "canvas", "switch", "textbox", NULL,
};

static const struct {
    const char *from, *to;
} aliases[] = {
    { "img", "image" }, { "hr", "separator" }, { "select", "dropdown" }, { "div", "vbox" }, { "row", "hbox" },
    { "column", "vbox" }, { "col", "vbox" }, { "textbox", "input" }, { "check", "checkbox" },
    { "switch", "toggle" }, { "tabview", "tabs" }, { "page", "tab" }, { "text", "label" }, { "h3", "h2" },
    { "frame", "group" }, { "fieldset", "group" }, { "a", "link" }, { "combobox", "dropdown" },
    { "listbox", "list" }, { "pre", "label" }, { "span", "label" },
};

static void error(struct parser *P, const char *fmt, const char *arg)
{
    dprintf(STDERR_FILENO, "%s:%d: ", P->src ? P->src : "aui", P->line);
    dprintf(STDERR_FILENO, fmt, arg);
    dprintf(STDERR_FILENO, "\n");
    P->error = true;
}

static bool is_void(const char *tag)
{
    for (int i = 0; void_tags[i]; i++)
        if (!strcmp(void_tags[i], tag))
            return true;
    return false;
}

static const char *canonical(const char *tag)
{
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (!strcmp(aliases[i].from, tag))
            return aliases[i].to;
    return tag;
}

static void advance(struct parser *P, int n)
{
    while (n-- > 0 && *P->p) {
        if (*P->p == '\n')
            P->line++;
        P->p++;
    }
}

static void skip_space(struct parser *P)
{
    while (isspace((unsigned char)*P->p))
        advance(P, 1);
}

static bool name_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':' || c == '.';
}

static int read_name(struct parser *P, char *out, int size)
{
    int n = 0;

    while (name_char(*P->p)) {
        if (n < size - 1)
            out[n++] = tolower((unsigned char)*P->p);
        advance(P, 1);
    }
    out[n] = 0;
    return n;
}

// Appends decoded text (entities expanded) to a growing buffer.
struct buf {
    char *s;
    int len, cap;
};

static void buf_put(struct buf *b, const char *s, int n)
{
    if (b->len + n + 1 > b->cap) {
        int cap = MAX(b->cap * 2, b->len + n + 64);
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

static void put_entity(struct parser *P, struct buf *b)
{
    static const struct {
        const char *name, *text;
    } ents[] = {
        { "lt", "<" }, { "gt", ">" }, { "amp", "&" }, { "quot", "\"" }, { "apos", "'" }, { "nbsp", "\xC2\xA0" },
        { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" }, { "trade", "\xE2\x84\xA2" }, { "hellip", "\xE2\x80\xA6" },
        { "mdash", "\xE2\x80\x94" }, { "ndash", "\xE2\x80\x93" }, { "bull", "\xE2\x80\xA2" },
        { "times", "\xC3\x97" }, { "divide", "\xC3\xB7" }, { "deg", "\xC2\xB0" }, { "euro", "\xE2\x82\xAC" },
        { "larr", "\xE2\x86\x90" }, { "rarr", "\xE2\x86\x92" }, { "uarr", "\xE2\x86\x91" },
        { "darr", "\xE2\x86\x93" },
    };
    const char *semi = strchr(P->p, ';');
    char name[16];
    int len;

    if (!semi || (len = semi - P->p - 1) <= 0 || len >= (int)sizeof(name)) {
        buf_put(b, "&", 1);
        advance(P, 1);
        return;
    }
    memcpy(name, P->p + 1, len);
    name[len] = 0;
    if (name[0] == '#') {
        uint32_t cp = name[1] == 'x' || name[1] == 'X' ? strtoul(name + 2, NULL, 16) : strtoul(name + 1, NULL, 10);
        char enc[4];

        if (cp && cp <= 0x10FFFF)
            buf_put(b, enc, utf8_encode(cp, enc));
        advance(P, len + 2);
        return;
    }
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        if (!strcmp(ents[i].name, name)) {
            buf_put(b, ents[i].text, strlen(ents[i].text));
            advance(P, len + 2);
            return;
        }
    }
    buf_put(b, "&", 1);
    advance(P, 1);
}

static char *read_value(struct parser *P)
{
    struct buf b = { 0 };
    char quote = 0;

    if (*P->p == '"' || *P->p == '\'') {
        quote = *P->p;
        advance(P, 1);
    }
    while (*P->p) {
        if (quote ? *P->p == quote : (isspace((unsigned char)*P->p) || *P->p == '>' || (*P->p == '/' && P->p[1] == '>')))
            break;
        if (*P->p == '&') {
            put_entity(P, &b);
            continue;
        }
        buf_put(&b, P->p, 1);
        advance(P, 1);
    }
    if (quote) {
        if (*P->p != quote)
            error(P, "unterminated attribute value", NULL);
        else
            advance(P, 1);
    }
    if (!b.s)
        b.s = strdup("");
    return b.s;
}

static bool skip_special(struct parser *P)
{
    if (!strncmp(P->p, "<!--", 4)) {
        const char *end = strstr(P->p + 4, "-->");

        if (!end) {
            error(P, "unterminated comment", NULL);
            P->p += strlen(P->p);
            return true;
        }
        advance(P, end + 3 - P->p);
        return true;
    }
    if (!strncmp(P->p, "<?", 2) || !strncmp(P->p, "<!", 2)) {
        const char *end = strchr(P->p, '>');

        advance(P, end ? end + 1 - P->p : (int)strlen(P->p));
        return true;
    }
    return false;
}

static bool translated(const char *name)
{
    return !strcmp(name, "text") || !strcmp(name, "title") || !strcmp(name, "placeholder")
           || !strcmp(name, "tooltip");
}

static void apply_window_attr(struct ui_window *win, const char *name, const char *value)
{
    if (!strcmp(name, "title")) {
        free(win->title);
        win->title = strdup(ui_translate(value));
    } else if (!strcmp(name, "width")) {
        win->width = atoi(value);
    } else if (!strcmp(name, "height")) {
        win->height = atoi(value);
    } else if (!strcmp(name, "resizable")) {
        if (!attr_bool(value))
            win->flags |= WM_FLAG_NO_RESIZE;
    } else if (!strcmp(name, "role")) {
        uint32_t role = !strcmp(value, "dialog") ? WM_ROLE_DIALOG : !strcmp(value, "panel") ? WM_ROLE_PANEL
                        : !strcmp(value, "desktop") ? WM_ROLE_DESKTOP : !strcmp(value, "overlay") ? WM_ROLE_OVERLAY
                        : WM_ROLE_NORMAL;

        win->flags = (win->flags & ~WM_ROLE_MASK) | role;
    } else if (!strcmp(name, "dock") && !strcmp(value, "top")) {
        win->flags |= WM_FLAG_PANEL_TOP;
    } else if (!strcmp(name, "hidden")) {
        win->autoshow = !attr_bool(value);
    } else if (!strcmp(name, "modal")) {
        win->modal = attr_bool(value);
    } else if (strcmp(name, "width") && strcmp(name, "height")) {
        // padding, spacing, onclose, ... belong to the root box.
        widget_set_attr(win->root, name, value);
    }
}

// Collapses runs of white space into single spaces and trims the ends.
static void collapse(char *s)
{
    char *d = s;
    bool space = false;

    for (char *p = s; *p; p++) {
        if (isspace((unsigned char)*p)) {
            space = d != s;
            continue;
        }
        if (space)
            *d++ = ' ';
        space = false;
        *d++ = *p;
    }
    *d = 0;
}

static struct widget *parse_element(struct parser *P, struct widget *parent, bool is_root);

static void parse_content(struct parser *P, struct widget *w, const char *tag)
{
    struct buf text = { 0 };
    bool raw = !strcmp(tag, "textarea") || !strcmp(tag, "pre");

    while (*P->p && !P->error) {
        if (*P->p == '<') {
            if (skip_special(P))
                continue;
            if (P->p[1] == '/') {
                char name[32];

                advance(P, 2);
                read_name(P, name, sizeof(name));
                skip_space(P);
                if (*P->p == '>')
                    advance(P, 1);
                if (strcmp(name, tag) && strcmp(canonical(name), w ? w->tag : tag)) {
                    if (is_void(name))
                        continue;   // a stray </input> after <input>
                    error(P, "unexpected closing tag </%s>", name);
                    break;
                }
                break;
            }
            if (!strncmp(P->p, "<br", 3) && !name_char(P->p[3])) {
                const char *end = strchr(P->p, '>');

                buf_put(&text, "\n", 1);
                advance(P, end ? end + 1 - P->p : 3);
                continue;
            }
            advance(P, 1);
            {
                struct widget *child = parse_element(P, w, false);

                if (child && w)
                    ui_add(w, child);
                else if (child)
                    widget_free(child);
            }
            continue;
        }
        if (*P->p == '&') {
            put_entity(P, &text);
            continue;
        }
        buf_put(&text, P->p, 1);
        advance(P, 1);
    }
    if (text.s && w) {
        char *t = text.s;

        if (raw) {
            if (*t == '\n')
                t++;
            // Drop the indentation line before the closing tag.
            for (int n = strlen(t); n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t'); n--)
                t[n - 1] = 0;
            if (*t && t[strlen(t) - 1] == '\n' && !strcmp(tag, "textarea"))
                t[strlen(t) - 1] = 0;
        } else {
            // <br> newlines survive collapsing as a marker.
            for (char *p = t; *p; p++)
                if (*p == '\n')
                    *p = ' ';
            collapse(t);
        }
        if (*t && !ui_attr(w, "text"))
            ui_set_text(w, ui_translate(t));
    }
    free(text.s);
}

// Parses an element after its '<'. Returns the widget (not yet added to
// parent), or NULL.
static struct widget *parse_element(struct parser *P, struct widget *parent, bool is_root)
{
    char tag[32], name[48];
    const char *ctag;
    struct widget *w = NULL;
    char *names[64], *values[64];
    int nattr = 0;
    bool self_close = false, window_root;

    if (!read_name(P, tag, sizeof(tag))) {
        error(P, "expected an element name", NULL);
        return NULL;
    }
    ctag = canonical(tag);
    window_root = is_root && !strcmp(ctag, "window");
    // Attributes.
    while (*P->p && !P->error) {
        skip_space(P);
        if (*P->p == '>') {
            advance(P, 1);
            break;
        }
        if (P->p[0] == '/' && P->p[1] == '>') {
            advance(P, 2);
            self_close = true;
            break;
        }
        if (!read_name(P, name, sizeof(name))) {
            error(P, "bad attribute in <%s>", tag);
            break;
        }
        skip_space(P);
        if (nattr < 64) {
            names[nattr] = strdup(name);
            if (*P->p == '=') {
                advance(P, 1);
                skip_space(P);
                values[nattr] = read_value(P);
            } else {
                values[nattr] = strdup("true");
            }
            nattr++;
        }
    }
    if (window_root) {
        w = P->win->root;
    } else if (strcmp(ctag, "br")) {
        w = widget_new(P->win, ctag);
        if (!w)
            error(P, "unknown element <%s>", tag);
    }
    if (w) {
        // The id first, so handlers can find the widget; then the rest in order.
        for (int i = 0; i < nattr; i++)
            if (!strcmp(names[i], "id"))
                widget_set_attr(w, "id", values[i]);
        for (int i = 0; i < nattr; i++) {
            const char *v = translated(names[i]) ? ui_translate(values[i]) : values[i];

            if (!strcmp(names[i], "id"))
                continue;
            if (window_root)
                apply_window_attr(P->win, names[i], v);
            else
                widget_set_attr(w, names[i], v);
        }
    }
    for (int i = 0; i < nattr; i++) {
        free(names[i]);
        free(values[i]);
    }
    if (!self_close && !is_void(tag) && !P->error)
        parse_content(P, w, tag);
    if (w && !P->error && (!strcmp(ctag, "list") || !strcmp(ctag, "table") || !strcmp(ctag, "dropdown")))
        ui_list_init_from_children(w);
    (void)parent;
    if (P->error && w && !window_root) {
        widget_free(w);
        return NULL;
    }
    return w;
}

// Parses AUI text. With parent set, the elements become its children and the
// first is returned. Without, the text describes a window (into win->root).
struct widget *aui_parse(struct ui_window *win, const char *text, struct widget *parent, const char *source)
{
    struct parser P = { text, source, 1, win, false };
    struct widget *first = NULL;
    bool root_done = false;

    while (*P.p && !P.error) {
        skip_space(&P);
        if (!*P.p)
            break;
        if (*P.p != '<') {
            error(&P, "text outside an element", NULL);
            break;
        }
        if (skip_special(&P))
            continue;
        advance(&P, 1);
        {
            bool as_root = !parent && !root_done;
            struct widget *w = parse_element(&P, parent ? parent : win->root, as_root);

            if (!w)
                continue;
            if (w == win->root) {
                root_done = true;
                if (!first)
                    first = w;
                continue;
            }
            ui_add(parent ? parent : win->root, w);
            if (!first)
                first = w;
        }
    }
    return P.error ? NULL : first ? first : parent ? NULL : win->root;
}

struct ui_window *ui_load_string(const char *aui, const struct ui_handler_entry *handlers, void *user)
{
    return ui_load_string_named(aui, handlers, user, NULL);
}

struct ui_window *ui_load_string_named(const char *aui, const struct ui_handler_entry *handlers, void *user,
                                       const char *source)
{
    struct ui_window *win = ui_window_new("", 0, 0, WM_ROLE_NORMAL);

    if (!win)
        return NULL;
    win->handlers = handlers;
    win->user = user;
    if (!aui_parse(win, aui, NULL, source)) {
        win->closing = true;
        return NULL;
    }
    return win;
}

struct ui_window *ui_load(const char *path, const struct ui_handler_entry *handlers, void *user)
{
    int fd = open(path, O_RDONLY);
    struct aegis_stat st;
    char *text;
    ssize_t n, got = 0;
    struct ui_window *win;

    if (fd < 0 || fstat(fd, &st) < 0) {
        dprintf(STDERR_FILENO, "ui: cannot open %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    if (!(text = malloc(st.size + 1))) {
        close(fd);
        return NULL;
    }
    while (got < (ssize_t)st.size && (n = read(fd, text + got, st.size - got)) > 0)
        got += n;
    close(fd);
    text[got] = 0;
    win = ui_load_string_named(text, handlers, user, path);
    free(text);
    return win;
}

struct widget *ui_parse_into(struct widget *parent, const char *aui)
{
    if (!parent || !parent->win)
        return NULL;
    return aui_parse(parent->win, aui, parent, NULL);
}
