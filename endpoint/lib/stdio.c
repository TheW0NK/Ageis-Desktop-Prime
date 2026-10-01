#include "aegis.h"

struct out {
    char *buf;
    size_t size, len;
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

static void put_number(struct out *o, uint64_t v, bool neg, unsigned base, bool upper,
                       int width, bool left, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;

    do {
        tmp[sizeof(tmp) - 1 - n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg) {
        if (pad == '0') {
            put(o, '-');
            width--;
        } else {
            tmp[sizeof(tmp) - 1 - n++] = '-';
        }
    }
    put_padded(o, tmp + sizeof(tmp) - n, n, width, left, pad);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        bool left = false;
        char pad = ' ';
        int width = 0, length = 0, precision = -1;

        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        for (fmt++;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                pad = '0';
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        }
        while (isdigit(*fmt))
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            precision = 0;
            if (*++fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            }
            for (; isdigit(*fmt); fmt++)
                precision = precision * 10 + (*fmt - '0');
        }
        while (*fmt == 'l' || *fmt == 'z')
            length++, fmt++;
        if (left)
            pad = ' ';

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = length ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_number(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, v < 0, 10, false, width, left, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v = length ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_number(&o, v, false, *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16, *fmt == 'X', width, left, pad);
            break;
        }
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_number(&o, (uint64_t)va_arg(ap, void *), false, 16, false, 0, false, ' ');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            put_padded(&o, s, precision >= 0 ? strnlen(s, precision) : strlen(s), width, left, ' ');
            break;
        }
        case 'c': {
            char c = va_arg(ap, int);
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
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    char buf[1024];
    va_list copy;
    int n;

    va_copy(copy, ap);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < (int)sizeof(buf)) {
        write(fd, buf, n);
    } else {
        char *big = malloc(n + 1);
        if (big) {
            vsnprintf(big, n + 1, fmt, copy);
            write(fd, big, n);
            free(big);
        }
    }
    va_end(copy);
    return n;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vdprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);
    return n;
}

int fputs_fd(const char *s, int fd)
{
    return write(fd, s, strlen(s));
}

int puts(const char *s)
{
    fputs_fd(s, STDOUT_FILENO);
    return write(STDOUT_FILENO, "\n", 1);
}

int putchar(int c)
{
    char ch = c;

    return write(STDOUT_FILENO, &ch, 1) == 1 ? c : -1;
}

void perror(const char *msg)
{
    if (msg && *msg)
        dprintf(STDERR_FILENO, "%s: %s\n", msg, strerror(errno));
    else
        dprintf(STDERR_FILENO, "%s\n", strerror(errno));
}

ssize_t read_line(int fd, char *buf, size_t size)
{
    size_t n = 0;

    while (n + 1 < size) {
        char c;
        ssize_t r = read(fd, &c, 1);

        if (r < 0)
            return n ? (ssize_t)n : -1;
        if (r == 0)
            break;
        if (c == '\n')
            break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

struct dir_stream *opendir(const char *path)
{
    struct dir_stream *d;
    int fd = open(path, O_RDONLY | O_DIRECTORY);

    if (fd < 0)
        return NULL;
    if (!(d = malloc(sizeof(*d)))) {
        close(fd);
        return NULL;
    }
    d->fd = fd;
    d->pos = d->len = 0;
    return d;
}

struct aegis_dirent *readdir(struct dir_stream *d)
{
    struct aegis_dirent *e;

    if (d->pos >= d->len) {
        ssize_t n = getdents(d->fd, d->buf, sizeof(d->buf));
        if (n <= 0)
            return NULL;
        d->len = n;
        d->pos = 0;
    }
    e = (struct aegis_dirent *)(d->buf + d->pos);
    d->pos += e->reclen;
    return e;
}

void closedir(struct dir_stream *d)
{
    if (d) {
        close(d->fd);
        free(d);
    }
}
