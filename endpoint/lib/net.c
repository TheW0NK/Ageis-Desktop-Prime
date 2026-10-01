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
