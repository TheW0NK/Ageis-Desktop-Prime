#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "aegis.h"
#include "gfx.h"
#include "wm_proto.h"
#include "abi/fb.h"
#include "abi/input.h"

#define TITLE_H         32
#define BORDER          1
#define RADIUS          8
#define SHADOW          22
#define RESIZE_EDGE     6
#define MAX_DAMAGE      32

enum { TILE_NONE, TILE_LEFT, TILE_RIGHT };

struct client {
    int fd;
    int pid;
    uint32_t uid;
    bool subscribed;
    struct client *next;
};

struct window {
    uint32_t id;                    // global, unique
    uint32_t cid;                   // the client's own number for it
    struct client *owner;
    uint32_t role, flags;
    char title[WM_TEXT_MAX];
    struct rect frame;              // screen position of the frame (or content, if frameless)
    int cw, ch;                     // content size
    uint32_t *pixels;               // the client's buffer, mapped
    size_t map_len;
    int bw, bh;                     // buffer size
    bool visible, minimized, maximized;
    int tiled;                      // TILE_LEFT or TILE_RIGHT: snapped to half the screen
    int workspace;                  // normal windows and dialogs: the one they are on
    bool autohide, revealed;        // panels: hidden until the pointer reaches their edge
    struct rect saved;              // frame before maximizing or tiling
    struct window *parent;
    bool frame_pending;
    int cursor;
    struct window *next;            // z-order, bottom to top
};

struct screen {
    int fd;
    struct aegis_fbinfo info;
    uint8_t *fb;
    struct surface *back;
    int width, height;
    struct rect work;               // the area not covered by panels
};

// A drag and drop in progress: the data waits in a shared memory object
// until it is dropped on a window that accepts it.
struct dnd {
    bool active;
    struct window *source, *target;
    int fd, len;
    uint32_t actions;               // WM_DND_* the source allows
    int action;                     // what the target under the pointer would do (0: refuses)
    int unanswered;                 // drag messages the target has not answered yet
    uint64_t released_ms;           // buttons released while an answer was due: drop when it comes
    char type[32];
    char label[96];
};

// Cursor shapes of the compositor's own, after the WM_CURSOR_* ones.
#define CURSOR_DND_NONE     100     // nothing would happen here
#define CURSOR_DND_COPY     101
#define CURSOR_DND_MOVE     102
#define CURSOR_DND_LINK     103

extern struct screen screen;
extern struct dnd dnd;
extern struct window *windows;
extern struct window *focused;
extern int pointer_x, pointer_y;

// render.c
void damage(struct rect r);
void damage_window(struct window *w);
void render(void);
struct rect window_content(struct window *w);
struct rect window_bounds(struct window *w);    // frame plus shadow
bool window_framed(struct window *w);
int cursor_for_point(struct window *w, int x, int y);
void set_cursor_shape(int shape);
int screenshot(const char *path);
// The signed-in user, or -1 at the sign-in screen.
uint32_t compositor_session_uid(void);
void set_frame_theme(const char *name);
// A message in the middle of the screen for a moment ("Workspace 2").
void show_osd(const char *text);
// Returns how long the main loop may wait before the message goes (-1: none shown).
int osd_tick(void);

// input.c
int input_open_device(void);
void input_handle(int fd);
void key_text(uint16_t code, uint32_t mods, char *out);
bool dnd_start(struct window *w, int fd, int len, uint32_t actions, const char *text);
void dnd_status(struct window *w, int action);
void input_forget(struct window *w);
// Called from the main loop: drops a released drag whose target is slow to answer.
// Returns how long the loop may wait (-1: no drag waiting).
int dnd_tick(void);

// main.c
void send_msg(struct client *c, struct wm_msg *m);
void send_window(struct window *w, uint32_t type, int a, int b, int c, int d, uint32_t flags);
void focus_window(struct window *w);
void raise_window(struct window *w);
struct window *window_at(int x, int y);
void close_popups(struct window *except);
void set_maximized(struct window *w, bool on);
void set_tiled(struct window *w, int side);
struct rect tile_rect(int side);
// A window being dragged to a screen edge shows where it will land.
extern struct window *snap_window;
extern struct rect snap_preview;
void set_minimized(struct window *w, bool on);
void announce(struct window *w, uint32_t type);
void cycle_focus(void);
// Workspaces.
extern int current_workspace;
bool on_screen(struct window *w);       // shown, not minimized, and on this workspace (or on all)
void switch_workspace(int n);
void move_to_workspace(struct window *w, int n);
// Shows or hides auto-hiding panels for the pointer's position.
void update_autohide(void);
void update_work_area(void);
void vt_leave(void);
void vt_enter(void);
extern bool screen_paused;

#endif
