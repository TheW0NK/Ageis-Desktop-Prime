#include "aegis.h"

int socket(int d, int t, int p) { return __check(syscall3(SYS_SOCKET, d, t, p)); }
int socketpair(int d, int t, int p, int fds[2]) { return __check(syscall4(SYS_SOCKETPAIR, d, t, p, fds)); }
int bind(int fd, const void *a, uint32_t l) { return __check(syscall3(SYS_BIND, fd, a, l)); }
int listen(int fd, int b) { return __check(syscall2(SYS_LISTEN, fd, b)); }
int accept(int fd, void *a, uint32_t *l) { return __check(syscall4(SYS_ACCEPT, fd, a, l, 0)); }
int accept4(int fd, void *a, uint32_t *l, int f) { return __check(syscall4(SYS_ACCEPT, fd, a, l, f)); }
int connect(int fd, const void *a, uint32_t l) { return __check(syscall3(SYS_CONNECT, fd, a, l)); }
ssize_t sendmsg(int fd, const struct aegis_msghdr *m, int f) { return __check(syscall3(SYS_SENDMSG, fd, m, f)); }
ssize_t recvmsg(int fd, struct aegis_msghdr *m, int f) { return __check(syscall3(SYS_RECVMSG, fd, m, f)); }
int shutdown(int fd, int how) { return __check(syscall2(SYS_SHUTDOWN, fd, how)); }
int getsockname(int fd, void *a, uint32_t *l) { return __check(syscall3(SYS_GETSOCKNAME, fd, a, l)); }
int getpeername(int fd, void *a, uint32_t *l) { return __check(syscall3(SYS_GETPEERNAME, fd, a, l)); }

int getsockopt(int fd, int level, int opt, void *val, uint32_t *len)
{
    return __check(syscall5(SYS_GETSOCKOPT, fd, level, opt, val, len));
}

int setsockopt(int fd, int level, int opt, const void *val, uint32_t len)
{
    return __check(syscall5(SYS_SETSOCKOPT, fd, level, opt, val, len));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const void *addr, uint32_t alen)
{
    struct iovec iov = { (void *)buf, len };
    struct aegis_msghdr m = { (void *)addr, alen, 1, &iov, NULL, 0, 0 };

    return sendmsg(fd, &m, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, void *addr, uint32_t *alen)
{
    struct iovec iov = { buf, len };
    struct aegis_msghdr m = { addr, alen ? *alen : 0, 1, &iov, NULL, 0, 0 };
    ssize_t n = recvmsg(fd, &m, flags);

    if (n >= 0 && alen)
        *alen = m.namelen;
    return n;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return sendto(fd, buf, len, flags, NULL, 0);
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return recvfrom(fd, buf, len, flags, NULL, NULL);
}

ssize_t send_fds(int fd, const void *buf, size_t len, const int *fds, int nfds)
{
    struct iovec iov = { (void *)buf, len };
    struct aegis_msghdr m = { NULL, 0, 1, &iov, (int32_t *)fds, nfds, 0 };

    return sendmsg(fd, &m, 0);
}

ssize_t recv_fds(int fd, void *buf, size_t len, int *fds, int *nfds)
{
    struct iovec iov = { buf, len };
    struct aegis_msghdr m = { NULL, 0, 1, &iov, fds, *nfds, 0 };
    ssize_t n = recvmsg(fd, &m, 0);

    *nfds = n >= 0 ? (int)m.nfds : 0;
    return n;
}

static uint32_t make_addr(struct sockaddr_un *a, const char *name)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strlcpy(a->sun_path, name, sizeof(a->sun_path));
    return sizeof(uint16_t) + strlen(a->sun_path) + 1;
}

int unix_listen(const char *name, int type)
{
    struct sockaddr_un a;
    uint32_t len = make_addr(&a, name);
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;
    if (bind(fd, &a, len) < 0 || (type != SOCK_DGRAM && listen(fd, 64) < 0)) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int unix_connect(const char *name, int type)
{
    struct sockaddr_un a;
    uint32_t len = make_addr(&a, name);
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;
    if (connect(fd, &a, len) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

// ---- Internet helpers ----

bool inet_parse(const char *text, uint32_t *addr)
{
    uint32_t parts[4];
    int n = 0;

    while (n < 4) {
        uint32_t v = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9' && digits < 4) {
            v = v * 10 + (*text++ - '0');
            digits++;
        }
        if (!digits || v > 255)
            return false;
        parts[n++] = v;
        if (n < 4 && *text++ != '.')
            return false;
    }
    if (*text)
        return false;
    *addr = htonl(parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]);
    return true;
}

char *inet_format(uint32_t addr, char *buf)
{
    uint32_t a = ntohl(addr);

    snprintf(buf, 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return buf;
}

int netconfig(int op, int index, struct aegis_netif *info)
{
    return __check(syscall3(SYS_NETCONFIG, op, index, info));
}

static bool hosts_lookup(const char *name, uint32_t *addr)
{
    char line[256];
    int fd = open("/msc/hosts", O_RDONLY);
    bool found = false;

    if (fd < 0)
        return false;
    while (!found && read_line(fd, line, sizeof(line)) >= 0) {
        char *p = line, *ip, *h;

        while (isspace(*p))
            p++;
        if (*p == '#' || !*p)
            continue;
        ip = p;
        while (*p && !isspace(*p))
            p++;
        if (*p)
            *p++ = 0;
        while (*p && !found) {
            while (isspace(*p))
                p++;
            h = p;
            while (*p && !isspace(*p) && *p != '#')
                p++;
            if (*p)
                *p++ = 0;
            if (*h && !strcmp(h, name))
                found = inet_parse(ip, addr);
        }
    }
    close(fd);
    return found;
}

static size_t encode_name(const char *name, uint8_t *out)
{
    size_t n = 0;

    while (*name) {
        const char *dot = strchr(name, '.');
        size_t len = dot ? (size_t)(dot - name) : strlen(name);

        if (!len || len > 63)
            return 0;
        out[n++] = len;
        memcpy(out + n, name, len);
        n += len;
        name += len + (dot ? 1 : 0);
    }
    out[n++] = 0;
    return n;
}

// Skips a (possibly compressed) name; returns the offset after it, or 0.
static size_t skip_name(const uint8_t *msg, size_t len, size_t off)
{
    while (off < len) {
        if (msg[off] == 0)
            return off + 1;
        if ((msg[off] & 0xC0) == 0xC0)
            return off + 2 <= len ? off + 2 : 0;
        off += msg[off] + 1;
    }
    return 0;
}

static int dns_query(uint32_t server, const char *name, uint32_t *addr)
{
    uint8_t q[512], r[1500];
    uint16_t id = (uint16_t)(uptime_ms() * 2654435761U);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr = { server } };
    struct pollfd p;
    size_t n, qlen;
    int fd, ret = -1;

    memset(q, 0, 12);
    q[0] = id >> 8;
    q[1] = id;
    q[2] = 0x01;                    // recursion desired
    q[5] = 1;                       // one question
    if (!(n = encode_name(name, q + 12))) {
        errno = ENOENT;
        return -1;
    }
    qlen = 12 + n;
    q[qlen++] = 0; q[qlen++] = 1;   // type A
    q[qlen++] = 0; q[qlen++] = 1;   // class IN

    if ((fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0)
        return -1;
    errno = ETIMEDOUT;
    for (int attempt = 0; attempt < 3 && ret < 0; attempt++) {
        if (sendto(fd, q, qlen, 0, &sa, sizeof(sa)) < 0)
            break;
        p = (struct pollfd){ fd, POLLIN, 0 };
        while (poll(&p, 1, 1500) == 1) {
            ssize_t got = recv(fd, r, sizeof(r), 0);
            size_t off;
            int answers;

            if (got < 12 || r[0] != q[0] || r[1] != q[1] || !(r[2] & 0x80))
                continue;
            if ((r[3] & 0x0F) == 3) {           // NXDOMAIN
                errno = ENOENT;
                attempt = 3;
                break;
            }
            answers = r[6] << 8 | r[7];
            off = skip_name(r, got, 12);
            if (!off || off + 4 > (size_t)got)
                break;
            off += 4;
            for (int i = 0; i < answers && off; i++) {
                uint16_t type, rdlen;

                off = skip_name(r, got, off);
                if (!off || off + 10 > (size_t)got)
                    break;
                type = r[off] << 8 | r[off + 1];
                rdlen = r[off + 8] << 8 | r[off + 9];
                off += 10;
                if (off + rdlen > (size_t)got)
                    break;
                // CNAMEs come first; the A record that follows is the answer.
                if (type == 1 && rdlen == 4) {
                    memcpy(addr, r + off, 4);
                    ret = 0;
                    break;
                }
                off += rdlen;
            }
            if (ret < 0)
                errno = ENOENT;
            attempt = 3;
            break;
        }
    }
    close(fd);
    return ret;
}

int resolve_host(const char *name, uint32_t *addr)
{
    struct aegis_netif info;

    if (inet_parse(name, addr))
        return 0;
    if (!strcmp(name, "localhost")) {
        *addr = htonl(INADDR_LOOPBACK);
        return 0;
    }
    if (hosts_lookup(name, addr))
        return 0;
    for (int i = 0; netconfig(NETCONFIG_GET, i, &info) == 0; i++) {
        for (int d = 0; d < 2; d++) {
            if (info.dns[d] && dns_query(info.dns[d], name, addr) == 0)
                return 0;
            if (info.dns[d] && errno == ENOENT)
                return -1;
        }
    }
    if (errno != ENOENT)
        errno = ETIMEDOUT;
    return -1;
}

int tcp_connect(const char *host, uint16_t port, uint64_t timeout_ms)
{
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    int fd;

    if (resolve_host(host, &sa.sin_addr.s_addr) < 0)
        return -1;
    if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
        return -1;
    if (timeout_ms)
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    if (connect(fd, &sa, sizeof(sa)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
