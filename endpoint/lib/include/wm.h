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
// Tells the session's programs (and the window frames) that a setting changed.
void wm_setting_changed(const char *key, const char *value);
// The session's clipboard, shared through the compositor. get returns NULL
// before anything was copied.
bool wm_clipboard_set(const char *text);
const char *wm_clipboard_get(void);

#endif
