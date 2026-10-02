#include "gfx.h"
#include <math.h>

// Desktop backgrounds: "default" (drawn), "color:#RRGGBB", or an image path
// (scaled to cover the screen).

static void glow(struct gfx *g, float cx, float cy, float r, color_t c)
{
    // Soft radial light built from translucent circles.
    for (int i = 12; i >= 1; i--)
        gfx_circle(g, cx, cy, r * i / 12, ALPHA(c, 6));
}

static void draw_default(struct gfx *g, struct rect r)
{
    float w = r.w, h = r.h;

    gfx_gradient(g, r, RGB(0x16264A), RGB(0x0A1022));
    glow(g, r.x + w * 0.78f, r.y + h * 0.22f, h * 0.65f, 0x3D7BFF);
    glow(g, r.x + w * 0.15f, r.y + h * 0.95f, h * 0.55f, 0x7A4DFF);
    glow(g, r.x + w * 0.45f, r.y + h * 0.55f, h * 0.30f, 0x29B6F6);
    // A few long, faint arcs.
    for (int k = 0; k < 3; k++) {
        float cx = r.x + w * (0.9f - k * 0.08f), cy = r.y + h * (1.6f + k * 0.12f), rad = h * (1.25f + k * 0.18f);
        float px = 0, py = 0;

        for (int i = 0; i <= 64; i++) {
            float a = 3.14159f * (1.05f + 0.55f * i / 64.0f), x = cx + rad * cosf(a), y = cy + rad * sinf(a);

            if (i)
                gfx_line(g, px, py, x, y, 1.5f, ALPHA(0xBFD4FF, 26 - k * 6));
            px = x;
            py = y;
        }
    }
}

static int hexval(const char *s)
{
    return (int)strtoul(s, NULL, 16);
}

void wallpaper_draw(struct gfx *g, struct rect r, const char *spec)
{
    struct surface *img;

    if (!spec || !*spec || !strcmp(spec, "default")) {
        draw_default(g, r);
        return;
    }
    if (!strncmp(spec, "color:", 6)) {
        const char *c = spec + 6;

        gfx_fill(g, r, RGB(hexval(*c == '#' ? c + 1 : c)));
        return;
    }
    if (!(img = image_load(spec))) {
        draw_default(g, r);
        return;
    }
    {
        // Cover: scale so the image fills r, centred, cropping the rest.
        float sx = (float)r.w / img->width, sy = (float)r.h / img->height, s = fmaxf(sx, sy);
        int dw = (int)(img->width * s + 0.5f), dh = (int)(img->height * s + 0.5f);

        gfx_blit_scaled(g, img, (struct rect){ 0, 0, img->width, img->height },
                        (struct rect){ r.x + (r.w - dw) / 2, r.y + (r.h - dh) / 2, dw, dh });
    }
    surface_destroy(img);
}

// A round user picture: the image at picture_path, or the first letter of
// the name on a colour chosen from the name.
void avatar_draw(struct gfx *g, struct rect r, const char *name, const char *picture_path)
{
    static const color_t colors[] = { 0x3D7BFF, 0x7A4DFF, 0x13A89E, 0xE0582F, 0xD12C7D, 0x2F9E44, 0xC28A00 };
    int size = MIN(r.w, r.h);
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    struct surface *img = picture_path && *picture_path ? image_load(picture_path) : NULL;

    if (img) {
        struct surface *tmp = surface_create(size, size);

        if (tmp) {
            struct gfx tg;
            float s = fmaxf((float)size / img->width, (float)size / img->height);
            int dw = (int)(img->width * s), dh = (int)(img->height * s);

            gfx_init(&tg, tmp);
            gfx_blit_scaled(&tg, img, (struct rect){ 0, 0, img->width, img->height },
                            (struct rect){ (size - dw) / 2, (size - dh) / 2, dw, dh });
            // Keep only the circle.
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++) {
                    float dx = x + 0.5f - size / 2.0f, dy = y + 0.5f - size / 2.0f;
                    float d = sqrtf(dx * dx + dy * dy) - size / 2.0f;
                    float a = d <= -0.5f ? 1 : d >= 0.5f ? 0 : 0.5f - d;

                    if (a < 1) {
                        uint32_t px = tmp->pixels[y * tmp->stride + x], out = 0;

                        for (int sh = 0; sh < 32; sh += 8)
                            out |= (uint32_t)(((px >> sh) & 255) * a + 0.5f) << sh;
                        tmp->pixels[y * tmp->stride + x] = out;
                    }
                }
            gfx_blit(g, tmp, (struct rect){ 0, 0, size, size }, (int)(cx - size / 2.0f), (int)(cy - size / 2.0f));
            surface_destroy(tmp);
        }
        surface_destroy(img);
        return;
    }
    {
        unsigned hash = 0;
        char initial[8] = "?";
        struct font *f = font_get(FONT_SANS_BOLD, size * 4 / 10);

        for (const char *p = name ? name : ""; *p; p++)
            hash = hash * 31 + (unsigned char)*p;
        gfx_circle(g, cx, cy, size / 2.0f, RGB(colors[hash % (sizeof(colors) / sizeof(colors[0]))]));
        if (name && *name) {
            const char *p = name;
            uint32_t cp = utf8_decode(&p);

            if (cp < 0x80)
                cp = toupper(cp);
            initial[utf8_encode(cp, initial)] = 0;
        }
        if (f)
            text_draw(g, f, (int)(cx - text_width(f, initial, -1) / 2.0f),
                      (int)(cy - font_line_height(f) / 2.0f), initial, -1, RGB(0xFFFFFF));
    }
}
