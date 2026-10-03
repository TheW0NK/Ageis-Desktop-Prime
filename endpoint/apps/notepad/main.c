#include "aegis.h"
#include "ui.h"

// Notepad: a plain text editor.

static const char page[] =
    "<window title='Notepad' width='760' height='540' padding='0' spacing='0' onclose='quit'>"
    "  <menubar>"
    "    <menu text='File'>"
    "      <item text='New' shortcut='Ctrl+N' onclick='new'/>"
    "      <item text='New window' shortcut='Ctrl+Shift+N' onclick='newwin'/>"
    "      <item text='Open...' shortcut='Ctrl+O' onclick='open'/>"
    "      <item text='Save' shortcut='Ctrl+S' onclick='save'/>"
    "      <item text='Save as...' shortcut='Ctrl+Shift+S' onclick='saveas'/>"
    "      <separator/>"
    "      <item text='Quit' shortcut='Ctrl+Q' onclick='quit'/>"
    "    </menu>"
    "    <menu text='Edit'>"
    "      <item text='Undo' shortcut='Ctrl+Z' onclick='undo'/>"
    "      <item text='Redo' shortcut='Ctrl+Y' onclick='redo'/>"
    "      <separator/>"
    "      <item text='Cut' shortcut='Ctrl+X' onclick='cut'/>"
    "      <item text='Copy' shortcut='Ctrl+C' onclick='copy'/>"
    "      <item text='Paste' shortcut='Ctrl+V' onclick='paste'/>"
    "      <item text='Delete' onclick='delete'/>"
    "      <separator/>"
    "      <item text='Find...' shortcut='Ctrl+F' onclick='find'/>"
    "      <item text='Find next' shortcut='F3' onclick='next'/>"
    "      <item text='Replace...' shortcut='Ctrl+H' onclick='replace'/>"
    "      <item text='Go to line...' shortcut='Ctrl+G' onclick='goto'/>"
    "      <separator/>"
    "      <item text='Select all' shortcut='Ctrl+A' onclick='selectall'/>"
    "      <item text='Insert date and time' shortcut='F5' onclick='datetime'/>"
    "    </menu>"
    "    <menu text='View'>"
    "      <item id='wrap' text='Word wrap' checked='true' onclick='wrap'/>"
    "      <item id='mono' text='Monospaced font' onclick='mono'/>"
    "      <item id='statusitem' text='Status bar' checked='true' onclick='status'/>"
    "    </menu>"
    "  </menubar>"
    "  <hbox id='findbar' hidden='true' padding='6' spacing='6'>"
    "    <input id='findtext' width='220' placeholder='Find' onactivate='next' onchange='findlive'/>"
    "    <button text='Previous' onclick='prev'/>"
    "    <button text='Next' onclick='next'/>"
    "    <checkbox id='case' text='Match case'/>"
    "    <input id='replacetext' width='180' placeholder='Replace with' hidden='true'/>"
    "    <button id='replaceone' text='Replace' hidden='true' onclick='replaceone'/>"
    "    <button id='replaceall' text='Replace all' hidden='true' onclick='replaceall'/>"
    "    <spacer/>"
    "    <button flat='true' symbol='close' onclick='closefind'/>"
    "  </hbox>"
    "  <textarea id='text' expand='1' wrap='true' tabfocus='false'/>"
    "  <statusbar id='statusbar'>"
    "    <label id='pos' text='Line 1, column 1'/>"
    "    <spacer/>"
    "    <label id='info' dim='true' text='UTF-8'/>"
    "  </statusbar>"
    "</window>";

static struct ui_window *win;
static struct widget *text;
static char path[512];
static int last_cursor = -1;

static const char *base_name(void)
{
    const char *s = strrchr(path, '/');

    return *path ? (s ? s + 1 : path) : "Untitled";
}

static void update_title(void)
{
    char t[600];

    snprintf(t, sizeof(t), "%s%s - Notepad", ui_textarea_modified(text) ? "*" : "", base_name());
    ui_window_set_title(win, t);
}

static void update_status(void)
{
    const char *s = ui_text(text);
    int cur = ui_textarea_cursor(text), line = 1, col = 1, lines = 1;
    char buf[96];

    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') {
            lines++;
            if (i < cur) {
                line++;
                col = 1;
            }
        } else if (i < cur && (s[i] & 0xC0) != 0x80) {
            col++;
        }
    }
    snprintf(buf, sizeof(buf), "Line %d, column %d", line, col);
    ui_set_text(ui_get(win, "pos"), buf);
    snprintf(buf, sizeof(buf), "%d lines   UTF-8", lines);
    ui_set_text(ui_get(win, "info"), buf);
}

static bool tick(void *u)
{
    static bool was_modified;
    int cur = ui_textarea_cursor(text);

    (void)u;
    if (cur != last_cursor) {
        last_cursor = cur;
        update_status();
    }
    if (ui_textarea_modified(text) != was_modified) {
        was_modified = ui_textarea_modified(text);
        update_title();
    }
    return true;
}

static bool load_file(const char *p)
{
    int fd = open(p, O_RDONLY);
    struct aegis_stat st;
    char *buf, msg[700];
    ssize_t n, got = 0;

    if (fd < 0 || fstat(fd, &st) < 0) {
        snprintf(msg, sizeof(msg), "\"%s\" cannot be opened: %s.", p, strerror(errno));
        ui_message(win, "Notepad", msg, "OK");
        if (fd >= 0)
            close(fd);
        return false;
    }
    if (st.size > 32 << 20) {
        ui_message(win, "Notepad", "This file is too large for Notepad.", "OK");
        close(fd);
        return false;
    }
    if (!(buf = malloc(st.size + 1))) {
        close(fd);
        return false;
    }
    while (got < (ssize_t)st.size && (n = read(fd, buf + got, st.size - got)) > 0)
        got += n;
    close(fd);
    buf[got] = 0;
    if (memchr(buf, 0, got)) {
        if (ui_message(win, "Notepad", "This looks like a binary file. Open it anyway?", "Open|Cancel") != 0) {
            free(buf);
            return false;
        }
        for (ssize_t i = 0; i < got; i++)
            if (!buf[i])
                buf[i] = ' ';
    }
    // Windows line endings become plain newlines.
    {
        char *d = buf;

        for (char *s = buf; *s; s++)
            if (!(*s == '\r' && s[1] == '\n'))
                *d++ = *s;
        *d = 0;
    }
    ui_set_text(text, buf);
    free(buf);
    strlcpy(path, p, sizeof(path));
    ui_textarea_set_modified(text, false);
    update_title();
    update_status();
    return true;
}

static bool write_file(const char *p)
{
    char tmp[600], msg[700];
    const char *s = ui_text(text);
    size_t len = strlen(s), off = 0;
    int fd;

    snprintf(tmp, sizeof(tmp), "%s.saving", p);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
        goto fail;
    while (off < len) {
        ssize_t w = write(fd, s + off, len - off);

        if (w <= 0) {
            close(fd);
            unlink(tmp);
            goto fail;
        }
        off += w;
    }
    close(fd);
    if (rename(tmp, p) < 0) {
        unlink(tmp);
        goto fail;
    }
    strlcpy(path, p, sizeof(path));
    ui_textarea_set_modified(text, false);
    update_title();
    return true;
fail:
    snprintf(msg, sizeof(msg), "\"%s\" could not be saved: %s.", p, strerror(errno));
    ui_message(win, "Notepad", msg, "OK");
    return false;
}

static bool save_as(void)
{
    char *p = ui_file_dialog(win, "Save as", NULL, true, *path ? base_name() : "Untitled.txt");
    bool ok;

    if (!p)
        return false;
    ok = write_file(p);
    free(p);
    return ok;
}

static bool save(void)
{
    return *path ? write_file(path) : save_as();
}

// Asks about unsaved changes. Returns false to cancel what was going on.
static bool settle(void)
{
    char msg[600];
    int r;

    if (!ui_textarea_modified(text))
        return true;
    snprintf(msg, sizeof(msg), "Save the changes to \"%s\"?", base_name());
    r = ui_message(win, "Notepad", msg, "Save|Don't save|Cancel");
    if (r == 0)
        return save();
    return r == 1;
}

static void on_new(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!settle())
        return;
    ui_set_text(text, "");
    path[0] = 0;
    ui_textarea_set_modified(text, false);
    update_title();
}

static void on_newwin(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    launch("/sysapps/notepad", NULL);
}

static void on_open(struct widget *w, void *u)
{
    char *p;

    (void)w;
    (void)u;
    if (!settle() || !(p = ui_file_dialog(win, "Open", NULL, false, NULL)))
        return;
    load_file(p);
    free(p);
}

static void on_save(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    save();
}

static void on_saveas(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    save_as();
}

static void on_quit(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (settle())
        ui_quit(0);
}

#define EDIT(name, cmd) \
    static void name(struct widget *w, void *u) { (void)w; (void)u; ui_edit_command(text, cmd); }
EDIT(on_undo, "undo")
EDIT(on_redo, "redo")
EDIT(on_cut, "cut")
EDIT(on_copy, "copy")
EDIT(on_paste, "paste")
EDIT(on_delete, "delete")
EDIT(on_selectall, "selectall")

// ---- Find and replace ----

static const char *find_in(const char *hay, const char *needle, bool match_case)
{
    size_t n = strlen(needle);

    for (; *hay; hay++)
        if (match_case ? !strncmp(hay, needle, n) : !strncasecmp(hay, needle, n))
            return hay;
    return NULL;
}

static bool find(bool backwards, bool from_start)
{
    const char *s = ui_text(text), *needle = ui_text(ui_get(win, "findtext")), *hit = NULL;
    bool mc = ui_value(ui_get(win, "case")) != 0;
    int start, end;

    if (!*needle)
        return false;
    ui_textarea_selection(text, &start, &end);
    if (backwards) {
        // The last match that starts before the selection.
        for (const char *p = find_in(s, needle, mc); p && p - s < start; p = find_in(p + 1, needle, mc))
            hit = p;
        if (!hit)
            for (const char *p = find_in(s, needle, mc); p; p = find_in(p + 1, needle, mc))
                hit = p;
    } else {
        hit = find_in(s + (from_start ? start : end), needle, mc);
        if (!hit)
            hit = find_in(s, needle, mc);       // wrap around
    }
    if (!hit) {
        ui_set_text(ui_get(win, "pos"), "Not found");
        return false;
    }
    ui_textarea_select(text, hit - s, hit - s + strlen(needle));
    return true;
}

static void show_find(bool replace)
{
    ui_set_visible(ui_get(win, "findbar"), true);
    ui_set_visible(ui_get(win, "replacetext"), replace);
    ui_set_visible(ui_get(win, "replaceone"), replace);
    ui_set_visible(ui_get(win, "replaceall"), replace);
    ui_focus(ui_get(win, "findtext"));
}

static void on_find(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    show_find(false);
}

static void on_replace(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    show_find(true);
}

static void on_next(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!ui_visible(ui_get(win, "findbar")))
        show_find(false);
    else
        find(false, false);
}

static void on_prev(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    find(true, false);
}

static void on_findlive(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    find(false, true);
}

static void on_closefind(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_set_visible(ui_get(win, "findbar"), false);
    ui_focus(text);
}

static void on_replaceone(struct widget *w, void *u)
{
    const char *needle = ui_text(ui_get(win, "findtext"));
    int start, end;

    (void)w;
    (void)u;
    ui_textarea_selection(text, &start, &end);
    // Replace the current match, then find the next one.
    if (end > start && (size_t)(end - start) == strlen(needle))
        ui_textarea_insert(text, ui_text(ui_get(win, "replacetext")));
    find(false, false);
}

static void on_replaceall(struct widget *w, void *u)
{
    const char *s = ui_text(text), *needle = ui_text(ui_get(win, "findtext"));
    const char *with = ui_text(ui_get(win, "replacetext"));
    bool mc = ui_value(ui_get(win, "case")) != 0;
    size_t nl = strlen(needle), wl = strlen(with), cap = strlen(s) + 1, len = 0;
    char *out, msg[64];
    int count = 0;

    (void)w;
    (void)u;
    if (!nl || !(out = malloc(cap)))
        return;
    for (const char *p = s; *p;) {
        if (mc ? !strncmp(p, needle, nl) : !strncasecmp(p, needle, nl)) {
            if (len + wl + 1 > cap && !(out = realloc(out, cap = (len + wl + 1) * 2)))
                return;
            memcpy(out + len, with, wl);
            len += wl;
            p += nl;
            count++;
        } else {
            if (len + 2 > cap && !(out = realloc(out, cap *= 2)))
                return;
            out[len++] = *p++;
        }
    }
    out[len] = 0;
    if (count) {
        // As one undoable edit.
        ui_edit_command(text, "selectall");
        ui_textarea_insert(text, out);
    }
    free(out);
    snprintf(msg, sizeof(msg), "Replaced %d", count);
    ui_set_text(ui_get(win, "pos"), msg);
}

static void on_goto(struct widget *w, void *u)
{
    char *line = ui_prompt(win, "Go to line", "Line number:", "1");
    const char *s = ui_text(text);
    int n, i = 0;

    (void)w;
    (void)u;
    if (!line)
        return;
    n = atoi(line);
    free(line);
    for (int l = 1; l < n && s[i]; i++)
        if (s[i] == '\n')
            l++;
    ui_textarea_select(text, i, i);
    ui_focus(text);
}

static void on_datetime(struct widget *w, void *u)
{
    int64_t now = time(NULL);
    struct tm tm;
    char buf[64];

    (void)w;
    (void)u;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M %Y-%m-%d", &tm);
    ui_textarea_insert(text, buf);
}

static void toggle(const char *item, const char *attr)
{
    struct widget *it = ui_get(win, item);
    bool on = !ui_attr_true(ui_attr(it, "checked"));

    ui_set_attr(it, "checked", on ? "true" : "false");
    ui_set_attr(text, attr, on ? "true" : "false");
    ui_redraw(text);
}

static void on_wrap(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    toggle("wrap", "wrap");
}

static void on_mono(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    toggle("mono", "mono");
}

static void on_status(struct widget *w, void *u)
{
    struct widget *it = ui_get(win, "statusitem");
    bool on = !ui_attr_true(ui_attr(it, "checked"));

    (void)w;
    (void)u;
    ui_set_attr(it, "checked", on ? "true" : "false");
    ui_set_visible(ui_get(win, "statusbar"), on);
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "new", on_new }, { "newwin", on_newwin }, { "open", on_open }, { "save", on_save },
        { "saveas", on_saveas }, { "quit", on_quit }, { "undo", on_undo }, { "redo", on_redo }, { "cut", on_cut },
        { "copy", on_copy }, { "paste", on_paste }, { "delete", on_delete }, { "selectall", on_selectall },
        { "find", on_find }, { "next", on_next }, { "prev", on_prev }, { "findlive", on_findlive },
        { "closefind", on_closefind }, { "replace", on_replace }, { "replaceone", on_replaceone },
        { "replaceall", on_replaceall }, { "goto", on_goto }, { "datetime", on_datetime }, { "wrap", on_wrap },
        { "mono", on_mono }, { "status", on_status }, { NULL, NULL },
    };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "notepad")))
        return 1;
    text = ui_get(win, "text");
    if (argc > 1) {
        struct aegis_stat st;

        // A name that does not exist yet becomes a new file there.
        if (stat(argv[1], &st) == 0)
            load_file(argv[1]);
        else
            strlcpy(path, argv[1], sizeof(path));
    }
    update_title();
    update_status();
    ui_timer(200, tick, NULL);
    ui_window_show(win);
    ui_focus(text);
    return ui_run();
}
