#include "commands.h"

int cmd_sudo(int argc, char **argv)
{
    char prompt[64], pass[256];
    int ret;

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: elevate COMMAND [args...]\n");
        return 1;
    }
    if (geteuid() != 0) {
        snprintf(prompt, sizeof(prompt), "[elevate] password for %s: ", user_name);
        set_raw(true);
        ssize_t n = read_secret(prompt, pass, sizeof(pass));
        set_raw(false);
        if (n < 0)
            return 1;
        ret = sudo(pass);
        memset(pass, 0, sizeof(pass));
        if (ret < 0) {
            if (errno == EPERM)
                dprintf(STDERR_FILENO, "elevate: %s is not an administrator\n", user_name);
            else
                dprintf(STDERR_FILENO, "elevate: incorrect password\n");
            return 1;
        }
    }

    set_raw(true);
    ret = run_args(argc - 1, argv + 1);
    set_raw(false);
    seteuid(getuid());
    return ret;
}
