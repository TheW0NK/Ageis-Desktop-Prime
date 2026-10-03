#include "aegis.h"

// Prints the kernel log. With -f, keeps printing new messages.
int main(int argc, char **argv)
{
    bool follow = argc > 1 && !strcmp(argv[1], "-f");
    int fd = open("/osystem/devices/klog", O_RDONLY | (follow ? 0 : O_NONBLOCK));
    char buf[4096];
    ssize_t n;

    if (fd < 0) {
        perror("klog: /osystem/devices/klog");
        return 1;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, n);
    close(fd);
    return 0;
}
