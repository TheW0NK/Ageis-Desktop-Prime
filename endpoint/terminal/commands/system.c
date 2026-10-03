#include "commands.h"

static int power(int cmd, const char *name)
{
    if (geteuid() != 0) {
        dprintf(STDERR_FILENO, "%s: must be the superuser (try: elevate %s)\n", name, name);
        return 1;
    }
    printf("%s...\n", cmd == REBOOT_RESTART ? "Restarting" : "Powering off");
    sync();
    reboot(cmd);
    perror(name);
    return 1;
}

// crash: stops the computer with the crash screen, to see what it looks
// like (root only). Unsaved work is lost.
int cmd_crash(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (geteuid() != 0) {
        dprintf(STDERR_FILENO, "crash: must be the superuser (try: elevate crash)\n");
        return 1;
    }
    sync();
    reboot(REBOOT_CRASH);
    perror("crash");
    return 1;
}

// reboot [--firmware]: --firmware restarts into the UEFI setup screen.
int cmd_reboot(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--firmware"))
        return power(REBOOT_FIRMWARE, "restart");
    return power(REBOOT_RESTART, "restart");
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

// tasks [-a]: the running threads (yours, or everyone's with -a).
int cmd_tasks(int argc, char **argv)
{
    static struct aegis_procinfo info[512];
    static const char *const states[] = { "running", "waiting", "stopped", "ended" };
    bool all = argc > 1 && !strcmp(argv[1], "-a");
    int n = procinfo(info, 512);

    if (n < 0) {
        perror("tasks");
        return 1;
    }
    printf("%6s  %-10s %-8s %8s %7s  %s\n", "ID", "ACCOUNT", "STATE", "MEMORY", "STRANDS", "NAME");
    for (int i = 0; i < n; i++) {
        struct aegis_procinfo *p = &info[i];
        struct user_info u;
        char who[16];

        if (!all && p->uid != getuid())
            continue;
        if (user_by_uid(p->uid, &u) == 0)
            snprintf(who, sizeof(who), "%s", u.name);
        else
            snprintf(who, sizeof(who), "%u", p->uid);
        printf("%6d  %-10s %-8s %7luK %7u  %s\n", p->pid, who, p->state < 4 ? states[p->state] : "?",
               (unsigned long)(p->memory >> 10), p->threads, p->name);
    }
    return 0;
}
