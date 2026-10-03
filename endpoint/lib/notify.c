#include "aegis.h"

// Notifications: one SOCK_SEQPACKET message "app\ntitle\nbody" to the
// desktop of the signed-in user, which shows it and keeps it in a list.

int notify_user(uint32_t uid, const char *app, const char *title, const char *body)
{
    char name[48], msg[1024];
    int fd, n;

    snprintf(name, sizeof(name), "@aegis/notify-%u", uid);
    if ((fd = unix_connect(name, SOCK_SEQPACKET)) < 0)
        return -1;
    n = snprintf(msg, sizeof(msg), "%s\n%s\n%s", app ? app : "", title ? title : "", body ? body : "");
    n = send(fd, msg, MIN(n, (int)sizeof(msg) - 1), MSG_NOSIGNAL) < 0 ? -1 : 0;
    close(fd);
    return n;
}

int notify(const char *app, const char *title, const char *body)
{
    return notify_user(getuid(), app, title, body);
}
