#include "aegis.h"
#include "ui.h"

// Files: the file manager.

static const char page[] =
    "<window title='Files' width='900' height='560' padding='0' spacing='0'>"
    "  <menubar>"
    "    <menu text='File'>"
    "      <item text='New window' shortcut='Ctrl+N' onclick='newwin'/>"
    "      <item text='New folder' shortcut='Ctrl+Shift+N' onclick='newfolder'/>"
    "      <item text='New text file' onclick='newfile'/>"
    "      <separator/>"
    "      <item text='Open' onclick='open'/>"
    "      <item text='Open in Terminal' onclick='terminal'/>"
    "      <separator/>"
    "      <item text='Rename...' shortcut='F2' onclick='rename'/>"
    "      <item text='Delete' shortcut='Delete' onclick='delete'/>"
    "      <item text='Delete permanently' shortcut='Shift+Delete' onclick='destroy'/>"
    "      <item id='restore' text='Restore' enabled='false' onclick='restore'/>"
    "      <item id='emptytrash' text='Empty Trash' onclick='emptytrash'/>"
    "      <item text='Properties' shortcut='Alt+Enter' onclick='properties'/>"
    "      <separator/>"
    "      <item text='Close' shortcut='Ctrl+W' onclick='close'/>"
    "    </menu>"
    "    <menu text='Edit'>"
    "      <item text='Cut' shortcut='Ctrl+X' onclick='cut'/>"
    "      <item text='Copy' shortcut='Ctrl+C' onclick='copy'/>"
    "      <item text='Paste' shortcut='Ctrl+V' onclick='paste'/>"
    "      <separator/>"
    "      <item text='Copy path' onclick='copypath'/>"
    "    </menu>"
    "    <menu text='View'>"
    "      <item id='hidden' text='Show hidden files' shortcut='Ctrl+H' onclick='togglehidden'/>"
    "      <item text='Refresh' shortcut='F5' onclick='refresh'/>"
    "    </menu>"
    "    <menu text='Go'>"
    "      <item text='Back' shortcut='Alt+Left' onclick='back'/>"
    "      <item text='Forward' shortcut='Alt+Right' onclick='forward'/>"
    "      <item text='Up' shortcut='Alt+Up' onclick='up'/>"
    "      <item text='Home' shortcut='Alt+Home' onclick='home'/>"
    "    </menu>"
    "  </menubar>"
    "  <toolbar>"
    "    <button flat='true' symbol='back' onclick='back' id='backbtn'/>"
    "    <button flat='true' symbol='forward' onclick='forward' id='fwdbtn'/>"
    "    <button flat='true' symbol='up' onclick='up'/>"
    "    <input id='path' expand='1' onactivate='go'/>"
    "    <input id='filter' width='180' placeholder='Filter' onchange='filter'/>"
    "  </toolbar>"
    "  <hbox expand='1' padding='8' spacing='8'>"
    "    <list id='places' width='170' onselect='place'/>"
    "    <table id='files' expand='1' columns='Name|Size:90:right|Type:110|Modified:150'"
    "           onactivate='open' oncontext='context' onsort='sort' onselect='selected'"
    "           placeholder='This folder is empty'/>"
    "  </hbox>"
    "  <statusbar>"
    "    <label id='status'/>"
    "    <spacer/>"
    "    <label id='space' dim='true'/>"
    "  </statusbar>"
    "  <menu id='ctx'>"
    "    <item text='Open' onclick='open'/>"
    "    <item text='Open in Terminal' onclick='terminal'/>"
    "    <separator/>"
    "    <item text='Cut' onclick='cut'/>"
    "    <item text='Copy' onclick='copy'/>"
    "    <item text='Paste' onclick='paste'/>"
    "    <separator/>"
    "    <item text='Rename...' onclick='rename'/>"
    "    <item text='Delete' onclick='delete'/>"
    "    <item id='ctxrestore' text='Restore' onclick='restore'/>"
    "    <separator/>"
    "    <item text='New folder' onclick='newfolder'/>"
    "    <item text='Properties' onclick='properties'/>"
    "  </menu>"
    "</window>";

struct entry {
    char name[256];
    bool dir, link;
    uint64_t size;
    int64_t mtime;
    uint32_t mode, uid;
};

static struct ui_window *win;
static struct widget *table, *places;
static struct entry *entries;
static int nentries, *shown, nshown;
static char cwd[512];
static char history[32][512];
static int hpos = -1, hlen;
static bool show_hidden;
static int sort_col;
static bool sort_desc;
// Cut or copied paths, newline separated.
static char *clip;
static bool clip_cut;
static struct user_info me;

static const char *const place_names[] = { "Home", "Desktop", "Documents", "Downloads", "Images", "Music",
                                           "Computer", "Trash" };
static const char *const place_icons[] = { "glyph:home", "glyph:desktop", "glyph:documents", "glyph:downloads",
                                           "glyph:image", "glyph:audio", "glyph:disk",
                                           "glyph:trash" };
#define NPLACES 8

static char trash_files[512];

static void load(const char *dir, bool record);

static bool in_trash(void)
{
    return *trash_files && !strcmp(cwd, trash_files);
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s/%s", strcmp(dir, "/") ? dir : "", name);
}

static const char *type_of(const struct entry *e)
{
    const char *dot = strrchr(e->name, '.');
    static char buf[32];

    if (e->dir)
        return "Folder";
    if (e->link)
        return "Link";
    if (!dot || dot == e->name)
        return (e->mode & 0111) ? "Program" : "File";
    snprintf(buf, sizeof(buf), "%s file", dot + 1);
    for (char *p = buf; *p && *p != ' '; p++)
        *p = toupper((unsigned char)*p);
    return buf;
}

static int compare(const void *a, const void *b)
{
    const struct entry *x = &entries[*(const int *)a], *y = &entries[*(const int *)b];
    int r = 0;

    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    switch (sort_col) {
    case 1: r = x->size < y->size ? -1 : x->size > y->size; break;
    case 2: r = strcasecmp(type_of(x), type_of(y)); break;
    case 3: r = x->mtime < y->mtime ? -1 : x->mtime > y->mtime; break;
    }
    if (!r)
        r = strcasecmp(x->name, y->name);
    return sort_desc ? -r : r;
}

static void status(void)
{
    char buf[128], free_s[32], total_s[32];
    struct aegis_statfs fs;
    int dirs = 0, sel = ui_list_selected(table);

    for (int i = 0; i < nshown; i++)
        dirs += entries[shown[i]].dir;
    if (in_trash() && sel >= 0 && sel < nshown) {
        struct trash_item *items;
        int n = trash_list(&items);

        snprintf(buf, sizeof(buf), "\"%s\" selected", entries[shown[sel]].name);
        for (int i = 0; i < n; i++) {
            if (!strcmp(items[i].name, entries[shown[sel]].name) && *items[i].origin)
                snprintf(buf, sizeof(buf), "\"%s\" was deleted from %.80s", items[i].name, items[i].origin);
        }
        free(items);
    } else if (in_trash()) {
        if (nshown)
            snprintf(buf, sizeof(buf), "%d item%s in the Trash", nshown, nshown == 1 ? "" : "s");
        else
            strlcpy(buf, "The Trash is empty", sizeof(buf));
    } else if (sel >= 0 && sel < nshown && !entries[shown[sel]].dir) {
        char size[32];

        ui_format_size(entries[shown[sel]].size, size, sizeof(size));
        snprintf(buf, sizeof(buf), "\"%s\" selected (%s)", entries[shown[sel]].name, size);
    } else {
        snprintf(buf, sizeof(buf), "%d folders, %d files", dirs, nshown - dirs);
    }
    ui_set_text(ui_get(win, "status"), buf);
    if (statfs(cwd, &fs) == 0) {
        ui_format_size(fs.blocks_free * fs.block_size, free_s, sizeof(free_s));
        ui_format_size(fs.blocks * fs.block_size, total_s, sizeof(total_s));
        snprintf(buf, sizeof(buf), "%s free of %s", free_s, total_s);
        ui_set_text(ui_get(win, "space"), buf);
    }
}

static void fill(void)
{
    const char *filter = ui_text(ui_get(win, "filter"));
    size_t fl = strlen(filter);
    int keep = ui_list_selected(table) >= 0 && ui_list_selected(table) < nshown ? shown[ui_list_selected(table)] : -1;
    char keep_name[256] = "";

    if (keep >= 0)
        strlcpy(keep_name, entries[keep].name, sizeof(keep_name));
    free(shown);
    shown = malloc(sizeof(int) * MAX(nentries, 1));
    nshown = 0;
    for (int i = 0; i < nentries; i++) {
        bool match = !fl;

        for (const char *p = entries[i].name; !match && *p; p++)
            match = !strncasecmp(p, filter, fl);
        if (match && (show_hidden || entries[i].name[0] != '.'))
            shown[nshown++] = i;
    }
    qsort(shown, nshown, sizeof(int), compare);
    ui_list_clear(table);
    for (int k = 0; k < nshown; k++) {
        struct entry *e = &entries[shown[k]];
        char row[512], size[32], when[32];
        struct tm tm;

        localtime_r(&e->mtime, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        if (e->dir)
            size[0] = 0;
        else
            ui_format_size(e->size, size, sizeof(size));
        snprintf(row, sizeof(row), "%s\t%s\t%s\t%s", e->name, size, type_of(e), when);
        ui_list_add(table, row);
        ui_list_set_icon_shared(table, k, icon_get(icon_for_file(e->name, e->dir), 20));
        if (*keep_name && !strcmp(keep_name, e->name))
            ui_list_select(table, k);
    }
    status();
}

static bool read_dir(const char *dir)
{
    struct dir_stream *d = opendir(dir);
    struct aegis_dirent *de;
    int cap = 0;

    if (!d)
        return false;
    nentries = 0;
    while ((de = readdir(d))) {
        char path[800];
        struct aegis_stat st, lst;
        struct entry *e;

        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        if (nentries == cap) {
            struct entry *n = realloc(entries, (cap = cap ? cap * 2 : 64) * sizeof(*n));

            if (!n)
                break;
            entries = n;
        }
        e = &entries[nentries];
        memset(e, 0, sizeof(*e));
        strlcpy(e->name, de->name, sizeof(e->name));
        join(path, sizeof(path), dir, de->name);
        if (lstat(path, &lst) == 0 && S_ISLNK(lst.mode))
            e->link = true;
        if (stat(path, &st) == 0) {
            e->dir = S_ISDIR(st.mode);
            e->size = st.size;
            e->mtime = st.mtime;
            e->mode = st.mode;
            e->uid = st.uid;
        }
        nentries++;
    }
    closedir(d);
    return true;
}

static void update_title(void)
{
    char title[600];
    const char *base = strrchr(cwd, '/');

    snprintf(title, sizeof(title), "%s - Files",
             in_trash() ? "Trash" : !strcmp(cwd, "/") ? "Computer" : base ? base + 1 : cwd);
    ui_set_enabled(ui_get(win, "restore"), in_trash());
    ui_set_attr(ui_get(win, "ctxrestore"), "hidden", in_trash() ? "false" : "true");
    ui_window_set_title(win, title);
    ui_set_enabled(ui_get(win, "backbtn"), hpos > 0);
    ui_set_enabled(ui_get(win, "fwdbtn"), hpos + 1 < hlen);
}

static void load(const char *dir, bool record)
{
    char real[512];

    strlcpy(real, dir, sizeof(real));
    // Tidy "a/b/.." and trailing slashes.
    {
        size_t n = strlen(real);

        while (n > 1 && real[n - 1] == '/')
            real[--n] = 0;
    }
    if (!read_dir(real)) {
        char msg[700];

        snprintf(msg, sizeof(msg), "\"%s\" cannot be opened: %s.", real, strerror(errno));
        ui_message(win, "Files", msg, "OK");
        return;
    }
    strlcpy(cwd, real, sizeof(cwd));
    ui_set_text(ui_get(win, "path"), cwd);
    ui_set_text(ui_get(win, "filter"), "");
    if (record) {
        if (hpos < 31) {
            hpos++;
        } else {
            memmove(history[0], history[1], sizeof(history[0]) * 31);
        }
        strlcpy(history[hpos], cwd, sizeof(history[hpos]));
        hlen = hpos + 1;
    }
    ui_list_select(table, -1);
    fill();
    update_title();
}

static struct entry *current(void)
{
    int sel = ui_list_selected(table);

    return sel >= 0 && sel < nshown ? &entries[shown[sel]] : NULL;
}

static void current_path(char *out, size_t size)
{
    struct entry *e = current();

    join(out, size, cwd, e ? e->name : "");
}

// ---- Handlers ----

static void open_entry(struct widget *w, void *u)
{
    struct entry *e = current();
    char path[800], msg[900];
    struct app_info app;

    (void)w;
    (void)u;
    if (!e)
        return;
    join(path, sizeof(path), cwd, e->name);
    if (e->dir) {
        load(path, true);
        return;
    }
    if (app_for_file(e->name, &app) == 0) {
        app_launch(&app, path);
        return;
    }
    if ((e->mode & 0111) && !strchr(e->name, '.')) {
        launch(path, NULL);
        return;
    }
    snprintf(msg, sizeof(msg), "No app is set to open \"%s\". Open it in Notepad?", e->name);
    if (ui_message(win, "Open", msg, "Open in Notepad|Cancel") == 0 && app_find("notepad", &app) == 0)
        app_launch(&app, path);
}

static void go(struct widget *w, void *u)
{
    (void)u;
    load(ui_text(w), true);
}

static void back(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (hpos > 0)
        load(history[--hpos], false);
}

static void forward(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (hpos + 1 < hlen)
        load(history[++hpos], false);
}

static void up(struct widget *w, void *u)
{
    char dir[512], *slash;

    (void)w;
    (void)u;
    strlcpy(dir, cwd, sizeof(dir));
    if (!(slash = strrchr(dir, '/')))
        return;
    if (slash == dir)
        slash[1] = 0;
    else
        *slash = 0;
    load(dir, true);
}

static void home(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    load(me.home, true);
}

static void place(struct widget *w, void *u)
{
    int i = ui_list_selected(w);
    char path[512];

    (void)u;
    if (i < 0)
        return;
    if (i == 6)
        strlcpy(path, "/", sizeof(path));
    else if (i == 7)
        strlcpy(path, trash_files, sizeof(path));
    else if (i == 0)
        strlcpy(path, me.home, sizeof(path));
    else
        snprintf(path, sizeof(path), "%s/%s", me.home, place_names[i]);
    load(path, true);
}

static void refresh(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    read_dir(cwd);
    fill();
}

static void filter(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    fill();
}

static void sort(struct widget *w, void *u)
{
    (void)u;
    sort_col = atoi(ui_attr(w, "sortcolumn"));
    sort_desc = ui_attr_true(ui_attr(w, "sortdescending"));
    fill();
}

static void selected(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    status();
}

static void toggle_hidden(struct widget *w, void *u)
{
    (void)u;
    show_hidden = !show_hidden;
    ui_set_attr(ui_get(win, "hidden"), "checked", show_hidden ? "true" : "false");
    (void)w;
    fill();
}

static void fail(const char *what, const char *name)
{
    char msg[600];

    snprintf(msg, sizeof(msg), "%s \"%s\" failed: %s.", what, name, strerror(errno));
    ui_message(win, "Files", msg, "OK");
}

static void new_folder(struct widget *w, void *u)
{
    char *name = ui_prompt(win, "New folder", "Name of the new folder:", "New folder");
    char path[800];

    (void)w;
    (void)u;
    if (!name)
        return;
    join(path, sizeof(path), cwd, name);
    if (*name && mkdir(path, 0755) < 0)
        fail("Creating", name);
    free(name);
    refresh(NULL, NULL);
}

static void new_file(struct widget *w, void *u)
{
    char *name = ui_prompt(win, "New text file", "Name of the new file:", "New file.txt");
    char path[800];
    int fd;

    (void)w;
    (void)u;
    if (!name)
        return;
    join(path, sizeof(path), cwd, name);
    if (*name && (fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644)) >= 0)
        close(fd);
    else if (*name)
        fail("Creating", name);
    free(name);
    refresh(NULL, NULL);
}

static void rename_entry(struct widget *w, void *u)
{
    struct entry *e = current();
    char *name, from[800], to[800];

    (void)w;
    (void)u;
    if (!e)
        return;
    if (!(name = ui_prompt(win, "Rename", "New name:", e->name)))
        return;
    join(from, sizeof(from), cwd, e->name);
    join(to, sizeof(to), cwd, name);
    if (*name && strcmp(name, e->name) && !strchr(name, '/')) {
        struct aegis_stat st;

        if (stat(to, &st) == 0)
            ui_message(win, "Rename", "Something with that name is already here.", "OK");
        else if (rename(from, to) < 0)
            fail("Renaming", e->name);
    }
    free(name);
    refresh(NULL, NULL);
}

static void destroy_entry(struct widget *w, void *u)
{
    struct entry *e = current();
    char path[800], msg[600];

    (void)w;
    (void)u;
    if (!e)
        return;
    snprintf(msg, sizeof(msg), "Delete \"%s\"%s permanently? This cannot be undone.", e->name,
             e->dir ? " and everything in it" : "");
    if (ui_message(win, "Delete", msg, "Delete|Cancel") != 0)
        return;
    join(path, sizeof(path), cwd, e->name);
    if (in_trash()) {
        struct trash_item *items;
        int n = trash_list(&items);

        for (int i = 0; i < n; i++) {
            if (!strcmp(items[i].name, e->name) && trash_delete(&items[i]) < 0)
                fail("Deleting", e->name);
        }
        free(items);
    } else if (remove_path(path) < 0) {
        fail("Deleting", e->name);
    }
    refresh(NULL, NULL);
}

// Delete moves things to the Trash; in the Trash it deletes them for good.
static void delete_entry(struct widget *w, void *u)
{
    struct entry *e = current();
    char path[800], msg[600];

    if (!e)
        return;
    if (in_trash()) {
        destroy_entry(w, u);
        return;
    }
    join(path, sizeof(path), cwd, e->name);
    if (trash_put(path) < 0) {
        snprintf(msg, sizeof(msg), "\"%s\" cannot be moved to the Trash (%s). Delete it permanently?", e->name,
                 strerror(errno));
        if (ui_message(win, "Delete", msg, "Delete|Cancel") != 0)
            return;
        if (remove_path(path) < 0)
            fail("Deleting", e->name);
    }
    refresh(NULL, NULL);
}

static void restore_entry(struct widget *w, void *u)
{
    struct entry *e = current();
    struct trash_item *items;
    int n;

    (void)w;
    (void)u;
    if (!e || !in_trash())
        return;
    n = trash_list(&items);
    for (int i = 0; i < n; i++) {
        if (!strcmp(items[i].name, e->name) && trash_restore(&items[i]) < 0)
            fail("Restoring", e->name);
    }
    free(items);
    refresh(NULL, NULL);
}

static void empty_trash(struct widget *w, void *u)
{
    int n = trash_count();
    char msg[200];

    (void)w;
    (void)u;
    if (!n) {
        ui_message(win, "Empty Trash", "The Trash is already empty.", "OK");
        return;
    }
    snprintf(msg, sizeof(msg), "Delete the %d item%s in the Trash permanently? This cannot be undone.", n,
             n == 1 ? "" : "s");
    if (ui_message(win, "Empty Trash", msg, "Empty Trash|Cancel") != 0)
        return;
    if (trash_empty() < 0)
        ui_message(win, "Empty Trash", "Some items could not be deleted.", "OK");
    if (in_trash())
        refresh(NULL, NULL);
}

static void set_clip(bool cut)
{
    char path[800];

    if (!current())
        return;
    current_path(path, sizeof(path));
    free(clip);
    clip = strdup(path);
    clip_cut = cut;
    // Other apps get the path as text.
    ui_clipboard_set(path);
}

static void cut(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    set_clip(true);
}

static void copy(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    set_clip(false);
}

static void paste(struct widget *w, void *u)
{
    const char *src = clip, *base;
    char dst[800];
    struct aegis_stat st;

    (void)w;
    (void)u;
    // Paths copied as text elsewhere work too.
    if (!src) {
        src = ui_clipboard_get();
        if (*src != '/' || stat(src, &st) < 0)
            return;
    }
    base = strrchr(src, '/') ? strrchr(src, '/') + 1 : src;
    unique_name(cwd, base, dst, sizeof(dst));
    if ((clip_cut && clip ? move_path(src, dst) : copy_path(src, dst)) < 0)
        fail(clip_cut ? "Moving" : "Copying", base);
    if (clip_cut && clip) {
        free(clip);
        clip = NULL;
        clip_cut = false;
    }
    refresh(NULL, NULL);
}

static void copy_path_text(struct widget *w, void *u)
{
    char path[800];

    (void)w;
    (void)u;
    if (current())
        current_path(path, sizeof(path));
    else
        strlcpy(path, cwd, sizeof(path));
    ui_clipboard_set(path);
}

static void properties(struct widget *w, void *u)
{
    struct entry *e = current();
    char msg[1200], size[32], when[64], owner[64] = "?";
    struct tm tm;
    struct user_info ou;

    (void)w;
    (void)u;
    if (!e)
        return;
    ui_format_size(e->size, size, sizeof(size));
    localtime_r(&e->mtime, &tm);
    strftime(when, sizeof(when), "%A, %B %e %Y at %H:%M", &tm);
    if (user_by_uid(e->uid, &ou) == 0)
        strlcpy(owner, ou.name, sizeof(owner));
    else if (e->uid == 0)
        strlcpy(owner, "root", sizeof(owner));
    snprintf(msg, sizeof(msg), "Name: %s\nKind: %s\nSize: %s\nOwner: %s\nPermissions: %o\nModified: %s\nFolder: %s",
             e->name, type_of(e), e->dir ? "-" : size, owner, e->mode & 07777, when, cwd);
    ui_message(win, "Properties", msg, "OK");
}

static void terminal(struct widget *w, void *u)
{
    struct entry *e = current();
    char path[800];

    (void)w;
    (void)u;
    if (e && e->dir)
        join(path, sizeof(path), cwd, e->name);
    else
        strlcpy(path, cwd, sizeof(path));
    launch("/bin/term", path);
}

static void new_window(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    launch("/bin/files", cwd);
}

static void close_window(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_quit(0);
}

static void context(struct widget *w, void *u)
{
    (void)u;
    ui_menu_popup(ui_get(win, "ctx"), w, -1, -1);
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "open", open_entry }, { "go", go }, { "back", back }, { "forward", forward }, { "up", up },
        { "home", home }, { "place", place }, { "refresh", refresh }, { "filter", filter }, { "sort", sort },
        { "selected", selected }, { "togglehidden", toggle_hidden }, { "newfolder", new_folder },
        { "newfile", new_file }, { "rename", rename_entry }, { "delete", delete_entry }, { "cut", cut },
        { "destroy", destroy_entry }, { "restore", restore_entry }, { "emptytrash", empty_trash },
        { "copy", copy }, { "paste", paste }, { "copypath", copy_path_text }, { "properties", properties },
        { "terminal", terminal }, { "newwin", new_window }, { "close", close_window }, { "context", context },
        { NULL, NULL },
    };

    ui_load_user_theme();
    if (user_current(&me) < 0)
        strlcpy(me.home, "/", sizeof(me.home));
    if (!(win = ui_load_string_named(page, handlers, NULL, "files")))
        return 1;
    table = ui_get(win, "files");
    places = ui_get(win, "places");
    trash_dir(trash_files, sizeof(trash_files));
    for (int i = 0; i < NPLACES; i++) {
        ui_list_add(places, place_names[i]);
        ui_list_set_icon_shared(places, i, icon_get(place_icons[i], 20));
    }
    load(argc > 1 ? argv[1] : me.home, true);
    ui_window_show(win);
    ui_focus(table);
    return ui_run();
}
