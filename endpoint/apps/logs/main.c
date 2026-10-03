#include "aegis.h"
#include "ui.h"

// Log Viewer: the kernel and system log (normally only visible on the text
// console), the shift log, and log files.

static const char page[] =
    "<window title='Log Viewer' width='900' height='600' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <dropdown id='source' width='260' onchange='source'/>"
    "    <input id='filter' width='240' placeholder='Show lines containing...' onchange='filter'/>"
    "    <checkbox id='follow' text='Follow new lines' checked='true'/>"
    "    <spacer/>"
    "    <button flat='true' symbol='refresh' onclick='reload'/>"
    "    <button flat='true' text='Save copy...' onclick='save'/>"
    "  </toolbar>"
    "  <textarea id='text' expand='1' mono='true' readonly='true'/>"
    "  <statusbar><label id='status'/></statusbar>"
    "</window>";

struct source {
    char name[64];
    char path[256];
};

static struct ui_window *win;
static struct source sources[16];
static int nsources, current = -1, kmsg_fd = -1;
static char *log_text;
static size_t log_len, log_cap;

static void append_log(const char *s, size_t n)
{
    if (log_len + n + 1 > log_cap) {
        size_t cap = MAX(log_cap * 2, log_len + n + 4096);
        char *t = realloc(log_text, cap);

        if (!t)
            return;
        log_text = t;
        log_cap = cap;
    }
    memcpy(log_text + log_len, s, n);
    log_len += n;
    log_text[log_len] = 0;
}

static void show(void)
{
    const char *filter = ui_text(ui_get(win, "filter"));
    struct widget *t = ui_get(win, "text");
    char status[128];
    int lines = 0, shown = 0;

    if (!log_text) {
        ui_set_text(t, "");
        return;
    }
    if (!*filter) {
        ui_set_text(t, log_text);
        for (size_t i = 0; i < log_len; i++)
            lines += log_text[i] == '\n';
        shown = lines;
    } else {
        // Only the matching lines.
        char *out = malloc(log_len + 1), *o = out;
        const char *p = log_text;

        if (!out)
            return;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
            char saved;
            bool match = false;

            for (size_t i = 0; i + strlen(filter) <= len && !match; i++)
                match = !strncasecmp(p + i, filter, strlen(filter));
            (void)saved;
            lines++;
            if (match) {
                memcpy(o, p, len);
                o += len;
                shown++;
            }
            p += len;
        }
        *o = 0;
        ui_set_text(t, out);
        free(out);
    }
    if (ui_value(ui_get(win, "follow")) && shown)
        ui_textarea_select(t, strlen(ui_text(t)), strlen(ui_text(t)));
    snprintf(status, sizeof(status), "%s: %d lines%s", sources[current].name, lines,
             *filter ? "" : "");
    if (*filter)
        snprintf(status + strlen(status), sizeof(status) - strlen(status), ", %d shown", shown);
    ui_set_text(ui_get(win, "status"), status);
}

static void kmsg_ready(int fd, void *u)
{
    char buf[8192];
    ssize_t n;
    bool any = false;

    (void)u;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        append_log(buf, n);
        any = true;
    }
    if (any && ui_value(ui_get(win, "follow")))
        show();
}

static bool load_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    char buf[8192];
    ssize_t n;

    if (fd < 0)
        return false;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        append_log(buf, n);
    close(fd);
    return true;
}

static void open_source(int i)
{
    char msg[400];

    if (kmsg_fd >= 0) {
        ui_unwatch_fd(kmsg_fd);
        close(kmsg_fd);
        kmsg_fd = -1;
    }
    current = i;
    log_len = 0;
    if (log_text)
        log_text[0] = 0;
    if (!strcmp(sources[i].path, "/osystem/devices/klog")) {
        if ((kmsg_fd = open("/osystem/devices/klog", O_RDONLY | O_NONBLOCK)) < 0
            && ui_elevate(win, "The system log is only for administrators."))
            kmsg_fd = open("/osystem/devices/klog", O_RDONLY | O_NONBLOCK);
        if (kmsg_fd < 0) {
            snprintf(msg, sizeof(msg), "The system log cannot be read: %s.\n", strerror(errno));
            append_log(msg, strlen(msg));
        } else {
            kmsg_ready(kmsg_fd, NULL);
            ui_watch_fd(kmsg_fd, kmsg_ready, NULL);
        }
    } else if (!load_file(sources[i].path)) {
        snprintf(msg, sizeof(msg), "%s cannot be read: %s.\n", sources[i].path, strerror(errno));
        append_log(msg, strlen(msg));
    }
    show();
}

static void add_source(const char *name, const char *path)
{
    if (nsources == 16)
        return;
    strlcpy(sources[nsources].name, name, sizeof(sources[nsources].name));
    strlcpy(sources[nsources].path, path, sizeof(sources[nsources].path));
    ui_list_add(ui_get(win, "source"), name);
    nsources++;
}

static bool follow_files(void *u)
{
    struct aegis_stat st;
    static int64_t last_size = -1;

    (void)u;
    // Plain files: reload when they grow.
    if (current < 0 || kmsg_fd >= 0 || !ui_value(ui_get(win, "follow")))
        return true;
    if (stat(sources[current].path, &st) == 0 && (int64_t)st.size != last_size) {
        bool first = last_size < 0;

        last_size = st.size;
        if (!first)
            open_source(current);
    }
    return true;
}

static void on_source(struct widget *w, void *u)
{
    (void)u;
    if (ui_list_selected(w) >= 0)
        open_source(ui_list_selected(w));
}

static void on_filter(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    show();
}

static void on_reload(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    open_source(current);
}

static void on_save(struct widget *w, void *u)
{
    char *p = ui_file_dialog(win, "Save a copy", NULL, true, "log.txt");
    const char *t = ui_text(ui_get(win, "text"));
    int fd;

    (void)w;
    (void)u;
    if (!p)
        return;
    if ((fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        write(fd, t, strlen(t));
        close(fd);
    } else {
        ui_message(win, "Log Viewer", "The copy could not be saved.", "OK");
    }
    free(p);
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "source", on_source }, { "filter", on_filter }, { "reload", on_reload }, { "save", on_save },
        { NULL, NULL },
    };
    struct user_info me;
    struct dir_stream *d;
    struct aegis_dirent *e;
    char path[256];

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "logs")))
        return 1;
    add_source("Kernel and system", "/osystem/devices/klog");
    if (user_current(&me) == 0) {
        user_path(&me, "system/shift.log", path, sizeof(path));
        add_source("This shift", path);
    }
    if ((d = opendir("/osystem/logs"))) {
        while ((e = readdir(d))) {
            if (e->name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "/osystem/logs/%s", e->name);
            add_source(e->name, path);
        }
        closedir(d);
    }
    if (argc > 1)
        add_source(strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1], argv[1]);
    ui_list_select(ui_get(win, "source"), argc > 1 ? nsources - 1 : 0);
    ui_window_show(win);
    open_source(argc > 1 ? nsources - 1 : 0);
    ui_timer(1000, follow_files, NULL);
    return ui_run();
}
