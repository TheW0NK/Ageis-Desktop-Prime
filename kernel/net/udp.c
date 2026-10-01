#include "inet.h"
#include "string.h"

struct udp_hdr {
    uint16_t sport, dport, len, sum;
} __attribute__((packed));

int udp_output(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len)
{
    struct pkt *p;
    struct udp_hdr *h;

    if (len > ETH_MTU - 28)
        return -EMSGSIZE;
    if (!(p = pkt_alloc()))
        return -ENOBUFS;
    memcpy(p->data, data, len);
    p->len = len;
    h = (struct udp_hdr *)pkt_push(p, sizeof(*h));
    h->sport = sport;
    h->dport = dport;
    h->len = htons(p->len);
    h->sum = 0;
    if (!src)
        src = ip_source_for(dst);
    h->sum = inet_checksum(h, p->len, inet_pseudo_sum(src, dst, IPPROTO_UDP, p->len));
    if (!h->sum)
        h->sum = 0xFFFF;
    return ip_output(p, src, dst, IPPROTO_UDP, 64);
}

void udp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst)
{
    struct udp_hdr *h = (struct udp_hdr *)p->data;
    size_t len;

    if (p->len < sizeof(*h) || (len = ntohs(h->len)) < sizeof(*h) || len > p->len)
        goto drop;
    p->len = len;
    if (h->sum && inet_checksum(p->data, len, inet_pseudo_sum(src, dst, IPPROTO_UDP, len)))
        goto drop;
    if (ntohs(h->dport) == 68 && dhcp_input(nif, p, src))
        return;
    inet_udp_input(src, h->sport, dst, h->dport, p->data + sizeof(*h), len - sizeof(*h), nif);
drop:
    pkt_free(p);
}
