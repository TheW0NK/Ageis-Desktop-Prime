#include "socket.h"
#include "mem.h"
#include "process.h"
#include "string.h"
#include "abi/poll.h"

// The generic part of sockets: files, creation, and dispatch to the domain.

static int64_t f_read(struct file *f, void *buf, size_t size)
{
    struct socket *s = f->priv;
    struct kmsg m = { .data = buf, .len = size };
    int64_t n = s->ops->recv(s, &m, f->flags & O_NONBLOCK);

    for (int i = 0; i < m.nfds; i++)
        file_put(m.fds[i]);         // descriptors need recvmsg
    return n;
}

static int64_t f_write(struct file *f, const void *buf, size_t size)
{
    struct socket *s = f->priv;
    struct kmsg m = { .data = (void *)buf, .len = size };

    return s->ops->send(s, &m, f->flags & O_NONBLOCK);
}

static uint32_t f_poll(struct file *f, struct poll_table *pt)
{
    struct socket *s = f->priv;

    return s->ops->poll(s, pt);
}

static void f_close(struct file *f)
{
    socket_free(f->priv);
}

static const struct file_ops socket_ops = {
    .read = f_read, .write = f_write, .close = f_close, .poll = f_poll,
};

struct socket *socket_alloc(int domain, int type)
{
    struct socket *s = kzalloc(sizeof(*s));
    struct process *p = process_current();

    if (!s)
        return NULL;
    s->domain = domain;
    s->type = type;
    if (p) {
        s->cred = (struct ucred){ p->pid, p->cred.euid, p->cred.egid };
        s->fs_cred = p->cred;
        s->cwd = p->cwd;
    } else {
        s->fs_cred = root_cred;
    }
    if (s->cwd)
        vref(s->cwd);
    return s;
}

void socket_free(struct socket *s)
{
    if (!s)
        return;
    if (s->ops && s->ops->release)
        s->ops->release(s);
    vput(s->cwd);
    kfree(s);
}

struct file *socket_file(struct socket *s, uint32_t flags)
{
    struct file *f = file_alloc(&socket_ops, O_RDWR | (flags & O_NONBLOCK));

    if (f)
        f->priv = s;
    return f;
}

struct socket *socket_from_file(struct file *f)
{
    return (f && f->ops == &socket_ops) ? f->priv : NULL;
}

int socket_create(int domain, int type, int protocol, struct socket **out)
{
    struct socket *s;
    int ret;

    if (protocol != 0)
        return -EPROTONOSUPPORT;
    if (domain != AF_UNIX)
        return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
        return -EPROTOTYPE;
    if (!(s = socket_alloc(domain, type)))
        return -ENOMEM;
    if ((ret = unix_create(s))) {
        socket_free(s);
        return ret;
    }
    *out = s;
    return 0;
}

int socket_pair(int domain, int type, struct socket **a, struct socket **b)
{
    int ret;

    if (domain != AF_UNIX)
        return -EOPNOTSUPP;
    if ((ret = socket_create(domain, type, 0, a)))
        return ret;
    if ((ret = socket_create(domain, type, 0, b))) {
        socket_free(*a);
        return ret;
    }
    if ((ret = unix_pair(*a, *b))) {
        socket_free(*a);
        socket_free(*b);
    }
    return ret;
}
