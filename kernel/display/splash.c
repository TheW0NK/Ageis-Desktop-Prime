#include "display.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"

// The boot splash: the Aegis wordmark and an activity bar on a dark
// background, drawn directly to the screen while the console is hidden.

#define BG          0x101820
#define FG          0xE8EEF4
#define ACCENT      0x3D8BFF
#define DIM         0x2A3440

static volatile bool active, want_verbose, want_stop;
static struct display *disp;

// Draws text from the console font scaled up `scale` times.
static void big_text(struct display *d, uint32_t x, uint32_t y, const char *s, uint32_t scale, uint32_t color)
{
    for (; *s; s++, x += FONT_WIDTH * scale) {
        unsigned char c = *s;

        if (c < FONT_FIRST || c >= FONT_FIRST + FONT_GLYPHS)
            continue;
        for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
            uint8_t bits = font8x16[c - FONT_FIRST][row];

            for (uint32_t col = 0; col < FONT_WIDTH; col++) {
                if (bits & (0x80 >> col))
                    fb_fill_rect(d, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw(struct display *d)
{
    uint32_t scale = d->width >= 1600 ? 8 : d->width >= 1000 ? 6 : 4;
    uint32_t tw = 5 * FONT_WIDTH * scale, th = FONT_HEIGHT * scale;
    uint32_t x = (d->width - tw) / 2, y = (d->height - th) / 2 - th / 2;
    const char *sub = "Starting up";

    fb_fill_rect(d, 0, 0, d->width, d->height, BG);
    big_text(d, x, y, "Aegis", scale, FG);
    big_text(d, (d->width - strlen(sub) * FONT_WIDTH) / 2, y + th + 24, sub, 1, 0x8899AA);
    fb_flush(d);
}

static void draw_bar(struct display *d, uint32_t step)
{
    uint32_t w = MIN(d->width / 4, 320U), h = 4;
    uint32_t x = (d->width - w) / 2, y = d->height * 2 / 3;
    uint32_t seg = w / 4, pos = (step * 8) % (w + seg);

    fb_fill_rect(d, x, y, w, h, DIM);
    if (pos < w + seg) {
        uint32_t start = pos > seg ? pos - seg : 0, end = MIN(pos, w);

        if (end > start)
            fb_fill_rect(d, x + start, y, end - start, h, ACCENT);
    }
    fb_flush(d);
}

static void replay_log(void)
{
    char buf[512];
    uint64_t pos = 0;
    size_t n;

    while ((n = klog_read(&pos, buf, sizeof(buf))))
        console_write(buf, n);
}

static void splash_thread(void *arg)
{
    uint32_t step = 0;

    (void)arg;
    while (active) {
        if (want_verbose || want_stop) {
            active = false;
            console_set_quiet(false);
            if (!display_claimed()) {
                console_set_hidden(false);
                if (want_verbose) {
                    console_clear();
                    replay_log();
                }
            }
            break;
        }
        if (!display_claimed())
            draw_bar(disp, step++);
        sched_sleep(16);
    }
}

void splash_start(void)
{
    if (!(disp = display_primary()))
        return;
    console_set_hidden(true);
    console_set_quiet(true);
    active = true;
    draw(disp);
    if (!thread_create("splash", splash_thread, NULL)) {
        active = false;
        console_set_quiet(false);
        console_set_hidden(false);
    }
}

void splash_request_verbose(void)
{
    if (active)
        want_verbose = true;
}

// A program wrote to the console: it is being used as a terminal.
void splash_user_output(void)
{
    if (active)
        want_stop = true;
}

bool splash_active(void)
{
    return active;
}
