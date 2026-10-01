#include "commands.h"

int cmd_run(int argc, char **argv)
{
    int ret;

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: run PROGRAM [args...]\n");
        return 1;
    }
    set_raw(true);
    ret = run_external(argc - 1, argv + 1);
    set_raw(false);
    return ret;
}
