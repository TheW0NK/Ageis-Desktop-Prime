#include "aegis.h"
#include "ui.h"

// Routines: schedule commands for yourself, or for the whole system.

static const char page[] =
    "<window title='Routines' width='900' height='600' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <button flat='true' symbol='add' text='New job' onclick='new' shortcut='Ctrl+N'/>"
    "    <button flat='true' text='Edit' onclick='edit'/>"
    "    <button flat='true' text='Delete' onclick='delete' shortcut='Delete'/>"
    "    <separator/>"
    "    <button flat='true' symbol='play' text='Run now' onclick='run'/>"
    "    <button flat='true' text='Turn on/off' onclick='toggle'/>"
    "    <spacer/>"
    "    <dropdown id='scope' width='190' onchange='scope'>"
    "      <option>My jobs</option><option>System jobs</option>"
    "    </dropdown>"
    "  </toolbar>"
    "  <vbox expand='1' padding='10' spacing='8'>"
    "    <table id='jobs' expand='2' onactivate='edit' onselect='selected' placeholder='No jobs yet'"
    "           columns='Name:160|When:230|Command|Next run:140|User:80'/>"
    "    <hbox spacing='8'><label text='Log' bold='true'/><spacer/>"
    "      <button flat='true' text='Clear log' onclick='clearlog'/></hbox>"
    "    <textarea id='log' expand='1' mono='true' readonly='true'/>"
    "  </vbox>"
    "</window>";

static const char editor[] =
    "<window title='Job' width='520' padding='18' spacing='12' resizable='false'>"
    "  <grid columns='2' spacing='10'>"
    "    <label text='Name'/><input id='name' placeholder='Optional'/>"
    "    <label text='Command'/><input id='command' placeholder='For example: date &gt;&gt; ~/times.txt'/>"
    "    <label id='userlabel' text='Run as'/><input id='user'/>"
    "    <label text='Repeat'/>"
    "    <dropdown id='mode' onchange='mode'>"
    "      <option>Every few minutes</option><option>Every hour</option><option>Every day</option>"
    "      <option>Every week</option><option>Every month</option><option>When the computer starts</option>"
    "      <option>Custom (five fields)</option>"
    "    </dropdown>"
    "    <label id='l_every' text='Every (minutes)'/><spin id='every' min='1' max='59' value='15'/>"
    "    <label id='l_minute' text='At minute'/><spin id='minute' min='0' max='59' value='0'/>"
    "    <label id='l_hour' text='At hour'/><spin id='hour' min='0' max='23' value='9'/>"
    "    <label id='l_day' text='Day'/>"
    "    <dropdown id='weekday'><option>Sunday</option><option>Monday</option><option>Tuesday</option>"
    "      <option>Wednesday</option><option>Thursday</option><option>Friday</option><option>Saturday</option>"
    "    </dropdown>"
    "    <label id='l_mday' text='Day of month'/><spin id='mday' min='1' max='31' value='1'/>"
    "    <label id='l_custom' text='Fields'/><input id='custom' placeholder='minute hour day month weekday'/>"
    "  </grid>"
    "  <checkbox id='enabled' text='Turned on' checked='true'/>"
    "  <label id='preview' dim='true'/>"
    "  <hbox justify='end' spacing='8'>"
    "    <button text='Cancel' cancel='true' onclick='cancel'/>"
    "    <button text='Save' default='true' onclick='save'/>"
    "  </hbox>"
    "</window>";

#define MAX_JOBS 128

static struct ui_window *win, *dlg;
static struct user_info me;
static struct cron_job jobs[MAX_JOBS];
static int njobs;
static bool system_scope;
static char crontab[256], logfile[256];
static char *other_lines[256];      // comments and blank lines, kept as they were
static int nother;

static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                                    "Saturday" };

// ---- Schedules ----

// Splits five cron fields; false if the schedule is not five fields.
static bool fields(const char *s, char f[5][24])
{
    int n = 0;

    while (*s && n < 5) {
        int len = 0;

        while (*s == ' ')
            s++;
        while (*s && *s != ' ' && len < 23)
            f[n][len++] = *s++;
        f[n][len] = 0;
        if (len)
            n++;
    }
    while (*s == ' ')
        s++;
    return n == 5 && !*s;
}

static bool number(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return false;
    return true;
}

static bool star(const char *s)
{
    return !strcmp(s, "*");
}

// Recognised shapes: the editor's modes. Returns the mode, or 6 (custom).
static int shape(const char *s, int *every, int *minute, int *hour, int *weekday, int *mday)
{
    char f[5][24];

    if (!strcmp(s, "@reboot"))
        return 5;
    if (!strcmp(s, "@hourly")) {
        *minute = 0;
        return 1;
    }
    if (!strcmp(s, "@daily")) {
        *minute = *hour = 0;
        return 2;
    }
    if (!fields(s, f))
        return 6;
    if (star(f[0]) && star(f[1]) && star(f[2]) && star(f[3]) && star(f[4])) {
        *every = 1;
        return 0;
    }
    if (!strncmp(f[0], "*/", 2) && number(f[0] + 2) && star(f[1]) && star(f[2]) && star(f[3]) && star(f[4])) {
        *every = atoi(f[0] + 2);
        return 0;
    }
    if (!number(f[0]) || !star(f[3]))
        return 6;
    *minute = atoi(f[0]);
    if (star(f[1]) && star(f[2]) && star(f[4]))
        return 1;
    if (!number(f[1]))
        return 6;
    *hour = atoi(f[1]);
    if (star(f[2]) && star(f[4]))
        return 2;
    if (star(f[2]) && number(f[4])) {
        *weekday = atoi(f[4]) % 7;
        return 3;
    }
    if (number(f[2]) && star(f[4])) {
        *mday = atoi(f[2]);
        return 4;
    }
    return 6;
}

static void describe(const char *s, char *out, size_t size)
{
    int every = 0, minute = 0, hour = 0, weekday = 0, mday = 1;

    switch (shape(s, &every, &minute, &hour, &weekday, &mday)) {
    case 0:
        if (every == 1)
            snprintf(out, size, "Every minute");
        else
            snprintf(out, size, "Every %d minutes", every);
        break;
    case 1: snprintf(out, size, "Every hour at :%02d", minute); break;
    case 2: snprintf(out, size, "Every day at %02d:%02d", hour, minute); break;
    case 3: snprintf(out, size, "Every %s at %02d:%02d", days[weekday], hour, minute); break;
    case 4: snprintf(out, size, "Day %d of each month at %02d:%02d", mday, hour, minute); break;
    case 5: snprintf(out, size, "When the computer starts"); break;
    default: snprintf(out, size, "%s", s);
    }
}

// ---- Loading and saving ----

static void free_other(void)
{
    for (int i = 0; i < nother; i++)
        free(other_lines[i]);
    nother = 0;
}

static void load_jobs(void)
{
    int fd = open(crontab, O_RDONLY);
    char line[512];

    njobs = 0;
    free_other();
    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) >= 0) {
        char copy[512];

        strlcpy(copy, line, sizeof(copy));
        if (njobs < MAX_JOBS && cron_parse(copy, system_scope, &jobs[njobs]))
            njobs++;
        else if (nother < 256 && line[0] == '#')
            other_lines[nother++] = strdup(line);
    }
    close(fd);
}

static bool save_jobs(void)
{
    char tmp[300], line[512];
    int fd;

    snprintf(tmp, sizeof(tmp), "%s.new", crontab);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, system_scope ? 0644 : 0600)) < 0) {
        ui_message(win, "Routines", "The jobs could not be saved.", "OK");
        return false;
    }
    for (int i = 0; i < nother; i++)
        dprintf(fd, "%s\n", other_lines[i]);
    for (int i = 0; i < njobs; i++) {
        cron_format(&jobs[i], system_scope, line, sizeof(line));
        dprintf(fd, "%s\n", line);
    }
    close(fd);
    return rename(tmp, crontab) == 0;
}

static void show_log(void)
{
    int fd = open(logfile, O_RDONLY);
    char *buf;
    struct aegis_stat st;
    ssize_t n = 0;
    struct widget *t = ui_get(win, "log");

    if (fd < 0 || fstat(fd, &st) < 0 || !(buf = malloc(st.size + 1))) {
        ui_set_text(t, fd < 0 ? "No job has run yet." : "");
        if (fd >= 0)
            close(fd);
        return;
    }
    n = read(fd, buf, st.size);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    ui_set_text(t, buf);
    ui_textarea_select(t, strlen(buf), strlen(buf));
    free(buf);
}

static void show_jobs(void)
{
    struct widget *t = ui_get(win, "jobs");
    int sel = ui_list_selected(t);
    int64_t now = time(NULL);

    ui_list_clear(t);
    for (int i = 0; i < njobs; i++) {
        char row[640], when[96], next[40] = "-";
        int64_t at = jobs[i].enabled ? cron_next(jobs[i].schedule, now) : -1;

        describe(jobs[i].schedule, when, sizeof(when));
        if (!jobs[i].enabled) {
            strlcpy(next, "Off", sizeof(next));
        } else if (at > 0) {
            struct tm tm;

            localtime_r(&at, &tm);
            strftime(next, sizeof(next), "%a %d %b %H:%M", &tm);
        }
        snprintf(row, sizeof(row), "%s\t%s\t%s\t%s\t%s", *jobs[i].name ? jobs[i].name : "(unnamed)", when,
                 jobs[i].command, next, system_scope ? jobs[i].user : me.name);
        ui_list_add(t, row);
        ui_list_set_icon_shared(t, i, icon_get(jobs[i].enabled ? "glyph:clock-glyph" : "glyph:pause", 18));
    }
    if (sel >= 0 && sel < njobs)
        ui_list_select(t, sel);
    show_log();
}

static void set_scope(bool sys)
{
    system_scope = sys;
    if (sys) {
        strlcpy(crontab, "/msc/routines", sizeof(crontab));
        strlcpy(logfile, "/osystem/logs/routines.log", sizeof(logfile));
    } else {
        char dir[256];

        cron_user_path(&me, "", dir, sizeof(dir));
        mkdir(dir, 0700);
        cron_user_path(&me, "routines", crontab, sizeof(crontab));
        cron_user_path(&me, "log", logfile, sizeof(logfile));
    }
    load_jobs();
    show_jobs();
}

// ---- The editor ----

static void editor_visibility(void)
{
    int mode = ui_list_selected(ui_get(dlg, "mode"));
    static const struct {
        const char *field;
        unsigned modes;             // bit per mode
    } rows[] = {
        { "every", 1 << 0 }, { "minute", 1 << 1 | 1 << 2 | 1 << 3 | 1 << 4 }, { "hour", 1 << 2 | 1 << 3 | 1 << 4 },
        { "weekday", 1 << 3 }, { "mday", 1 << 4 }, { "custom", 1 << 6 },
    };
    static const char *const labels[] = { "l_every", "l_minute", "l_hour", "l_day", "l_mday", "l_custom" };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        bool on = rows[i].modes & (1u << mode);

        ui_set_visible(ui_get(dlg, rows[i].field), on);
        ui_set_visible(ui_get(dlg, labels[i]), on);
    }
    ui_window_fit(dlg);
}

static void editor_schedule(char *out, size_t size)
{
    int mode = ui_list_selected(ui_get(dlg, "mode"));
    int every = (int)ui_value(ui_get(dlg, "every")), minute = (int)ui_value(ui_get(dlg, "minute"));
    int hour = (int)ui_value(ui_get(dlg, "hour")), wd = ui_list_selected(ui_get(dlg, "weekday"));
    int mday = (int)ui_value(ui_get(dlg, "mday"));

    switch (mode) {
    case 0: snprintf(out, size, every == 1 ? "* * * * *" : "*/%d * * * *", every); break;
    case 1: snprintf(out, size, "%d * * * *", minute); break;
    case 2: snprintf(out, size, "%d %d * * *", minute, hour); break;
    case 3: snprintf(out, size, "%d %d * * %d", minute, hour, MAX(wd, 0)); break;
    case 4: snprintf(out, size, "%d %d %d * *", minute, hour, mday); break;
    case 5: snprintf(out, size, "@reboot"); break;
    default: snprintf(out, size, "%s", ui_text(ui_get(dlg, "custom")));
    }
}

static void editor_preview(void)
{
    char sched[64], text[160], when[96];
    int64_t next;

    editor_schedule(sched, sizeof(sched));
    describe(sched, when, sizeof(when));
    next = cron_next(sched, time(NULL));
    if (next > 0) {
        struct tm tm;
        char at[48];

        localtime_r(&next, &tm);
        strftime(at, sizeof(at), "%A %d %B, %H:%M", &tm);
        snprintf(text, sizeof(text), "%s. Next: %s.", when, at);
    } else {
        snprintf(text, sizeof(text), "%s.", when);
    }
    ui_set_text(ui_get(dlg, "preview"), text);
}

static void on_mode(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    editor_visibility();
    editor_preview();
}

static void on_save(struct widget *w, void *u)
{
    char sched[64];
    struct tm tm = { 0 };

    (void)w;
    (void)u;
    editor_schedule(sched, sizeof(sched));
    if (!*ui_text(ui_get(dlg, "command"))) {
        ui_message(dlg, "Routines", "Type the command the job runs.", "OK");
        return;
    }
    // A custom schedule must at least parse.
    if (strcmp(sched, "@reboot") && *sched != '@') {
        char f[5][24];

        if (!fields(sched, f)) {
            ui_message(dlg, "Routines", "A custom schedule has five fields: minute hour day month weekday.",
                       "OK");
            return;
        }
        (void)cron_matches(sched, &tm);
    }
    ui_dialog_end(dlg, 1);
}

static void on_cancel(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_dialog_end(dlg, 0);
}

static void on_preview_change(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    editor_preview();
}

// Edits job (a new one if NULL). Returns true if saved into *out.
static bool edit_job(const struct cron_job *job, struct cron_job *out)
{
    static const struct ui_handler_entry handlers[] = {
        { "mode", on_mode }, { "save", on_save }, { "cancel", on_cancel }, { NULL, NULL },
    };
    int every = 15, minute = 0, hour = 9, weekday = 1, mday = 1, mode = 2;
    char sched[64];

    if (!(dlg = ui_load_string_named(editor, handlers, NULL, "cron-editor")))
        return false;
    if (job) {
        mode = shape(job->schedule, &every, &minute, &hour, &weekday, &mday);
        ui_set_text(ui_get(dlg, "name"), job->name);
        ui_set_text(ui_get(dlg, "command"), job->command);
        ui_set_text(ui_get(dlg, "user"), job->user);
        ui_set_text(ui_get(dlg, "custom"), job->schedule);
        ui_set_value(ui_get(dlg, "enabled"), job->enabled);
        ui_window_set_title(dlg, "Edit job");
    } else {
        ui_set_text(ui_get(dlg, "user"), "superuser");
        ui_window_set_title(dlg, "New job");
    }
    ui_set_visible(ui_get(dlg, "user"), system_scope);
    ui_set_visible(ui_get(dlg, "userlabel"), system_scope);
    ui_list_select(ui_get(dlg, "mode"), mode);
    ui_set_value(ui_get(dlg, "every"), every);
    ui_set_value(ui_get(dlg, "minute"), minute);
    ui_set_value(ui_get(dlg, "hour"), hour);
    ui_list_select(ui_get(dlg, "weekday"), weekday);
    ui_set_value(ui_get(dlg, "mday"), mday);
    {
        static const char *const live[] = { "every", "minute", "hour", "weekday", "mday", "custom" };

        for (int i = 0; i < 6; i++)
            ui_set_handler(ui_get(dlg, live[i]), "change", on_preview_change, NULL);
    }
    editor_visibility();
    editor_preview();
    if (ui_dialog_run(dlg, win) != 1)
        return false;
    memset(out, 0, sizeof(*out));
    editor_schedule(sched, sizeof(sched));
    strlcpy(out->schedule, sched, sizeof(out->schedule));
    strlcpy(out->command, ui_text(ui_get(dlg, "command")), sizeof(out->command));
    strlcpy(out->name, ui_text(ui_get(dlg, "name")), sizeof(out->name));
    strlcpy(out->user, *ui_text(ui_get(dlg, "user")) ? ui_text(ui_get(dlg, "user")) : "superuser", sizeof(out->user));
    out->enabled = ui_value(ui_get(dlg, "enabled")) != 0;
    return true;
}

// ---- Handlers ----

static int selected_job(void)
{
    int i = ui_list_selected(ui_get(win, "jobs"));

    return i >= 0 && i < njobs ? i : -1;
}

static void on_new(struct widget *w, void *u)
{
    struct cron_job j;

    (void)w;
    (void)u;
    if (njobs < MAX_JOBS && edit_job(NULL, &j)) {
        jobs[njobs++] = j;
        save_jobs();
        show_jobs();
        ui_list_select(ui_get(win, "jobs"), njobs - 1);
    }
}

static void on_edit(struct widget *w, void *u)
{
    int i = selected_job();
    struct cron_job j;

    (void)w;
    (void)u;
    if (i >= 0 && edit_job(&jobs[i], &j)) {
        jobs[i] = j;
        save_jobs();
        show_jobs();
    }
}

static void on_delete(struct widget *w, void *u)
{
    int i = selected_job();
    char msg[400];

    (void)w;
    (void)u;
    if (i < 0)
        return;
    snprintf(msg, sizeof(msg), "Delete the job \"%s\"?", *jobs[i].name ? jobs[i].name : jobs[i].command);
    if (ui_message(win, "Routines", msg, "Delete|Cancel") != 0)
        return;
    memmove(&jobs[i], &jobs[i + 1], (njobs - i - 1) * sizeof(jobs[0]));
    njobs--;
    save_jobs();
    show_jobs();
}

static void on_toggle(struct widget *w, void *u)
{
    int i = selected_job();

    (void)w;
    (void)u;
    if (i < 0)
        return;
    jobs[i].enabled = !jobs[i].enabled;
    save_jobs();
    show_jobs();
}

static void on_run(struct widget *w, void *u)
{
    int i = selected_job();
    char *argv[] = { "terminal", "-c", NULL, NULL }, line[400];
    int fd, saved[2], pid;

    (void)w;
    (void)u;
    if (i < 0)
        return;
    // Runs here, as whoever is running this app, with output in the log.
    argv[2] = jobs[i].command;
    if ((fd = open(logfile, O_WRONLY | O_CREAT, 0600)) < 0)
        return;
    lseek(fd, 0, SEEK_END);
    snprintf(line, sizeof(line), "(run now) %s\n", jobs[i].command);
    write(fd, line, strlen(line));
    saved[0] = dup(1);
    saved[1] = dup(2);
    dup2(fd, 1);
    dup2(fd, 2);
    pid = spawn("/sysapps/terminal", argv, environ);
    dup2(saved[0], 1);
    dup2(saved[1], 2);
    close(saved[0]);
    close(saved[1]);
    close(fd);
    if (pid > 0)
        waitpid(pid, NULL, 0);
    show_log();
}

static void on_clearlog(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    truncate(logfile, 0);
    show_log();
}

static void on_scope(struct widget *w, void *u)
{
    (void)u;
    if (ui_list_selected(w) == 1 && !ui_elevate(win, "System jobs run for any account on this computer.")) {
        ui_list_select(w, 0);
        return;
    }
    set_scope(ui_list_selected(w) == 1);
}

static void on_selected(struct widget *w, void *u)
{
    (void)w;
    (void)u;
}

static bool refresh(void *u)
{
    (void)u;
    // Next-run times move on as the minutes pass.
    show_jobs();
    return true;
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "new", on_new }, { "edit", on_edit }, { "delete", on_delete }, { "toggle", on_toggle },
        { "run", on_run }, { "clearlog", on_clearlog }, { "scope", on_scope }, { "selected", on_selected },
        { NULL, NULL },
    };

    ui_load_user_theme();
    if (user_current(&me) < 0)
        return 1;
    if (!(win = ui_load_string_named(page, handlers, NULL, "cron")))
        return 1;
    set_scope(false);
    ui_timer(5000, refresh, NULL);
    ui_window_show(win);
    return ui_run();
}
