#include "aegis.h"
#include "gfx.h"
#include "abi/fb.h"

// Draws a test card on the screen with libgfx, then waits for a key.
int main(void)
{
    struct aegis_fbinfo fi;
    int fd = open("/dev/fb0", O_RDWR);
    uint32_t *fb;
    struct surface *s;
    struct gfx g;
    struct font *title, *body, *mono;

    if (fd < 0 || ioctl(fd, IOCTL_FB_INFO, (unsigned long)&fi) < 0) {
        perror("gfxdemo: /dev/fb0");
        return 1;
    }
    fb = mmap(NULL, fi.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED || !(s = surface_create(fi.width, fi.height))) {
        perror("gfxdemo: mmap");
        return 1;
    }
    gfx_init(&g, s);
    gfx_gradient(&g, (struct rect){ 0, 0, s->width, s->height }, RGB(0x1B2B44), RGB(0x0D1421));
    title = font_get(FONT_SANS_BOLD, 32);
    body = font_get(FONT_SANS, 16);
    mono = font_get(FONT_MONO, 14);

    struct rect win = { 120, 100, 520, 340 };
    gfx_shadow(&g, win, 10, 24, ARGB(140, 0, 0, 0));
    gfx_fill_rounded(&g, win, 10, RGB(0xF4F6FA));
    gfx_fill_rounded(&g, (struct rect){ win.x, win.y, win.w, 40 }, 10, RGB(0xDDE3EC));
    gfx_fill(&g, (struct rect){ win.x, win.y + 30, win.w, 10 }, RGB(0xDDE3EC));
    text_draw(&g, body, win.x + 16, win.y + 11, "Aegis graphics test", -1, RGB(0x222831));
    gfx_circle(&g, win.x + win.w - 20, win.y + 20, 7, RGB(0xE5534B));
    text_draw(&g, title, win.x + 24, win.y + 60, "Hello, desktop", -1, RGB(0x1A2433));
    text_draw(&g, body, win.x + 24, win.y + 108, "Anti-aliased text: \xC3\xA9t\xC3\xA9, \xC3\xBC" "ber, se\xC3\xB1or, \xE2\x82\xAC 42", -1, RGB(0x3A4656));
    text_draw(&g, mono, win.x + 24, win.y + 140, "int main(void) { return 0; }", -1, RGB(0x0A7A3E));
    gfx_fill_rounded(&g, (struct rect){ win.x + 24, win.y + 180, 120, 36 }, 8, RGB(0x3D8BFF));
    text_draw(&g, body, win.x + 24 + (120 - text_width(body, "Button", -1)) / 2, win.y + 189, "Button", -1, RGB(0xFFFFFF));
    gfx_outline_rounded(&g, (struct rect){ win.x + 160, win.y + 180, 120, 36 }, 8, 2, RGB(0x3D8BFF));
    gfx_line(&g, win.x + 300, win.y + 230, win.x + 480, win.y + 190, 3, RGB(0xF2A33A));
    gfx_ring(&g, win.x + 420, win.y + 280, 30, 4, ARGB(200, 120, 80, 220));
    gfx_circle(&g, win.x + 340, win.y + 280, 26, ARGB(160, 40, 180, 120));
    gfx_fill(&g, (struct rect){ win.x + 24, win.y + 240, 200, 60 }, ARGB(128, 255, 0, 0));
    gfx_fill(&g, (struct rect){ win.x + 74, win.y + 260, 200, 60 }, ARGB(128, 0, 0, 255));

    // The framebuffer here is BGRX, the same layout as our pixels.
    for (int y = 0; y < s->height; y++)
        memcpy((uint8_t *)fb + (size_t)y * fi.pitch, s->pixels + (size_t)y * s->stride, s->width * 4);
    printf("gfxdemo: drawn %ux%u\n", fi.width, fi.height);
    char c;
    read(STDIN_FILENO, &c, 1);
    return 0;
}
