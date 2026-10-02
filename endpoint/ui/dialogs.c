#include "ui_internal.h"

// Modal dialogs: messages, text prompts and the file chooser. Each runs a
// nested event loop until it is answered.

struct modal {
    bool done;
    int result;
    struct ui_window *win;
};

static bool modal_closed(struct ui_window *win, void *user)
{
    struct modal *m = user;

    (void)win;
    m->done = true;
    m->result = -1;
    return true;
}

static void run_modal(struct modal *m)
{
    ui_on_close(m->win, modal_closed, m);
    m->win->modal = true;
    ui_window_show(m->win);
    while (!m->done && wm_connected())
        ui_iterate(-1);
    if (!m->win->closing)
        ui_window_close(m->win);
}

static struct ui_window *dialog_window(struct ui_window *parent, const char *title, int width)
{
    struct ui_window *win = ui_window_new(title, width, 0, WM_ROLE_DIALOG | WM_FLAG_NO_RESIZE);

    if (!win)
        return NULL;
    win->parent = parent;
    win->autoshow = false;
    widget_set_attr(win->root, "padding", "18");
    widget_set_attr(win->root, "spacing", "14");
    return win;
}

static void finish(struct widget *w, void *user)
{
    struct modal *m = user;
    const char *idx = ui_attr(w, "index");

    m->done = true;
    m->result = idx ? atoi(idx) : 0;
}

// Adds a right-aligned row of buttons ("A|B|C"); the first is the default,
// the last cancels.
static void add_buttons(struct ui_window *win, const char *buttons, struct modal *m)
{
    struct widget *row = ui_create(win, "hbox");
    char copy[256], *p = copy;
    int i = 0, n = 1;

    ui_set_attr(row, "justify", "end");
    ui_add(win->root, row);
    strlcpy(copy, buttons && *buttons ? buttons : "OK", sizeof(copy));
    for (char *q = copy; *q; q++)
        n += *q == '|';
    while (p) {
        char *bar = strchr(p, '|'), num[8];
        struct widget *b = ui_create(win, "button");

        if (bar)
            *bar = 0;
        snprintf(num, sizeof(num), "%d", i);
        ui_set_attr(b, "index", num);
        ui_set_text(b, ui_tr(p));
        if (i == 0)
            ui_set_attr(b, "default", "true");
        if (i == n - 1)
            ui_set_attr(b, "cancel", "true");
        ui_set_handler(b, "click", finish, m);
        ui_add(row, b);
        if (i == 0)
            win->focus = b;
        i++;
        p = bar ? bar + 1 : NULL;
    }
}

static int text_dialog_width(const char *text)
{
    int w = text_width(ui_font(), text ? text : "", -1) + 60;

    return MIN(MAX(w, 340), 560);
}

int ui_message(struct ui_window *parent, const char *title, const char *text, const char *buttons)
{
    struct modal m = { 0 };
    struct widget *p;

    if (!(m.win = dialog_window(parent, title, text_dialog_width(text))))
        return -1;
    p = ui_create(m.win, "p");
    ui_set_text(p, text);
    ui_add(m.win->root, p);
    add_buttons(m.win, buttons, &m);
    run_modal(&m);
    return m.result;
}

char *ui_prompt(struct ui_window *parent, const char *title, const char *text, const char *initial)
{
    struct modal m = { 0 };
    struct widget *p, *in;
    char *result = NULL;

    if (!(m.win = dialog_window(parent, title, text_dialog_width(text))))
        return NULL;
    p = ui_create(m.win, "p");
    ui_set_text(p, text);
    ui_add(m.win->root, p);
    in = ui_create(m.win, "input");
    ui_set_text(in, initial ? initial : "");
    ui_add(m.win->root, in);
    add_buttons(m.win, "OK|Cancel", &m);
    m.win->focus = in;
    m.win->focus_visible = true;
    ui_textarea_select(in, 0, strlen(ui_text(in)));
    run_modal(&m);
    if (m.result == 0)
        result = strdup(ui_text(in));
    return result;
}

char *ui_prompt_password(struct ui_window *parent, const char *title, const char *text)
{
    struct modal m = { 0 };
    struct widget *p, *in;
    char *result = NULL;

    if (!(m.win = dialog_window(parent, title, text_dialog_width(text))))
        return NULL;
    p = ui_create(m.win, "p");
    ui_set_text(p, text);
    ui_add(m.win->root, p);
    in = ui_create(m.win, "password");
    ui_add(m.win->root, in);
    add_buttons(m.win, "OK|Cancel", &m);
    m.win->focus = in;
    run_modal(&m);
    if (m.result == 0)
        result = strdup(ui_text(in));
    return result;
}

// Gets administrator rights for this program: asks for the user's password
// and calls sudo(). Returns true once the program runs as root.
bool ui_elevate(struct ui_window *parent, const char *why)
{
    struct user_info u;
    char text[400];

    if (geteuid() == 0)
        return true;
    if (user_current(&u) < 0 || !user_in_group(u.name, "sudo")) {
        snprintf(text, sizeof(text), "%s\n\nThis needs an administrator. Your account is not one.",
                 why ? why : "This change affects the whole computer.");
        ui_message(parent, "Administrator needed", text, "OK");
        return false;
    }
    for (int tries = 0; tries < 3; tries++) {
        char *pw;
        int r;

        snprintf(text, sizeof(text), "%s Enter your password to continue.",
                 tries ? "That password is not right." : why ? why : "This change affects the whole computer.");
        if (!(pw = ui_prompt_password(parent, "Administrator", text)))
            return false;
        r = sudo(pw);
        memset(pw, 0, strlen(pw));
        free(pw);
        if (r == 0)
            return true;
    }
    return false;
}

// ---- Dialogs built by apps ----

static bool app_dialog_closed(struct ui_window *win, void *user)
{
    (void)user;
    win->dialog_done = true;
    win->dialog_result = -1;
    return false;
}

int ui_dialog_run(struct ui_window *dlg, struct ui_window *parent)
{
    int result;

    if (!dlg)
        return -1;
    if (!dlg->wm) {
        dlg->parent = parent;
        dlg->flags = (dlg->flags & ~WM_ROLE_MASK) | WM_ROLE_DIALOG;
    }
    dlg->modal = true;
    dlg->dialog_done = false;
    ui_on_close(dlg, app_dialog_closed, NULL);
    ui_window_show(dlg);
    while (!dlg->dialog_done && wm_connected())
        ui_iterate(-1);
    result = dlg->dialog_result;
    ui_window_close(dlg);
    return result;
}

void ui_dialog_end(struct ui_window *dlg, int result)
{
    if (!dlg)
        return;
    dlg->dialog_done = true;
    dlg->dialog_result = result;
}

// ---- File chooser ----

struct entry {
    char *name;
    bool dir;
    uint64_t size;
    int64_t mtime;
};

struct chooser {
    struct modal m;
    bool save;
    char dir[512];
    struct entry *entries;
    int n;
    struct widget *path, *files, *name, *places;
    char *result;
    const char *filter;             // "*.txt;*.md" or NULL
};

static int entry_cmp(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;

    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

void ui_format_size(uint64_t size, char *buf, size_t len)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = size;
    int u = 0;

    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(buf, len, "%lu B", (unsigned long)size);
    else
        snprintf(buf, len, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

static bool matches_filter(const char *filter, const char *name)
{
    char pat[64];
    const char *p = filter;

    if (!filter || !*filter)
        return true;
    while (*p) {
        const char *semi = strchr(p, ';');
        int len = semi ? semi - p : (int)strlen(p);

        snprintf(pat, sizeof(pat), "%.*s", len, p);
        // Only "*.ext" and "*" patterns.
        if (!strcmp(pat, "*"))
            return true;
        if (pat[0] == '*' && pat[1] == '.') {
            size_t nl = strlen(name), el = strlen(pat + 1);

            if (nl >= el && !strcasecmp(name + nl - el, pat + 1))
                return true;
        }
        if (!semi)
            break;
        p = semi + 1;
    }
    return false;
}

static void free_entries(struct chooser *c)
{
    for (int i = 0; i < c->n; i++)
        free(c->entries[i].name);
    free(c->entries);
    c->entries = NULL;
    c->n = 0;
}

static void load_dir(struct chooser *c, const char *dir)
{
    struct dir_stream *d = opendir(dir);
    struct aegis_dirent *de;
    int cap = 0;

    if (!d) {
        ui_message(c->m.win, "Files", "This folder cannot be opened.", "OK");
        return;
    }
    free_entries(c);
    strlcpy(c->dir, dir, sizeof(c->dir));
    while ((de = readdir(d))) {
        char path[768];
        struct aegis_stat st;
        struct entry *e;

        if (de->name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", de->name);
        if (stat(path, &st) < 0)
            continue;
        if (!S_ISDIR(st.mode) && !matches_filter(c->filter, de->name))
            continue;
        if (c->n == cap) {
            struct entry *ne = realloc(c->entries, (cap = cap ? cap * 2 : 32) * sizeof(*ne));

            if (!ne)
                break;
            c->entries = ne;
        }
        e = &c->entries[c->n];
        if (!(e->name = strdup(de->name)))
            continue;
        e->dir = S_ISDIR(st.mode);
        e->size = st.size;
        e->mtime = st.mtime;
        c->n++;
    }
    closedir(d);
    qsort(c->entries, c->n, sizeof(*c->entries), entry_cmp);
    ui_list_clear(c->files);
    for (int i = 0; i < c->n; i++) {
        char row[400], size[32], when[32];
        struct tm tm;

        localtime_r(&c->entries[i].mtime, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        if (c->entries[i].dir)
            strlcpy(size, "Folder", sizeof(size));
        else
            ui_format_size(c->entries[i].size, size, sizeof(size));
        snprintf(row, sizeof(row), "%s%s\t%s\t%s", c->entries[i].name, c->entries[i].dir ? "/" : "", size, when);
        ui_list_add(c->files, row);
    }
    ui_set_text(c->path, c->dir);
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    if (name[0] == '/')
        strlcpy(out, name, size);
    else
        snprintf(out, size, "%s/%s", strcmp(dir, "/") ? dir : "", name);
}

static void go_up(struct widget *w, void *user)
{
    struct chooser *c = user;
    char dir[512], *slash;

    (void)w;
    strlcpy(dir, c->dir, sizeof(dir));
    slash = strrchr(dir, '/');
    if (!slash)
        return;
    if (slash == dir)
        slash[1] = 0;
    else
        *slash = 0;
    load_dir(c, dir);
}

static void choose(struct chooser *c, const char *path)
{
    free(c->result);
    c->result = strdup(path);
    c->m.done = true;
    c->m.result = 0;
}

static void accept_choice(struct widget *w, void *user)
{
    struct chooser *c = user;
    int sel = ui_list_selected(c->files);
    char path[768];
    struct aegis_stat st;

    (void)w;
    if (c->save) {
        const char *name = ui_text(c->name);

        if (sel >= 0 && c->entries[sel].dir && (!*name || ui_is_focused(c->files))) {
            join(path, sizeof(path), c->dir, c->entries[sel].name);
            load_dir(c, path);
            return;
        }
        if (!*name)
            return;
        join(path, sizeof(path), c->dir, name);
        if (stat(path, &st) == 0) {
            char q[900];

            if (S_ISDIR(st.mode)) {
                load_dir(c, path);
                ui_set_text(c->name, "");
                return;
            }
            snprintf(q, sizeof(q), "\"%s\" already exists. Replace it?", name);
            if (ui_message(c->m.win, "Save", q, "Replace|Cancel") != 0)
                return;
        }
        choose(c, path);
        return;
    }
    if (sel < 0)
        return;
    join(path, sizeof(path), c->dir, c->entries[sel].name);
    if (c->entries[sel].dir)
        load_dir(c, path);
    else
        choose(c, path);
}

static void selected(struct widget *w, void *user)
{
    struct chooser *c = user;
    int sel = ui_list_selected(w);

    if (c->save && sel >= 0 && !c->entries[sel].dir)
        ui_set_text(c->name, c->entries[sel].name);
}

static void path_entered(struct widget *w, void *user)
{
    struct chooser *c = user;
    struct aegis_stat st;
    const char *p = ui_text(w);

    if (stat(p, &st) == 0 && S_ISDIR(st.mode))
        load_dir(c, p);
    else if (!c->save && stat(p, &st) == 0)
        choose(c, p);
    else
        ui_message(c->m.win, "Files", "There is no folder with that name.", "OK");
}

static const char *place_dirs[] = { "", "Desktop", "Documents", "Downloads", "Images", "Music", NULL };

static void place_chosen(struct widget *w, void *user)
{
    struct chooser *c = user;
    int i = ui_list_selected(w);
    const char *home = getenv("HOME");
    char path[512];

    if (i < 0)
        return;
    if (i >= 6 || !home)
        strlcpy(path, "/", sizeof(path));
    else if (!*place_dirs[i])
        strlcpy(path, home, sizeof(path));
    else
        snprintf(path, sizeof(path), "%s/%s", home, place_dirs[i]);
    load_dir(c, path);
}

static const char chooser_aui[] =
    "<vbox padding='12' spacing='8' expand='1'>"
    "  <hbox spacing='6'>"
    "    <button id='up' text='Up' onclick='up'/>"
    "    <input id='path' expand='1' onactivate='path'/>"
    "  </hbox>"
    "  <hbox spacing='8' expand='1'>"
    "    <list id='places' width='150' onselect='place'>"
    "      <item>Home</item><item>Desktop</item><item>Documents</item><item>Downloads</item>"
    "      <item>Images</item><item>Music</item><item>Computer</item>"
    "    </list>"
    "    <table id='files' expand='1' columns='Name|Size:90:right|Modified:150'"
    "           onactivate='accept' onselect='select' placeholder='This folder is empty'/>"
    "  </hbox>"
    "  <hbox id='namerow' spacing='8'>"
    "    <label text='File name' align='center'/>"
    "    <input id='name' expand='1' onactivate='accept'/>"
    "  </hbox>"
    "  <hbox justify='end' spacing='8'>"
    "    <button id='cancel' text='Cancel' cancel='true' onclick='cancel'/>"
    "    <button id='ok' text='Open' default='true' onclick='accept'/>"
    "  </hbox>"
    "</vbox>";

static void cancel(struct widget *w, void *user)
{
    struct chooser *c = user;

    (void)w;
    c->m.done = true;
    c->m.result = -1;
}

char *ui_file_dialog(struct ui_window *parent, const char *title, const char *start_dir, bool save,
                     const char *suggested_name)
{
    return ui_file_dialog_filtered(parent, title, start_dir, save, suggested_name, NULL);
}

char *ui_file_dialog_filtered(struct ui_window *parent, const char *title, const char *start_dir, bool save,
                              const char *suggested_name, const char *filter)
{
    struct chooser c = { 0 };
    const struct ui_handler_entry handlers[] = {
        { "up", go_up }, { "path", path_entered }, { "place", place_chosen }, { "accept", accept_choice },
        { "select", selected }, { "cancel", cancel }, { NULL, NULL },
    };
    const char *home = getenv("HOME");
    struct aegis_stat st;

    c.save = save;
    c.filter = filter;
    if (!(c.m.win = ui_window_new(title ? title : save ? "Save" : "Open", 760, 480, WM_ROLE_DIALOG)))
        return NULL;
    c.m.win->parent = parent;
    c.m.win->autoshow = false;
    c.m.win->handlers = handlers;
    c.m.win->user = &c;
    if (!ui_parse_into(c.m.win->root, chooser_aui)) {
        ui_window_close(c.m.win);
        return NULL;
    }
    c.m.win->handlers = NULL;
    c.path = ui_get(c.m.win, "path");
    c.files = ui_get(c.m.win, "files");
    c.name = ui_get(c.m.win, "name");
    c.places = ui_get(c.m.win, "places");
    ui_set_text(ui_get(c.m.win, "ok"), save ? "Save" : "Open");
    if (!save)
        ui_set_visible(ui_get(c.m.win, "namerow"), false);
    else
        ui_set_text(c.name, suggested_name ? suggested_name : "");
    if (start_dir && stat(start_dir, &st) == 0 && S_ISDIR(st.mode))
        load_dir(&c, start_dir);
    else if (home && stat(home, &st) == 0)
        load_dir(&c, home);
    else
        load_dir(&c, "/");
    c.m.win->focus = save ? c.name : c.files;
    run_modal(&c.m);
    free_entries(&c);
    if (c.m.result != 0) {
        free(c.result);
        return NULL;
    }
    return c.result;
}
