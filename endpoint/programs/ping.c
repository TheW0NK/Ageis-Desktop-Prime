#include "aegis.h"

// ping [-c COUNT] HOST
int main(int argc, char **argv)
{
    int count = 4, sent = 0, received = 0, fd;
    const char *host = NULL;
    struct sockaddr_in sa = { .sin_family = AF_INET };
    char ip[16];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            count = atoi(argv[++i]);
        else
            host = argv[i];
    }
    if (!host) {
        dprintf(STDERR_FILENO, "usage: ping [-c COUNT] HOST\n");
        return 2;
    }
    if (resolve_host(host, &sa.sin_addr.s_addr) < 0) {
        dprintf(STDERR_FILENO, "ping: %s: %s\n", host, strerror(errno));
        return 2;
    }
    if ((fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)) < 0) {
        perror("ping: socket");
        return 2;
    }
    printf("PING %s (%s)\n", host, inet_format(sa.sin_addr.s_addr, ip));
    for (int seq = 1; seq <= count; seq++) {
        uint8_t pkt[64] = { 8, 0 }, reply[1500];
        struct pollfd p = { fd, POLLIN, 0 };
        uint64_t start = uptime_ms(), deadline = start + 1000;

        pkt[6] = seq >> 8;
        pkt[7] = seq;
        for (int i = 8; i < 64; i++)
            pkt[i] = i;
        if (sendto(fd, pkt, sizeof(pkt), 0, &sa, sizeof(sa)) < 0) {
            perror("ping: send");
            break;
        }
        sent++;
        for (;;) {
            uint64_t now = uptime_ms();

            if (now >= deadline || poll(&p, 1, deadline - now) != 1) {
                printf("timeout for seq %d\n", seq);
                break;
            }
            ssize_t n = recv(fd, reply, sizeof(reply), 0);
            if (n >= 8 && reply[0] == 0 && (reply[6] << 8 | reply[7]) == seq) {
                printf("%ld bytes from %s: seq=%d time=%lu ms\n", n, ip, seq, uptime_ms() - start);
                received++;
                break;
            }
        }
        if (seq < count)
            msleep(1000 - MIN(uptime_ms() - start, 1000));
    }
    printf("--- %d sent, %d received, %d%% loss\n", sent, received,
           sent ? (sent - received) * 100 / sent : 0);
    return received ? 0 : 1;
}
