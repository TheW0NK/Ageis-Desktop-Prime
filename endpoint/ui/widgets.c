#include "ui_internal.h"

// Containers and the basic controls.

static bool horizontal(struct widget *w)
{
    return w && (!strcmp(w->tag, "hbox") || !strcmp(w->tag, "toolbar") || !strcmp(w->tag, "statusbar"));
}

static int group_title_h(struct widget *w)
{
    return !strcmp(w->tag, "group") && w->text && *w->text ? font_line_height(ui_font_bold()) + 4 : 0;
}

// ---- Boxes ----

static void box_measure(struct widget *w, int avail, int *pw, int *ph)
{
    bool h = horizontal(w);
    int pad = w->padding, inner = avail >= 0 ? MAX(0, avail - 2 * pad) : -1, sum = 0, mx = 0, n = 0;

    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible)
            continue;
        ui_measure(c, h ? -1 : inner);
        sum += h ? c->pref_w : c->pref_h;
        mx = MAX(mx, h ? c->pref_h : c->pref_w);
        n++;
    }
    if (n > 1)
        sum += w->spacing * (n - 1);
    *pw = (h ? sum : mx) + 2 * pad;
    *ph = (h ? mx : sum) + 2 * pad + group_title_h(w);
    if (group_title_h(w))
        *pw = MAX(*pw, text_width(ui_font_bold(), w->text, -1) + 2 * pad);
}

static void box_arrange(struct widget *w)
{
    bool h = horizontal(w);
    int top = group_title_h(w);
    struct rect in = { w->r.x + w->padding, w->r.y + w->padding + top, MAX(0, w->r.w - 2 * w->padding),
                       MAX(0, w->r.h - 2 * w->padding - top) };
    int n = 0, total = 0, weights = 0, extra, pos, main_len = h ? in.w : in.h, cross_len = h ? in.h : in.w;
    int shrinkable = 0;
    const char *justify = ui_attr(w, "justify");

    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible)
            continue;
        if (!h)
            ui_measure(c, in.w);
        total += h ? c->pref_w : c->pref_h;
        weights += c->expand;
        if (c->expand)
            shrinkable += h ? c->pref_w : c->pref_h;
        n++;
    }
    if (n > 1)
        total += w->spacing * (n - 1);
    extra = main_len - total;
    pos = h ? in.x : in.y;
    if (extra > 0 && !weights && justify) {
        if (!strcmp(justify, "center"))
            pos += extra / 2;
        else if (!strcmp(justify, "end"))
            pos += extra;
    }
    for (struct widget *c = w->first; c; c = c->next) {
        int len, cross, off = 0, pref_cross;
        struct rect r;

        if (!c->visible) {
            c->r = (struct rect){ 0, 0, 0, 0 };
            continue;
        }
        len = h ? c->pref_w : c->pref_h;
        // A fixed size along the box's direction wins over expand.
        if (c->expand && (h ? c->fixed_w : c->fixed_h) >= 0) {
            weights -= c->expand;
            shrinkable -= len;
        } else if (c->expand && extra > 0 && weights) {
            int share = extra * c->expand / weights;

            extra -= share;
            weights -= c->expand;
            len += share;
        } else if (c->expand && extra < 0 && shrinkable > 0) {
            // Too little room: the expanding children give up space first.
            int give = (int)((int64_t)-extra * len / shrinkable);

            give = MIN(give, len - (h ? c->min_w : c->min_h));
            extra += give;
            shrinkable -= len;
            len -= MAX(give, 0);
        }
        pref_cross = h ? c->pref_h : c->pref_w;
        if (c->align == ALIGN_STRETCH && (h ? c->fixed_h : c->fixed_w) < 0) {
            cross = cross_len;
        } else {
            cross = MIN(pref_cross, cross_len);
            if (c->align == ALIGN_CENTER)
                off = (cross_len - cross) / 2;
            else if (c->align == ALIGN_END)
                off = cross_len - cross;
        }
        r = h ? (struct rect){ pos, in.y + off, len, cross } : (struct rect){ in.x + off, pos, cross, len };
        ui_place(c, r);
        pos += len + w->spacing;
    }
}

static void box_init(struct widget *w)
{
    if (!strcmp(w->tag, "vbox") || !strcmp(w->tag, "hbox") || !strcmp(w->tag, "box"))
        w->spacing = ui_theme.spacing;
    else if (!strcmp(w->tag, "group"))
        w->padding = 10, w->spacing = ui_theme.spacing;
    else if (!strcmp(w->tag, "toolbar"))
        w->padding = 4, w->spacing = 4;
    else if (!strcmp(w->tag, "statusbar"))
        w->padding = 4, w->spacing = 12;
    else if (!strcmp(w->tag, "tab"))
        w->padding = 12, w->spacing = ui_theme.spacing;
    else if (!strcmp(w->tag, "card"))
        w->padding = 24, w->spacing = 12;
}

static void box_paint(struct widget *w, struct gfx *g)
{
    if (!strcmp(w->tag, "toolbar")) {
        gfx_fill(g, w->r, ui_theme.surface_alt);
        gfx_fill(g, (struct rect){ w->r.x, w->r.y + w->r.h - 1, w->r.w, 1 }, ui_theme.border);
    } else if (!strcmp(w->tag, "statusbar")) {
        gfx_fill(g, w->r, ui_theme.surface_alt);
        gfx_fill(g, (struct rect){ w->r.x, w->r.y, w->r.w, 1 }, ui_theme.border);
    } else if (!strcmp(w->tag, "group")) {
        int th = group_title_h(w);
        struct rect frame = { w->r.x, w->r.y + th / 2, w->r.w, w->r.h - th / 2 };

        gfx_fill_rounded(g, frame, ui_theme.radius, ui_theme.surface);
        gfx_outline_rounded(g, frame, ui_theme.radius, 1, ui_theme.border);
        if (th) {
            struct font *f = ui_font_bold();
            int tw = text_width(f, w->text, -1);

            gfx_fill(g, (struct rect){ w->r.x + 8, w->r.y, tw + 8, th }, ui_theme.window);
            text_draw(g, f, w->r.x + 12, w->r.y + (th - font_line_height(f)) / 2, w->text, -1, ui_theme.text);
        }
    } else if (!strcmp(w->tag, "card")) {
        int rad = ui_theme.radius * 2;

        gfx_shadow(g, w->r, rad, 18, ui_theme.shadow);
        gfx_fill_rounded(g, w->r, rad, ALPHA(ui_theme.surface, 0xF0));
        gfx_outline_rounded(g, w->r, rad, 1, ALPHA(ui_theme.border, 0x80));
    } else if (ui_attr(w, "background") && !strcmp(ui_attr(w, "background"), "surface")) {
        gfx_fill(g, w->r, ui_theme.surface);
    }
}

static const struct wclass window_class = { "window", .init = box_init, .measure = box_measure,
                                            .arrange = box_arrange, .paint = box_paint };
static const struct wclass box_class = { "box", .init = box_init, .measure = box_measure, .arrange = box_arrange,
                                         .paint = box_paint };
static const struct wclass vbox_class = { "vbox", .init = box_init, .measure = box_measure,
                                          .arrange = box_arrange, .paint = box_paint };
static const struct wclass hbox_class = { "hbox", .init = box_init, .measure = box_measure,
                                          .arrange = box_arrange, .paint = box_paint };
static const struct wclass group_class = { "group", .init = box_init, .measure = box_measure,
                                           .arrange = box_arrange, .paint = box_paint };
static const struct wclass toolbar_class = { "toolbar", .init = box_init, .measure = box_measure,
                                             .arrange = box_arrange, .paint = box_paint };
static const struct wclass statusbar_class = { "statusbar", .init = box_init, .measure = box_measure,
                                               .arrange = box_arrange, .paint = box_paint };
static const struct wclass card_class = { "card", .init = box_init, .measure = box_measure,
                                          .arrange = box_arrange, .paint = box_paint };
static const struct wclass tab_class = { "tab", .init = box_init, .measure = box_measure, .arrange = box_arrange,
                                         .paint = box_paint };

// ---- Grid ----

#define GRID_MAX 32

static void grid_sizes(struct widget *w, int *cols, int *colw, int *nrows)
{
    const char *c = ui_attr(w, "columns");
    int n = MIN(MAX(c ? atoi(c) : 2, 1), GRID_MAX), i = 0;

    *cols = n;
    memset(colw, 0, sizeof(int) * GRID_MAX);
    for (struct widget *ch = w->first; ch; ch = ch->next) {
        if (!ch->visible)
            continue;
        ui_measure(ch, -1);
        colw[i % n] = MAX(colw[i % n], ch->pref_w);
        i++;
    }
    *nrows = (i + n - 1) / n;
}

static void grid_rows(struct widget *w, int cols, int *rowh, int nrows)
{
    int i = 0;

    memset(rowh, 0, sizeof(int) * nrows);
    for (struct widget *ch = w->first; ch; ch = ch->next) {
        if (!ch->visible)
            continue;
        rowh[i / cols] = MAX(rowh[i / cols], ch->pref_h);
        i++;
    }
}

static void grid_measure(struct widget *w, int avail, int *pw, int *ph)
{
    int cols, colw[GRID_MAX], nrows, *rowh, sw = 0, sh = 0;

    (void)avail;
    grid_sizes(w, &cols, colw, &nrows);
    rowh = calloc(MAX(nrows, 1), sizeof(int));
    if (!rowh)
        return;
    grid_rows(w, cols, rowh, nrows);
    for (int i = 0; i < cols; i++)
        sw += colw[i];
    for (int i = 0; i < nrows; i++)
        sh += rowh[i];
    *pw = sw + w->spacing * (cols - 1) + 2 * w->padding;
    *ph = sh + w->spacing * MAX(nrows - 1, 0) + 2 * w->padding;
    free(rowh);
}

static void grid_arrange(struct widget *w)
{
    int cols, colw[GRID_MAX], nrows, *rowh, sw = 0, extra, i = 0, x, y;
    bool stretch[GRID_MAX] = { 0 };
    int nstretch = 0;
    const char *s = ui_attr(w, "stretch");

    grid_sizes(w, &cols, colw, &nrows);
    if (!(rowh = calloc(MAX(nrows, 1), sizeof(int))))
        return;
    grid_rows(w, cols, rowh, nrows);
    for (int c = 0; c < cols; c++)
        sw += colw[c];
    if (s) {
        for (const char *p = s; *p;) {
            int c = atoi(p);

            if (c >= 0 && c < cols && !stretch[c])
                stretch[c] = true, nstretch++;
            while (*p && *p != ',')
                p++;
            if (*p)
                p++;
        }
    } else {
        stretch[cols - 1] = true;
        nstretch = 1;
    }
    extra = w->r.w - 2 * w->padding - sw - w->spacing * (cols - 1);
    for (int c = 0; c < cols && extra > 0 && nstretch; c++) {
        if (stretch[c]) {
            int share = extra / nstretch;

            colw[c] += share;
            extra -= share;
            nstretch--;
        }
    }
    y = w->r.y + w->padding;
    x = w->r.x + w->padding;
    for (struct widget *ch = w->first; ch; ch = ch->next) {
        int c = i % cols, row = i / cols, cw, chh, ox = 0, oy = 0;

        if (!ch->visible)
            continue;
        if (c == 0 && i) {
            y += rowh[row - 1] + w->spacing;
            x = w->r.x + w->padding;
        }
        cw = colw[c];
        chh = rowh[row];
        if (ch->align != ALIGN_STRETCH) {
            int pw = MIN(ch->pref_w, cw);

            ox = ch->align == ALIGN_CENTER ? (cw - pw) / 2 : ch->align == ALIGN_END ? cw - pw : 0;
            cw = pw;
        }
        ui_place(ch, (struct rect){ x + ox, y + oy, cw, chh });
        x += colw[c] + w->spacing;
        i++;
    }
    free(rowh);
}

static void grid_init(struct widget *w)
{
    w->spacing = ui_theme.spacing;
}

static const struct wclass grid_class = { "grid", .init = grid_init, .measure = grid_measure,
                                          .arrange = grid_arrange };

// ---- Stack and tabs ----

static void stack_measure(struct widget *w, int avail, int *pw, int *ph)
{
    int inner = avail >= 0 ? MAX(0, avail - 2 * w->padding) : -1;

    *pw = *ph = 0;
    for (struct widget *c = w->first; c; c = c->next) {
        bool v = c->visible;

        // Size for the largest page so switching does not resize the window.
        c->visible = true;
        ui_measure(c, inner);
        c->visible = v;
        *pw = MAX(*pw, c->pref_w);
        *ph = MAX(*ph, c->pref_h);
    }
    *pw += 2 * w->padding;
    *ph += 2 * w->padding;
}

static void stack_sync(struct widget *w)
{
    int i = 0;

    for (struct widget *c = w->first; c; c = c->next, i++)
        c->visible = i == (int)w->value;
}

static void stack_arrange_in(struct widget *w, struct rect in)
{
    stack_sync(w);
    for (struct widget *c = w->first; c; c = c->next) {
        if (c->visible) {
            ui_measure(c, in.w);
            ui_place(c, in);
        }
    }
}

static void stack_arrange(struct widget *w)
{
    stack_arrange_in(w, (struct rect){ w->r.x + w->padding, w->r.y + w->padding, w->r.w - 2 * w->padding,
                                       w->r.h - 2 * w->padding });
}

static void stack_value_changed(struct widget *w)
{
    stack_sync(w);
    if (w->win) {
        ui_relayout(w->win);
        if (w->win->focus && !ui_widget_shown(w->win->focus))
            w->win->focus = NULL;
    }
}

static void stack_init(struct widget *w)
{
    w->max = 1e9;
}

static const struct wclass stack_class = { "stack", .init = stack_init, .measure = stack_measure,
                                           .arrange = stack_arrange, .value_changed = stack_value_changed };

static const char *tab_title(struct widget *c)
{
    const char *t = ui_attr(c, "title");

    return t ? ui_translate(t) : c->text ? c->text : "";
}

static int tabs_header_h(void)
{
    return ui_theme.row_height + 6;
}

static int tab_at(struct widget *w, int x, int y)
{
    struct font *f = ui_font();
    int tx = w->r.x + 8, i = 0;

    if (y < w->r.y || y >= w->r.y + tabs_header_h())
        return -1;
    for (struct widget *c = w->first; c; c = c->next, i++) {
        int tw = text_width(f, tab_title(c), -1) + 28;

        if (x >= tx && x < tx + tw)
            return i;
        tx += tw;
    }
    return -1;
}

static void tabs_measure(struct widget *w, int avail, int *pw, int *ph)
{
    struct font *f = ui_font();
    int hw = 16;

    stack_measure(w, avail, pw, ph);
    for (struct widget *c = w->first; c; c = c->next)
        hw += text_width(f, tab_title(c), -1) + 28;
    *pw = MAX(*pw, hw);
    *ph += tabs_header_h();
}

static void tabs_arrange(struct widget *w)
{
    int hh = tabs_header_h();

    stack_arrange_in(w, (struct rect){ w->r.x + 1, w->r.y + hh, w->r.w - 2, w->r.h - hh - 1 });
}

static void tabs_paint(struct widget *w, struct gfx *g)
{
    struct font *f = ui_font();
    int hh = tabs_header_h(), tx = w->r.x + 8, i = 0;
    struct rect body = { w->r.x, w->r.y + hh - 1, w->r.w, w->r.h - hh + 1 };

    gfx_fill(g, body, ui_theme.surface);
    gfx_outline(g, body, 1, ui_theme.border);
    for (struct widget *c = w->first; c; c = c->next, i++) {
        const char *t = tab_title(c);
        int tw = text_width(f, t, -1) + 28;
        struct rect tr = { tx, w->r.y + 4, tw, hh - 4 };
        bool sel = i == (int)w->value;

        if (sel) {
            gfx_fill(g, (struct rect){ tr.x, tr.y, tr.w, tr.h }, ui_theme.surface);
            gfx_outline(g, (struct rect){ tr.x, tr.y, tr.w, tr.h }, 1, ui_theme.border);
            gfx_fill(g, (struct rect){ tr.x + 1, tr.y + tr.h - 1, tr.w - 2, 2 }, ui_theme.surface);
            gfx_fill(g, (struct rect){ tr.x, tr.y, tr.w, 2 }, ui_theme.accent);
        }
        ui_draw_text(g, sel ? ui_font_bold() : f, tr, t, sel ? ui_theme.text : ui_theme.text_dim, ALIGN_CENTER);
        if (sel && ui_is_focused(w) && w->win->focus_visible)
            ui_draw_focus(g, (struct rect){ tr.x + 3, tr.y + 3, tr.w - 6, tr.h - 6 }, 3);
        tx += tw;
    }
}

static void tabs_select(struct widget *w, int i)
{
    if (i < 0 || i >= ui_child_count(w) || i == (int)w->value)
        return;
    ui_set_value(w, i);
    ui_redraw(w);
    ui_emit(w, "change");
}

static bool tabs_pointer(struct widget *w, struct wm_event *ev)
{
    int i;

    if (ev->kind != WM_PTR_DOWN || ev->detail != BTN_LEFT)
        return false;
    if ((i = tab_at(w, ev->x, ev->y)) < 0)
        return false;
    tabs_select(w, i);
    return true;
}

static bool tabs_key(struct widget *w, struct wm_event *ev)
{
    if (!ev->value)
        return false;
    if (ev->key == KEY_LEFT && w->win->focus == w)
        tabs_select(w, (int)w->value - 1);
    else if (ev->key == KEY_RIGHT && w->win->focus == w)
        tabs_select(w, (int)w->value + 1);
    else if (ev->key == KEY_TAB && (ev->mods & MOD_CTRL)) {
        int n = ui_child_count(w);

        if (n)
            tabs_select(w, ((int)w->value + ((ev->mods & MOD_SHIFT) ? n - 1 : 1)) % n);
    } else {
        return false;
    }
    return true;
}

static const struct wclass tabs_class = { "tabs", true, .init = stack_init, .measure = tabs_measure,
                                          .arrange = tabs_arrange, .paint = tabs_paint, .pointer = tabs_pointer,
                                          .key = tabs_key, .value_changed = stack_value_changed };

// ---- Scroll view ----

#define BAR_W 12

struct scroll {
    int offset;                     // first, for scroll_into_view in core.c
    int content_h;
    bool dragging, bar_hot;
    int grab;
};

static void scroll_init(struct widget *w)
{
    w->data = calloc(1, sizeof(struct scroll));
    w->expand = 1;
}

static void scroll_measure(struct widget *w, int avail, int *pw, int *ph)
{
    int bw, bh;

    box_measure(w, avail >= 0 ? avail - BAR_W : -1, &bw, &bh);
    *pw = bw + BAR_W;
    *ph = MIN(bh, 160);
}

static struct rect bar_rect(struct widget *w)
{
    return (struct rect){ w->r.x + w->r.w - BAR_W, w->r.y + 2, BAR_W, w->r.h - 4 };
}

static void scroll_arrange(struct widget *w)
{
    struct scroll *s = w->data;
    struct rect saved = w->r;
    int pw, ph, cw = w->r.w;

    box_measure(w, cw, &pw, &ph);
    if (ph > w->r.h) {
        cw -= BAR_W;
        box_measure(w, cw, &pw, &ph);
    }
    s->content_h = ph;
    s->offset = MIN(MAX(s->offset, 0), MAX(0, ph - w->r.h));
    w->r = (struct rect){ saved.x, saved.y - s->offset, cw, MAX(ph, saved.h) };
    box_arrange(w);
    w->r = saved;
}

static void scroll_paint(struct widget *w, struct gfx *g)
{
    struct scroll *s = w->data;
    struct gfx saved = *g;

    ui_clip(g, w->r);
    ui_paint_children(w, g);
    *g = saved;
    if (s->content_h > w->r.h)
        ui_draw_scrollbar(g, bar_rect(w), s->content_h, w->r.h, s->offset, s->bar_hot || s->dragging);
}

static void scroll_to(struct widget *w, int offset)
{
    struct scroll *s = w->data;

    offset = MIN(MAX(offset, 0), MAX(0, s->content_h - w->r.h));
    if (offset == s->offset)
        return;
    s->offset = offset;
    ui_place(w, w->r);
    ui_redraw(w);
}

static bool scroll_pointer(struct widget *w, struct wm_event *ev)
{
    struct scroll *s = w->data;
    struct rect bar = bar_rect(w);
    bool has_bar = s->content_h > w->r.h;

    switch (ev->kind) {
    case WM_PTR_WHEEL: {
        int old = s->offset;

        scroll_to(w, s->offset - ev->detail * 48);
        return old != s->offset;
    }
    case WM_PTR_DOWN:
        if (has_bar && ev->detail == BTN_LEFT && rect_contains(bar, ev->x, ev->y)) {
            int th, ty = ui_scrollbar_thumb(bar, s->content_h, w->r.h, s->offset, &th);

            s->grab = ev->y >= ty && ev->y < ty + th ? ev->y - ty : th / 2;
            s->dragging = true;
            scroll_to(w, ui_scrollbar_offset(bar, s->content_h, w->r.h, ev->y, s->grab));
            ui_redraw(w);
            return true;
        }
        return false;
    case WM_PTR_MOVE: {
        bool hot = has_bar && rect_contains(bar, ev->x, ev->y);

        if (s->dragging) {
            scroll_to(w, ui_scrollbar_offset(bar, s->content_h, w->r.h, ev->y, s->grab));
            return true;
        }
        if (hot != s->bar_hot) {
            s->bar_hot = hot;
            ui_redraw(w);
        }
        return false;
    }
    case WM_PTR_UP:
        if (s->dragging) {
            s->dragging = false;
            ui_redraw(w);
            return true;
        }
        return false;
    }
    return false;
}

static bool scroll_key(struct widget *w, struct wm_event *ev)
{
    struct scroll *s = w->data;

    if (!ev->value)
        return false;
    switch (ev->key) {
    case KEY_PAGEDOWN: scroll_to(w, s->offset + w->r.h - 40); return true;
    case KEY_PAGEUP:   scroll_to(w, s->offset - (w->r.h - 40)); return true;
    }
    return false;
}

static void scroll_free(struct widget *w)
{
    free(w->data);
}

static const struct wclass scroll_class = { "scroll", .init = scroll_init, .measure = scroll_measure,
                                            .arrange = scroll_arrange, .paint = scroll_paint,
                                            .pointer = scroll_pointer, .key = scroll_key, .free = scroll_free,
                                            .paints_children = true };

// ---- Labels ----

struct label {
    int px;
    bool bold, mono, dim, wrap;
};

static void label_init(struct widget *w)
{
    struct label *l = calloc(1, sizeof(*l));

    w->data = l;
    if (!l)
        return;
    if (!strcmp(w->tag, "h1"))
        l->px = ui_theme.font_size + 10, l->bold = true;
    else if (!strcmp(w->tag, "h2"))
        l->px = ui_theme.font_size + 4, l->bold = true;
    else if (!strcmp(w->tag, "p"))
        l->wrap = true;
    if (!strcmp(w->tag, "link")) {
        w->focusable = true;
    }
}

static void label_attr(struct widget *w, const char *name, const char *value)
{
    struct label *l = w->data;

    if (!l)
        return;
    if (!strcmp(name, "size")) {
        if (!strcmp(value, "small"))
            l->px = ui_theme.font_size - 2;
        else if (!strcmp(value, "large"))
            l->px = ui_theme.font_size + 4;
        else if (!strcmp(value, "title"))
            l->px = ui_theme.font_size + 10;
        else if (!strcmp(value, "huge"))
            l->px = ui_theme.font_size + 26;
        else
            l->px = atoi(value);
    } else if (!strcmp(name, "bold")) {
        l->bold = attr_bool(value);
    } else if (!strcmp(name, "mono")) {
        l->mono = attr_bool(value);
    } else if (!strcmp(name, "dim")) {
        l->dim = attr_bool(value);
    } else if (!strcmp(name, "wrap")) {
        l->wrap = attr_bool(value);
    }
}

static struct font *label_font(struct widget *w)
{
    struct label *l = w->data;
    int px = l && l->px > 0 ? l->px : ui_theme.font_size;

    if (l && l->mono)
        return ui_font_sized(l->bold ? FONT_MONO_BOLD : ui_theme.font_mono, px);
    return ui_font_sized(l && l->bold ? ui_theme.font_bold : ui_theme.font, px);
}

static bool breakable(uint32_t cp)
{
    // CJK text has no spaces: any character boundary will do.
    return cp >= 0x2E80;
}

// Calls fn for each line of text wrapped to width (width < 0: no wrapping).
static int wrap_lines(struct font *f, const char *s, int width,
                      void (*fn)(void *, const char *, int), void *ctx)
{
    int lines = 0;

    while (true) {
        const char *nl = strchr(s, '\n');
        int len = nl ? nl - s : (int)strlen(s);

        if (width < 0 || text_width(f, s, len) <= width) {
            if (fn)
                fn(ctx, s, len);
            lines++;
        } else {
            const char *p = s;

            while (p < s + len) {
                int rest = s + len - p, fit = 0, brk = -1, pos = 0;

                while (pos < rest) {
                    const char *q = p + pos;
                    uint32_t cp = utf8_decode(&q);
                    int next = q - p;

                    if (text_width(f, p, next) > width)
                        break;
                    if (cp == ' ')
                        brk = next;
                    else if (breakable(cp))
                        brk = next;
                    fit = next;
                    pos = next;
                }
                if (pos >= rest) {
                    brk = rest;
                } else if (brk <= 0) {
                    brk = fit > 0 ? fit : utf8_next(p, 0);
                }
                {
                    int shown = brk;

                    while (shown > 0 && p[shown - 1] == ' ')
                        shown--;
                    if (fn)
                        fn(ctx, p, shown);
                }
                lines++;
                p += brk;
                while (p < s + len && *p == ' ')
                    p++;
            }
        }
        if (!nl)
            break;
        s = nl + 1;
    }
    return lines;
}

struct widest {
    struct font *f;
    int w;
};

static void widest_line(void *ctx, const char *s, int len)
{
    struct widest *wd = ctx;

    wd->w = MAX(wd->w, text_width(wd->f, s, len));
}

static void label_measure(struct widget *w, int avail, int *pw, int *ph)
{
    struct label *l = w->data;
    struct font *f = label_font(w);
    struct widest wd = { f, 0 };
    const char *t = w->text ? w->text : "";
    int lines = wrap_lines(f, t, l && l->wrap && avail > 0 ? avail : -1, widest_line, &wd);

    *pw = wd.w + (!strcmp(w->tag, "link") ? 2 : 0);
    *ph = lines * font_line_height(f);
    if (!strcmp(w->tag, "h1") || !strcmp(w->tag, "h2"))
        *ph += 4;
}

struct paint_lines {
    struct gfx *g;
    struct font *f;
    struct widget *w;
    int y;
    color_t c;
};

static void paint_line(void *ctx, const char *s, int len)
{
    struct paint_lines *p = ctx;
    struct widget *w = p->w;
    int tw = text_width(p->f, s, len), x = w->r.x;

    if (w->halign == ALIGN_CENTER)
        x += (w->r.w - tw) / 2;
    else if (w->halign == ALIGN_END)
        x += w->r.w - tw;
    text_draw(p->g, p->f, x, p->y, s, len, p->c);
    if (!strcmp(w->tag, "link") && w->hover)
        gfx_fill(p->g, (struct rect){ x, p->y + font_ascent(p->f) + 2, tw, 1 }, p->c);
    p->y += font_line_height(p->f);
}

static void label_paint(struct widget *w, struct gfx *g)
{
    struct label *l = w->data;
    struct font *f = label_font(w);
    const char *t = w->text ? w->text : "";
    int width = l && l->wrap ? w->r.w : -1, lines = wrap_lines(f, t, width, NULL, NULL);
    int h = lines * font_line_height(f);
    struct paint_lines p = { g, f, w, w->r.y + MAX(0, (w->r.h - h) / 2), ui_theme.text };
    struct gfx saved = *g;

    if (l && l->dim)
        p.c = ui_theme.text_dim;
    if (!strcmp(w->tag, "link"))
        p.c = ui_theme.accent;
    if (!w->enabled)
        p.c = ui_mix(p.c, ui_theme.window, 120);
    ui_clip(g, w->r);
    wrap_lines(f, t, width, paint_line, &p);
    *g = saved;
    if (!strcmp(w->tag, "link") && ui_is_focused(w) && w->win->focus_visible)
        ui_draw_focus(g, w->r, 3);
}

static bool link_pointer(struct widget *w, struct wm_event *ev)
{
    if (strcmp(w->tag, "link"))
        return false;
    if (ev->kind == WM_PTR_DOWN && ev->detail == BTN_LEFT) {
        w->pressed = true;
        return true;
    }
    if (ev->kind == WM_PTR_UP && w->pressed) {
        w->pressed = false;
        if (rect_contains(w->r, ev->x, ev->y))
            ui_emit(w, "click");
        return true;
    }
    return false;
}

static bool link_key(struct widget *w, struct wm_event *ev)
{
    if (ev->value == 1 && (ev->key == KEY_ENTER || ev->key == KEY_SPACE)) {
        ui_emit(w, "click");
        return true;
    }
    return false;
}

static void free_data(struct widget *w)
{
    free(w->data);
}

static const struct wclass label_class = { "label", .init = label_init, .measure = label_measure,
                                           .paint = label_paint, .attr = label_attr, .free = free_data };
static const struct wclass h1_class = { "h1", .init = label_init, .measure = label_measure, .paint = label_paint,
                                        .attr = label_attr, .free = free_data };
static const struct wclass h2_class = { "h2", .init = label_init, .measure = label_measure, .paint = label_paint,
                                        .attr = label_attr, .free = free_data };
static const struct wclass p_class = { "p", .init = label_init, .measure = label_measure, .paint = label_paint,
                                       .attr = label_attr, .free = free_data };
static const struct wclass link_class = { "link", true, WM_CURSOR_HAND, .init = label_init,
                                          .measure = label_measure, .paint = label_paint, .pointer = link_pointer,
                                          .key = link_key, .attr = label_attr, .free = free_data };

// ---- Buttons ----

struct button {
    struct surface *icon;
};

static void button_init(struct widget *w)
{
    w->data = calloc(1, sizeof(struct button));
}

static void button_attr(struct widget *w, const char *name, const char *value)
{
    struct button *b = w->data;

    if (b && !strcmp(name, "icon")) {
        surface_destroy(b->icon);
        b->icon = *value ? image_load(value) : NULL;
    }
}

static int icon_size(void)
{
    return ui_theme.font_size + 4;
}

static bool is_tile(struct widget *w)
{
    const char *t = ui_attr(w, "tile");

    return t && attr_bool(t);
}

// The button's picture: an image file (icon), an app tile (appicon) or a
// symbol drawn in the text colour (symbol).
static bool has_picture(struct widget *w)
{
    struct button *b = w->data;

    return (b && b->icon) || ui_attr(w, "appicon") || ui_attr(w, "symbol");
}

static void draw_picture(struct widget *w, struct gfx *g, struct rect r, color_t c)
{
    struct button *b = w->data;
    const char *app = ui_attr(w, "appicon"), *sym = ui_attr(w, "symbol");

    if (b && b->icon)
        gfx_blit_scaled(g, b->icon, (struct rect){ 0, 0, b->icon->width, b->icon->height }, r);
    else if (app)
        icon_draw(g, app, r);
    else if (sym)
        icon_draw_glyph(g, sym, r, c);
}

static int tile_icon(struct widget *w)
{
    const char *s = ui_attr(w, "iconsize");

    return s ? atoi(s) : 40;
}

static void button_measure(struct widget *w, int avail, int *pw, int *ph)
{
    const char *t = w->text ? w->text : "";
    const char *sz = ui_attr(w, "iconsize");
    int tw = *t ? text_width(ui_font(), t, -1) : 0, iw = has_picture(w) ? (sz ? atoi(sz) : icon_size()) : 0;

    (void)avail;
    if (is_tile(w)) {
        *pw = 96;
        *ph = tile_icon(w) + 14 + 2 * font_line_height(ui_font());
        return;
    }
    *pw = tw + iw + (tw && iw ? 8 : 0) + (tw ? 28 : 12);
    if (tw && !attr_bool(ui_attr(w, "flat") ? ui_attr(w, "flat") : "false"))
        *pw = MAX(*pw, 80);
    *ph = MAX(ui_theme.row_height, iw + 10);
}

static void button_paint(struct widget *w, struct gfx *g)
{
    const char *t = w->text ? w->text : "", *flat = ui_attr(w, "flat");
    const char *sz = ui_attr(w, "iconsize");
    bool primary = (ui_attr(w, "primary") && attr_bool(ui_attr(w, "primary")))
                   || (ui_attr(w, "default") && attr_bool(ui_attr(w, "default")));
    color_t c = primary ? ui_theme.accent_text : ui_theme.text;
    struct font *f = ui_font();
    int tw = *t ? text_width(f, t, -1) : 0, isz = has_picture(w) ? (sz ? atoi(sz) : icon_size()) : 0;
    int total = tw + isz + (tw && isz ? 8 : 0), x = w->r.x + (w->r.w - total) / 2;
    struct gfx saved = *g;

    if ((flat && attr_bool(flat)) || is_tile(w)) {
        if (w->hover && w->enabled)
            gfx_fill_rounded(g, w->r, ui_theme.radius, w->pressed ? ui_theme.pressed : ui_theme.hover);
        if ((ui_attr(w, "checked") && attr_bool(ui_attr(w, "checked"))) || w->value)
            gfx_fill_rounded(g, w->r, ui_theme.radius, ui_mix(ui_theme.selection, ui_theme.window, 60));
    } else {
        ui_draw_button_bg(g, w, w->r, primary);
    }
    if (!w->enabled)
        c = ui_mix(c, ui_theme.window, 130);
    ui_clip(g, w->r);
    if (is_tile(w)) {
        int ti = tile_icon(w), lh = font_line_height(f);

        draw_picture(w, g, (struct rect){ w->r.x + (w->r.w - ti) / 2, w->r.y + 6, ti, ti }, c);
        ui_draw_text(g, f, (struct rect){ w->r.x + 4, w->r.y + ti + 10, w->r.w - 8, lh }, t, c, ALIGN_CENTER);
        *g = saved;
        if (ui_is_focused(w) && w->win->focus_visible)
            ui_draw_focus(g, w->r, ui_theme.radius);
        return;
    }
    if (total > w->r.w - 8 && tw) {
        // Too narrow: keep the picture, shorten the text.
        x = w->r.x + 10;
        tw = MAX(0, w->r.w - 20 - isz - (isz ? 8 : 0));
    }
    if (isz) {
        draw_picture(w, g, (struct rect){ x, w->r.y + (w->r.h - isz) / 2, isz, isz }, c);
        x += isz + 8;
    }
    if (tw)
        ui_draw_text(g, f, (struct rect){ x, w->r.y, tw, w->r.h }, t, c, ALIGN_START);
    *g = saved;
    if (ui_is_focused(w) && w->win->focus_visible)
        ui_draw_focus(g, w->r, ui_theme.radius);
}

// Press-and-release behaviour shared by clickable widgets. Returns true when
// a click completed.
static bool press_release(struct widget *w, struct wm_event *ev, bool *handled)
{
    *handled = false;
    if (ev->kind == WM_PTR_DOWN && ev->detail == BTN_LEFT) {
        w->pressed = true;
        ui_redraw(w);
        *handled = true;
    } else if (ev->kind == WM_PTR_MOVE && w->pressed) {
        bool in = rect_contains(w->r, ev->x, ev->y);

        if (in != w->hover) {
            w->hover = in;
            ui_redraw(w);
        }
        *handled = true;
    } else if (ev->kind == WM_PTR_UP && w->pressed && ev->detail == BTN_LEFT) {
        w->pressed = false;
        ui_redraw(w);
        *handled = true;
        return rect_contains(w->r, ev->x, ev->y);
    }
    return false;
}

static bool button_pointer(struct widget *w, struct wm_event *ev)
{
    bool handled;

    // A right click on a button with an oncontext handler (taskbar buttons).
    if (ev->kind == WM_PTR_DOWN && ev->detail == BTN_RIGHT && ui_has_handler(w, "context")) {
        ui_emit(w, "context");
        return true;
    }
    if (press_release(w, ev, &handled)) {
        if (ui_attr(w, "menu")) {
            // A button that opens a menu (by id).
            struct widget *m = ui_get(w->win, ui_attr(w, "menu"));

            if (m)
                ui_menu_open(w, m, w->r.x, w->r.y + w->r.h, w->r.w);
        }
        ui_emit(w, "click");
    }
    return handled;
}

static bool activate_key(struct wm_event *ev)
{
    return ev->value == 1 && (ev->key == KEY_SPACE || ev->key == KEY_ENTER || ev->key == KEY_KPENTER) && !(ev->mods & MOD_KEYS);
}

static bool button_key(struct widget *w, struct wm_event *ev)
{
    if (!activate_key(ev))
        return false;
    ui_emit(w, "click");
    return true;
}

static void button_free(struct widget *w)
{
    struct button *b = w->data;

    if (b)
        surface_destroy(b->icon);
    free(b);
}

static const struct wclass button_class = { "button", true, .init = button_init, .measure = button_measure,
                                            .paint = button_paint, .pointer = button_pointer, .key = button_key,
                                            .attr = button_attr, .free = button_free };

// ---- Check boxes, radio buttons and switches ----

static int mark_size(struct widget *w)
{
    return !strcmp(w->tag, "toggle") ? 36 : 18;
}

static void check_measure(struct widget *w, int avail, int *pw, int *ph)
{
    const char *t = w->text ? w->text : "";

    (void)avail;
    *pw = mark_size(w) + (*t ? 8 + text_width(ui_font(), t, -1) : 0);
    *ph = MAX(ui_theme.row_height - 6, font_line_height(ui_font()));
}

static void check_paint(struct widget *w, struct gfx *g)
{
    int s = 18, y = w->r.y + (w->r.h - s) / 2, x = w->r.x;
    bool on = w->value != 0;
    color_t border = w->hover ? ui_theme.accent : ui_theme.border, tc = w->enabled ? ui_theme.text : ui_theme.text_dim;
    struct rect mark;

    if (!strcmp(w->tag, "toggle")) {
        int tw = 36, th = 20, ty = w->r.y + (w->r.h - th) / 2;
        color_t track = on ? ui_theme.accent : ui_mix(ui_theme.border, ui_theme.surface_alt, 60);

        if (!w->enabled)
            track = ui_mix(track, ui_theme.window, 140);
        mark = (struct rect){ x, ty, tw, th };
        gfx_fill_rounded(g, mark, th / 2, track);
        gfx_circle(g, on ? x + tw - th / 2 : x + th / 2, ty + th / 2.0f, th / 2.0f - 3, RGB(0xFFFFFF));
    } else if (!strcmp(w->tag, "radio")) {
        mark = (struct rect){ x, y, s, s };
        gfx_circle(g, x + s / 2.0f, y + s / 2.0f, s / 2.0f, on ? ui_theme.accent : border);
        gfx_circle(g, x + s / 2.0f, y + s / 2.0f, s / 2.0f - (on ? 5 : 1.2f),
                   on ? ui_theme.accent_text : ui_theme.input);
    } else {
        mark = (struct rect){ x, y, s, s };
        if (on) {
            gfx_fill_rounded(g, mark, 4, w->enabled ? ui_theme.accent : ui_theme.text_dim);
            ui_draw_check(g, x, y, s, ui_theme.accent_text);
        } else {
            gfx_fill_rounded(g, mark, 4, ui_theme.input);
            gfx_outline_rounded(g, mark, 4, 1, border);
        }
    }
    if (w->text && *w->text)
        ui_draw_text(g, ui_font(), (struct rect){ x + mark.w + 8, w->r.y, w->r.w - mark.w - 8, w->r.h }, w->text, tc,
                     ALIGN_START);
    if (ui_is_focused(w) && w->win->focus_visible)
        ui_draw_focus(g, mark, !strcmp(w->tag, "radio") ? s / 2 : 4);
}

static void check_toggle(struct widget *w)
{
    if (!strcmp(w->tag, "radio")) {
        const char *group = ui_attr(w, "group");

        if (w->value)
            return;
        // Clear the other radios in the group (same name, or same parent).
        if (w->parent) {
            for (struct widget *c = w->parent->first; c; c = c->next) {
                const char *cg = ui_attr(c, "group");

                if (c != w && !strcmp(c->tag, "radio")
                    && ((!group && !cg) || (group && cg && !strcmp(group, cg))))
                    ui_set_value(c, 0);
            }
        }
        if (group) {
            // Radios with a group name may live anywhere in the window.
            struct widget *stack[256];
            int n = 0;

            stack[n++] = w->win->root;
            while (n) {
                struct widget *c = stack[--n];

                if (c != w && !strcmp(c->tag, "radio") && ui_attr(c, "group") && !strcmp(ui_attr(c, "group"), group))
                    ui_set_value(c, 0);
                for (struct widget *k = c->first; k && n < 256; k = k->next)
                    stack[n++] = k;
            }
        }
        ui_set_value(w, 1);
    } else {
        ui_set_value(w, w->value ? 0 : 1);
    }
    ui_emit(w, "change");
}

static bool check_pointer(struct widget *w, struct wm_event *ev)
{
    bool handled;

    if (press_release(w, ev, &handled))
        check_toggle(w);
    return handled;
}

static bool check_key(struct widget *w, struct wm_event *ev)
{
    if (ev->value != 1 || ev->key != KEY_SPACE)
        return false;
    check_toggle(w);
    return true;
}

static void check_init(struct widget *w)
{
    w->max = 1;
}

static const struct wclass checkbox_class = { "checkbox", true, .init = check_init, .measure = check_measure,
                                              .paint = check_paint, .pointer = check_pointer, .key = check_key };
static const struct wclass radio_class = { "radio", true, .init = check_init, .measure = check_measure,
                                           .paint = check_paint, .pointer = check_pointer, .key = check_key };
static const struct wclass toggle_class = { "toggle", true, .init = check_init, .measure = check_measure,
                                            .paint = check_paint, .pointer = check_pointer, .key = check_key };

// ---- Sliders and progress bars ----

static double fraction(struct widget *w)
{
    return w->max > w->min ? (w->value - w->min) / (w->max - w->min) : 0;
}

static void slider_measure(struct widget *w, int avail, int *pw, int *ph)
{
    (void)avail;
    *pw = 160;
    *ph = !strcmp(w->tag, "progress") ? 16 : 26;
}

static void slider_paint(struct widget *w, struct gfx *g)
{
    double f = MIN(MAX(fraction(w), 0), 1);
    int pad = 9, track_w = w->r.w - 2 * pad, cy = w->r.y + w->r.h / 2;
    int fill = (int)(track_w * f);
    color_t acc = w->enabled ? ui_theme.accent : ui_theme.text_dim;

    gfx_fill_rounded(g, (struct rect){ w->r.x + pad, cy - 2, track_w, 4 }, 2, ui_mix(ui_theme.border, ui_theme.window, 60));
    gfx_fill_rounded(g, (struct rect){ w->r.x + pad, cy - 2, fill, 4 }, 2, acc);
    gfx_circle(g, w->r.x + pad + fill, cy, 8, acc);
    gfx_circle(g, w->r.x + pad + fill, cy, w->pressed || w->hover ? 4 : 5, ui_theme.surface);
    if (ui_is_focused(w) && w->win->focus_visible)
        gfx_ring(g, w->r.x + pad + fill, cy, 11, 2, ui_theme.focus_ring);
}

static void slider_set(struct widget *w, double v)
{
    double old = w->value;

    if (w->step > 0)
        v = w->min + (int64_t)((v - w->min) / w->step + (v >= w->min ? 0.5 : -0.5)) * w->step;
    ui_set_value(w, v);
    if (w->value != old)
        ui_emit(w, "change");
}

static void slider_at(struct widget *w, int x)
{
    int pad = 9, track_w = MAX(1, w->r.w - 2 * pad);

    slider_set(w, w->min + (double)(x - w->r.x - pad) / track_w * (w->max - w->min));
}

static bool slider_pointer(struct widget *w, struct wm_event *ev)
{
    if (ev->kind == WM_PTR_DOWN && ev->detail == BTN_LEFT) {
        w->pressed = true;
        slider_at(w, ev->x);
        return true;
    }
    if (ev->kind == WM_PTR_MOVE && w->pressed) {
        slider_at(w, ev->x);
        return true;
    }
    if (ev->kind == WM_PTR_UP && w->pressed) {
        w->pressed = false;
        ui_redraw(w);
        ui_emit(w, "release");
        return true;
    }
    if (ev->kind == WM_PTR_WHEEL) {
        slider_set(w, w->value + (ev->detail > 0 ? w->step : -w->step));
        return true;
    }
    return false;
}

static bool slider_key(struct widget *w, struct wm_event *ev)
{
    double big = MAX(w->step, (w->max - w->min) / 10);

    if (!ev->value)
        return false;
    switch (ev->key) {
    case KEY_LEFT: case KEY_DOWN: slider_set(w, w->value - w->step); break;
    case KEY_RIGHT: case KEY_UP: slider_set(w, w->value + w->step); break;
    case KEY_PAGEDOWN: slider_set(w, w->value - big); break;
    case KEY_PAGEUP: slider_set(w, w->value + big); break;
    case KEY_HOME: slider_set(w, w->min); break;
    case KEY_END: slider_set(w, w->max); break;
    default: return false;
    }
    return true;
}

static void progress_paint(struct widget *w, struct gfx *g)
{
    double f = MIN(MAX(fraction(w), 0), 1);
    int h = 8, y = w->r.y + (w->r.h - h) / 2;

    gfx_fill_rounded(g, (struct rect){ w->r.x, y, w->r.w, h }, h / 2, ui_mix(ui_theme.border, ui_theme.window, 80));
    if (f > 0)
        gfx_fill_rounded(g, (struct rect){ w->r.x, y, MAX(h, (int)(w->r.w * f)), h }, h / 2, ui_theme.accent);
}

static const struct wclass slider_class = { "slider", true, .measure = slider_measure, .paint = slider_paint,
                                            .pointer = slider_pointer, .key = slider_key };
static const struct wclass progress_class = { "progress", .measure = slider_measure, .paint = progress_paint };

// ---- Images ----

struct image {
    struct surface *s;
    bool owned;
};

static void image_init(struct widget *w)
{
    w->data = calloc(1, sizeof(struct image));
}

void ui_image_set(struct widget *w, struct surface *s, bool owned)
{
    struct image *im;

    if (!w || strcmp(w->tag, "image") || !(im = w->data))
        return;
    if (im->owned && im->s != s)
        surface_destroy(im->s);
    im->s = s;
    im->owned = owned;
    if (w->win) {
        ui_relayout(w->win);
        ui_redraw(w);
    }
}

static void image_attr(struct widget *w, const char *name, const char *value)
{
    if (!strcmp(name, "src")) {
        struct surface *s = *value ? image_load(value) : NULL;

        if (*value && !s)
            dprintf(STDERR_FILENO, "ui: cannot load image %s\n", value);
        ui_image_set(w, s, true);
    }
}

static void image_measure(struct widget *w, int avail, int *pw, int *ph)
{
    struct image *im = w->data;

    (void)avail;
    *pw = im && im->s ? im->s->width : 0;
    *ph = im && im->s ? im->s->height : 0;
}

static void image_paint(struct widget *w, struct gfx *g)
{
    struct image *im = w->data;
    const char *mode = ui_attr(w, "scale");
    struct rect from, to;
    struct gfx saved = *g;
    double sx, sy, s;

    if (!im || !im->s || w->r.w <= 0 || w->r.h <= 0)
        return;
    from = (struct rect){ 0, 0, im->s->width, im->s->height };
    sx = (double)w->r.w / im->s->width;
    sy = (double)w->r.h / im->s->height;
    if (mode && !strcmp(mode, "stretch")) {
        to = w->r;
    } else {
        if (mode && !strcmp(mode, "fill"))
            s = MAX(sx, sy);
        else if (mode && !strcmp(mode, "none"))
            s = 1;
        else
            s = MIN(1.0, MIN(sx, sy));      // "fit": shrink, never enlarge
        to.w = (int)(im->s->width * s + 0.5);
        to.h = (int)(im->s->height * s + 0.5);
        to.x = w->r.x + (w->r.w - to.w) / 2;
        to.y = w->r.y + (w->r.h - to.h) / 2;
    }
    ui_clip(g, w->r);
    if (to.w == from.w && to.h == from.h)
        gfx_blit(g, im->s, from, to.x, to.y);
    else
        gfx_blit_scaled(g, im->s, from, to);
    *g = saved;
}

static void image_free(struct widget *w)
{
    struct image *im = w->data;

    if (im && im->owned)
        surface_destroy(im->s);
    free(im);
}

static const struct wclass image_class = { "image", .init = image_init, .measure = image_measure,
                                           .paint = image_paint, .attr = image_attr, .free = image_free };

// ---- Separators and spacers ----

static void separator_measure(struct widget *w, int avail, int *pw, int *ph)
{
    (void)avail;
    if (horizontal(w->parent)) {
        *pw = 9;
        *ph = 1;
    } else {
        *pw = 1;
        *ph = 9;
    }
}

static void separator_paint(struct widget *w, struct gfx *g)
{
    if (horizontal(w->parent))
        gfx_fill(g, (struct rect){ w->r.x + w->r.w / 2, w->r.y + 2, 1, w->r.h - 4 }, ui_theme.border);
    else
        gfx_fill(g, (struct rect){ w->r.x, w->r.y + w->r.h / 2, w->r.w, 1 }, ui_theme.border);
}

static void spacer_init(struct widget *w)
{
    w->expand = 1;
}

static void spacer_measure(struct widget *w, int avail, int *pw, int *ph)
{
    const char *s = ui_attr(w, "size");

    (void)avail;
    *pw = *ph = s ? atoi(s) : 0;
}

static void spacer_attr(struct widget *w, const char *name, const char *value)
{
    (void)value;
    if (!strcmp(name, "size"))
        w->expand = 0;
}

static const struct wclass separator_class = { "separator", .measure = separator_measure,
                                               .paint = separator_paint };
static const struct wclass spacer_class = { "spacer", .init = spacer_init, .measure = spacer_measure,
                                            .attr = spacer_attr };

// ---- Canvas ----

struct canvas {
    ui_paint_fn paint;
    ui_input_fn input;
    void *user;
};

static void canvas_init(struct widget *w)
{
    w->data = calloc(1, sizeof(struct canvas));
    w->expand = 1;
}

void ui_canvas_set(struct widget *w, ui_paint_fn paint, ui_input_fn input, void *user)
{
    struct canvas *c;

    if (!w || strcmp(w->tag, "canvas") || !(c = w->data))
        return;
    c->paint = paint;
    c->input = input;
    c->user = user;
    ui_redraw(w);
}

static void canvas_measure(struct widget *w, int avail, int *pw, int *ph)
{
    (void)w;
    (void)avail;
    *pw = *ph = 0;
}

static void canvas_paint(struct widget *w, struct gfx *g)
{
    struct canvas *c = w->data;
    struct gfx saved = *g;

    if (!c || !c->paint)
        return;
    ui_clip(g, w->r);
    c->paint(w, g, w->r, c->user);
    *g = saved;
}

bool ui_is_focused_widget(struct widget *w)
{
    return ui_is_focused(w);
}

static bool canvas_pointer(struct widget *w, struct wm_event *ev)
{
    struct canvas *c = w->data;

    struct wm_event local;

    if (!c || !c->input)
        return false;
    // Pointer positions are relative to the canvas.
    local = *ev;
    local.x -= w->r.x;
    local.y -= w->r.y;
    c->input(w, &local, c->user);
    return true;
}

struct rect ui_rect(struct widget *w)
{
    return w ? w->r : (struct rect){ 0, 0, 0, 0 };
}

static bool canvas_key(struct widget *w, struct wm_event *ev)
{
    struct canvas *c = w->data;

    if (!c || !c->input)
        return false;
    // Tab moves the focus unless the canvas wants it (terminals).
    if (ev->key == KEY_TAB && !ui_attr_true(ui_attr(w, "wanttab")))
        return false;
    c->input(w, ev, c->user);
    // Keys go on to shortcuts unless the canvas takes them all.
    return ui_attr_true(ui_attr(w, "wantkeys")) || ui_attr_true(ui_attr(w, "wanttab"));
}

static const struct wclass canvas_class = { "canvas", .init = canvas_init, .measure = canvas_measure,
                                            .paint = canvas_paint, .pointer = canvas_pointer, .key = canvas_key,
                                            .free = free_data };

// ---- The class table ----

extern const struct wclass ui_input_class, ui_password_class, ui_textarea_class, ui_list_class, ui_table_class,
    ui_dropdown_class, ui_option_class, ui_item_class, ui_menubar_class, ui_menu_class, ui_menuitem_class,
    ui_popuplist_class, ui_spin_class;

const struct wclass *const ui_classes[] = {
    &window_class, &box_class, &vbox_class, &hbox_class, &group_class, &card_class, &toolbar_class, &statusbar_class, &tab_class,
    &grid_class, &stack_class, &tabs_class, &scroll_class, &label_class, &h1_class, &h2_class, &p_class,
    &link_class, &button_class, &checkbox_class, &radio_class, &toggle_class, &slider_class, &progress_class,
    &image_class, &separator_class, &spacer_class, &canvas_class,
    &ui_input_class, &ui_password_class, &ui_textarea_class, &ui_list_class, &ui_table_class, &ui_dropdown_class,
    &ui_option_class, &ui_item_class, &ui_menubar_class, &ui_menu_class, &ui_menuitem_class, &ui_popuplist_class,
    &ui_spin_class, NULL,
};
