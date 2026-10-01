#ifndef AEGIS_DISPLAY_H
#define AEGIS_DISPLAY_H

#include "kernel.h"
#include "bootinfo.h"

#define FONT_WIDTH      8
#define FONT_HEIGHT     16
#define FONT_FIRST      0x20
#define FONT_GLYPHS     95

#define DISPLAY_MAX     AEGIS_MAX_FRAMEBUFFERS

#define COLOR_BLACK     0x000000
#define COLOR_GREY      0xAAAAAA
#define COLOR_WHITE     0xFFFFFF
#define COLOR_RED       0xAA0000
#define COLOR_GREEN     0x00AA00
#define COLOR_YELLOW    0xFFFF55
#define COLOR_CYAN      0x00AAAA

struct display {
    bool present;
    volatile uint8_t *vram;
    uint8_t *back;
    uint8_t *target;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bytes_per_pixel;
    uint8_t red_shift, green_shift, blue_shift;
    uint8_t red_bits, green_bits, blue_bits;
    uint32_t dirty_top, dirty_bottom;
};

extern const uint8_t font8x16[FONT_GLYPHS][FONT_HEIGHT];

void display_init(const struct aegis_framebuffer *fbs, uint32_t count);
void display_enable_backbuffers(void);
uint32_t display_count(void);
struct display *display_get(uint32_t index);
struct display *display_primary(void);

uint32_t fb_color(const struct display *d, uint32_t rgb);
void fb_put_pixel(struct display *d, uint32_t x, uint32_t y, uint32_t rgb);
void fb_fill_rect(struct display *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_draw_glyph(struct display *d, uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void fb_scroll(struct display *d, uint32_t lines, uint32_t bg);
void fb_flush(struct display *d);

int display_set_mode(uint32_t width, uint32_t height);

void console_init(struct display *d);
void console_size(uint32_t *rows, uint32_t *cols);
void console_putc(char c);
void console_write(const char *s, size_t len);
void console_set_color(uint32_t fg, uint32_t bg);
void console_clear(void);
void console_set_hidden(bool hidden);
bool console_hidden(void);

// Boot splash: shown until a program prints to the console, takes the display
// over, or verbose output is requested (F13 or "verbose" on the command line).
void splash_start(void);
void splash_request_verbose(void);
void splash_user_output(void);
bool splash_active(void);

// One program (the window system) may own the display through /dev/fb0.
void display_devfs_init(void);
bool display_claimed(void);

#endif
