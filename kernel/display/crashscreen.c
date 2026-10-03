#include "display.h"
#include "string.h"
#include "stopcodes.h"

// The crash screen: what a kernel panic shows when the graphical desktop
// (or sign-in screen) has the display. It is drawn straight to the
// framebuffer with the console font, because nothing else can be trusted
// once the kernel has failed: a top bar with the logo, the name and "an
// error occurred", then the stop code, its line of text and the stop code
// again with its number, then the technical report and "System halted".
//
// Errors before the desktop starts keep the plain red text screen.

#define RED         0xB3141C
#define RED_DARK    0x7D0A10
#define WHITE       0xFFFFFF
#define PALE        0xF6D5D7

static struct display *d;

static void text(uint32_t x, uint32_t y, const char *s, size_t n, uint32_t scale, uint32_t color)
{
    for (size_t i = 0; i < n && s[i]; i++, x += FONT_WIDTH * scale) {
        unsigned char c = s[i];

        if (x + FONT_WIDTH * scale > d->width)
            break;
        if (c < FONT_FIRST || c >= FONT_FIRST + FONT_GLYPHS)
            continue;
        for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
            uint8_t bits = font8x16[c - FONT_FIRST][row];

            for (uint32_t col = 0; col < FONT_WIDTH; col++)
                if (bits & (0x80 >> col))
                    fb_fill_rect(d, x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void centered(uint32_t y, const char *s, uint32_t scale, uint32_t color)
{
    size_t n = strlen(s);
    uint32_t w = n * FONT_WIDTH * scale;

    text(w < d->width ? (d->width - w) / 2 : 0, y, s, n, scale, color);
}

static void shield_inner(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    for (uint32_t row = 0; row < h; row++) {
        uint32_t half = w / 2;

        if (row > h * 2 / 5)
            half = half * (h - row) / (h - h * 2 / 5);
        fb_fill_rect(d, x + w / 2 - half, y + row, half * 2, 1, RED);
    }
}

// The Aegis shield: straight sides, then narrowing to a point.
static void shield(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t row = 0; row < h; row++) {
        uint32_t half = w / 2;

        if (row > h * 2 / 5) {
            uint32_t t = row - h * 2 / 5, span = h - h * 2 / 5;

            half = half * (span - t) / span;
        }
        // Rounded top corners.
        if (row < w / 8)
            half = half - (w / 8 - row) / 2;
        fb_fill_rect(d, x + w / 2 - half, y + row, half * 2, 1, color);
    }
    // A darker inner shield, so it reads as a badge.
    if (w > 12 && h > 12 && color == WHITE)
        shield_inner(x + w / 5, y + h / 6, w - 2 * (w / 5), h - h / 3);
}

void crash_screen_draw(uint32_t code, const char *report)
{
    const struct stop_code *sc = stop_code_find(code);
    uint32_t big = 3, bar_h, y, line_h = FONT_HEIGHT + 2;
    char line[160];

    if (!(d = display_primary()) || !d->present)
        return;
    if (d->width >= 1800)
        big = 4;
    bar_h = FONT_HEIGHT * 2 + 32;

    fb_fill_rect(d, 0, 0, d->width, d->height, RED);

    // The top bar: logo, name, and what happened, centred.
    {
        const char *name = "Aegis", *what = "An error occurred";
        uint32_t logo = FONT_HEIGHT * 2, gap = 14;
        uint32_t w = logo + gap + strlen(name) * FONT_WIDTH * 2 + 3 * FONT_WIDTH * 2 + strlen(what) * FONT_WIDTH * 2;
        uint32_t x = w < d->width ? (d->width - w) / 2 : 0, ty = (bar_h - FONT_HEIGHT * 2) / 2;

        fb_fill_rect(d, 0, 0, d->width, bar_h, RED_DARK);
        shield(x, (bar_h - logo) / 2, logo * 4 / 5, logo, WHITE);
        x += logo + gap;
        text(x, ty, name, strlen(name), 2, WHITE);
        x += (strlen(name) + 1) * FONT_WIDTH * 2;
        text(x, ty, "-", 1, 2, PALE);
        x += 2 * FONT_WIDTH * 2;
        text(x, ty, what, strlen(what), 2, PALE);
    }

    // The stop code, its line, and the code again with its number.
    y = bar_h + (d->height > 700 ? 56 : 28);
    centered(y, sc->name, big, WHITE);
    y += FONT_HEIGHT * big + 18;
    centered(y, sc->quip, 1, PALE);
    y += line_h + 6;
    ksnprintf(line, sizeof(line), "Stop code: %s (0x%03x)", sc->name, code);
    centered(y, line, 1, WHITE);
    y += line_h + 22;
    fb_fill_rect(d, d->width / 8, y, d->width * 3 / 4, 1, PALE);
    y += 18;

    // The technical report, left-aligned in a centred column.
    {
        uint32_t col_w = d->width * 3 / 4, x = d->width / 8, cols = col_w / FONT_WIDTH;
        uint32_t bottom = d->height - line_h * 3;
        const char *p = report ? report : "";

        while (*p == '\n')
            p++;
        while (*p && y + line_h < bottom) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);

            // Long lines wrap.
            for (size_t done = 0; (done < n || n == 0) && y + line_h < bottom;) {
                size_t take = n - done > cols ? cols : n - done;

                text(x, y, p + done, take, 1, WHITE);
                y += line_h;
                done += take;
                if (!n)
                    break;
            }
            p = e ? e + 1 : p + n;
        }
    }

    centered(d->height - line_h * 2, "System halted. Restart the computer to continue.", 1, WHITE);
    fb_flush(d);
}
