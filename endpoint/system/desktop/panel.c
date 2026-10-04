#include "desktop.h"

// The taskbar: launcher button, the workspaces, a button per open window
// on the workspace shown, the volume, clock, and the session menu.

struct task {
    uint32_t id;
    uint32_t state;
    int workspace;
    char title[WM_TEXT_MAX];
    struct widget *button;
};

static struct ui_window *panel;
static struct task tasks[64];
static int ntasks;
static int workspace, nworkspaces = WM_WORKSPACES;
static struct widget *ws_buttons[WM_WORKSPACES];
static uint32_t menu_task;          // the window the task menu is for

static const char page[] =
    "<window role='panel' height='46' padding='5' spacing='0'>"
    "  <hbox expand='1' spacing='6'>"
    "    <button id='start' flat='true' symbol='apps' iconsize='22' text='Apps' onclick='launcher'/>"
    "    <separator/>"
    "    <hbox id='workspaces' spacing='2'/>"
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
    "    <item text='Command palette' shortcut='Super+P' onclick='palette'/>"
    "    <item text='Lock' shortcut='Super+L' onclick='lock'/>"
    "    <separator/>"
    "    <item text='Sign out' onclick='signout'/>"
    "    <item text='Restart' onclick='restart'/>"
    "    <item text='Shut down' onclick='poweroff'/>"
    "  </menu>"
    "  <menu id='taskmenu'>"
    "    <item id='tm0' text='Move to workspace 1' onclick='moveto'/>"
    "    <item id='tm1' text='Move to workspace 2' onclick='moveto'/>"
    "    <item id='tm2' text='Move to workspace 3' onclick='moveto'/>"
    "    <item id='tm3' text='Move to workspace 4' onclick='moveto'/>"
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
    // Only the windows on the workspace shown.
    ui_set_visible(t->button, t->workspace == workspace);
}

// ---- Workspaces ----

static void show_workspace(int n)
{
    char tip[48];

    workspace = n;
    for (int i = 0; i < WM_WORKSPACES; i++) {
        ui_set_value(ws_buttons[i], i == n);
        ui_set_visible(ws_buttons[i], i < nworkspaces);
        snprintf(tip, sizeof(tip), "Workspace %d%s", i + 1, i == n ? " (shown)" : "");
        ui_set_attr(ws_buttons[i], "tooltip", tip);
    }
    for (int i = 0; i < ntasks; i++)
        update_button(&tasks[i]);
}

static void ws_clicked(struct widget *w, void *u)
{
    (void)w;
    wm_switch_workspace((int)(intptr_t)u);
}

static void task_context(struct widget *w, void *u)
{
    struct task *t = u;

    menu_task = t->id;
    for (int i = 0; i < WM_WORKSPACES; i++) {
        char id[8];

        snprintf(id, sizeof(id), "tm%d", i);
        ui_set_enabled(ui_get(panel, id), i != t->workspace);
    }
    ui_menu_popup(ui_get(panel, "taskmenu"), w, -1, -1);
}

static void on_moveto(struct widget *w, void *u)
{
    const char *id = ui_id(w);

    (void)u;
    if (id && !strncmp(id, "tm", 2))
        wm_move_to_workspace(menu_task, atoi(id + 2));
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
    for (int i = 0; i < ntasks; i++) {
        ui_set_handler(tasks[i].button, "click", task_clicked, &tasks[i]);
        ui_set_handler(tasks[i].button, "context", task_context, &tasks[i]);
    }
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
    if (ev->type == WM_EV_COMMAND_ITEM) {
        palette_command_item(m);
        return;
    }
    if (ev->type == WM_EV_WORKSPACE) {
        nworkspaces = MIN(MAX(m->b, 1), WM_WORKSPACES);
        show_workspace(MIN(MAX(m->a, 0), nworkspaces - 1));
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
        t->workspace = m->c;
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
        t->workspace = m->c;
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
    if (me_guest && ui_message(NULL, "Sign out", "Sign out of Guest? Everything saved since signing in will be erased.",
                               "Sign out|Cancel") != 0)
        return;
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

int panel_windows(struct panel_window *out, int max)
{
    int n = 0;

    for (int i = 0; i < ntasks && n < max; i++, n++) {
        out[n].id = tasks[i].id;
        strlcpy(out[n].title, tasks[i].title, sizeof(out[n].title));
        out[n].workspace = tasks[i].workspace;
        out[n].focused = (tasks[i].state & WM_STATE_FOCUSED) && !(tasks[i].state & WM_STATE_MINIMIZED);
    }
    return n;
}

int panel_workspace(void)
{
    return workspace;
}

int panel_top(void)
{
    struct wm_window *w = panel ? ui_wm_window(panel) : NULL;
    int sh;

    wm_screen_size(NULL, &sh, NULL);
    return sh - (w ? w->height : 46);
}

void panel_action(const char *name)
{
    static const struct ui_handler_entry *table;
    static const struct ui_handler_entry actions[] = {
        { "lock", on_lock }, { "signout", on_signout }, { "restart", on_restart }, { "poweroff", on_poweroff },
        { "settings", on_settings }, { NULL, NULL },
    };

    for (table = actions; table->name; table++)
        if (!strcmp(table->name, name))
            table->fn(NULL, NULL);
}

static void on_palette(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    palette_toggle();
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
        { "signout", on_signout }, { "restart", on_restart }, { "poweroff", on_poweroff }, { "lock", on_lock },
        { "moveto", on_moveto }, { "palette", on_palette }, { NULL, NULL },
    };
    char who[128];

    if (!(panel = ui_load_string_named(page, handlers, NULL, "panel")))
        return;
    ui_window_set_backdrop(panel, backdrop, NULL);
    for (int i = 0; i < WM_WORKSPACES; i++) {
        char num[4];

        snprintf(num, sizeof(num), "%d", i + 1);
        ws_buttons[i] = ui_create(panel, "button");
        ui_set_attr(ws_buttons[i], "flat", "true");
        ui_set_attr(ws_buttons[i], "width", "30");
        ui_set_text(ws_buttons[i], num);
        ui_set_handler(ws_buttons[i], "click", ws_clicked, (void *)(intptr_t)i);
        ui_add(ui_get(panel, "workspaces"), ws_buttons[i]);
    }
    show_workspace(0);
    snprintf(who, sizeof(who), "Signed in as %s", me.display);
    ui_set_text(ui_get(panel, "who"), who);
    ui_on_key(panel, panel_key, NULL);
    ui_on_system_event(window_list, NULL);
    tick(NULL);
    ui_timer(1000, tick, NULL);
    launcher_init(panel);
    palette_init(panel);
    volume_init(panel, ui_get(panel, "volume"));
    notify_init(panel, ui_get(panel, "bell"));
    lock_init();
    ui_window_show(panel);
    wm_subscribe();
}
