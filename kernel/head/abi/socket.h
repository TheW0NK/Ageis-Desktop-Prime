#ifndef AEGIS_ABI_SOCKET_H
#define AEGIS_ABI_SOCKET_H

#include <stdint.h>

#define AF_UNIX         1
#define AF_INET         2
#define AF_INET6        10

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_SEQPACKET  5
#define SOCK_NONBLOCK   0x800
#define SOCK_CLOEXEC    0x80000

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#define MSG_PEEK        0x02
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000

#define SOL_SOCKET      1
#define SO_PEERCRED     17

#define UNIX_PATH_MAX   108
#define SCM_MAX_FDS     16

// A path beginning with '@' names an abstract socket that has no file.
struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[UNIX_PATH_MAX];
};

struct ucred {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

struct iovec {
    void *base;
    uint64_t len;
};

// Descriptors passed with a message travel in fds[] (SCM_RIGHTS).
struct aegis_msghdr {
    void *name;
    uint32_t namelen;
    uint32_t iovlen;
    struct iovec *iov;
    int32_t *fds;
    uint32_t nfds;                  // in: room in fds[]; out: received count
    uint32_t flags;                 // out: MSG_TRUNC etc.
};

#define MSG_TRUNC       0x20
#define MSG_CTRUNC      0x08

#endif
