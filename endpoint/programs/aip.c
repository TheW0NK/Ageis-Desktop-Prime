#include "aegis.h"

// aip info FILE | aip install FILE [--everyone] [--grant PERMS] | aip list |
// aip remove ID [--everyone] | aip repair ID: Aegis packages from the
// terminal. Installing for everyone needs sudo.

static int usage(void)
{
    dprintf(STDERR_FILENO, "usage: aip info FILE | install FILE [--everyone] [--grant a,b] | list | "
                           "remove ID [--everyone] | repair ID\n");
    return 1;
}

static bool flag(int argc, char **argv, const char *f)
{
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], f))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    char error[256];

    if (argc < 2)
        return usage();
    if ((!strcmp(argv[1], "info") || !strcmp(argv[1], "install")) && argc >= 3) {
        struct aip p;
        const struct aip_permission *req[16];
        int n;

        if (aip_open(argv[2], &p, error, sizeof(error)) < 0) {
            dprintf(STDERR_FILENO, "aip: %s\n", error);
            return 1;
        }
        n = aip_requested(&p, req, 16);
        if (!strcmp(argv[1], "info")) {
            printf("%s %s (%s)\n  by %s\n  %s\n  %u files, %llu KiB\n  permissions:", p.name, p.version, p.id,
                   *p.publisher ? p.publisher : "an unknown publisher", p.description, p.nfiles,
                   (unsigned long long)(p.payload + 1023) / 1024);
            for (int i = 0; i < n; i++)
                printf(" %s (%s)", req[i]->id, aip_tier_name(req[i]->tier));
            printf("%s\n", n ? "" : " none");
            aip_close(&p);
            return 0;
        }
        {
            // Granted: what --grant lists, else only the basic ones asked for.
            char granted[256] = "";
            const char *list = NULL;

            for (int i = 3; i + 1 < argc; i++)
                if (!strcmp(argv[i], "--grant"))
                    list = argv[i + 1];
            for (int i = 0; i < n; i++) {
                bool want = list ? strstr(list, req[i]->id) != NULL : req[i]->tier == AIP_TIER_BASIC;

                if (want) {
                    if (*granted)
                        strlcat(granted, ",", sizeof(granted));
                    strlcat(granted, req[i]->id, sizeof(granted));
                }
            }
            if (aip_install(&p, flag(argc, argv, "--everyone"), granted, error, sizeof(error)) < 0) {
                dprintf(STDERR_FILENO, "aip: %s\n", error);
                aip_close(&p);
                return 1;
            }
            printf("aip: installed %s %s%s\n", p.name, p.version, flag(argc, argv, "--everyone") ? " for everyone" : "");
            aip_close(&p);
            return 0;
        }
    }
    if (!strcmp(argv[1], "list")) {
        struct aip_installed a[64];
        int n = aip_list_installed(a, 64);

        for (int i = 0; i < n; i++)
            printf("%-20s %-10s %-9s %s\n", a[i].id, a[i].version, a[i].everyone ? "everyone" : "you", a[i].name);
        if (!n)
            printf("No packages are installed.\n");
        return 0;
    }
    if (!strcmp(argv[1], "remove") && argc >= 3) {
        if (aip_uninstall(argv[2], flag(argc, argv, "--everyone"), error, sizeof(error)) < 0) {
            dprintf(STDERR_FILENO, "aip: %s\n", error);
            return 1;
        }
        printf("aip: removed %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "repair") && argc >= 3) {
        struct aip_installed a[64];
        int n = aip_list_installed(a, 64);

        for (int i = 0; i < n; i++) {
            if (strcmp(a[i].id, argv[2]))
                continue;
            if (aip_repair(&a[i], error, sizeof(error)) < 0) {
                dprintf(STDERR_FILENO, "aip: %s\n", error);
                return 1;
            }
            printf("aip: repaired %s\n", argv[2]);
            return 0;
        }
        dprintf(STDERR_FILENO, "aip: %s is not installed\n", argv[2]);
        return 1;
    }
    return usage();
}
