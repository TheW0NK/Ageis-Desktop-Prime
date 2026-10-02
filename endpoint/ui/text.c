#include "ui_internal.h"

// Text entry: <input>, <password>, <spin> (numbers) and <textarea>. The
// widget's text is the edit buffer itself; positions are byte offsets that
// always sit on UTF-8 character boundaries.

#define UNDO_MAX    32
#define PAD_X       8
#define SPIN_W      20
#define BAR_W       12

struct vline {
    int start, len;                 // len excludes the newline
};

struct snapshot {
    char *text;
    int cursor;
};

struct edit {
    int len, cap;
    int cursor, anchor;
    int sx, sy;                     // scroll offsets in pixels
    int pref_x;                     // remembered column for up/down
    bool multiline, password, spin, readonly, wrap, mono, modified;
    bool dragging, bar_drag;
    int bar_grab;
    struct vline *lines;
    int nlines, lines_cap, lines_width;
    bool lines_dirty;
    struct snapshot undo[UNDO_MAX], redo[UNDO_MAX];
    int nundo, nredo;
    uint64_t last_edit_ms;
    int last_kind;                  // 1 typing, 2 deleting, 0 other
};

static const char bullet[] = "\xE2\x80\xA2";

static struct edit *E(struct widget *w)
{
    return w->data;
}

static struct font *edit_font(struct widget *w)
{
    return E(w)->mono ? ui_font_mono() : ui_font();
}

static int tab_width(struct font *f)
{
    return MAX(1, text_width(f, "    ", 4));
}

// ---- Measuring text with tabs ----

int ui_text_width_tabs(struct font *f, const char *s, int len, int tab_w)
{
    int x = 0, seg = 0;

    for (int i = 0; i < len; i++) {
        if (s[i] == '\t') {
            x += text_width(f, s + seg, i - seg);
            x = (x / tab_w + 1) * tab_w;
            seg = i + 1;
        }
    }
    return x + text_width(f, s + seg, len - seg);
}

// Pixel x of byte n within a line of text s.
static int line_x(struct widget *w, const char *s, int n)
{
    struct font *f = edit_font(w);

    if (E(w)->password) {
        int chars = 0;

        for (int i = 0; i < n; i = utf8_next(s, i))
            chars++;
        return chars * text_width(f, bullet, 3);
    }
    return ui_text_width_tabs(f, s, n, tab_width(f));
}

// The byte offset in a line (of length len) nearest to pixel x.
static int line_hit(struct widget *w, const char *s, int len, int x)
{
    struct font *f = edit_font(w);
    int tw = tab_width(f), segx = 0, seg = 0;

    if (x <= 0)
        return 0;
    if (E(w)->password) {
        int bw = MAX(1, text_width(f, bullet, 3)), target = (x + bw / 2) / bw, i = 0;

        while (target-- > 0 && i < len)
            i = utf8_next(s, i);
        return MIN(i, len);
    }
    for (int i = 0; i <= len; i++) {
        if (i == len || s[i] == '\t') {
            int sw = text_width(f, s + seg, i - seg);

            if (x < segx + sw)
                return seg + text_hit(f, s + seg, i - seg, x - segx);
            if (i == len)
                return len;
            {
                int stop = ((segx + sw) / tw + 1) * tw;

                if (x < (segx + sw + stop) / 2)
                    return i;
                segx = stop;
                seg = i + 1;
            }
        }
    }
    return len;
}

static void draw_line(struct widget *w, struct gfx *g, int x, int y, const char *s, int len, color_t c)
{
    struct font *f = edit_font(w);
    int tw = tab_width(f), seg = 0, cx = 0;

    if (E(w)->password) {
        int bw = text_width(f, bullet, 3);

        for (int i = 0; i < len; i = utf8_next(s, i), cx += bw)
            text_draw(g, f, x + cx, y, bullet, 3, c);
        return;
    }
    for (int i = 0; i <= len; i++) {
        if (i == len || s[i] == '\t') {
            if (i > seg)
                text_draw(g, f, x + cx, y, s + seg, i - seg, c);
            cx += text_width(f, s + seg, i - seg);
            if (i < len)
                cx = (cx / tw + 1) * tw;
            seg = i + 1;
        }
    }
}

// ---- Layout of lines ----

static struct rect text_area(struct widget *w)
{
    struct edit *e = E(w);
    struct rect r = { w->r.x + PAD_X, w->r.y + 4, w->r.w - 2 * PAD_X, w->r.h - 8 };

    if (e->multiline) {
        r.w -= BAR_W - 2;
    } else if (e->spin) {
        r.w -= SPIN_W;
    }
    return r;
}

static void push_line(struct edit *e, int start, int len)
{
    if (e->nlines == e->lines_cap) {
        int cap = e->lines_cap ? e->lines_cap * 2 : 64;
        struct vline *l = realloc(e->lines, cap * sizeof(*l));

        if (!l)
            return;
        e->lines = l;
        e->lines_cap = cap;
    }
    e->lines[e->nlines++] = (struct vline){ start, len };
}

static void compute_lines(struct widget *w)
{
    struct edit *e = E(w);
    const char *t = w->text;
    int width = text_area(w).w, pos = 0;

    if (!e->lines_dirty && e->lines_width == width)
        return;
    e->lines_dirty = false;
    e->lines_width = width;
    e->nlines = 0;
    while (true) {
        const char *nl = e->multiline ? memchr(t + pos, '\n', e->len - pos) : NULL;
        int end = nl ? nl - t : e->len;

        if (e->wrap && e->multiline && width > 20) {
            int s = pos;

            do {
                int n = end - s, fit = n;

                if (line_x(w, t + s, n) > width) {
                    fit = line_hit(w, t + s, n, width);
                    while (fit > 0 && line_x(w, t + s, fit) > width)
                        fit = utf8_prev(t + s, fit);
                    if (fit <= 0)
                        fit = utf8_next(t + s, 0);
                    // Prefer to break after a space.
                    for (int k = fit; k > 0; k--) {
                        if (t[s + k - 1] == ' ') {
                            fit = k;
                            break;
                        }
                    }
                }
                push_line(e, s, fit);
                s += fit;
            } while (s < end);
        } else {
            push_line(e, pos, end - pos);
        }
        if (!nl)
            break;
        pos = end + 1;
    }
}

static int line_of(struct widget *w, int pos)
{
    struct edit *e = E(w);
    int lo = 0, hi = e->nlines - 1;

    compute_lines(w);
    // The last line whose start is <= pos.
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;

        if (e->lines[mid].start <= pos)
            lo = mid;
        else
            hi = mid - 1;
    }
    // A wrapped line ends where the next begins: the cursor belongs to the next.
    return lo;
}

static int line_h(struct widget *w)
{
    return font_line_height(edit_font(w));
}

static int content_h(struct widget *w)
{
    compute_lines(w);
    return E(w)->nlines * line_h(w);
}

static void ensure_visible(struct widget *w)
{
    struct edit *e = E(w);
    struct rect a = text_area(w);
    int l, x, lh = line_h(w);

    if (a.w <= 0)
        return;
    compute_lines(w);
    l = line_of(w, e->cursor);
    x = line_x(w, w->text + e->lines[l].start, e->cursor - e->lines[l].start);
    if (x - e->sx > a.w - 2)
        e->sx = x - a.w + 2 + (e->multiline ? 40 : 0);
    if (x - e->sx < 0)
        e->sx = MAX(0, x - (e->multiline ? 40 : a.w / 3));
    if (!e->multiline && e->sx > 0) {
        // Do not leave empty space after the end of the text.
        int total = line_x(w, w->text, e->len);

        if (total - e->sx < a.w - 2)
            e->sx = MAX(0, total - a.w + 2);
    }
    if (e->multiline) {
        if (l * lh < e->sy)
            e->sy = l * lh;
        if ((l + 1) * lh > e->sy + a.h)
            e->sy = (l + 1) * lh - a.h;
        e->sy = MAX(0, MIN(e->sy, MAX(0, content_h(w) - a.h)));
    }
}

// ---- Editing ----

static void snapshot_free(struct snapshot *s)
{
    free(s->text);
    s->text = NULL;
}

static void push_snapshot(struct snapshot *stack, int *n, const char *text, int cursor)
{
    if (*n == UNDO_MAX) {
        snapshot_free(&stack[0]);
        memmove(stack, stack + 1, (UNDO_MAX - 1) * sizeof(*stack));
        (*n)--;
    }
    stack[*n].text = strdup(text);
    stack[*n].cursor = cursor;
    if (stack[*n].text)
        (*n)++;
}

static void save_undo(struct widget *w, int kind)
{
    struct edit *e = E(w);
    uint64_t now = uptime_ms();

    // Typing runs are undone together.
    if (!(kind && kind == e->last_kind && now - e->last_edit_ms < 1500 && e->nundo))
        push_snapshot(e->undo, &e->nundo, w->text, e->cursor);
    e->last_kind = kind;
    e->last_edit_ms = now;
    while (e->nredo)
        snapshot_free(&e->redo[--e->nredo]);
}

static bool reserve(struct widget *w, int need)
{
    struct edit *e = E(w);

    if (need + 1 <= e->cap)
        return true;
    {
        int cap = MAX(need + 1, e->cap * 2);
        char *t = realloc(w->text, cap);

        if (!t)
            return false;
        w->text = t;
        e->cap = cap;
    }
    return true;
}

static void changed(struct widget *w)
{
    struct edit *e = E(w);

    e->lines_dirty = true;
    e->modified = true;
    ensure_visible(w);
    ui_redraw(w);
    if (e->spin) {
        char *end;
        double v = strtod(w->text, &end);

        if (end != w->text && !*end)
            w->value = v;
    }
    ui_emit(w, "change");
}

static void replace(struct widget *w, int start, int end, const char *ins, int n, int kind)
{
    struct edit *e = E(w);
    const char *max = ui_attr(w, "maxlength");
    char *clean = NULL;

    if (e->readonly)
        return;
    if (!e->multiline && memchr(ins, '\n', n)) {
        // One-line fields take pasted lines joined by spaces.
        if (!(clean = malloc(n)))
            return;
        for (int i = 0; i < n; i++)
            clean[i] = ins[i] == '\n' || ins[i] == '\r' ? ' ' : ins[i];
        ins = clean;
    }
    if (max && e->len - (end - start) + n > atoi(max))
        n = MAX(0, atoi(max) - (e->len - (end - start)));
    while (n > 0 && (ins[n - 1] & 0xC0) == 0x80 && n < (int)strlen(ins))
        n--;     // never cut a character in half
    if (!reserve(w, e->len - (end - start) + n)) {
        free(clean);
        return;
    }
    save_undo(w, kind);
    memmove(w->text + start + n, w->text + end, e->len - end + 1);
    memcpy(w->text + start, ins, n);
    e->len += n - (end - start);
    e->cursor = e->anchor = start + n;
    free(clean);
    changed(w);
}

static bool has_selection(struct edit *e)
{
    return e->cursor != e->anchor;
}

static int sel_start(struct edit *e)
{
    return MIN(e->cursor, e->anchor);
}

static int sel_end(struct edit *e)
{
    return MAX(e->cursor, e->anchor);
}

static void insert_text(struct widget *w, const char *s, int kind)
{
    struct edit *e = E(w);

    replace(w, sel_start(e), sel_end(e), s, strlen(s), kind);
}

static void restore(struct widget *w, struct snapshot *from, int *nfrom, struct snapshot *to, int *nto)
{
    struct edit *e = E(w);
    struct snapshot s;
    int len;

    if (!*nfrom)
        return;
    s = from[--(*nfrom)];
    push_snapshot(to, nto, w->text, e->cursor);
    len = strlen(s.text);
    if (reserve(w, len)) {
        memcpy(w->text, s.text, len + 1);
        e->len = len;
        e->cursor = e->anchor = MIN(s.cursor, len);
    }
    free(s.text);
    e->last_kind = 0;
    changed(w);
}

// ---- Cursor movement ----

static bool is_word(const char *t, int i)
{
    unsigned char c = t[i];

    return isalnum(c) || c == '_' || c >= 0x80;
}

static int word_left(struct widget *w, int pos)
{
    const char *t = w->text;

    while (pos > 0 && !is_word(t, utf8_prev(t, pos)))
        pos = utf8_prev(t, pos);
    while (pos > 0 && is_word(t, utf8_prev(t, pos)))
        pos = utf8_prev(t, pos);
    return pos;
}

static int word_right(struct widget *w, int pos)
{
    struct edit *e = E(w);
    const char *t = w->text;

    while (pos < e->len && !is_word(t, pos))
        pos = utf8_next(t, pos);
    while (pos < e->len && is_word(t, pos))
        pos = utf8_next(t, pos);
    return pos;
}

static void move_to(struct widget *w, int pos, bool extend)
{
    struct edit *e = E(w);

    e->cursor = MIN(MAX(pos, 0), e->len);
    if (!extend)
        e->anchor = e->cursor;
    e->last_kind = 0;
    ensure_visible(w);
    ui_redraw(w);
}

static int cursor_x(struct widget *w)
{
    struct edit *e = E(w);
    int l = line_of(w, e->cursor);

    return line_x(w, w->text + e->lines[l].start, e->cursor - e->lines[l].start);
}

static int pos_in_line(struct widget *w, int l, int x)
{
    struct edit *e = E(w);
    struct vline *v = &e->lines[l];
    int p = line_hit(w, w->text + v->start, v->len, x);

    // On a wrapped line, the end belongs to the next line.
    if (l + 1 < e->nlines && v->start + v->len == e->lines[l + 1].start && p == v->len && p > 0)
        p = utf8_prev(w->text + v->start, p);
    return v->start + p;
}

static void move_lines(struct widget *w, int delta, bool extend)
{
    struct edit *e = E(w);
    int l = line_of(w, e->cursor), target = l + delta;

    if (e->last_kind != 3)
        e->pref_x = cursor_x(w);
    if (target < 0) {
        move_to(w, 0, extend);
    } else if (target >= e->nlines) {
        move_to(w, e->len, extend);
    } else {
        move_to(w, pos_in_line(w, target, e->pref_x), extend);
    }
    e->last_kind = 3;   // keep pref_x across consecutive vertical moves
}

// ---- Clipboard ----

static void copy_selection(struct widget *w)
{
    struct edit *e = E(w);
    char *s;

    if (!has_selection(e) || e->password)
        return;
    if (!(s = strndup(w->text + sel_start(e), sel_end(e) - sel_start(e))))
        return;
    ui_clipboard_set(s);
    free(s);
}

const char *ui_edit_shortcut(struct wm_event *ev)
{
    if (!(ev->mods & MOD_CTRL) || (ev->mods & MOD_ALT) || !ev->value)
        return NULL;
    switch (ev->key) {
    case KEY_A + 'a' - 'a': return "selectall";
    case KEY_A + 'c' - 'a': return "copy";
    case KEY_A + 'x' - 'a': return "cut";
    case KEY_A + 'v' - 'a': return "paste";
    case KEY_A + 'z' - 'a': return (ev->mods & MOD_SHIFT) ? "redo" : "undo";
    case KEY_A + 'y' - 'a': return "redo";
    case KEY_INSERT: return "copy";
    }
    return NULL;
}

// ---- Events ----

static void spin_step(struct widget *w, double delta)
{
    double v = w->value + delta;

    if (w->max > w->min)
        v = MIN(MAX(v, w->min), w->max);
    ui_set_value(w, v);
    ui_emit(w, "change");
}

static bool edit_key(struct widget *w, struct wm_event *ev)
{
    struct edit *e = E(w);
    bool shift = ev->mods & MOD_SHIFT, ctrl = ev->mods & MOD_CTRL;
    const char *action = ui_edit_shortcut(ev);
    int lines_per_page;

    if (!ev->value)
        return false;
    if (action) {
        if (!strcmp(action, "selectall")) {
            e->anchor = 0;
            move_to(w, e->len, true);
        } else if (!strcmp(action, "copy")) {
            copy_selection(w);
        } else if (!strcmp(action, "cut")) {
            copy_selection(w);
            if (has_selection(e) && !e->password)
                replace(w, sel_start(e), sel_end(e), "", 0, 0);
        } else if (!strcmp(action, "paste")) {
            insert_text(w, ui_clipboard_get(), 0);
        } else if (!strcmp(action, "undo")) {
            restore(w, e->undo, &e->nundo, e->redo, &e->nredo);
        } else if (!strcmp(action, "redo")) {
            restore(w, e->redo, &e->nredo, e->undo, &e->nundo);
        }
        return true;
    }
    if (ev->mods & MOD_SHIFT && ev->key == KEY_INSERT) {
        insert_text(w, ui_clipboard_get(), 0);
        return true;
    }
    if (ev->mods & MOD_SHIFT && ev->key == KEY_DELETE) {
        copy_selection(w);
        if (has_selection(e))
            replace(w, sel_start(e), sel_end(e), "", 0, 0);
        return true;
    }
    compute_lines(w);
    lines_per_page = MAX(1, text_area(w).h / line_h(w) - 1);
    switch (ev->key) {
    case KEY_LEFT:
        if (has_selection(e) && !shift)
            move_to(w, sel_start(e), false);
        else
            move_to(w, ctrl ? word_left(w, e->cursor) : utf8_prev(w->text, e->cursor), shift);
        return true;
    case KEY_RIGHT:
        if (has_selection(e) && !shift)
            move_to(w, sel_end(e), false);
        else
            move_to(w, ctrl ? word_right(w, e->cursor) : utf8_next(w->text, e->cursor), shift);
        return true;
    case KEY_HOME:
        if (ctrl || !e->multiline)
            move_to(w, 0, shift);
        else
            move_to(w, e->lines[line_of(w, e->cursor)].start, shift);
        return true;
    case KEY_END:
        if (ctrl || !e->multiline) {
            move_to(w, e->len, shift);
        } else {
            struct vline *v = &e->lines[line_of(w, e->cursor)];
            int end = v->start + v->len;

            // On a wrapped line, stop before the break.
            if (end < e->len && w->text[end] != '\n' && end > v->start)
                end = utf8_prev(w->text, end);
            move_to(w, end, shift);
        }
        return true;
    case KEY_UP:
        if (e->spin) {
            spin_step(w, w->step);
            return true;
        }
        if (!e->multiline)
            return false;
        move_lines(w, -1, shift);
        return true;
    case KEY_DOWN:
        if (e->spin) {
            spin_step(w, -w->step);
            return true;
        }
        if (!e->multiline)
            return false;
        move_lines(w, 1, shift);
        return true;
    case KEY_PAGEUP:
        if (!e->multiline)
            return false;
        move_lines(w, -lines_per_page, shift);
        return true;
    case KEY_PAGEDOWN:
        if (!e->multiline)
            return false;
        move_lines(w, lines_per_page, shift);
        return true;
    case KEY_BACKSPACE:
        if (has_selection(e))
            replace(w, sel_start(e), sel_end(e), "", 0, 2);
        else if (e->cursor > 0)
            replace(w, ctrl ? word_left(w, e->cursor) : utf8_prev(w->text, e->cursor), e->cursor, "", 0, 2);
        return true;
    case KEY_DELETE:
        if (has_selection(e))
            replace(w, sel_start(e), sel_end(e), "", 0, 2);
        else if (e->cursor < e->len)
            replace(w, e->cursor, ctrl ? word_right(w, e->cursor) : utf8_next(w->text, e->cursor), "", 0, 2);
        return true;
    case KEY_ENTER:
    case KEY_KPENTER:
        if (!e->multiline || ctrl) {
            if (ev->value != 1)
                return true;
            if (!ui_has_handler(w, "activate"))
                return false;       // let the window's default button have it
            ui_emit(w, "activate");
            return true;
        }
        if (e->readonly)
            return true;
        {
            // Keep the indentation of the current line.
            char buf[128] = "\n";
            int n = 1, ls = e->cursor;

            while (ls > 0 && w->text[ls - 1] != '\n')
                ls--;
            if (ui_attr(w, "autoindent") && attr_bool(ui_attr(w, "autoindent")))
                while (n < 120 && ls < e->cursor && (w->text[ls] == ' ' || w->text[ls] == '\t'))
                    buf[n++] = w->text[ls++];
            buf[n] = 0;
            insert_text(w, buf, 0);
        }
        return true;
    case KEY_TAB:
        if (!e->multiline || ctrl || shift || e->readonly
            || (ui_attr(w, "tabfocus") && attr_bool(ui_attr(w, "tabfocus"))))
            return false;
        insert_text(w, "\t", 1);
        return true;
    }
    if (ev->text[0] && !(ev->mods & (MOD_CTRL | MOD_ALT | MOD_META))) {
        if (e->spin && !(isdigit((unsigned char)ev->text[0]) || strchr(".-+eE", ev->text[0])))
            return true;
        insert_text(w, ev->text, 1);
        return true;
    }
    return false;
}

static int hit_pos(struct widget *w, int x, int y)
{
    struct edit *e = E(w);
    struct rect a = text_area(w);
    int l;

    compute_lines(w);
    if (!e->multiline)
        return line_hit(w, w->text, e->len, x - a.x + e->sx);
    l = (y - a.y + e->sy) / line_h(w);
    l = MIN(MAX(l, 0), e->nlines - 1);
    return pos_in_line(w, l, x - a.x + e->sx);
}

static struct rect edit_bar(struct widget *w)
{
    return (struct rect){ w->r.x + w->r.w - BAR_W - 1, w->r.y + 2, BAR_W, w->r.h - 4 };
}

static void scroll_y(struct widget *w, int sy)
{
    struct edit *e = E(w);

    sy = MAX(0, MIN(sy, MAX(0, content_h(w) - text_area(w).h)));
    if (sy != e->sy) {
        e->sy = sy;
        ui_redraw(w);
    }
}

static bool edit_pointer(struct widget *w, struct wm_event *ev)
{
    struct edit *e = E(w);
    struct rect bar = edit_bar(w);
    int clicks;

    switch (ev->kind) {
    case WM_PTR_DOWN:
        if (ev->detail != BTN_LEFT)
            return ev->detail == BTN_RIGHT;
        if (e->spin && ev->x >= w->r.x + w->r.w - SPIN_W) {
            spin_step(w, ev->y < w->r.y + w->r.h / 2 ? w->step : -w->step);
            return true;
        }
        if (e->multiline && content_h(w) > text_area(w).h && rect_contains(bar, ev->x, ev->y)) {
            int th, ty = ui_scrollbar_thumb(bar, content_h(w), text_area(w).h, e->sy, &th);

            e->bar_grab = ev->y >= ty && ev->y < ty + th ? ev->y - ty : th / 2;
            e->bar_drag = true;
            scroll_y(w, ui_scrollbar_offset(bar, content_h(w), text_area(w).h, ev->y, e->bar_grab));
            return true;
        }
        clicks = ui_click_count(w);
        if (clicks == 2) {
            int p = hit_pos(w, ev->x, ev->y), s = p, t = p;

            while (s > 0 && is_word(w->text, utf8_prev(w->text, s)))
                s = utf8_prev(w->text, s);
            while (t < e->len && is_word(w->text, t))
                t = utf8_next(w->text, t);
            e->anchor = s;
            move_to(w, t, true);
        } else if (clicks >= 3) {
            int p = hit_pos(w, ev->x, ev->y), s = p, t = p;

            while (s > 0 && w->text[s - 1] != '\n')
                s--;
            while (t < e->len && w->text[t] != '\n')
                t++;
            e->anchor = s;
            move_to(w, t, true);
        } else {
            move_to(w, hit_pos(w, ev->x, ev->y), ev->mods & MOD_SHIFT);
        }
        e->dragging = true;
        return true;
    case WM_PTR_MOVE:
        if (e->bar_drag) {
            scroll_y(w, ui_scrollbar_offset(bar, content_h(w), text_area(w).h, ev->y, e->bar_grab));
            return true;
        }
        if (e->dragging) {
            move_to(w, hit_pos(w, ev->x, ev->y), true);
            return true;
        }
        return false;
    case WM_PTR_UP:
        e->dragging = e->bar_drag = false;
        return true;
    case WM_PTR_WHEEL:
        if (e->spin) {
            spin_step(w, ev->detail > 0 ? w->step : -w->step);
            return true;
        }
        if (!e->multiline)
            return false;
        {
            int old = e->sy;

            scroll_y(w, e->sy - ev->detail * 3 * line_h(w));
            return old != e->sy;
        }
    }
    return false;
}

// ---- Painting ----

static void edit_paint(struct widget *w, struct gfx *g)
{
    struct edit *e = E(w);
    struct rect a = text_area(w);
    struct font *f = edit_font(w);
    int lh = font_line_height(f), ss = sel_start(e), se = sel_end(e), first, last;
    bool focused = ui_is_focused(w);
    struct gfx saved = *g;
    color_t border = focused ? ui_theme.accent : w->hover ? ui_mix(ui_theme.border, ui_theme.text, 60) : ui_theme.border;
    const char *placeholder = ui_attr(w, "placeholder");

    gfx_fill_rounded(g, w->r, ui_theme.radius, e->readonly || !w->enabled ? ui_theme.surface_alt : ui_theme.input);
    gfx_outline_rounded(g, w->r, ui_theme.radius, focused ? 2 : 1, border);
    if (e->spin) {
        int x = w->r.x + w->r.w - SPIN_W, mid = w->r.y + w->r.h / 2;

        gfx_fill(g, (struct rect){ x, w->r.y + 4, 1, w->r.h - 8 }, ui_theme.border);
        ui_draw_arrow(g, x + SPIN_W / 2, mid - 6, 8, 1, ui_theme.text_dim);
        ui_draw_arrow(g, x + SPIN_W / 2, mid + 6, 8, 0, ui_theme.text_dim);
    }
    compute_lines(w);
    ui_clip(g, a);
    if (!e->len && placeholder && !focused) {
        text_draw(g, f, a.x, e->multiline ? a.y : a.y + (a.h - lh) / 2, ui_translate(placeholder), -1,
                  ui_theme.text_dim);
    }
    if (e->multiline) {
        first = e->sy / lh;
        last = MIN(e->nlines - 1, (e->sy + a.h) / lh);
    } else {
        first = last = 0;
    }
    for (int l = first; l <= last && l < e->nlines; l++) {
        struct vline *v = &e->lines[l];
        int y = e->multiline ? a.y + l * lh - e->sy : a.y + (a.h - lh) / 2, x = a.x - e->sx;

        if (ss != se && se >= v->start && ss <= v->start + v->len) {
            int s0 = MAX(ss, v->start) - v->start, s1 = MIN(se, v->start + v->len) - v->start;
            int x0 = line_x(w, w->text + v->start, s0), x1 = line_x(w, w->text + v->start, s1);

            if (se > v->start + v->len && e->multiline)
                x1 += 6;    // show the selected newline
            gfx_fill(g, (struct rect){ x + x0, y, x1 - x0, lh },
                     focused ? ui_theme.selection : ui_mix(ui_theme.selection, ui_theme.input, 120));
        }
        draw_line(w, g, x, y, w->text + v->start, v->len, w->enabled ? ui_theme.text : ui_theme.text_dim);
        if (focused && !e->readonly && e->cursor >= v->start && e->cursor <= v->start + v->len
            && (l == line_of(w, e->cursor)))
            gfx_fill(g, (struct rect){ x + line_x(w, w->text + v->start, e->cursor - v->start), y, 2, lh },
                     ui_theme.text);
    }
    *g = saved;
    if (e->multiline)
        ui_draw_scrollbar(g, edit_bar(w), content_h(w), a.h, e->sy, e->bar_drag);
}

// ---- Setup ----

static void edit_measure(struct widget *w, int avail, int *pw, int *ph)
{
    struct edit *e = E(w);

    (void)avail;
    if (e->multiline) {
        *pw = 320;
        *ph = 160;
    } else {
        *pw = e->spin ? 100 : 200;
        *ph = ui_theme.row_height;
    }
}

static void edit_init(struct widget *w)
{
    struct edit *e = calloc(1, sizeof(*e));

    w->data = e;
    w->text = strdup("");
    if (!e || !w->text)
        return;
    e->cap = 1;
    e->lines_dirty = true;
    e->multiline = !strcmp(w->tag, "textarea");
    e->password = !strcmp(w->tag, "password");
    e->spin = !strcmp(w->tag, "spin");
    if (e->spin) {
        w->min = -1e300;
        w->max = 1e300;
        free(w->text);
        w->text = strdup("0");
        e->len = 1;
        e->cap = 2;
    }
}

static void edit_text_changed(struct widget *w)
{
    struct edit *e = E(w);

    e->len = strlen(w->text);
    e->cap = e->len + 1;
    e->cursor = e->anchor = e->multiline ? 0 : e->len;
    e->sx = e->sy = 0;
    e->lines_dirty = true;
    e->modified = false;
    while (e->nundo)
        snapshot_free(&e->undo[--e->nundo]);
    while (e->nredo)
        snapshot_free(&e->redo[--e->nredo]);
    if (e->spin) {
        double v = strtod(w->text, NULL);

        w->value = v;
    }
}

static void edit_value_changed(struct widget *w)
{
    struct edit *e = E(w);
    char buf[64];

    if (!e->spin)
        return;
    snprintf(buf, sizeof(buf), "%.10g", w->value);
    if (strcmp(buf, w->text)) {
        ui_set_text(w, buf);
        e->modified = true;
    }
}

static void edit_attr(struct widget *w, const char *name, const char *value)
{
    struct edit *e = E(w);

    if (!e)
        return;
    if (!strcmp(name, "readonly"))
        e->readonly = attr_bool(value);
    else if (!strcmp(name, "wrap"))
        e->wrap = attr_bool(value), e->lines_dirty = true;
    else if (!strcmp(name, "mono"))
        e->mono = attr_bool(value), e->lines_dirty = true;
    else if (!strcmp(name, "type") && !strcmp(value, "password"))
        e->password = true;
}

static void edit_focus(struct widget *w, bool focused)
{
    struct edit *e = E(w);

    if (focused && !e->multiline && w->win && w->win->focus_visible) {
        // Tabbing into a field selects its contents.
        e->anchor = 0;
        e->cursor = e->len;
    }
    if (!focused)
        e->dragging = false;
}

static void edit_free(struct widget *w)
{
    struct edit *e = E(w);

    if (!e)
        return;
    while (e->nundo)
        snapshot_free(&e->undo[--e->nundo]);
    while (e->nredo)
        snapshot_free(&e->redo[--e->nredo]);
    free(e->lines);
    free(e);
}

// Runs an editing command: undo, redo, cut, copy, paste, selectall, delete.
void ui_edit_command(struct widget *w, const char *cmd)
{
    struct edit *e;

    if (!w || w->cls->measure != edit_measure)
        return;
    e = E(w);
    if (!strcmp(cmd, "selectall")) {
        e->anchor = 0;
        move_to(w, e->len, true);
    } else if (!strcmp(cmd, "copy")) {
        copy_selection(w);
    } else if (!strcmp(cmd, "cut")) {
        copy_selection(w);
        if (has_selection(e) && !e->password)
            replace(w, sel_start(e), sel_end(e), "", 0, 0);
    } else if (!strcmp(cmd, "paste")) {
        insert_text(w, ui_clipboard_get(), 0);
    } else if (!strcmp(cmd, "delete")) {
        if (has_selection(e))
            replace(w, sel_start(e), sel_end(e), "", 0, 0);
    } else if (!strcmp(cmd, "undo")) {
        restore(w, e->undo, &e->nundo, e->redo, &e->nredo);
    } else if (!strcmp(cmd, "redo")) {
        restore(w, e->redo, &e->nredo, e->undo, &e->nundo);
    }
}

int ui_textarea_selection(struct widget *w, int *start, int *end)
{
    struct edit *e;

    if (!w || w->cls->measure != edit_measure)
        return -1;
    e = E(w);
    *start = sel_start(e);
    *end = sel_end(e);
    return 0;
}

void ui_textarea_insert(struct widget *w, const char *text)
{
    if (w && w->cls->measure == edit_measure)
        insert_text(w, text, 0);
}

int ui_textarea_cursor(struct widget *w)
{
    return w && w->data && w->cls->measure == edit_measure ? E(w)->cursor : 0;
}

void ui_textarea_select(struct widget *w, int start, int end)
{
    struct edit *e;

    if (!w || w->cls->measure != edit_measure)
        return;
    e = E(w);
    start = MIN(MAX(start, 0), e->len);
    end = MIN(MAX(end, 0), e->len);
    e->anchor = start;
    e->cursor = end;
    ensure_visible(w);
    ui_redraw(w);
}

bool ui_textarea_modified(struct widget *w)
{
    return w && w->cls->measure == edit_measure && E(w)->modified;
}

void ui_textarea_set_modified(struct widget *w, bool modified)
{
    if (w && w->cls->measure == edit_measure)
        E(w)->modified = modified;
}

const struct wclass ui_input_class = { "input", true, WM_CURSOR_TEXT, .init = edit_init, .measure = edit_measure,
                                       .paint = edit_paint, .pointer = edit_pointer, .key = edit_key,
                                       .attr = edit_attr, .text_changed = edit_text_changed,
                                       .value_changed = edit_value_changed, .focus = edit_focus, .free = edit_free };
const struct wclass ui_password_class = { "password", true, WM_CURSOR_TEXT, .init = edit_init,
                                          .measure = edit_measure, .paint = edit_paint, .pointer = edit_pointer,
                                          .key = edit_key, .attr = edit_attr, .text_changed = edit_text_changed,
                                          .focus = edit_focus, .free = edit_free };
const struct wclass ui_spin_class = { "spin", true, WM_CURSOR_TEXT, .init = edit_init, .measure = edit_measure,
                                      .paint = edit_paint, .pointer = edit_pointer, .key = edit_key,
                                      .attr = edit_attr, .text_changed = edit_text_changed,
                                      .value_changed = edit_value_changed, .focus = edit_focus, .free = edit_free };
const struct wclass ui_textarea_class = { "textarea", true, WM_CURSOR_TEXT, .init = edit_init,
                                          .measure = edit_measure, .paint = edit_paint, .pointer = edit_pointer,
                                          .key = edit_key, .attr = edit_attr, .text_changed = edit_text_changed,
                                          .focus = edit_focus, .free = edit_free };
