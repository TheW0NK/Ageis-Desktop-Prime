#include "net.h"
#include "string.h"
#include "inet.h"

struct ip_hdr {
    uint8_t vhl, tos;
    uint16_t len, id, frag;
    uint8_t ttl, proto;
    uint16_t sum;
    uint32_t src, dst;
} __attribute__((packed));

struct icmp_hdr {
    uint8_t type, code;
    uint16_t sum;
    uint16_t id, seq;
} __attribute__((packed));

static uint16_t next_id;

bool ip_is_local(uint32_t addr)
{
    if ((ntohl(addr) >> 24) == 127)
        return true;
    for (struct netif *n = netifs; n; n = n->next) {
        if (n->addr && n->addr == addr)
            return true;
    }
    return false;
}

bool ip_is_broadcast(struct netif *nif, uint32_t addr)
{
    if (addr == INADDR_BROADCAST)
        return true;
    return nif && nif->netmask && nif->addr
        && (addr & ~nif->netmask) == ~nif->netmask
        && (addr & nif->netmask) == (nif->addr & nif->netmask);
}

// Chooses the interface for dst and the address to send the frame to.
struct netif *ip_route(uint32_t dst, uint32_t *next_hop)
{
    struct netif *def = NULL;

    if ((ntohl(dst) >> 24) == 127 || ip_is_local(dst)) {
        for (struct netif *n = netifs; n; n = n->next) {
            if (n->flags & NETIF_LOOPBACK) {
                *next_hop = dst;
                return n;
            }
        }
    }
    for (struct netif *n = netifs; n; n = n->next) {
        if (!(n->flags & NETIF_UP) || (n->flags & NETIF_LOOPBACK))
            continue;
        if (dst == INADDR_BROADCAST || (n->netmask && (dst & n->netmask) == (n->addr & n->netmask))) {
            *next_hop = dst;
            return n;
        }
        if (!def && n->gateway)
            def = n;
    }
    if (def) {
        *next_hop = def->gateway;
        return def;
    }
    // Not configured yet (DHCP): broadcasts still leave through the first
    // interface that is up.
    for (struct netif *n = netifs; n; n = n->next) {
        if ((n->flags & NETIF_UP) && !(n->flags & NETIF_LOOPBACK) && dst == INADDR_BROADCAST) {
            *next_hop = dst;
            return n;
        }
    }
    return NULL;
}

uint32_t ip_source_for(uint32_t dst)
{
    uint32_t hop;
    struct netif *n = ip_route(dst, &hop);

    if (n && (n->flags & NETIF_LOOPBACK))
        return (ntohl(dst) >> 24) == 127 ? htonl(INADDR_LOOPBACK) : dst;
    return n ? n->addr : 0;
}

int ip_output(struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
    uint32_t hop;
    struct netif *nif = ip_route(dst, &hop);
    struct ip_hdr *h;

    if (!nif) {
        pkt_free(p);
        return -ENETUNREACH;
    }
    if (p->len + sizeof(*h) > nif->mtu) {
        pkt_free(p);
        return -EMSGSIZE;
    }
    h = (struct ip_hdr *)pkt_push(p, sizeof(*h));
    h->vhl = 0x45;
    h->tos = 0;
    h->len = htons(p->len);
    h->id = htons(next_id++);
    h->frag = htons(0x4000);        // don't fragment
    h->ttl = ttl ? ttl : 64;
    h->proto = proto;
    h->sum = 0;
    h->src = src ? src : ip_source_for(dst);
    h->dst = dst;
    h->sum = inet_checksum(h, sizeof(*h), 0);
    if (nif->flags & NETIF_LOOPBACK)
        return nif->xmit(nif, p);
    return arp_resolve_send(nif, hop, p);
}

void ip_input(struct netif *nif, struct pkt *p)
{
    struct ip_hdr *h = (struct ip_hdr *)p->data;
    size_t hlen, total;

    if (p->len < sizeof(*h) || (h->vhl >> 4) != 4)
        goto drop;
    hlen = (h->vhl & 0xF) * 4;
    total = ntohs(h->len);
    if (hlen < sizeof(*h) || total < hlen || total > p->len || inet_checksum(h, hlen, 0))
        goto drop;
    // Fragments are not reassembled; nothing this stack sends needs them.
    if (ntohs(h->frag) & 0x3FFF)
        goto drop;
    if (!(nif->flags & NETIF_LOOPBACK) && nif->addr && h->dst != nif->addr
        && !ip_is_broadcast(nif, h->dst) && !((ntohl(h->dst) >> 28) == 0xE))
        goto drop;
    p->len = total;

    uint32_t src = h->src, dst = h->dst;
    uint8_t proto = h->proto;

    pkt_pull(p, hlen);
    switch (proto) {
    case IPPROTO_ICMP:  icmp_input(nif, p, src, dst); return;
    case IPPROTO_UDP:   udp_input(nif, p, src, dst); return;
    case IPPROTO_TCP:   tcp_input(nif, p, src, dst); return;
    }
drop:
    pkt_free(p);
}

void icmp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst)
{
    struct icmp_hdr *h = (struct icmp_hdr *)p->data;

    (void)nif;
    if (p->len < sizeof(*h) || inet_checksum(p->data, p->len, 0)) {
        pkt_free(p);
        return;
    }
    if (h->type == 8 && h->code == 0) {
        // Echo request: answer in place.
        h->type = 0;
        h->sum = 0;
        h->sum = inet_checksum(p->data, p->len, 0);
        ip_output(p, ip_is_broadcast(nif, dst) ? 0 : dst, src, IPPROTO_ICMP, 64);
        return;
    }
    if (h->type == 0) {
        inet_icmp_reply(p, src);
        return;
    }
    pkt_free(p);
}
