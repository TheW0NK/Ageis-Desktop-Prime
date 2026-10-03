#include "aegis.h"
#include "ui.h"

// The sign-in screen. The compositor starts it as root; it hands a
// successful sign-in to /osystem/core/session and shows itself again when the
// session ends.

#define SESSION "/osystem/core/session"

static const char page[] =
    "<window role='overlay' padding='0' spacing='0'>"
    "  <vbox expand='1' justify='center' spacing='4'>"
    "    <label id='time' textalign='center' size='huge' bold='true'/>"
    "    <label id='date' textalign='center' size='large' dim='true'/>"
    "    <spacer size='28'/>"
    "    <hbox justify='center'>"
    "      <card width='380' spacing='14'>"
    "        <canvas id='avatar' expand='0' height='104'/>"
    "        <h2 id='name' textalign='center'/>"
    "        <dropdown id='users' hidden='true' onchange='user'/>"
    "        <password id='password' placeholder='Password' onactivate='signin'/>"
    "        <label id='error' textalign='center' text=''/>"
    "        <button id='signin' text='Sign in' default='true' onclick='signin'/>"
    "      </card>"
    "    </hbox>"
    "  </vbox>"
    "  <hbox padding='16' spacing='8'>"
    "    <label id='host' dim='true' align='center'/>"
    "    <spacer/>"
    "    <button text='Restart' flat='true' onclick='restart'/>"
    "    <button text='Shut down' flat='true' onclick='poweroff'/>"
    "  </hbox>"
    "</window>";

static struct ui_window *win;
static struct user_info users[32];
static int nusers, current;
static int session_pid = -1, session_in = -1, session_out = -1;
static char wallpaper[256] = "default";
static struct surface *backdrop_cache;

static void backdrop(struct ui_window *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    if (!backdrop_cache || backdrop_cache->width != r.w || backdrop_cache->height != r.h) {
        struct gfx bg;

        surface_destroy(backdrop_cache);
        if (!(backdrop_cache = surface_create(r.w, r.h)))
            return;
        gfx_init(&bg, backdrop_cache);
        wallpaper_draw(&bg, r, wallpaper);
    }
    gfx_blit(g, backdrop_cache, r, r.x, r.y);
}

static void paint_avatar(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    char pic[256] = "";
    int size = MIN(r.w, r.h);

    (void)w;
    (void)u;
    if (!nusers)
        return;
    user_setting_get(&users[current], "picture", pic, sizeof(pic));
    avatar_draw(g, (struct rect){ r.x + (r.w - size) / 2, r.y, size, size }, users[current].display, pic);
}

static void show_user(int i)
{
    current = i;
    ui_set_text(ui_get(win, "name"), nusers ? users[i].display : "No users");
    ui_redraw(ui_get(win, "avatar"));
    // Each user's wallpaper shows behind their sign-in.
    if (nusers && user_setting_get(&users[i], "background", wallpaper, sizeof(wallpaper)) <= 0)
        strlcpy(wallpaper, "default", sizeof(wallpaper));
    surface_destroy(backdrop_cache);
    backdrop_cache = NULL;
    ui_window_set_backdrop(win, backdrop, NULL);
}

// Accounts may have been added or removed while someone was signed in.
static void load_users(void)
{
    struct widget *list = ui_get(win, "users");

    nusers = user_list(users, 32);
    ui_list_clear(list);
    for (int i = 0; i < nusers; i++)
        ui_list_add(list, users[i].display);
    ui_set_visible(list, nusers > 1);
    if (current >= nusers)
        current = 0;
    if (nusers > 1)
        ui_list_select(list, current);
    show_user(current);
}

static void user_changed(struct widget *w, void *u)
{
    (void)u;
    show_user(ui_list_selected(w));
    ui_focus(ui_get(win, "password"));
}

static void set_error(const char *text)
{
    ui_set_text(ui_get(win, "error"), text);
}

static void set_busy(bool busy)
{
    ui_set_enabled(ui_get(win, "signin"), !busy);
    ui_set_enabled(ui_get(win, "password"), !busy);
    ui_set_enabled(ui_get(win, "users"), !busy);
}

static void close_pipes(void)
{
    if (session_out >= 0) {
        ui_unwatch_fd(session_out);
        close(session_out);
    }
    if (session_in >= 0)
        close(session_in);
    session_in = session_out = -1;
}

static void power(int cmd)
{
    sync();
    if (reboot(cmd) < 0)
        set_error("The computer could not be turned off.");
}

static bool watch_session(void *u)
{
    int status;

    (void)u;
    if (session_pid < 0 || waitpid(session_pid, &status, WNOHANG) != session_pid)
        return true;
    session_pid = -1;
    close_pipes();
    syslog("greeter", "session of %s ended (status %d)", users[current].name,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    {
        struct wm_msg m = { WM_SET_SESSION, 0, -1, 0, 0, 0, 0, 0, { 0 } };

        wm_send(&m);
        wm_setting_changed("theme", "light");
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 10)
        power(REBOOT_POWEROFF);
    else if (WIFEXITED(status) && WEXITSTATUS(status) == 11)
        power(REBOOT_RESTART);
    ui_set_text(ui_get(win, "password"), "");
    set_error("");
    set_busy(false);
    load_users();
    ui_window_show(win);
    ui_focus(ui_get(win, "password"));
    return false;
}

static void session_reply(int fd, void *u)
{
    char line[64];
    ssize_t n = read_line(fd, line, sizeof(line));

    (void)u;
    if (n > 0 && !strncmp(line, "ok ", 3)) {
        struct wm_msg m = { WM_SET_SESSION, 0, atoi(line + 3), 0, 0, 0, 0, 0, { 0 } };

        // Let the user's programs connect, step aside, then start them.
        wm_send(&m);
        ui_window_hide(win);
        ui_set_text(ui_get(win, "password"), "");
        dprintf(session_in, "go\n");
        syslog("greeter", "signed in %s", users[current].name);
        ui_unwatch_fd(fd);
        ui_timer(300, watch_session, NULL);
        return;
    }
    syslog("greeter", "sign-in failed for %s", users[current].name);
    set_error(n > 0 && strstr(line, "password") ? "That password is not right." : "Signing in failed.");
    set_busy(false);
    close_pipes();
    if (session_pid > 0)
        waitpid(session_pid, NULL, 0);
    session_pid = -1;
    ui_set_text(ui_get(win, "password"), "");
    ui_focus(ui_get(win, "password"));
}

static int start_session(bool automatic)
{
    int to[2], from[2], saved_in, saved_out, pid;
    char *argv[] = { "session", automatic ? "--auto" : NULL, NULL };

    if (pipe(to) < 0)
        return -1;
    if (pipe(from) < 0) {
        close(to[0]);
        close(to[1]);
        return -1;
    }
    saved_in = dup(STDIN_FILENO);
    saved_out = dup(STDOUT_FILENO);
    dup2(to[0], STDIN_FILENO);
    dup2(from[1], STDOUT_FILENO);
    pid = spawn(SESSION, argv, environ);
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);
    close(to[0]);
    close(from[1]);
    if (pid < 0) {
        close(to[1]);
        close(from[0]);
        return -1;
    }
    fcntl(to[1], F_SETFD, FD_CLOEXEC);
    fcntl(from[0], F_SETFD, FD_CLOEXEC);
    session_in = to[1];
    session_out = from[0];
    session_pid = pid;
    return 0;
}

static void signin(struct widget *w, void *u)
{
    const char *pass = ui_text(ui_get(win, "password"));

    (void)w;
    (void)u;
    if (!nusers || session_pid > 0)
        return;
    set_error("");
    if (start_session(false) < 0) {
        set_error("Signing in is not possible right now.");
        return;
    }
    set_busy(true);
    dprintf(session_in, "%s\n%s\n", users[current].name, pass);
    ui_watch_fd(session_out, session_reply, NULL);
}

static void restart(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_message(win, "Restart", "Restart the computer now?", "Restart|Cancel") == 0)
        power(REBOOT_RESTART);
}

static void poweroff(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_message(win, "Shut down", "Shut down the computer now?", "Shut down|Cancel") == 0)
        power(REBOOT_POWEROFF);
}

static bool tick(void *u)
{
    int64_t now = time(NULL);
    struct tm tm;
    char buf[64];

    (void)u;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    ui_set_text(ui_get(win, "time"), buf);
    strftime(buf, sizeof(buf), "%A, %B %e", &tm);
    ui_set_text(ui_get(win, "date"), buf);
    return true;
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "signin", signin }, { "user", user_changed }, { "restart", restart }, { "poweroff", poweroff },
        { NULL, NULL },
    };
    struct widget *list;

    ui_set_theme("dark");
    if (!(win = ui_load_string_named(page, handlers, NULL, "greeter")))
        return 1;
    (void)list;
    load_users();
    {
        char host[64];
        int fd = open("/msc/hostname", O_RDONLY);

        if (fd >= 0 && read_line(fd, host, sizeof(host)) > 0)
            ui_set_text(ui_get(win, "host"), host);
        if (fd >= 0)
            close(fd);
    }
    ui_canvas_set(ui_get(win, "avatar"), paint_avatar, NULL, NULL);
    tick(NULL);
    ui_timer(1000, tick, NULL);
    ui_window_show(win);
    ui_focus(ui_get(win, "password"));
    syslog("greeter", "ready");
    // Automatic sign-in, once per boot, for the first administrator.
    if (feature_enabled("autologin")) {
        for (int i = 0; i < nusers; i++) {
            if (account_is_admin(users[i].name) && start_session(true) == 0) {
                current = i;
                set_busy(true);
                dprintf(session_in, "%s\n\n", users[i].name);
                ui_watch_fd(session_out, session_reply, NULL);
                break;
            }
        }
    }
    // The greeter never closes itself: the display ends with it.
    while (wm_connected())
        ui_run();
    return 0;
}
