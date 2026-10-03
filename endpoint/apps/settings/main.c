#include "aegis.h"
#include "ui.h"

// Settings.

static const char page[] =
    "<window title='Settings' width='820' height='560' padding='0' spacing='0'>"
    "  <hbox expand='1' spacing='0'>"
    "    <list id='pages' width='200' onselect='page'/>"
    "    <stack id='stack' expand='1'>"
    // Appearance
    "      <scroll><vbox padding='24' spacing='14'>"
    "        <h1 text='Appearance'/>"
    "        <h2 text='Theme'/>"
    "        <hbox spacing='18'>"
    "          <radio id='light' group='theme' text='Light' onchange='theme'/>"
    "          <radio id='dark' group='theme' text='Dark' onchange='theme'/>"
    "          <radio id='contrast' group='theme' text='High contrast' onchange='theme'/>"
    "        </hbox>"
    "        <h2 text='Background'/>"
    "        <canvas id='preview' expand='0' height='180' width='320'/>"
    "        <hbox spacing='8'>"
    "          <dropdown id='bg' width='220' onchange='background'>"
    "            <option>Aegis (default)</option><option>Midnight</option><option>Slate</option>"
    "            <option>Forest</option><option>Plum</option><option>Sand</option><option>Picture</option>"
    "          </dropdown>"
    "          <button text='Choose picture...' onclick='picture'/>"
    "        </hbox>"
    "      </vbox></scroll>"
    // Account
    "      <scroll><vbox padding='24' spacing='14'>"
    "        <h1 text='Account'/>"
    "        <hbox spacing='18'>"
    "          <canvas id='avatar' expand='0' width='96' height='96'/>"
    "          <vbox spacing='6' align='center'>"
    "            <h2 id='fullname'/>"
    "            <label id='username' dim='true'/>"
    "            <label id='role' dim='true'/>"
    "          </vbox>"
    "        </hbox>"
    "        <hbox spacing='8'>"
    "          <button text='Change picture...' onclick='avatar'/>"
    "          <button text='Remove picture' onclick='noavatar'/>"
    "        </hbox>"
    "        <grid columns='2' spacing='10'>"
    "          <label text='Display name'/>"
    "          <hbox spacing='8'><input id='display' expand='1' onactivate='rename'/>"
    "            <button text='Apply' onclick='rename'/></hbox>"
    "        </grid>"
    "        <h2 text='Password'/>"
    "        <grid columns='2' spacing='10'>"
    "          <label text='Current password'/><password id='oldpw'/>"
    "          <label text='New password'/><password id='newpw'/>"
    "          <label text='Repeat new password'/><password id='newpw2' onactivate='password'/>"
    "        </grid>"
    "        <hbox><button text='Change password' onclick='password'/></hbox>"
    "        <label id='pwstatus' dim='true'/>"
    "      </vbox></scroll>"
    // Language and region
    "      <scroll><vbox padding='24' spacing='14'>"
    "        <h1 text='Language and region'/>"
    "        <grid columns='2' spacing='10' stretch=''>"
    "          <label text='Language'/>"
    "          <dropdown id='language' width='260' onchange='language'>"
    "            <option>English</option><option>Espa&#xF1;ol</option><option>Fran&#xE7;ais</option>"
    "            <option>Deutsch</option><option>&#x4E2D;&#x6587;&#xFF08;&#x7B80;&#x4F53;&#xFF09;</option>"
    "          </dropdown>"
    "          <label text='Time zone'/>"
    "          <dropdown id='zone' width='260' onchange='zone'/>"
    "        </grid>"
    "        <p dim='true'>Apps use the new language the next time they start. Changing the time zone"
    "           affects everyone on this computer and needs an administrator.</p>"
    "        <label id='now' size='large'/>"
    "      </vbox></scroll>"
    // Network
    "      <vbox padding='24' spacing='14'>"
    "        <h1 text='Network'/>"
    "        <table id='nics' expand='1' columns='Interface:110|Address:140|Gateway:130|Hardware:170|Traffic'/>"
    "        <hbox><button text='Renew address (DHCP)' onclick='dhcp'/><spacer/>"
    "          <label id='dns' dim='true' align='center'/></hbox>"
    "      </vbox>"
    // About
    "      <scroll><vbox padding='24' spacing='14'>"
    "        <h1 text='About this computer'/>"
    "        <grid columns='2' spacing='10'>"
    "          <label text='Computer name' dim='true'/>"
    "          <hbox spacing='8'><input id='hostname' width='220'/>"
    "            <button text='Rename' onclick='hostname'/></hbox>"
    "          <label text='System' dim='true'/><label id='system'/>"
    "          <label text='Processors' dim='true'/><label id='cpus'/>"
    "          <label text='Memory' dim='true'/><label id='memory'/>"
    "          <label text='Disk' dim='true'/><label id='disk'/>"
    "          <label text='Running for' dim='true'/><label id='uptime'/>"
    "        </grid>"
    "      </vbox></scroll>"
    "    </stack>"
    "  </hbox>"
    "</window>";

static struct ui_window *win;
static struct user_info me;
static char wallpaper[256];

static const char *const page_names[] = { "Appearance", "Account", "Language and region", "Network", "About" };
static const char *const page_icons[] = { "glyph:image", "glyph:user", "glyph:globe", "glyph:network",
                                          "glyph:info" };
static const char *const bg_specs[] = { "default", "color:#14213D", "color:#2E3440", "color:#1F3B2C",
                                        "color:#3B2441", "color:#C2A878" };
static const char *const languages[] = { "en", "es", "fr", "de", "zh" };
static const struct {
    const char *name;
    int minutes;
} zones[] = {
    { "UTC", 0 }, { "Europe/London", 0 }, { "Europe/Paris", 60 }, { "Europe/Berlin", 60 }, { "Europe/Madrid", 60 },
    { "Europe/Athens", 120 }, { "Europe/Moscow", 180 }, { "Asia/Dubai", 240 }, { "Asia/Kolkata", 330 },
    { "Asia/Shanghai", 480 }, { "Asia/Tokyo", 540 }, { "Australia/Sydney", 600 }, { "Pacific/Auckland", 720 },
    { "America/Sao_Paulo", -180 }, { "America/New_York", -300 }, { "America/Chicago", -360 },
    { "America/Denver", -420 }, { "America/Los_Angeles", -480 }, { "Pacific/Honolulu", -600 },
};

// ---- Appearance ----

static void paint_preview(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    wallpaper_draw(g, r, wallpaper);
    // A miniature window and taskbar for scale.
    gfx_fill(g, (struct rect){ r.x, r.y + r.h - 14, r.w, 14 }, ALPHA(0x11151D, 0xEE));
    gfx_fill_rounded(g, (struct rect){ r.x + r.w / 4, r.y + r.h / 5, r.w / 2, r.h / 2 }, 4, RGB(0xF3F5F8));
    gfx_fill(g, (struct rect){ r.x + r.w / 4, r.y + r.h / 5, r.w / 2, 10 }, RGB(0xE1E6EE));
}

static void set_background(const char *spec)
{
    strlcpy(wallpaper, spec, sizeof(wallpaper));
    user_setting_set(&me, "background", spec);
    wm_setting_changed("background", spec);
    ui_redraw(ui_get(win, "preview"));
}

static void on_theme(struct widget *w, void *u)
{
    const char *name = ui_id(w);

    (void)u;
    if (!ui_value(w))
        return;
    if (!strcmp(name, "contrast"))
        name = "high-contrast";
    user_setting_set(&me, "theme", name);
    ui_set_theme(name);
    wm_setting_changed("theme", name);
}

static void on_background(struct widget *w, void *u)
{
    int i = ui_list_selected(w);

    (void)u;
    if (i >= 0 && i < 6)
        set_background(bg_specs[i]);
}

static void on_picture(struct widget *w, void *u)
{
    char start[256];
    char *p;

    (void)w;
    (void)u;
    snprintf(start, sizeof(start), "%s/Images", me.home);
    if (!(p = ui_file_dialog_filtered(win, "Choose a background", start, false, NULL,
                                      "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga")))
        return;
    set_background(p);
    ui_list_select(ui_get(win, "bg"), 6);
    free(p);
}

// ---- Account ----

static void paint_avatar(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    char pic[256] = "";

    (void)w;
    (void)u;
    user_setting_get(&me, "picture", pic, sizeof(pic));
    avatar_draw(g, r, me.display, pic);
}

static void show_account(void)
{
    char buf[128];

    ui_set_text(ui_get(win, "fullname"), me.display);
    snprintf(buf, sizeof(buf), "Signed in as %s", me.name);
    ui_set_text(ui_get(win, "username"), buf);
    ui_set_text(ui_get(win, "role"), account_is_admin(me.name) ? "Administrator" : "Standard account");
    ui_set_text(ui_get(win, "display"), me.display);
    ui_redraw(ui_get(win, "avatar"));
}

static void on_avatar(struct widget *w, void *u)
{
    char start[256], dst[256];
    char *p;
    const char *ext;

    (void)w;
    (void)u;
    snprintf(start, sizeof(start), "%s/Images", me.home);
    if (!(p = ui_file_dialog_filtered(win, "Choose a picture", start, false, NULL, "*.png;*.jpg;*.jpeg;*.bmp")))
        return;
    // Keep a copy, so the picture survives the original being moved.
    ext = strrchr(p, '.');
    snprintf(dst, sizeof(dst), "%s/system/settings/picture%s", me.dir, ext ? ext : "");
    unlink(dst);
    if (copy_path(p, dst) == 0) {
        user_setting_set(&me, "picture", dst);
        wm_setting_changed("picture", dst);
    } else {
        ui_message(win, "Settings", "The picture could not be copied.", "OK");
    }
    free(p);
    show_account();
}

static void on_noavatar(struct widget *w, void *u)
{
    char path[256];

    (void)w;
    (void)u;
    snprintf(path, sizeof(path), "%s/system/settings/picture", me.dir);
    unlink(path);
    wm_setting_changed("picture", "");
    show_account();
}

static void on_rename(struct widget *w, void *u)
{
    const char *name = ui_text(ui_get(win, "display"));

    (void)w;
    (void)u;
    if (!*name || strchr(name, ':') || strchr(name, '\n')) {
        ui_message(win, "Settings", "Choose a name without colons.", "OK");
        return;
    }
    user_setting_set(&me, "name", name);
    strlcpy(me.display, name, sizeof(me.display));
    wm_setting_changed("name", name);
    show_account();
}

static void on_password(struct widget *w, void *u)
{
    const char *old = ui_text(ui_get(win, "oldpw")), *n1 = ui_text(ui_get(win, "newpw"));
    const char *n2 = ui_text(ui_get(win, "newpw2"));
    char err[160];

    (void)w;
    (void)u;
    if (strcmp(n1, n2)) {
        ui_set_text(ui_get(win, "pwstatus"), "The new passwords do not match.");
        return;
    }
    if (strlen(n1) < 4) {
        ui_set_text(ui_get(win, "pwstatus"), "Use at least four characters.");
        return;
    }
    ui_set_text(ui_get(win, "pwstatus"), "Changing...");
    if (change_own_password(old, n1, err, sizeof(err)) < 0) {
        ui_set_text(ui_get(win, "pwstatus"), err);
        return;
    }
    {
        // Stored passwords follow: their key is resealed with the new one.
        struct user_info me;

        if (user_current(&me) == 0)
            cred_rewrap(&me, old, n1);
    }
    ui_set_text(ui_get(win, "oldpw"), "");
    ui_set_text(ui_get(win, "newpw"), "");
    ui_set_text(ui_get(win, "newpw2"), "");
    ui_set_text(ui_get(win, "pwstatus"), "Your password has been changed.");
}

// ---- Language and region ----

static void on_language(struct widget *w, void *u)
{
    int i = ui_list_selected(w);

    (void)u;
    if (i >= 0)
        user_setting_set(&me, "language", languages[i]);
}

static void show_time(void)
{
    int64_t now = time(NULL);
    struct tm tm;
    char buf[96];

    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%A, %B %e %Y, %H:%M (%Z, UTC%z)", &tm);
    ui_set_text(ui_get(win, "now"), buf);
}

static void on_zone(struct widget *w, void *u)
{
    int i = ui_list_selected(w);
    char line[96];

    (void)u;
    if (i < 0 || !strcmp(timezone_name(), zones[i].name))
        return;
    if (!ui_elevate(win, "The time zone is shared by everyone on this computer.")) {
        // Put the old choice back.
        for (size_t k = 0; k < sizeof(zones) / sizeof(zones[0]); k++)
            if (!strcmp(zones[k].name, timezone_name()))
                ui_list_select(w, k);
        return;
    }
    snprintf(line, sizeof(line), "%d", zones[i].minutes);
    aset_set(COMPUTER_FILE, "timezone", zones[i].name, 0644);
    aset_set(COMPUTER_FILE, "timezone-offset", line, 0644);
    setenv("TZ_OFFSET", "");
    wm_setting_changed("timezone", zones[i].name);
    msleep(10);
    {
        char off[16];

        // This process sees the change at once; others within seconds.
        snprintf(off, sizeof(off), "%d", zones[i].minutes);
        setenv("TZ_OFFSET", off);
    }
    show_time();
}

// ---- Network ----

static void show_network(void)
{
    struct widget *t = ui_get(win, "nics");
    struct aegis_netif nif;
    char dns[96] = "DNS: ";

    ui_list_clear(t);
    for (int i = 0; netconfig(NETCONFIG_GET, i, &nif) == 0; i++) {
        char addr[20], gw[20], row[300], rx[24], tx[24];
        int bits = 0;

        for (uint32_t m = nif.netmask; m; m &= m - 1)
            bits++;

        inet_format(nif.addr, addr);
        inet_format(nif.gateway, gw);
        ui_format_size(nif.rx_bytes, rx, sizeof(rx));
        ui_format_size(nif.tx_bytes, tx, sizeof(tx));
        snprintf(row, sizeof(row), "%s\t%s/%d\t%s\t%02x:%02x:%02x:%02x:%02x:%02x\t%s in, %s out", nif.name,
                 nif.addr ? addr : "none", bits, nif.gateway ? gw : "-", nif.mac[0], nif.mac[1], nif.mac[2],
                 nif.mac[3], nif.mac[4], nif.mac[5], rx, tx);
        ui_list_add(t, row);
        if (nif.dns[0] && strlen(dns) < 8) {
            char d[20];

            inet_format(nif.dns[0], d);
            strlcat(dns, d, sizeof(dns));
        }
    }
    ui_set_text(ui_get(win, "dns"), strlen(dns) > 5 ? dns : "");
}

static void on_dhcp(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!ui_elevate(win, "Renewing the network address affects everyone on this computer."))
        return;
    for (int i = 0; netconfig(NETCONFIG_DHCP, i, NULL) == 0; i++)
        ;
    msleep(1500);
    show_network();
}

// ---- About ----

static void show_about(void)
{
    struct aegis_utsname un;
    struct aegis_sysinfo si;
    struct aegis_statfs fs;
    char buf[160], a[32], b[32];

    if (uname(&un) == 0) {
        snprintf(buf, sizeof(buf), "%s %s (%s)", un.sysname, un.release, un.machine);
        ui_set_text(ui_get(win, "system"), buf);
    }
    if (sysinfo(&si) == 0) {
        uint64_t s = si.uptime_ms / 1000;

        snprintf(buf, sizeof(buf), "%u", si.cpus);
        ui_set_text(ui_get(win, "cpus"), buf);
        ui_format_size(si.memory_total, a, sizeof(a));
        ui_format_size(si.memory_total - si.memory_free, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%s (%s in use)", a, b);
        ui_set_text(ui_get(win, "memory"), buf);
        snprintf(buf, sizeof(buf), "%lu h %lu min", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
        ui_set_text(ui_get(win, "uptime"), buf);
    }
    if (statfs("/", &fs) == 0) {
        ui_format_size(fs.blocks * fs.block_size, a, sizeof(a));
        ui_format_size(fs.blocks_free * fs.block_size, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%s, %s free (%s)", a, b, fs.fstype);
        ui_set_text(ui_get(win, "disk"), buf);
    }
    if (aset_get(COMPUTER_FILE, "name", buf, sizeof(buf)) > 0)
        ui_set_text(ui_get(win, "hostname"), buf);
}

static void on_hostname(struct widget *w, void *u)
{
    const char *name = ui_text(ui_get(win, "hostname"));

    (void)w;
    (void)u;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-') {
            ui_message(win, "Settings", "Use letters, digits and dashes only.", "OK");
            return;
        }
    if (!*name || !ui_elevate(win, "The computer's name is shared by everyone on it."))
        return;
    if (aset_set(COMPUTER_FILE, "name", name, 0644) == 0)
        ui_message(win, "Settings", "The computer has a new name.", "OK");
}

static void on_page(struct widget *w, void *u)
{
    int i = ui_list_selected(w);

    (void)u;
    if (i < 0)
        return;
    ui_set_value(ui_get(win, "stack"), i);
    if (i == 3)
        show_network();
    if (i == 4)
        show_about();
}

static bool tick(void *u)
{
    (void)u;
    show_time();
    return true;
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "page", on_page }, { "theme", on_theme }, { "background", on_background }, { "picture", on_picture },
        { "avatar", on_avatar }, { "noavatar", on_noavatar }, { "rename", on_rename },
        { "password", on_password }, { "language", on_language }, { "zone", on_zone }, { "dhcp", on_dhcp },
        { "hostname", on_hostname }, { NULL, NULL },
    };
    struct widget *pages, *zl;
    char buf[64];
    int start = 0;

    ui_load_user_theme();
    if (user_current(&me) < 0)
        return 1;
    if (!(win = ui_load_string_named(page, handlers, NULL, "settings")))
        return 1;
    pages = ui_get(win, "pages");
    for (int i = 0; i < 5; i++) {
        ui_list_add(pages, page_names[i]);
        ui_list_set_icon_shared(pages, i, icon_get(page_icons[i], 20));
        if (argc > 1 && !strcasecmp(argv[1], page_names[i]))
            start = i;
    }
    // Current values.
    if (user_setting_get(&me, "theme", buf, sizeof(buf)) <= 0)
        strlcpy(buf, "light", sizeof(buf));
    ui_set_value(ui_get(win, !strcmp(buf, "dark") ? "dark" : !strncmp(buf, "high", 4) ? "contrast" : "light"), 1);
    if (user_setting_get(&me, "background", wallpaper, sizeof(wallpaper)) <= 0)
        strlcpy(wallpaper, "default", sizeof(wallpaper));
    ui_list_select(ui_get(win, "bg"), 6);
    for (int i = 0; i < 6; i++)
        if (!strcmp(wallpaper, bg_specs[i]))
            ui_list_select(ui_get(win, "bg"), i);
    ui_canvas_set(ui_get(win, "preview"), paint_preview, NULL, NULL);
    ui_canvas_set(ui_get(win, "avatar"), paint_avatar, NULL, NULL);
    if (user_setting_get(&me, "language", buf, sizeof(buf)) > 0)
        for (int i = 0; i < 5; i++)
            if (!strcmp(buf, languages[i]))
                ui_list_select(ui_get(win, "language"), i);
    zl = ui_get(win, "zone");
    for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
        char row[80];
        int m = zones[i].minutes;

        snprintf(row, sizeof(row), "%s (UTC%c%02d:%02d)", zones[i].name, m < 0 ? '-' : '+', abs(m) / 60,
                 abs(m) % 60);
        ui_list_add(zl, row);
        if (!strcmp(zones[i].name, timezone_name()))
            ui_list_select(zl, i);
    }
    show_account();
    show_time();
    ui_timer(1000, tick, NULL);
    ui_list_select(pages, start);
    ui_set_value(ui_get(win, "stack"), start);
    ui_window_show(win);
    return ui_run();
}
