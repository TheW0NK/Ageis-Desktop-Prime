#include "aegis.h"

// Base64 (RFC 4648), for mail and stored secrets.

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const void *in, size_t len, char *out)
{
    const unsigned char *p = in;
    size_t o = 0;

    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < len ? (uint32_t)p[i + 1] << 8 : 0)
                     | (i + 2 < len ? p[i + 2] : 0);

        out[o++] = alphabet[v >> 18 & 63];
        out[o++] = alphabet[v >> 12 & 63];
        out[o++] = i + 1 < len ? alphabet[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? alphabet[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

ssize_t base64_decode(const char *in, size_t len, void *out)
{
    unsigned char *o = out;
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;

    for (size_t i = 0; i < len; i++) {
        int v = value(in[i]);

        if (in[i] == '=')
            break;
        if (v < 0)
            continue;           // line breaks and spaces
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o[n++] = acc >> bits & 0xFF;
        }
    }
    return n;
}
