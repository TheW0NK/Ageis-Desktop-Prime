#include "gfx.h"
#include <math.h>

struct surface *surface_create(int width, int height)
{
    struct surface *s;

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return NULL;
    if (!(s = malloc(sizeof(*s))))
        return NULL;
    if (!(s->pixels = calloc((size_t)width * height, 4))) {
        free(s);
        return NULL;
    }
    s->width = width;
    s->height = height;
    s->stride = width;
    s->owned = true;
    return s;
}

struct surface *surface_wrap(uint32_t *pixels, int width, int height, int stride)
{
    struct surface *s = malloc(sizeof(*s));

    if (s) {
        s->pixels = pixels;
        s->width = width;
        s->height = height;
        s->stride = stride;
        s->owned = false;
    }
    return s;
}

void surface_destroy(struct surface *s)
{
    if (!s)
        return;
    if (s->owned)
        free(s->pixels);
    free(s);
}

static uint32_t premultiply(color_t c)
{
    uint32_t a = c >> 24;

    if (a == 255)
        return c;
    return a << 24 | ((((c >> 16) & 255) * a + 127) / 255) << 16
         | ((((c >> 8) & 255) * a + 127) / 255) << 8 | (((c & 255) * a + 127) / 255);
}

void surface_clear(struct surface *s, color_t c)
{
    uint32_t p = premultiply(c);

    for (int y = 0; y < s->height; y++) {
        uint32_t *row = s->pixels + (size_t)y * s->stride;

        for (int x = 0; x < s->width; x++)
            row[x] = p;
    }
}

bool rect_empty(struct rect r)
{
    return r.w <= 0 || r.h <= 0;
}

bool rect_intersect(struct rect a, struct rect b, struct rect *out)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);

    *out = (struct rect){ x0, y0, x1 - x0, y1 - y0 };
    if (x1 <= x0 || y1 <= y0) {
        out->w = out->h = 0;
        return false;
    }
    return true;
}

bool rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);

    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

void gfx_init(struct gfx *g, struct surface *s)
{
    g->s = s;
    g->clip = (struct rect){ 0, 0, s->width, s->height };
    g->ox = g->oy = 0;
}

struct gfx gfx_push(struct gfx *g, struct rect r)
{
    struct gfx saved = *g;
    struct rect abs = { r.x + g->ox, r.y + g->oy, r.w, r.h };

    rect_intersect(g->clip, abs, &g->clip);
    g->ox = abs.x;
    g->oy = abs.y;
    return saved;
}

void gfx_restore(struct gfx *g, struct gfx saved)
{
    *g = saved;
}

// Blends premultiplied src over dst with extra coverage (0-255).
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t cov)
{
    uint32_t sa, inv, rb, ag;

    if (cov != 255) {
        rb = ((src & 0x00FF00FF) * cov) >> 8 & 0x00FF00FF;
        ag = ((src >> 8 & 0x00FF00FF) * cov) & 0xFF00FF00;
        src = rb | ag;
    }
    sa = src >> 24;
    if (sa == 255)
        return src;
    if (sa == 0)
        return dst;
    inv = 255 - sa;
    rb = ((dst & 0x00FF00FF) * inv) >> 8 & 0x00FF00FF;
    ag = ((dst >> 8 & 0x00FF00FF) * inv) & 0xFF00FF00;
    return src + (rb | ag);
}

static bool clipped(struct gfx *g, struct rect r, struct rect *out)
{
    struct rect abs = { r.x + g->ox, r.y + g->oy, r.w, r.h };

    return rect_intersect(g->clip, abs, out);
}

void gfx_fill(struct gfx *g, struct rect r, color_t c)
{
    struct rect a;
    uint32_t p = premultiply(c);

    if (!(c >> 24) || !clipped(g, r, &a))
        return;
    for (int y = a.y; y < a.y + a.h; y++) {
        uint32_t *row = g->s->pixels + (size_t)y * g->s->stride + a.x;

        if ((p >> 24) == 255) {
            for (int x = 0; x < a.w; x++)
                row[x] = p;
        } else {
            for (int x = 0; x < a.w; x++)
                row[x] = blend(row[x], p, 255);
        }
    }
}

void gfx_outline(struct gfx *g, struct rect r, int w, color_t c)
{
    gfx_fill(g, (struct rect){ r.x, r.y, r.w, w }, c);
    gfx_fill(g, (struct rect){ r.x, r.y + r.h - w, r.w, w }, c);
    gfx_fill(g, (struct rect){ r.x, r.y + w, w, r.h - 2 * w }, c);
    gfx_fill(g, (struct rect){ r.x + r.w - w, r.y + w, w, r.h - 2 * w }, c);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// Signed distance from (px, py) to a rounded rectangle centred at (cx, cy)
// with half sizes hw, hh and corner radius rad.
static float sd_round_rect(float px, float py, float cx, float cy, float hw, float hh, float rad)
{
    float qx = fabsf(px - cx) - (hw - rad), qy = fabsf(py - cy) - (hh - rad);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;

    return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - rad;
}

// Fills the pixels of `bounds` with coverage from a distance function.
#define FILL_SDF(g, bounds, c, expr)                                            \
    do {                                                                        \
        struct rect __a;                                                        \
        uint32_t __p = premultiply(c);                                          \
        if (!((c) >> 24) || !clipped(g, bounds, &__a))                          \
            break;                                                              \
        for (int __y = __a.y; __y < __a.y + __a.h; __y++) {                     \
            uint32_t *__row = (g)->s->pixels + (size_t)__y * (g)->s->stride;    \
            float py = __y - (g)->oy + 0.5f;                                    \
            for (int __x = __a.x; __x < __a.x + __a.w; __x++) {                 \
                float px = __x - (g)->ox + 0.5f;                                \
                float __cov = clampf(0.5f - (expr), 0, 1);                      \
                if (__cov > 0)                                                  \
                    __row[__x] = blend(__row[__x], __p, (uint32_t)(__cov * 255 + 0.5f)); \
            }                                                                   \
        }                                                                       \
    } while (0)

void gfx_fill_rounded(struct gfx *g, struct rect r, int radius, color_t c)
{
    float hw = r.w / 2.0f, hh = r.h / 2.0f, cx = r.x + hw, cy = r.y + hh;
    float rad = MIN((float)radius, MIN(hw, hh));

    if (radius <= 0) {
        gfx_fill(g, r, c);
        return;
    }
    // The middle band has full coverage; only the corners need the SDF.
    gfx_fill(g, (struct rect){ r.x, r.y + (int)rad, r.w, r.h - 2 * (int)rad }, c);
    FILL_SDF(g, ((struct rect){ r.x, r.y, r.w, (int)rad }), c, sd_round_rect(px, py, cx, cy, hw, hh, rad));
    FILL_SDF(g, ((struct rect){ r.x, r.y + r.h - (int)rad, r.w, (int)rad }), c,
             sd_round_rect(px, py, cx, cy, hw, hh, rad));
}

void gfx_outline_rounded(struct gfx *g, struct rect r, int radius, int width, color_t c)
{
    float hw = r.w / 2.0f, hh = r.h / 2.0f, cx = r.x + hw, cy = r.y + hh;
    float rad = MIN((float)radius, MIN(hw, hh)), half = width / 2.0f;

    FILL_SDF(g, r, c, fabsf(sd_round_rect(px, py, cx, cy, hw - half, hh - half, MAX(rad - half, 0.0f))) - half);
}

void gfx_gradient(struct gfx *g, struct rect r, color_t top, color_t bottom)
{
    for (int i = 0; i < r.h; i++) {
        float t = r.h > 1 ? (float)i / (r.h - 1) : 0;
        uint32_t c = 0;

        for (int sh = 0; sh < 32; sh += 8) {
            float a = (top >> sh) & 255, b = (bottom >> sh) & 255;
            c |= (uint32_t)(a + (b - a) * t + 0.5f) << sh;
        }
        gfx_fill(g, (struct rect){ r.x, r.y + i, r.w, 1 }, c);
    }
}

static float sd_segment(float px, float py, float ax, float ay, float bx, float by)
{
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float len2 = bax * bax + bay * bay;
    float h = len2 > 0 ? clampf((pax * bax + pay * bay) / len2, 0, 1) : 0;
    float dx = pax - bax * h, dy = pay - bay * h;

    return sqrtf(dx * dx + dy * dy);
}

void gfx_line(struct gfx *g, float x0, float y0, float x1, float y1, float width, color_t c)
{
    float hw = width / 2;
    struct rect b = { (int)floorf(fminf(x0, x1) - hw - 1), (int)floorf(fminf(y0, y1) - hw - 1), 0, 0 };

    b.w = (int)ceilf(fmaxf(x0, x1) + hw + 1) - b.x;
    b.h = (int)ceilf(fmaxf(y0, y1) + hw + 1) - b.y;
    FILL_SDF(g, b, c, sd_segment(px, py, x0, y0, x1, y1) - hw);
}

void gfx_circle(struct gfx *g, float cx, float cy, float radius, color_t c)
{
    struct rect b = { (int)floorf(cx - radius - 1), (int)floorf(cy - radius - 1),
                      (int)ceilf(2 * radius + 3), (int)ceilf(2 * radius + 3) };

    FILL_SDF(g, b, c, sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - radius);
}

void gfx_ring(struct gfx *g, float cx, float cy, float radius, float width, color_t c)
{
    struct rect b = { (int)floorf(cx - radius - width), (int)floorf(cy - radius - width),
                      (int)ceilf(2 * (radius + width) + 2), (int)ceilf(2 * (radius + width) + 2) };

    FILL_SDF(g, b, c, fabsf(sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - radius) - width / 2);
}

void gfx_shadow(struct gfx *g, struct rect r, int radius, int size, color_t c)
{
    struct rect b = { r.x - size, r.y - size, r.w + 2 * size, r.h + 2 * size }, a;
    float hw = r.w / 2.0f, hh = r.h / 2.0f, cx = r.x + hw, cy = r.y + hh, base = (c >> 24) / 255.0f;
    float rad = MIN((float)radius, MIN(hw, hh));

    if (!clipped(g, b, &a) || size <= 0)
        return;
    for (int y = a.y; y < a.y + a.h; y++) {
        uint32_t *row = g->s->pixels + (size_t)y * g->s->stride;
        float py = y - g->oy + 0.5f;

        for (int x = a.x; x < a.x + a.w; x++) {
            float px = x - g->ox + 0.5f;
            float d = sd_round_rect(px, py, cx, cy, hw, hh, rad);
            float t, alpha;

            if (d < 0)
                continue;           // under the window itself
            t = 1 - clampf(d / size, 0, 1);
            alpha = base * t * t;
            if (alpha > 0.004f)
                row[x] = blend(row[x], premultiply(ALPHA(c, (uint32_t)(alpha * 255))), 255);
        }
    }
}

void gfx_blit(struct gfx *g, const struct surface *src, struct rect from, int x, int y)
{
    struct rect a, s;

    rect_intersect(from, (struct rect){ 0, 0, src->width, src->height }, &s);
    if (!clipped(g, (struct rect){ x + s.x - from.x, y + s.y - from.y, s.w, s.h }, &a))
        return;
    int sx = s.x + a.x - (x + s.x - from.x + g->ox), sy = s.y + a.y - (y + s.y - from.y + g->oy);

    for (int row = 0; row < a.h; row++) {
        uint32_t *d = g->s->pixels + (size_t)(a.y + row) * g->s->stride + a.x;
        const uint32_t *p = src->pixels + (size_t)(sy + row) * src->stride + sx;

        for (int i = 0; i < a.w; i++)
            d[i] = blend(d[i], p[i], 255);
    }
}

void gfx_blit_scaled(struct gfx *g, const struct surface *src, struct rect from, struct rect dst)
{
    struct rect a;

    if (rect_empty(from) || rect_empty(dst) || !clipped(g, dst, &a))
        return;
    if (from.w == dst.w && from.h == dst.h) {
        gfx_blit(g, src, from, dst.x, dst.y);
        return;
    }
    float sx = (float)from.w / dst.w, sy = (float)from.h / dst.h;

    for (int y = a.y; y < a.y + a.h; y++) {
        uint32_t *d = g->s->pixels + (size_t)y * g->s->stride;
        float fy = from.y + (y - g->oy - dst.y + 0.5f) * sy - 0.5f;
        int y0 = (int)floorf(fy), y1;
        float ty = fy - y0;

        y0 = MAX(from.y, MIN(y0, from.y + from.h - 1));
        y1 = MIN(y0 + 1, from.y + from.h - 1);
        for (int x = a.x; x < a.x + a.w; x++) {
            float fx = from.x + (x - g->ox - dst.x + 0.5f) * sx - 0.5f;
            int x0 = (int)floorf(fx), x1;
            float tx = fx - x0;
            uint32_t out = 0;

            x0 = MAX(from.x, MIN(x0, from.x + from.w - 1));
            x1 = MIN(x0 + 1, from.x + from.w - 1);
            uint32_t p00 = src->pixels[(size_t)y0 * src->stride + x0], p01 = src->pixels[(size_t)y0 * src->stride + x1];
            uint32_t p10 = src->pixels[(size_t)y1 * src->stride + x0], p11 = src->pixels[(size_t)y1 * src->stride + x1];

            for (int sh = 0; sh < 32; sh += 8) {
                float top = ((p00 >> sh) & 255) * (1 - tx) + ((p01 >> sh) & 255) * tx;
                float bot = ((p10 >> sh) & 255) * (1 - tx) + ((p11 >> sh) & 255) * tx;
                out |= (uint32_t)(top * (1 - ty) + bot * ty + 0.5f) << sh;
            }
            d[x] = blend(d[x], out, 255);
        }
    }
}

void gfx_mask(struct gfx *g, const uint8_t *mask, int mask_stride, struct rect r, color_t c)
{
    struct rect a;
    uint32_t p = premultiply(c);

    if (!clipped(g, r, &a))
        return;
    int mx = a.x - (r.x + g->ox), my = a.y - (r.y + g->oy);

    for (int y = 0; y < a.h; y++) {
        uint32_t *d = g->s->pixels + (size_t)(a.y + y) * g->s->stride + a.x;
        const uint8_t *m = mask + (size_t)(my + y) * mask_stride + mx;

        for (int x = 0; x < a.w; x++) {
            if (m[x])
                d[x] = blend(d[x], p, m[x]);
        }
    }
}
