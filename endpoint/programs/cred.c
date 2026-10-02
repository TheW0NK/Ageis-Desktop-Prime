#include "aegis.h"

// cred set NAME SECRET | cred get NAME | cred delete NAME: the encrypted
// credential store of the signed-in user.

int main(int argc, char **argv)
{
    if (!cred_unlocked()) {
        dprintf(STDERR_FILENO, "cred: the credential store is locked (sign in with a password)\n");
        return 1;
    }
    if (argc == 4 && !strcmp(argv[1], "set")) {
        if (cred_set(argv[2], argv[3]) < 0) {
            perror("cred: set");
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "get")) {
        char *s = cred_get(argv[2]);

        if (!s) {
            dprintf(STDERR_FILENO, "cred: %s: not found or damaged\n", argv[2]);
            return 1;
        }
        printf("%s\n", s);
        free(s);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "delete"))
        return cred_delete(argv[2]) < 0 ? 1 : 0;
    dprintf(STDERR_FILENO, "usage: cred set NAME SECRET | get NAME | delete NAME\n");
    return 2;
}
