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
    struct rect saved;              // frame before maximizing
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

extern struct screen screen;
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
void set_frame_theme(const char *name);

// input.c
int input_open_device(void);
void input_handle(int fd);
void key_text(uint16_t code, uint32_t mods, char *out);

// main.c
void send_msg(struct client *c, struct wm_msg *m);
void send_window(struct window *w, uint32_t type, int a, int b, int c, int d, uint32_t flags);
void focus_window(struct window *w);
void raise_window(struct window *w);
struct window *window_at(int x, int y);
void close_popups(struct window *except);
void set_maximized(struct window *w, bool on);
void set_minimized(struct window *w, bool on);
void announce(struct window *w, uint32_t type);
void cycle_focus(void);
void update_work_area(void);
void vt_leave(void);
void vt_enter(void);
extern bool screen_paused;

#endif
