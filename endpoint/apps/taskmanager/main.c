#include "aegis.h"
#include "ui.h"

// Task Manager: running apps (windows) and processes.

static const char page[] =
    "<window title='Task Manager' width='820' height='540' padding='0' spacing='0'>"
    "  <tabs id='tabs' expand='1' onchange='tab'>"
    "    <tab title='Apps'>"
    "      <table id='apps' expand='1' columns='Window|Thread:160|ID:70:right|Memory:100:right'"
    "             onactivate='switch' placeholder='No windows are open'/>"
    "      <hbox justify='end' spacing='8'>"
    "        <button text='Switch to' onclick='switch'/>"
    "        <button text='End task' onclick='endapp'/>"
    "      </hbox>"
    "    </tab>"
    "    <tab title='Threads'>"
    "      <hbox spacing='8'>"
    "        <input id='filter' width='220' placeholder='Filter by name' onchange='refresh'/>"
    "        <checkbox id='mine' text='Only mine' onchange='refresh'/>"
    "      </hbox>"
    "      <table id='procs' expand='1' onsort='sort' oncontext='context'"
    "             columns='Name|ID:70:right|User:90|State:90|CPU:70:right|Memory:100:right|Strands:70:right'/>"
    "      <hbox justify='end' spacing='8'>"
    "        <button text='End thread' onclick='end'/>"
    "        <button text='Force stop' onclick='kill'/>"
    "      </hbox>"
    "    </tab>"
    "  </tabs>"
    "  <statusbar>"
    "    <label id='summary'/>"
    "  </statusbar>"
    "  <menu id='ctx'>"
    "    <item text='End thread' onclick='end'/>"
    "    <item text='Force stop' onclick='kill'/>"
    "  </menu>"
    "</window>";

#define MAX_PROCS 512

struct proc {
    struct aegis_procinfo info;
    double cpu;                     // percent of one processor
};

struct window_entry {
    uint32_t id;
    int pid;
    char title[WM_TEXT_MAX];
};

static struct ui_window *win;
static struct proc procs[MAX_PROCS];
static int nprocs, *order;
static struct {
    int pid;
    uint64_t cpu_ms;
} prev[MAX_PROCS];
static int nprev;
static uint64_t prev_time;
static struct window_entry windows[64];
static int nwindows;
static int sort_col = 4;
static bool sort_desc = true;

static const char *state_name(uint32_t s)
{
    switch (s) {
    case PROC_RUNNING: return "Running";
    case PROC_SLEEPING: return "Waiting";
    case PROC_STOPPED: return "Stopped";
    case PROC_ZOMBIE: return "Ended";
    }
    return "?";
}

static void user_name(uint32_t uid, char *buf, size_t size)
{
    struct user_info u;

    if (uid == 0)
        strlcpy(buf, "superuser", size);
    else if (user_by_uid(uid, &u) == 0)
        strlcpy(buf, u.name, size);
    else
        snprintf(buf, size, "%u", uid);
}

static int compare(const void *a, const void *b)
{
    const struct proc *x = &procs[*(const int *)a], *y = &procs[*(const int *)b];
    int r = 0;

    switch (sort_col) {
    case 0: r = strcasecmp(x->info.name, y->info.name); break;
    case 1: r = x->info.pid - y->info.pid; break;
    case 2: r = (int)x->info.uid - (int)y->info.uid; break;
    case 3: r = (int)x->info.state - (int)y->info.state; break;
    case 4: r = x->cpu < y->cpu ? -1 : x->cpu > y->cpu; break;
    case 5: r = x->info.memory < y->info.memory ? -1 : x->info.memory > y->info.memory; break;
    case 6: r = (int)x->info.threads - (int)y->info.threads; break;
    }
    if (!r)
        r = x->info.pid - y->info.pid;
    return sort_desc ? -r : r;
}

static void sample(void)
{
    struct aegis_procinfo info[MAX_PROCS];
    uint64_t now = uptime_ms(), dt = prev_time ? now - prev_time : 0;
    int n = procinfo(info, MAX_PROCS);

    nprocs = 0;
    for (int i = 0; i < n; i++) {
        struct proc *p = &procs[nprocs++];

        p->info = info[i];
        p->cpu = 0;
        for (int k = 0; k < nprev && dt; k++)
            if (prev[k].pid == info[i].pid) {
                p->cpu = 100.0 * (info[i].cpu_ms - prev[k].cpu_ms) / dt;
                break;
            }
    }
    nprev = 0;
    for (int i = 0; i < nprocs; i++) {
        prev[nprev].pid = procs[i].info.pid;
        prev[nprev++].cpu_ms = procs[i].info.cpu_ms;
    }
    prev_time = now;
}

static struct proc *find_proc(int pid)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].info.pid == pid)
            return &procs[i];
    return NULL;
}

static int selected_pid(void)
{
    struct widget *t = ui_get(win, "procs");
    int sel = ui_list_selected(t);
    char buf[16];

    if (sel < 0)
        return -1;
    return atoi(ui_list_column(t, sel, 1, buf, sizeof(buf)));
}

static void show_procs(void)
{
    struct widget *t = ui_get(win, "procs");
    const char *filter = ui_text(ui_get(win, "filter"));
    bool mine = ui_value(ui_get(win, "mine")) != 0;
    int keep = selected_pid(), shown = 0;

    free(order);
    order = malloc(sizeof(int) * MAX(nprocs, 1));
    for (int i = 0; i < nprocs; i++) {
        if (*filter && !strstr(procs[i].info.name, filter))
            continue;
        if (mine && procs[i].info.uid != getuid())
            continue;
        order[shown++] = i;
    }
    qsort(order, shown, sizeof(int), compare);
    // Rewrite rows in place so the selection and scroll stay put.
    while (ui_list_count(t) > shown)
        ui_list_remove(t, ui_list_count(t) - 1);
    for (int k = 0; k < shown; k++) {
        struct proc *p = &procs[order[k]];
        char row[256], who[32], mem[32];

        user_name(p->info.uid, who, sizeof(who));
        ui_format_size(p->info.memory, mem, sizeof(mem));
        snprintf(row, sizeof(row), "%s\t%d\t%s\t%s\t%.1f%%\t%s\t%u", p->info.name, p->info.pid, who,
                 state_name(p->info.state), p->cpu, mem, p->info.threads);
        if (k < ui_list_count(t))
            ui_list_set_item(t, k, row);
        else
            ui_list_add(t, row);
        if (p->info.pid == keep)
            ui_list_select(t, k);
    }
}

static void show_apps(void)
{
    struct widget *t = ui_get(win, "apps");
    int sel = ui_list_selected(t);

    ui_list_clear(t);
    for (int i = 0; i < nwindows; i++) {
        struct proc *p = find_proc(windows[i].pid);
        char row[400], mem[32] = "";

        if (p)
            ui_format_size(p->info.memory, mem, sizeof(mem));
        snprintf(row, sizeof(row), "%s\t%s\t%d\t%s", windows[i].title, p ? p->info.name : "", windows[i].pid, mem);
        ui_list_add(t, row);
        ui_list_set_icon_shared(t, i, icon_get("glyph:desktop", 18));
    }
    if (sel >= 0 && sel < nwindows)
        ui_list_select(t, sel);
}

static void summary(void)
{
    struct aegis_sysinfo si;
    char buf[160], used[32], total[32];
    double cpu = 0;

    for (int i = 0; i < nprocs; i++)
        cpu += procs[i].cpu;
    if (sysinfo(&si) < 0)
        return;
    ui_format_size(si.memory_total - si.memory_free, used, sizeof(used));
    ui_format_size(si.memory_total, total, sizeof(total));
    snprintf(buf, sizeof(buf), "%d threads, %u strands   CPU %.0f%%   Memory %s of %s", nprocs, si.threads,
             cpu / MAX(si.cpus, 1), used, total);
    ui_set_text(ui_get(win, "summary"), buf);
}

static bool tick(void *u)
{
    (void)u;
    sample();
    show_procs();
    show_apps();
    summary();
    return true;
}

static void signal_pid(int pid, int sig)
{
    struct proc *p = find_proc(pid);
    char msg[200];

    if (!p)
        return;
    if (pid <= 1) {
        ui_message(win, "Task Manager", "This thread keeps the computer running and cannot be ended.", "OK");
        return;
    }
    if (p->info.uid != geteuid() && geteuid() != 0
        && !ui_elevate(win, "This thread belongs to another account."))
        return;
    if (kill(pid, sig) < 0) {
        snprintf(msg, sizeof(msg), "\"%s\" could not be ended: %s.", p->info.name, strerror(errno));
        ui_message(win, "Task Manager", msg, "OK");
    }
    tick(NULL);
}

static void on_end(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    signal_pid(selected_pid(), SIGTERM);
}

static void on_kill(struct widget *w, void *u)
{
    struct proc *p = find_proc(selected_pid());
    char msg[200];

    (void)w;
    (void)u;
    if (!p)
        return;
    snprintf(msg, sizeof(msg), "Stop \"%s\" at once? Anything it has not saved will be lost.", p->info.name);
    if (ui_message(win, "Force stop", msg, "Force stop|Cancel") == 0)
        signal_pid(p->info.pid, SIGKILL);
}

static void on_switch(struct widget *w, void *u)
{
    int sel = ui_list_selected(ui_get(win, "apps"));

    (void)w;
    (void)u;
    if (sel >= 0 && sel < nwindows)
        wm_activate(windows[sel].id, false);
}

static void on_endapp(struct widget *w, void *u)
{
    int sel = ui_list_selected(ui_get(win, "apps"));

    (void)w;
    (void)u;
    if (sel >= 0 && sel < nwindows)
        signal_pid(windows[sel].pid, SIGTERM);
}

static void on_sort(struct widget *w, void *u)
{
    (void)u;
    sort_col = atoi(ui_attr(w, "sortcolumn"));
    sort_desc = ui_attr_true(ui_attr(w, "sortdescending"));
    show_procs();
}

static void on_refresh(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    show_procs();
}

static void on_tab(struct widget *w, void *u)
{
    (void)w;
    (void)u;
}

static void on_context(struct widget *w, void *u)
{
    (void)u;
    ui_menu_popup(ui_get(win, "ctx"), w, -1, -1);
}

static void window_list(struct wm_event *ev, void *u)
{
    struct wm_msg *m = &ev->msg;

    (void)u;
    if (ev->type != WM_EV_LIST)
        return;
    for (int i = 0; i < nwindows; i++) {
        if (windows[i].id != m->window)
            continue;
        if (m->type == WM_LIST_REMOVE)
            windows[i] = windows[--nwindows];
        else
            strlcpy(windows[i].title, m->text, sizeof(windows[i].title));
        show_apps();
        return;
    }
    if (m->type == WM_LIST_ADD && nwindows < 64) {
        windows[nwindows].id = m->window;
        windows[nwindows].pid = m->b;
        strlcpy(windows[nwindows].title, m->text, sizeof(windows[nwindows].title));
        nwindows++;
        show_apps();
    }
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "end", on_end }, { "kill", on_kill }, { "switch", on_switch }, { "endapp", on_endapp },
        { "sort", on_sort }, { "refresh", on_refresh }, { "tab", on_tab }, { "context", on_context },
        { NULL, NULL },
    };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "tasks")))
        return 1;
    ui_set_attr(ui_get(win, "procs"), "sortcolumn", "4");
    ui_set_attr(ui_get(win, "procs"), "sortdescending", "true");
    ui_on_system_event(window_list, NULL);
    sample();
    tick(NULL);
    ui_timer(1500, tick, NULL);
    ui_window_show(win);
    wm_subscribe();
    return ui_run();
}
