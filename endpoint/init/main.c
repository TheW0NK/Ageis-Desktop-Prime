#include "aegis.h"

// The first process. It keeps these running:
//  - the privilege helper (/osystem/core/privd), the job scheduler (/osystem/core/crond) and
//    the audio server (/osystem/core/audiod),
//  - the text console's shell (the login prompt shown with Ctrl+Alt+F2),
//  - the display (/osystem/core/compositor, which starts the sign-in screen).
// Either is started again when it exits. If the display keeps failing (no
// screen), the computer stays in text mode.

#define TERMINAL    "/sysapps/terminal"
#define COMPOSITOR  "/osystem/core/compositor"
#define PRIVD       "/osystem/core/privd"
#define CROND       "/osystem/core/crond"
#define AUDIOD      "/osystem/core/audiod"

struct service {
    const char *path;
    char *argv[3];
    int pid;
    int failures;                   // quick exits in a row
    uint64_t started;
    bool disabled;
};

static void start(struct service *s)
{
    s->started = uptime_ms();
    s->pid = spawn(s->path, s->argv, environ);
    if (s->pid < 0) {
        dprintf(STDERR_FILENO, "init: cannot start %s: %s\n", s->path, strerror(errno));
        s->failures++;
    }
}

// The installer leaves the new owner's account in /msc/firstboot; it is
// made here, once, on the installed system's first start.
static void first_boot(void)
{
    char buf[1024], user[64] = "", display[128] = "", hash[200] = "", language[16] = "en";
    bool admin = false;
    int fd = open("/msc/firstboot", O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    for (char *line = buf, *next; line && *line; line = next) {
        char *eq;

        if ((next = strchr(line, '\n')))
            *next++ = 0;
        if (!(eq = strchr(line, '=')))
            continue;
        *eq++ = 0;
        if (!strcmp(line, "user"))
            strlcpy(user, eq, sizeof(user));
        else if (!strcmp(line, "display"))
            strlcpy(display, eq, sizeof(display));
        else if (!strcmp(line, "hash"))
            strlcpy(hash, eq, sizeof(hash));
        else if (!strcmp(line, "admin"))
            admin = atoi(eq) != 0;
        else if (!strcmp(line, "language"))
            strlcpy(language, eq, sizeof(language));
    }
    memset(buf, 0, sizeof(buf));
    if (!*user || !*hash || account_add_hashed(user, display, hash, admin) < 0) {
        dprintf(STDERR_FILENO, "init: first boot: cannot create the account \"%s\": %s\n", user, strerror(errno));
    } else {
        struct user_info u;

        if (user_by_name(user, &u) == 0)
            user_setting_set(&u, "language", language);
        dprintf(STDERR_FILENO, "init: first boot: created the account %s\n", user);
    }
    memset(hash, 0, sizeof(hash));
    unlink("/msc/firstboot");
    sync();
}

static bool has_word(const char *s, const char *w)
{
    size_t n = strlen(w);

    for (const char *p = s; (p = strstr(p, w)); p += n)
        if ((p == s || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ' || p[n] == '\n'))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    struct service services[] = {
        { PRIVD, { "privd", NULL }, -1, 0, 0, false },
        { CROND, { "crond", NULL }, -1, 0, 0, false },
        { AUDIOD, { "audiod", NULL }, -1, 0, 0, false },
        { TERMINAL, { "terminal", NULL }, -1, 0, 0, false },
        { COMPOSITOR, { "compositor", NULL, NULL }, -1, 0, 0, false },
    };
    int n = sizeof(services) / sizeof(services[0]);
    struct aegis_stat st;

    (void)argc;
    (void)argv;
    setenv("PATH", "/sysapps:/osystem/core:/userApps/commands");
    first_boot();
    {
        char cmdline[512] = "";
        int fd = open("/osystem/devices/cmdline", O_RDONLY);

        if (fd >= 0) {
            ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);

            cmdline[n > 0 ? n : 0] = 0;
            close(fd);
        }
        // Recovery (a boot entry with "recovery") and the live system on the
        // install media start their own program instead of the sign-in screen.
        if (has_word(cmdline, "recovery"))
            services[4].argv[1] = "/osystem/core/recovery";
        else if (stat("/msc/live", &st) == 0)
            services[4].argv[1] = "/osystem/core/installer";
        // Safe mode: only what is needed to sign in and fix things.
        if (has_word(cmdline, "safe")) {
            setenv("AEGIS_SAFE_MODE", "1");
            // Safe mode chosen once from recovery: the next start is normal.
            if (stat("/msc/safe-mode-once", &st) == 0) {
                feature_set("bootlog", feature_enabled("bootlog"));
                unlink("/msc/safe-mode-once");
                sync();
            }
            services[1].disabled = services[2].disabled = true;
            dprintf(STDERR_FILENO, "init: safe mode: scheduled jobs and sound are off\n");
        }
    }
    if (!feature_enabled("audio"))
        services[2].disabled = true;
    // No screen to draw on: text mode only (and then the console must run).
    if (stat("/osystem/devices/fb0", &st) < 0)
        services[4].disabled = true;
    else if (!feature_enabled("console"))
        services[3].disabled = true;
    if (!feature_enabled("cron"))
        services[1].disabled = true;
    for (int i = 0; i < n; i++)
        if (!services[i].disabled)
            start(&services[i]);

    for (;;) {
        int status, pid = waitpid(-1, &status, 0);

        // Also reaps orphans that were reparented to init.
        if (pid < 0) {
            msleep(1000);
        }
        for (int i = 0; i < n; i++) {
            struct service *s = &services[i];

            if (s->disabled || (s->pid >= 0 && s->pid != pid))
                continue;
            if (s->pid >= 0 && uptime_ms() - s->started < 5000)
                s->failures++;
            else if (s->pid >= 0)
                s->failures = 0;
            s->pid = -1;
            if (s->failures >= 5) {
                dprintf(STDERR_FILENO, "init: %s keeps failing; not starting it again\n", s->path);
                s->disabled = true;
                continue;
            }
            if (s->failures)
                msleep(1000 * s->failures);
            start(s);
        }
    }
}
