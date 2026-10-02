#include "aegis.h"
#include "ui.h"

// Management Console: one window over the computer's parts. Each entry on
// the left is a snap-in with a table and actions.

static const char page[] =
    "<window title='Management Console' width='980' height='620' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <button flat='true' symbol='refresh' text='Refresh' onclick='refresh' shortcut='F5'/>"
    "    <spacer/>"
    "    <label id='lockstate' dim='true' align='center'/>"
    "    <button id='unlock' flat='true' symbol='lock' text='Unlock' onclick='unlock'/>"
    "  </toolbar>"
    "  <hbox expand='1' padding='10' spacing='10'>"
    "    <list id='snapins' width='220' onselect='snapin'/>"
    "    <vbox expand='1' spacing='8'>"
    "      <h2 id='title'/>"
    "      <p id='about' dim='true'/>"
    "      <table id='table' expand='1' onactivate='primary' oncontext='context'/>"
    "      <hbox id='actions' spacing='8'/>"
    "    </vbox>"
    "  </hbox>"
    "  <statusbar><label id='status'/></statusbar>"
    "</window>";

struct action {
    const char *label;
    void (*fn)(void);
};

struct snapin {
    const char *name, *icon, *about, *columns;
    void (*fill)(struct widget *table);
    struct action actions[4];
};

static struct ui_window *win;
static int current;

static void open_app(const char *id, const char *arg)
{
    struct app_info a;

    if (app_find(id, &a) == 0)
        app_launch(&a, arg);
}

static void status(const char *s)
{
    ui_set_text(ui_get(win, "status"), s);
}

static int selected(char *col0, size_t size)
{
    struct widget *t = ui_get(win, "table");
    int i = ui_list_selected(t);

    if (i >= 0 && col0)
        ui_list_column(t, i, 0, col0, size);
    return i;
}

// ---- Computer ----

static void fill_computer(struct widget *t)
{
    struct aegis_utsname un;
    struct aegis_sysinfo si;
    char row[256], a[32], b[32];
    int fd;

    if (uname(&un) == 0) {
        snprintf(row, sizeof(row), "Operating system\t%s %s (%s)", un.sysname, un.release, un.machine);
        ui_list_add(t, row);
    }
    if ((fd = open("/etc/hostname", O_RDONLY)) >= 0) {
        char h[64];

        if (read_line(fd, h, sizeof(h)) > 0) {
            snprintf(row, sizeof(row), "Computer name\t%s", h);
            ui_list_add(t, row);
        }
        close(fd);
    }
    if (sysinfo(&si) == 0) {
        snprintf(row, sizeof(row), "Processors\t%u", si.cpus);
        ui_list_add(t, row);
        ui_format_size(si.memory_total, a, sizeof(a));
        ui_format_size(si.memory_free, b, sizeof(b));
        snprintf(row, sizeof(row), "Memory\t%s (%s free)", a, b);
        ui_list_add(t, row);
        snprintf(row, sizeof(row), "Processes\t%u processes, %u threads", si.processes, si.threads);
        ui_list_add(t, row);
        snprintf(row, sizeof(row), "Running for\t%lu min", (unsigned long)(si.uptime_ms / 60000));
        ui_list_add(t, row);
    }
    snprintf(row, sizeof(row), "Time zone\t%s", timezone_name());
    ui_list_add(t, row);
}

static void act_settings(void) { open_app("settings", "About"); }
static void act_resources(void) { open_app("resources", NULL); }

// ---- Users and groups ----

static void fill_users(struct widget *t)
{
    struct user_info users[64];
    int n = user_list(users, 64);

    for (int i = 0; i < n; i++) {
        char row[400];

        snprintf(row, sizeof(row), "%s\t%s\t%u\t%s\t%s", users[i].name, users[i].display, users[i].uid,
                 account_is_admin(users[i].name) ? "Administrator" : "Standard", users[i].home);
        ui_list_add(t, row);
        ui_list_set_icon_shared(t, i, icon_get("glyph:user", 18));
    }
}

static void act_users(void) { open_app("users", NULL); }

static void fill_groups(struct widget *t)
{
    int fd = open("/etc/group", O_RDONLY);
    char line[512];

    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) > 0) {
        char *f[4] = { line, NULL, NULL, NULL }, row[600];
        int k = 1;

        for (char *p = line; *p && k < 4; p++)
            if (*p == ':') {
                *p = 0;
                f[k++] = p + 1;
            }
        if (k < 4)
            continue;
        snprintf(row, sizeof(row), "%s\t%s\t%s", f[0], f[2], *f[3] ? f[3] : "-");
        ui_list_add(t, row);
    }
    close(fd);
}

// ---- Services ----

static const struct {
    const char *name, *path, *about;
} services[] = {
    { "init", "/sbin/init", "Starts and keeps the services below running" },
    { "privd", "/sbin/privd", "Lets users change their own password" },
    { "crond", "/sbin/crond", "Runs scheduled jobs" },
    { "compositor", "/sbin/compositor", "The display and window system" },
    { "greeter", "/sbin/greeter", "The sign-in screen" },
    { "terminal", "/bin/terminal", "Text console shells" },
};

static void fill_services(struct widget *t)
{
    struct aegis_procinfo p[256];
    int n = procinfo(p, 256);

    for (size_t s = 0; s < sizeof(services) / sizeof(services[0]); s++) {
        int pid = -1, count = 0;
        uint64_t mem = 0;
        char row[300], m[32];

        for (int i = 0; i < n; i++)
            if (!strcmp(p[i].name, services[s].name)) {
                if (pid < 0)
                    pid = p[i].pid;
                count++;
                mem += p[i].memory;
            }
        ui_format_size(mem, m, sizeof(m));
        snprintf(row, sizeof(row), "%s\t%s\t%s\t%s\t%s", services[s].name, pid > 0 ? "Running" : "Stopped",
                 pid > 0 ? (count > 1 ? "several" : "") : "-", pid > 0 ? m : "-", services[s].about);
        if (pid > 0 && count == 1)
            snprintf(row, sizeof(row), "%s\tRunning\t%d\t%s\t%s", services[s].name, pid, m, services[s].about);
        ui_list_add(t, row);
        ui_list_set_icon_shared(t, s, icon_get(pid > 0 ? "glyph:play" : "glyph:stop", 18));
    }
}

static void act_restart_service(void)
{
    char name[64], msg[200];
    struct aegis_procinfo p[256];
    int n;

    if (selected(name, sizeof(name)) < 0)
        return;
    if (!strcmp(name, "init") || !strcmp(name, "compositor") || !strcmp(name, "greeter")) {
        ui_message(win, "Management Console",
                   "Restarting this service would end the desktop session. Restart the computer instead.", "OK");
        return;
    }
    if (!ui_elevate(win, "Restarting a service affects everyone on this computer."))
        return;
    n = procinfo(p, 256);
    for (int i = 0; i < n; i++)
        if (!strcmp(p[i].name, name) && p[i].ppid == 1)
            kill(p[i].pid, SIGTERM);
    // init starts it again.
    msleep(1500);
    snprintf(msg, sizeof(msg), "%s was restarted.", name);
    status(msg);
    syslog("console", "restarted %s", name);
}

// ---- Scheduled jobs ----

static void add_crontab(struct widget *t, const char *path, bool system, const char *owner)
{
    int fd = open(path, O_RDONLY);
    char line[512];
    struct cron_job j;

    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) > 0) {
        char row[700];

        if (!cron_parse(line, system, &j))
            continue;
        snprintf(row, sizeof(row), "%s\t%s\t%s\t%s\t%s", *j.name ? j.name : "-", system ? j.user : owner,
                 j.schedule, j.enabled ? "On" : "Off", j.command);
        ui_list_add(t, row);
    }
    close(fd);
}

static void fill_jobs(struct widget *t)
{
    struct user_info users[64];
    int n = user_list(users, 64);

    add_crontab(t, "/etc/crontab", true, NULL);
    // Other users' jobs are private unless unlocked.
    for (int i = 0; i < n; i++) {
        char path[256];

        if (users[i].uid != getuid() && geteuid() != 0)
            continue;
        cron_user_path(&users[i], "crontab", path, sizeof(path));
        add_crontab(t, path, false, users[i].name);
    }
}

static void act_cron(void) { open_app("cron", NULL); }

// ---- Storage, devices, network ----

static void fill_storage(struct widget *t)
{
    static const char *const mounts[] = { "/", "/boot" };
    uint64_t last = 0;

    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        struct aegis_statfs fs;
        char row[300], total[32], free_s[32];

        if (statfs(mounts[i], &fs) < 0 || fs.blocks == last)
            continue;
        last = fs.blocks;
        ui_format_size(fs.blocks * fs.block_size, total, sizeof(total));
        ui_format_size(fs.blocks_free * fs.block_size, free_s, sizeof(free_s));
        snprintf(row, sizeof(row), "%s\t%s\t%s\t%s\t%.0f%%", mounts[i], fs.fstype, total, free_s,
                 fs.blocks ? 100.0 * (fs.blocks - fs.blocks_free) / fs.blocks : 0.0);
        ui_list_add(t, row);
        ui_list_set_icon_shared(t, ui_list_count(t) - 1, icon_get("glyph:disk", 18));
    }
}

static void fill_devices(struct widget *t)
{
    struct dir_stream *d = opendir("/dev");
    struct aegis_dirent *e;

    if (!d)
        return;
    while ((e = readdir(d))) {
        char path[128], row[256], owner[32];
        struct aegis_stat st;
        const char *kind = "Device";

        if (e->name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/dev/%s", e->name);
        if (stat(path, &st) < 0)
            continue;
        if (!strncmp(e->name, "sata", 4) || !strncmp(e->name, "nvme", 4))
            kind = "Disk";
        else if (!strcmp(e->name, "input"))
            kind = "Keyboard and pointer events";
        else if (!strcmp(e->name, "fb0"))
            kind = "Screen";
        else if (!strcmp(e->name, "kmsg"))
            kind = "System log";
        else if (!strncmp(e->name, "tty", 3) || !strcmp(e->name, "console") || !strncmp(e->name, "pts", 3))
            kind = "Terminal";
        else if (strstr(e->name, "random") || !strcmp(e->name, "null") || !strcmp(e->name, "zero"))
            kind = "Built in";
        snprintf(owner, sizeof(owner), "%u:%u", st.uid, st.gid);
        snprintf(row, sizeof(row), "%s\t%s\t%o\t%s", e->name, kind, st.mode & 0777, owner);
        ui_list_add(t, row);
    }
    closedir(d);
}

static void fill_network(struct widget *t)
{
    struct aegis_netif nif;

    for (int i = 0; netconfig(NETCONFIG_GET, i, &nif) == 0; i++) {
        char addr[20], gw[20], row[300], rx[24], tx[24];

        inet_format(nif.addr, addr);
        inet_format(nif.gateway, gw);
        ui_format_size(nif.rx_bytes, rx, sizeof(rx));
        ui_format_size(nif.tx_bytes, tx, sizeof(tx));
        snprintf(row, sizeof(row), "%s\t%s\t%s\t%u\t%s / %s", nif.name, nif.addr ? addr : "none",
                 nif.gateway ? gw : "-", nif.mtu, rx, tx);
        ui_list_add(t, row);
        ui_list_set_icon_shared(t, i, icon_get("glyph:network", 18));
    }
}

static void act_network(void) { open_app("settings", "Network"); }

// ---- Applications ----

static void fill_apps(struct widget *t)
{
    struct app_info apps[96];
    int n = app_list(apps, 96);

    for (int i = 0; i < n; i++) {
        char row[400];

        snprintf(row, sizeof(row), "%s\t%s\t%s\t%s\t%s", apps[i].name, *apps[i].suite ? apps[i].suite : "-",
                 apps[i].tier, apps[i].exec, apps[i].description);
        ui_list_add(t, row);
        ui_list_set_icon_shared(t, i, icon_get(apps[i].icon, 18));
    }
}

static void act_open_app(void)
{
    char name[64];
    struct app_info apps[96];
    int n = app_list(apps, 96);

    if (selected(name, sizeof(name)) < 0)
        return;
    for (int i = 0; i < n; i++)
        if (!strcmp(apps[i].name, name))
            app_launch(&apps[i], NULL);
}

// ---- Events ----

static void fill_events(struct widget *t)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    char *buf = malloc(256 * 1024), *p;
    ssize_t n, len = 0;

    if (fd < 0 || !buf) {
        ui_list_add(t, "\tThe system log is for administrators. Unlock to read it.");
        if (fd >= 0)
            close(fd);
        free(buf);
        return;
    }
    while (len < 256 * 1024 - 1 && (n = read(fd, buf + len, 256 * 1024 - 1 - len)) > 0)
        len += n;
    close(fd);
    buf[len] = 0;
    // Newest first.
    for (p = buf + len; p > buf;) {
        char *start = p - 1, row[600], *close_br;

        while (start > buf && start[-1] != '\n')
            start--;
        if (p - start > 1) {
            p[-1] == '\n' ? (p[-1] = 0) : 0;
            close_br = start[0] == '[' ? strchr(start, ']') : NULL;
            if (close_br) {
                *close_br = 0;
                snprintf(row, sizeof(row), "%s s\t%s", start + 1, close_br + 2);
            } else {
                snprintf(row, sizeof(row), "\t%s", start);
            }
            ui_list_add(t, row);
        }
        p = start;
        if (ui_list_count(t) >= 400)
            break;
    }
    free(buf);
}

static void act_logs(void) { open_app("logs", NULL); }
static void act_tasks(void) { open_app("tasks", NULL); }

static const struct snapin snapins[] = {
    { "Computer", "glyph:desktop", "A summary of this computer.", "Property:180|Value", fill_computer,
      { { "Settings", act_settings }, { "Resource Manager", act_resources } } },
    { "Users", "glyph:user", "The accounts that can sign in.",
      "User:110|Name:160|Number:80:right|Type:120|Home folder", fill_users, { { "Open User Manager", act_users } } },
    { "Groups", "glyph:users", "Groups give their members extra rights: sudo and adm are administrators.",
      "Group:140|Number:90:right|Members", fill_groups, { { "Open User Manager", act_users } } },
    { "Services", "glyph:gear", "Programs that run in the background for everyone.",
      "Service:120|State:90|Process:80:right|Memory:90:right|Purpose", fill_services,
      { { "Restart service", act_restart_service }, { "Task Manager", act_tasks } } },
    { "Scheduled jobs", "glyph:clock-glyph", "Commands that run on a schedule (system jobs and accounts' jobs).",
      "Job:150|Runs as:100|Schedule:130|State:60|Command", fill_jobs, { { "Open Cron Jobs", act_cron } } },
    { "Storage", "glyph:disk", "Mounted file systems and their free space.",
      "Mounted at:120|Type:80|Size:100:right|Free:100:right|Used:70:right", fill_storage, { { NULL, NULL } } },
    { "Devices", "glyph:cpu", "Devices the system offers to programs, in /dev.",
      "Device:140|Kind:220|Permissions:110|Owner", fill_devices, { { NULL, NULL } } },
    { "Network", "glyph:network", "Network interfaces and their addresses.",
      "Interface:110|Address:140|Gateway:140|MTU:70:right|Received / sent", fill_network,
      { { "Network settings", act_network } } },
    { "Applications", "glyph:apps", "Installed apps, their suites and the rights they need.",
      "App:170|Suite:120|Rights:90|Program:150|Description", fill_apps, { { "Open app", act_open_app } } },
    { "Events", "glyph:logs", "The newest messages from the kernel and services.", "Time:90:right|Message",
      fill_events, { { "Open Log Viewer", act_logs } } },
};

#define NSNAPINS ((int)(sizeof(snapins) / sizeof(snapins[0])))

static void action_clicked(struct widget *w, void *u)
{
    void (*fn)(void) = (void (*)(void))u;

    (void)w;
    fn();
}

static void show(int i)
{
    const struct snapin *s = &snapins[i];
    struct widget *t = ui_get(win, "table"), *actions = ui_get(win, "actions");
    char st[64];

    current = i;
    ui_set_text(ui_get(win, "title"), s->name);
    ui_set_text(ui_get(win, "about"), s->about);
    ui_set_attr(t, "columns", s->columns);
    ui_list_clear(t);
    s->fill(t);
    while (ui_children(actions))
        ui_remove(ui_child(actions, 0));
    for (int k = 0; k < 4 && s->actions[k].label; k++) {
        struct widget *b = ui_create(win, "button");

        ui_set_text(b, s->actions[k].label);
        ui_set_handler(b, "click", action_clicked, (void *)s->actions[k].fn);
        ui_add(actions, b);
    }
    snprintf(st, sizeof(st), "%d items", ui_list_count(t));
    status(st);
}

static void on_snapin(struct widget *w, void *u)
{
    (void)u;
    if (ui_list_selected(w) >= 0)
        show(ui_list_selected(w));
}

static void on_refresh(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    show(current);
}

static void show_lock(void)
{
    ui_set_text(ui_get(win, "lockstate"), geteuid() == 0 ? "Unlocked: changes allowed" : "Some details need unlocking");
    ui_set_visible(ui_get(win, "unlock"), geteuid() != 0);
}

static void on_unlock(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_elevate(win, "Unlocking shows every account's details and allows changes."))
        show(current);
    show_lock();
}

static void on_primary(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (snapins[current].actions[0].fn)
        snapins[current].actions[0].fn();
}

static void on_context(struct widget *w, void *u)
{
    (void)w;
    (void)u;
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "snapin", on_snapin }, { "refresh", on_refresh }, { "unlock", on_unlock }, { "primary", on_primary },
        { "context", on_context }, { NULL, NULL },
    };
    struct widget *list;
    int start = 0;

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "console")))
        return 1;
    list = ui_get(win, "snapins");
    for (int i = 0; i < NSNAPINS; i++) {
        ui_list_add(list, snapins[i].name);
        ui_list_set_icon_shared(list, i, icon_get(snapins[i].icon, 20));
        if (argc > 1 && !strcasecmp(argv[1], snapins[i].name))
            start = i;
    }
    ui_list_select(list, start);
    show(start);
    show_lock();
    ui_window_show(win);
    return ui_run();
}
