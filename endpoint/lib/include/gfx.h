#ifndef AEGIS_GFX_H
#define AEGIS_GFX_H

#include "aegis.h"

// 2D graphics. Surfaces hold 32-bit pixels, 0xAARRGGBB with premultiplied
// alpha. Colors passed to drawing functions are straight (not premultiplied)
// 0xAARRGGBB; an alpha of 0 in a color constant written as 0xRRGGBB is
// taken as opaque by RGB().

typedef uint32_t color_t;

#define ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGB(rgb)        (0xFF000000U | (uint32_t)(rgb))
#define ALPHA(c, a)     (((uint32_t)(c) & 0x00FFFFFFU) | ((uint32_t)(a) << 24))

struct rect {
    int x, y, w, h;
};

struct surface {
    uint32_t *pixels;
    int width, height;
    int stride;                     // in pixels
    bool owned;
};

struct surface *surface_create(int width, int height);
struct surface *surface_wrap(uint32_t *pixels, int width, int height, int stride);
void surface_destroy(struct surface *s);
void surface_clear(struct surface *s, color_t c);

bool rect_intersect(struct rect a, struct rect b, struct rect *out);
bool rect_contains(struct rect r, int x, int y);
struct rect rect_union(struct rect a, struct rect b);
bool rect_empty(struct rect r);

// A painter: draws into a surface through a clip rectangle, with an origin.
struct gfx {
    struct surface *s;
    struct rect clip;               // in surface coordinates
    int ox, oy;                     // added to every coordinate
};

void gfx_init(struct gfx *g, struct surface *s);
// Narrows the clip to r (in current coordinates) and moves the origin to it.
// Returns the previous state for gfx_restore.
struct gfx gfx_push(struct gfx *g, struct rect r);
void gfx_restore(struct gfx *g, struct gfx saved);

void gfx_fill(struct gfx *g, struct rect r, color_t c);
void gfx_outline(struct gfx *g, struct rect r, int width, color_t c);
void gfx_fill_rounded(struct gfx *g, struct rect r, int radius, color_t c);
void gfx_outline_rounded(struct gfx *g, struct rect r, int radius, int width, color_t c);
void gfx_gradient(struct gfx *g, struct rect r, color_t top, color_t bottom);
void gfx_line(struct gfx *g, float x0, float y0, float x1, float y1, float width, color_t c);
void gfx_circle(struct gfx *g, float cx, float cy, float radius, color_t c);
void gfx_ring(struct gfx *g, float cx, float cy, float radius, float width, color_t c);
// A soft shadow around r, `size` pixels wide.
void gfx_shadow(struct gfx *g, struct rect r, int radius, int size, color_t c);
// Copies src (blending by its alpha) with its top-left at x, y.
void gfx_blit(struct gfx *g, const struct surface *src, struct rect from, int x, int y);
// Scales src into dst (bilinear).
void gfx_blit_scaled(struct gfx *g, const struct surface *src, struct rect from, struct rect dst);
// Fills r with a single-channel coverage mask in color c.
void gfx_mask(struct gfx *g, const uint8_t *mask, int mask_stride, struct rect r, color_t c);

// Images (PNG, JPEG, BMP, GIF, TGA).
struct surface *image_load(const char *path);
struct surface *image_decode(const void *data, size_t len);
int image_save_png(const struct surface *s, const char *path);

// Text. Fonts are cached by name and pixel size.
struct font;

#define FONT_SANS       "sans"
#define FONT_SANS_BOLD  "sans-bold"
#define FONT_MONO       "mono"
#define FONT_MONO_BOLD  "mono-bold"
#define FONT_SERIF      "serif"

struct font *font_get(const char *name, int px);
int font_ascent(struct font *f);
int font_line_height(struct font *f);
// Width of the first `len` bytes of a UTF-8 string (len < 0: all of it).
int text_width(struct font *f, const char *s, int len);
// Draws UTF-8 text with its top at y. Returns the x after the last glyph.
int text_draw(struct gfx *g, struct font *f, int x, int y, const char *s, int len, color_t c);
// The byte offset in s nearest to pixel x.
int text_hit(struct font *f, const char *s, int len, int x);

// UTF-8.
uint32_t utf8_decode(const char **s);
int utf8_encode(uint32_t cp, char out[4]);
int utf8_prev(const char *s, int pos);
int utf8_next(const char *s, int pos);

#endif
