#include "aegis.h"

// netbench [MB]: TCP throughput over loopback, then UDP round trips.

static int lfd;
static size_t total_mb = 32;

static void *receiver(void *arg)
{
    static char buf[65536];
    int c = accept(lfd, NULL, NULL);
    size_t got = 0;
    ssize_t n;

    (void)arg;
    while ((n = recv(c, buf, sizeof(buf), 0)) > 0)
        got += n;
    close(c);
    return (void *)got;
}

int main(int argc, char **argv)
{
    static char buf[65536];
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(9000), .sin_addr = { htonl(INADDR_LOOPBACK) } };
    thread_t t;
    void *got;
    int c;

    if (argc > 1)
        total_mb = atoi(argv[1]);
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(lfd, &sa, sizeof(sa)) < 0 || listen(lfd, 1) < 0) {
        perror("netbench: listen");
        return 1;
    }
    thread_create(&t, receiver, NULL);
    c = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(c, &sa, sizeof(sa)) < 0) {
        perror("netbench: connect");
        return 1;
    }
    uint64_t start = uptime_ms();
    for (size_t sent = 0; sent < total_mb << 20;) {
        ssize_t n = send(c, buf, sizeof(buf), 0);
        if (n <= 0) {
            perror("netbench: send");
            break;
        }
        sent += n;
    }
    close(c);
    thread_join(t, &got);
    uint64_t ms = uptime_ms() - start;
    printf("tcp loopback: %lu bytes in %lu ms (%lu KB/s)\n", (size_t)got, ms, ms ? (size_t)got / ms : 0);
    close(lfd);
    return 0;
}
