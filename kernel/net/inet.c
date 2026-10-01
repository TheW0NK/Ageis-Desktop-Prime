#include "inet.h"
#include "mem.h"
#include "process.h"
#include "random.h"
#include "string.h"
#include "abi/poll.h"

// AF_INET sockets over TCP, UDP and ICMP echo ("ping" datagram sockets).

#define DGRAM_QUEUE_MAX (256 * 1024)

static struct inet_sock *dgram_socks;
static const struct sock_ops inet_ops;

void inet_wake(struct inet_sock *is)
{
    wake_up(&is->wq);
}

// Sleeps until woken or the timeout passes; called and returns with net_lock.
static int inet_wait(struct inet_sock *is, bool nonblock, uint64_t deadline)
{
    struct wait_entry e = { sched_current(), NULL };
    uint64_t now = net_now();

    if (nonblock)
        return -EAGAIN;
    if (signal_pending())
        return -EINTR;
    if (deadline && now >= deadline)
        return -EAGAIN;
    wait_queue_add(&is->wq, &e);
    mutex_unlock(&net_lock);
    sched_block_timeout(deadline ? deadline - now : UINT64_MAX);
    wait_queue_remove(&is->wq, &e);
    mutex_lock(&net_lock);
    return 0;
}

static uint64_t deadline_for(uint64_t timeout)
{
    return timeout ? net_now() + timeout : 0;
}

static bool dgram_port_in_use(int proto, uint32_t addr, uint16_t port, struct inet_sock *except)
{
    for (struct inet_sock *o = dgram_socks; o; o = o->next) {
        if (o != except && o->proto == proto && o->bound && o->lport == port
            && (!addr || !o->laddr || o->laddr == addr))
            return true;
    }
    return false;
}

static uint16_t dgram_ephemeral(int proto)
{
    for (int tries = 0; tries < 1000; tries++) {
        uint16_t port = htons(49152 + random_u64() % 16384);

        if (!dgram_port_in_use(proto, 0, port, NULL))
            return port;
    }
    return 0;
}

static void enqueue(struct inet_sock *is, uint32_t src, uint16_t sport, const uint8_t *data, size_t len)
{
    struct dgram *d;

    if (is->shut_rd || is->rq_bytes + len > DGRAM_QUEUE_MAX || !(d = kmalloc(sizeof(*d) + len)))
        return;
    d->next = NULL;
    d->src = src;
    d->sport = sport;
    d->len = len;
    memcpy(d->data, data, len);
    if (is->rq_tail)
        is->rq_tail->next = d;
    else
        is->rq = d;
    is->rq_tail = d;
    is->rq_bytes += len;
    inet_wake(is);
}

void inet_udp_input(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                    const uint8_t *data, size_t len, struct netif *nif)
{
    bool bcast = ip_is_broadcast(nif, dst) || (ntohl(dst) >> 28) == 0xE;

    for (struct inet_sock *is = dgram_socks; is; is = is->next) {
        if (is->proto != IPPROTO_UDP || !is->bound || is->lport != dport)
            continue;
        if (is->laddr && is->laddr != dst && !bcast)
            continue;
        if (is->connected && (is->raddr != src || is->rport != sport))
            continue;
        enqueue(is, src, sport, data, len);
        if (!bcast)
            return;
    }
}

// Echo replies go to the ping socket whose port is the identifier.
void inet_icmp_reply(struct pkt *p, uint32_t src)
{
    uint16_t id = p->len >= 8 ? *(uint16_t *)(p->data + 4) : 0;

    for (struct inet_sock *is = dgram_socks; is; is = is->next) {
        if (is->proto == IPPROTO_ICMP && is->bound && is->lport == id) {
            enqueue(is, src, 0, p->data, p->len);
            break;
        }
    }
    pkt_free(p);
}

static int addr_in(const void *addr, uint32_t len, uint32_t *ip, uint16_t *port)
{
    const struct sockaddr_in *sa = addr;

    if (len < sizeof(*sa) || sa->sin_family != AF_INET)
        return -EINVAL;
    *ip = sa->sin_addr.s_addr;
    *port = sa->sin_port;
    return 0;
}

static void addr_out(void *addr, uint32_t *len, uint32_t ip, uint16_t port)
{
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = port, .sin_addr = { ip } };

    *len = MIN(*len, (uint32_t)sizeof(sa));
    memcpy(addr, &sa, *len);
}

static int i_bind(struct socket *s, const void *addr, uint32_t len)
{
    struct inet_sock *is = s->impl;
    uint32_t ip;
    uint16_t port;
    int ret;

    if ((ret = addr_in(addr, len, &ip, &port)))
        return ret;
    mutex_lock(&net_lock);
    if (is->bound) {
        ret = -EINVAL;
    } else if (ip && !ip_is_local(ip)) {
        ret = -EADDRNOTAVAIL;
    } else if (port && ntohs(port) < 1024 && s->fs_cred.euid != 0) {
        ret = -EACCES;
    } else if (is->type == SOCK_STREAM) {
        if (!port)
            port = tcp_ephemeral_port();
        else if (tcp_port_in_use(ip, port) && !is->reuseaddr)
            ret = -EADDRINUSE;
    } else {
        if (!port)
            port = is->proto == IPPROTO_ICMP ? htons(random_u64() & 0xFFFF) : dgram_ephemeral(is->proto);
        else if (dgram_port_in_use(is->proto, ip, port, is) && !is->reuseaddr)
            ret = -EADDRINUSE;
    }
    if (!ret) {
        is->laddr = ip;
        is->lport = port;
        is->bound = true;
    }
    mutex_unlock(&net_lock);
    return ret;
}

static int i_listen(struct socket *s, int backlog)
{
    struct inet_sock *is = s->impl;
    int ret;

    if (is->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    mutex_lock(&net_lock);
    if (!is->bound) {
        is->lport = tcp_ephemeral_port();
        is->bound = true;
    }
    ret = tcp_listen(is->tcb, is->laddr, is->lport, backlog);
    mutex_unlock(&net_lock);
    return ret;
}

static int i_accept(struct socket *s, bool nonblock, struct socket **out)
{
    struct inet_sock *is = s->impl, *cis;
    struct socket *cs;
    struct tcb *c;
    uint64_t deadline = deadline_for(is->rcvtimeo);
    int ret;

    if (is->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (!(cs = socket_alloc(AF_INET, SOCK_STREAM)) || !(cis = kzalloc(sizeof(*cis)))) {
        kfree(cs);
        return -ENOMEM;
    }
    mutex_lock(&net_lock);
    for (;;) {
        wait_prepare();
        if (tcp_state(is->tcb) != TCP_LISTEN) {
            ret = -EINVAL;
            break;
        }
        if ((c = tcp_accept(is->tcb, cis))) {
            ret = 0;
            break;
        }
        if ((ret = inet_wait(is, nonblock, deadline)))
            break;
    }
    if (ret) {
        mutex_unlock(&net_lock);
        kfree(cis);
        vput(cs->cwd);
        kfree(cs);
        return ret;
    }
    cis->sock = cs;
    cis->type = SOCK_STREAM;
    cis->proto = IPPROTO_TCP;
    cis->wq = (struct wait_queue)WAIT_QUEUE_INIT;
    cis->tcb = c;
    cis->bound = cis->connected = true;
    tcp_endpoints(c, &cis->laddr, &cis->lport, &cis->raddr, &cis->rport);
    cs->impl = cis;
    cs->ops = &inet_ops;
    mutex_unlock(&net_lock);
    *out = cs;
    return 0;
}

static int i_connect(struct socket *s, const void *addr, uint32_t len, bool nonblock)
{
    struct inet_sock *is = s->impl;
    uint64_t deadline = deadline_for(is->sndtimeo);
    uint32_t ip;
    uint16_t port;
    int ret;

    if ((ret = addr_in(addr, len, &ip, &port)))
        return ret;
    mutex_lock(&net_lock);
    if (is->type != SOCK_STREAM) {
        // Datagram sockets just remember the peer.
        if (!is->bound) {
            is->lport = dgram_ephemeral(is->proto);
            is->bound = true;
        }
        is->raddr = ip;
        is->rport = port;
        is->connected = ip != 0;
        if (!is->laddr && ip)
            is->laddr = ip_source_for(ip);
        mutex_unlock(&net_lock);
        return 0;
    }
    if (is->connected || tcp_state(is->tcb) != TCP_CLOSED) {
        enum tcp_state st = tcp_state(is->tcb);

        mutex_unlock(&net_lock);
        return st == TCP_SYN_SENT ? -EALREADY : -EISCONN;
    }
    if (!is->bound) {
        is->lport = tcp_ephemeral_port();
        is->bound = true;
    }
    if (!port || !ip) {
        mutex_unlock(&net_lock);
        return -EINVAL;
    }
    if ((ret = tcp_connect(is->tcb, is->laddr, is->lport, ip, port))) {
        mutex_unlock(&net_lock);
        return ret;
    }
    tcp_endpoints(is->tcb, &is->laddr, &is->lport, &is->raddr, &is->rport);
    for (;;) {
        enum tcp_state st;

        wait_prepare();
        st = tcp_state(is->tcb);
        if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) {
            is->connected = true;
            ret = 0;
            break;
        }
        if (st == TCP_CLOSED) {
            ret = -tcp_take_error(is->tcb);
            if (!ret)
                ret = -ECONNREFUSED;
            break;
        }
        if (nonblock) {
            ret = -EINPROGRESS;
            break;
        }
        if ((ret = inet_wait(is, false, deadline))) {
            if (ret == -EAGAIN)
                ret = -ETIMEDOUT;
            break;
        }
    }
    mutex_unlock(&net_lock);
    return ret;
}

static int64_t i_send(struct socket *s, struct kmsg *m, bool nonblock)
{
    struct inet_sock *is = s->impl;
    uint64_t deadline = deadline_for(is->sndtimeo);
    int64_t ret = 0;
    size_t sent = 0;

    if (m->nfds)
        return -EOPNOTSUPP;
    mutex_lock(&net_lock);
    if (is->type == SOCK_STREAM) {
        for (;;) {
            int64_t n;

            wait_prepare();
            if (is->shut_wr) {
                ret = -EPIPE;
                break;
            }
            n = tcp_send(is->tcb, (uint8_t *)m->data + sent, m->len - sent);
            if (n > 0) {
                sent += n;
                if (sent == m->len)
                    break;
                continue;
            }
            if (n != -EAGAIN) {
                ret = n;
                break;
            }
            if (sent && nonblock)
                break;
            if ((ret = inet_wait(is, nonblock, deadline)))
                break;
        }
        mutex_unlock(&net_lock);
        if (ret == -EPIPE && !(m->flags & MSG_NOSIGNAL) && !sent)
            signal_thread(sched_current(), SIGPIPE);
        return sent ? (int64_t)sent : ret;
    }

    uint32_t dst = is->raddr;
    uint16_t dport = is->rport;

    if (m->addrlen) {
        if ((ret = addr_in(m->addr, m->addrlen, &dst, &dport))) {
            mutex_unlock(&net_lock);
            return ret;
        }
    } else if (!is->connected) {
        mutex_unlock(&net_lock);
        return -EDESTADDRREQ;
    }
    if (!is->bound) {
        is->lport = is->proto == IPPROTO_ICMP ? htons(random_u64() & 0xFFFF) : dgram_ephemeral(is->proto);
        is->bound = true;
    }
    if (dst == INADDR_BROADCAST && !is->broadcast) {
        mutex_unlock(&net_lock);
        return -EACCES;
    }
    if (is->proto == IPPROTO_UDP) {
        ret = udp_output(is->laddr, is->lport, dst, dport, m->data, m->len);
    } else {
        // ICMP echo: the caller supplies type, code and payload; the kernel
        // fills in the identifier and checksum.
        struct pkt *p = pkt_alloc();

        if (m->len < 8 || m->len > 1472 || ((uint8_t *)m->data)[0] != 8) {
            pkt_free(p);
            ret = -EINVAL;
        } else if (!p) {
            ret = -ENOBUFS;
        } else {
            memcpy(p->data, m->data, m->len);
            p->len = m->len;
            *(uint16_t *)(p->data + 4) = is->lport;
            *(uint16_t *)(p->data + 2) = 0;
            *(uint16_t *)(p->data + 2) = inet_checksum(p->data, p->len, 0);
            ret = ip_output(p, is->laddr, dst, IPPROTO_ICMP, 64);
        }
    }
    mutex_unlock(&net_lock);
    return ret < 0 ? ret : (int64_t)m->len;
}

static int64_t i_recv(struct socket *s, struct kmsg *m, bool nonblock)
{
    struct inet_sock *is = s->impl;
    uint64_t deadline = deadline_for(is->rcvtimeo);
    bool peek = m->flags & MSG_PEEK;
    int64_t ret;

    m->nfds = 0;
    m->addrlen = 0;
    m->flags &= ~MSG_TRUNC;
    mutex_lock(&net_lock);
    for (;;) {
        wait_prepare();
        if (is->type == SOCK_STREAM) {
            ret = tcp_recv(is->tcb, m->data, m->len, peek);
            if (ret != -EAGAIN)
                break;
        } else if (is->rq) {
            struct dgram *d = is->rq;

            ret = MIN(m->len, d->len);
            memcpy(m->data, d->data, ret);
            if ((size_t)ret < d->len)
                m->flags |= MSG_TRUNC;
            struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = d->sport, .sin_addr = { d->src } };
            memcpy(m->addr, &sa, sizeof(sa));
            m->addrlen = sizeof(sa);
            if (!peek) {
                is->rq = d->next;
                if (!is->rq)
                    is->rq_tail = NULL;
                is->rq_bytes -= d->len;
                kfree(d);
            }
            break;
        } else if (is->shut_rd) {
            ret = 0;
            break;
        }
        if ((ret = inet_wait(is, nonblock, deadline)))
            break;
    }
    mutex_unlock(&net_lock);
    return ret;
}

static int i_shutdown(struct socket *s, int how)
{
    struct inet_sock *is = s->impl;

    mutex_lock(&net_lock);
    if (how == SHUT_RD || how == SHUT_RDWR)
        is->shut_rd = true;
    if (how == SHUT_WR || how == SHUT_RDWR) {
        is->shut_wr = true;
        if (is->tcb)
            tcp_shutdown(is->tcb);
    }
    inet_wake(is);
    mutex_unlock(&net_lock);
    return 0;
}

static uint32_t i_poll(struct socket *s, struct poll_table *pt)
{
    struct inet_sock *is = s->impl;
    uint32_t ev;

    mutex_lock(&net_lock);
    if (is->type == SOCK_STREAM)
        ev = tcp_poll(is->tcb);
    else
        ev = POLLOUT | (is->rq || is->shut_rd ? POLLIN : 0);
    poll_wait(pt, &is->wq);
    mutex_unlock(&net_lock);
    return ev;
}

static void i_release(struct socket *s)
{
    struct inet_sock *is = s->impl;

    if (!is)
        return;
    mutex_lock(&net_lock);
    if (is->tcb) {
        tcp_close(is->tcb);
        is->tcb = NULL;
    } else {
        for (struct inet_sock **pp = &dgram_socks; *pp; pp = &(*pp)->next) {
            if (*pp == is) {
                *pp = is->next;
                break;
            }
        }
        while (is->rq) {
            struct dgram *d = is->rq;
            is->rq = d->next;
            kfree(d);
        }
    }
    mutex_unlock(&net_lock);
    s->impl = NULL;
    kfree(is);
}

static int i_getsockopt(struct socket *s, int level, int opt, void *val, uint32_t *len)
{
    struct inet_sock *is = s->impl;
    uint64_t v;

    if (level == IPPROTO_TCP && opt == TCP_NODELAY)
        v = is->nodelay;
    else if (level != SOL_SOCKET)
        return -ENOPROTOOPT;
    else switch (opt) {
    case SO_ERROR:
        mutex_lock(&net_lock);
        v = is->tcb ? (uint64_t)tcp_take_error(is->tcb) : (uint64_t)is->so_error;
        is->so_error = 0;
        mutex_unlock(&net_lock);
        break;
    case SO_REUSEADDR:  v = is->reuseaddr; break;
    case SO_BROADCAST:  v = is->broadcast; break;
    case SO_RCVTIMEO:   v = is->rcvtimeo; break;
    case SO_SNDTIMEO:   v = is->sndtimeo; break;
    case SO_RCVBUF:
    case SO_SNDBUF:     v = 65536; break;
    default:            return -ENOPROTOOPT;
    }
    if (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) {
        *len = MIN(*len, (uint32_t)sizeof(uint64_t));
        memcpy(val, &v, *len);
    } else {
        int32_t i = v;
        *len = MIN(*len, (uint32_t)sizeof(i));
        memcpy(val, &i, *len);
    }
    return 0;
}

static int i_setsockopt(struct socket *s, int level, int opt, const void *val, uint32_t len)
{
    struct inet_sock *is = s->impl;
    uint64_t v = 0;

    memcpy(&v, val, MIN(len, (uint32_t)sizeof(v)));
    if (len == 4)
        v &= 0xFFFFFFFF;
    if (level == IPPROTO_TCP && opt == TCP_NODELAY) {
        is->nodelay = v != 0;
        return 0;
    }
    if (level != SOL_SOCKET)
        return -ENOPROTOOPT;
    switch (opt) {
    case SO_REUSEADDR:  is->reuseaddr = v != 0; return 0;
    case SO_BROADCAST:  is->broadcast = v != 0; return 0;
    case SO_RCVTIMEO:   is->rcvtimeo = v; return 0;
    case SO_SNDTIMEO:   is->sndtimeo = v; return 0;
    case SO_KEEPALIVE:
    case SO_RCVBUF:
    case SO_SNDBUF:     return 0;
    }
    return -ENOPROTOOPT;
}

static int i_getname(struct socket *s, bool peer, void *addr, uint32_t *len)
{
    struct inet_sock *is = s->impl;

    mutex_lock(&net_lock);
    if (is->tcb && tcp_state(is->tcb) != TCP_CLOSED)
        tcp_endpoints(is->tcb, &is->laddr, &is->lport, &is->raddr, &is->rport);
    if (peer && !is->connected) {
        mutex_unlock(&net_lock);
        return -ENOTCONN;
    }
    addr_out(addr, len, peer ? is->raddr : is->laddr, peer ? is->rport : is->lport);
    mutex_unlock(&net_lock);
    return 0;
}

static const struct sock_ops inet_ops = {
    .bind = i_bind, .listen = i_listen, .accept = i_accept, .connect = i_connect,
    .send = i_send, .recv = i_recv, .shutdown = i_shutdown, .poll = i_poll,
    .release = i_release, .getsockopt = i_getsockopt, .setsockopt = i_setsockopt,
    .getname = i_getname,
};

int inet_create(struct socket *s, int type, int protocol)
{
    struct inet_sock *is;

    if (type == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP))
        protocol = IPPROTO_TCP;
    else if (type == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP))
        protocol = IPPROTO_UDP;
    else if (!(type == SOCK_DGRAM && protocol == IPPROTO_ICMP))
        return -EPROTONOSUPPORT;
    if (!(is = kzalloc(sizeof(*is))))
        return -ENOMEM;
    is->sock = s;
    is->type = type;
    is->proto = protocol;
    is->wq = (struct wait_queue)WAIT_QUEUE_INIT;
    mutex_lock(&net_lock);
    if (type == SOCK_STREAM) {
        if (!(is->tcb = tcp_new(is))) {
            mutex_unlock(&net_lock);
            kfree(is);
            return -ENOMEM;
        }
    } else {
        is->next = dgram_socks;
        dgram_socks = is;
    }
    mutex_unlock(&net_lock);
    s->impl = is;
    s->ops = &inet_ops;
    return 0;
}
