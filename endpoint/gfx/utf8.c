#include "gfx.h"

uint32_t utf8_decode(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int extra;

    if (p[0] < 0x80) {
        *s += 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07;
        extra = 3;
    } else {
        *s += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += i;
            return 0xFFFD;
        }
        cp = cp << 6 | (p[i] & 0x3F);
    }
    *s += extra + 1;
    return cp;
}

int utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = 0xC0 | cp >> 6;
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = 0xE0 | cp >> 12;
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    out[0] = 0xF0 | cp >> 18;
    out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F);
    out[3] = 0x80 | (cp & 0x3F);
    return 4;
}

int utf8_prev(const char *s, int pos)
{
    if (pos <= 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

int utf8_next(const char *s, int pos)
{
    if (!s[pos])
        return pos;
    pos++;
    while (s[pos] && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}
