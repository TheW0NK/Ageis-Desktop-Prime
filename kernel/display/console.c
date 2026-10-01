#include "display.h"
#include "string.h"

#define MAX_COLS    512
#define MAX_ROWS    256

struct cell {
    char c;
    uint32_t fg, bg;
};

static const uint32_t palette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

static struct cell cells[MAX_COLS * MAX_ROWS];

static struct {
    struct display *d;
    uint32_t cols, rows;
    uint32_t x, y;
    uint32_t fg, bg;
    uint32_t default_fg, default_bg;
    bool bold;
    int state;
    int params[4];
    int nparams;
    bool cursor_drawn;
} con;

static struct cell *cell_at(uint32_t x, uint32_t y)
{
    return &cells[y * MAX_COLS + x];
}

static void draw_cell(uint32_t x, uint32_t y)
{
    struct cell *c = cell_at(x, y);

    fb_draw_glyph(con.d, x * FONT_WIDTH, y * FONT_HEIGHT, c->c ? c->c : ' ', c->fg, c->bg);
}

static void cursor_hide(void)
{
    if (con.cursor_drawn && con.x < con.cols && con.y < con.rows)
        draw_cell(con.x, con.y);
    con.cursor_drawn = false;
}

static void cursor_show(void)
{
    uint32_t x = MIN(con.x, con.cols - 1);

    fb_fill_rect(con.d, x * FONT_WIDTH, con.y * FONT_HEIGHT + FONT_HEIGHT - 2, FONT_WIDTH, 2, con.fg);
    con.cursor_drawn = true;
}

static void clear_cells(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y)
{
    for (uint32_t y = from_y; y <= to_y && y < con.rows; y++) {
        uint32_t x0 = y == from_y ? from_x : 0;
        uint32_t x1 = y == to_y ? to_x : con.cols - 1;

        for (uint32_t x = x0; x <= x1 && x < con.cols; x++)
            *cell_at(x, y) = (struct cell){ ' ', con.fg, con.bg };
        if (x0 <= x1 && x0 < con.cols)
            fb_fill_rect(con.d, x0 * FONT_WIDTH, y * FONT_HEIGHT,
                         (MIN(x1, con.cols - 1) - x0 + 1) * FONT_WIDTH, FONT_HEIGHT, con.bg);
    }
}

void console_init(struct display *d)
{
    con.default_fg = con.fg = COLOR_GREY;
    con.default_bg = con.bg = COLOR_BLACK;
    if (!d || d->width < FONT_WIDTH || d->height < FONT_HEIGHT)
        return;

    con.d = d;
    con.cols = MIN(d->width / FONT_WIDTH, MAX_COLS);
    con.rows = MIN(d->height / FONT_HEIGHT, MAX_ROWS);
    console_clear();
}

void console_size(uint32_t *rows, uint32_t *cols)
{
    *rows = con.rows;
    *cols = con.cols;
}

void console_clear(void)
{
    if (!con.d)
        return;
    fb_fill_rect(con.d, 0, 0, con.d->width, con.d->height, con.bg);
    for (uint32_t i = 0; i < MAX_COLS * con.rows; i++)
        cells[i] = (struct cell){ ' ', con.fg, con.bg };
    con.x = con.y = 0;
    con.cursor_drawn = false;
    fb_flush(con.d);
}

void console_set_color(uint32_t fg, uint32_t bg)
{
    con.fg = con.default_fg = fg;
    con.bg = con.default_bg = bg;
}

static void scroll(void)
{
    memmove(cells, cells + MAX_COLS, sizeof(struct cell) * MAX_COLS * (con.rows - 1));
    for (uint32_t x = 0; x < con.cols; x++)
        *cell_at(x, con.rows - 1) = (struct cell){ ' ', con.fg, con.bg };
    fb_scroll(con.d, FONT_HEIGHT, con.bg);
}

static void newline(void)
{
    con.x = 0;
    if (++con.y == con.rows) {
        scroll();
        con.y--;
    }
}

static void put_glyph(char c)
{
    if (con.x >= con.cols)
        newline();
    *cell_at(con.x, con.y) = (struct cell){ c, con.fg, con.bg };
    draw_cell(con.x, con.y);
    con.x++;
}

static int param(int i, int def)
{
    return i < con.nparams && con.params[i] > 0 ? con.params[i] : def;
}

static void sgr(void)
{
    if (con.nparams == 0)
        con.params[con.nparams++] = 0;
    for (int i = 0; i < con.nparams; i++) {
        int p = con.params[i];

        if (p == 0) {
            con.fg = con.default_fg;
            con.bg = con.default_bg;
            con.bold = false;
        } else if (p == 1) {
            con.bold = true;
        } else if (p == 22) {
            con.bold = false;
        } else if (p >= 30 && p <= 37) {
            con.fg = palette[p - 30 + (con.bold ? 8 : 0)];
        } else if (p == 39) {
            con.fg = con.default_fg;
        } else if (p >= 40 && p <= 47) {
            con.bg = palette[p - 40];
        } else if (p == 49) {
            con.bg = con.default_bg;
        } else if (p >= 90 && p <= 97) {
            con.fg = palette[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            con.bg = palette[p - 100 + 8];
        }
    }
}

static void csi(char final)
{
    switch (final) {
    case 'A':
        con.y = con.y > (uint32_t)param(0, 1) ? con.y - param(0, 1) : 0;
        break;
    case 'B':
        con.y = MIN(con.y + param(0, 1), con.rows - 1);
        break;
    case 'C':
        con.x = MIN(con.x + param(0, 1), con.cols - 1);
        break;
    case 'D':
        con.x = con.x > (uint32_t)param(0, 1) ? con.x - param(0, 1) : 0;
        break;
    case 'G':
        con.x = MIN((uint32_t)param(0, 1) - 1, con.cols - 1);
        break;
    case 'H':
    case 'f':
        con.y = MIN((uint32_t)param(0, 1) - 1, con.rows - 1);
        con.x = MIN((uint32_t)param(1, 1) - 1, con.cols - 1);
        break;
    case 'J':
        if (param(0, 0) == 2 || param(0, 0) == 3) {
            clear_cells(0, 0, con.cols - 1, con.rows - 1);
        } else if (param(0, 0) == 1) {
            clear_cells(0, 0, con.x, con.y);
        } else {
            clear_cells(con.x, con.y, con.cols - 1, con.rows - 1);
        }
        break;
    case 'K':
        if (param(0, 0) == 2)
            clear_cells(0, con.y, con.cols - 1, con.y);
        else if (param(0, 0) == 1)
            clear_cells(0, con.y, con.x, con.y);
        else
            clear_cells(con.x, con.y, con.cols - 1, con.y);
        break;
    case 'm':
        sgr();
        break;
    }
}

static void put(char c)
{
    if (con.state == 1) {
        if (c == '[') {
            con.state = 2;
            con.nparams = 0;
            memset(con.params, 0, sizeof(con.params));
        } else {
            con.state = 0;
        }
        return;
    }
    if (con.state == 2) {
        if (c >= '0' && c <= '9') {
            if (con.nparams == 0)
                con.nparams = 1;
            if (con.nparams <= 4)
                con.params[con.nparams - 1] = con.params[con.nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (con.nparams == 0)
                con.nparams = 1;
            if (con.nparams < 4)
                con.nparams++;
        } else if (c == '?') {
        } else {
            csi(c);
            con.state = 0;
        }
        return;
    }

    switch (c) {
    case 0x1B:
        con.state = 1;
        return;
    case '\n':
        newline();
        return;
    case '\r':
        con.x = 0;
        return;
    case '\t':
        do
            put_glyph(' ');
        while (con.x % 4 && con.x < con.cols);
        return;
    case '\b':
        if (con.x)
            con.x--;
        return;
    case 0x07:
        return;
    }
    put_glyph(c);
}

void console_putc(char c)
{
    console_write(&c, 1);
}

void console_write(const char *s, size_t len)
{
    if (!con.d)
        return;
    cursor_hide();
    while (len--)
        put(*s++);
    cursor_show();
    fb_flush(con.d);
}
