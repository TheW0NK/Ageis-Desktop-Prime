#ifndef AEGIS_WM_PROTO_H
#define AEGIS_WM_PROTO_H

#include <stdint.h>

// Window system protocol. Clients talk to the compositor over a
// SOCK_SEQPACKET socket named WM_SOCKET; every message is one struct wm_msg.
// Window contents travel in shared memory objects passed with WM_BUFFER.

#define WM_SOCKET       "@aegis/display"
#define WM_VERSION      1
#define WM_TEXT_MAX     256

enum wm_type {
    // Client to compositor.
    WM_HELLO = 1,           // a = WM_VERSION
    WM_CREATE,              // window, a = width, b = height, c = x, d = y (-1: placed by the WM),
                            // flags = WM_ROLE_* | WM_FLAG_*, text = title, parent = owner window
    WM_BUFFER,              // window, a = width, b = height; carries the shm descriptor (a*b*4 bytes)
    WM_DAMAGE,              // window, a, b, c, d = x, y, w, h of changed content
    WM_SET_TITLE,           // window, text
    WM_SHOW,                // window
    WM_HIDE,                // window
    WM_DESTROY,             // window
    WM_MOVE,                // window, a = x, b = y (frame position)
    WM_RESIZE,              // window, a = width, b = height (content)
    WM_SET_STATE,           // window, a = WM_STATE_* to set (minimize, maximize, restore)
    WM_ACTIVATE,            // window = global window id (panels may activate any window)
    WM_SET_CURSOR,          // window, a = WM_CURSOR_*
    WM_SUBSCRIBE,           // ask for WM_LIST_* updates (panels)
    WM_SET_SESSION,         // a = uid allowed to connect (greeter only, root)
    WM_BEGIN_MOVE,          // window: start dragging the window with the pointer
    WM_SCREENSHOT,          // text = path to save a PNG of the screen
    WM_SETTING_CHANGED,     // text = "key=value": tell the session's programs (theme, background, ...)
    WM_CLIPBOARD_SET,       // a = length; carries a shm descriptor holding the UTF-8 text
    WM_DRAG_START,          // window (holding the pointer), a = length, b = WM_DND_* allowed,
                            // text = "type\nlabel"; carries a shm descriptor with the data
    WM_DRAG_STATUS,         // window (under a drag), a = WM_DND_* it would take, 0 to refuse

    // Compositor to client.
    WM_WELCOME = 64,        // a = screen width, b = screen height, c = work area height
    WM_CONFIGURE,           // window, a = width, b = height: draw at this size
    WM_CLOSE_REQUEST,       // window
    WM_FOCUS,               // window, a = 1 focused, 0 not
    WM_KEY,                 // window, a = key code, b = 0 up / 1 down / 2 repeat, c = modifiers,
                            // text = UTF-8 typed, if any
    WM_POINTER,             // window, a = x, b = y (content coordinates), c = buttons held,
                            // d = WM_PTR_*, flags = button (BTN_*) or wheel delta,
                            // parent = keyboard modifiers
    WM_FRAME,               // window: the last damage is on screen
    WM_POPUP_DONE,          // window: a popup was dismissed by a click outside it
    WM_LIST_ADD,            // window = global id, text = title, a = state, b = pid
    WM_LIST_REMOVE,         // window = global id
    WM_LIST_CHANGE,         // window = global id, text = title, a = state
    WM_ERROR,               // a = errno, text = message
    WM_SCREEN,              // a = width, b = height after a mode change
    WM_SETTING,             // text = "key=value" from WM_SETTING_CHANGED
    WM_CLIPBOARD,           // a = length; carries a shm descriptor with the session's clipboard
    WM_DRAG_OVER,           // window, a = x, b = y (content), c = WM_DND_* allowed, d = WM_DRAG_*,
                            // parent = keyboard modifiers, text = type; answer with WM_DRAG_STATUS.
                            // WM_DRAG_ENTER carries the data too, flags = its length
    WM_DROP,                // window, a = x, b = y, c = WM_DND_* chosen, d = length,
                            // parent = keyboard modifiers, text = type; carries the data
    WM_DRAG_END,            // window (the source), a = WM_DND_* taken by the target, 0 if none
};

// Window roles (in flags).
#define WM_ROLE_NORMAL      0x0
#define WM_ROLE_PANEL       0x1     // docked at the top or bottom edge, always on top
#define WM_ROLE_DESKTOP     0x2     // the background, below everything
#define WM_ROLE_POPUP       0x3     // menus and tooltips: no frame, dismissed by outside clicks
#define WM_ROLE_DIALOG      0x4     // framed, kept above its parent
#define WM_ROLE_OVERLAY     0x5     // full screen, above everything (lock screen, greeter)
#define WM_ROLE_MASK        0xF
#define WM_FLAG_NO_RESIZE   0x10
#define WM_FLAG_PANEL_TOP   0x20    // panels default to the bottom edge
#define WM_FLAG_HIDDEN      0x40    // create without showing
#define WM_FLAG_KEYBOARD    0x80    // popups: take the keyboard while shown (launchers)

// Window states (WM_SET_STATE and WM_LIST_*).
#define WM_STATE_NORMAL     0x0
#define WM_STATE_MINIMIZED  0x1
#define WM_STATE_MAXIMIZED  0x2
#define WM_STATE_FOCUSED    0x4

// Pointer events.
#define WM_PTR_MOVE         0
#define WM_PTR_DOWN         1
#define WM_PTR_UP           2
#define WM_PTR_WHEEL        3       // flags = signed vertical delta (positive: up)
#define WM_PTR_ENTER        4
#define WM_PTR_LEAVE        5
#define WM_PTR_HWHEEL       6

// Drag and drop: what happens to the data (WM_DRAG_START, WM_DRAG_STATUS, WM_DROP).
#define WM_DND_COPY         0x1
#define WM_DND_MOVE         0x2
#define WM_DND_LINK         0x4     // open it, or make a shortcut to it

// WM_DRAG_OVER kinds.
#define WM_DRAG_ENTER       0
#define WM_DRAG_MOVE        1
#define WM_DRAG_LEAVE       2

// Cursors.
#define WM_CURSOR_ARROW     0
#define WM_CURSOR_TEXT      1
#define WM_CURSOR_HAND      2
#define WM_CURSOR_WAIT      3
#define WM_CURSOR_RESIZE_H  4
#define WM_CURSOR_RESIZE_V  5
#define WM_CURSOR_NONE      6

struct wm_msg {
    uint32_t type;
    uint32_t window;
    int32_t a, b, c, d;
    uint32_t flags;
    uint32_t parent;
    char text[WM_TEXT_MAX];
};

#endif
