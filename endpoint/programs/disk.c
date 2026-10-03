#include "aegis.h"

// disk list | disk mount FSTYPE DEVICE PATH | disk umount PATH
// disk install DISK USER PASSWORD: erase DISK and install Aegis on it.

static void progress(int percent, const char *step, void *u)
{
    static char last[96];

    (void)u;
    if (strcmp(last, step)) {
        strlcpy(last, step, sizeof(last));
        printf("%3d%%  %s\n", percent, step);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "list")) {
        struct disk_info d[16];
        int n = disk_list(d, 16);

        if (n < 0) {
            perror("disk: list");
            return 1;
        }
        for (int i = 0; i < n; i++)
            printf("%-8s %8llu MiB  %s%s\n", d[i].name, (unsigned long long)(d[i].size >> 20), d[i].description,
                   d[i].mounted ? "  (in use)" : "");
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "mount")) {
        if (mount(argv[2], argv[3], argv[4], 0) < 0) {
            perror("disk: mount");
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "umount")) {
        if (umount(argv[2]) < 0) {
            perror("disk: umount");
            return 1;
        }
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "install")) {
        struct install_options o = { argv[2], argv[3], argv[3], argv[4], true, "aegis", "UTC", 0, "en" };
        char error[256];

        if (install_system(&o, progress, NULL, error, sizeof(error)) < 0) {
            dprintf(STDERR_FILENO, "disk: install: %s\n", error);
            return 1;
        }
        printf("disk: installed on %s\n", argv[2]);
        return 0;
    }
    dprintf(STDERR_FILENO, "usage: disk list | mount FSTYPE DEVICE PATH | umount PATH | install DISK USER PASSWORD\n");
    return 1;
}
