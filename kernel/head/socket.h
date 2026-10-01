#ifndef AEGIS_SOCKET_H
#define AEGIS_SOCKET_H

#include "vfs.h"
#include "abi/socket.h"

struct socket;

// A message on its way in or out of a socket, in kernel memory.
struct kmsg {
    void *data;
    size_t len;
    struct file *fds[SCM_MAX_FDS];
    int nfds;
    uint8_t addr[sizeof(struct sockaddr_un)];   // big enough for every family
    uint32_t addrlen;
    uint32_t flags;                 // MSG_* in, MSG_TRUNC etc. out
};

struct sock_ops {
    int (*bind)(struct socket *s, const void *addr, uint32_t len);
    int (*listen)(struct socket *s, int backlog);
    int (*accept)(struct socket *s, bool nonblock, struct socket **out);
    int (*connect)(struct socket *s, const void *addr, uint32_t len, bool nonblock);
    int64_t (*send)(struct socket *s, struct kmsg *m, bool nonblock);
    int64_t (*recv)(struct socket *s, struct kmsg *m, bool nonblock);
    int (*shutdown)(struct socket *s, int how);
    uint32_t (*poll)(struct socket *s, struct poll_table *pt);
    void (*release)(struct socket *s);
    int (*getsockopt)(struct socket *s, int level, int opt, void *val, uint32_t *len);
    int (*setsockopt)(struct socket *s, int level, int opt, const void *val, uint32_t len);
    int (*getname)(struct socket *s, bool peer, void *addr, uint32_t *len);
};

struct socket {
    int domain, type;
    const struct sock_ops *ops;
    void *impl;
    // Credentials of the creating process, for SO_PEERCRED and path checks.
    struct ucred cred;
    struct cred fs_cred;
    struct vnode *cwd;
};

struct file *socket_file(struct socket *s, uint32_t flags);
struct socket *socket_from_file(struct file *f);
struct socket *socket_alloc(int domain, int type);
void socket_free(struct socket *s);

int socket_create(int domain, int type, int protocol, struct socket **out);
int socket_pair(int domain, int type, struct socket **a, struct socket **b);

int unix_create(struct socket *s);
int unix_pair(struct socket *a, struct socket *b);

#endif
