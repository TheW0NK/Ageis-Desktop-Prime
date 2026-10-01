#ifndef AEGIS_ABI_NET_H
#define AEGIS_ABI_NET_H

#include <stdint.h>

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define INADDR_ANY      0x00000000U
#define INADDR_BROADCAST 0xFFFFFFFFU
#define INADDR_LOOPBACK 0x7F000001U     // in host byte order

// Addresses and ports in sockaddr_in are in network byte order.
struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t zero[8];
};

// Socket options (level SOL_SOCKET unless noted).
#define SO_REUSEADDR    2
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20      // uint64_t milliseconds, 0 = none
#define SO_SNDTIMEO     21
#define TCP_NODELAY     1       // level IPPROTO_TCP

// Interface information (SYS_NETCONFIG).
#define NETIF_UP        0x1
#define NETIF_LOOPBACK  0x2
#define NETIF_DHCP      0x4     // configured by DHCP
#define NETIF_LINK      0x8     // link is up

struct aegis_netif {
    char name[16];
    uint8_t mac[6];
    uint16_t mtu;
    uint32_t flags;
    uint32_t addr, netmask, gateway;    // network byte order
    uint32_t dns[2];
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_errors;
};

#define NETCONFIG_GET   0       // index, struct aegis_netif *: fills it, -ENODEV past the end
#define NETCONFIG_SET   1       // index, struct aegis_netif *: static address (root)
#define NETCONFIG_DHCP  2       // index: restart DHCP (root)

#endif
