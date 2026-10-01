#include "aegis.h"
#include "tls.h"

// fetch [-o FILE] [-i] [--ca FILE] URL: downloads over HTTP/1.1 or HTTPS.
// -i prints the response headers too; --ca trusts another CA bundle.

static int parse_url(const char *url, char *host, size_t hsize, uint16_t *port, const char **path,
                     bool *https)
{
    const char *p = url, *slash, *colon;
    size_t len;

    *https = false;
    if (!strncmp(p, "http://", 7)) {
        p += 7;
    } else if (!strncmp(p, "https://", 8)) {
        p += 8;
        *https = true;
    } else if (strstr(p, "://")) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    slash = strchr(p, '/');
    len = slash ? (size_t)(slash - p) : strlen(p);
    *path = slash ? slash : "/";
    colon = memchr(p, ':', len);
    *port = colon ? atoi(colon + 1) : *https ? 443 : 80;
    if (colon)
        len = colon - p;
    if (!len || len >= hsize) {
        errno = EINVAL;
        return -1;
    }
    memcpy(host, p, len);
    host[len] = 0;
    return 0;
}

static struct tls *tls;
static int sock = -1;

static ssize_t conn_read(void *buf, size_t len)
{
    return tls ? tls_read(tls, buf, len) : recv(sock, buf, len, 0);
}

static ssize_t conn_write(const void *buf, size_t len)
{
    return tls ? tls_write(tls, buf, len) : send(sock, buf, len, 0);
}

int main(int argc, char **argv)
{
    const char *url = NULL, *out = NULL, *path, *ca = NULL, *error;
    bool headers = false, https;
    char host[256], buf[8192];
    uint16_t port;
    int ofd = STDOUT_FILENO;
    ssize_t n;
    size_t total = 0;
    bool in_body = false;
    int match = 0, status = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-i"))
            headers = true;
        else if (!strcmp(argv[i], "--ca") && i + 1 < argc)
            ca = argv[++i];
        else
            url = argv[i];
    }
    if (!url) {
        dprintf(STDERR_FILENO, "usage: fetch [-o FILE] [-i] [--ca FILE] URL\n");
        return 2;
    }
    if (parse_url(url, host, sizeof(host), &port, &path, &https) < 0) {
        dprintf(STDERR_FILENO, "fetch: %s: %s\n", url, strerror(errno));
        return 2;
    }
    if (https) {
        if (!(tls = tls_connect(host, port, ca, &error))) {
            dprintf(STDERR_FILENO, "fetch: %s:%u: %s\n", host, port, error);
            return 1;
        }
    } else if ((sock = tcp_connect(host, port, 10000)) < 0) {
        dprintf(STDERR_FILENO, "fetch: %s:%u: %s\n", host, port, strerror(errno));
        return 1;
    }
    n = snprintf(buf, sizeof(buf), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Aegis-fetch/1.0\r\n"
                 "Connection: close\r\nAccept: */*\r\n\r\n", path, host);
    if (conn_write(buf, n) != n) {
        perror("fetch: send");
        return 1;
    }
    if (out && (ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        perror("fetch: open");
        return 1;
    }
    while ((n = conn_read(buf, sizeof(buf))) > 0) {
        ssize_t start = 0;

        if (!in_body) {
            // Status line, then headers up to the blank line.
            if (!status && n > 12 && !strncmp(buf, "HTTP/1.", 7))
                status = atoi(buf + 9);
            for (ssize_t i = 0; i < n && !in_body; i++) {
                static const char end[] = "\r\n\r\n";

                match = buf[i] == end[match] ? match + 1 : buf[i] == '\r' ? 1 : 0;
                if (match == 4) {
                    in_body = true;
                    start = i + 1;
                }
            }
            if (headers)
                write(STDERR_FILENO, buf, in_body ? start : n);
            if (!in_body)
                continue;
        }
        write(ofd, buf + start, n - start);
        total += n - start;
    }
    if (n < 0)
        dprintf(STDERR_FILENO, "fetch: receive failed: %s\n", tls ? tls_error(tls) : strerror(errno));
    if (tls)
        tls_close(tls);
    else
        close(sock);
    if (out) {
        close(ofd);
        printf("fetch: saved %lu bytes to %s (HTTP %d)\n", total, out, status);
    }
    return n < 0 || status >= 400 ? 1 : 0;
}
