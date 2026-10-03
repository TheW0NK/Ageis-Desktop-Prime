#include "kernel.h"
#include "apic.h"
#include "devfs.h"
#include "mem.h"
#include "sched.h"
#include "string.h"
#include "vfs.h"
#include "vm.h"
#include "abi/errno.h"
#include "abi/video.h"

// The test camera: /osystem/devices/camera when there is no real camera (QEMU has none
// to offer). Each frame is drawn on demand: a sky, a sun that moves across
// it, a ball that bounces, colour bars and a running clock, so programs see
// a live picture. Integer arithmetic only.

#define WIDTH       640
#define HEIGHT      480
#define FPS         15
#define DEVFS_GID_VIDEO 44

static const int8_t sine[64] = {
    0, 12, 25, 37, 48, 59, 70, 80, 89, 97, 104, 110, 115, 119, 122, 123, 124, 123, 122, 119, 115, 110,
    104, 97, 89, 80, 70, 59, 48, 37, 25, 12, 0, -12, -25, -37, -48, -59, -70, -80, -89, -97, -104,
    -110, -115, -119, -122, -123, -124, -123, -122, -119, -115, -110, -104, -97, -89, -80, -70, -59,
    -48, -37, -25, -12,
};

// 3x5 digits for the clock.
static const uint16_t digits[11] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF, 0x0410,
};

static uint32_t mix(uint32_t a, uint32_t b, int t)        // t: 0..256
{
    uint32_t r = ((a >> 16 & 0xFF) * (256 - t) + (b >> 16 & 0xFF) * t) >> 8;
    uint32_t g = ((a >> 8 & 0xFF) * (256 - t) + (b >> 8 & 0xFF) * t) >> 8;
    uint32_t bl = ((a & 0xFF) * (256 - t) + (b & 0xFF) * t) >> 8;

    return r << 16 | g << 8 | bl;
}

static void draw_digit(uint32_t *px, int x0, int y0, int d, int scale, uint32_t c)
{
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 3; x++)
            if (digits[d] >> (14 - (y * 3 + x)) & 1)
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++) {
                        int X = x0 + x * scale + dx, Y = y0 + y * scale + dy;

                        if (X >= 0 && X < WIDTH && Y >= 0 && Y < HEIGHT)
                            px[Y * WIDTH + X] = c;
                    }
}

static void render(uint32_t *px, uint64_t ms)
{
    uint64_t frame = ms * FPS / 1000;
    int sun_x = (int)((ms / 40) % (WIDTH + 160)) - 80, sun_y = 140 - sine[(ms / 160) % 64] / 3;
    int ball_x = WIDTH / 2 + sine[(ms / 30) % 64] * 2, ball_y = 330 - (sine[(ms / 25) % 32] * 120 / 124);
    int horizon = 300;
    static const uint32_t bars[7] = { 0xC0C0C0, 0xC0C000, 0x00C0C0, 0x00C000, 0xC000C0, 0xC00000, 0x0000C0 };
    uint64_t secs = ms / 1000;

    for (int y = 0; y < HEIGHT; y++) {
        uint32_t *row = px + y * WIDTH;

        for (int x = 0; x < WIDTH; x++) {
            uint32_t c;

            if (y < horizon) {
                c = mix(0x5A8FE0, 0xF6C79A, y * 256 / horizon);
            } else if (y < 440) {
                // A field with stripes that drift.
                int s = ((x + (int)(frame * 2)) / 24 + (y - horizon) / 12) & 1;

                c = mix(0x3E8E4A, 0x2C6B36, (y - horizon) * 256 / 140);
                if (s)
                    c = mix(c, 0x000000, 24);
            } else {
                c = bars[x * 7 / WIDTH];
            }
            {
                int dx = x - sun_x, dy = y - sun_y, d2 = dx * dx + dy * dy;

                if (y < horizon && d2 < 40 * 40)
                    c = 0xFFF2C4;
                else if (y < horizon && d2 < 90 * 90)
                    c = mix(c, 0xFFE3A0, 256 - (d2 - 1600) * 256 / (8100 - 1600));
            }
            {
                int dx = x - ball_x, dy = y - ball_y, d2 = dx * dx + dy * dy;

                if (d2 < 34 * 34)
                    c = d2 < 14 * 14 && dx < 0 && dy < 0 ? 0xFFB0B0 : 0xD9363E;
                else if (y > ball_y + 30 && y < ball_y + 44 && dx * dx < 34 * 34 && y < 440)
                    c = mix(c, 0x000000, 60);
            }
            row[x] = c;
        }
    }
    // Clock: HH:MM:SS of uptime and a frame counter.
    {
        int d[8] = { (int)(secs / 36000 % 10), (int)(secs / 3600 % 10), 10, (int)(secs / 600 % 6),
                     (int)(secs / 60 % 10), 10, (int)(secs / 10 % 6), (int)(secs % 10) };

        for (int i = 0; i < 8; i++)
            draw_digit(px, 20 + i * 20, 20, d[i], 5, 0xFFFFFF);
        for (int i = 0; i < 4; i++)
            draw_digit(px, WIDTH - 100 + i * 20, 20, (int)(frame / (uint64_t[]){ 1000, 100, 10, 1 }[i] % 10), 5,
                       0xFFFFFF);
    }
}

#define FRAME_BYTES (WIDTH * HEIGHT * 4)
#define FRAME_PAGES ((FRAME_BYTES + PAGE_SIZE - 1) / PAGE_SIZE)

// A frame is bigger than one read: the first read draws it into the
// reader's buffer, and reads hand it out in pieces until it is all taken.
struct reader {
    uint64_t last_frame;
    uint32_t *frame;
    size_t offset;                  // into the frame; FRAME_BYTES when used up
};

static int64_t vcam_read(struct file *f, void *buf, size_t size)
{
    struct reader *r = f->priv;
    size_t n;

    if (r->offset >= FRAME_BYTES) {
        uint64_t now, frame;

        // Wait for the next frame time.
        for (;;) {
            now = timer_uptime_ms();
            frame = now * FPS / 1000;
            if (frame != r->last_frame)
                break;
            sched_sleep((frame + 1) * 1000 / FPS - now + 1);
        }
        r->last_frame = frame;
        render(r->frame, now);
        r->offset = 0;
    }
    n = MIN(size, FRAME_BYTES - r->offset);
    memcpy(buf, (uint8_t *)r->frame + r->offset, n);
    r->offset += n;
    return n;
}

static int64_t vcam_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    (void)f;
    if (cmd == IOCTL_VIDEO_INFO) {
        struct aegis_videoinfo info = { WIDTH, HEIGHT, VIDEO_FORMAT_XRGB32, FPS, "Aegis test camera" };

        return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
    }
    return -ENOTTY;
}

static void vcam_close(struct file *f)
{
    struct reader *r = f->priv;

    pmm_free_pages((uint64_t)r->frame, FRAME_PAGES);
    kfree(r);
}

static const struct file_ops vcam_ops = { .read = vcam_read, .ioctl = vcam_ioctl, .close = vcam_close };

static int vcam_open(void *ctx, uint32_t flags, struct file **out)
{
    struct file *f;

    (void)ctx;
    if (!(f = file_alloc(&vcam_ops, flags)))
        return -ENOMEM;
    {
        struct reader *r = kzalloc(sizeof(*r));

        if (!r || !(r->frame = (uint32_t *)pmm_alloc_pages(FRAME_PAGES))) {
            kfree(r);
            kfree(f);
            return -ENOMEM;
        }
        r->offset = FRAME_BYTES;
        f->priv = r;
    }
    *out = f;
    return 0;
}

void vcam_init(void)
{
    devfs_register("camera", 0660, 0, DEVFS_GID_VIDEO, vcam_open, NULL);
}
