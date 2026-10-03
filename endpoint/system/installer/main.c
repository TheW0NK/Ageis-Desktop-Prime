#include "aegis.h"
#include "ui.h"

// The installer: what the live system on the install media starts instead
// of the sign-in screen. Welcome, choose a disk, make an account, confirm,
// install (erasing the disk), restart.

static const char page[] =
    "<window role='overlay' padding='0' spacing='0'>"
    "  <vbox expand='1' justify='center'>"
    "    <hbox justify='center'>"
    "      <card width='640' spacing='14'>"
    "        <hbox spacing='12'>"
    "          <button flat='true' symbol='shield' iconsize='36' focusable='false'/>"
    "          <vbox spacing='0' expand='1'>"
    "            <h2 text='Install Aegis'/>"
    "            <label id='steplabel' dim='true'/>"
    "          </vbox>"
    "        </hbox>"
    "        <separator/>"
    "        <stack id='pages' value='0' height='330'>"
    // 0: welcome
    "          <vbox spacing='12'>"
    "            <p text='Welcome. This puts Aegis on this computer&apos;s disk so it starts without the install"
    " media. Installing erases the disk you choose: everything on it is replaced.'/>"
    "            <hbox spacing='10'><label text='Language' width='140' align='center'/>"
    "              <dropdown id='language' expand='1'><option>English</option><option>Espa\xc3\xb1ol</option>"
    "                <option>Fran\xc3\xa7" "ais</option><option>Deutsch</option><option>Chinese, Simplified (pinyin)</option>"
    "              </dropdown></hbox>"
    "            <hbox spacing='10'><label text='Time zone' width='140' align='center'/>"
    "              <dropdown id='timezone' expand='1'/></hbox>"
    "            <spacer/>"
    "            <p dim='true' text='To look around without installing, open a Terminal from the button below.'/>"
    "          </vbox>"
    // 1: disk
    "          <vbox spacing='10'>"
    "            <p text='Choose the disk to install Aegis on.'/>"
    "            <list id='disks' expand='1' onselect='disk'/>"
    "            <label id='diskwarn' wrap='true' text=''/>"
    "          </vbox>"
    // 2: account
    "          <grid columns='2' stretch='1' spacing='10'>"
    "            <label text='Your name'/><input id='display' onchange='display'/>"
    "            <label text='User name'/><input id='user' maxlength='31' onchange='validate'/>"
    "            <label text='Password'/><password id='pw1' onchange='validate'/>"
    "            <label text='Confirm password'/><password id='pw2' onchange='validate' onactivate='next'/>"
    "            <label text='Computer name'/><input id='host' text='aegis' maxlength='31' onchange='validate'/>"
    "            <spacer/><label id='accounterr' text=''/>"
    "          </grid>"
    // 3: confirm
    "          <vbox spacing='10'>"
    "            <p text='Ready to install. Check the details below.'/>"
    "            <label id='summary' wrap='true'/>"
    "            <spacer/>"
    "            <label id='finalwarn' wrap='true' bold='true'/>"
    "          </vbox>"
    // 4: progress
    "          <vbox spacing='12' justify='center'>"
    "            <label id='progstep' text='Starting'/>"
    "            <progress id='progress' value='0'/>"
    "            <label dim='true' wrap='true' text='This takes a few minutes. Do not switch the computer off.'/>"
    "          </vbox>"
    // 5: done
    "          <vbox spacing='12' justify='center'>"
    "            <h2 id='donetitle' text='Aegis is installed'/>"
    "            <p id='donetext' text='Remove the install media, then restart. Sign in with the account you made.'/>"
    "          </vbox>"
    "        </stack>"
    "        <separator/>"
    "        <hbox spacing='8'>"
    "          <button id='terminal' text='Terminal' flat='true' onclick='terminal'/>"
    "          <button id='power' text='Shut down' flat='true' onclick='poweroff'/>"
    "          <spacer/>"
    "          <button id='back' text='Back' onclick='back'/>"
    "          <button id='next' text='Next' default='true' onclick='next'/>"
    "        </hbox>"
    "      </card>"
    "    </hbox>"
    "  </vbox>"
    "</window>";

enum { P_WELCOME, P_DISK, P_ACCOUNT, P_CONFIRM, P_PROGRESS, P_DONE };

static const struct {
    const char *name;
    int offset;
} zones[] = {
    { "UTC", 0 }, { "London (UTC+0)", 0 }, { "Paris, Berlin, Madrid (UTC+1)", 60 },
    { "Athens, Helsinki (UTC+2)", 120 }, { "Moscow (UTC+3)", 180 }, { "Dubai (UTC+4)", 240 },
    { "New Delhi (UTC+5:30)", 330 }, { "Beijing, Shanghai (UTC+8)", 480 }, { "Tokyo (UTC+9)", 540 },
    { "Sydney (UTC+10)", 600 }, { "Auckland (UTC+12)", 720 }, { "Honolulu (UTC-10)", -600 },
    { "Anchorage (UTC-9)", -540 }, { "Los Angeles (UTC-8)", -480 }, { "Denver (UTC-7)", -420 },
    { "Chicago, Mexico City (UTC-6)", -360 }, { "New York (UTC-5)", -300 }, { "S\xc3\xa3o Paulo (UTC-3)", -180 },
};
static const char *const lang_codes[] = { "en", "es", "fr", "de", "zh" };
static const char *const step_names[] = { "Welcome", "Disk", "Account", "Summary", "Installing", "Finished" };

static struct ui_window *win;
static int pagei;
static struct disk_info disks[16];
static int ndisks;
static bool user_edited;
static int pipe_fds[2];
static char install_error[256];
static bool install_ok;

// What the install thread reads; filled in before it starts.
static char opt_disk[16], opt_user[32], opt_display[128], opt_password[256], opt_host[32];
static struct install_options opts;

static void size_text(uint64_t bytes, char *out, size_t size)
{
    ui_format_size(bytes, out, size);
}

static bool valid_user(const char *s)
{
    if (!*s || !(*s >= 'a' && *s <= 'z') || strlen(s) > 31)
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
            return false;
    return true;
}

static bool valid_host(const char *s)
{
    if (!*s || strlen(s) > 31)
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-'))
            return false;
    return true;
}

static int chosen_disk(void)
{
    int i = ui_list_selected(ui_get(win, "disks"));

    return i >= 0 && i < ndisks ? i : -1;
}

// Why the current page cannot go on, or NULL.
static const char *page_problem(void)
{
    switch (pagei) {
    case P_DISK: {
        int d = chosen_disk();

        if (!ndisks)
            return "No disk was found to install on.";
        if (d < 0)
            return "Choose a disk.";
        if (disks[d].size < (2ULL << 30))
            return "That disk is too small: Aegis needs at least 2 GB.";
        if (disks[d].mounted)
            return "That disk is in use.";
        return NULL;
    }
    case P_ACCOUNT: {
        const char *user = ui_text(ui_get(win, "user")), *p1 = ui_text(ui_get(win, "pw1")),
                   *p2 = ui_text(ui_get(win, "pw2"));

        if (!*ui_text(ui_get(win, "display")) && !*user)
            return "Enter your name.";
        if (!valid_user(user))
            return "User names are lowercase letters, digits, - and _, starting with a letter.";
        if (!strcmp(user, "root"))
            return "Choose a user name other than root.";
        if (strlen(p1) < 4)
            return "Choose a password of at least 4 characters.";
        if (strcmp(p1, p2))
            return "The passwords do not match.";
        if (!valid_host(ui_text(ui_get(win, "host"))))
            return "Computer names are letters, digits and -.";
        return NULL;
    }
    }
    return NULL;
}

static void show_page(int p)
{
    char label[64];

    pagei = p;
    ui_set_value(ui_get(win, "pages"), p);
    snprintf(label, sizeof(label), "Step %d of 5: %s", MIN(p + 1, 5), step_names[p]);
    ui_set_text(ui_get(win, "steplabel"), p == P_DONE ? step_names[p] : label);
    ui_set_visible(ui_get(win, "back"), p > P_WELCOME && p < P_PROGRESS);
    ui_set_enabled(ui_get(win, "terminal"), p != P_PROGRESS);
    ui_set_enabled(ui_get(win, "power"), p != P_PROGRESS);
    ui_set_visible(ui_get(win, "next"), p != P_PROGRESS);
    ui_set_text(ui_get(win, "next"), p == P_CONFIRM ? "Erase disk and install" : p == P_DONE ? (install_ok ? "Restart" : "Try again") : "Next");
    ui_set_enabled(ui_get(win, "next"), page_problem() == NULL);
    if (p == P_ACCOUNT)
        ui_focus(ui_get(win, "display"));
    else if (p == P_DISK)
        ui_focus(ui_get(win, "disks"));
    else
        ui_focus(ui_get(win, "next"));
    ui_relayout(win);
}

static void refresh_problem(void)
{
    const char *why = page_problem();

    ui_set_enabled(ui_get(win, "next"), why == NULL);
    if (pagei == P_ACCOUNT)
        ui_set_text(ui_get(win, "accounterr"), why ? why : "");
    else if (pagei == P_DISK && chosen_disk() >= 0)
        ui_set_text(ui_get(win, "diskwarn"), why ? why : "Everything on this disk will be erased.");
}

static void load_disks(void)
{
    struct widget *list = ui_get(win, "disks");

    ndisks = disk_list(disks, 16);
    if (ndisks < 0)
        ndisks = 0;
    ui_list_clear(list);
    for (int i = 0; i < ndisks; i++) {
        char row[128], size[32];

        size_text(disks[i].size, size, sizeof(size));
        snprintf(row, sizeof(row), "%s  \xe2\x80\x94  %s  (%s)", size, disks[i].description, disks[i].name);
        ui_list_add(list, row);
        ui_list_set_icon_shared(list, i, icon_get("glyph:disk", 20));
    }
    // Preselect the disk when only one is big enough.
    {
        int fit = -1, nfit = 0;

        for (int i = 0; i < ndisks; i++)
            if (disks[i].size >= (2ULL << 30) && !disks[i].mounted) {
                fit = i;
                nfit++;
            }
        if (nfit == 1)
            ui_list_select(list, fit);
    }
    ui_set_text(ui_get(win, "diskwarn"), ndisks ? "" : "No disk was found. Add a disk to this computer and restart.");
}

static void fill_summary(void)
{
    int d = chosen_disk();
    char text[700], size[32];

    size_text(disks[d].size, size, sizeof(size));
    snprintf(text, sizeof(text),
             "Disk: %s %s (%s)\nAccount: %s (%s), administrator\nComputer name: %s\nLanguage: %s\nTime zone: %s",
             size, disks[d].description, disks[d].name, ui_text(ui_get(win, "display")), ui_text(ui_get(win, "user")),
             ui_text(ui_get(win, "host")), ui_list_item(ui_get(win, "language"), ui_list_selected(ui_get(win, "language"))),
             zones[MAX(0, ui_list_selected(ui_get(win, "timezone")))].name);
    ui_set_text(ui_get(win, "summary"), text);
    snprintf(text, sizeof(text), "Everything on %s (%s) will be erased. This cannot be undone.", disks[d].name, size);
    ui_set_text(ui_get(win, "finalwarn"), text);
}

// ---- Installing, on its own thread ----

struct progress_msg {
    int percent;
    char step[60];
};

static void report(int percent, const char *step, void *u)
{
    struct progress_msg m = { percent, "" };

    (void)u;
    strlcpy(m.step, step, sizeof(m.step));
    write(pipe_fds[1], &m, sizeof(m));
}

static void *install_thread(void *arg)
{
    struct progress_msg m = { -1, "" };

    (void)arg;
    install_ok = install_system(&opts, report, NULL, install_error, sizeof(install_error)) == 0;
    memset(opt_password, 0, sizeof(opt_password));
    write(pipe_fds[1], &m, sizeof(m));
    return NULL;
}

static void progress_ready(int fd, void *u)
{
    struct progress_msg m;

    (void)u;
    while (read(fd, &m, sizeof(m)) == sizeof(m)) {
        if (m.percent < 0) {
            if (!install_ok) {
                ui_set_text(ui_get(win, "donetitle"), "The installation failed");
                ui_set_text(ui_get(win, "donetext"), install_error);
            }
            dprintf(STDERR_FILENO, "installer: %s%s\n", install_ok ? "done" : "failed: ", install_ok ? "" : install_error);
            show_page(P_DONE);
            return;
        }
        ui_set_value(ui_get(win, "progress"), m.percent);
        ui_set_text(ui_get(win, "progstep"), m.step);
        dprintf(STDERR_FILENO, "installer: %d%% %s\n", m.percent, m.step);
        break;
    }
}

static void start_install(void)
{
    int d = chosen_disk(), z = MAX(0, ui_list_selected(ui_get(win, "timezone")));
    int lang = MAX(0, ui_list_selected(ui_get(win, "language")));
    thread_t t;

    strlcpy(opt_disk, disks[d].name, sizeof(opt_disk));
    strlcpy(opt_user, ui_text(ui_get(win, "user")), sizeof(opt_user));
    strlcpy(opt_display, ui_text(ui_get(win, "display")), sizeof(opt_display));
    strlcpy(opt_password, ui_text(ui_get(win, "pw1")), sizeof(opt_password));
    strlcpy(opt_host, ui_text(ui_get(win, "host")), sizeof(opt_host));
    ui_set_text(ui_get(win, "pw1"), "");
    ui_set_text(ui_get(win, "pw2"), "");
    opts = (struct install_options){ opt_disk, opt_user, opt_display, opt_password, true, opt_host,
                                     zones[z].name, zones[z].offset, lang_codes[lang] };
    show_page(P_PROGRESS);
    dprintf(STDERR_FILENO, "installer: installing on %s\n", opt_disk);
    if (thread_create(&t, install_thread, NULL) != 0) {
        strlcpy(install_error, "The installer could not start.", sizeof(install_error));
        install_ok = false;
        report(-1, "", NULL);
    }
}

// ---- Handlers ----

static void on_next(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (page_problem())
        return;
    switch (pagei) {
    case P_WELCOME:
        load_disks();
        show_page(P_DISK);
        refresh_problem();
        break;
    case P_DISK:
        show_page(P_ACCOUNT);
        refresh_problem();
        break;
    case P_ACCOUNT:
        fill_summary();
        show_page(P_CONFIRM);
        break;
    case P_CONFIRM:
        start_install();
        break;
    case P_DONE:
        if (install_ok) {
            sync();
            reboot(REBOOT_RESTART);
        } else {
            show_page(P_DISK);
            load_disks();
            refresh_problem();
        }
        break;
    }
}

static void on_back(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (pagei > P_WELCOME && pagei < P_PROGRESS)
        show_page(pagei - 1);
}

static void on_disk(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    refresh_problem();
}

static void on_validate(struct widget *w, void *u)
{
    (void)u;
    if (w == ui_get(win, "user"))
        user_edited = *ui_text(w) != 0;
    refresh_problem();
}

// The user name follows the name typed, until edited by hand.
static void on_display(struct widget *w, void *u)
{
    (void)u;
    if (!user_edited) {
        char user[32];
        int n = 0;

        for (const char *s = ui_text(w); *s && n < 31; s++) {
            char c = *s;

            if (c >= 'A' && c <= 'Z')
                c += 32;
            if ((c >= 'a' && c <= 'z') || (n && c >= '0' && c <= '9'))
                user[n++] = c;
            else if (c == ' ')
                break;
        }
        user[n] = 0;
        ui_set_text(ui_get(win, "user"), user);
    }
    refresh_problem();
}

static void on_terminal(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    launch("/bin/term", "/");
}

static void on_poweroff(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_message(win, "Shut down", "Shut down the computer without installing?", "Shut down|Cancel") == 0) {
        sync();
        reboot(REBOOT_POWEROFF);
    }
}

static bool on_close(struct ui_window *w, void *u)
{
    (void)w;
    (void)u;
    return false;               // the installer is the whole session
}

static void backdrop(struct ui_window *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    wallpaper_draw(g, r, "default");
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "next", on_next }, { "back", on_back }, { "disk", on_disk }, { "validate", on_validate },
        { "display", on_display }, { "terminal", on_terminal }, { "poweroff", on_poweroff }, { NULL, NULL },
    };
    struct widget *tz;
    int sw, sh;

    if (pipe(pipe_fds) < 0 || !(win = ui_load_string_named(page, handlers, NULL, "installer")))
        return 1;
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    wm_screen_size(&sw, &sh, NULL);
    ui_window_set_size(win, sw, sh);
    ui_window_move(win, 0, 0);
    ui_window_set_backdrop(win, backdrop, NULL);
    ui_on_close(win, on_close, NULL);
    tz = ui_get(win, "timezone");
    for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++)
        ui_list_add(tz, zones[i].name);
    ui_list_select(tz, 0);
    ui_list_select(ui_get(win, "language"), 0);
    ui_watch_fd(pipe_fds[0], progress_ready, NULL);
    show_page(P_WELCOME);
    ui_window_show(win);
    dprintf(STDERR_FILENO, "installer: ready\n");
    return ui_run();
}
