#include "desktop.h"

// Notifications: programs send them with notify(); each shows for a few
// seconds above the panel, and the bell in the panel lists the recent ones.

#define TOAST_WIDTH 360
#define MAX_KEPT    40
#define SHOW_MS     5000

struct note {
    char app[48], title[96], body[400];
    int64_t when;
};

static const char toast_page[] =
    "<window padding='12' spacing='10'><vbox id='notes' spacing='10'/></window>";

static const char center_page[] =
    "<window padding='12' spacing='8'>"
    "  <hbox><label text='Notifications' bold='true' expand='true'/>"
    "    <button flat='true' text='Clear' onclick='clear'/></hbox>"
    "  <separator/>"
    "  <scroll id='scroll' height='320'><vbox id='list' spacing='10'/></scroll>"
    "  <label id='empty' text='Nothing new.' dim='true'/>"
    "</window>";

static struct note notes[MAX_KEPT];
static int nnotes, unread;
static struct ui_window *toast, *center, *panel_win;
static struct widget *bell;
static int hide_timer = -1, lfd = -1;
static uint64_t center_hidden_at;

static void add_note_widgets(struct ui_window *win, struct widget *box, const struct note *n, bool with_time)
{
    struct widget *v = ui_create(win, "vbox"), *head = ui_create(win, "label"), *title = ui_create(win, "label"),
                  *body = ui_create(win, "label");
    char line[160];

    ui_set_attr(v, "spacing", "2");
    if (with_time) {
        struct tm tm;
        char when[16];

        localtime_r(&n->when, &tm);
        strftime(when, sizeof(when), "%H:%M", &tm);
        snprintf(line, sizeof(line), "%s  \xC2\xB7  %s", n->app, when);
    } else {
        strlcpy(line, n->app, sizeof(line));
    }
    ui_set_text(head, line);
    ui_set_attr(head, "dim", "true");
    ui_set_attr(head, "size", "small");
    ui_set_text(title, n->title);
    ui_set_attr(title, "bold", "true");
    ui_set_text(body, n->body);
    ui_set_attr(body, "wrap", "true");
    ui_add(v, head);
    ui_add(v, title);
    if (*n->body)
        ui_add(v, body);
    ui_add(box, v);
}

static void clear_box(struct widget *box)
{
    while (ui_children(box))
        ui_remove(ui_child(box, 0));
}

static void update_bell(void)
{
    char tip[48];

    snprintf(tip, sizeof(tip), unread ? "%d new notification%s" : "Notifications", unread, unread == 1 ? "" : "s");
    ui_set_attr(bell, "tooltip", tip);
    ui_set_attr(bell, "dim", unread ? "false" : "true");
    ui_set_text(bell, unread ? "\xE2\x80\xA2" : "");
}

// Places a popup above the panel, at the right.
static void place(struct ui_window *w, int width)
{
    struct rect r = ui_rect(ui_root(panel_win));
    int h;

    ui_window_set_size(w, width, 10);
    ui_window_fit(w);
    h = ui_wm_window(w) ? ui_wm_window(w)->height : 120;
    ui_window_set_size(w, width, h);
    ui_window_move(w, r.w - width - 6, -h - 6);
}

static bool hide_toast(void *u)
{
    (void)u;
    hide_timer = -1;
    ui_window_hide(toast);
    return false;
}

static void show_toast(void)
{
    struct widget *box = ui_get(toast, "notes");
    int shown = 0;

    clear_box(box);
    // The newest few.
    for (int i = nnotes - 1; i >= 0 && shown < 3; i--, shown++)
        if (notes[i].when > time(NULL) - 10)
            add_note_widgets(toast, box, &notes[i], false);
    if (!ui_children(box))
        return;
    place(toast, TOAST_WIDTH);
    ui_window_show(toast);
    if (hide_timer >= 0)
        ui_timer_cancel(hide_timer);
    hide_timer = ui_timer(SHOW_MS, hide_toast, NULL);
}

static void received(int fd, void *u)
{
    char msg[1024], *f[3] = { msg, "", "" };
    struct note *n;
    ssize_t len = recv(fd, msg, sizeof(msg) - 1, 0);
    int k = 1;

    (void)u;
    ui_unwatch_fd(fd);
    close(fd);
    if (len <= 0)
        return;
    msg[len] = 0;
    for (char *p = msg; k < 3 && (p = strchr(p, '\n')); k++) {
        *p++ = 0;
        f[k] = p;
    }
    if (nnotes == MAX_KEPT) {
        memmove(&notes[0], &notes[1], (MAX_KEPT - 1) * sizeof(notes[0]));
        nnotes--;
    }
    n = &notes[nnotes++];
    strlcpy(n->app, *f[0] ? f[0] : "Aegis", sizeof(n->app));
    strlcpy(n->title, f[1], sizeof(n->title));
    strlcpy(n->body, f[2], sizeof(n->body));
    n->when = time(NULL);
    unread++;
    update_bell();
    show_toast();
}

static void accept_note(int fd, void *u)
{
    int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);

    (void)u;
    if (c >= 0)
        ui_watch_fd(c, received, NULL);
}

static void rebuild_center(void)
{
    struct widget *list = ui_get(center, "list");

    clear_box(list);
    for (int i = nnotes - 1; i >= 0; i--)
        add_note_widgets(center, list, &notes[i], true);
    ui_set_visible(ui_get(center, "empty"), nnotes == 0);
    ui_set_visible(ui_get(center, "scroll"), nnotes > 0);
}

static void on_clear(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    nnotes = unread = 0;
    update_bell();
    rebuild_center();
    ui_window_hide(center);
}

static void on_bell(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_wm_window(center) && ui_wm_window(center)->visible) {
        ui_window_hide(center);
        return;
    }
    if (uptime_ms() - center_hidden_at < 300)
        return;
    unread = 0;
    update_bell();
    rebuild_center();
    if (toast)
        ui_window_hide(toast);
    place(center, 380);
    ui_window_show(center);
}

static bool center_dismissed(struct ui_window *w, void *u)
{
    (void)u;
    center_hidden_at = uptime_ms();
    ui_window_hide(w);
    return false;
}

void notify_init(struct ui_window *panel, struct widget *button)
{
    static const struct ui_handler_entry handlers[] = { { "clear", on_clear }, { NULL, NULL } };
    char name[48];

    panel_win = panel;
    bell = button;
    ui_set_handler(bell, "click", on_bell, NULL);
    if ((toast = ui_load_string_named(toast_page, NULL, NULL, "toast"))) {
        ui_window_set_parent(toast, panel);
        ui_window_set_flags(toast, WM_ROLE_POPUP);
        ui_window_hide(toast);
    }
    if ((center = ui_load_string_named(center_page, handlers, NULL, "notifications"))) {
        ui_window_set_parent(center, panel);
        ui_window_set_flags(center, WM_ROLE_POPUP);
        ui_window_hide(center);
        ui_on_close(center, center_dismissed, NULL);
    }
    snprintf(name, sizeof(name), "@aegis/notify-%u", getuid());
    if ((lfd = unix_listen(name, SOCK_SEQPACKET)) >= 0)
        ui_watch_fd(lfd, accept_note, NULL);
    update_bell();
}
