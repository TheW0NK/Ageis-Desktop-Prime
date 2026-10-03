#include "desktop.h"

// The background window: wallpaper and the icons of ~/Desktop.

#define CELL_W  100
#define CELL_H  96
#define ICON    48

struct item {
    char name[128];                 // file name
    char label[96];
    char icon[256];
    char path[300];
};

static struct ui_window *win;
static struct widget *canvas;
static struct item *items;
static int nitems, selected = -1;
static char wallpaper[256];
static struct surface *cache;
static int64_t desktop_mtime;
static char desktop_dir[200];

// ---- Shortcuts ----

// Reads a .shortcut file: name=, app= or target=, icon=.
static void read_shortcut(const char *path, char *name, size_t nsize, char *app, size_t asize, char *target,
                          size_t tsize, char *icon, size_t isize)
{
    int fd = open(path, O_RDONLY);
    char line[300];

    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) >= 0) {
        char *eq = strchr(line, '=');

        if (!eq)
            continue;
        *eq++ = 0;
        if (!strcmp(line, "name"))
            strlcpy(name, eq, nsize);
        else if (!strcmp(line, "app"))
            strlcpy(app, eq, asize);
        else if (!strcmp(line, "target"))
            strlcpy(target, eq, tsize);
        else if (!strcmp(line, "icon"))
            strlcpy(icon, eq, isize);
    }
    close(fd);
}

static bool has_suffix(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);

    return a >= b && !strcmp(s + a - b, suffix);
}

void open_path(const char *path)
{
    struct aegis_stat st;
    struct app_info app;
    char msg[400];

    if (has_suffix(path, ".shortcut")) {
        char name[96] = "", id[32] = "", target[256] = "", icon[256] = "";

        read_shortcut(path, name, sizeof(name), id, sizeof(id), target, sizeof(target), icon, sizeof(icon));
        if (*id) {
            if (app_find(id, &app) == 0 && app_launch(&app, NULL) > 0)
                return;
            snprintf(msg, sizeof(msg), "The app \"%s\" is not installed.", id);
            ui_message(NULL, "Open", msg, "OK");
            return;
        }
        if (*target)
            open_path(target);
        return;
    }
    if (stat(path, &st) < 0) {
        snprintf(msg, sizeof(msg), "\"%s\" no longer exists.", path);
        ui_message(NULL, "Open", msg, "OK");
        return;
    }
    if (S_ISDIR(st.mode)) {
        if (app_find("files", &app) == 0)
            app_launch(&app, path);
        return;
    }
    if (app_for_file(path, &app) == 0) {
        app_launch(&app, path);
        return;
    }
    snprintf(msg, sizeof(msg), "No app on this computer opens \"%s\".", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    ui_message(NULL, "Open", msg, "OK");
}

// The first time a user signs in, their desktop gets a few shortcuts.
static void first_run(void)
{
    static const char *const defaults[] = { "files", "term", "settings", "notepad", "browser", NULL };
    char flag[256];
    int fd;

    user_path(&me, "system/appdata/desktop", flag, sizeof(flag));
    mkdir(flag, 0700);
    strlcat(flag, "/initialized", sizeof(flag));
    if (access(flag, 0) == 0)
        return;
    for (int i = 0; defaults[i]; i++) {
        struct app_info a;
        char path[300];

        if (app_find(defaults[i], &a) < 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s.shortcut", desktop_dir, a.name);
        if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644)) >= 0) {
            dprintf(fd, "name=%s\napp=%s\nicon=%s\n", a.name, a.id, a.icon);
            close(fd);
        }
    }
    if ((fd = open(flag, O_WRONLY | O_CREAT, 0600)) >= 0)
        close(fd);
}

static int by_label(const void *a, const void *b)
{
    const struct item *x = a, *y = b;
    bool xs = has_suffix(x->name, ".shortcut"), ys = has_suffix(y->name, ".shortcut");

    if (xs != ys)
        return xs ? -1 : 1;     // shortcuts first
    return strcasecmp(x->label, y->label);
}

static void scan(void)
{
    struct dir_stream *d = opendir(desktop_dir);
    struct aegis_dirent *e;
    int cap = 0;

    nitems = 0;
    selected = -1;
    if (!d)
        return;
    while ((e = readdir(d))) {
        struct item *it;
        struct aegis_stat st;

        if (e->name[0] == '.')
            continue;
        if (nitems == cap) {
            struct item *n = realloc(items, (cap = cap ? cap * 2 : 16) * sizeof(*n));

            if (!n)
                break;
            items = n;
        }
        it = &items[nitems];
        memset(it, 0, sizeof(*it));
        strlcpy(it->name, e->name, sizeof(it->name));
        snprintf(it->path, sizeof(it->path), "%s/%s", desktop_dir, e->name);
        if (has_suffix(e->name, ".shortcut")) {
            char id[32] = "", target[256] = "";

            read_shortcut(it->path, it->label, sizeof(it->label), id, sizeof(id), target, sizeof(target), it->icon,
                          sizeof(it->icon));
            if (!it->icon[0]) {
                struct app_info a;

                if (*id && app_find(id, &a) == 0)
                    strlcpy(it->icon, a.icon, sizeof(it->icon));
                else
                    strlcpy(it->icon, *target ? icon_for_file(target, false) : "file", sizeof(it->icon));
            }
            if (!it->label[0]) {
                snprintf(it->label, sizeof(it->label), "%s", e->name);
                it->label[strlen(it->label) - 9] = 0;
            }
        } else {
            strlcpy(it->label, e->name, sizeof(it->label));
            strlcpy(it->icon, icon_for_file(e->name, stat(it->path, &st) == 0 && S_ISDIR(st.mode)),
                    sizeof(it->icon));
        }
        nitems++;
    }
    closedir(d);
    qsort(items, nitems, sizeof(*items), by_label);
}

// ---- Drawing ----

static struct rect cell_rect(struct rect r, int i)
{
    int rows = MAX(1, (r.h - 16) / CELL_H);

    return (struct rect){ r.x + 12 + (i / rows) * CELL_W, r.y + 12 + (i % rows) * CELL_H, CELL_W - 8, CELL_H - 6 };
}

static void backdrop(struct ui_window *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    if (!cache || cache->width != r.w || cache->height != r.h) {
        struct gfx bg;

        surface_destroy(cache);
        if (!(cache = surface_create(r.w, r.h)))
            return;
        gfx_init(&bg, cache);
        wallpaper_draw(&bg, r, wallpaper);
    }
    gfx_blit(g, cache, r, r.x, r.y);
}

static void paint(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    struct font *f = font_get(FONT_SANS, 13);

    (void)w;
    (void)u;
    for (int i = 0; i < nitems; i++) {
        struct rect c = cell_rect(r, i);
        int lh = font_line_height(f), tw;
        char line1[96], line2[96];
        const char *label = items[i].label;
        int cut = strlen(label);

        if (i == selected)
            gfx_fill_rounded(g, c, 8, ALPHA(0xFFFFFF, 0x38));
        icon_draw(g, items[i].icon, (struct rect){ c.x + (c.w - ICON) / 2, c.y + 4, ICON, ICON });
        // Up to two lines of label, with a shadow for contrast.
        while (cut > 0 && text_width(f, label, cut) > c.w - 4)
            cut = utf8_prev(label, cut);
        if (cut < (int)strlen(label)) {
            int brk = cut;

            for (int k = cut; k > cut / 2; k--)
                if (label[k] == ' ') {
                    brk = k;
                    break;
                }
            snprintf(line1, sizeof(line1), "%.*s", brk, label);
            snprintf(line2, sizeof(line2), "%s", label + brk + (label[brk] == ' '));
            if (text_width(f, line2, -1) > c.w - 4) {
                int c2 = strlen(line2);

                while (c2 > 0 && text_width(f, line2, c2) + text_width(f, "\xE2\x80\xA6", 3) > c.w - 4)
                    c2 = utf8_prev(line2, c2);
                strcpy(line2 + c2, "\xE2\x80\xA6");
            }
        } else {
            strlcpy(line1, label, sizeof(line1));
            line2[0] = 0;
        }
        for (int k = 0; k < 2; k++) {
            const char *t = k ? line2 : line1;
            int y = c.y + ICON + 8 + k * lh;

            if (!*t)
                continue;
            tw = text_width(f, t, -1);
            text_draw(g, f, c.x + (c.w - tw) / 2 + 1, y + 1, t, -1, ALPHA(0x000000, 0xB0));
            text_draw(g, f, c.x + (c.w - tw) / 2, y, t, -1, RGB(0xFFFFFF));
        }
    }
}

static int item_at(struct widget *w, int x, int y)
{
    struct rect r = ui_rect(w);

    r.x = r.y = 0;
    for (int i = 0; i < nitems; i++)
        if (rect_contains(cell_rect(r, i), x, y))
            return i;
    return -1;
}

static void input(struct widget *w, struct wm_event *ev, void *u)
{
    int i;

    (void)u;
    if (ev->type != WM_EV_POINTER || ev->kind != WM_PTR_DOWN)
        return;
    i = item_at(w, ev->x, ev->y);
    if (i != selected) {
        selected = i;
        ui_redraw(w);
    }
    if (i >= 0 && ev->detail == BTN_LEFT && ui_click_count(w) == 2)
        open_path(items[i].path);
}

// ---- Keeping up with changes ----

void background_reload(void)
{
    char spec[256];

    if (user_setting_get(&me, "background", spec, sizeof(spec)) <= 0)
        strlcpy(spec, "default", sizeof(spec));
    if (strcmp(spec, wallpaper)) {
        strlcpy(wallpaper, spec, sizeof(wallpaper));
        surface_destroy(cache);
        cache = NULL;
        ui_window_set_backdrop(win, backdrop, NULL);
    }
}

static bool poll_changes(void *u)
{
    struct aegis_stat st;

    (void)u;
    background_reload();
    if (stat(desktop_dir, &st) == 0 && st.mtime != desktop_mtime) {
        desktop_mtime = st.mtime;
        scan();
        ui_redraw(canvas);
    }
    return true;
}

void background_start(void)
{
    struct aegis_stat st;

    snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop", me.home);
    mkdir(desktop_dir, 0755);
    first_run();
    win = ui_window_new("Desktop", 0, 0, WM_ROLE_DESKTOP);
    canvas = ui_create(win, "canvas");
    ui_add(ui_root(win), canvas);
    ui_canvas_set(canvas, paint, input, NULL);
    background_reload();
    scan();
    if (stat(desktop_dir, &st) == 0)
        desktop_mtime = st.mtime;
    ui_timer(2000, poll_changes, NULL);
    ui_window_show(win);
}
