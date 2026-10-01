#include "display.h"
#include "string.h"

static uint32_t scale(uint32_t value, uint8_t bits)
{
    return bits >= 8 ? value << (bits - 8) : value >> (8 - bits);
}

static inline void store(uint8_t *p, uint32_t bpp, uint32_t color)
{
    switch (bpp) {
    case 4:
        *(uint32_t *)p = color;
        break;
    case 3:
        p[0] = color;
        p[1] = color >> 8;
        p[2] = color >> 16;
        break;
    default:
        *(uint16_t *)p = color;
        break;
    }
}

static void mark_dirty(struct display *d, uint32_t y, uint32_t h)
{
    if (!d->back)
        return;
    if (d->dirty_bottom <= d->dirty_top) {
        d->dirty_top = y;
        d->dirty_bottom = y + h;
        return;
    }
    d->dirty_top = MIN(d->dirty_top, y);
    d->dirty_bottom = MAX(d->dirty_bottom, y + h);
}

uint32_t fb_color(const struct display *d, uint32_t rgb)
{
    return scale((rgb >> 16) & 0xFF, d->red_bits) << d->red_shift
         | scale((rgb >> 8) & 0xFF, d->green_bits) << d->green_shift
         | scale(rgb & 0xFF, d->blue_bits) << d->blue_shift;
}

void fb_put_pixel(struct display *d, uint32_t x, uint32_t y, uint32_t rgb)
{
    if (!d || !d->present || x >= d->width || y >= d->height)
        return;
    store(d->target + (uint64_t)y * d->pitch + x * d->bytes_per_pixel,
          d->bytes_per_pixel, fb_color(d, rgb));
    mark_dirty(d, y, 1);
}

void fb_fill_rect(struct display *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    uint32_t color, bpp;

    if (!d || !d->present || x >= d->width || y >= d->height)
        return;
    w = MIN(w, d->width - x);
    h = MIN(h, d->height - y);
    color = fb_color(d, rgb);
    bpp = d->bytes_per_pixel;

    for (uint32_t row = y; row < y + h; row++) {
        uint8_t *p = d->target + (uint64_t)row * d->pitch + x * bpp;

        if (bpp == 4) {
            uint32_t *q = (uint32_t *)p;
            for (uint32_t i = 0; i < w; i++)
                q[i] = color;
        } else {
            for (uint32_t i = 0; i < w; i++, p += bpp)
                store(p, bpp, color);
        }
    }
    mark_dirty(d, y, h);
}

void fb_draw_glyph(struct display *d, uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    unsigned index = (unsigned char)c - FONT_FIRST;
    uint32_t fgc, bgc, bpp;

    if (!d || !d->present || x + FONT_WIDTH > d->width || y + FONT_HEIGHT > d->height)
        return;
    if (index >= FONT_GLYPHS)
        index = '?' - FONT_FIRST;
    fgc = fb_color(d, fg);
    bgc = fb_color(d, bg);
    bpp = d->bytes_per_pixel;

    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t *p = d->target + (uint64_t)(y + row) * d->pitch + x * bpp;
        uint8_t bits = font8x16[index][row];

        for (uint32_t col = 0; col < FONT_WIDTH; col++, p += bpp)
            store(p, bpp, (bits & (0x80 >> col)) ? fgc : bgc);
    }
    mark_dirty(d, y, FONT_HEIGHT);
}

void fb_scroll(struct display *d, uint32_t lines, uint32_t bg)
{
    if (!d || !d->present)
        return;
    if (lines >= d->height) {
        fb_fill_rect(d, 0, 0, d->width, d->height, bg);
        return;
    }

    memmove(d->target, d->target + (uint64_t)lines * d->pitch,
            (uint64_t)(d->height - lines) * d->pitch);
    fb_fill_rect(d, 0, d->height - lines, d->width, lines, bg);
    mark_dirty(d, 0, d->height);
}

void fb_flush(struct display *d)
{
    if (!d || !d->back || d->dirty_bottom <= d->dirty_top)
        return;

    uint64_t offset = (uint64_t)d->dirty_top * d->pitch;
    memcpy((void *)(d->vram + offset), d->back + offset,
           (uint64_t)(d->dirty_bottom - d->dirty_top) * d->pitch);
    d->dirty_top = d->dirty_bottom = 0;
}
