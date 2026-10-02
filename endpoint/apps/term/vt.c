#include "vt.h"

enum { GROUND, ESCAPE, CSI, OSC, OSC_ESC, CHARSET };

static const uint32_t base16[16] = {
    0x1B1F27, 0xE5534B, 0x57AB5A, 0xC69026, 0x539BF5, 0xB083F0, 0x39C5CF, 0xC5CBD3,
    0x636E7B, 0xFF7B72, 0x7EE787, 0xF2CC60, 0x79C0FF, 0xD2A8FF, 0x56D4DD, 0xF0F3F6,
};

uint32_t vt_palette(int i)
{
    if (i < 16)
        return base16[i & 15];
    if (i < 232) {
        static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
        int v = i - 16;

        return (steps[v / 36] << 16) | (steps[v / 6 % 6] << 8) | steps[v % 6];
    }
    {
        int g = 8 + (i - 232) * 10;

        return (g << 16) | (g << 8) | g;
    }
}

static struct vt_cell blank(struct vt *vt)
{
    struct vt_cell c = { ' ', vt->pen.fg, vt->pen.bg, 0 };

    return c;
}

static struct vt_cell *cell(struct vt *vt, int x, int y)
{
    return &vt->screen[y * vt->cols + x];
}

static void clear_cells(struct vt *vt, int x0, int y0, int x1, int y1)
{
    struct vt_cell b = blank(vt);

    // Inclusive start, exclusive end, in reading order.
    for (int y = y0; y <= y1 && y < vt->rows; y++)
        for (int x = (y == y0 ? x0 : 0); x < (y == y1 ? x1 : vt->cols); x++)
            *cell(vt, x, y) = b;
    vt->dirty = true;
}

static void push_history(struct vt *vt, int y)
{
    int slot;

    if (vt->on_alt || !vt->history)
        return;
    if (vt->history_len < VT_SCROLLBACK) {
        slot = (vt->history_start + vt->history_len) % VT_SCROLLBACK;
        vt->history_len++;
    } else {
        slot = vt->history_start;
        vt->history_start = (vt->history_start + 1) % VT_SCROLLBACK;
    }
    memcpy(&vt->history[(size_t)slot * vt->cols], cell(vt, 0, y), vt->cols * sizeof(struct vt_cell));
}

static void scroll_up(struct vt *vt, int n)
{
    int lines = vt->bottom - vt->top + 1;

    n = MIN(n, lines);
    for (int i = 0; i < n; i++)
        if (vt->top == 0)
            push_history(vt, i);
    memmove(cell(vt, 0, vt->top), cell(vt, 0, vt->top + n), (size_t)(lines - n) * vt->cols * sizeof(struct vt_cell));
    clear_cells(vt, 0, vt->bottom - n + 1, vt->cols, vt->bottom);
}

static void scroll_down(struct vt *vt, int n)
{
    int lines = vt->bottom - vt->top + 1;

    n = MIN(n, lines);
    memmove(cell(vt, 0, vt->top + n), cell(vt, 0, vt->top), (size_t)(lines - n) * vt->cols * sizeof(struct vt_cell));
    clear_cells(vt, 0, vt->top, vt->cols, vt->top + n - 1);
}

static void newline(struct vt *vt)
{
    if (vt->cy == vt->bottom)
        scroll_up(vt, 1);
    else if (vt->cy < vt->rows - 1)
        vt->cy++;
}

static void put_char(struct vt *vt, uint32_t ch)
{
    struct vt_cell *c;

    if (vt->pending_wrap) {
        vt->pending_wrap = false;
        vt->cx = 0;
        newline(vt);
    }
    c = cell(vt, vt->cx, vt->cy);
    *c = vt->pen;
    c->ch = ch;
    vt->dirty = true;
    if (vt->cx == vt->cols - 1) {
        if (vt->autowrap)
            vt->pending_wrap = true;
    } else {
        vt->cx++;
    }
}

static void reset(struct vt *vt)
{
    vt->pen = (struct vt_cell){ ' ', VT_DEFAULT, VT_DEFAULT, 0 };
    vt->cx = vt->cy = 0;
    vt->top = 0;
    vt->bottom = vt->rows - 1;
    vt->cursor_visible = true;
    vt->autowrap = true;
    vt->pending_wrap = false;
    vt->state = GROUND;
    clear_cells(vt, 0, 0, vt->cols, vt->rows - 1);
}

bool vt_init(struct vt *vt, int cols, int rows)
{
    memset(vt, 0, sizeof(*vt));
    vt->cols = MAX(cols, 2);
    vt->rows = MAX(rows, 2);
    vt->screen = calloc((size_t)vt->cols * vt->rows, sizeof(struct vt_cell));
    vt->alt = calloc((size_t)vt->cols * vt->rows, sizeof(struct vt_cell));
    vt->history = calloc((size_t)VT_SCROLLBACK * vt->cols, sizeof(struct vt_cell));
    if (!vt->screen || !vt->alt || !vt->history)
        return false;
    reset(vt);
    {
        struct vt_cell b = blank(vt);

        for (int i = 0; i < vt->cols * vt->rows; i++)
            vt->alt[i] = b;
    }
    return true;
}

void vt_free(struct vt *vt)
{
    free(vt->screen);
    free(vt->alt);
    free(vt->history);
}

static struct vt_cell *resized(struct vt_cell *old, int ocols, int orows, int cols, int rows, int shift)
{
    struct vt_cell *n = calloc((size_t)cols * rows, sizeof(*n));

    if (!n)
        return NULL;
    for (int i = 0; i < cols * rows; i++)
        n[i] = (struct vt_cell){ ' ', VT_DEFAULT, VT_DEFAULT, 0 };
    for (int y = 0; y < rows; y++) {
        int oy = y + shift;

        if (oy < 0 || oy >= orows)
            continue;
        memcpy(&n[y * cols], &old[oy * ocols], MIN(cols, ocols) * sizeof(*n));
    }
    return n;
}

void vt_resize(struct vt *vt, int cols, int rows)
{
    struct vt_cell *s, *a, *h;
    int shift;

    cols = MAX(cols, 2);
    rows = MAX(rows, 2);
    if (cols == vt->cols && rows == vt->rows)
        return;
    // Keep the cursor on screen by dropping lines from the top.
    shift = MAX(0, vt->cy - (rows - 1));
    s = resized(vt->screen, vt->cols, vt->rows, cols, rows, shift);
    a = resized(vt->alt, vt->cols, vt->rows, cols, rows, 0);
    h = calloc((size_t)VT_SCROLLBACK * cols, sizeof(struct vt_cell));
    if (!s || !a || !h) {
        free(s);
        free(a);
        free(h);
        return;
    }
    // The scrollback keeps its lines, cut or padded to the new width.
    for (int i = 0; i < vt->history_len; i++) {
        const struct vt_cell *src = vt_history_line(vt, i);

        for (int x = 0; x < cols; x++)
            h[(size_t)i * cols + x] = x < vt->cols ? src[x] : (struct vt_cell){ ' ', VT_DEFAULT, VT_DEFAULT, 0 };
    }
    vt->history_start = 0;
    free(vt->screen);
    free(vt->alt);
    free(vt->history);
    vt->screen = s;
    vt->alt = a;
    vt->history = h;
    vt->cols = cols;
    vt->rows = rows;
    vt->cy -= shift;
    vt->cx = MIN(vt->cx, cols - 1);
    vt->cy = MIN(MAX(vt->cy, 0), rows - 1);
    vt->top = 0;
    vt->bottom = rows - 1;
    vt->pending_wrap = false;
    vt->dirty = true;
}

const struct vt_cell *vt_history_line(struct vt *vt, int i)
{
    if (i < vt->history_len)
        return &vt->history[(size_t)((vt->history_start + i) % VT_SCROLLBACK) * vt->cols];
    return cell(vt, 0, MIN(i - vt->history_len, vt->rows - 1));
}

static int param(struct vt *vt, int i, int def)
{
    return i < vt->nparams && vt->params[i] > 0 ? vt->params[i] : def;
}

static void sgr(struct vt *vt)
{
    if (!vt->nparams) {
        vt->pen.fg = vt->pen.bg = VT_DEFAULT;
        vt->pen.flags = 0;
        return;
    }
    for (int i = 0; i < vt->nparams; i++) {
        int p = vt->params[i];

        if (p == 0) {
            vt->pen.fg = vt->pen.bg = VT_DEFAULT;
            vt->pen.flags = 0;
        } else if (p == 1) {
            vt->pen.flags |= VT_BOLD;
        } else if (p == 2) {
            vt->pen.flags |= VT_DIM;
        } else if (p == 3) {
            vt->pen.flags |= VT_ITALIC;
        } else if (p == 4) {
            vt->pen.flags |= VT_UNDERLINE;
        } else if (p == 7) {
            vt->pen.flags |= VT_INVERSE;
        } else if (p == 22) {
            vt->pen.flags &= ~(VT_BOLD | VT_DIM);
        } else if (p == 23) {
            vt->pen.flags &= ~VT_ITALIC;
        } else if (p == 24) {
            vt->pen.flags &= ~VT_UNDERLINE;
        } else if (p == 27) {
            vt->pen.flags &= ~VT_INVERSE;
        } else if (p >= 30 && p <= 37) {
            vt->pen.fg = vt_palette(p - 30);
        } else if (p == 39) {
            vt->pen.fg = VT_DEFAULT;
        } else if (p >= 40 && p <= 47) {
            vt->pen.bg = vt_palette(p - 40);
        } else if (p == 49) {
            vt->pen.bg = VT_DEFAULT;
        } else if (p >= 90 && p <= 97) {
            vt->pen.fg = vt_palette(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            vt->pen.bg = vt_palette(p - 100 + 8);
        } else if ((p == 38 || p == 48) && i + 1 < vt->nparams) {
            uint32_t c = VT_DEFAULT;

            if (vt->params[i + 1] == 5 && i + 2 < vt->nparams) {
                c = vt_palette(vt->params[i + 2] & 255);
                i += 2;
            } else if (vt->params[i + 1] == 2 && i + 4 < vt->nparams) {
                c = ((vt->params[i + 2] & 255) << 16) | ((vt->params[i + 3] & 255) << 8) | (vt->params[i + 4] & 255);
                i += 4;
            } else {
                continue;
            }
            if (p == 38)
                vt->pen.fg = c;
            else
                vt->pen.bg = c;
        }
    }
}

static void set_mode(struct vt *vt, bool on)
{
    for (int i = 0; i < vt->nparams; i++) {
        int p = vt->params[i];

        if (!vt->private_mode)
            continue;
        if (p == 25) {
            vt->cursor_visible = on;
        } else if (p == 7) {
            vt->autowrap = on;
        } else if (p == 1049 || p == 1047 || p == 47) {
            if (on != vt->on_alt) {
                struct vt_cell *t = vt->screen;

                if (on) {
                    vt->saved_cx = vt->cx;
                    vt->saved_cy = vt->cy;
                }
                vt->screen = vt->alt;
                vt->alt = t;
                vt->on_alt = on;
                if (on)
                    clear_cells(vt, 0, 0, vt->cols, vt->rows - 1);
                else {
                    vt->cx = vt->saved_cx;
                    vt->cy = vt->saved_cy;
                }
                vt->dirty = true;
            }
        }
    }
}

static void csi(struct vt *vt, char final)
{
    int n = param(vt, 0, 1);

    vt->pending_wrap = false;
    switch (final) {
    case 'A': vt->cy = MAX(vt->cy - n, 0); break;
    case 'B': vt->cy = MIN(vt->cy + n, vt->rows - 1); break;
    case 'C': vt->cx = MIN(vt->cx + n, vt->cols - 1); break;
    case 'D': vt->cx = MAX(vt->cx - n, 0); break;
    case 'E': vt->cx = 0; vt->cy = MIN(vt->cy + n, vt->rows - 1); break;
    case 'F': vt->cx = 0; vt->cy = MAX(vt->cy - n, 0); break;
    case 'G': case '`': vt->cx = MIN(n - 1, vt->cols - 1); break;
    case 'd': vt->cy = MIN(n - 1, vt->rows - 1); break;
    case 'H':
    case 'f':
        vt->cy = MIN(param(vt, 0, 1) - 1, vt->rows - 1);
        vt->cx = MIN(param(vt, 1, 1) - 1, vt->cols - 1);
        break;
    case 'J': {
        int mode = vt->nparams ? vt->params[0] : 0;

        if (mode == 0)
            clear_cells(vt, vt->cx, vt->cy, vt->cols, vt->rows - 1);
        else if (mode == 1)
            clear_cells(vt, 0, 0, vt->cx + 1, vt->cy);
        else
            clear_cells(vt, 0, 0, vt->cols, vt->rows - 1);
        if (mode == 3)
            vt->history_len = 0;
        break;
    }
    case 'K': {
        int mode = vt->nparams ? vt->params[0] : 0;

        if (mode == 0)
            clear_cells(vt, vt->cx, vt->cy, vt->cols, vt->cy);
        else if (mode == 1)
            clear_cells(vt, 0, vt->cy, vt->cx + 1, vt->cy);
        else
            clear_cells(vt, 0, vt->cy, vt->cols, vt->cy);
        break;
    }
    case 'L':
        if (vt->cy >= vt->top && vt->cy <= vt->bottom) {
            int t = vt->top;

            vt->top = vt->cy;
            scroll_down(vt, n);
            vt->top = t;
        }
        break;
    case 'M':
        if (vt->cy >= vt->top && vt->cy <= vt->bottom) {
            int t = vt->top;

            vt->top = vt->cy;
            scroll_up(vt, n);
            vt->top = t;
        }
        break;
    case 'P': {
        struct vt_cell *row = cell(vt, 0, vt->cy);

        n = MIN(n, vt->cols - vt->cx);
        memmove(row + vt->cx, row + vt->cx + n, (vt->cols - vt->cx - n) * sizeof(*row));
        clear_cells(vt, vt->cols - n, vt->cy, vt->cols, vt->cy);
        break;
    }
    case '@': {
        struct vt_cell *row = cell(vt, 0, vt->cy);

        n = MIN(n, vt->cols - vt->cx);
        memmove(row + vt->cx + n, row + vt->cx, (vt->cols - vt->cx - n) * sizeof(*row));
        clear_cells(vt, vt->cx, vt->cy, vt->cx + n, vt->cy);
        break;
    }
    case 'X':
        clear_cells(vt, vt->cx, vt->cy, MIN(vt->cx + n, vt->cols), vt->cy);
        break;
    case 'S': scroll_up(vt, n); break;
    case 'T': scroll_down(vt, n); break;
    case 'm': sgr(vt); break;
    case 'r':
        vt->top = MIN(param(vt, 0, 1) - 1, vt->rows - 1);
        vt->bottom = MIN(param(vt, 1, vt->rows) - 1, vt->rows - 1);
        if (vt->top >= vt->bottom) {
            vt->top = 0;
            vt->bottom = vt->rows - 1;
        }
        vt->cx = vt->cy = 0;
        break;
    case 's': vt->saved_cx = vt->cx; vt->saved_cy = vt->cy; break;
    case 'u': vt->cx = vt->saved_cx; vt->cy = vt->saved_cy; break;
    case 'h': set_mode(vt, true); break;
    case 'l': set_mode(vt, false); break;
    case 'n':
        if (vt->nparams && vt->params[0] == 6)
            vt->reply_len = snprintf(vt->reply, sizeof(vt->reply), "\x1b[%d;%dR", vt->cy + 1, vt->cx + 1);
        else if (vt->nparams && vt->params[0] == 5)
            vt->reply_len = snprintf(vt->reply, sizeof(vt->reply), "\x1b[0n");
        break;
    case 'c':
        vt->reply_len = snprintf(vt->reply, sizeof(vt->reply), "\x1b[?62;22c");
        break;
    }
    vt->dirty = true;
}

static void osc_done(struct vt *vt)
{
    vt->osc[vt->osc_len] = 0;
    if ((vt->osc[0] == '0' || vt->osc[0] == '2') && vt->osc[1] == ';') {
        strlcpy(vt->title, vt->osc + 2, sizeof(vt->title));
        vt->title_changed = true;
    }
}

static void control(struct vt *vt, unsigned char c)
{
    switch (c) {
    case '\r': vt->cx = 0; vt->pending_wrap = false; break;
    case '\n': case '\v': case '\f': newline(vt); vt->pending_wrap = false; break;
    case '\b':
        if (vt->cx > 0)
            vt->cx--;
        vt->pending_wrap = false;
        break;
    case '\t':
        vt->cx = MIN((vt->cx / 8 + 1) * 8, vt->cols - 1);
        break;
    case 0x1B: vt->state = ESCAPE; break;
    }
    vt->dirty = true;
}

static void byte(struct vt *vt, unsigned char c)
{
    switch (vt->state) {
    case ESCAPE:
        vt->state = GROUND;
        switch (c) {
        case '[':
            vt->state = CSI;
            vt->nparams = 0;
            vt->params[0] = 0;
            vt->private_mode = false;
            return;
        case ']': vt->state = OSC; vt->osc_len = 0; return;
        case '(': case ')': vt->state = CHARSET; return;
        case '7': vt->saved_cx = vt->cx; vt->saved_cy = vt->cy; vt->saved_pen = vt->pen; return;
        case '8': vt->cx = vt->saved_cx; vt->cy = vt->saved_cy; vt->pen = vt->saved_pen; return;
        case 'D': newline(vt); return;
        case 'E': vt->cx = 0; newline(vt); return;
        case 'M':
            if (vt->cy == vt->top)
                scroll_down(vt, 1);
            else if (vt->cy > 0)
                vt->cy--;
            return;
        case 'c': reset(vt); return;
        }
        return;
    case CHARSET:
        vt->state = GROUND;
        return;
    case CSI:
        if (c >= '0' && c <= '9') {
            if (!vt->nparams)
                vt->nparams = 1;
            vt->params[vt->nparams - 1] = MIN(vt->params[vt->nparams - 1] * 10 + (c - '0'), 99999);
        } else if (c == ';' || c == ':') {
            if (!vt->nparams)
                vt->nparams = 1;
            if (vt->nparams < 16)
                vt->params[vt->nparams++] = 0;
        } else if (c == '?' || c == '>' || c == '=') {
            vt->private_mode = true;
        } else if (c >= 0x40 && c <= 0x7E) {
            vt->state = GROUND;
            csi(vt, c);
        } else if (c < 0x20) {
            control(vt, c);
        }
        return;
    case OSC:
        if (c == 7) {
            osc_done(vt);
            vt->state = GROUND;
        } else if (c == 0x1B) {
            vt->state = OSC_ESC;
        } else if (vt->osc_len < (int)sizeof(vt->osc) - 1) {
            vt->osc[vt->osc_len++] = c;
        }
        return;
    case OSC_ESC:
        osc_done(vt);
        vt->state = GROUND;
        return;
    }
    // Ground state: UTF-8 text and control characters.
    if (vt->utf8_left) {
        if ((c & 0xC0) == 0x80) {
            vt->utf8_cp = (vt->utf8_cp << 6) | (c & 0x3F);
            if (--vt->utf8_left == 0)
                put_char(vt, vt->utf8_cp);
            return;
        }
        vt->utf8_left = 0;
        put_char(vt, 0xFFFD);
    }
    if (c < 0x20 || c == 0x7F) {
        if (c != 0x7F)
            control(vt, c);
    } else if (c < 0x80) {
        put_char(vt, c);
    } else if ((c & 0xE0) == 0xC0) {
        vt->utf8_cp = c & 0x1F;
        vt->utf8_left = 1;
    } else if ((c & 0xF0) == 0xE0) {
        vt->utf8_cp = c & 0x0F;
        vt->utf8_left = 2;
    } else if ((c & 0xF8) == 0xF0) {
        vt->utf8_cp = c & 0x07;
        vt->utf8_left = 3;
    } else {
        put_char(vt, 0xFFFD);
    }
}

void vt_write(struct vt *vt, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        byte(vt, (unsigned char)data[i]);
}
