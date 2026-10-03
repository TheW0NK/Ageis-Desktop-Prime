#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "ui.h"

// Widget kinds. Each has a class with its behaviour (struct wclass).
struct attr {
    char *name, *value;
    struct attr *next;
};

struct handler {
    char event[16];
    ui_handler fn;
    void *user;
    struct handler *next;
};

enum align { ALIGN_STRETCH, ALIGN_START, ALIGN_CENTER, ALIGN_END };

struct widget {
    const struct wclass *cls;
    char tag[16];
    struct ui_window *win;
    struct widget *parent, *first, *last, *next, *prev;
    char *id;
    struct attr *attrs;
    struct handler *handlers;
    char *text;

    struct rect r;                  // window coordinates
    int pref_w, pref_h;             // from the last measure

    // Layout properties (from attributes).
    int expand;                     // share of extra space along the parent's axis
    int fixed_w, fixed_h;           // -1: natural size
    int min_w, min_h;
    int padding, spacing;
    enum align align;               // cross-axis placement within the parent
    enum align halign;              // content alignment (labels)

    bool enabled, visible, hover, pressed, focusable;
    double value, min, max, step;
    void *data;                     // per-class state
    // Drop target (ui_set_drop_target).
    ui_drop_over_fn drop_over;
    ui_drop_fn drop;
    void *drop_user;
};

struct wclass {
    const char *tag;
    bool focusable;
    int cursor;
    void (*init)(struct widget *w);
    // Natural size; avail_w is the width the parent can offer (-1: unknown).
    void (*measure)(struct widget *w, int avail_w, int *pw, int *ph);
    // Positions the children inside w->r.
    void (*arrange)(struct widget *w);
    void (*paint)(struct widget *w, struct gfx *g);
    // Pointer events in window coordinates. Return true if handled.
    bool (*pointer)(struct widget *w, struct wm_event *ev);
    bool (*key)(struct widget *w, struct wm_event *ev);
    void (*attr)(struct widget *w, const char *name, const char *value);
    void (*text_changed)(struct widget *w);
    void (*value_changed)(struct widget *w);
    void (*focus)(struct widget *w, bool focused);
    void (*free)(struct widget *w);
    // Children painted by the class itself (scroll views).
    bool paints_children;
    // Built-in drop target (text fields), used when the app sets none; such
    // widgets show the drop point themselves.
    int (*drop_over)(struct widget *w, struct ui_drop *d);
    void (*drop)(struct widget *w, struct ui_drop *d, int action);
};

struct ui_window {
    struct wm_window *wm;
    struct widget *root;
    struct ui_window *parent;       // dialogs and popups
    char *title;
    int width, height;              // requested (0: natural)
    uint32_t flags;
    bool shown, autoshow, closing, needs_layout, modal;
    struct rect dirty;
    bool has_dirty;
    struct widget *focus, *hover, *capture;
    struct widget *drop_target;     // under a drag, and accepting it
    bool drop_by_class;             // ... through its built-in handler (text fields)
    bool focus_visible;             // focus moved by keyboard: draw the ring
    // Double clicks.
    struct widget *last_click;
    uint64_t last_click_ms;
    int last_click_x, last_click_y;
    int clicks;
    int cursor;
    int x, y;                       // popups: position in the parent's content
    bool positioned;                // x, y set by ui_window_move
    int ptr_x, ptr_y;               // last pointer position
    // The menu or dropdown list open over this window.
    struct ui_window *popup;
    struct widget *popup_owner;
    bool is_popup;
    bool dialog_done;
    int dialog_result;
    bool (*on_close)(struct ui_window *, void *);
    void *on_close_user;
    void (*on_key)(struct ui_window *, struct wm_event *, void *);
    void *on_key_user;
    const struct ui_handler_entry *handlers;
    void (*backdrop)(struct ui_window *, struct gfx *, struct rect, void *);
    void (*overlay)(struct ui_window *, struct gfx *, void *);
    void *overlay_user;
    bool (*pointer_filter)(struct ui_window *, struct widget *, struct wm_event *, void *);
    void *pointer_filter_user;
    void *backdrop_user;
    void *user;
    struct ui_window *next;
};

extern struct ui_window *ui_windows;

// core.c
struct widget *widget_new(struct ui_window *win, const char *tag);
void widget_free(struct widget *w);
const struct wclass *class_for(const char *tag);
void widget_set_attr(struct widget *w, const char *name, const char *value);
bool attr_bool(const char *v);
void ui_emit(struct widget *w, const char *event);
bool ui_has_handler(struct widget *w, const char *event);
void ui_damage(struct ui_window *win, struct rect r);
void ui_measure(struct widget *w, int avail_w);
void ui_place(struct widget *w, struct rect r);
void ui_paint_widget(struct widget *w, struct gfx *g);
void ui_paint_children(struct widget *w, struct gfx *g);
struct widget *ui_hit(struct widget *w, int x, int y);
int ui_click_count(struct widget *w);
void ui_set_cursor(struct ui_window *win, int cursor);
struct ui_window *ui_window_alloc(void);
void ui_window_destroy(struct ui_window *win);
void ui_window_layout(struct ui_window *win);
void ui_window_measure(struct ui_window *win, int *w, int *h);
// One turn of the event loop: waits up to timeout_ms, dispatches and repaints.
void ui_iterate(int timeout_ms);
int ui_child_count(struct widget *w);
struct widget *ui_child_at(struct widget *w, int index);
bool ui_widget_shown(struct widget *w);
void ui_focus_next(struct ui_window *win, bool backwards);
bool ui_is_focused(struct widget *w);
const char *ui_translate(const char *text);
void ui_window_hide(struct ui_window *win);

// Drawing helpers (theme.c).
struct font *ui_font(void);
struct font *ui_font_bold(void);
struct font *ui_font_mono(void);
struct font *ui_font_sized(const char *name, int px);
void ui_draw_text(struct gfx *g, struct font *f, struct rect r, const char *s, color_t c, enum align h);
void ui_draw_focus(struct gfx *g, struct rect r, int radius);
void ui_draw_button_bg(struct gfx *g, struct widget *w, struct rect r, bool primary);
void ui_draw_check(struct gfx *g, float x, float y, float size, color_t c);
void ui_draw_arrow(struct gfx *g, int cx, int cy, int size, int dir, color_t c);  // 0 down 1 up 2 right 3 left
void ui_draw_scrollbar(struct gfx *g, struct rect track, int total, int visible, int offset, bool hot);
// Scrollbar hit test: returns the new offset for a click/drag at y, or -1.
int ui_scrollbar_offset(struct rect track, int total, int visible, int y, int grab);
int ui_scrollbar_thumb(struct rect track, int total, int visible, int offset, int *thumb_h);
void ui_clip(struct gfx *g, struct rect r);
color_t ui_mix(color_t a, color_t b, int t);   // t of 255 toward b

// Drag and drop (core.c).
void ui_drag_forget(struct widget *w);

// text.c: shared helpers for input fields.
int ui_text_width_tabs(struct font *f, const char *s, int len, int tab_w);
// Converts a key press with Ctrl to a clipboard/selection action name, or NULL.
const char *ui_edit_shortcut(struct wm_event *ev);

// list.c
void ui_list_init_from_children(struct widget *w);

// menu.c
void ui_menu_open(struct widget *owner, struct widget *menu, int x, int y, int min_w);
void ui_popup_list_open(struct widget *owner, int x, int y, int w, int count, int selected,
                        const char *(*item)(struct widget *, int), void (*chosen)(struct widget *, int));
void ui_popup_close(struct ui_window *win);
bool ui_menu_shortcut(struct ui_window *win, struct wm_event *ev);
// The command palette asks for the menu bar's commands (run < 0), or runs one.
void ui_menu_commands(struct ui_window *win, int run);
// Parses a shortcut such as "Ctrl+Shift+S". Returns false if invalid.
bool ui_parse_shortcut(const char *s, uint16_t *key, uint32_t *mods);

// aui.c
struct widget *aui_parse(struct ui_window *win, const char *text, struct widget *parent, const char *source);

#endif
