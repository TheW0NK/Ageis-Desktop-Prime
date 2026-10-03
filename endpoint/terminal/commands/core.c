#include "commands.h"

int cmd_help(int argc, char **argv)
{
    if (argc > 1) {
        const struct command *c = find_command(argv[1]);

        if (!c) {
            dprintf(STDERR_FILENO, "help: no such command: %s\n", argv[1]);
            return 1;
        }
        printf("%s %s\n  %s\n", c->name, c->usage, c->help);
        return 0;
    }
    printf("Built-in commands:\n");
    for (size_t i = 0; i < command_count; i++)
        printf("  %-11s %s\n", commands[i].name, commands[i].help);
    printf("Programs in /sysapps can be run by name, or by path with ./NAME or run.\n");
    return 0;
}

int cmd_echo(int argc, char **argv)
{
    bool newline = true;
    int i = 1;

    if (argc > 1 && !strcmp(argv[1], "-n")) {
        newline = false;
        i++;
    }
    for (; i < argc; i++)
        printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    if (newline)
        printf("\n");
    return 0;
}

int cmd_exit(int argc, char **argv)
{
    exit(argc > 1 ? atoi(argv[1]) : 0);
}

int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("\x1b[2J\x1b[H");
    return 0;
}

int cmd_history(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    history_print();
    return 0;
}

int cmd_env(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (char **e = environ; e && *e; e++)
        printf("%s\n", *e);
    return 0;
}

int cmd_export(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (!eq || eq == argv[i]) {
            dprintf(STDERR_FILENO, "export: expected NAME=VALUE: %s\n", argv[i]);
            return 1;
        }
        *eq = '\0';
        setenv(argv[i], eq + 1);
    }
    return 0;
}

int cmd_whoami(int argc, char **argv)
{
    char name[32];

    (void)argc;
    (void)argv;
    printf("%s\n", uid_to_name(geteuid(), name, sizeof(name)));
    return 0;
}

int cmd_id(int argc, char **argv)
{
    char un[32], gn[32];

    (void)argc;
    (void)argv;
    printf("uid=%u(%s) gid=%u(%s)", getuid(), uid_to_name(getuid(), un, sizeof(un)),
           getgid(), gid_to_name(getgid(), gn, sizeof(gn)));
    if (geteuid() != getuid())
        printf(" euid=%u(%s)", geteuid(), uid_to_name(geteuid(), un, sizeof(un)));
    printf("\n");
    return 0;
}

static void civil(int64_t t, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    int64_t days = t / 86400, secs = t % 86400;
    int64_t z = days + 719468, era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;

    *d = doy - (153 * mp + 2) / 5 + 1;
    *mo = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*mo <= 2);
    *h = secs / 3600;
    *mi = secs % 3600 / 60;
    *s = secs % 60;
}

int cmd_date(int argc, char **argv)
{
    int y, mo, d, h, mi, s;

    (void)argc;
    (void)argv;
    civil(time(NULL), &y, &mo, &d, &h, &mi, &s);
    printf("%04d-%02d-%02d %02d:%02d:%02d UTC\n", y, mo, d, h, mi, s);
    return 0;
}

int cmd_uptime(int argc, char **argv)
{
    uint64_t s = uptime_ms() / 1000;

    (void)argc;
    (void)argv;
    printf("up %lu:%02lu:%02lu\n", s / 3600, s / 60 % 60, s % 60);
    return 0;
}

int cmd_uname(int argc, char **argv)
{
    struct aegis_utsname u;

    if (uname(&u) < 0) {
        perror("uname");
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "-a"))
        printf("%s %s %s %s\n", u.sysname, u.release, u.version, u.machine);
    else
        printf("%s\n", u.sysname);
    return 0;
}

static int signal_number(const char *s)
{
    static const struct { const char *name; int sig; } names[] = {
        { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "KILL", SIGKILL },
        { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "TERM", SIGTERM }, { "CONT", SIGCONT },
        { "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "ALRM", SIGALRM }, { "PIPE", SIGPIPE },
    };

    if (isdigit(*s))
        return atoi(s);
    if (!strncmp(s, "SIG", 3))
        s += 3;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(names[i].name, s))
            return names[i].sig;
    }
    return -1;
}

int cmd_kill(int argc, char **argv)
{
    int ret = 0, sig = SIGTERM, i = 1;

    if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        if ((sig = signal_number(argv[1] + 1)) < 0 || sig > NSIG) {
            dprintf(STDERR_FILENO, "kill: unknown signal %s\n", argv[1] + 1);
            return 1;
        }
        i++;
    }
    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: kill [-SIGNAL] PID...\n");
        return 1;
    }
    for (; i < argc; i++) {
        if (kill(atoi(argv[i]), sig) < 0) {
            fail("kill", argv[i]);
            ret = 1;
        }
    }
    return ret;
}

int cmd_sleep(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: sleep SECONDS\n");
        return 1;
    }
    msleep((uint64_t)atoi(argv[1]) * 1000);
    return 0;
}
