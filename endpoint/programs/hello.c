#include "aegis.h"

int main(int argc, char **argv)
{
    printf("Hello from an Aegis program (pid %d, uid %u, euid %u)\n", getpid(), getuid(), geteuid());
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);
    return 0;
}
