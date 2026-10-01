#include "aegis.h"
#include "wm.h"

// A window-system test client: draws, shows typed text and click points.

static char typed[128];
static int dots[32][2], ndots;

static void draw(struct wm_window *w)
{
    struct gfx g;
    struct font *f = font_get(FONT_SANS, 15), *big = font_get(FONT_SANS_BOLD, 22);
    char info[96];

    gfx_init(&g, w->surface);
    gfx_fill(&g, (struct rect){ 0, 0, w->width, w->height }, RGB(0xFAFBFD));
    text_draw(&g, big, 20, 16, "Window system test", -1, RGB(0x1D2633));
    snprintf(info, sizeof(info), "Size %d x %d, %s", w->width, w->height, w->focused ? "focused" : "not focused");
    text_draw(&g, f, 20, 52, info, -1, RGB(0x5A6575));
    text_draw(&g, f, 20, 80, "Typed:", -1, RGB(0x5A6575));
    gfx_fill_rounded(&g, (struct rect){ 80, 74, w->width - 100, 28 }, 6, RGB(0xFFFFFF));
    gfx_outline_rounded(&g, (struct rect){ 80, 74, w->width - 100, 28 }, 6, 1, RGB(0xC3CBD6));
    text_draw(&g, f, 88, 80, typed, -1, RGB(0x1D2633));
    for (int i = 0; i < ndots; i++)
        gfx_circle(&g, dots[i][0], dots[i][1], 6, RGB(0x3D8BFF));
    wm_present_all(w);
}

int main(int argc, char **argv)
{
    struct wm_window *w;
    struct wm_event ev;

    if (!wm_connect()) {
        dprintf(STDERR_FILENO, "wmdemo: no display\n");
        return 1;
    }
    if (!(w = wm_create(argc > 1 ? argv[1] : "Window test", 460, 300, WM_ROLE_NORMAL)))
        return 1;
    draw(w);
    while (wm_connected()) {
        if (!wm_next_event(&ev, -1))
            continue;
        switch (ev.type) {
        case WM_EV_CLOSE:
            wm_destroy(w);
            return 0;
        case WM_EV_KEY:
            if (ev.value && ev.key == KEY_BACKSPACE && typed[0])
                typed[strlen(typed) - 1] = 0;
            else if (ev.value && ev.text[0] && strlen(typed) + strlen(ev.text) < sizeof(typed))
                strcat(typed, ev.text);
            draw(w);
            break;
        case WM_EV_POINTER:
            if (ev.kind == WM_PTR_DOWN && ndots < 32) {
                dots[ndots][0] = ev.x;
                dots[ndots][1] = ev.y;
                ndots++;
                draw(w);
            }
            break;
        case WM_EV_RESIZE:
        case WM_EV_FOCUS:
            draw(w);
            break;
        default:
            break;
        }
    }
    return 0;
}
