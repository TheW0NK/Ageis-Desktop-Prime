#include "kernel.h"
#include "string.h"

struct out {
    char *buf;
    size_t size;
    size_t len;
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_padded(struct out *o, const char *s, size_t n, int width, bool left, char pad)
{
    int fill = width > (int)n ? width - (int)n : 0;

    if (!left)
        while (fill-- > 0)
            put(o, pad);
    while (n--)
        put(o, *s++);
    if (left)
        while (fill-- > 0)
            put(o, ' ');
}

static void put_number(struct out *o, uint64_t v, bool negative, unsigned base,
                       bool upper, int width, bool left, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;

    do {
        tmp[sizeof(tmp) - 1 - n++] = digits[v % base];
        v /= base;
    } while (v);

    if (negative) {
        if (pad == '0') {
            put(o, '-');
            width--;
        } else {
            tmp[sizeof(tmp) - 1 - n++] = '-';
        }
    }
    put_padded(o, tmp + sizeof(tmp) - n, n, width, left, pad);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }

        bool left = false;
        char pad = ' ';
        int width = 0, length = 0, precision = -1;

        for (fmt++;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                pad = '0';
            else
                break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            precision = 0;
            for (fmt++; *fmt >= '0' && *fmt <= '9'; fmt++)
                precision = precision * 10 + (*fmt - '0');
        }
        while (*fmt == 'l' || *fmt == 'z') {
            length++;
            fmt++;
        }
        if (left)
            pad = ' ';

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = length ? va_arg(args, int64_t) : va_arg(args, int);
            put_number(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, v < 0, 10, false, width, left, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = length ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
            put_number(&o, v, false, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, left, pad);
            break;
        }
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_number(&o, (uint64_t)va_arg(args, void *), false, 16, false, 16, false, '0');
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s)
                s = "(null)";
            put_padded(&o, s, precision >= 0 ? strnlen(s, precision) : strlen(s), width, left, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            put_padded(&o, &c, 1, width, left, ' ');
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }

    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int n;

    va_start(args, fmt);
    n = kvsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}
