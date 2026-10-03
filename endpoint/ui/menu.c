#include "ui_internal.h"

// Menu bars, menus (popup windows), dropdown lists and keyboard shortcuts.
//
//   <menubar>
//     <menu text="File">
//       <item text="Open..." shortcut="Ctrl+O" onclick="open"/>
//       <separator/>
//       <menu text="Recent"> ... </menu>
//     </menu>
//   </menubar>
//
// A <menu> outside a menu bar is not shown; open it with ui_menu_popup().

#define POPUP_ROWS_MAX  14

struct pentry {
    char *text;
    char *shortcut;
    bool sep, enabled, checked, sub;
    struct widget *src;             // the <item> or the submenu
};

struct popup {
    struct pentry *e;
    int n, hover, top;
    struct widget *owner;
    void (*chosen)(struct widget *, int);
    bool is_list;
};

static struct widget *closed_owner;
static uint64_t closed_ms;

// True right after a popup owned by w was dismissed by a click: that click
// should not open it again.
static bool just_closed(struct widget *w)
{
    return closed_owner == w && uptime_ms() - closed_ms < 250;
}

void ui_popup_close(struct ui_window *win)
{
    struct ui_window *pw;
    struct widget *owner;

    if (!win || !(pw = win->popup))
        return;
    ui_popup_close(pw);
    owner = win->popup_owner;
    win->popup = NULL;
    win->popup_owner = NULL;
    ui_window_close(pw);
    if (owner) {
        closed_owner = owner;
        closed_ms = uptime_ms();
        owner->pressed = false;
        ui_redraw(owner);
    }
}

// ---- The popup list widget ----

static int entry_h(struct pentry *e)
{
    return e->sep ? 9 : ui_theme.row_height;
}

static int visible_rows(struct popup *p)
{
    return p->is_list ? MIN(p->n, POPUP_ROWS_MAX) : p->n;
}

static void popup_measure(struct widget *w, int avail, int *pw, int *ph)
{
    struct popup *p = w->data;
    struct font *f = ui_font();
    int tw = 0, sw = 0, h = 0;

    (void)avail;
    if (!p)
        return;
    for (int i = 0; i < p->n; i++) {
        tw = MAX(tw, text_width(f, p->e[i].text, -1));
        if (p->e[i].shortcut)
            sw = MAX(sw, text_width(f, p->e[i].shortcut, -1));
    }
    for (int i = p->top; i < p->top + visible_rows(p); i++)
        h += entry_h(&p->e[i]);
    *pw = MAX(tw + (sw ? sw + 32 : 0) + (p->is_list ? 24 : 64), 120);
    *ph = h + 8;
}

static int entry_y(struct widget *w, int i)
{
    struct popup *p = w->data;
    int y = w->r.y + 4;

    for (int k = p->top; k < i; k++)
        y += entry_h(&p->e[k]);
    return y;
}

static int entry_at(struct widget *w, int y)
{
    struct popup *p = w->data;
    int ey = w->r.y + 4;

    for (int i = p->top; i < p->top + visible_rows(p); i++) {
        int h = entry_h(&p->e[i]);

        if (y >= ey && y < ey + h)
            return i;
        ey += h;
    }
    return -1;
}

static void popup_paint(struct widget *w, struct gfx *g)
{
    struct popup *p = w->data;
    struct font *f = ui_font();

    if (!p)
        return;
    for (int i = p->top; i < p->top + visible_rows(p); i++) {
        struct pentry *e = &p->e[i];
        int y = entry_y(w, i), h = entry_h(e);
        struct rect row = { w->r.x + 4, y, w->r.w - 8, h };
        color_t tc = e->enabled ? ui_theme.text : ui_mix(ui_theme.text_dim, ui_theme.surface, 80);
        int tx = row.x + (p->is_list ? 10 : 30);

        if (e->sep) {
            gfx_fill(g, (struct rect){ row.x + 6, y + h / 2, row.w - 12, 1 }, ui_theme.border);
            continue;
        }
        if (i == p->hover && e->enabled) {
            gfx_fill_rounded(g, row, 4, ui_theme.accent);
            tc = ui_theme.accent_text;
        }
        if (e->checked) {
            if (p->is_list)
                gfx_fill(g, (struct rect){ row.x + 2, y + 8, 3, h - 16 }, i == p->hover ? tc : ui_theme.accent);
            else
                ui_draw_check(g, row.x + 7, y + (h - 16) / 2.0f, 16, tc);
        }
        ui_draw_text(g, f, (struct rect){ tx, y, row.x + row.w - tx - 10, h }, e->text, tc, ALIGN_START);
        if (e->shortcut)
            ui_draw_text(g, f, (struct rect){ row.x, y, row.w - 12, h }, e->shortcut,
                         i == p->hover ? tc : ui_theme.text_dim, ALIGN_END);
        if (e->sub)
            ui_draw_arrow(g, row.x + row.w - 12, y + h / 2, 8, 2, tc);
    }
    if (p->is_list && p->n > visible_rows(p)) {
        int total = p->n * ui_theme.row_height, vis = visible_rows(p) * ui_theme.row_height;

        ui_draw_scrollbar(g, (struct rect){ w->r.x + w->r.w - 12, w->r.y + 4, 10, w->r.h - 8 }, total, vis,
                          p->top * ui_theme.row_height, false);
    }
}

static struct ui_window *top_window(struct ui_window *win)
{
    while (win && win->is_popup && win->parent)
        win = win->parent;
    return win;
}

static struct popup *menu_entries(struct widget *menu);
static void open_popup(struct ui_window *parent, struct widget *owner, int x, int y, struct popup *p, int min_w);
static void menubar_step(struct widget *bar, int dir);

static void open_submenu(struct widget *w, int i)
{
    struct popup *p = w->data, *sub;

    if (w->win->popup && w->win->popup_owner == w && p->hover == i)
        return;
    if (!(sub = menu_entries(p->e[i].src)))
        return;
    sub->owner = p->owner;
    open_popup(w->win, w, w->r.x + w->r.w - 6, entry_y(w, i) - 4, sub, 0);
}

static void activate(struct widget *w, int i)
{
    struct popup *p = w->data;
    struct pentry *e;
    struct widget *owner = p->owner, *src;
    void (*chosen)(struct widget *, int) = p->chosen;

    if (i < 0 || i >= p->n)
        return;
    e = &p->e[i];
    if (e->sep || !e->enabled)
        return;
    if (e->sub) {
        p->hover = i;
        open_submenu(w, i);
        return;
    }
    src = e->src;
    // Closing frees this popup: everything needed is copied above.
    ui_popup_close(top_window(w->win));
    if (chosen)
        chosen(owner, i);
    else if (src)
        ui_emit(src, "click");
}

static void set_hover(struct widget *w, int i)
{
    struct popup *p = w->data;

    if (i == p->hover)
        return;
    p->hover = i;
    if (i >= 0 && i < p->top)
        p->top = i;
    else if (i >= p->top + visible_rows(p))
        p->top = i - visible_rows(p) + 1;
    ui_redraw(w);
}

static bool popup_pointer(struct widget *w, struct wm_event *ev)
{
    struct popup *p = w->data;
    int i = entry_at(w, ev->y);

    if (!p)
        return false;
    switch (ev->kind) {
    case WM_PTR_MOVE:
    case WM_PTR_ENTER:
        if (i >= 0 && p->e[i].sep)
            i = -1;
        set_hover(w, i);
        if (i >= 0 && p->e[i].sub && p->e[i].enabled)
            open_submenu(w, i);
        else if (i >= 0 && w->win->popup)
            ui_popup_close(w->win);
        return true;
    case WM_PTR_UP:
        if (ev->detail == BTN_LEFT || ev->detail == BTN_RIGHT)
            activate(w, i);
        return true;
    case WM_PTR_WHEEL:
        if (p->is_list) {
            p->top = MIN(MAX(p->top - ev->detail, 0), p->n - visible_rows(p));
            ui_redraw(w);
        }
        return true;
    }
    return true;
}

static void step_hover(struct widget *w, int dir)
{
    struct popup *p = w->data;
    int i = p->hover;

    for (int k = 0; k < p->n; k++) {
        i = i < 0 ? (dir > 0 ? 0 : p->n - 1) : (i + dir + p->n) % p->n;
        if (!p->e[i].sep && p->e[i].enabled) {
            set_hover(w, i);
            return;
        }
    }
}

static bool popup_key(struct widget *w, struct wm_event *ev)
{
    struct popup *p = w->data;
    struct ui_window *sub = w->win->popup;

    if (!p || !ev->value)
        return true;
    if (sub && sub->root && sub->root->cls->key(sub->root, ev))
        return true;
    switch (ev->key) {
    case KEY_UP: step_hover(w, -1); return true;
    case KEY_DOWN: step_hover(w, 1); return true;
    case KEY_HOME: set_hover(w, -1); step_hover(w, 1); return true;
    case KEY_END: set_hover(w, -1); step_hover(w, -1); return true;
    case KEY_PAGEUP: for (int k = 0; k < 10; k++) step_hover(w, -1); return true;
    case KEY_PAGEDOWN: for (int k = 0; k < 10; k++) step_hover(w, 1); return true;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_SPACE:
        if (ev->value == 1)
            activate(w, p->hover);
        return true;
    case KEY_ESC:
        ui_popup_close(w->win->parent);
        return true;
    case KEY_LEFT:
        if (w->win->parent && w->win->parent->is_popup)
            ui_popup_close(w->win->parent);
        else if (p->owner && !strcmp(p->owner->tag, "menubar"))
            menubar_step(p->owner, -1);
        return true;
    case KEY_RIGHT:
        if (p->hover >= 0 && p->e[p->hover].sub) {
            open_submenu(w, p->hover);
            if (w->win->popup && w->win->popup->root)
                step_hover(w->win->popup->root, 1);
        } else if (p->owner && !strcmp(p->owner->tag, "menubar")) {
            menubar_step(p->owner, 1);
        }
        return true;
    case KEY_TAB:
        return true;
    }
    if (ev->text[0] && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        // Jump to the next entry starting with the typed letter.
        for (int k = 1; k <= p->n; k++) {
            int i = (MAX(p->hover, 0) + k) % p->n;

            if (!p->e[i].sep && p->e[i].enabled && !strncasecmp(p->e[i].text, ev->text, strlen(ev->text))) {
                set_hover(w, i);
                break;
            }
        }
    }
    // While a menu is open, it has the keyboard.
    return true;
}

static void popup_free(struct widget *w)
{
    struct popup *p = w->data;

    if (!p)
        return;
    for (int i = 0; i < p->n; i++) {
        free(p->e[i].text);
        free(p->e[i].shortcut);
    }
    free(p->e);
    free(p);
}

const struct wclass ui_popuplist_class = { "popuplist", .measure = popup_measure, .paint = popup_paint,
                                           .pointer = popup_pointer, .key = popup_key, .free = popup_free };

// ---- Opening popups ----

static void open_popup(struct ui_window *parent, struct widget *owner, int x, int y, struct popup *p, int min_w)
{
    struct ui_window *pw;
    struct widget *root;

    ui_popup_close(parent);
    if (!(pw = ui_window_alloc())) {
        popup_free(&(struct widget){ .data = p });
        return;
    }
    pw->is_popup = true;
    pw->autoshow = false;
    pw->parent = parent;
    pw->flags = WM_ROLE_POPUP;
    pw->title = strdup("");
    pw->x = x;
    pw->y = y;
    if (!(root = widget_new(pw, "popuplist"))) {
        pw->closing = true;
        popup_free(&(struct widget){ .data = p });
        return;
    }
    root->data = p;
    root->min_w = min_w;
    pw->root = root;
    parent->popup = pw;
    parent->popup_owner = owner;
    ui_window_show(pw);
    ui_redraw(owner);
}

static bool item_enabled(struct widget *c)
{
    const char *d = ui_attr(c, "disabled");

    return c->enabled && !(d && attr_bool(d));
}

static struct popup *menu_entries(struct widget *menu)
{
    struct popup *p = calloc(1, sizeof(*p));
    int n = 0;

    if (!p)
        return NULL;
    p->hover = -1;
    p->e = calloc(MAX(ui_child_count(menu), 1), sizeof(struct pentry));
    if (!p->e) {
        free(p);
        return NULL;
    }
    for (struct widget *c = menu->first; c; c = c->next) {
        struct pentry *e = &p->e[n];
        const char *hidden = ui_attr(c, "hidden");

        if (hidden && attr_bool(hidden))
            continue;
        if (!strcmp(c->tag, "separator")) {
            e->sep = true;
            e->text = strdup("");
        } else if (!strcmp(c->tag, "item") || !strcmp(c->tag, "menuitem") || !strcmp(c->tag, "menu")) {
            const char *sc = ui_attr(c, "shortcut"), *chk = ui_attr(c, "checked");

            e->text = strdup(c->text ? c->text : "");
            e->shortcut = sc ? strdup(sc) : NULL;
            e->enabled = item_enabled(c);
            e->checked = (chk && attr_bool(chk)) || (c->value != 0 && strcmp(c->tag, "menu"));
            e->sub = !strcmp(c->tag, "menu");
            e->src = c;
        } else {
            continue;
        }
        n++;
    }
    p->n = n;
    return p;
}

void ui_menu_open(struct widget *owner, struct widget *menu, int x, int y, int min_w)
{
    struct popup *p;

    if (!owner || !menu || !owner->win || !(p = menu_entries(menu)))
        return;
    p->owner = owner;
    open_popup(owner->win, owner, x, y, p, min_w);
}

void ui_menu_popup(struct widget *menu, struct widget *at, int x, int y)
{
    struct widget *owner = at ? at : menu;

    if (!owner || !owner->win)
        return;
    if (x < 0 || y < 0) {
        x = owner->win->ptr_x;
        y = owner->win->ptr_y;
    }
    ui_menu_open(owner, menu, x, y, 0);
}

void ui_popup_list_open(struct widget *owner, int x, int y, int w, int count, int selected,
                        const char *(*item)(struct widget *, int), void (*chosen)(struct widget *, int))
{
    struct popup *p;

    if (just_closed(owner) || !count || !(p = calloc(1, sizeof(*p))))
        return;
    if (!(p->e = calloc(count, sizeof(struct pentry)))) {
        free(p);
        return;
    }
    for (int i = 0; i < count; i++) {
        const char *t = item(owner, i), *tab;

        t = t ? t : "";
        tab = strchr(t, '\t');
        p->e[i].text = tab ? strndup(t, tab - t) : strdup(t);
        p->e[i].enabled = true;
        p->e[i].checked = i == selected;
    }
    p->n = count;
    p->is_list = true;
    p->owner = owner;
    p->chosen = chosen;
    p->hover = selected;
    p->top = MAX(0, MIN(selected - POPUP_ROWS_MAX / 2, count - POPUP_ROWS_MAX));
    open_popup(owner->win, owner, x, y, p, w);
}

// ---- Menu bar ----

struct menubar {
    int open, hover;
};

static void menubar_init(struct widget *w)
{
    struct menubar *m = calloc(1, sizeof(*m));

    w->data = m;
    if (m)
        m->open = m->hover = -1;
}

static int title_w(struct widget *menu)
{
    return text_width(ui_font(), menu->text ? menu->text : "", -1) + 22;
}

static void menubar_measure(struct widget *w, int avail, int *pw, int *ph)
{
    int sum = 8;

    (void)avail;
    for (struct widget *c = w->first; c; c = c->next)
        if (!strcmp(c->tag, "menu"))
            sum += title_w(c);
    *pw = sum;
    *ph = ui_theme.row_height + 2;
}

static int menubar_at(struct widget *w, int x, struct widget **menu, int *mx)
{
    int tx = w->r.x + 4, i = 0;

    for (struct widget *c = w->first; c; c = c->next) {
        if (strcmp(c->tag, "menu"))
            continue;
        if (x >= tx && x < tx + title_w(c)) {
            if (menu)
                *menu = c;
            if (mx)
                *mx = tx;
            return i;
        }
        tx += title_w(c);
        i++;
    }
    return -1;
}

static bool menubar_open_now(struct widget *w)
{
    return w->win->popup && w->win->popup_owner == w;
}

static void menubar_paint(struct widget *w, struct gfx *g)
{
    struct menubar *m = w->data;
    int tx = w->r.x + 4, i = 0;
    bool open = menubar_open_now(w);

    // The pointer may have left without the bar hearing of it.
    m->hover = w->hover && rect_contains(w->r, w->win->ptr_x, w->win->ptr_y)
                   ? menubar_at(w, w->win->ptr_x, NULL, NULL) : -1;

    gfx_fill(g, w->r, ui_theme.surface);
    gfx_fill(g, (struct rect){ w->r.x, w->r.y + w->r.h - 1, w->r.w, 1 }, ui_theme.border);
    for (struct widget *c = w->first; c; c = c->next) {
        struct rect r;

        if (strcmp(c->tag, "menu"))
            continue;
        r = (struct rect){ tx, w->r.y + 3, title_w(c), w->r.h - 6 };
        if (open && i == m->open)
            gfx_fill_rounded(g, r, 4, ui_theme.pressed);
        else if (i == m->hover)
            gfx_fill_rounded(g, r, 4, ui_theme.hover);
        ui_draw_text(g, ui_font(), r, c->text ? c->text : "", c->enabled ? ui_theme.text : ui_theme.text_dim,
                     ALIGN_CENTER);
        tx += title_w(c);
        i++;
    }
}

static void menubar_open(struct widget *w, int i)
{
    struct menubar *m = w->data;
    int tx = w->r.x + 4, k = 0;

    for (struct widget *c = w->first; c; c = c->next) {
        if (strcmp(c->tag, "menu"))
            continue;
        if (k == i) {
            m->open = i;
            ui_menu_open(w, c, tx, w->r.y + w->r.h - 1, 0);
            if (w->win->popup && w->win->popup->root && w->win->focus_visible)
                step_hover(w->win->popup->root, 1);
            ui_redraw(w);
            return;
        }
        tx += title_w(c);
        k++;
    }
}

static void menubar_step(struct widget *bar, int dir)
{
    struct menubar *m = bar->data;
    int n = 0;

    for (struct widget *c = bar->first; c; c = c->next)
        if (!strcmp(c->tag, "menu"))
            n++;
    if (!n)
        return;
    bar->win->focus_visible = true;
    menubar_open(bar, (m->open + dir + n) % n);
}

static bool menubar_pointer(struct widget *w, struct wm_event *ev)
{
    struct menubar *m = w->data;
    int i = menubar_at(w, ev->x, NULL, NULL);

    switch (ev->kind) {
    case WM_PTR_MOVE:
        if (i != m->hover) {
            m->hover = i;
            ui_redraw(w);
        }
        if (i >= 0 && menubar_open_now(w) && i != m->open)
            menubar_open(w, i);
        return true;
    case WM_PTR_LEAVE:
        m->hover = -1;
        ui_redraw(w);
        return true;
    case WM_PTR_DOWN:
        if (i < 0 || ev->detail != BTN_LEFT)
            return true;
        if (menubar_open_now(w) && m->open == i)
            ui_popup_close(w->win);
        else if (!(just_closed(w) && m->open == i)) {
            w->win->focus_visible = false;
            menubar_open(w, i);
        }
        return true;
    }
    return true;
}

const struct wclass ui_menubar_class = { "menubar", .init = menubar_init, .measure = menubar_measure,
                                         .paint = menubar_paint, .pointer = menubar_pointer, .free = NULL,
                                         .paints_children = true };

static void menu_init(struct widget *w)
{
    // Menus are shown as popups, never laid out in place.
    w->visible = false;
}

const struct wclass ui_menu_class = { "menu", .init = menu_init };
const struct wclass ui_menuitem_class = { "menuitem", .init = menu_init };

// ---- Keyboard shortcuts ----

static const struct {
    const char *name;
    uint16_t key;
} key_names[] = {
    { "enter", KEY_ENTER }, { "return", KEY_ENTER }, { "esc", KEY_ESC }, { "escape", KEY_ESC },
    { "backspace", KEY_BACKSPACE }, { "tab", KEY_TAB }, { "space", KEY_SPACE }, { "minus", KEY_MINUS },
    { "-", KEY_MINUS }, { "plus", KEY_EQUAL }, { "=", KEY_EQUAL }, { "equal", KEY_EQUAL },
    { "[", KEY_LEFTBRACE }, { "]", KEY_RIGHTBRACE }, { ";", KEY_SEMICOLON }, { ",", KEY_COMMA },
    { ".", KEY_DOT }, { "/", KEY_SLASH }, { "\\", KEY_BACKSLASH }, { "`", KEY_GRAVE },
    { "insert", KEY_INSERT }, { "ins", KEY_INSERT }, { "delete", KEY_DELETE }, { "del", KEY_DELETE },
    { "home", KEY_HOME }, { "end", KEY_END }, { "pageup", KEY_PAGEUP }, { "pgup", KEY_PAGEUP },
    { "pagedown", KEY_PAGEDOWN }, { "pgdn", KEY_PAGEDOWN }, { "up", KEY_UP }, { "down", KEY_DOWN },
    { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "printscreen", KEY_SYSRQ }, { "menu", KEY_COMPOSE },
};

bool ui_parse_shortcut(const char *s, uint16_t *key, uint32_t *mods)
{
    char part[24];

    *key = 0;
    *mods = 0;
    while (*s) {
        const char *plus = strchr(s + 1, '+');
        int len = plus ? plus - s : (int)strlen(s);

        if (len >= (int)sizeof(part))
            return false;
        for (int i = 0; i < len; i++)
            part[i] = tolower((unsigned char)s[i]);
        part[len] = 0;
        s += len + (plus ? 1 : 0);
        if (!strcmp(part, "ctrl") || !strcmp(part, "control")) {
            *mods |= MOD_LCTRL;
        } else if (!strcmp(part, "shift")) {
            *mods |= MOD_LSHIFT;
        } else if (!strcmp(part, "alt")) {
            *mods |= MOD_LALT;
        } else if (!strcmp(part, "super") || !strcmp(part, "meta") || !strcmp(part, "win")) {
            *mods |= MOD_LMETA;
        } else if (len == 1 && part[0] >= 'a' && part[0] <= 'z') {
            *key = KEY_A + part[0] - 'a';
        } else if (len == 1 && part[0] >= '1' && part[0] <= '9') {
            *key = KEY_1 + part[0] - '1';
        } else if (len == 1 && part[0] == '0') {
            *key = KEY_0;
        } else if (part[0] == 'f' && isdigit((unsigned char)part[1])) {
            int n = atoi(part + 1);

            if (n >= 1 && n <= 12)
                *key = KEY_F1 + n - 1;
            else if (n >= 13 && n <= 24)
                *key = KEY_F13 + n - 13;
            else
                return false;
        } else {
            bool found = false;

            for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
                if (!strcmp(part, key_names[i].name)) {
                    *key = key_names[i].key;
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }
    return *key != 0;
}

static uint32_t norm_mods(uint32_t m)
{
    return ((m & MOD_CTRL) ? 1 : 0) | ((m & MOD_SHIFT) ? 2 : 0) | ((m & MOD_ALT) ? 4 : 0) | ((m & MOD_META) ? 8 : 0);
}

static bool enabled_chain(struct widget *w)
{
    for (; w; w = w->parent) {
        if (!item_enabled(w))
            return false;
        // Shortcuts on hidden pages do not fire, except inside menus.
        if (!w->visible && strcmp(w->tag, "menu") && strcmp(w->tag, "item") && strcmp(w->tag, "menuitem"))
            return false;
    }
    return true;
}

static struct widget *find_shortcut(struct widget *w, uint16_t key, uint32_t mods)
{
    const char *sc = ui_attr(w, "shortcut");
    uint16_t k;
    uint32_t m;

    if (sc && ui_parse_shortcut(sc, &k, &m) && k == key && norm_mods(m) == mods && enabled_chain(w))
        return w;
    for (struct widget *c = w->first; c; c = c->next) {
        struct widget *f = find_shortcut(c, key, mods);

        if (f)
            return f;
    }
    return NULL;
}

static struct widget *find_tag(struct widget *w, const char *tag)
{
    if (!w)
        return NULL;
    if (!strcmp(w->tag, tag))
        return w;
    for (struct widget *c = w->first; c; c = c->next) {
        struct widget *f = find_tag(c, tag);

        if (f)
            return f;
    }
    return NULL;
}

bool ui_menu_shortcut(struct ui_window *win, struct wm_event *ev)
{
    struct widget *w;

    if (!win->root || !ev->value)
        return false;
    if (ev->key == KEY_F1 + 9 && !(ev->mods & MOD_KEYS) && (w = find_tag(win->root, "menubar")) && ui_widget_shown(w)) {
        // F10 opens the menu bar.
        win->focus_visible = true;
        menubar_open(w, 0);
        return true;
    }
    if (!(w = find_shortcut(win->root, ev->key, norm_mods(ev->mods))))
        return false;
    if (ev->value == 2 && strcmp(w->tag, "item") && strcmp(w->tag, "menuitem"))
        return true;     // buttons do not auto-repeat
    if (!strcmp(w->tag, "checkbox") || !strcmp(w->tag, "toggle")) {
        ui_set_value(w, w->value ? 0 : 1);
        ui_emit(w, "change");
    } else {
        ui_emit(w, "click");
    }
    return true;
}

// ---- The command palette ----

// Walks the items under a menu bar menu: sends each one as a command, or
// runs number `run`. Returns the next index.
static int menu_commands(struct ui_window *win, struct widget *menu, const char *prefix, int index, int run)
{
    for (struct widget *c = menu->first; c; c = c->next) {
        char label[WM_TEXT_MAX];

        if (!c->visible || !c->enabled)
            continue;
        snprintf(label, sizeof(label), "%s%s%s", prefix, *prefix ? " \xE2\x80\xBA " : "", ui_translate(c->text ? c->text : ""));
        if (!strcmp(c->tag, "menu")) {
            index = menu_commands(win, c, label, index, run);
            continue;
        }
        if ((strcmp(c->tag, "item") && strcmp(c->tag, "menuitem")) || !ui_has_handler(c, "click"))
            continue;
        if (run < 0) {
            const char *sc = ui_attr(c, "shortcut");

            if (sc && *sc)
                snprintf(label + strlen(label), sizeof(label) - strlen(label), "\t%s", sc);
            wm_command_item(win->wm, index, label);
        } else if (run == index) {
            ui_emit(c, "click");
            return -1000000;
        }
        index++;
    }
    return index;
}

void ui_menu_commands(struct ui_window *win, int run)
{
    struct widget *bar = find_tag(win->root, "menubar");

    if (bar && ui_widget_shown(bar))
        menu_commands(win, bar, "", 0, run);
    if (run < 0)
        wm_command_item(win->wm, -1, "");
}
