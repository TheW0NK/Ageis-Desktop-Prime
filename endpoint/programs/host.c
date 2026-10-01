#include "aegis.h"

// host NAME: looks a host name up.
int main(int argc, char **argv)
{
    uint32_t addr;
    char buf[16];

    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: host NAME\n");
        return 2;
    }
    if (resolve_host(argv[1], &addr) < 0) {
        dprintf(STDERR_FILENO, "host: %s: %s\n", argv[1], errno == ENOENT ? "not found" : strerror(errno));
        return 1;
    }
    printf("%s has address %s\n", argv[1], inet_format(addr, buf));
    return 0;
}
