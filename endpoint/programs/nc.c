#include "aegis.h"

// nc HOST PORT      connect and relay standard input and output
// nc -l PORT        accept one connection and relay
// nc -u HOST PORT   send standard input as UDP datagrams

int main(int argc, char **argv)
{
    bool listen_mode = argc == 3 && !strcmp(argv[1], "-l");
    bool udp = argc == 4 && !strcmp(argv[1], "-u");
    int fd;
    char buf[4096];

    if (listen_mode) {
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[2])) };
        int one = 1, lfd = socket(AF_INET, SOCK_STREAM, 0);

        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(lfd, &sa, sizeof(sa)) < 0 || listen(lfd, 1) < 0) {
            perror("nc: listen");
            return 1;
        }
        if ((fd = accept(lfd, NULL, NULL)) < 0) {
            perror("nc: accept");
            return 1;
        }
        close(lfd);
    } else if (udp) {
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[3])) };

        if (resolve_host(argv[2], &sa.sin_addr.s_addr) < 0 || (fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0
            || connect(fd, &sa, sizeof(sa)) < 0) {
            perror("nc");
            return 1;
        }
    } else if (argc == 3) {
        if ((fd = tcp_connect(argv[1], atoi(argv[2]), 10000)) < 0) {
            perror("nc: connect");
            return 1;
        }
    } else {
        dprintf(STDERR_FILENO, "usage: nc HOST PORT | nc -l PORT | nc -u HOST PORT\n");
        return 2;
    }

    struct pollfd p[2] = { { STDIN_FILENO, POLLIN, 0 }, { fd, POLLIN, 0 } };
    bool in_open = true;

    for (;;) {
        ssize_t n;

        if (poll(p, 2, -1) < 0)
            break;
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if ((n = recv(fd, buf, sizeof(buf), 0)) <= 0)
                break;
            write(STDOUT_FILENO, buf, n);
        }
        if (in_open && (p[0].revents & (POLLIN | POLLHUP))) {
            if ((n = read(STDIN_FILENO, buf, sizeof(buf))) <= 0) {
                in_open = false;
                p[0].fd = -1;
                if (!udp)
                    shutdown(fd, SHUT_WR);
                if (udp)
                    break;
            } else if (send(fd, buf, n, 0) < 0) {
                break;
            }
        }
    }
    close(fd);
    return 0;
}
