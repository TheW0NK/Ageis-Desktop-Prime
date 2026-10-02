#include "aegis.h"

// The first process. It keeps these running:
//  - the privilege helper (/sbin/privd), the job scheduler (/sbin/crond) and
//    the audio server (/sbin/audiod),
//  - the text console's shell (the login prompt shown with Ctrl+Alt+F2),
//  - the display (/sbin/compositor, which starts the sign-in screen).
// Either is started again when it exits. If the display keeps failing (no
// screen), the computer stays in text mode.

#define TERMINAL    "/bin/terminal"
#define COMPOSITOR  "/sbin/compositor"
#define PRIVD       "/sbin/privd"
#define CROND       "/sbin/crond"
#define AUDIOD      "/sbin/audiod"

struct service {
    const char *path;
    char *argv[2];
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

int main(int argc, char **argv)
{
    struct service services[] = {
        { PRIVD, { "privd", NULL }, -1, 0, 0, false },
        { CROND, { "crond", NULL }, -1, 0, 0, false },
        { AUDIOD, { "audiod", NULL }, -1, 0, 0, false },
        { TERMINAL, { "terminal", NULL }, -1, 0, 0, false },
        { COMPOSITOR, { "compositor", NULL }, -1, 0, 0, false },
    };
    int n = sizeof(services) / sizeof(services[0]);
    struct aegis_stat st;

    (void)argc;
    (void)argv;
    setenv("PATH", "/bin:/sbin");
    // No screen to draw on: text mode only (and then the console must run).
    if (stat("/dev/fb0", &st) < 0)
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
