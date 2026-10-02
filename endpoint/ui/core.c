#include "ui_internal.h"

// The widget tree, layout, painting, event routing and the event loop.

struct ui_window *ui_windows;

struct timer {
    int id;
    uint64_t interval, next;
    bool (*fn)(void *);
    void *user;
    bool dead;
};

struct watch {
    int fd;
    void (*fn)(int, void *);
    void *user;
};

static struct timer *timers;
static int ntimers, timer_cap, next_timer_id = 1;
static struct watch *watches;
static int nwatches, watch_cap;
static bool quitting;
static int quit_code;
static const char *(*translator)(const char *);

extern const struct wclass *const ui_classes[];

// ---- Classes and widgets ----

const struct wclass *class_for(const char *tag)
{
    for (int i = 0; ui_classes[i]; i++) {
        if (!strcmp(ui_classes[i]->tag, tag))
            return ui_classes[i];
    }
    return NULL;
}

struct widget *widget_new(struct ui_window *win, const char *tag)
{
    const struct wclass *cls = class_for(tag);
    struct widget *w;

    if (!cls)
        return NULL;
    if (!(w = calloc(1, sizeof(*w))))
        return NULL;
    w->cls = cls;
    strlcpy(w->tag, tag, sizeof(w->tag));
    w->win = win;
    w->fixed_w = w->fixed_h = -1;
    w->enabled = w->visible = true;
    w->focusable = cls->focusable;
    w->max = 100;
    w->step = 1;
    w->halign = ALIGN_START;
    if (cls->init)
        cls->init(w);
    return w;
}

struct widget *ui_create(struct ui_window *win, const char *tag)
{
    struct widget *w = widget_new(win, tag);

    if (!w)
        dprintf(STDERR_FILENO, "ui: unknown element <%s>\n", tag);
    return w;
}

static void detach(struct widget *w)
{
    struct widget *p = w->parent;

    if (!p)
        return;
    if (w->prev)
        w->prev->next = w->next;
    else
        p->first = w->next;
    if (w->next)
        w->next->prev = w->prev;
    else
        p->last = w->prev;
    w->parent = w->prev = w->next = NULL;
}

static void forget(struct ui_window *win, struct widget *w)
{
    // Drop window references into a subtree that is going away.
    for (struct widget *x = win->focus; x; x = x->parent)
        if (x == w) {
            win->focus = NULL;
            break;
        }
    for (struct widget *x = win->hover; x; x = x->parent)
        if (x == w) {
            win->hover = NULL;
            break;
        }
    for (struct widget *x = win->capture; x; x = x->parent)
        if (x == w) {
            win->capture = NULL;
            break;
        }
    for (struct widget *x = win->last_click; x; x = x->parent)
        if (x == w) {
            win->last_click = NULL;
            break;
        }
    for (struct widget *x = win->popup_owner; x; x = x->parent)
        if (x == w) {
            ui_popup_close(win);
            break;
        }
}

void widget_free(struct widget *w)
{
    struct widget *c, *n;

    if (!w)
        return;
    for (c = w->first; c; c = n) {
        n = c->next;
        c->parent = NULL;
        widget_free(c);
    }
    if (w->cls->free)
        w->cls->free(w);
    for (struct attr *a = w->attrs, *an; a; a = an) {
        an = a->next;
        free(a->name);
        free(a->value);
        free(a);
    }
    for (struct handler *h = w->handlers, *hn; h; h = hn) {
        hn = h->next;
        free(h);
    }
    free(w->id);
    free(w->text);
    free(w);
}

void ui_add(struct widget *parent, struct widget *child)
{
    if (!parent || !child)
        return;
    detach(child);
    child->parent = parent;
    child->prev = parent->last;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
    if (child->win)
        ui_relayout(child->win);
}

void ui_remove(struct widget *w)
{
    struct ui_window *win;

    if (!w)
        return;
    win = w->win;
    if (win) {
        forget(win, w);
        if (win->root == w)
            win->root = NULL;
        ui_relayout(win);
    }
    detach(w);
    widget_free(w);
}

int ui_child_count(struct widget *w)
{
    int n = 0;

    for (struct widget *c = w->first; c; c = c->next)
        n++;
    return n;
}

struct widget *ui_child_at(struct widget *w, int index)
{
    struct widget *c = w->first;

    while (c && index-- > 0)
        c = c->next;
    return c;
}

bool ui_widget_shown(struct widget *w)
{
    for (; w; w = w->parent)
        if (!w->visible)
            return false;
    return true;
}

// ---- Attributes ----

bool attr_bool(const char *v)
{
    return !v || !*v || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "on");
}

static enum align parse_align(const char *v)
{
    if (!strcmp(v, "start") || !strcmp(v, "left") || !strcmp(v, "top"))
        return ALIGN_START;
    if (!strcmp(v, "center") || !strcmp(v, "middle"))
        return ALIGN_CENTER;
    if (!strcmp(v, "end") || !strcmp(v, "right") || !strcmp(v, "bottom"))
        return ALIGN_END;
    return ALIGN_STRETCH;
}

const char *ui_attr(struct widget *w, const char *name)
{
    if (!w)
        return NULL;
    for (struct attr *a = w->attrs; a; a = a->next) {
        if (!strcmp(a->name, name))
            return a->value;
    }
    return NULL;
}

static void store_attr(struct widget *w, const char *name, const char *value)
{
    struct attr *a;

    for (a = w->attrs; a; a = a->next) {
        if (!strcmp(a->name, name)) {
            char *v = strdup(value);

            if (v) {
                free(a->value);
                a->value = v;
            }
            return;
        }
    }
    if (!(a = calloc(1, sizeof(*a))) || !(a->name = strdup(name)) || !(a->value = strdup(value))) {
        if (a)
            free(a->name);
        free(a);
        return;
    }
    a->next = w->attrs;
    w->attrs = a;
}

static ui_handler find_handler(struct ui_window *win, const char *name)
{
    if (!win || !win->handlers)
        return NULL;
    for (const struct ui_handler_entry *e = win->handlers; e->name; e++) {
        if (!strcmp(e->name, name))
            return e->fn;
    }
    return NULL;
}

void widget_set_attr(struct widget *w, const char *name, const char *value)
{
    if (!value)
        value = "";
    store_attr(w, name, value);

    if (!strcmp(name, "id")) {
        free(w->id);
        w->id = strdup(value);
    } else if (!strcmp(name, "text") || (!strcmp(name, "title") && !strcmp(w->tag, "group"))) {
        ui_set_text(w, value);
    } else if (!strcmp(name, "expand")) {
        w->expand = !strcmp(value, "false") ? 0 : isdigit(*value) ? atoi(value) : 1;
    } else if (!strcmp(name, "width")) {
        w->fixed_w = atoi(value);
    } else if (!strcmp(name, "height")) {
        w->fixed_h = atoi(value);
    } else if (!strcmp(name, "minwidth")) {
        w->min_w = atoi(value);
    } else if (!strcmp(name, "minheight")) {
        w->min_h = atoi(value);
    } else if (!strcmp(name, "padding")) {
        w->padding = atoi(value);
    } else if (!strcmp(name, "spacing")) {
        w->spacing = atoi(value);
    } else if (!strcmp(name, "align")) {
        w->align = parse_align(value);
    } else if (!strcmp(name, "textalign")) {
        w->halign = parse_align(value);
    } else if (!strcmp(name, "disabled")) {
        ui_set_enabled(w, !attr_bool(value));
    } else if (!strcmp(name, "enabled")) {
        ui_set_enabled(w, attr_bool(value));
    } else if (!strcmp(name, "hidden")) {
        ui_set_visible(w, !attr_bool(value));
    } else if (!strcmp(name, "visible")) {
        ui_set_visible(w, attr_bool(value));
    } else if (!strcmp(name, "min")) {
        w->min = strtod(value, NULL);
    } else if (!strcmp(name, "max")) {
        w->max = strtod(value, NULL);
    } else if (!strcmp(name, "step")) {
        w->step = strtod(value, NULL);
    } else if (!strcmp(name, "value") || !strcmp(name, "checked") || !strcmp(name, "selected")) {
        double v = !strcmp(name, "value") ? strtod(value, NULL) : attr_bool(value);

        // Lists and dropdowns pick their selection after their items exist.
        ui_set_value(w, v);
    } else if (!strcmp(name, "focusable")) {
        w->focusable = attr_bool(value);
    } else if (!strncmp(name, "on", 2) && name[2]) {
        ui_handler fn = find_handler(w->win, value);

        if (fn)
            ui_set_handler(w, name + 2, fn, w->win ? w->win->user : NULL);
        else if (*value)
            dprintf(STDERR_FILENO, "ui: no handler named \"%s\" for %s\n", value, name);
    }
    if (w->cls->attr)
        w->cls->attr(w, name, value);
    if (w->win)
        ui_relayout(w->win);
}

void ui_set_attr(struct widget *w, const char *name, const char *value)
{
    if (w)
        widget_set_attr(w, name, value);
}

const char *ui_id(struct widget *w)
{
    return w ? w->id : NULL;
}

// ---- Handlers ----

void ui_set_handler(struct widget *w, const char *event, ui_handler fn, void *user)
{
    struct handler *h;

    if (!w)
        return;
    for (h = w->handlers; h; h = h->next) {
        if (!strcmp(h->event, event)) {
            h->fn = fn;
            h->user = user;
            return;
        }
    }
    if (!(h = calloc(1, sizeof(*h))))
        return;
    strlcpy(h->event, event, sizeof(h->event));
    h->fn = fn;
    h->user = user;
    h->next = w->handlers;
    w->handlers = h;
}

bool ui_has_handler(struct widget *w, const char *event)
{
    for (struct handler *h = w->handlers; h; h = h->next) {
        if (!strcmp(h->event, event) && h->fn)
            return true;
    }
    return false;
}

void ui_emit(struct widget *w, const char *event)
{
    for (struct handler *h = w->handlers; h; h = h->next) {
        if (!strcmp(h->event, event) && h->fn) {
            h->fn(w, h->user);
            return;
        }
    }
}

// ---- Text and values ----

void ui_set_text(struct widget *w, const char *text)
{
    char *t;

    if (!w)
        return;
    if (!text)
        text = "";
    if (w->text && !strcmp(w->text, text))
        return;
    if (!(t = strdup(text)))
        return;
    free(w->text);
    w->text = t;
    if (w->cls->text_changed)
        w->cls->text_changed(w);
    if (w->win) {
        ui_relayout(w->win);
        ui_redraw(w);
    }
}

const char *ui_text(struct widget *w)
{
    return w && w->text ? w->text : "";
}

void ui_set_value(struct widget *w, double v)
{
    if (!w)
        return;
    if (w->max > w->min) {
        if (v < w->min)
            v = w->min;
        if (v > w->max)
            v = w->max;
    }
    if (v == w->value)
        return;
    w->value = v;
    if (w->cls->value_changed)
        w->cls->value_changed(w);
    ui_redraw(w);
}

double ui_value(struct widget *w)
{
    return w ? w->value : 0;
}

void ui_set_enabled(struct widget *w, bool enabled)
{
    if (!w || w->enabled == enabled)
        return;
    w->enabled = enabled;
    if (!enabled && w->win && w->win->focus == w)
        w->win->focus = NULL;
    ui_redraw(w);
}

void ui_set_visible(struct widget *w, bool visible)
{
    if (!w || w->visible == visible)
        return;
    w->visible = visible;
    if (w->win) {
        if (!visible)
            forget(w->win, w);
        ui_relayout(w->win);
    }
}

bool ui_visible(struct widget *w)
{
    return w && ui_widget_shown(w);
}

struct ui_window *ui_window_of(struct widget *w)
{
    return w ? w->win : NULL;
}

static struct widget *find_id(struct widget *w, const char *id)
{
    if (!w)
        return NULL;
    if (w->id && !strcmp(w->id, id))
        return w;
    for (struct widget *c = w->first; c; c = c->next) {
        struct widget *f = find_id(c, id);

        if (f)
            return f;
    }
    return NULL;
}

struct widget *ui_get(struct ui_window *win, const char *id)
{
    struct widget *w = win ? find_id(win->root, id) : NULL;

    if (!w && win)
        dprintf(STDERR_FILENO, "ui: no element with id \"%s\"\n", id);
    return w;
}

void ui_set_translator(const char *(*fn)(const char *key))
{
    translator = fn;
}

// Translates the toolkit's own strings ("OK", "Cancel", ...).
const char *ui_tr(const char *text)
{
    const char *t = translator ? translator(text) : NULL;

    return t ? t : text;
}

const char *ui_translate(const char *text)
{
    const char *t;

    if (text && text[0] == '@' && text[1] != '@' && translator && (t = translator(text + 1)))
        return t;
    if (text && text[0] == '@' && text[1] == '@')
        return text + 1;
    return text;
}

// ---- Layout ----

void ui_measure(struct widget *w, int avail_w)
{
    int pw = 0, ph = 0;

    if (!w->visible) {
        w->pref_w = w->pref_h = 0;
        return;
    }
    if (w->fixed_w >= 0)
        avail_w = w->fixed_w;
    if (w->cls->measure)
        w->cls->measure(w, avail_w, &pw, &ph);
    if (w->fixed_w >= 0)
        pw = w->fixed_w;
    if (w->fixed_h >= 0)
        ph = w->fixed_h;
    w->pref_w = MAX(pw, w->min_w);
    w->pref_h = MAX(ph, w->min_h);
}

void ui_place(struct widget *w, struct rect r)
{
    w->r = r;
    if (w->visible && w->cls->arrange)
        w->cls->arrange(w);
}

void ui_window_measure(struct ui_window *win, int *w, int *h)
{
    int avail = win->width > 0 ? win->width : -1;

    ui_measure(win->root, avail);
    *w = win->width > 0 ? win->width : win->root->pref_w;
    *h = win->height > 0 ? win->height : win->root->pref_h;
    if (win->width <= 0 || win->height <= 0) {
        // The height of wrapped text depends on the final width.
        ui_measure(win->root, *w);
        if (win->height <= 0)
            *h = win->root->pref_h;
    }
}

void ui_window_layout(struct ui_window *win)
{
    struct wm_window *wm = win->wm;

    win->needs_layout = false;
    if (!win->root || !wm)
        return;
    ui_measure(win->root, wm->width);
    ui_place(win->root, (struct rect){ 0, 0, wm->width, wm->height });
    ui_damage(win, (struct rect){ 0, 0, wm->width, wm->height });
}

void ui_relayout(struct ui_window *win)
{
    if (win)
        win->needs_layout = true;
}

// ---- Painting ----

void ui_damage(struct ui_window *win, struct rect r)
{
    struct rect all;

    if (!win || !win->wm)
        return;
    all = (struct rect){ 0, 0, win->wm->width, win->wm->height };
    if (!rect_intersect(r, all, &r))
        return;
    win->dirty = win->has_dirty ? rect_union(win->dirty, r) : r;
    win->has_dirty = true;
}

void ui_redraw(struct widget *w)
{
    struct rect r;

    if (!w || !w->win || !ui_widget_shown(w))
        return;
    // Include the focus ring drawn just outside the widget.
    r = (struct rect){ w->r.x - 3, w->r.y - 3, w->r.w + 6, w->r.h + 6 };
    // Clip to scrolled ancestors.
    for (struct widget *p = w->parent; p; p = p->parent) {
        if (p->cls->paints_children && !rect_intersect(r, p->r, &r))
            return;
    }
    ui_damage(w->win, r);
}

void ui_paint_children(struct widget *w, struct gfx *g)
{
    for (struct widget *c = w->first; c; c = c->next)
        ui_paint_widget(c, g);
}

void ui_paint_widget(struct widget *w, struct gfx *g)
{
    struct rect vis, grow;

    if (!w->visible)
        return;
    grow = (struct rect){ w->r.x - 3, w->r.y - 3, w->r.w + 6, w->r.h + 6 };
    if (!rect_intersect(grow, g->clip, &vis))
        return;
    if (w->cls->paint)
        w->cls->paint(w, g);
    if (!w->cls->paints_children)
        ui_paint_children(w, g);
}

static void repaint(struct ui_window *win)
{
    struct gfx g;
    struct rect r = win->dirty;

    win->has_dirty = false;
    if (!win->wm || !win->root)
        return;
    gfx_init(&g, win->wm->surface);
    if (!rect_intersect(r, g.clip, &g.clip))
        return;
    gfx_fill(&g, g.clip, win->is_popup ? ui_theme.surface : ui_theme.window);
    ui_paint_widget(win->root, &g);
    if (win->is_popup)
        gfx_outline(&g, (struct rect){ 0, 0, win->wm->width, win->wm->height }, 1, ui_theme.border);
    wm_present(win->wm, r);
}

// ---- Focus ----

bool ui_is_focused(struct widget *w)
{
    return w && w->win && w->win->focus == w && w->win->wm && w->win->wm->focused;
}

static bool can_focus(struct widget *w)
{
    for (struct widget *p = w; p; p = p->parent)
        if (!p->enabled || !p->visible)
            return false;
    return w->focusable;
}

static void scroll_into_view(struct widget *w)
{
    for (struct widget *p = w->parent; p; p = p->parent) {
        if (!strcmp(p->tag, "scroll")) {
            int top = w->r.y - p->r.y, bottom = w->r.y + w->r.h - (p->r.y + p->r.h);
            int *offset = p->data;

            if (top < 0)
                *offset += top - 4;
            else if (bottom > 0 && w->r.h < p->r.h)
                *offset += bottom + 4;
            else
                continue;
            ui_place(p, p->r);
            ui_redraw(p);
        }
    }
}

void ui_focus(struct widget *w)
{
    struct ui_window *win;
    struct widget *old;

    if (!w || !(win = w->win) || win->focus == w || !can_focus(w))
        return;
    old = win->focus;
    win->focus = w;
    if (old) {
        if (old->cls->focus)
            old->cls->focus(old, false);
        ui_redraw(old);
    }
    if (w->cls->focus)
        w->cls->focus(w, true);
    scroll_into_view(w);
    ui_redraw(w);
}

static void collect_focusable(struct widget *w, struct widget **list, int *n, int max)
{
    if (!w->visible || !w->enabled)
        return;
    if (w->focusable && *n < max)
        list[(*n)++] = w;
    for (struct widget *c = w->first; c; c = c->next)
        collect_focusable(c, list, n, max);
}

void ui_focus_next(struct ui_window *win, bool backwards)
{
    struct widget *list[512];
    int n = 0, at = -1;

    if (!win->root)
        return;
    collect_focusable(win->root, list, &n, 512);
    if (!n)
        return;
    for (int i = 0; i < n; i++)
        if (list[i] == win->focus)
            at = i;
    if (at < 0)
        at = backwards ? 0 : n - 1;
    at = (at + (backwards ? n - 1 : 1)) % n;
    win->focus_visible = true;
    ui_focus(list[at]);
}

// ---- Hit testing and pointer routing ----

struct widget *ui_hit(struct widget *w, int x, int y)
{
    struct widget *found = NULL;

    if (!w || !w->visible || !rect_contains(w->r, x, y))
        return NULL;
    for (struct widget *c = w->first; c; c = c->next) {
        struct widget *h = ui_hit(c, x, y);

        if (h)
            found = h;      // later children are on top
    }
    return found ? found : w;
}

int ui_click_count(struct widget *w)
{
    return w && w->win && w->win->last_click == w ? w->win->clicks : 1;
}

void ui_set_cursor(struct ui_window *win, int cursor)
{
    if (win->cursor != cursor && win->wm) {
        win->cursor = cursor;
        wm_set_cursor(win->wm, cursor);
    }
}

static void set_hover(struct ui_window *win, struct widget *w)
{
    if (win->hover == w)
        return;
    for (struct widget *p = win->hover; p; p = p->parent) {
        p->hover = false;
        ui_redraw(p);
    }
    win->hover = w;
    for (struct widget *p = w; p; p = p->parent)
        p->hover = true;
    if (w)
        ui_redraw(w);
}

static struct ui_window *modal_window(void)
{
    struct ui_window *m = NULL;

    for (struct ui_window *w = ui_windows; w; w = w->next)
        if (w->modal && !w->closing && w->shown)
            m = w;      // the newest one
    return m;
}

static bool blocked_by_modal(struct ui_window *win)
{
    struct ui_window *m = modal_window();

    if (!m || m == win)
        return false;
    // Popups belong to their owner window.
    for (struct ui_window *p = win; p; p = p->parent)
        if (p == m)
            return false;
    return true;
}

static void pointer_event(struct ui_window *win, struct wm_event *ev)
{
    struct widget *target;

    if (!win->root)
        return;
    win->ptr_x = ev->x;
    win->ptr_y = ev->y;
    if (ev->kind == WM_PTR_LEAVE) {
        if (!win->capture)
            set_hover(win, NULL);
        return;
    }
    if (blocked_by_modal(win))
        return;
    target = win->capture ? win->capture : ui_hit(win->root, ev->x, ev->y);
    if (ev->kind == WM_PTR_MOVE || ev->kind == WM_PTR_ENTER) {
        if (!win->capture) {
            struct widget *h = ui_hit(win->root, ev->x, ev->y);

            set_hover(win, h);
            for (; h && !(h->enabled && h->cls->cursor); h = h->parent)
                ;
            ui_set_cursor(win, h && h->enabled ? h->cls->cursor : WM_CURSOR_ARROW);
        }
        for (struct widget *w = target; w; w = w->parent) {
            if (w->enabled && w->cls->pointer && w->cls->pointer(w, ev))
                break;
        }
        return;
    }
    if (ev->kind == WM_PTR_DOWN) {
        struct widget *f;
        uint64_t now = uptime_ms();

        if (!target)
            return;
        // Focus the nearest focusable widget under the pointer.
        for (f = target; f && !can_focus(f); f = f->parent)
            ;
        if (f) {
            win->focus_visible = false;
            ui_focus(f);
        }
        if (ev->detail == BTN_LEFT) {
            if (win->last_click == target && now - win->last_click_ms < 450
                && abs(ev->x - win->last_click_x) < 5 && abs(ev->y - win->last_click_y) < 5)
                win->clicks++;
            else
                win->clicks = 1;
            win->last_click = target;
            win->last_click_ms = now;
            win->last_click_x = ev->x;
            win->last_click_y = ev->y;
        }
        win->capture = target;
    }
    for (struct widget *w = target; w; w = w->parent) {
        if (w->enabled && w->cls->pointer && w->cls->pointer(w, ev)) {
            if (ev->kind == WM_PTR_DOWN)
                win->capture = w;
            break;
        }
    }
    if (ev->kind == WM_PTR_UP && !ev->buttons) {
        win->capture = NULL;
        set_hover(win, ui_hit(win->root, ev->x, ev->y));
    }
}

static struct widget *find_flagged(struct widget *w, const char *attr)
{
    if (!w || !w->visible)
        return NULL;
    if (!strcmp(w->tag, "button") && attr_bool(ui_attr(w, attr) ? ui_attr(w, attr) : "false") && w->enabled)
        return w;
    for (struct widget *c = w->first; c; c = c->next) {
        struct widget *f = find_flagged(c, attr);

        if (f)
            return f;
    }
    return NULL;
}

static void key_event(struct ui_window *win, struct wm_event *ev)
{
    bool handled = false, press = ev->value != 0;

    if (blocked_by_modal(win))
        return;
    if (win->popup && win->popup->root && press) {
        struct widget *r = win->popup->root;

        if (r->cls->key && r->cls->key(r, ev))
            return;
    }
    for (struct widget *w = win->focus; w && !handled; w = w->parent) {
        if (w->enabled && w->cls->key)
            handled = w->cls->key(w, ev);
    }
    if (!handled && press) {
        if (ev->key == KEY_TAB && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
            ui_focus_next(win, ev->mods & MOD_SHIFT);
            handled = true;
        } else if ((ev->key == KEY_ENTER || ev->key == KEY_KPENTER) && !ev->mods) {
            struct widget *b = find_flagged(win->root, "default");

            if (b && ev->value == 1) {
                ui_emit(b, "click");
                handled = true;
            }
        } else if (ev->key == KEY_ESC) {
            struct widget *b = find_flagged(win->root, "cancel");

            if (b && ev->value == 1) {
                ui_emit(b, "click");
                handled = true;
            }
        }
        if (!handled)
            handled = ui_menu_shortcut(win, ev);
    }
    if (win->on_key && !win->closing)
        win->on_key(win, ev, win->on_key_user);
}

// ---- Windows ----

struct ui_window *ui_window_alloc(void)
{
    struct ui_window *win = calloc(1, sizeof(*win)), **pp;

    if (!win)
        return NULL;
    win->autoshow = true;
    win->cursor = -1;
    // Keep creation order: dialogs end up after their parents.
    for (pp = &ui_windows; *pp; pp = &(*pp)->next)
        ;
    *pp = win;
    return win;
}

struct ui_window *ui_window_new(const char *title, int width, int height, uint32_t flags)
{
    struct ui_window *win = ui_window_alloc();

    if (!win)
        return NULL;
    win->title = strdup(title ? title : "");
    win->width = width;
    win->height = height;
    win->flags = flags;
    win->root = widget_new(win, "window");
    return win;
}

struct widget *ui_root(struct ui_window *win)
{
    return win ? win->root : NULL;
}

struct wm_window *ui_wm_window(struct ui_window *win)
{
    return win ? win->wm : NULL;
}

static bool ensure_display(void)
{
    if (wm_connected())
        return true;
    if (!wm_connect()) {
        dprintf(STDERR_FILENO, "ui: cannot connect to the display\n");
        return false;
    }
    return true;
}

static bool create_wm(struct ui_window *win)
{
    int w, h, sw, sh, work_h;
    uint32_t flags = win->flags | WM_FLAG_HIDDEN;

    if (win->wm)
        return true;
    if (!ensure_display() || !win->root)
        return false;
    ui_window_measure(win, &w, &h);
    wm_screen_size(&sw, &sh, &work_h);
    if ((flags & WM_ROLE_MASK) == WM_ROLE_NORMAL || (flags & WM_ROLE_MASK) == WM_ROLE_DIALOG) {
        w = MIN(MAX(w, 120), sw - 16);
        h = MIN(MAX(h, 40), (work_h ? work_h : sh) - 48);
    }
    if ((flags & WM_ROLE_MASK) == WM_ROLE_NORMAL && win->parent)
        flags = (flags & ~WM_ROLE_MASK) | WM_ROLE_DIALOG;
    win->wm = wm_create_at(win->title, win->is_popup ? win->x : -1, win->is_popup ? win->y : -1, MAX(w, 1),
                           MAX(h, 1), flags,
                           win->parent ? win->parent->wm : NULL);
    if (!win->wm)
        return false;
    win->wm->user = win;
    ui_window_layout(win);
    return true;
}

void ui_window_show(struct ui_window *win)
{
    if (!win || win->closing)
        return;
    win->autoshow = false;
    if (!create_wm(win)) {
        dprintf(STDERR_FILENO, "ui: cannot create the window \"%s\"\n", win->title ? win->title : "");
        exit(1);
    }
    if (!win->shown) {
        // Draw before showing so the window never appears empty.
        if (win->needs_layout)
            ui_window_layout(win);
        win->dirty = (struct rect){ 0, 0, win->wm->width, win->wm->height };
        win->has_dirty = true;
        repaint(win);
        wm_show(win->wm, true);
        win->shown = true;
        if (!win->focus && !win->is_popup) {
            ui_focus_next(win, false);
            win->focus_visible = false;
        }
    }
}

void ui_window_hide(struct ui_window *win)
{
    if (!win)
        return;
    win->autoshow = false;
    if (win->wm && win->shown)
        wm_show(win->wm, false);
    win->shown = false;
}

void ui_window_close(struct ui_window *win)
{
    if (!win || win->closing)
        return;
    win->closing = true;
    if (win->popup)
        ui_popup_close(win);
    if (win->parent && win->parent->popup == win) {
        win->parent->popup = NULL;
        win->parent->popup_owner = NULL;
    }
    // Close dialogs and popups owned by this window first.
    for (struct ui_window *w = ui_windows; w; w = w->next)
        if (w->parent == win)
            ui_window_close(w);
    if (win->wm) {
        wm_destroy(win->wm);
        win->wm = NULL;
    }
}

void ui_window_destroy(struct ui_window *win)
{
    for (struct ui_window **pp = &ui_windows; *pp; pp = &(*pp)->next) {
        if (*pp == win) {
            *pp = win->next;
            break;
        }
    }
    if (win->wm)
        wm_destroy(win->wm);
    widget_free(win->root);
    free(win->title);
    free(win);
}

void ui_window_set_title(struct ui_window *win, const char *title)
{
    if (!win)
        return;
    free(win->title);
    win->title = strdup(title ? title : "");
    if (win->wm)
        wm_set_title(win->wm, win->title);
}

void ui_window_set_size(struct ui_window *win, int width, int height)
{
    if (!win)
        return;
    win->width = width;
    win->height = height;
    if (win->wm)
        wm_resize(win->wm, width, height);
}

void ui_on_close(struct ui_window *win, bool (*fn)(struct ui_window *, void *), void *user)
{
    win->on_close = fn;
    win->on_close_user = user;
}

void ui_on_key(struct ui_window *win, void (*fn)(struct ui_window *, struct wm_event *, void *), void *user)
{
    win->on_key = fn;
    win->on_key_user = user;
}

void *ui_window_user(struct ui_window *win)
{
    return win ? win->user : NULL;
}

// ---- Timers and descriptors ----

int ui_timer(uint64_t ms, bool (*fn)(void *user), void *user)
{
    if (ntimers == timer_cap) {
        int cap = timer_cap ? timer_cap * 2 : 8;
        struct timer *t = realloc(timers, cap * sizeof(*t));

        if (!t)
            return -1;
        timers = t;
        timer_cap = cap;
    }
    timers[ntimers] = (struct timer){ next_timer_id++, MAX(ms, 1), uptime_ms() + MAX(ms, 1), fn, user, false };
    return timers[ntimers++].id;
}

void ui_timer_cancel(int id)
{
    for (int i = 0; i < ntimers; i++)
        if (timers[i].id == id)
            timers[i].dead = true;
}

void ui_watch_fd(int fd, void (*fn)(int fd, void *user), void *user)
{
    for (int i = 0; i < nwatches; i++) {
        if (watches[i].fd == fd) {
            watches[i].fn = fn;
            watches[i].user = user;
            return;
        }
    }
    if (nwatches == watch_cap) {
        int cap = watch_cap ? watch_cap * 2 : 4;
        struct watch *w = realloc(watches, cap * sizeof(*w));

        if (!w)
            return;
        watches = w;
        watch_cap = cap;
    }
    watches[nwatches++] = (struct watch){ fd, fn, user };
}

void ui_unwatch_fd(int fd)
{
    for (int i = 0; i < nwatches; i++) {
        if (watches[i].fd == fd) {
            watches[i] = watches[--nwatches];
            return;
        }
    }
}

static void run_timers(void)
{
    uint64_t now = uptime_ms();
    int n = ntimers;

    for (int i = 0; i < n; i++) {
        struct timer *t = &timers[i];

        if (t->dead || now < t->next)
            continue;
        t->next = now + t->interval;
        if (!t->fn(t->user))
            timers[i].dead = true;      // the array may have moved
    }
    for (int i = 0; i < ntimers;) {
        if (timers[i].dead)
            timers[i] = timers[--ntimers];
        else
            i++;
    }
}

// ---- The event loop ----

static struct ui_window *window_for(struct wm_window *wm)
{
    for (struct ui_window *w = ui_windows; w; w = w->next)
        if (w->wm && w->wm == wm)
            return w;
    return NULL;
}

static void dispatch(struct wm_event *ev)
{
    struct ui_window *win = ev->window ? window_for(ev->window) : NULL;

    if (!win || win->closing)
        return;
    switch (ev->type) {
    case WM_EV_KEY:
        key_event(win, ev);
        break;
    case WM_EV_POINTER:
        pointer_event(win, ev);
        break;
    case WM_EV_RESIZE:
        ui_window_layout(win);
        break;
    case WM_EV_FOCUS:
        if (!ev->focused) {
            if (win->popup)
                ui_popup_close(win);
            win->capture = NULL;
        }
        if (win->focus)
            ui_redraw(win->focus);
        break;
    case WM_EV_CLOSE:
        if (blocked_by_modal(win))
            break;
        if (win->on_close && !win->on_close(win, win->on_close_user))
            break;
        if (win->root && ui_has_handler(win->root, "close")) {
            // An onclose handler decides for itself whether to close.
            ui_emit(win->root, "close");
            break;
        }
        ui_window_close(win);
        break;
    case WM_EV_POPUP_DONE:
        if (win->is_popup && win->parent)
            ui_popup_close(win->parent);
        break;
    default:
        break;
    }
}

static int depth;

static void flush(void)
{
    struct ui_window *w, *next;

    for (w = ui_windows; w; w = next) {
        next = w->next;
        if (w->closing) {
            // A nested loop (a dialog) must not free windows that the
            // outer dispatch may still be using.
            if (depth > 0)
                continue;
            ui_window_destroy(w);
            continue;
        }
        if (w->autoshow && !w->is_popup)
            ui_window_show(w);
        if (!w->wm)
            continue;
        if (w->needs_layout)
            ui_window_layout(w);
        if (w->has_dirty)
            repaint(w);
    }
}

void ui_iterate(int timeout_ms)
{
    struct pollfd fds[33];
    int n = 0, nw;
    uint64_t now;
    struct wm_event ev;

    flush();
    now = uptime_ms();
    for (int i = 0; i < ntimers; i++) {
        int left = timers[i].next > now ? (int)MIN(timers[i].next - now, 1000000) : 0;

        if (!timers[i].dead && (timeout_ms < 0 || left < timeout_ms))
            timeout_ms = left;
    }
    if (wm_connected())
        fds[n++] = (struct pollfd){ wm_fd(), POLLIN, 0 };
    nw = MIN(nwatches, 32);
    for (int i = 0; i < nw; i++)
        fds[n++] = (struct pollfd){ watches[i].fd, POLLIN, 0 };
    if (poll(fds, n, timeout_ms) > 0) {
        int base = wm_connected() ? 1 : 0;

        if (base && fds[0].revents) {
            // Drain everything that is queued before repainting.
            while (wm_next_event(&ev, 0)) {
                if (ev.type != WM_EV_NONE) {
                    depth++;
                    dispatch(&ev);
                    depth--;
                }
            }
        }
        for (int i = 0; i < nw; i++) {
            if (fds[base + i].revents && i < nwatches && watches[i].fd == fds[base + i].fd) {
                depth++;
                watches[i].fn(watches[i].fd, watches[i].user);
                depth--;
            }
        }
    }
    depth++;
    run_timers();
    depth--;
    flush();
}

void ui_quit(int code)
{
    quitting = true;
    quit_code = code;
}

static bool have_windows(void)
{
    for (struct ui_window *w = ui_windows; w; w = w->next)
        if (!w->closing && !w->is_popup)
            return true;
    return false;
}

int ui_run(void)
{
    quitting = false;
    flush();
    while (!quitting && have_windows() && wm_connected())
        ui_iterate(-1);
    return quitting ? quit_code : 0;
}

// ---- Clipboard (per process until the window system shares one) ----

static char *clipboard;

void ui_clipboard_set(const char *text)
{
    free(clipboard);
    clipboard = text ? strdup(text) : NULL;
}

const char *ui_clipboard_get(void)
{
    return clipboard ? clipboard : "";
}
