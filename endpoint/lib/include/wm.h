#ifndef AEGIS_WM_H
#define AEGIS_WM_H

#include "aegis.h"
#include "gfx.h"
#include "wm_proto.h"
#include "abi/input.h"

// Client side of the window system.

struct wm_window {
    uint32_t id;
    uint32_t role;
    int width, height;              // content size
    struct surface *surface;        // draw here, then wm_present()
    bool focused, visible;
    void *user;
    // Shared buffer the compositor reads.
    uint32_t *shared;
    size_t shared_len;
    int shm_fd;
    struct wm_window *next;
};

enum wm_event_type {
    WM_EV_NONE, WM_EV_KEY, WM_EV_POINTER, WM_EV_RESIZE, WM_EV_CLOSE, WM_EV_FOCUS,
    WM_EV_FRAME, WM_EV_POPUP_DONE, WM_EV_LIST, WM_EV_SCREEN, WM_EV_SETTING,
    WM_EV_DRAG,         // a drag is over the window: x, y, kind (WM_DRAG_*), mods, drag_type, actions, data
    WM_EV_DROP,         // dropped: x, y, mods, drag_type, action, data
    WM_EV_DRAG_END,     // the drag this window started is over: action (0: nothing was done)
    WM_EV_WORKSPACE,    // panels: the workspace shown changed (msg.a, of msg.b)
    WM_EV_COMMANDS,     // list this window's commands (answer with wm_command_item)
    WM_EV_COMMAND_RUN,  // run command number value of this window
    WM_EV_COMMAND_ITEM, // panels: one of the focused window's commands (msg)
};

struct wm_event {
    enum wm_event_type type;
    struct wm_window *window;
    // Keys: code, value (0 up, 1 down, 2 repeat), modifiers, UTF-8 text.
    uint16_t key;
    int value;
    uint32_t mods;
    char text[8];
    // Pointer: position in content, buttons held, kind (WM_PTR_*), button or wheel delta.
    int x, y;
    uint32_t buttons;
    int kind;
    int detail;
    // Resize: new content size. Focus: focused or not.
    int width, height;
    bool focused;
    // Drag and drop: the data's type, what the source allows (WM_DND_*),
    // the action chosen, and the data (valid until another drag enters).
    char drag_type[32];
    uint32_t actions;
    int action;
    const char *data;
    size_t data_len;
    // Window list (panels): the raw message.
    struct wm_msg msg;
};

// Connects to the compositor. Returns false if there is no display.
bool wm_connect(void);
int wm_fd(void);
void wm_screen_size(int *w, int *h, int *work_h);

struct wm_window *wm_create(const char *title, int width, int height, uint32_t flags);
struct wm_window *wm_create_at(const char *title, int x, int y, int width, int height, uint32_t flags,
                               struct wm_window *parent);
void wm_destroy(struct wm_window *w);
void wm_set_title(struct wm_window *w, const char *title);
void wm_show(struct wm_window *w, bool show);
void wm_move(struct wm_window *w, int x, int y);
void wm_resize(struct wm_window *w, int width, int height);
void wm_set_state(struct wm_window *w, uint32_t state);
void wm_set_cursor(struct wm_window *w, int cursor);
void wm_begin_move(struct wm_window *w);
void wm_send(struct wm_msg *m);
// Copies r of the surface to the screen buffer and tells the compositor.
void wm_present(struct wm_window *w, struct rect r);
void wm_present_all(struct wm_window *w);
// Waits up to timeout_ms (-1: forever) for an event. Returns false on timeout
// or when the connection closes (then wm_connected() is false).
bool wm_next_event(struct wm_event *ev, int timeout_ms);
bool wm_connected(void);
// For panels: window list updates arrive as WM_EV_LIST events.
void wm_subscribe(void);
void wm_activate(uint32_t global_id, bool toggle);
// Shows another workspace, or moves a window (by global id) to one.
void wm_switch_workspace(int workspace);
// The command palette: ask for the focused window's commands (they come
// as WM_EV_COMMAND_ITEM, ending with index -1), and run one. Programs
// answer WM_EV_COMMANDS with wm_command_item.
void wm_commands_query(void);
void wm_command_run(uint32_t global_id, int index);
void wm_command_item(struct wm_window *w, int index, const char *text);
void wm_move_to_workspace(uint32_t global_id, int workspace);
// Tells the session's programs (and the window frames) that a setting changed.
void wm_setting_changed(const char *key, const char *value);
// The session's clipboard, shared through the compositor. get returns NULL
// before anything was copied.
bool wm_clipboard_set(const char *text);
const char *wm_clipboard_get(void);
// Drag and drop. Start a drag while a pointer button is held in w: type
// names the data ("files": paths, one per line; "text": UTF-8), label is
// shown beside the cursor, actions are the WM_DND_* the target may take.
// The window gets WM_EV_DRAG_END when it is over.
bool wm_drag_start(struct wm_window *w, const char *type, const void *data, size_t len, const char *label,
                   uint32_t actions);
// Answers a WM_EV_DRAG: what a drop at that point would do (0: nothing).
void wm_drag_status(struct wm_window *w, int action);

#endif
