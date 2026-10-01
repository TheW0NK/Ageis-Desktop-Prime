#include "gfx.h"
#include <math.h>

#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x)  ((void)0)
#define STBI_MALLOC     malloc
#define STBI_REALLOC    realloc
#define STBI_FREE       free
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

struct surface *image_decode(const void *data, size_t len)
{
    int w, h, n;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    struct surface *s;

    if (!rgba)
        return NULL;
    if ((s = surface_create(w, h))) {
        for (int i = 0; i < w * h; i++) {
            uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];

            if (a != 255) {
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            }
            s->pixels[i] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    stbi_image_free(rgba);
    return s;
}

struct surface *image_load(const char *path)
{
    struct aegis_stat st;
    struct surface *s = NULL;
    unsigned char *data;
    int fd = open(path, O_RDONLY);
    size_t got = 0;

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) == 0 && st.size && (data = malloc(st.size))) {
        ssize_t n;

        while (got < st.size && (n = read(fd, data + got, st.size - got)) > 0)
            got += n;
        if (got == st.size)
            s = image_decode(data, got);
        free(data);
    }
    close(fd);
    return s;
}

static void write_fd(void *ctx, void *data, int size)
{
    write(*(int *)ctx, data, size);
}

int image_save_png(const struct surface *s, const char *path)
{
    unsigned char *rgba = malloc((size_t)s->width * s->height * 4);
    int fd, ok;

    if (!rgba)
        return -1;
    for (int y = 0; y < s->height; y++) {
        for (int x = 0; x < s->width; x++) {
            uint32_t p = s->pixels[(size_t)y * s->stride + x], a = p >> 24;
            unsigned char *o = rgba + ((size_t)y * s->width + x) * 4;

            // Undo premultiplication.
            o[0] = a ? MIN(255U, ((p >> 16) & 255) * 255 / a) : 0;
            o[1] = a ? MIN(255U, ((p >> 8) & 255) * 255 / a) : 0;
            o[2] = a ? MIN(255U, (p & 255) * 255 / a) : 0;
            o[3] = a;
        }
    }
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        free(rgba);
        return -1;
    }
    ok = stbi_write_png_to_func(write_fd, &fd, s->width, s->height, 4, rgba, s->width * 4);
    close(fd);
    free(rgba);
    return ok ? 0 : -1;
}
