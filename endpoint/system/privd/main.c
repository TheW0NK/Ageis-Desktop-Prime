#include "aegis.h"

// The privilege helper. Runs as root (started by init) and does the few
// things any signed-in user may do that need root, after checking who is
// asking (SO_PEERCRED):
//
//   "passwd\n<old>\n<new>"   change your own password
//
// Each request is one SOCK_SEQPACKET message; the reply is "ok" or
// "error <message>". Administrators use sudo for everything else.

#define SOCKET_NAME PRIVD_SOCKET

static void reply(int fd, const char *msg)
{
    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
}

static void handle(int fd)
{
    struct ucred cred;
    uint32_t len = sizeof(cred);
    char req[1024], *fields[3], *p;
    struct user_info u;
    ssize_t n;
    int k = 0;

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
        return;
    if ((n = recv(fd, req, sizeof(req) - 1, 0)) <= 0)
        return;
    req[n] = 0;
    for (p = req; k < 3;) {
        fields[k++] = p;
        if (!(p = strchr(p, '\n')))
            break;
        *p++ = 0;
    }
    if (user_by_uid(cred.uid, &u) < 0) {
        reply(fd, "error unknown user");
        return;
    }
    if (k == 3 && !strcmp(fields[0], "passwd")) {
        if (strlen(fields[2]) < 4) {
            reply(fd, "error The new password is too short.");
        } else if (account_check_password(u.name, fields[1]) < 0) {
            syslog("privd", "wrong current password for %s", u.name);
            msleep(1500);
            reply(fd, "error The current password is not right.");
        } else if (account_set_password(u.name, fields[2]) < 0) {
            reply(fd, "error The password could not be saved.");
        } else {
            syslog("privd", "password changed for %s", u.name);
            reply(fd, "ok");
        }
        memset(req, 0, sizeof(req));
        return;
    }
    reply(fd, "error unknown request");
}

int main(void)
{
    int lfd = unix_listen(SOCKET_NAME, SOCK_SEQPACKET);

    if (lfd < 0) {
        perror("privd: " SOCKET_NAME);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);

        if (fd < 0)
            continue;
        handle(fd);
        close(fd);
    }
}
