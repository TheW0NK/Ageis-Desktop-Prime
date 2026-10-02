#include "desktop.h"

// The launcher: a popup above the Apps button with every app, grouped by
// suite, a search field and the session buttons.

#define WIDTH   600
#define HEIGHT  560

static const char page[] =
    "<window padding='16' spacing='12'>"
    "  <input id='search' placeholder='Search apps' onchange='search' onactivate='first'/>"
    "  <scroll id='scroll' expand='1'>"
    "    <vbox id='apps' spacing='6'/>"
    "  </scroll>"
    "  <separator/>"
    "  <hbox spacing='10'>"
    "    <canvas id='avatar' expand='0' width='32' height='32'/>"
    "    <label id='name' align='center'/>"
    "    <spacer/>"
    "    <button flat='true' symbol='logout' iconsize='20' onclick='signout'/>"
    "    <button flat='true' symbol='restart' iconsize='20' onclick='restart'/>"
    "    <button flat='true' symbol='power' iconsize='20' onclick='poweroff'/>"
    "  </hbox>"
    "</window>";

static const char *const suites[] = { "Default", "System", "Administrative", "Development", "" };

static struct ui_window *win;
static struct app_info apps[96];
static struct widget *tiles[96], *headers[5], *grids[5];
static int napps;
static uint64_t hidden_at;

static void rebuild_tiles(void);

static void launch_app(struct widget *w, void *u)
{
    struct app_info *a = u;

    (void)w;
    app_launch(a, NULL);
    ui_window_hide(win);
}

static bool matches(const struct app_info *a, const char *q)
{
    size_t n = strlen(q);

    if (!n)
        return true;
    for (const char *s = a->name; *s; s++)
        if (!strncasecmp(s, q, n))
            return true;
    for (const char *s = a->description; *s; s++)
        if (!strncasecmp(s, q, n))
            return true;
    return false;
}

static int suite_of(const struct app_info *a)
{
    for (int i = 0; i < 4; i++)
        if (!strcasecmp(a->suite, suites[i]))
            return i;
    return 4;
}

static void filter(struct widget *w, void *u)
{
    const char *q = ui_text(w);
    int shown[5] = { 0 };

    (void)u;
    for (int i = 0; i < napps; i++) {
        bool m = matches(&apps[i], q);

        ui_set_visible(tiles[i], m);
        if (m)
            shown[suite_of(&apps[i])]++;
    }
    for (int s = 0; s < 5; s++) {
        if (headers[s])
            ui_set_visible(headers[s], shown[s] > 0);
        if (grids[s])
            ui_set_visible(grids[s], shown[s] > 0);
    }
}

static void first(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    for (int i = 0; i < napps; i++)
        if (ui_visible(tiles[i])) {
            launch_app(tiles[i], &apps[i]);
            return;
        }
}

static void paint_avatar(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    char pic[256] = "";

    (void)w;
    (void)u;
    user_setting_get(&me, "picture", pic, sizeof(pic));
    avatar_draw(g, r, me.display, pic);
}

static void signout(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_window_hide(win);
    end_session(EXIT_SIGN_OUT);
}

static void restart(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_window_hide(win);
    if (ui_message(NULL, "Restart", "Restart the computer? Unsaved work in open apps will be lost.",
                   "Restart|Cancel") == 0)
        end_session(EXIT_RESTART);
}

static void poweroff(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_window_hide(win);
    if (ui_message(NULL, "Shut down", "Shut down the computer? Unsaved work in open apps will be lost.",
                   "Shut down|Cancel") == 0)
        end_session(EXIT_POWEROFF);
}

static bool dismissed(struct ui_window *w, void *u)
{
    (void)u;
    // Clicked outside: hide, but keep the window for next time.
    ui_window_hide(w);
    hidden_at = uptime_ms();
    return false;
}

static void key(struct ui_window *w, struct wm_event *ev, void *u)
{
    (void)u;
    if (!ev->value && (ev->key == KEY_LEFTMETA || ev->key == KEY_RIGHTMETA))
        ui_window_hide(w);
    else if (ev->value == 1 && ev->key == KEY_ESC)
        ui_window_hide(w);
}

void launcher_refresh_user(void)
{
    if (!win)
        return;
    ui_set_text(ui_get(win, "name"), me.display);
    ui_redraw(ui_get(win, "avatar"));
}

bool launcher_shown(void)
{
    return win && ui_wm_window(win) && ui_wm_window(win)->visible;
}

void launcher_toggle(void)
{
    if (!win)
        return;
    if (launcher_shown()) {
        ui_window_hide(win);
        return;
    }
    // The click on the Apps button that dismissed the launcher should not
    // open it again.
    if (uptime_ms() - hidden_at < 300)
        return;
    rebuild_tiles();
    ui_set_text(ui_get(win, "search"), "");
    filter(ui_get(win, "search"), NULL);
    ui_window_show(win);
    ui_focus(ui_get(win, "search"));
}

// The app tiles, rebuilt each time the launcher opens so newly installed
// apps show up.
static void rebuild_tiles(void)
{
    struct widget *box = ui_get(win, "apps");

    while (ui_children(box))
        ui_remove(ui_child(box, 0));
    memset(tiles, 0, sizeof(tiles));
    memset(headers, 0, sizeof(headers));
    memset(grids, 0, sizeof(grids));
    napps = app_list(apps, 96);
    for (int s = 0; s < 5; s++) {
        int count = 0;

        for (int i = 0; i < napps; i++)
            if (!apps[i].hidden && suite_of(&apps[i]) == s)
                count++;
        if (!count)
            continue;
        headers[s] = ui_create(win, "label");
        ui_set_text(headers[s], s < 4 ? suites[s] : "Other");
        ui_set_attr(headers[s], "bold", "true");
        ui_set_attr(headers[s], "dim", "true");
        ui_add(box, headers[s]);
        grids[s] = ui_create(win, "grid");
        ui_set_attr(grids[s], "columns", "5");
        ui_set_attr(grids[s], "stretch", "");
        ui_set_attr(grids[s], "spacing", "4");
        ui_add(box, grids[s]);
        for (int i = 0; i < napps; i++) {
            if (apps[i].hidden || suite_of(&apps[i]) != s)
                continue;
            tiles[i] = ui_create(win, "button");
            ui_set_attr(tiles[i], "tile", "true");
            ui_set_attr(tiles[i], "appicon", apps[i].icon);
            ui_set_text(tiles[i], apps[i].name);
            ui_set_handler(tiles[i], "click", launch_app, &apps[i]);
            ui_add(grids[s], tiles[i]);
        }
    }
    // Hidden apps get no tile; keep the array dense for filter().
    for (int i = 0; i < napps; i++)
        if (!tiles[i]) {
            tiles[i] = ui_create(win, "spacer");
            ui_set_visible(tiles[i], false);
            ui_add(box, tiles[i]);
        }
}

void launcher_init(struct ui_window *panel)
{
    static const struct ui_handler_entry handlers[] = {
        { "search", filter }, { "first", first }, { "signout", signout }, { "restart", restart },
        { "poweroff", poweroff }, { NULL, NULL },
    };
    if (!(win = ui_load_string_named(page, handlers, NULL, "launcher")))
        return;
    ui_window_set_size(win, WIDTH, HEIGHT);
    ui_window_set_parent(win, panel);
    // Above the panel, at its left edge (relative to the panel's content).
    ui_window_move(win, 4, -HEIGHT - 6);
    ui_window_hide(win);
    ui_on_close(win, dismissed, NULL);
    ui_on_key(win, key, NULL);
    // The launcher is a popup that takes the keyboard.
    ui_window_set_flags(win, WM_ROLE_POPUP | WM_FLAG_KEYBOARD);
    ui_set_text(ui_get(win, "name"), me.display);
    ui_canvas_set(ui_get(win, "avatar"), paint_avatar, NULL, NULL);

    rebuild_tiles();
}
