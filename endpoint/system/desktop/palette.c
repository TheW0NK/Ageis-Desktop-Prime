#include "desktop.h"

// The command palette (Super+P): one list of things to do, filtered by
// typing. The focused app's menu commands come first, then its windows,
// workspaces, apps to open and the session.

#define WIDTH   640
#define HEIGHT  420
#define MAX_ENTRIES 512
#define MAX_SHOWN   200

static const char page[] =
    "<window padding='12' spacing='8'>"
    "  <input id='query' placeholder='Type a command, an app or a window' onchange='filter' onactivate='run'/>"
    "  <list id='results' expand='1' columns='Command|Where:200:right' singleclick='true' onactivate='run'"
    "        placeholder='Nothing matches'/>"
    "</window>";

enum kind { K_APP_COMMAND, K_WINDOW, K_WORKSPACE, K_MOVE, K_OPEN_APP, K_ACTION, K_THEME };

struct entry {
    enum kind kind;
    int arg;
    uint32_t id;                    // a window's global id
    char label[200];
    char where[80];
};

static struct ui_window *win;
static struct widget *query, *results;
static struct entry entries[MAX_ENTRIES];
static int nentries, shown[MAX_SHOWN], nshown;
static uint32_t target;             // the window focused when the palette opened
static char target_name[64];        // its app, from the title ("Notepad")
static struct app_info apps[96];
static uint64_t hidden_at;

static const char *const actions[] = { "Lock the screen", "Sign out", "Restart", "Shut down", "Settings" };
static const char *const action_ids[] = { "lock", "signout", "restart", "poweroff", "settings" };
static const char *const themes[] = { "light", "dark", "high-contrast" };
static const char *const theme_names[] = { "Light", "Dark", "High contrast" };

static struct entry *add(enum kind kind, int arg, uint32_t id, const char *label, const char *where)
{
    struct entry *e;

    if (nentries == MAX_ENTRIES)
        return NULL;
    e = &entries[nentries++];
    e->kind = kind;
    e->arg = arg;
    e->id = id;
    strlcpy(e->label, label, sizeof(e->label));
    strlcpy(e->where, where ? where : "", sizeof(e->where));
    return e;
}

// Every word typed must appear in the command or where it is.
static bool matches(const struct entry *e, const char *q)
{
    char word[64];

    while (*q) {
        size_t n;
        bool found = false;

        while (*q == ' ')
            q++;
        n = strcspn(q, " ");
        if (!n)
            break;
        snprintf(word, sizeof(word), "%.*s", (int)MIN(n, sizeof(word) - 1), q);
        q += n;
        for (const char *s = e->label; *s && !found; s++)
            found = !strncasecmp(s, word, strlen(word));
        for (const char *s = e->where; *s && !found; s++)
            found = !strncasecmp(s, word, strlen(word));
        if (!found)
            return false;
    }
    return true;
}

static void filter(struct widget *w, void *u)
{
    const char *q = ui_text(query);

    (void)w;
    (void)u;
    nshown = 0;
    ui_list_clear(results);
    for (int i = 0; i < nentries && nshown < MAX_SHOWN; i++) {
        char row[300];

        if (!matches(&entries[i], q))
            continue;
        shown[nshown++] = i;
        snprintf(row, sizeof(row), "%s\t%s", entries[i].label, entries[i].where);
        ui_list_add(results, row);
    }
    if (nshown)
        ui_list_select(results, 0);
}

static void hide(void)
{
    ui_window_hide(win);
    hidden_at = uptime_ms();
}

static void run(struct widget *w, void *u)
{
    int i = ui_list_selected(results);
    struct entry e;

    (void)w;
    (void)u;
    if (i < 0 || i >= nshown)
        return;
    e = entries[shown[i]];
    // Out of the way first: the command may open a window or a dialog.
    hide();
    switch (e.kind) {
    case K_APP_COMMAND:
        wm_command_run(e.id, e.arg);
        break;
    case K_WINDOW:
        wm_activate(e.id, false);
        break;
    case K_WORKSPACE:
        wm_switch_workspace(e.arg);
        break;
    case K_MOVE:
        wm_move_to_workspace(e.id, e.arg);
        break;
    case K_OPEN_APP:
        app_launch(&apps[e.arg], NULL);
        break;
    case K_ACTION:
        panel_action(action_ids[e.arg]);
        break;
    case K_THEME:
        user_setting_set(&me, "theme", themes[e.arg]);
        wm_setting_changed("theme", themes[e.arg]);
        break;
    }
}

// The focused app's commands arrive one by one after the palette opens.
void palette_command_item(const struct wm_msg *m)
{
    char label[WM_TEXT_MAX], *tab, where[80], keep[200] = "";
    int at, sel;

    if (!win || m->window != target || m->a < 0)
        return;
    strlcpy(label, m->text, sizeof(label));
    tab = strchr(label, '\t');
    if (tab)
        *tab++ = 0;
    snprintf(where, sizeof(where), "%s%s%s", target_name, tab && *target_name ? " \xC2\xB7 " : "", tab ? tab : "");
    // Whatever is selected stays selected.
    sel = ui_list_selected(results);
    if (sel > 0 && sel < nshown)
        strlcpy(keep, entries[shown[sel]].label, sizeof(keep));
    // App commands go before everything else, in the order they came.
    for (at = 0; at < nentries && entries[at].kind == K_APP_COMMAND; at++)
        ;
    if (nentries == MAX_ENTRIES)
        return;
    memmove(&entries[at + 1], &entries[at], (nentries - at) * sizeof(entries[0]));
    nentries++;
    entries[at] = (struct entry){ K_APP_COMMAND, m->a, m->window, "", "" };
    strlcpy(entries[at].label, label, sizeof(entries[at].label));
    strlcpy(entries[at].where, where, sizeof(entries[at].where));
    filter(NULL, NULL);
    for (int k = 0; *keep && k < nshown; k++)
        if (!strcmp(entries[shown[k]].label, keep)) {
            ui_list_select(results, k);
            break;
        }
}

static void build(void)
{
    struct panel_window wins[64];
    int nwins = panel_windows(wins, 64), napps;
    char label[200], where[80];

    nentries = 0;
    target = 0;
    target_name[0] = 0;
    for (int i = 0; i < nwins; i++) {
        if (wins[i].focused) {
            const char *dash = strstr(wins[i].title, " - ");

            target = wins[i].id;
            // "notes.txt - Notepad": the app's name is after the last dash.
            while (dash && strstr(dash + 3, " - "))
                dash = strstr(dash + 3, " - ");
            strlcpy(target_name, dash ? dash + 3 : wins[i].title, sizeof(target_name));
        }
    }
    for (int i = 0; i < nwins; i++) {
        snprintf(label, sizeof(label), "Switch to %s", wins[i].title);
        snprintf(where, sizeof(where), "Window \xC2\xB7 workspace %d", wins[i].workspace + 1);
        add(K_WINDOW, 0, wins[i].id, label, where);
    }
    for (int i = 0; i < WM_WORKSPACES; i++) {
        snprintf(label, sizeof(label), "Go to workspace %d", i + 1);
        add(K_WORKSPACE, i, 0, label, panel_workspace() == i ? "Shown now" : "Workspace");
    }
    if (target)
        for (int i = 0; i < WM_WORKSPACES; i++) {
            if (i == panel_workspace())
                continue;
            snprintf(label, sizeof(label), "Move this window to workspace %d", i + 1);
            add(K_MOVE, i, target, label, target_name);
        }
    napps = app_list(apps, 96);
    for (int i = 0; i < napps; i++) {
        if (apps[i].hidden)
            continue;
        snprintf(label, sizeof(label), "Open %s", apps[i].name);
        add(K_OPEN_APP, i, 0, label, *apps[i].suite ? apps[i].suite : "App");
    }
    for (int i = 0; i < (int)(sizeof(actions) / sizeof(actions[0])); i++)
        add(K_ACTION, i, 0, actions[i], !strcmp(action_ids[i], "lock") ? "Super+L" : "Session");
    for (int i = 0; i < 3; i++) {
        snprintf(label, sizeof(label), "Theme: %s", theme_names[i]);
        add(K_THEME, i, 0, label, "Appearance");
    }
}

static void key(struct ui_window *w, struct wm_event *ev, void *u)
{
    int sel = ui_list_selected(results);

    (void)w;
    (void)u;
    if (!ev->value)
        return;
    // Esc, or Super+P again, closes it.
    if ((ev->key == KEY_ESC || ((ev->mods & MOD_META) && ev->key == KEY_A + 'p' - 'a')) && ev->value == 1) {
        hide();
    } else if (ev->key == KEY_DOWN && nshown) {
        ui_list_select(results, MIN(sel + 1, nshown - 1));
    } else if (ev->key == KEY_UP && nshown) {
        ui_list_select(results, MAX(sel - 1, 0));
    } else if (ev->key == KEY_PAGEDOWN && nshown) {
        ui_list_select(results, MIN(sel + 10, nshown - 1));
    } else if (ev->key == KEY_PAGEUP && nshown) {
        ui_list_select(results, MAX(sel - 10, 0));
    }
}

static bool dismissed(struct ui_window *w, void *u)
{
    (void)u;
    // Clicked outside: hide, but keep the window for next time.
    ui_window_hide(w);
    hidden_at = uptime_ms();
    return false;
}

bool palette_shown(void)
{
    return win && ui_wm_window(win) && ui_wm_window(win)->visible;
}

void palette_toggle(void)
{
    int sw, sh;

    if (!win)
        return;
    if (palette_shown()) {
        hide();
        return;
    }
    if (uptime_ms() - hidden_at < 300)
        return;
    if (launcher_shown())
        launcher_toggle();
    build();
    ui_set_text(query, "");
    filter(NULL, NULL);
    // Near the top of the screen, in the middle (relative to the panel,
    // which is at the bottom).
    wm_screen_size(&sw, &sh, NULL);
    ui_window_move(win, (sw - WIDTH) / 2, sh / 7 - panel_top());
    ui_window_show(win);
    ui_focus(query);
    if (target)
        wm_commands_query();
}

void palette_init(struct ui_window *panel)
{
    static const struct ui_handler_entry handlers[] = {
        { "filter", filter }, { "run", run }, { NULL, NULL },
    };

    if (!(win = ui_load_string_named(page, handlers, NULL, "palette")))
        return;
    query = ui_get(win, "query");
    results = ui_get(win, "results");
    ui_window_set_size(win, WIDTH, HEIGHT);
    ui_window_set_parent(win, panel);
    ui_window_hide(win);
    ui_on_close(win, dismissed, NULL);
    ui_on_key(win, key, NULL);
    ui_window_set_flags(win, WM_ROLE_POPUP | WM_FLAG_KEYBOARD);
}
