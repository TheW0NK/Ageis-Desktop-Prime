#include "net.h"
#include "inet.h"
#include "mem.h"
#include "random.h"
#include "string.h"

// DHCP client (RFC 2131): discovers an address for each Ethernet interface
// and renews the lease at half its time.

#define DHCP_MAGIC      0x63825363
#define RETRY_MS        2000

enum { DHCP_IDLE, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND };

struct dhcp_msg {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

struct dhcp {
    int state;
    uint32_t xid;
    uint32_t offered, server;
    uint64_t next_send, renew_at;
    int tries;
};

static void send_msg(struct netif *nif, struct dhcp *d, uint8_t type)
{
    struct dhcp_msg *m = kzalloc(sizeof(*m));
    uint8_t *o;
    struct pkt *p;
    struct {
        uint16_t sport, dport, len, sum;
    } __attribute__((packed)) *uh;
    struct {
        uint8_t vhl, tos;
        uint16_t len, id, frag;
        uint8_t ttl, proto;
        uint16_t sum;
        uint32_t src, dst;
    } __attribute__((packed)) *ih;
    size_t len;
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    if (!m)
        return;
    m->op = 1;
    m->htype = 1;
    m->hlen = 6;
    m->xid = d->xid;
    m->flags = htons(0x8000);       // ask for broadcast replies
    if (d->state == DHCP_BOUND)
        m->ciaddr = nif->addr;
    memcpy(m->chaddr, nif->mac, 6);
    m->magic = htonl(DHCP_MAGIC);
    o = m->options;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (type == 3 && d->state != DHCP_BOUND) {
        *o++ = 50; *o++ = 4; memcpy(o, &d->offered, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, &d->server, 4); o += 4;
    }
    *o++ = 12; *o++ = 5; memcpy(o, "aegis", 5); o += 5;
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;      // mask, router, DNS
    *o++ = 255;
    len = sizeof(*m) - sizeof(m->options) + (o - m->options);

    // Built by hand: the interface may have no address to send from yet.
    if (!(p = pkt_alloc())) {
        kfree(m);
        return;
    }
    memcpy(p->data, m, len);
    p->len = len;
    kfree(m);
    uh = (void *)pkt_push(p, sizeof(*uh));
    uh->sport = htons(68);
    uh->dport = htons(67);
    uh->len = htons(p->len);
    uh->sum = 0;
    ih = (void *)pkt_push(p, sizeof(*ih));
    memset(ih, 0, sizeof(*ih));
    ih->vhl = 0x45;
    ih->len = htons(p->len);
    ih->ttl = 64;
    ih->proto = IPPROTO_UDP;
    ih->src = d->state == DHCP_BOUND ? nif->addr : 0;
    ih->dst = INADDR_BROADCAST;
    ih->sum = inet_checksum(ih, sizeof(*ih), 0);
    uh->sum = inet_checksum(uh, ntohs(uh->len), inet_pseudo_sum(ih->src, ih->dst, IPPROTO_UDP, ntohs(uh->len)));
    ether_output(nif, p, broadcast, ETH_P_IP);
}

void dhcp_start(struct netif *nif)
{
    struct dhcp *d = nif->dhcp;

    if (!d && !(d = nif->dhcp = kzalloc(sizeof(*d))))
        return;
    d->state = DHCP_SELECTING;
    d->xid = (uint32_t)random_u64();
    d->tries = 0;
    d->next_send = 0;
}

static const uint8_t *option(const struct dhcp_msg *m, size_t len, uint8_t code, uint8_t *olen)
{
    const uint8_t *o = m->options, *end = (const uint8_t *)m + len;

    while (o < end && *o != 255) {
        if (*o == 0) {
            o++;
            continue;
        }
        if (o + 2 > end || o + 2 + o[1] > end)
            break;
        if (*o == code) {
            *olen = o[1];
            return o + 2;
        }
        o += 2 + o[1];
    }
    return NULL;
}

static unsigned prefix_len(uint32_t mask)
{
    unsigned n = 0;

    for (uint32_t m = mask; m; m &= m - 1)
        n++;
    return n;
}

bool dhcp_input(struct netif *nif, struct pkt *p, uint32_t src)
{
    struct dhcp *d = nif->dhcp;
    const struct dhcp_msg *m = (const void *)(p->data + 8);
    size_t len = p->len - 8;
    const uint8_t *v;
    uint8_t olen, type;

    (void)src;
    if (!d || len < sizeof(*m) - sizeof(m->options) || m->op != 2 || m->xid != d->xid
        || ntohl(m->magic) != DHCP_MAGIC || memcmp(m->chaddr, nif->mac, 6))
        return false;
    if (!(v = option(m, len, 53, &olen)) || olen != 1) {
        pkt_free(p);
        return true;
    }
    type = *v;
    if (type == 2 && d->state == DHCP_SELECTING) {
        // Offer: request it.
        d->offered = m->yiaddr;
        d->server = (v = option(m, len, 54, &olen)) && olen == 4 ? *(const uint32_t *)v : m->siaddr;
        d->state = DHCP_REQUESTING;
        d->tries = 0;
        send_msg(nif, d, 3);
        d->next_send = net_now() + RETRY_MS;
    } else if (type == 5 && (d->state == DHCP_REQUESTING || d->state == DHCP_BOUND)) {
        uint32_t lease = 3600, mask = 0, router = 0;

        if ((v = option(m, len, 51, &olen)) && olen == 4)
            lease = ntohl(*(const uint32_t *)v);
        if ((v = option(m, len, 1, &olen)) && olen == 4)
            mask = *(const uint32_t *)v;
        if ((v = option(m, len, 3, &olen)) && olen >= 4)
            router = *(const uint32_t *)v;
        nif->dns[0] = nif->dns[1] = 0;
        if ((v = option(m, len, 6, &olen)) && olen >= 4) {
            nif->dns[0] = *(const uint32_t *)v;
            if (olen >= 8)
                nif->dns[1] = *(const uint32_t *)(v + 4);
        }
        bool changed = nif->addr != m->yiaddr;

        nif->addr = m->yiaddr;
        nif->netmask = mask ? mask : htonl(0xFFFFFF00);
        nif->gateway = router;
        nif->flags |= NETIF_DHCP | NETIF_UP;
        d->state = DHCP_BOUND;
        d->renew_at = net_now() + (uint64_t)MAX(lease / 2, 30U) * 1000;
        if (changed) {
            uint32_t a = ntohl(nif->addr), g = ntohl(router);

            kprintf("net: %s %u.%u.%u.%u/%u via %u.%u.%u.%u (DHCP)\n", nif->name, a >> 24, (a >> 16) & 255,
                    (a >> 8) & 255, a & 255, prefix_len(nif->netmask), g >> 24, (g >> 16) & 255,
                    (g >> 8) & 255, g & 255);
        }
    } else if (type == 6) {
        // NAK: start over.
        nif->addr = 0;
        dhcp_start(nif);
    }
    pkt_free(p);
    return true;
}

void dhcp_tick(uint64_t now)
{
    for (struct netif *nif = netifs; nif; nif = nif->next) {
        struct dhcp *d = nif->dhcp;

        if (!d || !(nif->flags & NETIF_LINK))
            continue;
        if (d->state == DHCP_BOUND) {
            if (now >= d->renew_at) {
                d->xid = (uint32_t)random_u64();
                send_msg(nif, d, 3);
                d->renew_at = now + 60000;
            }
            continue;
        }
        if (now < d->next_send)
            continue;
        if (d->state == DHCP_REQUESTING && ++d->tries > 4) {
            dhcp_start(nif);
            continue;
        }
        send_msg(nif, d, d->state == DHCP_SELECTING ? 1 : 3);
        d->next_send = now + RETRY_MS + MIN(d->tries, 5) * 1000;
    }
}
