#include "inet.h"
#include "mem.h"
#include "random.h"
#include "string.h"
#include "abi/poll.h"

// TCP (RFC 793 with RFC 6298 retransmission timers, slow start, congestion
// avoidance and fast retransmit). No window scaling, SACK or timestamps.

#define BUF_SIZE        (64 * 1024)
#define DEFAULT_MSS     536
#define RTO_INIT        1000
#define RTO_MIN         200
#define RTO_MAX         60000
#define MAX_RETRIES     12
#define TIME_WAIT_MS    10000
#define FIN_WAIT_2_MS   60000
#define OOO_MAX         64

#define F_FIN   0x01
#define F_SYN   0x02
#define F_RST   0x04
#define F_PSH   0x08
#define F_ACK   0x10

#define SEQ_LT(a, b)    ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b)    ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)    ((int32_t)((a) - (b)) > 0)
#define SEQ_GE(a, b)    ((int32_t)((a) - (b)) >= 0)

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t win, sum, urg;
} __attribute__((packed));

struct ooo {
    struct ooo *next;
    uint32_t seq;
    size_t len;
    uint8_t data[];
};

struct tcb {
    enum tcp_state state;
    struct inet_sock *owner;
    struct tcb *listener;           // for connections not yet accepted
    struct tcb *aq_head, *aq_tail, *aq_next;
    int backlog, pending;
    uint32_t laddr, raddr;
    uint16_t lport, rport;

    uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
    uint8_t *sbuf;
    size_t sstart, slen;            // unacknowledged and unsent bytes from snd_una
    bool fin_queued, fin_sent;
    uint32_t fin_seq;

    uint32_t irs, rcv_nxt;
    uint8_t *rbuf;
    size_t rstart, rlen;
    bool fin_received;
    uint32_t last_wnd;
    struct ooo *ooo;
    int nooo;

    uint16_t mss;
    uint32_t cwnd, ssthresh;
    int dupacks;
    uint64_t rto, rto_at, srtt, rttvar;
    int retries;
    bool timing;
    uint32_t rtt_seq;
    uint64_t rtt_start;
    uint64_t linger_at;             // TIME_WAIT and orphaned FIN_WAIT_2 end
    int error;
    struct tcb *next;
};

static struct tcb *tcbs;

static void wake(struct tcb *t)
{
    if (t->owner)
        inet_wake(t->owner);
    else if (t->listener && t->listener->owner)
        inet_wake(t->listener->owner);
}

struct tcb *tcp_new(struct inet_sock *owner)
{
    struct tcb *t = kzalloc(sizeof(*t));

    if (!t)
        return NULL;
    t->sbuf = kmalloc(BUF_SIZE);
    t->rbuf = kmalloc(BUF_SIZE);
    if (!t->sbuf || !t->rbuf) {
        kfree(t->sbuf);
        kfree(t->rbuf);
        kfree(t);
        return NULL;
    }
    t->owner = owner;
    t->state = TCP_CLOSED;
    t->mss = DEFAULT_MSS;
    t->rto = RTO_INIT;
    t->next = tcbs;
    tcbs = t;
    return t;
}

static void tcb_free(struct tcb *t)
{
    for (struct tcb **pp = &tcbs; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    while (t->ooo) {
        struct ooo *o = t->ooo;
        t->ooo = o->next;
        kfree(o);
    }
    kfree(t->sbuf);
    kfree(t->rbuf);
    kfree(t);
}

enum tcp_state tcp_state(struct tcb *t)
{
    return t->state;
}

void tcp_endpoints(struct tcb *t, uint32_t *laddr, uint16_t *lport, uint32_t *raddr, uint16_t *rport)
{
    *laddr = t->laddr;
    *lport = t->lport;
    *raddr = t->raddr;
    *rport = t->rport;
}

bool tcp_port_in_use(uint32_t laddr, uint16_t lport)
{
    for (struct tcb *t = tcbs; t; t = t->next) {
        if (t->lport == lport && t->state != TCP_CLOSED
            && (!laddr || !t->laddr || t->laddr == laddr))
            return true;
    }
    return false;
}

uint16_t tcp_ephemeral_port(void)
{
    for (int tries = 0; tries < 1000; tries++) {
        uint16_t port = htons(49152 + random_u64() % 16384);

        if (!tcp_port_in_use(0, port))
            return port;
    }
    return 0;
}

static uint16_t our_mss(uint32_t dst)
{
    uint32_t hop;
    struct netif *n = ip_route(dst, &hop);

    return n ? n->mtu - 40 : DEFAULT_MSS;
}

static uint32_t rcv_window(struct tcb *t)
{
    size_t free = BUF_SIZE - t->rlen;

    return MIN(free, (size_t)65535);
}

static int send_segment(struct tcb *t, uint32_t seq, uint8_t flags, size_t data_off, size_t len)
{
    struct pkt *p = pkt_alloc();
    struct tcp_hdr *h;
    size_t opt = (flags & F_SYN) ? 4 : 0;

    if (!p)
        return -ENOBUFS;
    if (len > PKT_SIZE - PKT_HEADROOM) {
        pkt_free(p);
        return -EMSGSIZE;
    }
    // Payload from the send ring, then the header in front of it.
    for (size_t i = 0; i < len; i++)
        p->data[i] = t->sbuf[(t->sstart + data_off + i) % BUF_SIZE];
    p->len = len;
    h = (struct tcp_hdr *)pkt_push(p, sizeof(*h) + opt);
    h->sport = t->lport;
    h->dport = t->rport;
    h->seq = htonl(seq);
    h->ack = (flags & F_ACK) ? htonl(t->rcv_nxt) : 0;
    h->off = ((sizeof(*h) + opt) / 4) << 4;
    h->flags = flags;
    t->last_wnd = rcv_window(t);
    h->win = htons(t->last_wnd);
    h->sum = 0;
    h->urg = 0;
    if (opt) {
        uint8_t *o = (uint8_t *)(h + 1);
        uint16_t mss = our_mss(t->raddr);

        o[0] = 2;
        o[1] = 4;
        o[2] = mss >> 8;
        o[3] = mss & 0xFF;
    }
    h->sum = inet_checksum(h, p->len, inet_pseudo_sum(t->laddr, t->raddr, IPPROTO_TCP, p->len));
    return ip_output(p, t->laddr, t->raddr, IPPROTO_TCP, 64);
}

static void send_rst(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                     uint32_t seq, uint32_t ack, bool with_ack)
{
    struct pkt *p = pkt_alloc();
    struct tcp_hdr *h;

    if (!p)
        return;
    h = (struct tcp_hdr *)pkt_push(p, sizeof(*h));
    memset(h, 0, sizeof(*h));
    h->sport = sport;
    h->dport = dport;
    h->seq = htonl(seq);
    h->ack = htonl(ack);
    h->off = (sizeof(*h) / 4) << 4;
    h->flags = F_RST | (with_ack ? F_ACK : 0);
    h->sum = inet_checksum(h, p->len, inet_pseudo_sum(src, dst, IPPROTO_TCP, p->len));
    ip_output(p, src, dst, IPPROTO_TCP, 64);
}

static void arm_rto(struct tcb *t)
{
    t->rto_at = net_now() + t->rto;
}

// Sends whatever the windows allow: data, then FIN, else a pending ACK.
static void output(struct tcb *t, bool force_ack)
{
    bool sent = false;

    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT
        || t->state == TCP_FIN_WAIT_1 || t->state == TCP_LAST_ACK || t->state == TCP_CLOSING) {
        uint32_t wnd = MIN(t->snd_wnd, t->cwnd);
        size_t off = t->snd_nxt - t->snd_una;

        // The FIN, once sent, sits one sequence number past the data.
        if (t->fin_sent && off > t->slen)
            off = t->slen;
        while (off < t->slen) {
            uint32_t inflight = t->snd_nxt - t->snd_una;
            size_t n = MIN(t->slen - off, (size_t)t->mss);

            if (inflight >= wnd) {
                // Zero window: probe with one byte once nothing is in flight.
                if (t->snd_wnd == 0 && inflight == 0 && !t->rto_at) {
                    send_segment(t, t->snd_nxt, F_ACK | F_PSH, off, 1);
                    t->snd_nxt++;
                    arm_rto(t);
                }
                break;
            }
            n = MIN(n, (size_t)(wnd - inflight));
            if (!t->timing) {
                t->timing = true;
                t->rtt_seq = t->snd_nxt + n;
                t->rtt_start = net_now();
            }
            send_segment(t, t->snd_nxt, F_ACK | F_PSH, off, n);
            t->snd_nxt += n;
            off += n;
            sent = true;
            if (!t->rto_at)
                arm_rto(t);
        }
        if (t->fin_queued && !t->fin_sent && off == t->slen) {
            t->fin_seq = t->snd_nxt;
            send_segment(t, t->snd_nxt, F_FIN | F_ACK, 0, 0);
            t->snd_nxt++;
            t->fin_sent = true;
            sent = true;
            if (!t->rto_at)
                arm_rto(t);
        }
    }
    if (force_ack && !sent && t->state != TCP_SYN_SENT && t->state != TCP_CLOSED
        && t->state != TCP_LISTEN)
        send_segment(t, t->snd_nxt, F_ACK, 0, 0);
}

static void set_error(struct tcb *t, int err)
{
    t->error = err;
    t->state = TCP_CLOSED;
    t->rto_at = 0;
    wake(t);
}

int tcp_connect(struct tcb *t, uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport)
{
    if (t->state != TCP_CLOSED)
        return -EISCONN;
    t->laddr = laddr ? laddr : ip_source_for(raddr);
    if (!t->laddr)
        return -ENETUNREACH;
    t->lport = lport;
    t->raddr = raddr;
    t->rport = rport;
    t->iss = (uint32_t)random_u64();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->snd_wnd = 1;
    t->cwnd = 10 * DEFAULT_MSS;
    t->ssthresh = 65535;
    t->state = TCP_SYN_SENT;
    t->retries = 0;
    send_segment(t, t->iss, F_SYN, 0, 0);
    arm_rto(t);
    return 0;
}

int tcp_listen(struct tcb *t, uint32_t laddr, uint16_t lport, int backlog)
{
    if (t->state != TCP_CLOSED && t->state != TCP_LISTEN)
        return -EINVAL;
    t->laddr = laddr;
    t->lport = lport;
    t->backlog = backlog < 1 ? 1 : MIN(backlog, 128);
    t->state = TCP_LISTEN;
    return 0;
}

struct tcb *tcp_accept(struct tcb *l, struct inet_sock *owner)
{
    struct tcb *c = l->aq_head;

    if (!c)
        return NULL;
    l->aq_head = c->aq_next;
    if (!l->aq_head)
        l->aq_tail = NULL;
    l->pending--;
    c->aq_next = NULL;
    c->listener = NULL;
    c->owner = owner;
    return c;
}

int64_t tcp_send(struct tcb *t, const void *data, size_t len)
{
    size_t room, n;

    if (t->error)
        return -t->error;
    if (t->fin_queued)
        return -EPIPE;
    switch (t->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        break;
    case TCP_SYN_SENT:
    case TCP_SYN_RECEIVED:
        return -EAGAIN;
    default:
        return -EPIPE;
    }
    room = BUF_SIZE - t->slen;
    if (!room)
        return -EAGAIN;
    n = MIN(len, room);
    for (size_t i = 0; i < n; i++)
        t->sbuf[(t->sstart + t->slen + i) % BUF_SIZE] = ((const uint8_t *)data)[i];
    t->slen += n;
    output(t, false);
    return n;
}

int64_t tcp_recv(struct tcb *t, void *buf, size_t len, bool peek)
{
    size_t n = MIN(len, t->rlen);
    uint32_t before = rcv_window(t);

    if (!n) {
        if (t->fin_received || t->state == TCP_CLOSED)
            return t->error ? -t->error : 0;
        if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECEIVED || t->state == TCP_ESTABLISHED
            || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2)
            return -EAGAIN;
        return 0;
    }
    for (size_t i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = t->rbuf[(t->rstart + i) % BUF_SIZE];
    if (!peek) {
        t->rstart = (t->rstart + n) % BUF_SIZE;
        t->rlen -= n;
        // Tell the peer when the window opens up again.
        if (before < t->mss && rcv_window(t) >= t->mss && t->state != TCP_CLOSED)
            output(t, true);
    }
    return n;
}

void tcp_shutdown(struct tcb *t)
{
    if (t->fin_queued)
        return;
    if (t->state == TCP_ESTABLISHED || t->state == TCP_SYN_RECEIVED) {
        t->fin_queued = true;
        t->state = TCP_FIN_WAIT_1;
    } else if (t->state == TCP_CLOSE_WAIT) {
        t->fin_queued = true;
        t->state = TCP_LAST_ACK;
    } else {
        return;
    }
    output(t, false);
}

void tcp_close(struct tcb *t)
{
    t->owner = NULL;
    switch (t->state) {
    case TCP_LISTEN:
        // Connections never accepted are reset.
        for (struct tcb *c = tcbs, *next; c; c = next) {
            next = c->next;
            if (c->listener == t) {
                send_rst(c->laddr, c->lport, c->raddr, c->rport, c->snd_nxt, 0, false);
                tcb_free(c);
            }
        }
        t->state = TCP_CLOSED;
        break;
    case TCP_SYN_SENT:
        t->state = TCP_CLOSED;
        break;
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    case TCP_SYN_RECEIVED:
        if (t->rlen) {
            // Unread data: abort rather than pretend it was delivered.
            send_rst(t->laddr, t->lport, t->raddr, t->rport, t->snd_nxt, t->rcv_nxt, true);
            t->state = TCP_CLOSED;
            break;
        }
        tcp_shutdown(t);
        if (t->state == TCP_FIN_WAIT_1)
            t->linger_at = net_now() + FIN_WAIT_2_MS;
        break;
    default:
        break;
    }
    if (t->state == TCP_CLOSED)
        tcb_free(t);
}

uint32_t tcp_poll(struct tcb *t)
{
    uint32_t ev = 0;

    if (t->state == TCP_LISTEN)
        return t->aq_head ? POLLIN : 0;
    if (t->rlen || t->fin_received)
        ev |= POLLIN;
    if (t->error || t->state == TCP_CLOSED)
        ev |= POLLIN | POLLHUP | (t->error ? POLLERR : 0);
    if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) && !t->fin_queued
        && t->slen < BUF_SIZE)
        ev |= POLLOUT;
    if (t->fin_received && t->fin_queued)
        ev |= POLLHUP;
    return ev;
}

int tcp_take_error(struct tcb *t)
{
    int e = t->error;

    t->error = 0;
    return e;
}

// ---- Input ----

static struct tcb *demux(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport)
{
    struct tcb *listener = NULL;

    for (struct tcb *t = tcbs; t; t = t->next) {
        if (t->state == TCP_CLOSED || t->lport != dport)
            continue;
        if (t->state == TCP_LISTEN) {
            if (!t->laddr || t->laddr == dst)
                listener = t;
            continue;
        }
        if (t->rport == sport && t->raddr == src && t->laddr == dst)
            return t;
    }
    return listener;
}

static void parse_mss(struct tcb *t, const struct tcp_hdr *h, size_t hlen)
{
    const uint8_t *o = (const uint8_t *)(h + 1), *end = (const uint8_t *)h + hlen;

    while (o < end && *o != 0) {
        if (*o == 1) {
            o++;
            continue;
        }
        if (o + 1 >= end || o[1] < 2 || o + o[1] > end)
            break;
        if (o[0] == 2 && o[1] == 4)
            t->mss = MIN((uint16_t)(o[2] << 8 | o[3]), our_mss(t->raddr));
        o += o[1];
    }
}

static void rtt_sample(struct tcb *t, uint64_t rtt)
{
    if (!t->srtt) {
        t->srtt = rtt;
        t->rttvar = rtt / 2;
    } else {
        uint64_t diff = t->srtt > rtt ? t->srtt - rtt : rtt - t->srtt;

        t->rttvar = (3 * t->rttvar + diff) / 4;
        t->srtt = (7 * t->srtt + rtt) / 8;
    }
    t->rto = MAX((uint64_t)RTO_MIN, MIN((uint64_t)RTO_MAX, t->srtt + MAX((uint64_t)10, 4 * t->rttvar)));
}

static void store_data(struct tcb *t, const uint8_t *data, size_t len)
{
    size_t n = MIN(len, BUF_SIZE - t->rlen);

    for (size_t i = 0; i < n; i++)
        t->rbuf[(t->rstart + t->rlen + i) % BUF_SIZE] = data[i];
    t->rlen += n;
    t->rcv_nxt += n;
}

// Moves queued out-of-order segments that are now in order.
static void drain_ooo(struct tcb *t)
{
    bool progress = true;

    while (progress) {
        progress = false;
        for (struct ooo **pp = &t->ooo, *o; (o = *pp);) {
            uint32_t end = o->seq + o->len;

            if (SEQ_LE(end, t->rcv_nxt)) {
                *pp = o->next;
                t->nooo--;
                kfree(o);
                continue;
            }
            if (SEQ_LE(o->seq, t->rcv_nxt)) {
                size_t skip = t->rcv_nxt - o->seq;

                store_data(t, o->data + skip, o->len - skip);
                *pp = o->next;
                t->nooo--;
                kfree(o);
                progress = true;
                continue;
            }
            pp = &o->next;
        }
    }
}

static void queue_ooo(struct tcb *t, uint32_t seq, const uint8_t *data, size_t len)
{
    struct ooo *o;

    if (t->nooo >= OOO_MAX || !(o = kmalloc(sizeof(*o) + len)))
        return;
    o->seq = seq;
    o->len = len;
    memcpy(o->data, data, len);
    o->next = t->ooo;
    t->ooo = o;
    t->nooo++;
}

static void handle_ack(struct tcb *t, uint32_t ack, uint32_t seq, uint16_t win, size_t datalen)
{
    if (SEQ_GT(ack, t->snd_nxt)) {
        output(t, true);
        return;
    }
    if (SEQ_GT(ack, t->snd_una)) {
        uint32_t acked = ack - t->snd_una;
        size_t bytes = MIN((size_t)acked, t->slen);

        t->sstart = (t->sstart + bytes) % BUF_SIZE;
        t->slen -= bytes;
        t->snd_una = ack;
        t->dupacks = 0;
        t->retries = 0;
        if (t->timing && SEQ_GE(ack, t->rtt_seq)) {
            t->timing = false;
            rtt_sample(t, net_now() - t->rtt_start);
        }
        if (t->cwnd < t->ssthresh)
            t->cwnd += t->mss;
        else
            t->cwnd += MAX(1U, (uint32_t)t->mss * t->mss / t->cwnd);
        t->rto_at = t->snd_una == t->snd_nxt ? 0 : net_now() + t->rto;
        wake(t);
    } else if (ack == t->snd_una && !datalen && t->snd_nxt != t->snd_una && win == t->snd_wnd) {
        if (++t->dupacks == 3) {
            uint32_t flight = t->snd_nxt - t->snd_una;

            t->ssthresh = MAX(flight / 2, 2U * t->mss);
            t->cwnd = t->ssthresh;
            if (t->slen)
                send_segment(t, t->snd_una, F_ACK | F_PSH, 0, MIN(t->slen, (size_t)t->mss));
        }
    }
    if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LE(t->snd_wl2, ack))) {
        t->snd_wnd = win;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
    }
}

static bool fin_acked(struct tcb *t)
{
    return t->fin_sent && SEQ_GT(t->snd_una, t->fin_seq);
}

static void accept_child(struct tcb *t)
{
    struct tcb *l = t->listener;

    if (!l)
        return;
    if (l->aq_tail)
        l->aq_tail->aq_next = t;
    else
        l->aq_head = t;
    l->aq_tail = t;
    wake(l);
}

void tcp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst)
{
    struct tcp_hdr *h = (struct tcp_hdr *)p->data;
    size_t hlen, datalen;
    uint32_t seq, ack;
    uint8_t flags;
    struct tcb *t;
    const uint8_t *data;

    (void)nif;
    if (p->len < sizeof(*h) || inet_checksum(p->data, p->len, inet_pseudo_sum(src, dst, IPPROTO_TCP, p->len)))
        goto drop;
    hlen = (h->off >> 4) * 4;
    if (hlen < sizeof(*h) || hlen > p->len)
        goto drop;
    seq = ntohl(h->seq);
    ack = ntohl(h->ack);
    flags = h->flags;
    data = p->data + hlen;
    datalen = p->len - hlen;

    t = demux(src, h->sport, dst, h->dport);
    if (!t) {
        if (!(flags & F_RST)) {
            if (flags & F_ACK)
                send_rst(dst, h->dport, src, h->sport, ack, 0, false);
            else
                send_rst(dst, h->dport, src, h->sport, 0,
                         seq + datalen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0), true);
        }
        goto drop;
    }

    if (t->state == TCP_LISTEN) {
        struct tcb *c;

        if (flags & F_RST)
            goto drop;
        if (flags & F_ACK) {
            send_rst(dst, h->dport, src, h->sport, ack, 0, false);
            goto drop;
        }
        if (!(flags & F_SYN) || t->pending >= t->backlog || !(c = tcp_new(NULL)))
            goto drop;
        c->listener = t;
        t->pending++;
        c->laddr = dst;
        c->lport = h->dport;
        c->raddr = src;
        c->rport = h->sport;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->iss = (uint32_t)random_u64();
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->snd_wnd = ntohs(h->win);
        c->snd_wl1 = seq;
        c->cwnd = 10 * DEFAULT_MSS;
        c->ssthresh = 65535;
        parse_mss(c, h, hlen);
        c->cwnd = 10 * c->mss;
        c->state = TCP_SYN_RECEIVED;
        send_segment(c, c->iss, F_SYN | F_ACK, 0, 0);
        arm_rto(c);
        goto drop;
    }

    if (t->state == TCP_SYN_SENT) {
        if ((flags & F_ACK) && ack != t->iss + 1) {
            if (!(flags & F_RST))
                send_rst(dst, h->dport, src, h->sport, ack, 0, false);
            goto drop;
        }
        if (flags & F_RST) {
            if (flags & F_ACK)
                set_error(t, ECONNREFUSED);
            goto drop;
        }
        if (!(flags & F_SYN))
            goto drop;
        t->irs = seq;
        t->rcv_nxt = seq + 1;
        t->snd_wnd = ntohs(h->win);
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        parse_mss(t, h, hlen);
        t->cwnd = 10 * t->mss;
        if (flags & F_ACK) {
            t->snd_una = ack;
            t->state = TCP_ESTABLISHED;
            t->rto_at = 0;
            t->retries = 0;
            output(t, true);
            wake(t);
        } else {
            t->state = TCP_SYN_RECEIVED;
            send_segment(t, t->iss, F_SYN | F_ACK, 0, 0);
        }
        goto drop;
    }

    // Acceptability: the segment must touch the receive window.
    uint32_t wnd = rcv_window(t);
    uint32_t seg_end = seq + datalen + ((flags & F_FIN) ? 1 : 0);

    if (SEQ_LT(seg_end, t->rcv_nxt) || (datalen == 0 && !(flags & F_FIN) && SEQ_LT(seq, t->rcv_nxt))
        || SEQ_GT(seq, t->rcv_nxt + MAX(wnd, 1U))) {
        if (!(flags & F_RST))
            output(t, true);
        goto drop;
    }
    if (flags & F_RST) {
        if (t->state == TCP_SYN_RECEIVED && t->listener) {
            t->listener->pending--;
            t->listener = NULL;
            t->state = TCP_CLOSED;
        } else {
            set_error(t, t->state == TCP_SYN_RECEIVED ? ECONNREFUSED : ECONNRESET);
        }
        if (!t->owner)
            tcb_free(t);
        goto drop;
    }
    if (flags & F_SYN) {
        // A SYN inside an established connection: reset it.
        send_rst(dst, h->dport, src, h->sport, t->snd_nxt, 0, false);
        set_error(t, ECONNRESET);
        if (!t->owner && !t->listener)
            tcb_free(t);
        goto drop;
    }
    if (!(flags & F_ACK))
        goto drop;

    if (t->state == TCP_SYN_RECEIVED) {
        if (SEQ_LE(ack, t->snd_una) || SEQ_GT(ack, t->snd_nxt)) {
            send_rst(dst, h->dport, src, h->sport, ack, 0, false);
            goto drop;
        }
        t->state = TCP_ESTABLISHED;
        t->snd_una = ack;
        t->snd_wnd = ntohs(h->win);
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        t->rto_at = 0;
        t->retries = 0;
        if (t->listener)
            accept_child(t);
        else
            wake(t);
    } else {
        handle_ack(t, ack, seq, ntohs(h->win), datalen);
    }

    switch (t->state) {
    case TCP_FIN_WAIT_1:
        if (fin_acked(t)) {
            t->state = TCP_FIN_WAIT_2;
            t->rto_at = 0;
            if (!t->owner)
                t->linger_at = net_now() + FIN_WAIT_2_MS;
        }
        break;
    case TCP_CLOSING:
        if (fin_acked(t)) {
            t->state = TCP_TIME_WAIT;
            t->linger_at = net_now() + TIME_WAIT_MS;
        }
        break;
    case TCP_LAST_ACK:
        if (fin_acked(t)) {
            t->state = TCP_CLOSED;
            wake(t);
            if (!t->owner)
                tcb_free(t);
            goto drop;
        }
        break;
    case TCP_TIME_WAIT:
        if (flags & F_FIN)
            output(t, true);
        goto drop;
    default:
        break;
    }

    bool ack_now = false;

    if (datalen && (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1
                    || t->state == TCP_FIN_WAIT_2)) {
        if (SEQ_LE(seq, t->rcv_nxt)) {
            size_t skip = t->rcv_nxt - seq;

            if (skip < datalen) {
                store_data(t, data + skip, datalen - skip);
                drain_ooo(t);
            }
        } else {
            queue_ooo(t, seq, data, datalen);
        }
        ack_now = true;
        wake(t);
    }
    if ((flags & F_FIN) && seq + datalen == t->rcv_nxt && !t->fin_received) {
        t->rcv_nxt++;
        t->fin_received = true;
        ack_now = true;
        switch (t->state) {
        case TCP_ESTABLISHED:
            t->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            t->state = fin_acked(t) ? TCP_TIME_WAIT : TCP_CLOSING;
            if (t->state == TCP_TIME_WAIT)
                t->linger_at = net_now() + TIME_WAIT_MS;
            break;
        case TCP_FIN_WAIT_2:
            t->state = TCP_TIME_WAIT;
            t->linger_at = net_now() + TIME_WAIT_MS;
            break;
        default:
            break;
        }
        wake(t);
    }
    output(t, ack_now);
drop:
    pkt_free(p);
}

void tcp_tick(uint64_t now)
{
    for (struct tcb *t = tcbs, *next; t; t = next) {
        next = t->next;

        if (t->linger_at && now >= t->linger_at
            && (t->state == TCP_TIME_WAIT || (t->state == TCP_FIN_WAIT_2 && !t->owner)
                || (t->state == TCP_FIN_WAIT_1 && !t->owner))) {
            t->state = TCP_CLOSED;
            if (!t->owner) {
                tcb_free(t);
                continue;
            }
        }
        if (!t->rto_at || now < t->rto_at)
            continue;
        if (++t->retries > MAX_RETRIES) {
            if (t->owner || t->listener) {
                if (t->listener) {
                    t->listener->pending--;
                    t->listener = NULL;
                }
                set_error(t, ETIMEDOUT);
            }
            if (!t->owner) {
                tcb_free(t);
                continue;
            }
            continue;
        }
        t->rto = MIN(t->rto * 2, (uint64_t)RTO_MAX);
        t->rto_at = now + t->rto;
        t->timing = false;
        switch (t->state) {
        case TCP_SYN_SENT:
            send_segment(t, t->iss, F_SYN, 0, 0);
            break;
        case TCP_SYN_RECEIVED:
            send_segment(t, t->iss, F_SYN | F_ACK, 0, 0);
            break;
        default: {
            // Go back to the first unacknowledged byte.
            uint32_t flight = t->snd_nxt - t->snd_una;

            t->ssthresh = MAX(flight / 2, 2U * t->mss);
            t->cwnd = t->mss;
            t->snd_nxt = t->snd_una;
            if (t->fin_sent && !fin_acked(t))
                t->fin_sent = false;
            if (t->snd_wnd == 0 && t->slen) {
                // Window probe.
                send_segment(t, t->snd_nxt, F_ACK, 0, 1);
                t->snd_nxt++;
            } else {
                t->rto_at = 0;
                output(t, false);
                if (!t->rto_at)
                    t->rto_at = now + t->rto;
            }
        }
        }
    }
}
