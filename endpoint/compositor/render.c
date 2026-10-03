#include "compositor.h"

#define CURSOR_SIZE     24

static struct rect damaged[MAX_DAMAGE];
static int ndamaged;
static int cursor_shape;
static struct rect cursor_drawn;

// Theme.
#define DESKTOP_TOP     RGB(0x24324A)
#define DESKTOP_BOTTOM  RGB(0x0F1726)
// Frame colours follow the session's theme (WM_SETTING_CHANGED theme=...).
struct frame_theme {
    color_t active, inactive, border, title, title_dim, content;
};

static const struct frame_theme frame_themes[] = {
    { RGB(0xE8ECF2), RGB(0xF4F6F9), RGB(0xBCC5D1), RGB(0x1D2633), RGB(0x8A94A3), RGB(0xF4F6F9) },     // light
    { RGB(0x2B313A), RGB(0x22272E), RGB(0x48505B), RGB(0xE6EAF0), RGB(0x8892A0), RGB(0x1E2228) },     // dark
    { RGB(0x000000), RGB(0x000000), RGB(0xFFFFFF), RGB(0xFFE600), RGB(0xFFFFFF), RGB(0x000000) },     // contrast
};
static const struct frame_theme *ft = &frame_themes[0];

void set_frame_theme(const char *name)
{
    ft = &frame_themes[!strcmp(name, "dark") ? 1 : !strncmp(name, "high", 4) || !strcmp(name, "contrast") ? 2 : 0];
    damage((struct rect){ 0, 0, screen.width, screen.height });
}

#define FRAME_ACTIVE    (ft->active)
#define FRAME_INACTIVE  (ft->inactive)
#define FRAME_BORDER    (ft->border)
#define TITLE_ACTIVE    (ft->title)
#define TITLE_INACTIVE  (ft->title_dim)
#define CLOSE_HOVER     RGB(0xE5534B)
#define BUTTON_HOVER    ARGB(40, 0, 0, 0)

int hover_button;                   // 1 minimize, 2 maximize, 3 close
struct window *hover_window;

void damage(struct rect r)
{
    struct rect s = { 0, 0, screen.width, screen.height };

    if (!rect_intersect(r, s, &r))
        return;
    for (int i = 0; i < ndamaged; i++) {
        struct rect u = rect_union(damaged[i], r);

        // Merge with an overlapping or nearby rectangle when that wastes little.
        if ((int64_t)u.w * u.h <= (int64_t)damaged[i].w * damaged[i].h + (int64_t)r.w * r.h + 4096) {
            damaged[i] = u;
            return;
        }
    }
    if (ndamaged == MAX_DAMAGE) {
        for (int i = 1; i < ndamaged; i++)
            damaged[0] = rect_union(damaged[0], damaged[i]);
        ndamaged = 1;
        damaged[0] = rect_union(damaged[0], r);
        return;
    }
    damaged[ndamaged++] = r;
}

bool window_framed(struct window *w)
{
    uint32_t role = w->role;

    return role == WM_ROLE_NORMAL || role == WM_ROLE_DIALOG;
}

struct rect window_content(struct window *w)
{
    if (!window_framed(w))
        return w->frame;
    return (struct rect){ w->frame.x + BORDER, w->frame.y + TITLE_H, w->cw, w->ch };
}

struct rect window_bounds(struct window *w)
{
    struct rect r = w->frame;

    if (window_framed(w) || w->role == WM_ROLE_POPUP)
        r = (struct rect){ r.x - SHADOW, r.y - SHADOW, r.w + 2 * SHADOW, r.h + 2 * SHADOW };
    return r;
}

void damage_window(struct window *w)
{
    damage(window_bounds(w));
}

// Title bar buttons, right to left: close, maximize, minimize.
static struct rect title_button(struct window *w, int which)
{
    int size = 26, gap = 4, right = w->frame.x + w->frame.w - 8;
    int x = right - size * (4 - which) - gap * (3 - which);

    return (struct rect){ x, w->frame.y + (TITLE_H - size) / 2, size, size };
}

int title_button_at(struct window *w, int x, int y)
{
    if (!window_framed(w) || y < w->frame.y || y >= w->frame.y + TITLE_H)
        return 0;
    for (int b = 1; b <= 3; b++) {
        if (b != 3 && (w->flags & WM_FLAG_NO_RESIZE) && b == 2)
            continue;
        if (rect_contains(title_button(w, b), x, y))
            return b;
    }
    return 0;
}

static void draw_frame(struct gfx *g, struct window *w)
{
    bool active = w == focused;
    struct rect f = w->frame;
    struct font *font = font_get(active ? FONT_SANS_BOLD : FONT_SANS, 14);
    color_t bar = active ? FRAME_ACTIVE : FRAME_INACTIVE, ink = active ? TITLE_ACTIVE : TITLE_INACTIVE;

    gfx_shadow(g, f, RADIUS, SHADOW, ARGB(active ? 110 : 60, 0, 0, 0));
    gfx_fill_rounded(g, f, RADIUS, FRAME_BORDER);
    gfx_fill_rounded(g, (struct rect){ f.x + 1, f.y + 1, f.w - 2, f.h - 2 }, RADIUS - 1, bar);

    int title_w = f.w - 3 * 30 - 32;
    char title[WM_TEXT_MAX];
    strlcpy(title, w->title, sizeof(title));
    // Shorten long titles with an ellipsis.
    while (title[0] && text_width(font, title, -1) > title_w) {
        int len = strlen(title), cut = utf8_prev(title, len - (len > 3 && !strcmp(title + len - 3, "...") ? 3 : 0));
        strcpy(title + cut, "...");
        if (cut == 0)
            break;
    }
    text_draw(g, font, f.x + 14, f.y + (TITLE_H - font_line_height(font)) / 2, title, -1, ink);

    for (int b = 1; b <= 3; b++) {
        struct rect r = title_button(w, b);
        bool hover = hover_window == w && hover_button == b;
        float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
        color_t c = ink;

        if (b == 2 && (w->flags & WM_FLAG_NO_RESIZE))
            continue;
        if (hover) {
            gfx_fill_rounded(g, r, 6, b == 3 ? CLOSE_HOVER : BUTTON_HOVER);
            if (b == 3)
                c = RGB(0xFFFFFF);
        }
        if (b == 1) {
            gfx_line(g, cx - 5, cy + 4, cx + 5, cy + 4, 1.5f, c);
        } else if (b == 2) {
            if (w->maximized) {
                gfx_outline(g, (struct rect){ (int)cx - 5, (int)cy - 2, 8, 8 }, 1, c);
                gfx_line(g, cx - 2, cy - 5, cx + 5, cy - 5, 1, c);
                gfx_line(g, cx + 5, cy - 5, cx + 5, cy + 2, 1, c);
            } else {
                gfx_outline(g, (struct rect){ (int)cx - 5, (int)cy - 5, 11, 11 }, 1, c);
            }
        } else {
            gfx_line(g, cx - 5, cy - 5, cx + 5, cy + 5, 1.5f, c);
            gfx_line(g, cx + 5, cy - 5, cx - 5, cy + 5, 1.5f, c);
        }
    }
}

static void draw_content(struct gfx *g, struct window *w)
{
    struct rect c = window_content(w), from, area;
    struct surface src;

    if (!w->pixels)
        return;
    src = (struct surface){ w->pixels, w->bw, w->bh, w->bw, false };
    from = (struct rect){ 0, 0, MIN(w->bw, c.w), MIN(w->bh, c.h) };
    // Normal windows are opaque: copy rows instead of blending.
    if (window_framed(w)) {
        struct rect dst = { c.x, c.y, from.w, from.h };

        if (rect_intersect(dst, g->clip, &area)) {
            for (int y = 0; y < area.h; y++) {
                uint32_t *d = g->s->pixels + (size_t)(area.y + y) * g->s->stride + area.x;
                const uint32_t *s = w->pixels + (size_t)(area.y - c.y + y) * w->bw + (area.x - c.x);

                for (int x = 0; x < area.w; x++)
                    d[x] = s[x] | 0xFF000000U;
            }
        }
        // Content smaller than the window (while it catches up with a resize).
        if (from.w < c.w)
            gfx_fill(g, (struct rect){ c.x + from.w, c.y, c.w - from.w, c.h }, ft->content);
        if (from.h < c.h)
            gfx_fill(g, (struct rect){ c.x, c.y + from.h, from.w, c.h - from.h }, ft->content);
        return;
    }
    if (w->role == WM_ROLE_POPUP)
        gfx_shadow(g, c, 6, 14, ARGB(90, 0, 0, 0));
    gfx_blit(g, &src, from, c.x, c.y);
}

// ---- Cursor ----

static const char *const arrow[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..XX..X    ",
    "X.X  X..X   ",
    "XX   X..X   ",
    "X     X..X  ",
    "      X..X  ",
    "       XX   ",
};

void set_cursor_shape(int shape)
{
    if (shape == cursor_shape)
        return;
    cursor_shape = shape;
    damage(cursor_drawn);
    damage((struct rect){ pointer_x - CURSOR_SIZE / 2, pointer_y - CURSOR_SIZE / 2, CURSOR_SIZE * 2, CURSOR_SIZE * 2 });
}

// What is being dragged, in a tag beside the cursor.
static struct rect dnd_badge(void)
{
    struct font *f = font_get(FONT_SANS, 13);
    int w = *dnd.label ? MIN(text_width(f, dnd.label, -1), 220) + 16 : 0;

    return (struct rect){ pointer_x + 22, pointer_y + 20, w, font_line_height(f) + 8 };
}

static struct rect cursor_rect(void)
{
    if (cursor_shape >= CURSOR_DND_NONE) {
        struct rect b = dnd_badge();

        return rect_union((struct rect){ pointer_x, pointer_y, 30, 32 }, b.w ? b : (struct rect){ pointer_x, pointer_y, 1, 1 });
    }
    switch (cursor_shape) {
    case WM_CURSOR_ARROW:
    case WM_CURSOR_HAND:
    case WM_CURSOR_WAIT:
        return (struct rect){ pointer_x, pointer_y, 13, 20 };
    default:
        return (struct rect){ pointer_x - 10, pointer_y - 10, 21, 21 };
    }
}

static void draw_cursor(struct gfx *g)
{
    int x = pointer_x, y = pointer_y;
    color_t ink = RGB(0x111111), paper = RGB(0xFFFFFF);

    switch (cursor_shape) {
    case WM_CURSOR_NONE:
        return;
    case WM_CURSOR_TEXT:
        for (int pass = 0; pass < 2; pass++) {
            float w = pass ? 1.2f : 3.2f;
            color_t c = pass ? ink : paper;

            gfx_line(g, x, y - 8, x, y + 8, w, c);
            gfx_line(g, x - 3, y - 9, x + 3, y - 9, w, c);
            gfx_line(g, x - 3, y + 9, x + 3, y + 9, w, c);
        }
        return;
    case WM_CURSOR_RESIZE_H:
    case WM_CURSOR_RESIZE_V:
        for (int pass = 0; pass < 2; pass++) {
            float w = pass ? 1.5f : 3.5f;
            color_t c = pass ? ink : paper;
            bool h = cursor_shape == WM_CURSOR_RESIZE_H;
            int dx = h ? 8 : 0, dy = h ? 0 : 8;

            gfx_line(g, x - dx, y - dy, x + dx, y + dy, w, c);
            gfx_line(g, x - dx, y - dy, x - dx + (h ? 4 : -4), y - dy + (h ? -4 : 4), w, c);
            gfx_line(g, x - dx, y - dy, x - dx + 4, y - dy + 4, w, c);
            gfx_line(g, x + dx, y + dy, x + dx - 4, y + dy - 4, w, c);
            gfx_line(g, x + dx, y + dy, x + dx - (h ? 4 : -4), y + dy + (h ? 4 : -4), w, c);
        }
        return;
    default:
        break;
    }
    for (int row = 0; row < (int)(sizeof(arrow) / sizeof(arrow[0])); row++) {
        for (int col = 0; arrow[row][col]; col++) {
            char p = arrow[row][col];

            if (p == 'X')
                gfx_fill(g, (struct rect){ x + col, y + row, 1, 1 }, ink);
            else if (p == '.')
                gfx_fill(g, (struct rect){ x + col, y + row, 1, 1 }, paper);
        }
    }
    if (cursor_shape == WM_CURSOR_WAIT)
        gfx_ring(g, x + 14, y + 16, 4, 2, RGB(0x3D8BFF));
    if (cursor_shape >= CURSOR_DND_NONE) {
        struct rect b = dnd_badge();
        float cx = x + 20, cy = y + 22;

        // A sign for what dropping here would do; moving needs none.
        if (cursor_shape == CURSOR_DND_NONE) {
            gfx_circle(g, cx, cy, 7, paper);
            gfx_ring(g, cx, cy, 6, 2, RGB(0xD93A3A));
            gfx_line(g, cx - 4, cy + 4, cx + 4, cy - 4, 2, RGB(0xD93A3A));
        } else if (cursor_shape == CURSOR_DND_COPY) {
            gfx_circle(g, cx, cy, 7, RGB(0x2E9E4F));
            gfx_line(g, cx - 4, cy, cx + 4, cy, 2, paper);
            gfx_line(g, cx, cy - 4, cx, cy + 4, 2, paper);
        } else if (cursor_shape == CURSOR_DND_LINK) {
            gfx_circle(g, cx, cy, 7, RGB(0x3D7BE0));
            gfx_line(g, cx - 3, cy + 3, cx + 3, cy - 3, 2, paper);
            gfx_line(g, cx, cy - 3, cx + 3, cy - 3, 2, paper);
            gfx_line(g, cx + 3, cy - 3, cx + 3, cy, 2, paper);
        }
        if (b.w) {
            struct font *f = font_get(FONT_SANS, 13);
            struct gfx clip = *g;
            struct rect t = { b.x + 8, b.y + 4, b.w - 16, b.h - 8 };

            gfx_fill_rounded(g, b, 6, ALPHA(0x1D2633, 0xE0));
            if (rect_intersect(g->clip, t, &clip.clip))
                text_draw(&clip, f, t.x, t.y, dnd.label, -1, RGB(0xFFFFFF));
        }
    }
}

// ---- Composition ----

static void present(struct rect r)
{
    struct aegis_fbinfo *fi = &screen.info;
    bool same = fi->bpp == 32 && fi->red_shift == 16 && fi->green_shift == 8 && fi->blue_shift == 0;

    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *src = screen.back->pixels + (size_t)y * screen.back->stride + r.x;
        uint8_t *dst = screen.fb + (size_t)y * fi->pitch + (size_t)r.x * (fi->bpp / 8);

        if (same) {
            memcpy(dst, src, r.w * 4);
            continue;
        }
        for (int x = 0; x < r.w; x++) {
            uint32_t p = src[x];
            uint32_t v = ((p >> 16 & 255) >> (8 - fi->red_bits)) << fi->red_shift
                       | ((p >> 8 & 255) >> (8 - fi->green_bits)) << fi->green_shift
                       | ((p & 255) >> (8 - fi->blue_bits)) << fi->blue_shift;

            memcpy(dst + x * (fi->bpp / 8), &v, fi->bpp / 8);
        }
    }
}

// Where a window dragged to an edge will go: a translucent panel behind it.
static void draw_snap_preview(struct gfx *g)
{
    struct rect r = { snap_preview.x + 8, snap_preview.y + 8, snap_preview.w - 16, snap_preview.h - 16 };

    gfx_fill_rounded(g, r, RADIUS, ALPHA(0x5B8DEF, 70));
    gfx_outline_rounded(g, r, RADIUS, 2, ALPHA(0x8FB3FF, 200));
}

void render(void)
{
    struct gfx g;
    struct rect cr = cursor_rect();

    // The text console is in front: draw nothing until it is switched back.
    if (screen_paused)
        return;
    if (!ndamaged && cr.x == cursor_drawn.x && cr.y == cursor_drawn.y && cr.w == cursor_drawn.w)
        return;
    damage(cursor_drawn);
    damage(cr);
    gfx_init(&g, screen.back);
    for (int i = 0; i < ndamaged; i++) {
        struct rect d = damaged[i];

        g.clip = d;
        gfx_gradient(&g, (struct rect){ 0, 0, screen.width, screen.height }, DESKTOP_TOP, DESKTOP_BOTTOM);
        for (struct window *w = windows; w; w = w->next) {
            struct rect b = window_bounds(w), tmp;

            if (w == snap_window && snap_preview.w)
                draw_snap_preview(&g);
            if (!w->visible || w->minimized || !rect_intersect(b, d, &tmp))
                continue;
            if (window_framed(w))
                draw_frame(&g, w);
            draw_content(&g, w);
        }
        draw_cursor(&g);
    }
    for (int i = 0; i < ndamaged; i++)
        present(damaged[i]);
    cursor_drawn = cr;
    ndamaged = 0;

    // Tell clients their updates are visible.
    for (struct window *w = windows; w; w = w->next) {
        if (w->frame_pending) {
            w->frame_pending = false;
            send_window(w, WM_FRAME, 0, 0, 0, 0, 0);
        }
    }
}

// Resizing grabs the right and bottom edges, a little outside the frame too.
int cursor_for_point(struct window *w, int x, int y)
{
    struct rect f;

    if (!w || !window_framed(w) || w->maximized || (w->flags & WM_FLAG_NO_RESIZE))
        return -1;
    f = w->frame;
    bool right = x >= f.x + f.w - RESIZE_EDGE && x < f.x + f.w + RESIZE_EDGE;
    bool bottom = y >= f.y + f.h - RESIZE_EDGE && y < f.y + f.h + RESIZE_EDGE;

    if (right && y < f.y + f.h + RESIZE_EDGE && !bottom)
        return WM_CURSOR_RESIZE_H;
    if (bottom && x < f.x + f.w + RESIZE_EDGE)
        return right ? WM_CURSOR_RESIZE_H : WM_CURSOR_RESIZE_V;
    return -1;
}

int screenshot(const char *path)
{
    return image_save_png(screen.back, path);
}
