#include "aegis.h"

// update FILE.upd | update --restart FILE.upd | update --check FILE.upd |
// update --status | update --undo [--restart] | update --cancel
//
// Installs a system update: checks the file, keeps it for Aegis Recovery and
// makes the next start go there; Recovery installs it, keeping accounts,
// settings, apps and everyone's files, and starts the updated system. With
// --restart it restarts at once (for routines and scripts); otherwise it
// asks. Installing and undoing need the superuser (elevate update ...).

static int usage(void)
{
    dprintf(STDERR_FILENO, "usage: update [--restart] FILE.upd | --check FILE.upd | --status | "
                           "--undo [--restart] | --cancel\n");
    return 1;
}

static void restart(void)
{
    printf("Restarting into Aegis Recovery...\n");
    sync();
    reboot(REBOOT_RESTART);
    dprintf(STDERR_FILENO, "update: the computer could not be restarted: %s\n", strerror(errno));
}

// Asks on the terminal; nobody there (a routine) means no.
static bool ask_restart(void)
{
    char line[16];

    printf("Restart now? (yes/no) ");
    if (read_line(STDIN_FILENO, line, sizeof(line)) <= 0)
        return false;
    return line[0] == 'y' || line[0] == 'Y';
}

static int status(void)
{
    char ver[32], prev[32];
    struct update_info w;
    int pending = update_pending("", &w);

    system_version("", ver, sizeof(ver));
    printf("Installed version: %s\n", ver);
    if (pending == 1)
        printf("Waiting to install at the next start: %s\n", w.version);
    else if (pending == 2)
        printf("Waiting at the next start: undoing the last update\n");
    if (update_can_undo("", prev, sizeof(prev)))
        printf("The last update can be undone (back to %s): update --undo\n", prev);
    return 0;
}

static bool superuser(void)
{
    if (geteuid() == 0)
        return true;
    dprintf(STDERR_FILENO, "update: this needs the superuser: elevate update ...\n");
    return false;
}

int main(int argc, char **argv)
{
    bool now = false, check = false, undo = false, cancel = false;
    const char *file = NULL;
    char error[300], ver[32];
    struct update_info info;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--restart"))
            now = true;
        else if (!strcmp(argv[i], "--check"))
            check = true;
        else if (!strcmp(argv[i], "--undo"))
            undo = true;
        else if (!strcmp(argv[i], "--cancel"))
            cancel = true;
        else if (!strcmp(argv[i], "--status"))
            return status();
        else if (argv[i][0] == '-')
            return usage();
        else
            file = argv[i];
    }
    if (cancel) {
        if (!superuser())
            return 1;
        if (!update_pending("", NULL)) {
            printf("Nothing is waiting to be installed.\n");
            return 0;
        }
        if (update_cancel() < 0) {
            dprintf(STDERR_FILENO, "update: %s\n", strerror(errno));
            return 1;
        }
        printf("Cancelled: the computer starts normally again.\n");
        return 0;
    }
    if (undo) {
        if (!superuser())
            return 1;
        if (update_schedule_undo(error, sizeof(error)) < 0) {
            dprintf(STDERR_FILENO, "update: %s\n", error);
            return 1;
        }
        printf("The last update will be undone the next time the computer starts.\n");
        if (now || ask_restart())
            restart();
        return 0;
    }
    if (!file)
        return usage();
    printf("Checking %s...\n", file);
    if (update_check(file, &info, error, sizeof(error)) < 0) {
        dprintf(STDERR_FILENO, "update: %s\n", error);
        return 1;
    }
    system_version("", ver, sizeof(ver));
    printf("Aegis %s%s%s%s (%u files, %lu KiB)\n", info.version, *info.build ? ", build " : "", info.build,
           version_compare(info.version, ver) > 0 ? "" : version_compare(info.version, ver) == 0
                                                          ? " - the version already installed"
                                                          : " - older than the version installed",
           info.files, (unsigned long)(info.size / 1024));
    if (*info.description)
        printf("%s\n", info.description);
    printf("Installed now: %s\n", ver);
    if (check) {
        printf("The update file is intact.\n");
        return 0;
    }
    if (!superuser())
        return 1;
    if (update_schedule(file, error, sizeof(error)) < 0) {
        dprintf(STDERR_FILENO, "update: %s\n", error);
        return 1;
    }
    printf("Aegis %s will be installed by Recovery the next time the computer starts.\n"
           "Accounts, settings, apps and everyone's files stay. 'update --cancel' changes your mind.\n",
           info.version);
    if (now || ask_restart())
        restart();
    return 0;
}
