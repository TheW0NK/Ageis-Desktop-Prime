#include "commands.h"

static int power(int cmd, const char *name)
{
    if (geteuid() != 0) {
        dprintf(STDERR_FILENO, "%s: must be root (try: sudo %s)\n", name, name);
        return 1;
    }
    printf("%s...\n", cmd == REBOOT_RESTART ? "Restarting" : "Powering off");
    sync();
    reboot(cmd);
    perror(name);
    return 1;
}

int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return power(REBOOT_RESTART, "reboot");
}

int cmd_shutdown(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return power(REBOOT_POWEROFF, "shutdown");
}

int cmd_resolution(int argc, char **argv)
{
    unsigned long w, h;
    char *end;

    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: resolution WIDTHxHEIGHT\n");
        return 1;
    }
    w = strtoul(argv[1], &end, 10);
    if (*end != 'x' || !w) {
        dprintf(STDERR_FILENO, "resolution: expected WIDTHxHEIGHT\n");
        return 1;
    }
    h = strtoul(end + 1, &end, 10);
    if (*end || !h || w > 0xFFFF || h > 0xFFFF) {
        dprintf(STDERR_FILENO, "resolution: expected WIDTHxHEIGHT\n");
        return 1;
    }
    if (ioctl(STDOUT_FILENO, IOCTL_DISPLAY_MODE, w << 16 | h) < 0) {
        perror("resolution");
        return 1;
    }
    return 0;
}
