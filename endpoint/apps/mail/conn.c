#include "mail.h"

// Line-oriented connections for IMAP and SMTP, plain or over TLS.

int mconn_open(struct mconn *c, const char *host, int port, bool tls, const char *ca_file)
{
    const char *error;

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    if (tls) {
        if (!(c->tls = tls_connect(host, port, ca_file && *ca_file ? ca_file : NULL, &error))) {
            snprintf(c->error, sizeof(c->error), "Could not make a secure connection to %s: %s", host, error);
            return -1;
        }
        return 0;
    }
    if ((c->fd = tcp_connect(host, port, 15000)) < 0) {
        snprintf(c->error, sizeof(c->error), "Could not connect to %s:%d (%s)", host, port, strerror(errno));
        return -1;
    }
    return 0;
}

int mconn_starttls(struct mconn *c, const char *host, const char *ca_file)
{
    const char *error;

    c->len = c->pos = 0;
    if (!(c->tls = tls_wrap(c->fd, host, ca_file && *ca_file ? ca_file : NULL, &error))) {
        snprintf(c->error, sizeof(c->error), "Could not make the connection secure: %s", error);
        c->fd = -1;
        return -1;
    }
    c->fd = -1;
    return 0;
}

static ssize_t fill(struct mconn *c)
{
    ssize_t n;

    if (c->pos == c->len)
        c->pos = c->len = 0;
    else if (c->pos) {
        memmove(c->buf, c->buf + c->pos, c->len - c->pos);
        c->len -= c->pos;
        c->pos = 0;
    }
    if (c->len == sizeof(c->buf))
        return -1;
    n = c->tls ? tls_read(c->tls, c->buf + c->len, sizeof(c->buf) - c->len)
               : recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
    if (n <= 0) {
        snprintf(c->error, sizeof(c->error), "The server closed the connection.");
        return -1;
    }
    c->len += n;
    return n;
}

ssize_t mconn_line(struct mconn *c, char *line, size_t size)
{
    for (;;) {
        char *nl = memchr(c->buf + c->pos, '\n', c->len - c->pos);

        if (nl) {
            size_t n = nl - (c->buf + c->pos);
            size_t copy = MIN(n, size - 1);

            memcpy(line, c->buf + c->pos, copy);
            line[copy] = 0;
            if (copy && line[copy - 1] == '\r')
                line[--copy] = 0;
            c->pos += n + 1;
            return copy;
        }
        if (c->len - c->pos >= sizeof(c->buf) - 1) {
            // An overlong line: hand back what fits.
            size_t copy = MIN(c->len - c->pos, size - 1);

            memcpy(line, c->buf + c->pos, copy);
            line[copy] = 0;
            c->pos += copy;
            return copy;
        }
        if (fill(c) < 0)
            return -1;
    }
}

int mconn_read(struct mconn *c, char *buf, size_t n)
{
    size_t got = 0;

    while (got < n) {
        size_t have = c->len - c->pos;

        if (!have) {
            if (fill(c) < 0)
                return -1;
            continue;
        }
        have = MIN(have, n - got);
        memcpy(buf + got, c->buf + c->pos, have);
        c->pos += have;
        got += have;
    }
    return 0;
}

int mconn_send(struct mconn *c, const void *data, size_t len)
{
    const char *p = data;

    while (len) {
        ssize_t n = c->tls ? tls_write(c->tls, p, len) : send(c->fd, p, len, 0);

        if (n <= 0) {
            snprintf(c->error, sizeof(c->error), "Sending to the server failed.");
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

int mconn_printf(struct mconn *c, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return mconn_send(c, buf, MIN(n, (int)sizeof(buf) - 1));
}

void mconn_close(struct mconn *c)
{
    if (c->tls)
        tls_close(c->tls);
    else if (c->fd >= 0)
        close(c->fd);
    c->tls = NULL;
    c->fd = -1;
}
