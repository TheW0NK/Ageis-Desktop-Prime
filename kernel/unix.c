#include "socket.h"
#include "mem.h"
#include "process.h"
#include "string.h"
#include "abi/poll.h"

// AF_UNIX sockets: stream, datagram and sequenced-packet (message) sockets,
// named by a file or by an abstract name beginning with '@', with descriptor
// passing and peer credentials. One mutex guards all of them.

#define QUEUE_LIMIT     (256 * 1024)
#define MAX_BACKLOG     128

enum { US_IDLE, US_LISTENING, US_CONNECTED, US_DISCONNECTED };

struct umsg {
    struct umsg *next;
    size_t len, off;
    struct file *fds[SCM_MAX_FDS];
    int nfds;
    char from[UNIX_PATH_MAX];
    uint8_t data[];
};

struct usock {
    struct socket *sock;
    int type, state;
    uint32_t refs;
    bool released;
    struct wait_queue wq;
    struct usock *peer;
    struct ucred peer_cred;
    struct umsg *rq, *rq_tail;
    size_t rq_bytes;
    bool rd_shut, wr_shut, peer_wr_shut;
    // Listening: connections waiting for accept().
    struct usock *backlog, *backlog_tail, *bl_next;
    int backlog_max, backlog_n;
    // Name.
    char name[UNIX_PATH_MAX];
    bool bound;
    uint32_t mount_id;
    uint64_t ino;
    struct usock *bound_next;
};

static struct mutex ulock = MUTEX_INIT;
static struct usock *bound_list;
static const struct sock_ops unix_ops;

static void unref(struct usock *u, struct umsg **garbage)
{
    if (--u->refs)
        return;
    // Queued messages may hold files; they are released after the lock.
    if (u->rq_tail) {
        u->rq_tail->next = *garbage;
        *garbage = u->rq;
    }
    kfree(u);
}

static void free_garbage(struct umsg *m)
{
    while (m) {
        struct umsg *next = m->next;

        for (int i = 0; i < m->nfds; i++)
            file_put(m->fds[i]);
        kfree(m);
        m = next;
    }
}

static struct usock *usock_new(struct socket *s)
{
    struct usock *u = kzalloc(sizeof(*u));

    if (!u)
        return NULL;
    u->sock = s;
    u->type = s->type;
    u->refs = 1;
    u->wq = (struct wait_queue)WAIT_QUEUE_INIT;
    s->impl = u;
    s->ops = &unix_ops;
    return u;
}

int unix_create(struct socket *s)
{
    return usock_new(s) ? 0 : -ENOMEM;
}

static void link_peers(struct usock *a, struct usock *b)
{
    a->peer = b;
    b->peer = a;
    a->refs++;
    b->refs++;
    a->state = b->state = US_CONNECTED;
    a->peer_cred = b->sock->cred;
    b->peer_cred = a->sock->cred;
}

int unix_pair(struct socket *a, struct socket *b)
{
    mutex_lock(&ulock);
    link_peers(a->impl, b->impl);
    mutex_unlock(&ulock);
    return 0;
}

// Sleeps until woken; called with ulock held, returns with it held.
static int uwait(struct usock *u, bool nonblock)
{
    struct wait_entry e = { sched_current(), NULL };

    if (nonblock)
        return -EAGAIN;
    if (signal_pending())
        return -EINTR;
    wait_queue_add(&u->wq, &e);
    mutex_unlock(&ulock);
    sched_block_timeout(UINT64_MAX);
    wait_queue_remove(&u->wq, &e);
    mutex_lock(&ulock);
    return 0;
}

static int name_from(const void *addr, uint32_t len, char *name)
{
    const struct sockaddr_un *sa = addr;
    size_t n;

    if (len < sizeof(uint16_t) + 1 || len > sizeof(*sa) || sa->sun_family != AF_UNIX)
        return -EINVAL;
    n = strnlen(sa->sun_path, MIN(len - sizeof(uint16_t), (uint32_t)UNIX_PATH_MAX - 1));
    if (!n)
        return -EINVAL;
    memset(name, 0, UNIX_PATH_MAX);
    memcpy(name, sa->sun_path, n);
    return 0;
}

static struct usock *find_bound(const char *name, struct socket *by, int *err)
{
    if (name[0] == '@') {
        for (struct usock *u = bound_list; u; u = u->bound_next) {
            if (!strcmp(u->name, name))
                return u;
        }
        *err = -ECONNREFUSED;
        return NULL;
    }

    struct vnode *v;
    int ret = vfs_lookup(name, by->cwd, &by->fs_cred, true, &v);

    if (ret) {
        *err = ret;
        return NULL;
    }
    if (!S_ISSOCK(v->mode)) {
        vput(v);
        *err = -ECONNREFUSED;
        return NULL;
    }
    if ((ret = vfs_permission(v, &by->fs_cred, W_OK))) {
        vput(v);
        *err = ret;
        return NULL;
    }
    for (struct usock *u = bound_list; u; u = u->bound_next) {
        if (u->name[0] != '@' && u->mount_id == v->mount->id && u->ino == v->ino) {
            vput(v);
            return u;
        }
    }
    vput(v);
    *err = -ECONNREFUSED;
    return NULL;
}

static int u_bind(struct socket *s, const void *addr, uint32_t len)
{
    struct usock *u = s->impl;
    char name[UNIX_PATH_MAX];
    struct vnode *v = NULL;
    int ret;

    if ((ret = name_from(addr, len, name)))
        return ret;
    mutex_lock(&ulock);
    if (u->bound) {
        ret = -EINVAL;
    } else if (name[0] == '@') {
        for (struct usock *o = bound_list; o && !ret; o = o->bound_next) {
            if (!strcmp(o->name, name))
                ret = -EADDRINUSE;
        }
    } else {
        ret = vfs_mknod(name, s->cwd, &s->fs_cred, S_IFSOCK | 0755, &v);
        if (ret == -EEXIST)
            ret = -EADDRINUSE;
        if (!ret) {
            u->mount_id = v->mount->id;
            u->ino = v->ino;
            vput(v);
        }
    }
    if (!ret) {
        memcpy(u->name, name, UNIX_PATH_MAX);
        u->bound = true;
        u->bound_next = bound_list;
        bound_list = u;
    }
    mutex_unlock(&ulock);
    return ret;
}

static int u_listen(struct socket *s, int backlog)
{
    struct usock *u = s->impl;
    int ret = 0;

    if (u->type == SOCK_DGRAM)
        return -EOPNOTSUPP;
    mutex_lock(&ulock);
    if (!u->bound || (u->state != US_IDLE && u->state != US_LISTENING))
        ret = -EINVAL;
    else {
        u->state = US_LISTENING;
        u->backlog_max = backlog < 1 ? 1 : MIN(backlog, MAX_BACKLOG);
    }
    mutex_unlock(&ulock);
    return ret;
}

static int u_connect(struct socket *s, const void *addr, uint32_t len, bool nonblock)
{
    struct usock *u = s->impl, *target, *server;
    struct socket *ss;
    struct umsg *garbage = NULL;
    char name[UNIX_PATH_MAX];
    int ret = 0;

    (void)nonblock;
    if ((ret = name_from(addr, len, name)))
        return ret;
    mutex_lock(&ulock);
    if (!(target = find_bound(name, s, &ret)))
        goto out;
    if (u->type == SOCK_DGRAM) {
        if (target->type != SOCK_DGRAM) {
            ret = -EPROTOTYPE;
            goto out;
        }
        // A datagram socket's peer is only a default destination.
        if (u->peer)
            unref(u->peer, &garbage);
        u->peer = target;
        target->refs++;
        u->peer_cred = target->sock->cred;
        goto out;
    }
    if (u->state == US_CONNECTED) {
        ret = -EISCONN;
        goto out;
    }
    if (u->state != US_IDLE) {
        ret = -EINVAL;
        goto out;
    }
    if (target->state != US_LISTENING || target->type != u->type) {
        ret = target->type != u->type ? -EPROTOTYPE : -ECONNREFUSED;
        goto out;
    }
    if (target->backlog_n >= target->backlog_max) {
        ret = -ECONNREFUSED;
        goto out;
    }
    // The server's end exists now and waits in the backlog for accept().
    if (!(ss = socket_alloc(AF_UNIX, u->type)) || !(server = usock_new(ss))) {
        kfree(ss);
        ret = -ENOMEM;
        goto out;
    }
    ss->cred = target->sock->cred;
    ss->fs_cred = target->sock->fs_cred;
    memcpy(server->name, target->name, UNIX_PATH_MAX);
    link_peers(u, server);
    server->refs++;                 // held by the backlog
    if (target->backlog_tail)
        target->backlog_tail->bl_next = server;
    else
        target->backlog = server;
    target->backlog_tail = server;
    target->backlog_n++;
    wake_up(&target->wq);
out:
    mutex_unlock(&ulock);
    free_garbage(garbage);
    return ret;
}

static int u_accept(struct socket *s, bool nonblock, struct socket **out)
{
    struct usock *u = s->impl, *c;
    struct umsg *g = NULL;
    int ret;

    mutex_lock(&ulock);
    for (;;) {
        wait_prepare();
        if (u->state != US_LISTENING) {
            mutex_unlock(&ulock);
            return -EINVAL;
        }
        if (u->backlog)
            break;
        if ((ret = uwait(u, nonblock))) {
            mutex_unlock(&ulock);
            return ret;
        }
    }
    c = u->backlog;
    u->backlog = c->bl_next;
    if (!u->backlog)
        u->backlog_tail = NULL;
    u->backlog_n--;
    c->bl_next = NULL;
    unref(c, &g);                   // the backlog's hold; the file now owns it
    mutex_unlock(&ulock);
    free_garbage(g);
    *out = c->sock;
    return 0;
}

static bool at_eof(struct usock *u)
{
    if (u->rd_shut || u->peer_wr_shut)
        return true;
    return u->type != SOCK_DGRAM && u->state == US_DISCONNECTED;
}

static int64_t u_send(struct socket *s, struct kmsg *m, bool nonblock)
{
    struct usock *u = s->impl, *dest = NULL;
    size_t sent = 0;
    int ret = 0;

    if (u->type == SOCK_SEQPACKET && m->len > QUEUE_LIMIT)
        return -EMSGSIZE;
    mutex_lock(&ulock);
    for (;;) {
        size_t room, chunk;
        struct umsg *msg;

        wait_prepare();
        if (u->wr_shut) {
            ret = -EPIPE;
            break;
        }
        if (u->type == SOCK_DGRAM) {
            if (m->addrlen) {
                char name[UNIX_PATH_MAX];

                if ((ret = name_from(m->addr, m->addrlen, name)))
                    break;
                if (!(dest = find_bound(name, s, &ret)))
                    break;
                if (dest->type != SOCK_DGRAM) {
                    ret = -EPROTOTYPE;
                    break;
                }
            } else if (!(dest = u->peer)) {
                ret = -EDESTADDRREQ;
                break;
            }
            if (dest->released) {
                ret = -ECONNREFUSED;
                break;
            }
        } else {
            if (u->state == US_DISCONNECTED) {
                ret = -EPIPE;
                break;
            }
            if (u->state != US_CONNECTED) {
                ret = -ENOTCONN;
                break;
            }
            dest = u->peer;
        }
        if (dest->rd_shut) {
            ret = -EPIPE;
            break;
        }
        room = QUEUE_LIMIT > dest->rq_bytes ? QUEUE_LIMIT - dest->rq_bytes : 0;
        if ((u->type == SOCK_STREAM && !room && m->len - sent)
            || (u->type != SOCK_STREAM && room < m->len && dest->rq)) {
            if (sent) {
                ret = 0;
                break;
            }
            if ((ret = uwait(u, nonblock)))
                break;
            continue;
        }
        chunk = u->type == SOCK_STREAM ? MIN(m->len - sent, MAX(room, (size_t)1)) : m->len;
        if (!(msg = kmalloc(sizeof(*msg) + chunk + 1))) {
            ret = -ENOBUFS;
            break;
        }
        memset(msg, 0, sizeof(*msg));
        msg->len = chunk;
        memcpy(msg->data, (uint8_t *)m->data + sent, chunk);
        if (!sent) {
            // Descriptors travel with the first piece.
            for (int i = 0; i < m->nfds; i++) {
                msg->fds[i] = m->fds[i];
                m->fds[i] = NULL;
            }
            msg->nfds = m->nfds;
            m->nfds = 0;
        }
        if (u->bound)
            memcpy(msg->from, u->name, UNIX_PATH_MAX);
        if (dest->rq_tail)
            dest->rq_tail->next = msg;
        else
            dest->rq = msg;
        dest->rq_tail = msg;
        dest->rq_bytes += chunk;
        sent += chunk;
        wake_up(&dest->wq);
        if (sent >= m->len)
            break;
    }
    mutex_unlock(&ulock);
    if (ret == -EPIPE && !(m->flags & MSG_NOSIGNAL))
        signal_thread(sched_current(), SIGPIPE);
    return sent ? (int64_t)sent : ret;
}

static void fill_from(struct kmsg *m, const char *from)
{
    struct sockaddr_un *sa = (struct sockaddr_un *)m->addr;

    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    memcpy(sa->sun_path, from, UNIX_PATH_MAX);
    m->addrlen = sizeof(uint16_t) + strnlen(from, UNIX_PATH_MAX) + 1;
}

static int64_t u_recv(struct socket *s, struct kmsg *m, bool nonblock)
{
    struct usock *u = s->impl;
    bool peek = m->flags & MSG_PEEK;
    struct umsg *done = NULL;
    size_t got = 0;
    int ret = 0;

    m->flags &= ~(MSG_TRUNC | MSG_CTRUNC);
    m->nfds = 0;
    m->addrlen = 0;
    mutex_lock(&ulock);
    for (;;) {
        wait_prepare();
        if (u->rq)
            break;
        if (u->state == US_LISTENING) {
            ret = -EINVAL;
            goto out;
        }
        if (at_eof(u))
            goto out;
        if (u->type != SOCK_DGRAM && u->state == US_IDLE) {
            ret = -ENOTCONN;
            goto out;
        }
        if ((ret = uwait(u, nonblock)))
            goto out;
    }

    if (u->type != SOCK_STREAM) {
        struct umsg *msg = u->rq;
        size_t n = MIN(m->len, msg->len);

        memcpy(m->data, msg->data, n);
        got = n;
        if (n < msg->len)
            m->flags |= MSG_TRUNC;
        if (u->type == SOCK_DGRAM && msg->from[0])
            fill_from(m, msg->from);
        if (!peek) {
            for (int i = 0; i < msg->nfds; i++)
                m->fds[i] = msg->fds[i];
            m->nfds = msg->nfds;
            msg->nfds = 0;
            u->rq = msg->next;
            if (!u->rq)
                u->rq_tail = NULL;
            u->rq_bytes -= msg->len;
            msg->next = done;
            done = msg;
        }
    } else {
        struct umsg *msg = u->rq;
        size_t skip = 0;

        while (msg && got < m->len) {
            size_t avail = msg->len - msg->off - (peek ? skip : 0);
            size_t n = MIN(m->len - got, avail);

            // Stop before a later message that carries descriptors.
            if (got && msg->nfds)
                break;
            memcpy((uint8_t *)m->data + got, msg->data + msg->off + (peek ? skip : 0), n);
            got += n;
            if (!peek && msg->nfds) {
                for (int i = 0; i < msg->nfds; i++)
                    m->fds[i] = msg->fds[i];
                m->nfds = msg->nfds;
                msg->nfds = 0;
            }
            if (peek) {
                msg = msg->next;
                skip = 0;
                continue;
            }
            msg->off += n;
            u->rq_bytes -= n;
            if (msg->off == msg->len) {
                u->rq = msg->next;
                if (!u->rq)
                    u->rq_tail = NULL;
                msg->next = done;
                done = msg;
                msg = u->rq;
            }
        }
    }
    // Senders blocked on a full queue wait on their own sockets.
    if (!peek && u->peer)
        wake_up(&u->peer->wq);
    if (!peek && u->type == SOCK_DGRAM) {
        for (struct usock *o = bound_list; o; o = o->bound_next)
            wake_up(&o->wq);
    }
out:
    mutex_unlock(&ulock);
    free_garbage(done);
    return got ? (int64_t)got : ret;
}

static int u_shutdown(struct socket *s, int how)
{
    struct usock *u = s->impl;

    mutex_lock(&ulock);
    if (how == SHUT_RD || how == SHUT_RDWR)
        u->rd_shut = true;
    if (how == SHUT_WR || how == SHUT_RDWR) {
        u->wr_shut = true;
        if (u->peer && u->type != SOCK_DGRAM) {
            u->peer->peer_wr_shut = true;
            wake_up(&u->peer->wq);
        }
    }
    wake_up(&u->wq);
    mutex_unlock(&ulock);
    return 0;
}

static uint32_t u_poll(struct socket *s, struct poll_table *pt)
{
    struct usock *u = s->impl;
    uint32_t ev = 0;

    mutex_lock(&ulock);
    if (u->rq || (u->state == US_LISTENING && u->backlog))
        ev |= POLLIN;
    if (u->state != US_LISTENING && at_eof(u))
        ev |= POLLIN | POLLHUP;
    if (u->type == SOCK_DGRAM) {
        if (!u->peer || u->peer->rq_bytes < QUEUE_LIMIT)
            ev |= POLLOUT;
    } else if (u->state == US_CONNECTED && !u->wr_shut && u->peer->rq_bytes < QUEUE_LIMIT) {
        ev |= POLLOUT;
    } else if (u->state == US_DISCONNECTED) {
        ev |= POLLOUT | POLLERR;
    }
    poll_wait(pt, &u->wq);
    mutex_unlock(&ulock);
    return ev;
}

static void u_release(struct socket *s)
{
    struct usock *u = s->impl;
    struct umsg *garbage = NULL;

    if (!u)
        return;
    mutex_lock(&ulock);
    u->released = true;
    if (u->bound) {
        for (struct usock **pp = &bound_list; *pp; pp = &(*pp)->bound_next) {
            if (*pp == u) {
                *pp = u->bound_next;
                break;
            }
        }
        u->bound = false;
    }
    // Connections never accepted are refused.
    while (u->backlog) {
        struct usock *c = u->backlog;
        struct socket *cs = c->sock;

        u->backlog = c->bl_next;
        if (c->peer) {
            struct usock *client = c->peer;

            client->state = US_DISCONNECTED;
            client->peer = NULL;
            c->peer = NULL;
            wake_up(&client->wq);
            unref(c, &garbage);         // the client's pointer to c
            unref(client, &garbage);    // c's pointer to the client
        }
        c->released = true;
        c->sock = NULL;
        unref(c, &garbage);             // the backlog's hold
        unref(c, &garbage);             // the hold its file would have had
        vput(cs->cwd);
        kfree(cs);
    }
    u->backlog_tail = NULL;
    if (u->peer) {
        struct usock *p = u->peer;

        if (u->type != SOCK_DGRAM && p->peer == u) {
            p->state = US_DISCONNECTED;
            p->peer = NULL;
            wake_up(&p->wq);
            unref(u, &garbage);
        }
        u->peer = NULL;
        unref(p, &garbage);
    }
    // Free queued data now rather than when the last reference goes.
    if (u->rq_tail) {
        u->rq_tail->next = garbage;
        garbage = u->rq;
    }
    u->rq = u->rq_tail = NULL;
    u->rq_bytes = 0;
    wake_up(&u->wq);
    s->impl = NULL;
    u->sock = NULL;
    unref(u, &garbage);
    mutex_unlock(&ulock);
    free_garbage(garbage);
}

static int u_getsockopt(struct socket *s, int level, int opt, void *val, uint32_t *len)
{
    struct usock *u = s->impl;

    if (level != SOL_SOCKET || opt != SO_PEERCRED)
        return -ENOPROTOOPT;
    if (*len < sizeof(struct ucred))
        return -EINVAL;
    mutex_lock(&ulock);
    memcpy(val, &u->peer_cred, sizeof(struct ucred));
    mutex_unlock(&ulock);
    *len = sizeof(struct ucred);
    return 0;
}

static int u_getname(struct socket *s, bool peer, void *addr, uint32_t *len)
{
    struct usock *u = s->impl;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };

    mutex_lock(&ulock);
    if (peer) {
        if (!u->peer) {
            mutex_unlock(&ulock);
            return -ENOTCONN;
        }
        memcpy(sa.sun_path, u->peer->name, UNIX_PATH_MAX);
    } else {
        memcpy(sa.sun_path, u->name, UNIX_PATH_MAX);
    }
    mutex_unlock(&ulock);
    *len = MIN(*len, (uint32_t)sizeof(sa));
    memcpy(addr, &sa, *len);
    return 0;
}

static const struct sock_ops unix_ops = {
    .bind = u_bind, .listen = u_listen, .accept = u_accept, .connect = u_connect,
    .send = u_send, .recv = u_recv, .shutdown = u_shutdown, .poll = u_poll,
    .release = u_release, .getsockopt = u_getsockopt, .getname = u_getname,
};
