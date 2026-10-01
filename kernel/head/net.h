#ifndef AEGIS_NET_H
#define AEGIS_NET_H

#include "kernel.h"
#include "sync.h"
#include "abi/errno.h"
#include "abi/net.h"

// Network stack. All protocol state is guarded by net_lock, a mutex held by
// the network thread while it handles packets and timers and by socket calls.

#define PKT_HEADROOM    192
#define PKT_SIZE        (PKT_HEADROOM + 2048)
#define ETH_HLEN        14
#define ETH_MTU         1500

#define ETH_P_IP        0x0800
#define ETH_P_ARP       0x0806
#define ETH_P_IPV6      0x86DD

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

struct netif;

struct pkt {
    struct pkt *next;
    struct netif *nif;
    uint8_t *data;
    size_t len;
    uint8_t buf[PKT_SIZE];
};

struct netif {
    int index;
    char name[16];
    uint8_t mac[6];
    uint16_t mtu;
    uint32_t flags;                 // NETIF_*
    uint32_t addr, netmask, gateway;    // network byte order
    uint32_t dns[2];
    // Sends a complete frame and frees it. Called with net_lock held.
    int (*xmit)(struct netif *nif, struct pkt *p);
    void *driver;
    void *dhcp;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_errors;
    struct netif *next;
};

extern struct mutex net_lock;
extern struct netif *netifs;

struct pkt *pkt_alloc(void);
void pkt_free(struct pkt *p);
uint8_t *pkt_push(struct pkt *p, size_t n);
uint8_t *pkt_pull(struct pkt *p, size_t n);

void net_init(void);
// Adds an interface; it starts DHCP once its link is up.
void netif_register(struct netif *nif);
// Hands a received frame to the stack. Safe in interrupt context.
void netif_rx(struct netif *nif, struct pkt *p);
void netif_link_changed(struct netif *nif, bool up);
struct netif *netif_by_index(int index);
uint64_t net_now(void);

uint16_t inet_checksum(const void *data, size_t len, uint32_t initial);
uint32_t inet_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

// Ethernet and ARP.
void ether_input(struct netif *nif, struct pkt *p);
int ether_output(struct netif *nif, struct pkt *p, const uint8_t *dst, uint16_t type);
void arp_input(struct netif *nif, struct pkt *p);
// Resolves next_hop and sends the IP packet in p (which it then owns).
int arp_resolve_send(struct netif *nif, uint32_t next_hop, struct pkt *p);
void arp_tick(uint64_t now);

// IPv4. Addresses in network byte order.
void ip_input(struct netif *nif, struct pkt *p);
// Prepends the IP header and sends; src 0 picks the interface address.
int ip_output(struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl);
struct netif *ip_route(uint32_t dst, uint32_t *next_hop);
uint32_t ip_source_for(uint32_t dst);
bool ip_is_local(uint32_t addr);
bool ip_is_broadcast(struct netif *nif, uint32_t addr);

void icmp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst);
void udp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst);
void tcp_input(struct netif *nif, struct pkt *p, uint32_t src, uint32_t dst);
void tcp_tick(uint64_t now);

void dhcp_start(struct netif *nif);
void dhcp_tick(uint64_t now);
// UDP to port 68 goes here before sockets see it.
bool dhcp_input(struct netif *nif, struct pkt *p, uint32_t src);

struct socket;
int inet_create(struct socket *s, int type, int protocol);

int net_config(int op, int index, struct aegis_netif *info, bool root);

void virtio_net_init(void);
void e1000_init(void);

#endif
