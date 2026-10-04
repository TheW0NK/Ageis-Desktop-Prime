#include "aegis.h"
#include "ui.h"

// Updates: installs an Aegis system update (a .upd file, opened from Files
// or with File > Open) by handing it to Recovery and restarting, and undoes
// the last update. The update command does the same from the terminal.

static const char page[] =
    "<window title='Updates' width='600' height='400' padding='20' spacing='12'>"
    "  <hbox spacing='14'>"
    "    <canvas id='icon' width='56' height='56' expand='0'/>"
    "    <vbox spacing='2' expand='1'>"
    "      <h2 id='title' text='Aegis updates'/>"
    "      <label id='installed' dim='true'/>"
    "    </vbox>"
    "  </hbox>"
    "  <separator/>"
    "  <label id='file' bold='true' wrap='true'/>"
    "  <label id='about' wrap='true'/>"
    "  <label id='status' wrap='true'/>"
    "  <spacer/>"
    "  <p dim='true'>Updating restarts the computer into Aegis Recovery, which replaces the system files and"
    "     starts the updated system. Accounts, settings, apps and everyone&apos;s files stay, and the update"
    "     can be undone afterwards.</p>"
    "  <hbox spacing='8'>"
    "    <button id='undo' text='Undo the last update' hidden='true' onclick='undo'/>"
    "    <button id='cancelwait' text='Cancel the waiting update' hidden='true' onclick='cancelwait'/>"
    "    <spacer/>"
    "    <button text='Open an update file...' onclick='open'/>"
    "    <button id='install' text='Restart and update' default='true' disabled='true' onclick='install'/>"
    "  </hbox>"
    "</window>";

static struct ui_window *win;
static char path[512];
static struct update_info info;
static bool checked;

static void paint_icon(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    icon_draw(g, "shield", (struct rect){ r.x, r.y, MIN(r.w, r.h), MIN(r.w, r.h) });
}

// The installed version, and what waits or can be undone.
static void show_state(void)
{
    char ver[32], prev[32], text[200];
    struct update_info w;
    int pending = update_pending("", &w);

    system_version("", ver, sizeof(ver));
    snprintf(text, sizeof(text), "Installed: Aegis %s", ver);
    if (pending == 1)
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "  \xC2\xB7  Aegis %s is waiting for the next start",
                 w.version);
    else if (pending == 2)
        strlcat(text, "  \xC2\xB7  Undoing the last update waits for the next start", sizeof(text));
    ui_set_text(ui_get(win, "installed"), text);
    ui_set_visible(ui_get(win, "cancelwait"), pending != 0);
    if (update_can_undo("", prev, sizeof(prev))) {
        snprintf(text, sizeof(text), "Undo the last update (back to %s)", prev);
        ui_set_text(ui_get(win, "undo"), text);
        ui_set_visible(ui_get(win, "undo"), !pending);
    } else {
        ui_set_visible(ui_get(win, "undo"), false);
    }
}

static bool check_file(void *u)
{
    char error[300], ver[32], text[400];
    int cmp;

    (void)u;
    checked = update_check(path, &info, error, sizeof(error)) == 0;
    ui_set_enabled(ui_get(win, "install"), checked);
    if (!checked) {
        ui_set_text(ui_get(win, "about"), "");
        ui_set_text(ui_get(win, "status"), error);
        return false;
    }
    system_version("", ver, sizeof(ver));
    cmp = version_compare(info.version, ver);
    snprintf(text, sizeof(text), "Aegis %s%s%s", info.version, *info.build ? ", build " : "", info.build);
    ui_set_text(ui_get(win, "title"), text);
    snprintf(text, sizeof(text), "%s%s%u files, ", info.description, *info.description ? "\n" : "", info.files);
    ui_format_size(info.size, text + strlen(text), sizeof(text) - strlen(text));
    ui_set_text(ui_get(win, "about"), text);
    ui_set_text(ui_get(win, "status"), cmp > 0 ? "The update file is intact and newer than this system."
                                       : cmp == 0 ? "This is the version already installed: updating installs it again."
                                                  : "This is older than the version installed: updating goes back to it.");
    return false;
}

static void load(const char *p)
{
    const char *base = strrchr(p, '/') ? strrchr(p, '/') + 1 : p;

    strlcpy(path, p, sizeof(path));
    checked = false;
    ui_set_text(ui_get(win, "file"), base);
    ui_set_text(ui_get(win, "status"), "Checking the update file...");
    ui_set_enabled(ui_get(win, "install"), false);
    // After the window has shown that it is checking.
    ui_timer(60, check_file, NULL);
}

static void on_open(struct widget *w, void *u)
{
    char *p = ui_file_dialog_filtered(win, "Open an update file", NULL, false, NULL, "*.upd");

    (void)w;
    (void)u;
    if (p) {
        load(p);
        free(p);
    }
}

static void offer_restart(const char *done)
{
    if (ui_message(win, "Updates", done, "Restart now|Later") == 0) {
        sync();
        reboot(REBOOT_RESTART);
        ui_message(win, "Updates", "The computer could not be restarted. Restart it from the session menu.", "OK");
    }
}

static void on_install(struct widget *w, void *u)
{
    char error[300], msg[400];

    (void)w;
    (void)u;
    if (!checked || !ui_elevate(win, "Updating changes the system for everyone on this computer."))
        return;
    if (update_schedule(path, error, sizeof(error)) < 0) {
        ui_message(win, "Updates", error, "OK");
        return;
    }
    show_state();
    snprintf(msg, sizeof(msg), "Aegis %s will be installed when the computer restarts. Restart now? Unsaved work in "
             "open apps will be lost.", info.version);
    offer_restart(msg);
}

static void on_undo(struct widget *w, void *u)
{
    char error[300];

    (void)w;
    (void)u;
    if (ui_message(win, "Undo the last update", "Put back the system as it was before the last update? Accounts, "
                   "settings, apps and files stay.", "Undo|Cancel") != 0
        || !ui_elevate(win, "Undoing an update changes the system for everyone on this computer."))
        return;
    if (update_schedule_undo(error, sizeof(error)) < 0) {
        ui_message(win, "Updates", error, "OK");
        return;
    }
    show_state();
    offer_restart("The last update will be undone when the computer restarts. Restart now? Unsaved work in open "
                  "apps will be lost.");
}

static void on_cancel_wait(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!ui_elevate(win, "Changing what happens at the next start needs an administrator."))
        return;
    if (update_cancel() < 0)
        ui_message(win, "Updates", "The waiting update could not be cancelled.", "OK");
    show_state();
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "open", on_open }, { "install", on_install }, { "undo", on_undo }, { "cancelwait", on_cancel_wait },
        { NULL, NULL },
    };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "updates")))
        return 1;
    ui_canvas_set(ui_get(win, "icon"), paint_icon, NULL, NULL);
    show_state();
    if (argc > 1)
        load(argv[1]);
    else
        ui_set_text(ui_get(win, "status"), "Open an .upd file to update this computer.");
    ui_window_show(win);
    return ui_run();
}
