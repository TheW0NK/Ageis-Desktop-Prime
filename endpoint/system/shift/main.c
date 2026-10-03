#include "aegis.h"

// Starts a shift: a signed-in user's desktop. The greeter runs this as root with pipes for
// standard input and output:
//
//   greeter -> shift:  <user>\n<password>\n
//   shift -> greeter:  ok <uid>\n   or   fail <reason>\n
//   greeter -> shift:  go\n         (once the display accepts the user)
//
// The shift then becomes the user, prepares their folders and runs the
// desktop until it exits. The exit status tells the greeter what to do
// next: 0 sign out, 10 shut down, 11 restart.

#define DESKTOP "/osystem/core/desktop"

static void reply(const char *fmt, const char *arg)
{
    dprintf(STDOUT_FILENO, fmt, arg);
}

static void redirect_output(const struct user_info *u)
{
    char path[256];
    int fd;

    // The desktop's messages go to the user's shift log.
    user_path(u, "system/shift.log", path, sizeof(path));
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
        fd = open("/osystem/devices/nothing", O_WRONLY);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    if ((fd = open("/osystem/devices/nothing", O_RDONLY)) >= 0) {
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
}

int main(int argc, char **argv)
{
    bool automatic = argc > 1 && !strcmp(argv[1], "--auto");
    char name[64], pass[256], go[16];
    struct user_info u;
    char *desktop_argv[] = { "desktop", NULL };
    char theme[32], lang[16];
    int pid, status;

    if (read_line(STDIN_FILENO, name, sizeof(name)) <= 0
        || read_line(STDIN_FILENO, pass, sizeof(pass)) < 0) {
        reply("fail %s\n", "input");
        return 1;
    }
    if (user_by_name(name, &u) < 0 || u.uid == 0) {
        memset(pass, 0, sizeof(pass));
        msleep(1000);
        reply("fail %s\n", "password");
        return 1;
    }
    // While still root: make sure the user's folders exist and are theirs.
    user_setup_dirs(&u);
    if (automatic) {
        // Only when an administrator switched automatic sign-in on.
        if (!feature_enabled("autologin") || !account_is_admin(name) || become(u.uid) < 0) {
            reply("fail %s\n", "auto");
            return 1;
        }
        syslog("shift", "signed in %s automatically", name);
    } else if (login(name, pass) < 0) {
        memset(pass, 0, sizeof(pass));
        reply("fail %s\n", "password");
        return 1;
    } else {
        // The password also unlocks the user's stored passwords.
        char key[65];

        if (cred_unlock(&u, pass, key, sizeof(key)) == 0)
            setenv("AEGIS_CRED_KEY", key);
        else
            syslog("shift", "the credential store of %s could not be unlocked", name);
        memset(key, 0, sizeof(key));
    }
    memset(pass, 0, sizeof(pass));
    dprintf(STDOUT_FILENO, "ok %u\n", u.uid);
    if (read_line(STDIN_FILENO, go, sizeof(go)) <= 0 || strcmp(go, "go"))
        return 1;

    setenv("HOME", u.home);
    setenv("USER", u.name);
    setenv("PATH", "/sysapps:/osystem/core:/userApps/commands");
    if (user_setting_get(&u, "theme", theme, sizeof(theme)) > 0)
        setenv("AEGIS_THEME", theme);
    if (user_setting_get(&u, "language", lang, sizeof(lang)) > 0)
        setenv("LANG", lang);
    if (chdir(u.home) < 0)
        chdir("/");
    redirect_output(&u);
    if ((pid = spawn(DESKTOP, desktop_argv, environ)) < 0) {
        dprintf(STDERR_FILENO, "shift: cannot start %s: %s\n", DESKTOP, strerror(errno));
        return 1;
    }
    while (waitpid(pid, &status, 0) != pid)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 0;
}
