#include "net.h"
#include "mem.h"
#include "string.h"

#define ARP_ENTRIES     64
#define ARP_TTL_MS      (10 * 60 * 1000)
#define ARP_RETRY_MS    1000
#define ARP_TRIES       3
#define ARP_QUEUE_MAX   8

struct ether_hdr {
    uint8_t dst[6], src[6];
    uint16_t type;
} __attribute__((packed));

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct arp_entry {
    struct netif *nif;
    uint32_t ip;
    uint8_t mac[6];
    bool resolved;
    uint64_t expires, next_try;
    int tries;
    struct pkt *queue;              // packets waiting for the reply
    int queued;
};

static struct arp_entry table[ARP_ENTRIES];
static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

int ether_output(struct netif *nif, struct pkt *p, const uint8_t *dst, uint16_t type)
{
    struct ether_hdr *h = (struct ether_hdr *)pkt_push(p, ETH_HLEN);

    if (!h) {
        pkt_free(p);
        return -ENOBUFS;
    }
    memcpy(h->dst, dst, 6);
    memcpy(h->src, nif->mac, 6);
    h->type = htons(type);
    // Ethernet frames are at least 60 bytes before the checksum.
    while (p->len < 60)
        p->data[p->len++] = 0;
    nif->tx_packets++;
    nif->tx_bytes += p->len;
    return nif->xmit(nif, p);
}

void ether_input(struct netif *nif, struct pkt *p)
{
    struct ether_hdr *h = (struct ether_hdr *)pkt_pull(p, ETH_HLEN);

    if (!h) {
        pkt_free(p);
        return;
    }
    if (memcmp(h->dst, nif->mac, 6) && memcmp(h->dst, broadcast, 6) && !(h->dst[0] & 1)) {
        pkt_free(p);
        return;
    }
    switch (ntohs(h->type)) {
    case ETH_P_IP:  ip_input(nif, p); return;
    case ETH_P_ARP: arp_input(nif, p); return;
    }
    pkt_free(p);
}

static void send_arp(struct netif *nif, uint16_t op, const uint8_t *tha, uint32_t tpa)
{
    struct pkt *p = pkt_alloc();
    struct arp_pkt *a;

    if (!p)
        return;
    a = (struct arp_pkt *)pkt_push(p, sizeof(*a));
    a->htype = htons(1);
    a->ptype = htons(ETH_P_IP);
    a->hlen = 6;
    a->plen = 4;
    a->op = htons(op);
    memcpy(a->sha, nif->mac, 6);
    a->spa = nif->addr;
    memcpy(a->tha, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tha, 6);
    a->tpa = tpa;
    ether_output(nif, p, op == 1 ? broadcast : tha, ETH_P_ARP);
}

static struct arp_entry *lookup(struct netif *nif, uint32_t ip)
{
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (table[i].nif == nif && table[i].ip == ip)
            return &table[i];
    }
    return NULL;
}

static void flush_queue(struct arp_entry *e, bool send)
{
    while (e->queue) {
        struct pkt *p = e->queue;

        e->queue = p->next;
        p->next = NULL;
        if (send)
            ether_output(e->nif, p, e->mac, ETH_P_IP);
        else
            pkt_free(p);
    }
    e->queued = 0;
}

static struct arp_entry *slot_for(struct netif *nif, uint32_t ip)
{
    struct arp_entry *e = lookup(nif, ip), *oldest = &table[0];

    if (e)
        return e;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!table[i].nif)
            return &table[i];
        if (table[i].expires < oldest->expires)
            oldest = &table[i];
    }
    flush_queue(oldest, false);
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

static void learn(struct netif *nif, uint32_t ip, const uint8_t *mac, bool create)
{
    struct arp_entry *e = lookup(nif, ip);

    if (!e && !create)
        return;
    if (!e) {
        e = slot_for(nif, ip);
        e->nif = nif;
        e->ip = ip;
    }
    memcpy(e->mac, mac, 6);
    e->resolved = true;
    e->expires = net_now() + ARP_TTL_MS;
    flush_queue(e, true);
}

void arp_input(struct netif *nif, struct pkt *p)
{
    struct arp_pkt *a = (struct arp_pkt *)p->data;
    bool for_us;

    if (p->len < sizeof(*a) || ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_P_IP
        || a->hlen != 6 || a->plen != 4) {
        pkt_free(p);
        return;
    }
    for_us = nif->addr && a->tpa == nif->addr;
    if (a->spa)
        learn(nif, a->spa, a->sha, for_us);
    if (for_us && ntohs(a->op) == 1)
        send_arp(nif, 2, a->sha, a->spa);
    pkt_free(p);
}

int arp_resolve_send(struct netif *nif, uint32_t next_hop, struct pkt *p)
{
    struct arp_entry *e;

    if (ip_is_broadcast(nif, next_hop))
        return ether_output(nif, p, broadcast, ETH_P_IP);
    e = lookup(nif, next_hop);
    if (e && e->resolved && e->expires > net_now())
        return ether_output(nif, p, e->mac, ETH_P_IP);
    if (!e) {
        e = slot_for(nif, next_hop);
        e->nif = nif;
        e->ip = next_hop;
    }
    if (e->queued >= ARP_QUEUE_MAX) {
        pkt_free(p);
        return -ENOBUFS;
    }
    struct pkt **tail = &e->queue;

    while (*tail)
        tail = &(*tail)->next;
    p->next = NULL;
    *tail = p;
    e->queued++;
    if (!e->tries || e->resolved) {
        e->resolved = false;
        e->tries = 1;
        e->expires = net_now() + ARP_TTL_MS;
        e->next_try = net_now() + ARP_RETRY_MS;
        send_arp(nif, 1, NULL, next_hop);
    }
    return 0;
}

void arp_tick(uint64_t now)
{
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &table[i];

        if (!e->nif || e->resolved || !e->tries || now < e->next_try)
            continue;
        if (e->tries >= ARP_TRIES) {
            flush_queue(e, false);
            memset(e, 0, sizeof(*e));
            continue;
        }
        e->tries++;
        e->next_try = now + ARP_RETRY_MS;
        send_arp(e->nif, 1, NULL, e->ip);
    }
}
