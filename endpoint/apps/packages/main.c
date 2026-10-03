#include "aegis.h"
#include "ui.h"

// Packages: installs .aip packages (opened from Files, or File > Open) after
// asking which permissions to grant, and lists installed apps to repair or
// uninstall them.

static const char page[] =
    "<window title='Packages' width='720' height='540' padding='0' spacing='0'>"
    "  <stack id='pages' value='0' expand='1'>"
    // 0: installed apps
    "    <vbox padding='16' spacing='10'>"
    "      <hbox spacing='8'><h2 text='Installed apps' expand='1'/>"
    "        <button text='Install a package...' onclick='open'/></hbox>"
    "      <table id='installed' expand='1' columns='Name|Version:90|For:90|Permissions:220' onselect='picked'"
    "             placeholder='No packages are installed. Open an .aip file to install one.'/>"
    "      <hbox spacing='8'><label id='status' dim='true' expand='1'/>"
    "        <button id='repair' text='Repair' onclick='repair' disabled='true'/>"
    "        <button id='remove' text='Uninstall' onclick='remove' disabled='true'/></hbox>"
    "    </vbox>"
    // 1: install a package
    "    <vbox padding='16' spacing='12'>"
    "      <hbox spacing='14'>"
    "        <canvas id='icon' width='64' height='64' expand='0'/>"
    "        <vbox spacing='2' expand='1'>"
    "          <h2 id='name'/>"
    "          <label id='publisher' dim='true'/>"
    "          <label id='about' wrap='true'/>"
    "        </vbox>"
    "      </hbox>"
    "      <separator/>"
    "      <label text='Permissions this app asks for' bold='true'/>"
    "      <scroll expand='1'><vbox id='perms' spacing='6'/></scroll>"
    "      <label text='Install for' bold='true'/>"
    "      <hbox spacing='16'>"
    "        <radio id='me' text='Just me' checked='true' group='scope'/>"
    "        <radio id='everyone' text='Everyone on this computer (administrator)' group='scope'/>"
    "      </hbox>"
    "      <label id='warn' wrap='true' dim='true'/>"
    "      <hbox spacing='8'>"
    "        <label id='size' dim='true' expand='1'/>"
    "        <button text='Cancel' onclick='cancel'/>"
    "        <button id='install' text='Install' default='true' onclick='install'/>"
    "      </hbox>"
    "    </vbox>"
    "  </stack>"
    "</window>";

static struct ui_window *win;
static struct aip pkg;
static bool have_pkg;
static struct aip_installed installed[64];
static int ninstalled;
static const struct aip_permission *req[16];
static struct widget *checks[16];
static int nreq;
static struct surface *pkg_icon;
static struct user_info me;

static void refresh_installed(void)
{
    struct widget *t = ui_get(win, "installed");

    ninstalled = aip_list_installed(installed, 64);
    ui_list_clear(t);
    for (int i = 0; i < ninstalled; i++) {
        char row[600];

        snprintf(row, sizeof(row), "%s\t%s\t%s\t%s", installed[i].name, installed[i].version,
                 installed[i].everyone ? "Everyone" : "You", *installed[i].granted ? installed[i].granted : "none");
        ui_list_add(t, row);
    }
    ui_set_enabled(ui_get(win, "repair"), false);
    ui_set_enabled(ui_get(win, "remove"), false);
}

static void paint_icon(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    if (pkg_icon)
        gfx_blit_scaled(g, pkg_icon, (struct rect){ 0, 0, pkg_icon->width, pkg_icon->height }, r);
    else
        icon_draw(g, "package", r);
}

// The icon is a file inside the package: decode it from there.
static void load_pkg_icon(void)
{
    size_t off = pkg.files_off;

    surface_destroy(pkg_icon);
    pkg_icon = NULL;
    for (uint32_t i = 0; i < pkg.nfiles && *pkg.icon; i++) {
        uint16_t plen;
        uint64_t size;
        char name[256];

        memcpy(&plen, pkg.data + off, 2);
        memcpy(&size, pkg.data + off + 8, 8);
        off += 16;
        memcpy(name, pkg.data + off, plen < 255 ? plen : 255);
        name[plen < 255 ? plen : 255] = 0;
        off += plen;
        if (!strcmp(name, pkg.icon)) {
            pkg_icon = image_decode(pkg.data + off, size);
            break;
        }
        off += size;
    }
}

static int highest_granted_tier(void)
{
    int t = AIP_TIER_BASIC;

    for (int i = 0; i < nreq; i++)
        if (ui_value(checks[i]) && req[i]->tier > t)
            t = req[i]->tier;
    return t;
}

static void update_warning(void)
{
    bool everyone = ui_value(ui_get(win, "everyone")) != 0;
    int t = highest_granted_tier();
    char text[300];

    snprintf(text, sizeof(text), "%s%s",
             everyone ? "Installing for everyone asks for an administrator's password. " : "",
             t >= AIP_TIER_SYSTEM ? "System permissions ask for your password."
             : t == AIP_TIER_ELEVATED ? "Elevated permissions can only be granted by an administrator." : "");
    ui_set_text(ui_get(win, "warn"), text);
}

static void on_check(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    update_warning();
}

static void show_package(const char *path)
{
    char error[256], text[300], size[32];
    struct widget *box = ui_get(win, "perms");
    bool admin = account_is_admin(me.name);

    if (have_pkg)
        aip_close(&pkg);
    if (aip_open(path, &pkg, error, sizeof(error)) < 0) {
        have_pkg = false;
        ui_message(win, "Packages", error, "OK");
        return;
    }
    have_pkg = true;
    ui_set_text(ui_get(win, "name"), pkg.name);
    snprintf(text, sizeof(text), "Version %s from %s. Not signed: install it only if you trust where it came from.",
             *pkg.version ? pkg.version : "?", *pkg.publisher ? pkg.publisher : "an unknown publisher");
    ui_set_text(ui_get(win, "publisher"), text);
    ui_set_text(ui_get(win, "about"), pkg.description);
    ui_format_size(pkg.payload, size, sizeof(size));
    snprintf(text, sizeof(text), "%u files, %s", pkg.nfiles, size);
    ui_set_text(ui_get(win, "size"), text);
    load_pkg_icon();
    ui_redraw(ui_get(win, "icon"));

    while (ui_children(box))
        ui_remove(ui_child(box, 0));
    nreq = aip_requested(&pkg, req, 16);
    for (int i = 0; i < nreq; i++) {
        char label[200];

        snprintf(label, sizeof(label), "%s  \xe2\x80\x94  %s", req[i]->description, aip_tier_name(req[i]->tier));
        checks[i] = ui_create(win, "checkbox");
        ui_set_text(checks[i], label);
        // Basic permissions start granted; the others are the user's choice.
        ui_set_value(checks[i], req[i]->tier == AIP_TIER_BASIC);
        // Elevated and above: administrators only.
        ui_set_enabled(checks[i], req[i]->tier == AIP_TIER_BASIC || admin);
        ui_set_handler(checks[i], "change", on_check, NULL);
        ui_add(box, checks[i]);
    }
    if (!nreq) {
        struct widget *l = ui_create(win, "label");

        ui_set_text(l, "None. It only uses its own files.");
        ui_set_attr(l, "dim", "true");
        ui_add(box, l);
    }
    ui_set_enabled(ui_get(win, "everyone"), strcmp(pkg.scope, "user") != 0);
    ui_set_enabled(ui_get(win, "me"), strcmp(pkg.scope, "machine") != 0);
    ui_set_value(ui_get(win, strcmp(pkg.scope, "machine") ? "me" : "everyone"), 1);
    ui_set_value(ui_get(win, "pages"), 1);
    ui_window_set_title(win, "Install a package - Packages");
    update_warning();
    ui_relayout(win);
    ui_focus(ui_get(win, "install"));
}

static void on_install(struct widget *w, void *u)
{
    bool everyone = ui_value(ui_get(win, "everyone")) != 0;
    char granted[256] = "", error[256], msg[400];
    int tier = highest_granted_tier();
    bool elevated = false;

    (void)w;
    (void)u;
    if (!have_pkg)
        return;
    for (int i = 0; i < nreq; i++) {
        if (!ui_value(checks[i]))
            continue;
        if (*granted)
            strlcat(granted, ",", sizeof(granted));
        strlcat(granted, req[i]->id, sizeof(granted));
    }
    // Everyone, and System permissions, need an administrator's password.
    if (everyone || tier >= AIP_TIER_SYSTEM) {
        if (!ui_elevate(win, everyone ? "Installing for everyone changes the whole computer."
                                      : "System permissions let the app change the whole computer."))
            return;
        elevated = true;
    }
    if (elevated && !everyone)
        seteuid(getuid());      // just me: the files are mine
    if (aip_install(&pkg, everyone, granted, error, sizeof(error)) < 0) {
        ui_message(win, "Packages", error, "OK");
        seteuid(getuid());
        return;
    }
    seteuid(getuid());
    syslog("packages", "%s installed %s %s%s", me.name, pkg.id, pkg.version, everyone ? " for everyone" : "");
    notify("Packages", "App installed", pkg.name);
    snprintf(msg, sizeof(msg), "%s is installed. Find it in the launcher.", pkg.name);
    if (ui_message(win, "Packages", msg, "Open it now|Done") == 0) {
        struct app_info a;
        char id[48];

        snprintf(id, sizeof(id), "aip-%s", pkg.id);
        if (app_find(id, &a) == 0)
            app_launch(&a, NULL);
    }
    aip_close(&pkg);
    have_pkg = false;
    refresh_installed();
    ui_set_value(ui_get(win, "pages"), 0);
    ui_window_set_title(win, "Packages");
}

static void on_cancel(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (have_pkg)
        aip_close(&pkg);
    have_pkg = false;
    ui_set_value(ui_get(win, "pages"), 0);
    ui_window_set_title(win, "Packages");
}

static void on_open(struct widget *w, void *u)
{
    char start[300], *path;

    (void)w;
    (void)u;
    user_path(&me, "home/Downloads", start, sizeof(start));
    if ((path = ui_file_dialog(win, "Install a package", start, false, NULL))) {
        show_package(path);
        free(path);
    }
}

static struct aip_installed *selected_app(void)
{
    int i = ui_list_selected(ui_get(win, "installed"));

    return i >= 0 && i < ninstalled ? &installed[i] : NULL;
}

static void on_picked(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_set_enabled(ui_get(win, "repair"), selected_app() != NULL);
    ui_set_enabled(ui_get(win, "remove"), selected_app() != NULL);
}

static void on_remove(struct widget *w, void *u)
{
    struct aip_installed *a = selected_app();
    char msg[300], error[256];

    (void)w;
    (void)u;
    if (!a)
        return;
    snprintf(msg, sizeof(msg), "Uninstall %s%s?", a->name, a->everyone ? " for everyone" : "");
    if (ui_message(win, "Uninstall", msg, "Uninstall|Cancel") != 0)
        return;
    if (a->everyone && !ui_elevate(win, "This app is installed for everyone."))
        return;
    if (aip_uninstall(a->id, a->everyone, error, sizeof(error)) < 0)
        ui_message(win, "Packages", error, "OK");
    else
        ui_set_text(ui_get(win, "status"), "Uninstalled.");
    seteuid(getuid());
    refresh_installed();
}

static void on_repair(struct widget *w, void *u)
{
    struct aip_installed *a = selected_app();
    char error[256];

    (void)w;
    (void)u;
    if (!a)
        return;
    if (a->everyone && !ui_elevate(win, "This app is installed for everyone."))
        return;
    if (aip_repair(a, error, sizeof(error)) < 0)
        ui_message(win, "Packages", error, "OK");
    else
        ui_set_text(ui_get(win, "status"), "Repaired: its files are back as installed.");
    seteuid(getuid());
    refresh_installed();
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "open", on_open }, { "picked", on_picked }, { "repair", on_repair }, { "remove", on_remove },
        { "install", on_install }, { "cancel", on_cancel }, { "check", on_check }, { NULL, NULL },
    };

    ui_load_user_theme();
    user_current(&me);
    if (!(win = ui_load_string_named(page, handlers, NULL, "packages")))
        return 1;
    ui_canvas_set(ui_get(win, "icon"), paint_icon, NULL, NULL);
    ui_set_handler(ui_get(win, "everyone"), "change", on_check, NULL);
    refresh_installed();
    if (argc > 1)
        show_package(argv[1]);
    ui_window_show(win);
    return ui_run();
}
