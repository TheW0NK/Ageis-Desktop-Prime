#include "desktop.h"

// The taskbar: launcher button, a button per open window, the volume,
// clock, and the session menu.

struct task {
    uint32_t id;
    uint32_t state;
    char title[WM_TEXT_MAX];
    struct widget *button;
};

static struct ui_window *panel;
static struct task tasks[64];
static int ntasks;

static const char page[] =
    "<window role='panel' height='46' padding='5' spacing='0'>"
    "  <hbox expand='1' spacing='6'>"
    "    <button id='start' flat='true' symbol='apps' iconsize='22' text='Apps' onclick='launcher'/>"
    "    <separator/>"
    "    <hbox id='tasks' expand='1' spacing='4'/>"
    "    <button id='bell' flat='true' symbol='bell' iconsize='18'/>"
    "    <button id='volume' flat='true' symbol='volume' iconsize='20'/>"
    "    <button id='clock' flat='true' text='' onclick='clock'/>"
    "    <button id='session' flat='true' symbol='power' iconsize='20' menu='sessionmenu'/>"
    "  </hbox>"
    "  <menu id='sessionmenu'>"
    "    <item id='who' text='' disabled='true'/>"
    "    <separator/>"
    "    <item text='Settings' onclick='settings'/>"
    "    <item text='Lock' shortcut='Super+L' onclick='lock'/>"
    "    <separator/>"
    "    <item text='Sign out' onclick='signout'/>"
    "    <item text='Restart' onclick='restart'/>"
    "    <item text='Shut down' onclick='poweroff'/>"
    "  </menu>"
    "</window>";

void end_session(int code)
{
    ui_quit(code);
}

static void backdrop(struct ui_window *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    gfx_fill(g, r, ALPHA(0x11151D, 0xEE));
    gfx_fill(g, (struct rect){ r.x, r.y, r.w, 1 }, ALPHA(0xFFFFFF, 0x22));
}

static void task_clicked(struct widget *w, void *u)
{
    struct task *t = u;

    (void)w;
    wm_activate(t->id, true);
}

static void update_button(struct task *t)
{
    ui_set_text(t->button, t->title);
    ui_set_value(t->button, (t->state & WM_STATE_FOCUSED) && !(t->state & WM_STATE_MINIMIZED));
}

static struct task *find_task(uint32_t id)
{
    for (int i = 0; i < ntasks; i++)
        if (tasks[i].id == id)
            return &tasks[i];
    return NULL;
}

static void rebind(void)
{
    // Handlers point into the array, which moves when tasks are removed.
    for (int i = 0; i < ntasks; i++)
        ui_set_handler(tasks[i].button, "click", task_clicked, &tasks[i]);
}

static void window_list(struct wm_event *ev, void *u)
{
    struct wm_msg *m = &ev->msg;
    struct task *t;

    (void)u;
    if (ev->type == WM_EV_SETTING) {
        if (!strncmp(m->text, "background=", 11))
            background_reload();
        else if (!strncmp(m->text, "name=", 5) || !strncmp(m->text, "picture=", 8)) {
            char who[128];

            user_current(&me);
            snprintf(who, sizeof(who), "Signed in as %s", me.display);
            ui_set_text(ui_get(panel, "who"), who);
            launcher_refresh_user();
        }
        return;
    }
    if (ev->type != WM_EV_LIST)
        return;
    switch (m->type) {
    case WM_LIST_ADD:
        if (find_task(m->window) || ntasks == 64)
            return;
        t = &tasks[ntasks++];
        memset(t, 0, sizeof(*t));
        t->id = m->window;
        t->state = m->a;
        strlcpy(t->title, m->text, sizeof(t->title));
        t->button = ui_create(panel, "button");
        ui_set_attr(t->button, "flat", "true");
        ui_set_attr(t->button, "width", "200");
        ui_add(ui_get(panel, "tasks"), t->button);
        update_button(t);
        rebind();
        break;
    case WM_LIST_CHANGE:
        if (!(t = find_task(m->window)))
            return;
        t->state = m->a;
        strlcpy(t->title, m->text, sizeof(t->title));
        update_button(t);
        // Only one window has the focus.
        if (t->state & WM_STATE_FOCUSED)
            for (int i = 0; i < ntasks; i++)
                if (&tasks[i] != t && (tasks[i].state & WM_STATE_FOCUSED)) {
                    tasks[i].state &= ~WM_STATE_FOCUSED;
                    update_button(&tasks[i]);
                }
        break;
    case WM_LIST_REMOVE:
        if (!(t = find_task(m->window)))
            return;
        ui_remove(t->button);
        *t = tasks[--ntasks];
        rebind();
        break;
    }
}

static bool tick(void *u)
{
    int64_t now = time(NULL);
    struct tm tm;
    char buf[32];

    (void)u;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    ui_set_text(ui_get(panel, "clock"), buf);
    return true;
}

static void on_launcher(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    launcher_toggle();
}

static void on_clock(struct widget *w, void *u)
{
    struct app_info a;

    (void)w;
    (void)u;
    if (app_find("clock", &a) == 0)
        app_launch(&a, NULL);
}

static void on_settings(struct widget *w, void *u)
{
    struct app_info a;

    (void)w;
    (void)u;
    if (app_find("settings", &a) == 0)
        app_launch(&a, NULL);
}

static void on_signout(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    end_session(EXIT_SIGN_OUT);
}

static void on_restart(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_message(NULL, "Restart", "Restart the computer? Unsaved work in open apps will be lost.",
                   "Restart|Cancel") == 0)
        end_session(EXIT_RESTART);
}

static void on_poweroff(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_message(NULL, "Shut down", "Shut down the computer? Unsaved work in open apps will be lost.",
                   "Shut down|Cancel") == 0)
        end_session(EXIT_POWEROFF);
}

static void on_lock(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    lock_screen();
}

static void panel_key(struct ui_window *w, struct wm_event *ev, void *u)
{
    (void)w;
    (void)u;
    if (lock_active())
        return;
    if (ev->value == 1 && (ev->mods & MOD_META) && ev->key == KEY_A + 'l' - 'a') {
        lock_screen();
        return;
    }
    if (volume_key(ev))
        return;
    // The Meta key on its own opens the launcher.
    if ((ev->key == KEY_LEFTMETA || ev->key == KEY_RIGHTMETA) && ev->value == 0)
        launcher_toggle();
}

void panel_start(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "launcher", on_launcher }, { "clock", on_clock }, { "settings", on_settings },
        { "signout", on_signout }, { "restart", on_restart }, { "poweroff", on_poweroff }, { "lock", on_lock }, { NULL, NULL },
    };
    char who[128];

    if (!(panel = ui_load_string_named(page, handlers, NULL, "panel")))
        return;
    ui_window_set_backdrop(panel, backdrop, NULL);
    snprintf(who, sizeof(who), "Signed in as %s", me.display);
    ui_set_text(ui_get(panel, "who"), who);
    ui_on_key(panel, panel_key, NULL);
    ui_on_system_event(window_list, NULL);
    tick(NULL);
    ui_timer(1000, tick, NULL);
    launcher_init(panel);
    volume_init(panel, ui_get(panel, "volume"));
    notify_init(panel, ui_get(panel, "bell"));
    lock_init();
    ui_window_show(panel);
    wm_subscribe();
}
