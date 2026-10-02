#ifndef VT_H
#define VT_H

#include "aegis.h"

// A VT100/xterm-style screen: what the program running in a terminal
// sees. Feed it output bytes; read back cells.

#define VT_BOLD         0x01
#define VT_UNDERLINE    0x02
#define VT_INVERSE      0x04
#define VT_DIM          0x08
#define VT_ITALIC       0x10

#define VT_DEFAULT      0xFFFFFFFFu     // the default colour
#define VT_SCROLLBACK   2000

struct vt_cell {
    uint32_t ch;
    uint32_t fg, bg;                    // 0xRRGGBB or VT_DEFAULT
    uint8_t flags;
};

struct vt {
    int cols, rows;
    struct vt_cell *screen;             // rows * cols
    struct vt_cell *alt;                // the other screen (alternate buffer)
    bool on_alt;
    // Lines that scrolled off the top (main screen only), oldest first.
    struct vt_cell *history;
    int history_len, history_start;
    int cx, cy;                         // cursor
    int saved_cx, saved_cy;
    bool pending_wrap;
    bool cursor_visible, autowrap;
    int top, bottom;                    // scrolling region
    struct vt_cell pen;                 // attributes for new characters
    struct vt_cell saved_pen;
    // Parser.
    int state;
    int params[16], nparams;
    bool private_mode;
    char osc[256];
    int osc_len;
    uint32_t utf8_cp;
    int utf8_left;
    char title[128];
    bool title_changed;
    bool dirty;
    // Bytes the terminal must send back (device status reports).
    char reply[64];
    int reply_len;
};

bool vt_init(struct vt *vt, int cols, int rows);
void vt_free(struct vt *vt);
void vt_resize(struct vt *vt, int cols, int rows);
void vt_write(struct vt *vt, const char *data, size_t len);
// A line of the scrollback (0 = oldest) or of the screen.
const struct vt_cell *vt_history_line(struct vt *vt, int i);
uint32_t vt_palette(int index);

#endif
