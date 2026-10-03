#ifndef AEGIS_UI_H
#define AEGIS_UI_H

#include "aegis.h"
#include "gfx.h"
#include "wm.h"

// The Aegis UI toolkit. Windows are trees of widgets, usually described in
// AUI markup (an HTML-like language without styling; the theme decides how
// things look):
//
//   <window title="Hello" width="320" height="200">
//     <vbox padding="12" spacing="8">
//       <label text="Your name"/>
//       <input id="name" placeholder="Type here" onactivate="greet"/>
//       <button text="Greet" onclick="greet"/>
//     </vbox>
//   </window>
//
// Handlers named in on* attributes are looked up in the table passed to
// ui_load(). See docs/AUI.md for every element and attribute.

struct widget;
struct ui_window;

typedef void (*ui_handler)(struct widget *w, void *user);

struct ui_handler_entry {
    const char *name;
    ui_handler fn;
};

// ---- Windows ----

struct ui_window *ui_load(const char *path, const struct ui_handler_entry *handlers, void *user);
// Supplies handlers for on* names missing from the table (script apps).
void ui_set_handler_resolver(ui_handler (*fn)(const char *name, void **user));
struct ui_window *ui_load_string(const char *aui, const struct ui_handler_entry *handlers, void *user);
// A window built in code: add children to ui_root(win).
struct ui_window *ui_window_new(const char *title, int width, int height, uint32_t flags);
struct widget *ui_root(struct ui_window *win);
struct ui_window *ui_load_string_named(const char *aui, const struct ui_handler_entry *handlers, void *user,
                                       const char *source_name);
void ui_window_show(struct ui_window *win);
void ui_window_hide(struct ui_window *win);
void ui_window_close(struct ui_window *win);
void ui_window_set_title(struct ui_window *win, const char *title);
void ui_window_set_size(struct ui_window *win, int width, int height);
// Resizes the window to its content's natural height.
void ui_window_fit(struct ui_window *win);
// Position before showing: screen coordinates, or for popups relative to
// the parent window's content.
void ui_window_move(struct ui_window *win, int x, int y);
// Role and WM_FLAG_* flags; set before the window is first shown.
void ui_window_set_flags(struct ui_window *win, uint32_t flags);
// The owner of a dialog or popup; set before the window is first shown.
void ui_window_set_parent(struct ui_window *win, struct ui_window *parent);
// Window list and screen changes (for taskbars): WM_EV_LIST, WM_EV_SCREEN.
void ui_on_system_event(void (*fn)(struct wm_event *ev, void *user), void *user);
struct wm_window *ui_wm_window(struct ui_window *win);
// The user pointer given to ui_load().
void *ui_window_user(struct ui_window *win);
// Draws the window's background instead of the theme colour (wallpapers).
void ui_window_set_backdrop(struct ui_window *win,
                            void (*fn)(struct ui_window *, struct gfx *, struct rect, void *), void *user);
// Draws over the widgets after each repaint (selection outlines).
void ui_window_set_overlay(struct ui_window *win, void (*fn)(struct ui_window *, struct gfx *, void *), void *user);
// Sees pointer events first, with the widget under the pointer; returning
// true keeps them from the widgets (designers).
void ui_on_pointer(struct ui_window *win,
                   bool (*fn)(struct ui_window *, struct widget *hit, struct wm_event *, void *), void *user);
void ui_window_redraw(struct ui_window *win);
// Called when the user closes the window; return false to keep it open.
void ui_on_close(struct ui_window *win, bool (*fn)(struct ui_window *, void *), void *user);
// Called after the default handling of every key press in the window.
void ui_on_key(struct ui_window *win, void (*fn)(struct ui_window *, struct wm_event *, void *), void *user);

// Runs the event loop until ui_quit() or the last window closes.
int ui_run(void);
void ui_quit(int code);
// Repeating timer; return false from the callback to stop it.
int ui_timer(uint64_t ms, bool (*fn)(void *user), void *user);
void ui_timer_cancel(int id);
// Watches a descriptor from the event loop.
void ui_watch_fd(int fd, void (*fn)(int fd, void *user), void *user);
void ui_unwatch_fd(int fd);

// ---- Widgets ----

struct widget *ui_get(struct ui_window *win, const char *id);
struct ui_window *ui_window_of(struct widget *w);
const char *ui_id(struct widget *w);

void ui_set_text(struct widget *w, const char *text);
const char *ui_text(struct widget *w);
void ui_set_value(struct widget *w, double value);       // slider, progress, checkbox, toggle, list selection
double ui_value(struct widget *w);
void ui_set_enabled(struct widget *w, bool enabled);
void ui_set_visible(struct widget *w, bool visible);
bool ui_visible(struct widget *w);
void ui_focus(struct widget *w);
// True if w has the keyboard focus and its window is focused.
bool ui_is_focused_widget(struct widget *w);
void ui_set_attr(struct widget *w, const char *name, const char *value);
const char *ui_attr(struct widget *w, const char *name);
// True for "true", "yes", "1" or "on" (false for NULL).
bool ui_attr_true(const char *value);
void ui_redraw(struct widget *w);
void ui_relayout(struct ui_window *win);
void ui_set_handler(struct widget *w, const char *event, ui_handler fn, void *user);

// Lists, tables and dropdowns: rows of tab-separated columns. Events:
// "select" (selection changed), "activate" (double click or Enter),
// "context" (right click or the Menu key), "sort" (a table header was
// clicked; read the "sortcolumn" and "sortdescending" attributes), "drag"
// (a row was pulled away with the pointer: call ui_drag_start).
void ui_list_clear(struct widget *w);
int ui_list_add(struct widget *w, const char *text);
int ui_list_count(struct widget *w);
const char *ui_list_item(struct widget *w, int index);
void ui_list_set_item(struct widget *w, int index, const char *text);
void ui_list_remove(struct widget *w, int index);
int ui_list_selected(struct widget *w);
void ui_list_select(struct widget *w, int index);
// An image shown at the start of each row (optional).
void ui_list_set_icon(struct widget *w, int index, struct surface *icon);
// The same for an icon the list must not free (from icon_get()).
void ui_list_set_icon_shared(struct widget *w, int index, struct surface *icon);
// The row at window coordinates x, y, or -1.
int ui_list_row_at(struct widget *w, int x, int y);
// Marks a row as where a drop would go (-1: none).
void ui_list_set_drop_row(struct widget *w, int index);
// Copies one column of a row into buf and returns buf.
const char *ui_list_column(struct widget *w, int index, int column, char *buf, size_t size);

// Menus: opens a <menu> as a popup at window coordinates x, y next to the
// widget `at` (x < 0: at the pointer).
void ui_menu_popup(struct widget *menu, struct widget *at, int x, int y);

// Images.
void ui_image_set(struct widget *w, struct surface *s, bool owned);

// Text areas.
void ui_textarea_insert(struct widget *w, const char *text);
// Text fields: "undo", "redo", "cut", "copy", "paste", "selectall", "delete".
void ui_edit_command(struct widget *w, const char *cmd);
int ui_textarea_selection(struct widget *w, int *start, int *end);
int ui_textarea_cursor(struct widget *w);
void ui_textarea_select(struct widget *w, int start, int end);
bool ui_textarea_modified(struct widget *w);
void ui_textarea_set_modified(struct widget *w, bool modified);

// 1 for a single click, 2 for a double click, ... (in a pointer handler).
int ui_click_count(struct widget *w);

// The widget's rectangle in window coordinates.
struct rect ui_rect(struct widget *w);

// Canvas: custom drawing (r is the canvas rectangle) and input (pointer
// positions relative to the canvas).
typedef void (*ui_paint_fn)(struct widget *w, struct gfx *g, struct rect r, void *user);
typedef void (*ui_input_fn)(struct widget *w, struct wm_event *ev, void *user);
void ui_canvas_set(struct widget *w, ui_paint_fn paint, ui_input_fn input, void *user);

// Building trees in code.
struct widget *ui_create(struct ui_window *win, const char *tag);
void ui_add(struct widget *parent, struct widget *child);
void ui_remove(struct widget *w);
int ui_children(struct widget *w);
struct widget *ui_child(struct widget *w, int index);
struct widget *ui_parent(struct widget *w);
const char *ui_tag(struct widget *w);
struct widget *ui_parse_into(struct widget *parent, const char *aui);

// ---- Dialogs ----

// Shows a modal message; returns the index of the button pressed (buttons
// separated by '|', e.g. "Save|Don't save|Cancel"), or -1 if closed.
int ui_message(struct ui_window *parent, const char *title, const char *text, const char *buttons);
// Asks for a line of text; returns a malloc'd string or NULL if cancelled.
char *ui_prompt(struct ui_window *parent, const char *title, const char *text, const char *initial);
// Runs a window loaded by the app as a modal dialog over parent until a
// handler calls ui_dialog_end(); returns that code (-1 if closed). The
// window is closed afterwards; its widgets can still be read until the
// event loop runs again.
int ui_dialog_run(struct ui_window *dlg, struct ui_window *parent);
void ui_dialog_end(struct ui_window *dlg, int result);
// Asks for a password (shown as bullets); free the result.
char *ui_prompt_password(struct ui_window *parent, const char *title, const char *text);
// Makes this program an administrator (root) after asking for the user's
// password; false if they cancel or are not an administrator.
bool ui_elevate(struct ui_window *parent, const char *why);
// File chooser. save: ask for a new name. Returns a malloc'd path or NULL.
char *ui_file_dialog(struct ui_window *parent, const char *title, const char *start_dir, bool save,
                     const char *suggested_name);
// The same, listing only files matching filter ("*.txt;*.md").
char *ui_file_dialog_filtered(struct ui_window *parent, const char *title, const char *start_dir, bool save,
                              const char *suggested_name, const char *filter);

// ---- Drag and drop ----

// Data travels with a type: "files" is paths, one per line; "text" is UTF-8.
struct ui_drop {
    const char *type;
    int kind;                       // WM_DRAG_ENTER, WM_DRAG_MOVE, WM_DRAG_LEAVE; WM_DRAG_MOVE for a drop
    int x, y;                       // window coordinates
    uint32_t actions;               // WM_DND_* the source allows
    uint32_t mods;                  // keyboard modifiers
    const char *data;
    size_t len;
};

// over: returns what a drop at d->x, d->y would do (WM_DND_COPY, _MOVE or
// _LINK), or 0 to refuse (the parent widget is asked next). It is called
// once more with kind WM_DRAG_LEAVE when the drag moves away.
// drop: takes the data; action is the value over returned.
typedef int (*ui_drop_over_fn)(struct widget *w, struct ui_drop *d, void *user);
typedef void (*ui_drop_fn)(struct widget *w, struct ui_drop *d, int action, void *user);
// The target is outlined while a drag is over it, unless it has
// dropoutline="false" (it shows the drop point itself).
void ui_set_drop_target(struct widget *w, ui_drop_over_fn over, ui_drop_fn drop, void *user);
// Makes w (usually the window's root) open files dropped on it: open is
// called for each path until it returns false.
void ui_accept_files(struct widget *w, bool (*open)(const char *path, void *user), void *user);
// Starts dragging from w, while a pointer button is held (from a "drag"
// event or a pointer handler). done, if given, hears the action the target
// took (0: none) once the drag is over.
bool ui_drag_start(struct widget *w, const char *type, const char *data, size_t len, const char *label,
                   uint32_t actions, void (*done)(struct widget *w, int action, void *user), void *user);
// The usual choice for files: Ctrl copies, Shift moves, Alt links; otherwise
// fallback, if the source allows it.
int ui_drop_action(struct ui_drop *d, int fallback);
// True if a drag started in this program is in progress.
bool ui_dragging(void);

// ---- Clipboard ----

void ui_clipboard_set(const char *text);
const char *ui_clipboard_get(void);

// ---- Text ----

// Strings in AUI attributes that start with '@' are translation keys
// ("@file.open"); "@@" escapes a literal '@'. The translator returns NULL
// for unknown keys.
void ui_set_translator(const char *(*fn)(const char *key));
const char *ui_tr(const char *text);
void ui_format_size(uint64_t size, char *buf, size_t len);

// ---- Theme ----

struct ui_theme {
    color_t window, surface, surface_alt, border, text, text_dim, accent, accent_text,
            selection, selection_text, hover, pressed, danger, focus_ring, input, shadow;
    int font_size, radius, padding, spacing, row_height;
    const char *font, *font_bold, *font_mono;
};

extern struct ui_theme ui_theme;
// Applies a named theme: "light", "dark" or "high-contrast".
void ui_set_theme(const char *name);
// Applies the theme from AEGIS_THEME or the user's settings.
void ui_load_user_theme(void);

#endif
