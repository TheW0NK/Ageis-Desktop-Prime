#include "desktop.h"

// The launcher: a popup above the Apps button with every app, grouped by
// suite, a search field and the session buttons.

#define WIDTH   600
#define HEIGHT  560

static const char page[] =
    "<window padding='16' spacing='12'>"
    "  <input id='search' placeholder='Search apps and files' onchange='search' onactivate='first'/>"
    "  <scroll id='scroll' expand='1'>"
    "    <vbox id='apps' spacing='6'/>"
    "    <label id='fileshead' text='Files' bold='true' dim='true' visible='false'/>"
    "    <list id='files' singleclick='true' onactivate='openfile' visible='false'/>"
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

// ---- File search: names under the home folder ----

#define FILE_RESULTS 8
#define SCAN_LIMIT   4000

static char results[FILE_RESULTS][512];
static bool result_dir[FILE_RESULTS];
static int nresults, scanned, search_timer = -1;

static bool contains(const char *s, const char *q, size_t n)
{
    for (; *s; s++)
        if (!strncasecmp(s, q, n))
            return true;
    return false;
}

static void scan(const char *dir, const char *q, int depth)
{
    struct dir_stream *d;
    struct aegis_dirent *de;
    size_t n = strlen(q);

    if (depth > 8 || !(d = opendir(dir)))
        return;
    while ((de = readdir(d)) && nresults < FILE_RESULTS && scanned < SCAN_LIMIT) {
        char path[512];
        struct aegis_stat st;
        bool isdir;

        if (de->name[0] == '.')
            continue;
        scanned++;
        snprintf(path, sizeof(path), "%s/%s", dir, de->name);
        isdir = stat(path, &st) == 0 && S_ISDIR(st.mode);
        if (contains(de->name, q, n)) {
            result_dir[nresults] = isdir;
            strlcpy(results[nresults++], path, sizeof(results[0]));
        }
        if (isdir)
            scan(path, q, depth + 1);
    }
    closedir(d);
}

static bool run_search(void *u)
{
    const char *q = ui_text(ui_get(win, "search"));
    struct widget *list = ui_get(win, "files");
    size_t hl = strlen(me.home);

    (void)u;
    search_timer = -1;
    nresults = scanned = 0;
    ui_list_clear(list);
    if (strlen(q) >= 2)
        scan(me.home, q, 0);
    for (int i = 0; i < nresults; i++) {
        const char *base = strrchr(results[i], '/') + 1;
        char row[600], where[512];

        // "name   in Documents/Notes"
        strlcpy(where, results[i], sizeof(where));
        where[base - results[i] - 1] = 0;
        snprintf(row, sizeof(row), "%s    in %s", base,
                 !strncmp(where, me.home, hl) ? (where[hl] ? where + hl + 1 : "Home") : where);
        ui_list_add(list, row);
        ui_list_set_icon_shared(list, i, icon_get(icon_for_file(base, result_dir[i]), 20));
    }
    {
        char h[16];

        snprintf(h, sizeof(h), "%d", nresults * ui_theme.row_height + 6);
        ui_set_attr(list, "height", h);
    }
    ui_set_visible(ui_get(win, "fileshead"), nresults > 0);
    ui_set_visible(list, nresults > 0);
    ui_relayout(win);
    return false;
}

static void open_result(int i)
{
    struct app_info a;

    if (i < 0 || i >= nresults)
        return;
    if (result_dir[i] ? app_find("files", &a) == 0 : app_for_file(results[i], &a) == 0)
        app_launch(&a, results[i]);
    else if (app_find("notepad", &a) == 0)
        app_launch(&a, results[i]);
    ui_window_hide(win);
}

static void open_file(struct widget *w, void *u)
{
    (void)u;
    open_result(ui_list_selected(w));
}

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
    // Searching the disk waits for a pause in typing.
    if (search_timer >= 0)
        ui_timer_cancel(search_timer);
    search_timer = ui_timer(150, run_search, NULL);
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
    if (search_timer >= 0)
        run_search(NULL);
    open_result(0);
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
    // As the session menu does it (a guest is asked first).
    panel_action("signout");
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
        { "poweroff", poweroff }, { "openfile", open_file }, { NULL, NULL },
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
