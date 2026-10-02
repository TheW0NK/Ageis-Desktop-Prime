#include "gfx.h"
#include <math.h>

#define STBTT_ifloor(x)     ((int)floor(x))
#define STBTT_iceil(x)      ((int)ceil(x))
#define STBTT_sqrt(x)       sqrt(x)
#define STBTT_pow(x, y)     pow(x, y)
#define STBTT_fmod(x, y)    fmod(x, y)
#define STBTT_cos(x)        cos(x)
#define STBTT_acos(x)       acos(x)
#define STBTT_fabs(x)       fabs(x)
#define STBTT_malloc(x, u)  ((void)(u), malloc(x))
#define STBTT_free(x, u)    ((void)(u), free(x))
#define STBTT_assert(x)     ((void)0)
#define STBTT_strlen(x)     strlen(x)
#define STBTT_memcpy        memcpy
#define STBTT_memset        memset
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// Fonts live in /usr/share/fonts. A family file is loaded once; each pixel
// size gets its own glyph cache. Code points missing from a font are looked
// up in the fallback files (for example a CJK font, when installed).

#define FONT_DIR        "/usr/share/fonts/"
#define CACHE_BUCKETS   256

struct family {
    char name[32];
    unsigned char *data;
    stbtt_fontinfo info;
    struct family *next;
};

struct glyph {
    uint32_t cp;
    struct family *fam;
    int index;
    int w, h, xoff, yoff, advance;
    uint8_t *bitmap;
    struct glyph *next;
};

struct font {
    struct family *fam;
    char name[40];
    bool oblique;                   // slanted by shearing the glyphs ("-italic")
    int px;
    float scale;
    int ascent, descent, line_gap;
    struct glyph *cache[CACHE_BUCKETS];
    struct font *next;
};

static const struct {
    const char *name, *file;
} files[] = {
    { FONT_SANS, "DejaVuSans.ttf" },
    { FONT_SANS_BOLD, "DejaVuSans-Bold.ttf" },
    { FONT_MONO, "DejaVuSansMono.ttf" },
    { FONT_MONO_BOLD, "DejaVuSansMono-Bold.ttf" },
    { FONT_SERIF, "DejaVuSerif.ttf" },
    { FONT_SERIF_BOLD, "DejaVuSerif-Bold.ttf" },
    { "fallback-cjk", "NotoSansCJK-Regular.ttc" },
};

static struct family *families;
static struct font *fonts;
static mutex_t lock;

static unsigned char *read_file(const char *path)
{
    struct aegis_stat st;
    unsigned char *data;
    int fd = open(path, O_RDONLY);
    size_t got = 0;

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) < 0 || !(data = malloc(st.size + 1))) {
        close(fd);
        return NULL;
    }
    while (got < st.size) {
        ssize_t n = read(fd, data + got, st.size - got);

        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    if (got != st.size) {
        free(data);
        return NULL;
    }
    return data;
}

static struct family *family(const char *name)
{
    char path[256];
    struct family *f;

    for (f = families; f; f = f->next) {
        if (!strcmp(f->name, name))
            return f->data ? f : NULL;
    }
    if (!(f = calloc(1, sizeof(*f))))
        return NULL;
    strlcpy(f->name, name, sizeof(f->name));
    f->next = families;
    families = f;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (strcmp(files[i].name, name))
            continue;
        snprintf(path, sizeof(path), FONT_DIR "%s", files[i].file);
        if ((f->data = read_file(path))
            && !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
            free(f->data);
            f->data = NULL;
        }
    }
    return f->data ? f : NULL;
}

struct font *font_get(const char *name, int px)
{
    struct family *fam;
    struct font *f;
    char base[40];
    size_t len = strlen(name);
    bool oblique = false;

    if (px < 4)
        px = 4;
    mutex_lock(&lock);
    for (f = fonts; f; f = f->next) {
        if (f->px == px && !strcmp(f->name, name)) {
            mutex_unlock(&lock);
            return f;
        }
    }
    // "sans-italic", "serif-bold-italic": the upright face, slanted.
    strlcpy(base, name, sizeof(base));
    if (len > 7 && len < sizeof(base) && !strcmp(name + len - 7, "-italic")) {
        base[len - 7] = 0;
        oblique = true;
    }
    if (!(fam = family(base)) && !(fam = family(FONT_SANS))) {
        mutex_unlock(&lock);
        return NULL;
    }
    if ((f = calloc(1, sizeof(*f)))) {
        f->fam = fam;
        strlcpy(f->name, name, sizeof(f->name));
        f->oblique = oblique;
        f->px = px;
        f->scale = stbtt_ScaleForPixelHeight(&fam->info, px);
        stbtt_GetFontVMetrics(&fam->info, &f->ascent, &f->descent, &f->line_gap);
        f->ascent = (int)ceilf(f->ascent * f->scale);
        f->descent = (int)floorf(f->descent * f->scale);
        f->line_gap = (int)(f->line_gap * f->scale);
        f->next = fonts;
        fonts = f;
    }
    mutex_unlock(&lock);
    return f;
}

int font_ascent(struct font *f)
{
    return f ? f->ascent : 0;
}

int font_line_height(struct font *f)
{
    return f ? f->ascent - f->descent + f->line_gap : 16;
}

// Slants a glyph for a synthetic italic: each row moves right by its height
// above the baseline times SLANT, blending the fractional part.
#define SLANT 0.21f

static void shear(struct glyph *g)
{
    float lo = -(g->yoff + g->h - 1) * SLANT, hi = -g->yoff * SLANT;
    int left = (int)floorf(lo), w = g->w + (int)ceilf(hi) - left + 1;
    uint8_t *out = calloc((size_t)w * g->h, 1);

    if (!out)
        return;
    for (int y = 0; y < g->h; y++) {
        float shift = -(g->yoff + y) * SLANT - left;
        int whole = (int)floorf(shift);
        int frac = (int)((shift - whole) * 256);

        for (int x = 0; x < g->w; x++) {
            int v = g->bitmap[y * g->w + x], o = y * w + x + whole;

            out[o] = MIN(255, out[o] + (v * (256 - frac) >> 8));
            if (x + whole + 1 < w)
                out[o + 1] = MIN(255, out[o + 1] + (v * frac >> 8));
        }
    }
    free(g->bitmap);
    g->bitmap = out;
    g->xoff += left;
    g->w = w;
}

static struct glyph *glyph(struct font *f, uint32_t cp)
{
    unsigned bucket = cp % CACHE_BUCKETS;
    struct family *fam = f->fam;
    struct glyph *g;
    float scale = f->scale;
    int index, x0, y0, x1, y1, adv, lsb;

    for (g = f->cache[bucket]; g; g = g->next) {
        if (g->cp == cp)
            return g;
    }
    index = stbtt_FindGlyphIndex(&fam->info, cp);
    if (!index && cp > 0x7F) {
        struct family *fb = family("fallback-cjk");

        if (fb && (index = stbtt_FindGlyphIndex(&fb->info, cp))) {
            fam = fb;
            scale = stbtt_ScaleForPixelHeight(&fb->info, f->px);
        }
    }
    if (!(g = calloc(1, sizeof(*g))))
        return NULL;
    g->cp = cp;
    g->fam = fam;
    g->index = index;
    stbtt_GetGlyphHMetrics(&fam->info, index, &adv, &lsb);
    g->advance = (int)lroundf(adv * scale);
    stbtt_GetGlyphBitmapBox(&fam->info, index, scale, scale, &x0, &y0, &x1, &y1);
    g->w = x1 - x0;
    g->h = y1 - y0;
    g->xoff = x0;
    g->yoff = y0;
    if (g->w > 0 && g->h > 0 && (g->bitmap = malloc((size_t)g->w * g->h))) {
        stbtt_MakeGlyphBitmap(&fam->info, g->bitmap, g->w, g->h, g->w, scale, scale, index);
        if (f->oblique)
            shear(g);
    }
    g->next = f->cache[bucket];
    f->cache[bucket] = g;
    return g;
}

static int kern(struct font *f, struct glyph *a, struct glyph *b)
{
    if (!a || !b || a->fam != b->fam || a->fam != f->fam)
        return 0;
    return (int)lroundf(stbtt_GetGlyphKernAdvance(&f->fam->info, a->index, b->index) * f->scale);
}

int text_width(struct font *f, const char *s, int len)
{
    const char *end = s + (len < 0 ? (int)strlen(s) : len);
    struct glyph *prev = NULL;
    int w = 0;

    if (!f)
        return 0;
    mutex_lock(&lock);
    while (s < end && *s) {
        struct glyph *g = glyph(f, utf8_decode(&s));

        if (!g)
            continue;
        w += kern(f, prev, g) + g->advance;
        prev = g;
    }
    mutex_unlock(&lock);
    return w;
}

int text_draw(struct gfx *gc, struct font *f, int x, int y, const char *s, int len, color_t c)
{
    const char *end = s + (len < 0 ? (int)strlen(s) : len);
    struct glyph *prev = NULL;

    if (!f)
        return x;
    mutex_lock(&lock);
    while (s < end && *s) {
        uint32_t cp = utf8_decode(&s);
        struct glyph *g;

        if (cp == '\t')
            cp = ' ';
        if (!(g = glyph(f, cp)))
            continue;
        x += kern(f, prev, g);
        if (g->bitmap)
            gfx_mask(gc, g->bitmap, g->w, (struct rect){ x + g->xoff, y + f->ascent + g->yoff, g->w, g->h }, c);
        x += g->advance;
        prev = g;
    }
    mutex_unlock(&lock);
    return x;
}

int text_hit(struct font *f, const char *s, int len, int x)
{
    int pos = 0, cur = 0;

    if (len < 0)
        len = strlen(s);
    while (pos < len) {
        int next = utf8_next(s, pos), w = text_width(f, s + pos, next - pos);

        if (x < cur + w / 2)
            return pos;
        cur += w;
        pos = next;
    }
    return len;
}
