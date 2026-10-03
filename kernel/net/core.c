#include "net.h"
#include "apic.h"
#include "mem.h"
#include "sched.h"
#include "string.h"

struct mutex net_lock = MUTEX_INIT;
struct netif *netifs;

static struct {
    spinlock_t lock;
    struct pkt *head, *tail;
    int count;
} rxq;

static struct wait_queue net_wq = WAIT_QUEUE_INIT;
static int next_index, next_net;

#define RX_QUEUE_MAX    1024
#define TICK_MS         10

struct pkt *pkt_alloc(void)
{
    struct pkt *p = kmalloc(sizeof(*p));

    if (p) {
        p->next = NULL;
        p->nif = NULL;
        p->data = p->buf + PKT_HEADROOM;
        p->len = 0;
    }
    return p;
}

void pkt_free(struct pkt *p)
{
    kfree(p);
}

uint8_t *pkt_push(struct pkt *p, size_t n)
{
    if ((size_t)(p->data - p->buf) < n)
        return NULL;
    p->data -= n;
    p->len += n;
    return p->data;
}

uint8_t *pkt_pull(struct pkt *p, size_t n)
{
    uint8_t *old = p->data;

    if (n > p->len)
        return NULL;
    p->data += n;
    p->len -= n;
    return old;
}

uint64_t net_now(void)
{
    return timer_uptime_ms();
}

uint32_t inet_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len)
{
    uint32_t sum = 0;

    sum += (src & 0xFFFF) + (src >> 16);
    sum += (dst & 0xFFFF) + (dst >> 16);
    sum += htons(proto);
    sum += htons(len);
    return sum;
}

// One's complement sum, finished; `initial` carries a pseudo-header sum.
uint16_t inet_checksum(const void *data, size_t len, uint32_t initial)
{
    const uint8_t *b = data;
    uint64_t sum = initial;

    while (len >= 2) {
        sum += (uint16_t)(b[0] | b[1] << 8);
        b += 2;
        len -= 2;
    }
    if (len)
        sum += b[0];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum & 0xFFFF;
}

struct netif *netif_by_index(int index)
{
    for (struct netif *n = netifs; n; n = n->next) {
        if (n->index == index)
            return n;
    }
    return NULL;
}

void netif_register(struct netif *nif)
{
    struct netif **pp;

    mutex_lock(&net_lock);
    nif->index = next_index++;
    if (!nif->name[0])
        ksnprintf(nif->name, sizeof(nif->name), "net%d", next_net++);
    if (!nif->mtu)
        nif->mtu = ETH_MTU;
    for (pp = &netifs; *pp; pp = &(*pp)->next)
        ;
    *pp = nif;
    mutex_unlock(&net_lock);
    if (!(nif->flags & NETIF_LOOPBACK))
        kprintf("net: %s %02x:%02x:%02x:%02x:%02x:%02x\n", nif->name, nif->mac[0], nif->mac[1],
                nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5]);
}

void netif_link_changed(struct netif *nif, bool up)
{
    mutex_lock(&net_lock);
    if (up && !(nif->flags & NETIF_LINK)) {
        nif->flags |= NETIF_LINK | NETIF_UP;
        if (!nif->addr)
            dhcp_start(nif);
    } else if (!up) {
        nif->flags &= ~NETIF_LINK;
    }
    mutex_unlock(&net_lock);
}

void netif_rx(struct netif *nif, struct pkt *p)
{
    uint64_t flags = spin_lock_irqsave(&rxq.lock);

    if (rxq.count >= RX_QUEUE_MAX) {
        spin_unlock_irqrestore(&rxq.lock, flags);
        nif->rx_dropped++;
        pkt_free(p);
        return;
    }
    p->nif = nif;
    p->next = NULL;
    if (rxq.tail)
        rxq.tail->next = p;
    else
        rxq.head = p;
    rxq.tail = p;
    rxq.count++;
    spin_unlock_irqrestore(&rxq.lock, flags);
    wake_up(&net_wq);
}

static struct pkt *rx_take(void)
{
    uint64_t flags = spin_lock_irqsave(&rxq.lock);
    struct pkt *p = rxq.head;

    if (p) {
        rxq.head = p->next;
        if (!rxq.head)
            rxq.tail = NULL;
        rxq.count--;
    }
    spin_unlock_irqrestore(&rxq.lock, flags);
    return p;
}

// ---- Loopback ----

static int loop_xmit(struct netif *nif, struct pkt *p)
{
    // Frames on loopback carry no Ethernet header.
    nif->tx_packets++;
    nif->tx_bytes += p->len;
    netif_rx(nif, p);
    return 0;
}

static struct netif loopback = {
    .name = "lo", .mtu = ETH_MTU, .flags = NETIF_UP | NETIF_LINK | NETIF_LOOPBACK,
    .xmit = loop_xmit,
};

static void deliver(struct pkt *p)
{
    struct netif *nif = p->nif;

    nif->rx_packets++;
    nif->rx_bytes += p->len;
    if (nif->flags & NETIF_LOOPBACK)
        ip_input(nif, p);
    else
        ether_input(nif, p);
}

static void net_thread(void *arg)
{
    uint64_t last_tick = 0;

    (void)arg;
    for (;;) {
        struct pkt *p;
        uint64_t now;
        uint64_t flags;

        wait_prepare();
        mutex_lock(&net_lock);
        for (int n = 0; n < 64 && (p = rx_take()); n++)
            deliver(p);
        now = net_now();
        if (now - last_tick >= TICK_MS) {
            last_tick = now;
            arp_tick(now);
            tcp_tick(now);
            dhcp_tick(now);
        }
        mutex_unlock(&net_lock);

        flags = spin_lock_irqsave(&rxq.lock);
        bool idle = !rxq.head;
        spin_unlock_irqrestore(&rxq.lock, flags);
        if (idle) {
            flags = spin_lock_irqsave(&net_wq.lock);
            wait_queue_sleep_locked_timeout(&net_wq, TICK_MS);
            irq_restore(flags);
        }
    }
}

void net_init(void)
{
    loopback.addr = htonl(INADDR_LOOPBACK);
    loopback.netmask = htonl(0xFF000000);
    netif_register(&loopback);
    if (!thread_create("netd", net_thread, NULL))
        kprintf("net: cannot start the network thread\n");
}

int net_config(int op, int index, struct aegis_netif *info, bool root)
{
    struct netif *nif;
    int ret = 0;

    mutex_lock(&net_lock);
    if (!(nif = netif_by_index(index))) {
        mutex_unlock(&net_lock);
        return -ENODEV;
    }
    switch (op) {
    case NETCONFIG_GET:
        memset(info, 0, sizeof(*info));
        memcpy(info->name, nif->name, sizeof(info->name));
        memcpy(info->mac, nif->mac, 6);
        info->mtu = nif->mtu;
        info->flags = nif->flags;
        info->addr = nif->addr;
        info->netmask = nif->netmask;
        info->gateway = nif->gateway;
        info->dns[0] = nif->dns[0];
        info->dns[1] = nif->dns[1];
        info->rx_packets = nif->rx_packets;
        info->tx_packets = nif->tx_packets;
        info->rx_bytes = nif->rx_bytes;
        info->tx_bytes = nif->tx_bytes;
        info->rx_dropped = nif->rx_dropped;
        info->tx_errors = nif->tx_errors;
        break;
    case NETCONFIG_SET:
        if (!root) {
            ret = -EPERM;
            break;
        }
        nif->addr = info->addr;
        nif->netmask = info->netmask;
        nif->gateway = info->gateway;
        nif->dns[0] = info->dns[0];
        nif->dns[1] = info->dns[1];
        nif->flags &= ~NETIF_DHCP;
        if (info->flags & NETIF_UP)
            nif->flags |= NETIF_UP;
        else
            nif->flags &= ~NETIF_UP;
        break;
    case NETCONFIG_DHCP:
        if (!root) {
            ret = -EPERM;
            break;
        }
        dhcp_start(nif);
        break;
    default:
        ret = -EINVAL;
    }
    mutex_unlock(&net_lock);
    return ret;
}
