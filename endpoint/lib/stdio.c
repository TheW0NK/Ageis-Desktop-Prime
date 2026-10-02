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

// Floating point: %f, %e and %g with a precision (default 6). Exact to about
// 17 significant digits, which is all a double holds.
static void put_double(struct out *o, double v, char conv, int precision, int width, bool left, char pad,
                       bool alt)
{
    char tmp[400], *t = tmp;
    bool neg = v < 0 || (v == 0 && 1 / v < 0);
    int exp10 = 0, digits;
    bool expform = conv == 'e' || conv == 'E', strip = false;

    if (precision < 0)
        precision = 6;
    if (precision > 40)
        precision = 40;
    if (neg)
        v = -v;
    if (v != v || v - v != 0) {
        const char *s = v != v ? "nan" : "inf";

        if (neg && v == v)
            s = "-inf";
        put_padded(o, s, strlen(s), width, left, ' ');
        return;
    }
    if (v != 0) {
        double m = v;

        while (m >= 10) {
            m /= 10;
            exp10++;
        }
        while (m < 1) {
            m *= 10;
            exp10--;
        }
    }
    if (conv == 'g' || conv == 'G') {
        int p = precision ? precision : 1;

        // Rounding can carry into a new digit (9.9999 -> 10.000).
        if (v != 0) {
            double r = v, scale = 1;
            int e = exp10;

            for (int i = 0; i < p - 1 - e && i < 330; i++)
                scale *= 10;
            for (int i = 0; i < e - (p - 1) && i < 330; i++)
                scale /= 10;
            r = (double)(uint64_t)(v * scale + 0.5) / scale;
            if (r >= 1) {
                double m = r;

                exp10 = 0;
                while (m >= 10) {
                    m /= 10;
                    exp10++;
                }
                while (m < 1) {
                    m *= 10;
                    exp10--;
                }
            }
        }
        expform = exp10 < -4 || exp10 >= p;
        precision = expform ? p - 1 : p - 1 - exp10;
        strip = !alt;
    }
    if (neg)
        *t++ = '-';
    if (expform) {
        double m = v;

        for (int i = 0; i < exp10; i++)
            m /= 10;
        for (int i = 0; i > exp10; i--)
            m *= 10;
        digits = precision;
        {
            double scale = 1;
            uint64_t whole;

            for (int i = 0; i < digits; i++)
                scale *= 10;
            whole = (uint64_t)(m * scale + 0.5);
            if (whole >= (uint64_t)(10 * scale)) {
                whole /= 10;
                exp10++;
            }
            char d[48];
            int n = 0;

            do {
                d[n++] = '0' + whole % 10;
                whole /= 10;
            } while (whole);
            while (n < digits + 1)
                d[n++] = '0';
            *t++ = d[n - 1];
            if (digits) {
                char *dot = t;

                *t++ = '.';
                for (int i = n - 2; i >= 0; i--)
                    *t++ = d[i];
                if (strip) {
                    while (t[-1] == '0')
                        t--;
                    if (t[-1] == '.')
                        t--;
                }
                (void)dot;
            }
        }
        *t++ = conv == 'E' || conv == 'G' ? 'E' : 'e';
        *t++ = exp10 < 0 ? '-' : '+';
        if (exp10 < 0)
            exp10 = -exp10;
        if (exp10 >= 100)
            *t++ = '0' + exp10 / 100;
        *t++ = '0' + exp10 / 10 % 10;
        *t++ = '0' + exp10 % 10;
    } else {
        // Integer part, then the fraction, rounded at `precision` digits.
        double scale = 1, ip;
        char d[360];
        int n = 0;

        for (int i = 0; i < precision; i++)
            scale *= 10;
        if (v * scale < 1.8e19) {
            uint64_t all = (uint64_t)(v * scale + 0.5), whole = all / (uint64_t)scale,
                     frac = all % (uint64_t)scale;

            do {
                d[n++] = '0' + whole % 10;
                whole /= 10;
            } while (whole);
            while (n)
                *t++ = d[--n];
            if (precision) {
                *t++ = '.';
                for (int i = precision - 1; i >= 0; i--) {
                    d[i] = '0' + frac % 10;
                    frac /= 10;
                }
                memcpy(t, d, precision);
                t += precision;
            }
        } else {
            // Too big for exact integer conversion: digits from the leading
            // power of ten, the rest are noise anyway.
            ip = v;
            for (int i = exp10; i >= 0 && t < tmp + sizeof(tmp) - 50; i--) {
                double p10 = 1;
                int dg;

                for (int k = 0; k < i; k++)
                    p10 *= 10;
                dg = (int)(ip / p10);
                if (dg > 9)
                    dg = 9;
                if (dg < 0)
                    dg = 0;
                if (dg == 0 && (t == tmp || t[-1] == '-'))
                    continue;
                *t++ = '0' + dg;
                ip -= dg * p10;
            }
            if (precision) {
                *t++ = '.';
                for (int i = 0; i < precision; i++)
                    *t++ = '0';
            }
        }
        if (strip && precision) {
            while (t[-1] == '0')
                t--;
            if (t[-1] == '.')
                t--;
        }
    }
    if (pad == '0' && neg && !left) {
        put(o, '-');
        put_padded(o, tmp + 1, t - tmp - 1, width - 1, left, pad);
    } else {
        put_padded(o, tmp, t - tmp, width, left, pad);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        bool left = false, alt = false;
        char pad = ' ', sign = 0;
        int width = 0, length = 0, precision = -1;

        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        for (fmt++;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '#')
                alt = true;
            else if (*fmt == '+')
                sign = '+';
            else if (*fmt == ' ' && !sign)
                sign = ' ';
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

            if (v >= 0 && sign) {
                put(&o, sign);
                width--;
            }
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
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        {
            double v = va_arg(ap, double);

            if (sign && !(v < 0)) {
                put(&o, sign);
                width--;
            }
            put_double(&o, v, *fmt == 'F' ? 'f' : *fmt, precision, width, left, pad, alt);
        }
            break;
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

// Adds a line to the system log (/dev/kmsg, root only); others write to
// standard error.
void syslog(const char *tag, const char *fmt, ...)
{
    char line[480];
    int n, fd;
    va_list ap;

    n = snprintf(line, sizeof(line), "%s: ", tag);
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if ((fd = open("/dev/kmsg", O_WRONLY)) >= 0) {
        write(fd, line, strlen(line));
        close(fd);
    } else {
        dprintf(STDERR_FILENO, "%s\n", line);
    }
}
