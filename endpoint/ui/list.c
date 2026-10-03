#include "ui_internal.h"

// <list>, <table> (a list with column headers) and <dropdown>. Rows are
// strings of tab-separated columns. The widget's value is the selected row
// (-1: none).

#define MAX_COLS    16
#define BAR_W       12

struct column {
    char *title;
    int width;                      // 0: share the remaining space
    enum align align;
};

struct list {
    char **items;
    struct surface **icons;
    bool *shared;                   // icons the list does not own
    int n, cap;
    int sy, hover;
    struct column cols[MAX_COLS];
    int ncols;
    bool bar_drag;
    int bar_grab;
    char find[32];                  // type-to-find prefix
    uint64_t find_ms;
    int drop_row;                   // where a drag would land (-1: none)
    bool armed;                     // a row was pressed: moving away drags it
    int press_x, press_y;
};

static struct list *L(struct widget *w)
{
    return w->data;
}

static void drop_icon(struct list *l, int i)
{
    if (!l->shared[i])
        surface_destroy(l->icons[i]);
    l->icons[i] = NULL;
}

static bool is_list(struct widget *w)
{
    return w && (!strcmp(w->tag, "list") || !strcmp(w->tag, "table") || !strcmp(w->tag, "dropdown"));
}

static int row_h(void)
{
    return ui_theme.row_height - 2;
}

static int header_h(struct widget *w)
{
    return !strcmp(w->tag, "table") && L(w)->ncols ? ui_theme.row_height : 0;
}

static struct rect rows_rect(struct widget *w)
{
    int hh = header_h(w);

    return (struct rect){ w->r.x + 1, w->r.y + 1 + hh, w->r.w - 2, w->r.h - 2 - hh };
}

static int total_h(struct widget *w)
{
    return L(w)->n * row_h();
}

static void clamp_scroll(struct widget *w)
{
    struct list *l = L(w);

    l->sy = MAX(0, MIN(l->sy, MAX(0, total_h(w) - rows_rect(w).h)));
}

static void show_row(struct widget *w, int i)
{
    struct list *l = L(w);
    struct rect rr = rows_rect(w);

    if (i < 0 || strcmp(w->tag, "dropdown") == 0)
        return;
    if (i * row_h() < l->sy)
        l->sy = i * row_h();
    else if ((i + 1) * row_h() > l->sy + rr.h)
        l->sy = (i + 1) * row_h() - rr.h;
    clamp_scroll(w);
}

// ---- Columns ----

static void parse_columns(struct widget *w, const char *spec)
{
    struct list *l = L(w);
    const char *p = spec;

    for (int i = 0; i < l->ncols; i++)
        free(l->cols[i].title);
    l->ncols = 0;
    while (*p && l->ncols < MAX_COLS) {
        const char *end = strchr(p, '|');
        int len = end ? end - p : (int)strlen(p);
        char part[128], *colon;
        struct column *c = &l->cols[l->ncols];

        snprintf(part, sizeof(part), "%.*s", len, p);
        c->width = 0;
        c->align = ALIGN_START;
        if ((colon = strchr(part, ':'))) {
            char *second;

            *colon++ = 0;
            c->width = atoi(colon);
            if ((second = strchr(colon, ':')))
                c->align = !strcmp(second + 1, "right") ? ALIGN_END : !strcmp(second + 1, "center") ? ALIGN_CENTER
                                                                                                    : ALIGN_START;
        }
        c->title = strdup(ui_translate(part));
        l->ncols++;
        if (!end)
            break;
        p = end + 1;
    }
}

// Column x positions within the row rectangle.
static void column_layout(struct widget *w, struct rect rr, int *xs, int *ws, int *n)
{
    struct list *l = L(w);
    int fixed = 0, flex = 0, x = rr.x;

    if (!l->ncols) {
        xs[0] = rr.x;
        ws[0] = rr.w;
        *n = 1;
        return;
    }
    for (int i = 0; i < l->ncols; i++) {
        if (l->cols[i].width)
            fixed += l->cols[i].width;
        else
            flex++;
    }
    for (int i = 0; i < l->ncols; i++) {
        int cw = l->cols[i].width ? l->cols[i].width : MAX(40, (rr.w - fixed) / MAX(flex, 1));

        xs[i] = x;
        ws[i] = cw;
        x += cw;
    }
    *n = l->ncols;
}

// ---- Items ----

void ui_list_clear(struct widget *w)
{
    struct list *l;

    if (!is_list(w))
        return;
    l = L(w);
    for (int i = 0; i < l->n; i++) {
        free(l->items[i]);
        drop_icon(l, i);
    }
    l->n = 0;
    l->sy = 0;
    l->hover = -1;
    w->value = -1;
    if (w->win)
        ui_relayout(w->win);
    ui_redraw(w);
}

int ui_list_add(struct widget *w, const char *text)
{
    struct list *l;

    if (!is_list(w))
        return -1;
    l = L(w);
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        char **items = realloc(l->items, cap * sizeof(char *));
        struct surface **icons;

        if (!items)
            return -1;
        l->items = items;
        if (!(icons = realloc(l->icons, cap * sizeof(*icons))))
            return -1;
        l->icons = icons;
        {
            bool *sh = realloc(l->shared, cap * sizeof(bool));

            if (!sh)
                return -1;
            l->shared = sh;
        }
        l->cap = cap;
    }
    if (!(l->items[l->n] = strdup(text ? text : "")))
        return -1;
    l->icons[l->n] = NULL;
    l->shared[l->n] = false;
    if (!strcmp(w->tag, "dropdown") && w->win)
        ui_relayout(w->win);
    ui_redraw(w);
    return l->n++;
}

int ui_list_count(struct widget *w)
{
    return is_list(w) ? L(w)->n : 0;
}

const char *ui_list_item(struct widget *w, int i)
{
    return is_list(w) && i >= 0 && i < L(w)->n ? L(w)->items[i] : NULL;
}

void ui_list_set_item(struct widget *w, int i, const char *text)
{
    char *t;

    if (!is_list(w) || i < 0 || i >= L(w)->n || !(t = strdup(text ? text : "")))
        return;
    free(L(w)->items[i]);
    L(w)->items[i] = t;
    ui_redraw(w);
}

void ui_list_remove(struct widget *w, int i)
{
    struct list *l;

    if (!is_list(w) || i < 0 || i >= (l = L(w))->n)
        return;
    free(l->items[i]);
    drop_icon(l, i);
    memmove(l->items + i, l->items + i + 1, (l->n - i - 1) * sizeof(char *));
    memmove(l->icons + i, l->icons + i + 1, (l->n - i - 1) * sizeof(*l->icons));
    memmove(l->shared + i, l->shared + i + 1, (l->n - i - 1) * sizeof(bool));
    l->n--;
    if (w->value >= l->n)
        w->value = l->n - 1;
    else if (w->value > i)
        w->value--;
    clamp_scroll(w);
    ui_redraw(w);
}

int ui_list_selected(struct widget *w)
{
    return is_list(w) ? (int)w->value : -1;
}

void ui_list_select(struct widget *w, int i)
{
    if (!is_list(w))
        return;
    if (i < -1 || i >= L(w)->n)
        i = -1;
    ui_set_value(w, i);
}

void ui_list_set_icon(struct widget *w, int i, struct surface *icon)
{
    if (!is_list(w) || i < 0 || i >= L(w)->n) {
        surface_destroy(icon);
        return;
    }
    drop_icon(L(w), i);
    L(w)->icons[i] = icon;
    ui_redraw(w);
}

void ui_list_set_icon_shared(struct widget *w, int i, struct surface *icon)
{
    if (!is_list(w) || i < 0 || i >= L(w)->n)
        return;
    drop_icon(L(w), i);
    L(w)->icons[i] = icon;
    L(w)->shared[i] = true;
    ui_redraw(w);
}

// Copies the text of column col of row i into buf.
static const char *column_text(const char *row, int col, char *buf, size_t size)
{
    for (int c = 0; c < col && row; c++) {
        row = strchr(row, '\t');
        if (row)
            row++;
    }
    if (!row)
        return "";
    {
        const char *end = strchr(row, '\t');
        size_t len = end ? (size_t)(end - row) : strlen(row);

        if (len >= size)
            len = size - 1;
        memcpy(buf, row, len);
        buf[len] = 0;
    }
    return buf;
}

const char *ui_list_column(struct widget *w, int i, int col, char *buf, size_t size)
{
    const char *row = ui_list_item(w, i);

    if (!row) {
        if (size)
            buf[0] = 0;
        return buf;
    }
    return column_text(row, col, buf, size);
}

void ui_list_init_from_children(struct widget *w)
{
    struct widget *c, *next;
    int selected = -1;

    if (!is_list(w))
        return;
    for (c = w->first; c; c = next) {
        next = c->next;
        if (!strcmp(c->tag, "item") || !strcmp(c->tag, "option")) {
            int i = ui_list_add(w, c->text ? c->text : "");
            const char *icon = ui_attr(c, "icon"), *sel = ui_attr(c, "selected");

            if (icon && i >= 0)
                ui_list_set_icon(w, i, image_load(icon));
            if (sel && attr_bool(sel))
                selected = i;
        }
        ui_remove(c);
    }
    if (selected >= 0)
        ui_list_select(w, selected);
    else if (ui_attr(w, "value"))
        ui_list_select(w, atoi(ui_attr(w, "value")));
    else if (!strcmp(w->tag, "dropdown") && L(w)->n)
        ui_list_select(w, 0);
}

// ---- Class: list and table ----

static void list_init(struct widget *w)
{
    struct list *l = calloc(1, sizeof(*l));

    w->data = l;
    w->min = -1;
    w->max = 1e9;
    w->value = -1;
    w->step = 1;
    if (l)
        l->hover = l->drop_row = -1;
    if (strcmp(w->tag, "dropdown"))
        w->expand = 1;
}

static void list_attr(struct widget *w, const char *name, const char *value)
{
    if (!strcmp(name, "columns"))
        parse_columns(w, value);
}

static void list_measure(struct widget *w, int avail, int *pw, int *ph)
{
    (void)avail;
    if (!strcmp(w->tag, "dropdown")) {
        struct font *f = ui_font();
        int mw = 0;
        char buf[256];

        for (int i = 0; i < L(w)->n; i++)
            mw = MAX(mw, text_width(f, column_text(L(w)->items[i], 0, buf, sizeof(buf)), -1));
        *pw = MAX(120, mw + 48);
        *ph = ui_theme.row_height;
        return;
    }
    *pw = 200;
    *ph = header_h(w) + 5 * row_h() + 2;
}

static void list_arrange(struct widget *w)
{
    clamp_scroll(w);
}

static void list_paint(struct widget *w, struct gfx *g)
{
    struct list *l = L(w);
    struct rect rr = rows_rect(w);
    struct font *f = ui_font(), *fb = ui_font_bold();
    int xs[MAX_COLS], ws[MAX_COLS], ncol, rh = row_h(), first, last, isz = rh - 10;
    bool focused = ui_is_focused(w);
    struct gfx saved = *g;
    char buf[512];
    bool bar = total_h(w) > rr.h;

    gfx_fill_rounded(g, w->r, ui_theme.radius, ui_theme.surface);
    gfx_outline_rounded(g, w->r, ui_theme.radius, 1, focused ? ui_theme.accent : ui_theme.border);
    if (bar)
        rr.w -= BAR_W;
    column_layout(w, rr, xs, ws, &ncol);
    if (header_h(w)) {
        struct rect hr = { w->r.x + 1, w->r.y + 1, w->r.w - 2, header_h(w) };
        const char *sort = ui_attr(w, "sortcolumn");

        gfx_fill(g, hr, ui_theme.surface_alt);
        gfx_fill(g, (struct rect){ hr.x, hr.y + hr.h - 1, hr.w, 1 }, ui_theme.border);
        for (int c = 0; c < ncol; c++) {
            struct rect cr = { xs[c] + 8, hr.y, ws[c] - 16, hr.h };

            ui_draw_text(g, fb, cr, l->cols[c].title, ui_theme.text_dim, l->cols[c].align);
            if (c)
                gfx_fill(g, (struct rect){ xs[c], hr.y + 6, 1, hr.h - 12 }, ui_theme.border);
            if (sort && atoi(sort) == c)
                ui_draw_arrow(g, xs[c] + ws[c] - 10, hr.y + hr.h / 2,
                              8, ui_attr(w, "sortdescending") && attr_bool(ui_attr(w, "sortdescending")) ? 0 : 1,
                              ui_theme.text_dim);
        }
    }
    ui_clip(g, rr);
    first = l->sy / rh;
    last = MIN(l->n - 1, (l->sy + rr.h) / rh);
    for (int i = first; i <= last; i++) {
        struct rect row = { rr.x, rr.y + i * rh - l->sy, rr.w, rh };
        bool sel = i == (int)w->value;
        color_t tc = ui_theme.text;

        if (sel) {
            gfx_fill(g, row, focused ? ui_theme.accent : ui_theme.selection);
            tc = focused ? ui_theme.accent_text : ui_theme.selection_text;
        } else if (i == l->drop_row) {
            gfx_fill(g, row, ui_theme.selection);
            tc = ui_theme.selection_text;
        } else if (i == l->hover) {
            gfx_fill(g, row, ui_theme.hover);
        }
        if (i == l->drop_row)
            gfx_outline_rounded(g, row, 4, 2, ui_theme.accent);
        for (int c = 0; c < ncol; c++) {
            struct rect cr = { xs[c] + 8, row.y, ws[c] - 16, rh };

            if (c == 0 && l->icons[i]) {
                gfx_blit_scaled(g, l->icons[i], (struct rect){ 0, 0, l->icons[i]->width, l->icons[i]->height },
                                (struct rect){ cr.x, row.y + (rh - isz) / 2, isz, isz });
                cr.x += isz + 6;
                cr.w -= isz + 6;
            }
            ui_draw_text(g, f, cr, column_text(l->items[i], c, buf, sizeof(buf)),
                         c && !sel ? ui_theme.text_dim : tc, l->ncols ? l->cols[c].align : ALIGN_START);
        }
    }
    if (!l->n && ui_attr(w, "placeholder"))
        ui_draw_text(g, f, (struct rect){ rr.x, rr.y + 8, rr.w, rh }, ui_translate(ui_attr(w, "placeholder")),
                     ui_theme.text_dim, ALIGN_CENTER);
    *g = saved;
    if (bar)
        ui_draw_scrollbar(g, (struct rect){ w->r.x + w->r.w - BAR_W - 1, rr.y + 2, BAR_W, rr.h - 4 }, total_h(w),
                          rr.h, l->sy, l->bar_drag);
}

static void select_row(struct widget *w, int i)
{
    struct list *l = L(w);

    if (!l->n)
        return;
    i = MIN(MAX(i, 0), l->n - 1);
    show_row(w, i);
    if (i != (int)w->value) {
        ui_set_value(w, i);
        ui_emit(w, "select");
        ui_emit(w, "change");
    }
    ui_redraw(w);
}

static int row_at(struct widget *w, int y)
{
    struct rect rr = rows_rect(w);
    int i;

    if (y < rr.y || y >= rr.y + rr.h)
        return -1;
    i = (y - rr.y + L(w)->sy) / row_h();
    return i < L(w)->n ? i : -1;
}

int ui_list_row_at(struct widget *w, int x, int y)
{
    if (!is_list(w) || !rect_contains(rows_rect(w), x, y))
        return -1;
    return row_at(w, y);
}

void ui_list_set_drop_row(struct widget *w, int index)
{
    if (is_list(w) && L(w)->drop_row != index) {
        L(w)->drop_row = index;
        ui_redraw(w);
    }
}

static struct rect list_bar(struct widget *w)
{
    struct rect rr = rows_rect(w);

    return (struct rect){ w->r.x + w->r.w - BAR_W - 1, rr.y + 2, BAR_W, rr.h - 4 };
}

static bool list_pointer(struct widget *w, struct wm_event *ev)
{
    struct list *l = L(w);
    struct rect rr = rows_rect(w), bar = list_bar(w);
    bool has_bar = total_h(w) > rr.h;

    switch (ev->kind) {
    case WM_PTR_MOVE: {
        int i = rect_contains(w->r, ev->x, ev->y) ? row_at(w, ev->y) : -1;

        if (l->bar_drag) {
            l->sy = ui_scrollbar_offset(bar, total_h(w), rr.h, ev->y, l->bar_grab);
            ui_redraw(w);
            return true;
        }
        if (l->armed && (ev->buttons & 1) && (abs(ev->x - l->press_x) > 6 || abs(ev->y - l->press_y) > 6)) {
            // Pulled away from the pressed row.
            l->armed = false;
            ui_emit(w, "drag");
            return true;
        }
        if (has_bar && ev->x >= bar.x)
            i = -1;
        if (i != l->hover) {
            l->hover = i;
            ui_redraw(w);
        }
        return true;
    }
    case WM_PTR_LEAVE:
        l->hover = -1;
        ui_redraw(w);
        return true;
    case WM_PTR_DOWN: {
        int i;

        if (has_bar && ev->detail == BTN_LEFT && rect_contains(bar, ev->x, ev->y)) {
            int th, ty = ui_scrollbar_thumb(bar, total_h(w), rr.h, l->sy, &th);

            l->bar_grab = ev->y >= ty && ev->y < ty + th ? ev->y - ty : th / 2;
            l->bar_drag = true;
            l->sy = ui_scrollbar_offset(bar, total_h(w), rr.h, ev->y, l->bar_grab);
            ui_redraw(w);
            return true;
        }
        if (header_h(w) && ev->y < rr.y && ev->detail == BTN_LEFT) {
            int xs[MAX_COLS], ws[MAX_COLS], n;

            column_layout(w, rr, xs, ws, &n);
            for (int c = 0; c < n; c++) {
                if (ev->x >= xs[c] && ev->x < xs[c] + ws[c]) {
                    char num[8];
                    const char *cur = ui_attr(w, "sortcolumn");
                    bool desc = cur && atoi(cur) == c && !(ui_attr(w, "sortdescending")
                                                            && attr_bool(ui_attr(w, "sortdescending")));

                    snprintf(num, sizeof(num), "%d", c);
                    ui_set_attr(w, "sortcolumn", num);
                    ui_set_attr(w, "sortdescending", desc ? "true" : "false");
                    ui_emit(w, "sort");
                    ui_redraw(w);
                }
            }
            return true;
        }
        i = row_at(w, ev->y);
        if (i >= 0)
            select_row(w, i);
        if (i >= 0 && ev->detail == BTN_LEFT && ui_has_handler(w, "drag")) {
            l->armed = true;
            l->press_x = ev->x;
            l->press_y = ev->y;
        }
        if (ev->detail == BTN_RIGHT) {
            ui_emit(w, "context");
        } else if (ev->detail == BTN_LEFT && i >= 0
                   && (ui_click_count(w) == 2 || attr_bool(ui_attr(w, "singleclick") ? ui_attr(w, "singleclick") : "false"))) {
            ui_emit(w, "activate");
        }
        return true;
    }
    case WM_PTR_UP:
        l->bar_drag = l->armed = false;
        return true;
    case WM_PTR_WHEEL: {
        int old = l->sy;

        l->sy -= ev->detail * 3 * row_h();
        clamp_scroll(w);
        ui_redraw(w);
        return old != l->sy;
    }
    }
    return false;
}

static void find_prefix(struct widget *w, const char *text)
{
    struct list *l = L(w);
    uint64_t now = uptime_ms();
    size_t len;
    int start = MAX(0, (int)w->value);

    if (now - l->find_ms > 1000)
        l->find[0] = 0;
    l->find_ms = now;
    len = strlen(l->find);
    if (len + strlen(text) < sizeof(l->find))
        strcat(l->find, text);
    len = strlen(l->find);
    // A repeated single letter cycles through matches.
    if (len == 2 && l->find[0] == l->find[1]) {
        l->find[1] = 0;
        len = 1;
        start++;
    }
    for (int k = 0; k < l->n; k++) {
        int i = (start + k) % l->n;

        if (!strncasecmp(l->items[i], l->find, len)) {
            select_row(w, i);
            return;
        }
    }
}

static bool list_key(struct widget *w, struct wm_event *ev)
{
    struct list *l = L(w);
    int page = MAX(1, rows_rect(w).h / row_h() - 1), cur = (int)w->value;

    if (!ev->value)
        return false;
    switch (ev->key) {
    case KEY_UP: select_row(w, cur < 0 ? 0 : cur - 1); return true;
    case KEY_DOWN: select_row(w, cur + 1); return true;
    case KEY_HOME: select_row(w, 0); return true;
    case KEY_END: select_row(w, l->n - 1); return true;
    case KEY_PAGEUP: select_row(w, cur - page); return true;
    case KEY_PAGEDOWN: select_row(w, cur + page); return true;
    case KEY_ENTER:
    case KEY_KPENTER:
        if (cur >= 0 && ui_has_handler(w, "activate") && ev->value == 1) {
            ui_emit(w, "activate");
            return true;
        }
        return false;
    case KEY_COMPOSE:
        ui_emit(w, "context");
        return true;
    }
    if (ev->text[0] && ev->text[0] != ' ' && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        find_prefix(w, ev->text);
        return true;
    }
    return false;
}

static void list_value_changed(struct widget *w)
{
    show_row(w, (int)w->value);
}

static void list_free(struct widget *w)
{
    struct list *l = L(w);

    if (!l)
        return;
    for (int i = 0; i < l->n; i++) {
        free(l->items[i]);
        drop_icon(l, i);
    }
    for (int i = 0; i < l->ncols; i++)
        free(l->cols[i].title);
    free(l->items);
    free(l->icons);
    free(l->shared);
    free(l);
}

const struct wclass ui_list_class = { "list", true, .init = list_init, .measure = list_measure,
                                      .arrange = list_arrange, .paint = list_paint, .pointer = list_pointer,
                                      .key = list_key, .attr = list_attr, .value_changed = list_value_changed,
                                      .free = list_free };
const struct wclass ui_table_class = { "table", true, .init = list_init, .measure = list_measure,
                                       .arrange = list_arrange, .paint = list_paint, .pointer = list_pointer,
                                       .key = list_key, .attr = list_attr, .value_changed = list_value_changed,
                                       .free = list_free };

// ---- Class: dropdown ----

static void dropdown_paint(struct widget *w, struct gfx *g)
{
    const char *item = ui_list_item(w, (int)w->value);
    char buf[256];
    struct rect tr = { w->r.x + 10, w->r.y, w->r.w - 36, w->r.h };

    ui_draw_button_bg(g, w, w->r, false);
    ui_draw_text(g, ui_font(), tr, item ? column_text(item, 0, buf, sizeof(buf)) : "",
                 w->enabled ? ui_theme.text : ui_theme.text_dim, ALIGN_START);
    ui_draw_arrow(g, w->r.x + w->r.w - 16, w->r.y + w->r.h / 2, 9, 0, ui_theme.text_dim);
    if (ui_is_focused(w) && w->win->focus_visible)
        ui_draw_focus(g, w->r, ui_theme.radius);
}

static const char *dropdown_item(struct widget *w, int i)
{
    return ui_list_item(w, i);
}

static void dropdown_chosen(struct widget *w, int i)
{
    if (i >= 0 && i != (int)w->value) {
        ui_set_value(w, i);
        ui_emit(w, "change");
        ui_emit(w, "select");
    }
    ui_redraw(w);
}

static void dropdown_open(struct widget *w)
{
    ui_popup_list_open(w, w->r.x, w->r.y + w->r.h + 2, w->r.w, L(w)->n, (int)w->value, dropdown_item,
                       dropdown_chosen);
}

static bool dropdown_pointer(struct widget *w, struct wm_event *ev)
{
    if (ev->kind == WM_PTR_DOWN && ev->detail == BTN_LEFT) {
        w->pressed = true;
        ui_redraw(w);
        if (w->win->popup && w->win->popup_owner == w)
            ui_popup_close(w->win);
        else
            dropdown_open(w);
        return true;
    }
    if (ev->kind == WM_PTR_UP) {
        w->pressed = false;
        ui_redraw(w);
        return true;
    }
    if (ev->kind == WM_PTR_WHEEL) {
        dropdown_chosen(w, MIN(MAX((int)w->value - (ev->detail > 0 ? 1 : -1), 0), L(w)->n - 1));
        return true;
    }
    return false;
}

static bool dropdown_key(struct widget *w, struct wm_event *ev)
{
    int cur = (int)w->value;

    if (!ev->value)
        return false;
    if (ev->key == KEY_SPACE || ((ev->mods & MOD_ALT) && ev->key == KEY_DOWN)) {
        dropdown_open(w);
        return true;
    }
    if (ev->key == KEY_UP && cur > 0) {
        dropdown_chosen(w, cur - 1);
        return true;
    }
    if (ev->key == KEY_DOWN && cur + 1 < L(w)->n) {
        dropdown_chosen(w, cur + 1);
        return true;
    }
    return ev->key == KEY_UP || ev->key == KEY_DOWN;
}

const struct wclass ui_dropdown_class = { "dropdown", true, .init = list_init, .measure = list_measure,
                                          .paint = dropdown_paint, .pointer = dropdown_pointer,
                                          .key = dropdown_key, .free = list_free };

// <item> and <option> only exist while parsing; their parent takes them in.
static void hidden_init(struct widget *w)
{
    w->visible = false;
}

const struct wclass ui_option_class = { "option", .init = hidden_init };
const struct wclass ui_item_class = { "item", .init = hidden_init };
