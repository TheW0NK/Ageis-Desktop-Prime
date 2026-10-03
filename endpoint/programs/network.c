#include "aegis.h"

// network                         list interfaces
// network IF ADDR/BITS [gw GW] [dns DNS]   set a static address (superuser)
// network IF dhcp                 configure by DHCP again (superuser)

static void show(struct aegis_netif *n)
{
    char a[16], m[16], g[16], d0[16], d1[16];

    printf("%s: %s%s%s mtu %u\n", n->name, (n->flags & NETIF_UP) ? "up" : "down",
           (n->flags & NETIF_LINK) ? "" : " (no link)", (n->flags & NETIF_DHCP) ? " dhcp" : "", n->mtu);
    if (!(n->flags & NETIF_LOOPBACK))
        printf("    ether %02x:%02x:%02x:%02x:%02x:%02x\n", n->mac[0], n->mac[1], n->mac[2],
               n->mac[3], n->mac[4], n->mac[5]);
    printf("    inet %s netmask %s", inet_format(n->addr, a), inet_format(n->netmask, m));
    if (n->gateway)
        printf(" gateway %s", inet_format(n->gateway, g));
    printf("\n");
    if (n->dns[0])
        printf("    dns %s%s%s\n", inet_format(n->dns[0], d0), n->dns[1] ? " " : "",
               n->dns[1] ? inet_format(n->dns[1], d1) : "");
    printf("    rx %lu packets %lu bytes (%lu dropped), tx %lu packets %lu bytes (%lu errors)\n",
           n->rx_packets, n->rx_bytes, n->rx_dropped, n->tx_packets, n->tx_bytes, n->tx_errors);
}

static int find(const char *name, struct aegis_netif *out)
{
    for (int i = 0; netconfig(NETCONFIG_GET, i, out) == 0; i++) {
        if (!strcmp(out->name, name))
            return i;
    }
    return -1;
}

int main(int argc, char **argv)
{
    struct aegis_netif n;
    int index;

    if (argc == 1) {
        for (int i = 0; netconfig(NETCONFIG_GET, i, &n) == 0; i++)
            show(&n);
        return 0;
    }
    if ((index = find(argv[1], &n)) < 0) {
        dprintf(STDERR_FILENO, "network: %s: no such interface\n", argv[1]);
        return 1;
    }
    if (argc == 2) {
        show(&n);
        return 0;
    }
    if (!strcmp(argv[2], "dhcp")) {
        if (netconfig(NETCONFIG_DHCP, index, &n) < 0) {
            perror("network");
            return 1;
        }
        return 0;
    }
    char *slash = strchr(argv[2], '/');
    int bits = slash ? atoi(slash + 1) : 24;

    if (slash)
        *slash = 0;
    if (!inet_parse(argv[2], &n.addr) || bits < 0 || bits > 32) {
        dprintf(STDERR_FILENO, "network: bad address %s\n", argv[2]);
        return 1;
    }
    n.netmask = htonl(bits ? 0xFFFFFFFFU << (32 - bits) : 0);
    n.gateway = 0;
    n.dns[0] = n.dns[1] = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "gw"))
            inet_parse(argv[i + 1], &n.gateway);
        else if (!strcmp(argv[i], "dns"))
            inet_parse(argv[i + 1], &n.dns[0]);
    }
    n.flags |= NETIF_UP;
    if (netconfig(NETCONFIG_SET, index, &n) < 0) {
        perror("network");
        return 1;
    }
    return 0;
}
