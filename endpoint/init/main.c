#include "aegis.h"

#define TERMINAL "/bin/terminal"

int main(int argc, char **argv)
{
    char *targv[] = { "terminal", NULL };

    (void)argc;
    (void)argv;
    for (;;) {
        int status, pid = spawn(TERMINAL, targv, environ);

        if (pid < 0) {
            dprintf(STDERR_FILENO, "init: cannot start %s: %s\n", TERMINAL, strerror(errno));
            msleep(5000);
            continue;
        }
        // Also reaps orphans that were reparented to init.
        while (waitpid(-1, &status) != pid)
            ;
    }
}
