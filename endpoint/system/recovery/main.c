#include "aegis.h"
#include "ui.h"

// Aegis Recovery: what the Recovery boot entry starts (the live system from
// the EFI partition, with "recovery" on its command line). It finds the
// installed system, mounts it at /mnt/system (its EFI partition at
// /mnt/esp) and offers ways to get it going again.

#define SYS     "/mnt/system"
#define ESP     "/mnt/esp"
#define BCD     ESP "/EFI/Aegis/bcd"

static const char page[] =
    "<window role='desktop' padding='0' spacing='0'>"
    "  <vbox expand='1' justify='center'>"
    "    <hbox justify='center'>"
    "      <card width='900' height='600' spacing='12'>"
    "        <hbox spacing='12'>"
    "          <button flat='true' symbol='shield' iconsize='36' focusable='false'/>"
    "          <vbox spacing='0' expand='1'>"
    "            <h2 text='Aegis Recovery'/>"
    "            <label id='system' dim='true'/>"
    "          </vbox>"
    "        </hbox>"
    "        <separator/>"
    "        <hbox spacing='14' expand='1'>"
    "          <list id='tools' width='210' onselect='tool'/>"
    "          <stack id='pages' value='0' expand='1'>"
    // 0: start
    "            <vbox spacing='10'>"
    "              <h2 text='Start Aegis'/>"
    "              <p text='Restart to try the installed system again, or start it once in safe mode: only what is"
    " needed to sign in, with scheduled jobs and sound off.'/>"
    "              <hbox spacing='8'><button text='Restart' default='true' onclick='restart'/>"
    "                <button id='safebtn' text='Restart in safe mode' onclick='safe'/>"
    "                <button text='Shut down' onclick='poweroff'/></hbox>"
    "              <separator/>"
    "              <h2 text='Firmware settings'/>"
    "              <p text='Restart into this computer&apos;s UEFI setup screen (boot order, Secure Boot, clock).'/>"
    "              <hbox><button id='fwbtn' text='Restart to UEFI firmware settings' onclick='firmware'/></hbox>"
    "            </vbox>"
    // 1: startup settings
    "            <vbox spacing='8'>"
    "              <h2 text='Startup settings'/>"
    "              <label text='Services' bold='true'/>"
    "              <checkbox id='svc_console' text='Text console (Ctrl+Alt+F2)'/>"
    "              <checkbox id='svc_cron' text='Scheduled jobs'/>"
    "              <checkbox id='svc_audio' text='Sound'/>"
    "              <label text='Boot options' bold='true'/>"
    "              <checkbox id='verbose' text='Show startup messages instead of the logo'/>"
    "              <grid columns='2' stretch='1' spacing='8'>"
    "                <label text='Boot menu wait (seconds)'/><spin id='timeout' min='0' max='60'/>"
    "                <label text='Screen resolution'/><input id='resolution' placeholder='auto, max or 1280x800'/>"
    "                <label text='Extra kernel options'/><input id='extra' placeholder='for example: debug'/>"
    "              </grid>"
    "              <spacer/>"
    "              <hbox spacing='8'><label id='startupmsg' dim='true' expand='1'/>"
    "                <button text='Save' default='true' onclick='savestartup'/></hbox>"
    "            </vbox>"
    // 2: BCD
    "            <vbox spacing='8'>"
    "              <h2 text='Boot configuration (BCD)'/>"
    "              <label dim='true' wrap='true' text='The boot menu&apos;s settings and entries, as stored on the EFI"
    " partition. Global settings come first, then one [section] per entry.'/>"
    "              <textarea id='bcd' expand='1' mono='true'/>"
    "              <hbox spacing='8'><label id='bcdmsg' dim='true' expand='1'/>"
    "                <button text='Reload' onclick='loadbcd'/><button text='Save' onclick='savebcd'/></hbox>"
    "            </vbox>"
    // 3: password
    "            <vbox spacing='10'>"
    "              <h2 text='Reset a password'/>"
    "              <grid columns='2' stretch='1' spacing='8'>"
    "                <label text='Account'/><dropdown id='users'/>"
    "                <label text='New password'/><password id='pw1'/>"
    "                <label text='Confirm'/><password id='pw2' onactivate='resetpw'/>"
    "              </grid>"
    "              <p dim='true' text='Saved passwords in the account&apos;s encrypted credentials cannot be recovered:"
    " they are set aside, and the account starts a new store.'/>"
    "              <hbox spacing='8'><label id='pwmsg' expand='1'/><button text='Reset password' onclick='resetpw'/></hbox>"
    "            </vbox>"
    // 4: system image
    "            <vbox spacing='8'>"
    "              <h2 text='System image'/>"
    "              <p text='An image holds the whole system: apps, settings, accounts and everyone&apos;s files."
    " Images are kept in /var/backups on the system, which restoring leaves alone.'/>"
    "              <table id='images' expand='1' columns='Image|Made:170|Size:90:right'"
    "                     placeholder='No images yet.'/>"
    "              <progress id='imgprogress' value='0'/>"
    "              <hbox spacing='8'><label id='imgmsg' dim='true' expand='1'/>"
    "                <button text='Create image' onclick='mkimage'/>"
    "                <button text='Restore' onclick='restore'/></hbox>"
    "            </vbox>"
    // 5: reinstall
    "            <vbox spacing='10'>"
    "              <h2 text='Reinstall Aegis'/>"
    "              <p text='Puts back a fresh copy of the system files, boot loader and kernel. Accounts, settings,"
    " installed apps and everyone&apos;s files stay.'/>"
    "              <hbox><button text='Reinstall, keeping files' onclick='reinstall'/></hbox>"
    "              <progress id='reprogress' value='0'/>"
    "              <label id='remsg' dim='true'/>"
    "              <separator/>"
    "              <p text='Or erase the whole disk and install again from the start.'/>"
    "              <hbox><button text='Erase and install...' onclick='erase'/></hbox>"
    "            </vbox>"
    // 6: terminal
    "            <vbox spacing='10'>"
    "              <h2 text='Recovery terminal'/>"
    "              <p text='A terminal as root. The installed system is at /mnt/system and its EFI partition at"
    " /mnt/esp. Changes take effect at once: take care.'/>"
    "              <hbox><button text='Open terminal' onclick='terminal'/></hbox>"
    "            </vbox>"
    "          </stack>"
    "        </hbox>"
    "      </card>"
    "    </hbox>"
    "  </vbox>"
    "</window>";

static const char *const tool_names[] = { "Start Aegis", "Startup settings", "Boot configuration", "Reset a password",
                                          "System image", "Reinstall", "Terminal" };
static const char *const tool_icons[] = { "glyph:power", "glyph:gear", "glyph:code", "glyph:key", "glyph:disk",
                                          "glyph:refresh", "glyph:apps" };

static struct ui_window *win;
static bool have_system, have_esp;
static char sys_dev[24], esp_dev[24];
static int pipe_fds[2];
static volatile bool busy;

// ---- Small file helpers ----

static char *slurp(const char *path)
{
    struct aegis_stat st;
    int fd;
    char *buf;
    ssize_t n;

    if (stat(path, &st) < 0 || (fd = open(path, O_RDONLY)) < 0)
        return NULL;
    if (!(buf = malloc(st.size + 1))) {
        close(fd);
        return NULL;
    }
    n = read(fd, buf, st.size);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return buf;
}

// FAT cannot rename over a file: write beside it, then swap.
static int spill(const char *path, const char *text, uint32_t mode)
{
    char tmp[300];
    int fd;
    size_t n = strlen(text);

    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode)) < 0)
        return -1;
    if (write(fd, text, n) != (ssize_t)n) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    unlink(path);
    if (rename(tmp, path) < 0)
        return -1;
    sync();
    return 0;
}

// ---- Finding the installed system ----

static void find_system(void)
{
    struct dir_stream *d = opendir("/dev");
    struct aegis_dirent *de;
    char parent[16] = "";
    struct aegis_stat st;

    mkdir("/mnt", 0755);
    mkdir(SYS, 0755);
    mkdir(ESP, 0755);
    if (!d) {
        dprintf(STDERR_FILENO, "recovery: cannot read /dev: %s\n", strerror(errno));
        return;
    }
    while (!have_system && (de = readdir(d))) {
        struct aegis_blockinfo bi;
        char path[64], guid[37];
        int fd;

        if (de->type != DT_BLK)
            continue;
        snprintf(path, sizeof(path), "/dev/%s", de->name);
        if ((fd = open(path, O_RDONLY)) < 0)
            continue;
        if (ioctl(fd, IOCTL_BLOCK_INFO, (uint64_t)&bi) == 0 && (bi.flags & BLOCK_INFO_PARTITION)
            && !(bi.flags & BLOCK_INFO_MOUNTED) && bi.sector_count) {
            guid_to_string(bi.type_guid, guid);
            if (!strcmp(guid, DISK_TYPE_DATA) && mount("ext4", de->name, SYS, 0) < 0)
                dprintf(STDERR_FILENO, "recovery: cannot mount %s: %s\n", de->name, strerror(errno));
            else if (!strcmp(guid, DISK_TYPE_DATA)) {
                if (stat(SYS "/etc/passwd", &st) == 0) {
                    have_system = true;
                    strlcpy(sys_dev, de->name, sizeof(sys_dev));
                    strlcpy(parent, bi.parent, sizeof(parent));
                } else {
                    umount(SYS);
                }
            }
        }
        close(fd);
    }
    closedir(d);
    // Its EFI partition: on the same disk.
    if (have_system && (d = opendir("/dev"))) {
        while (!have_esp && (de = readdir(d))) {
            struct aegis_blockinfo bi;
            char path[64], guid[37];
            int fd;

            if (de->type != DT_BLK)
                continue;
            snprintf(path, sizeof(path), "/dev/%s", de->name);
            if ((fd = open(path, O_RDONLY)) < 0)
                continue;
            if (ioctl(fd, IOCTL_BLOCK_INFO, (uint64_t)&bi) == 0 && (bi.flags & BLOCK_INFO_PARTITION)
                && !strcmp(bi.parent, parent)) {
                guid_to_string(bi.type_guid, guid);
                if (!strcmp(guid, DISK_TYPE_ESP) && mount("fat", de->name, ESP, 0) == 0) {
                    have_esp = true;
                    strlcpy(esp_dev, de->name, sizeof(esp_dev));
                }
            }
            close(fd);
        }
        closedir(d);
    }
}

// ---- features.conf on the installed system ----

static bool feature_on(const char *text, const char *name, bool def)
{
    size_t n = strlen(name);

    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, name, n) && l[n] == '=')
            return !strncmp(l + n + 1, "on", 2);
    return def;
}

static void features_set(const char *const *names, const bool *values, int count)
{
    char *old = slurp(SYS "/etc/features.conf"), out[2048] = "";

    // Keep other lines; replace the ones being set.
    for (const char *l = old ? old : ""; *l;) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        bool replaced = false;

        for (int i = 0; i < count; i++) {
            size_t n = strlen(names[i]);

            if (len > n && !strncmp(l, names[i], n) && l[n] == '=')
                replaced = true;
        }
        if (!replaced && len)
            snprintf(out + strlen(out), sizeof(out) - strlen(out), "%.*s\n", (int)len, l);
        l += len + (e ? 1 : 0);
    }
    for (int i = 0; i < count; i++)
        snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s=%s\n", names[i], values[i] ? "on" : "off");
    spill(SYS "/etc/features.conf", out, 0644);
    free(old);
}

// ---- BCD ----

static void bcd_global(const char *bcd, const char *key, char *out, size_t size)
{
    size_t n = strlen(key);

    out[0] = 0;
    for (const char *l = bcd; l && *l && *l != '['; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        if (!strncmp(l, key, n) && l[n] == '=') {
            const char *e = strchr(l, '\n');
            size_t len = e ? (size_t)(e - l - n - 1) : strlen(l + n + 1);

            snprintf(out, size, "%.*s", (int)len, l + n + 1);
            return;
        }
    }
}

// The [aegis] entry's command line, without root= (the extra options).
static void bcd_extra(const char *bcd, char *root, size_t rsize, char *extra, size_t esize)
{
    const char *s = strstr(bcd, "[aegis]"), *c;

    root[0] = extra[0] = 0;
    if (!s || !(c = strstr(s, "cmdline=")))
        return;
    c += 8;
    while (*c && *c != '\n') {
        const char *e = c;
        size_t len;

        while (*e && *e != ' ' && *e != '\n')
            e++;
        len = e - c;
        if (len > 5 && !strncmp(c, "root=", 5))
            snprintf(root, rsize, "%.*s", (int)len, c);
        else if (len)
            snprintf(extra + strlen(extra), esize - strlen(extra), "%s%.*s", *extra ? " " : "", (int)len, c);
        c = *e == ' ' ? e + 1 : e;
    }
}

// Rewrites global keys and the normal and verbose entries' command lines.
static char *bcd_update(const char *bcd, const char *timeout, const char *resolution, const char *def,
                        const char *root, const char *extra)
{
    size_t cap = strlen(bcd) + 1024;
    char *out = malloc(cap), section[40] = "";

    if (!out)
        return NULL;
    out[0] = 0;
    for (const char *l = bcd; *l;) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[600];

        snprintf(line, sizeof(line), "%.*s", (int)len, l);
        if (line[0] == '[')
            snprintf(section, sizeof(section), "%s", line);
        if (!*section && !strncmp(line, "timeout=", 8))
            snprintf(line, sizeof(line), "timeout=%s", timeout);
        else if (!*section && !strncmp(line, "resolution=", 11))
            snprintf(line, sizeof(line), "resolution=%s", *resolution ? resolution : "auto");
        else if (!*section && !strncmp(line, "default=", 8))
            snprintf(line, sizeof(line), "default=%s", def);
        else if (!strncmp(line, "cmdline=", 8) && *root
                 && (!strcmp(section, "[aegis]") || !strcmp(section, "[verbose]") || !strcmp(section, "[safe]")))
            snprintf(line, sizeof(line), "cmdline=%s%s%s%s", root, *extra ? " " : "", extra,
                     !strcmp(section, "[verbose]") ? " verbose" : !strcmp(section, "[safe]") ? " safe" : "");
        snprintf(out + strlen(out), cap - strlen(out), "%s\n", line);
        l += len + (e ? 1 : 0);
    }
    return out;
}

// ---- Pages ----

static void load_startup(void)
{
    char *features = slurp(SYS "/etc/features.conf"), *bcd = have_esp ? slurp(BCD) : NULL;
    char v[64], root[96], extra[300];

    ui_set_value(ui_get(win, "svc_console"), feature_on(features ? features : "", "console", true));
    ui_set_value(ui_get(win, "svc_cron"), feature_on(features ? features : "", "cron", true));
    ui_set_value(ui_get(win, "svc_audio"), feature_on(features ? features : "", "audio", true));
    ui_set_value(ui_get(win, "verbose"), feature_on(features ? features : "", "bootlog", false));
    if (bcd) {
        bcd_global(bcd, "timeout", v, sizeof(v));
        ui_set_value(ui_get(win, "timeout"), atoi(v));
        bcd_global(bcd, "resolution", v, sizeof(v));
        ui_set_text(ui_get(win, "resolution"), v);
        bcd_extra(bcd, root, sizeof(root), extra, sizeof(extra));
        ui_set_text(ui_get(win, "extra"), extra);
    }
    free(features);
    free(bcd);
}

static void on_save_startup(struct widget *w, void *u)
{
    static const char *const names[] = { "console", "cron", "audio", "bootlog" };
    bool values[4] = { ui_value(ui_get(win, "svc_console")) != 0, ui_value(ui_get(win, "svc_cron")) != 0,
                       ui_value(ui_get(win, "svc_audio")) != 0, ui_value(ui_get(win, "verbose")) != 0 };
    char *bcd, *updated, timeout[16], root[96], extra[300];

    (void)w;
    (void)u;
    features_set(names, values, 4);
    if (have_esp && (bcd = slurp(BCD))) {
        bcd_extra(bcd, root, sizeof(root), extra, sizeof(extra));
        snprintf(timeout, sizeof(timeout), "%d", (int)ui_value(ui_get(win, "timeout")));
        updated = bcd_update(bcd, timeout, ui_text(ui_get(win, "resolution")), values[3] ? "verbose" : "aegis", root,
                             ui_text(ui_get(win, "extra")));
        if (!updated || spill(BCD, updated, 0644) < 0)
            ui_set_text(ui_get(win, "startupmsg"), "The boot configuration could not be saved.");
        else
            ui_set_text(ui_get(win, "startupmsg"), "Saved. The changes apply from the next start.");
        free(updated);
        free(bcd);
    } else {
        ui_set_text(ui_get(win, "startupmsg"), "Services saved. No boot configuration was found to change.");
    }
}

static void on_load_bcd(struct widget *w, void *u)
{
    char *bcd = have_esp ? slurp(BCD) : NULL;

    (void)w;
    (void)u;
    ui_set_text(ui_get(win, "bcd"), bcd ? bcd : "");
    ui_set_text(ui_get(win, "bcdmsg"), bcd ? BCD : "No boot configuration was found.");
    free(bcd);
}

static void on_save_bcd(struct widget *w, void *u)
{
    const char *text = ui_text(ui_get(win, "bcd"));

    (void)w;
    (void)u;
    if (!have_esp)
        return;
    if (!strstr(text, "[") || !strstr(text, "path=")) {
        ui_set_text(ui_get(win, "bcdmsg"), "That does not look like a boot configuration: it has no entries.");
        return;
    }
    if (ui_message(win, "Boot configuration", "Save the boot configuration? A mistake can stop Aegis from starting"
                   " (the boot menu then shows the error and the line).", "Save|Cancel") != 0)
        return;
    ui_set_text(ui_get(win, "bcdmsg"), spill(BCD, text, 0644) == 0 ? "Saved." : "It could not be saved.");
}

static void load_users(void)
{
    char *passwd = slurp(SYS "/etc/passwd");
    struct widget *dd = ui_get(win, "users");

    ui_list_clear(dd);
    for (char *l = passwd; l && *l;) {
        char *e = strchr(l, '\n'), name[64];
        char *c1 = strchr(l, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;

        if (e)
            *e = 0;
        if (c1 && c2 && atoi(c2 + 1) >= 1000) {
            snprintf(name, sizeof(name), "%.*s", (int)(c1 - l), l);
            ui_list_add(dd, name);
        }
        l = e ? e + 1 : NULL;
    }
    ui_list_select(dd, 0);
    free(passwd);
    {
        struct aegis_stat st;

        ui_set_text(ui_get(win, "pwmsg"), ui_list_count(dd) ? ""
                    : stat(SYS "/etc/firstboot", &st) == 0 ? "The account is made on the system's first start: start it once first."
                    : "This system has no accounts.");
    }
}

static void on_reset_pw(struct widget *w, void *u)
{
    struct widget *dd = ui_get(win, "users");
    const char *user = ui_list_item(dd, ui_list_selected(dd)), *p1 = ui_text(ui_get(win, "pw1"));
    char hash[160], *shadow, out[4096] = "", cred[300], old[320];
    size_t n;

    (void)w;
    (void)u;
    if (!user)
        return;
    if (strlen(p1) < 4 || strcmp(p1, ui_text(ui_get(win, "pw2")))) {
        ui_set_text(ui_get(win, "pwmsg"), strlen(p1) < 4 ? "Use at least 4 characters." : "The passwords differ.");
        return;
    }
    if (password_hash(p1, hash, sizeof(hash)) < 0 || !(shadow = slurp(SYS "/etc/shadow"))) {
        ui_set_text(ui_get(win, "pwmsg"), "The password file could not be read.");
        return;
    }
    n = strlen(user);
    for (char *l = shadow; *l;) {
        char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);

        if (len > n && !strncmp(l, user, n) && l[n] == ':') {
            const char *rest = strchr(l + n + 1, ':');

            snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s:%s%.*s\n", user, hash,
                     rest ? (int)(len - (rest - l)) : 1, rest ? rest : ":");
        } else if (len) {
            snprintf(out + strlen(out), sizeof(out) - strlen(out), "%.*s\n", (int)len, l);
        }
        l += len + (e ? 1 : 0);
    }
    free(shadow);
    memset(hash, 0, sizeof(hash));
    if (spill(SYS "/etc/shadow", out, 0600) < 0) {
        ui_set_text(ui_get(win, "pwmsg"), "The password could not be saved.");
        return;
    }
    // The old credential store is locked with the old password.
    snprintf(cred, sizeof(cred), SYS "/users/%s/system/credentials", user);
    snprintf(old, sizeof(old), "%s.old", cred);
    remove_path(old);
    if (rename(cred, old) == 0) {
        struct aegis_stat st;

        if (stat(old, &st) == 0 && mkdir(cred, 0700) == 0)
            chown(cred, st.uid, st.gid);
    }
    sync();
    ui_set_text(ui_get(win, "pw1"), "");
    ui_set_text(ui_get(win, "pw2"), "");
    syslog("recovery", "reset the password of %s", user);
    ui_set_text(ui_get(win, "pwmsg"), "Done. Sign in with the new password after restarting.");
}

// ---- Long jobs: images and reinstalling, on a thread ----

struct msg {
    int percent;
    int done;                       // 1 ok, -1 failed
    char text[200];
};

static void report(int percent, const char *what, void *u)
{
    struct msg m = { percent, 0, "" };

    (void)u;
    strlcpy(m.text, what, sizeof(m.text));
    write(pipe_fds[1], &m, sizeof(m));
}

static void finish(bool ok, const char *error)
{
    struct msg m = { 100, ok ? 1 : -1, "" };

    strlcpy(m.text, ok ? "Done." : error, sizeof(m.text));
    write(pipe_fds[1], &m, sizeof(m));
}

static int job_kind;                // 0 image create, 1 restore, 2 reinstall
static char job_arg[400];

static void *job_thread(void *arg)
{
    char error[256] = "";
    int r;

    (void)arg;
    if (job_kind == 0)
        r = sysimage_create(SYS, job_arg, report, NULL, error, sizeof(error));
    else if (job_kind == 1)
        r = sysimage_restore(job_arg, SYS, report, NULL, error, sizeof(error));
    else
        r = install_refresh(SYS, ESP, report, NULL, error, sizeof(error));
    finish(r == 0, error);
    return NULL;
}

static void load_images(void);

static void job_progress(int fd, void *u)
{
    struct msg m;
    const char *bar = job_kind == 2 ? "reprogress" : "imgprogress", *label = job_kind == 2 ? "remsg" : "imgmsg";

    (void)u;
    while (read(fd, &m, sizeof(m)) == sizeof(m)) {
        ui_set_value(ui_get(win, bar), m.percent);
        ui_set_text(ui_get(win, label), m.text);
        if (m.done) {
            busy = false;
            dprintf(STDERR_FILENO, "recovery: job %d %s: %s\n", job_kind, m.done > 0 ? "done" : "failed", m.text);
            if (job_kind != 2)
                load_images();
        }
    }
}

static bool start_job(int kind, const char *arg)
{
    thread_t t;

    if (busy)
        return false;
    busy = true;
    job_kind = kind;
    strlcpy(job_arg, arg ? arg : "", sizeof(job_arg));
    if (thread_create(&t, job_thread, NULL) != 0) {
        busy = false;
        return false;
    }
    return true;
}

static char images[32][300];
static int nimages;

static void load_images(void)
{
    struct widget *t = ui_get(win, "images");
    struct dir_stream *d = opendir(SYS "/var/backups");
    struct aegis_dirent *de;

    ui_list_clear(t);
    nimages = 0;
    while (d && (de = readdir(d)) && nimages < 32) {
        char path[300], row[400], when[40], size[32];
        int64_t created;
        uint64_t bytes;
        struct tm tm;

        if (!strstr(de->name, ".aimg"))
            continue;
        snprintf(path, sizeof(path), SYS "/var/backups/%s", de->name);
        if (sysimage_info(path, &created, &bytes) < 0)
            continue;
        localtime_r(&created, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        ui_format_size(bytes, size, sizeof(size));
        snprintf(row, sizeof(row), "%s\t%s\t%s", de->name, when, size);
        ui_list_add(t, row);
        strlcpy(images[nimages++], path, sizeof(images[0]));
    }
    if (d)
        closedir(d);
}

static void on_mkimage(struct widget *w, void *u)
{
    char path[300], name[80];
    struct tm tm;
    int64_t now = time(NULL);

    (void)w;
    (void)u;
    if (!have_system)
        return;
    mkdir(SYS "/var/backups", 0700);
    localtime_r(&now, &tm);
    strftime(name, sizeof(name), "system-%Y-%m-%d-%H%M.aimg", &tm);
    snprintf(path, sizeof(path), SYS "/var/backups/%s", name);
    if (start_job(0, path))
        ui_set_text(ui_get(win, "imgmsg"), "Saving the system...");
}

static void on_restore(struct widget *w, void *u)
{
    int i = ui_list_selected(ui_get(win, "images"));
    char msg[500];

    (void)w;
    (void)u;
    if (i < 0 || i >= nimages || busy)
        return;
    snprintf(msg, sizeof(msg), "Replace everything on the installed system with %s? Apps, settings, accounts and "
             "files made since then are lost.", strrchr(images[i], '/') + 1);
    if (ui_message(win, "Restore a system image", msg, "Restore|Cancel") != 0)
        return;
    start_job(1, images[i]);
}

static void on_reinstall(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!have_system || !have_esp || busy)
        return;
    if (ui_message(win, "Reinstall Aegis", "Reinstall the system files? Accounts, settings, apps and files stay.",
                   "Reinstall|Cancel") != 0)
        return;
    start_job(2, NULL);
}

static void unmount_all(void)
{
    sync();
    if (have_esp)
        umount(ESP);
    if (have_system)
        umount(SYS);
}

static void on_erase(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (ui_message(win, "Erase and install", "The installer erases the disk you choose. Continue?",
                   "Open the installer|Cancel") != 0)
        return;
    unmount_all();
    have_system = have_esp = false;
    launch("/sbin/installer", NULL);
}

static void on_terminal(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    launch("/bin/term", have_system ? SYS : "/");
}

static void on_restart(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (busy)
        return;
    unmount_all();
    reboot(REBOOT_RESTART);
}

static void on_poweroff(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (busy)
        return;
    unmount_all();
    reboot(REBOOT_POWEROFF);
}

static void on_firmware(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (busy)
        return;
    unmount_all();
    if (reboot(REBOOT_FIRMWARE) < 0)
        ui_message(win, "Firmware settings", "This computer's firmware cannot be asked to open its setup screen. "
                   "Restart and press its setup key (often F2, Del or Esc) instead.", "OK");
}

// Safe mode once: the boot menu's default becomes the safe entry, and the
// system puts it back when it starts (see init).
static void on_safe(struct widget *w, void *u)
{
    char *bcd, *updated, timeout[16], res[64], root[96], extra[300];

    (void)w;
    (void)u;
    if (!have_esp || !(bcd = slurp(BCD)) || !strstr(bcd, "[safe]")) {
        ui_message(win, "Safe mode", "This system's boot menu has no safe mode entry.", "OK");
        return;
    }
    bcd_global(bcd, "timeout", timeout, sizeof(timeout));
    bcd_global(bcd, "resolution", res, sizeof(res));
    bcd_extra(bcd, root, sizeof(root), extra, sizeof(extra));
    updated = bcd_update(bcd, timeout, res, "safe", root, extra);
    free(bcd);
    if (!updated || spill(BCD, updated, 0644) < 0) {
        free(updated);
        ui_message(win, "Safe mode", "The boot configuration could not be changed.", "OK");
        return;
    }
    free(updated);
    spill(SYS "/etc/safe-mode-once", "\n", 0644);
    on_restart(NULL, NULL);
}

static void on_tool(struct widget *w, void *u)
{
    int i = ui_list_selected(w);

    (void)u;
    if (i < 0)
        return;
    ui_set_value(ui_get(win, "pages"), i);
    if (i == 1)
        load_startup();
    else if (i == 2)
        on_load_bcd(NULL, NULL);
    else if (i == 3)
        load_users();
    else if (i == 4)
        load_images();
    ui_relayout(win);
}

static bool on_close(struct ui_window *w, void *u)
{
    (void)w;
    (void)u;
    return false;
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
        { "tool", on_tool }, { "restart", on_restart }, { "safe", on_safe }, { "poweroff", on_poweroff },
        { "firmware", on_firmware }, { "savestartup", on_save_startup }, { "loadbcd", on_load_bcd },
        { "savebcd", on_save_bcd }, { "resetpw", on_reset_pw }, { "mkimage", on_mkimage }, { "restore", on_restore },
        { "reinstall", on_reinstall }, { "erase", on_erase }, { "terminal", on_terminal }, { NULL, NULL },
    };
    struct widget *tools;
    char text[200];
    int sw, sh;

    if (pipe(pipe_fds) < 0 || !(win = ui_load_string_named(page, handlers, NULL, "recovery")))
        return 1;
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    wm_screen_size(&sw, &sh, NULL);
    ui_window_set_size(win, sw, sh);
    ui_window_move(win, 0, 0);
    ui_window_set_backdrop(win, backdrop, NULL);
    ui_on_close(win, on_close, NULL);
    tools = ui_get(win, "tools");
    for (int i = 0; i < 7; i++) {
        ui_list_add(tools, tool_names[i]);
        ui_list_set_icon_shared(tools, i, icon_get(tool_icons[i], 20));
    }
    find_system();
    if (have_system)
        snprintf(text, sizeof(text), "Installed system on %s%s%s", sys_dev, have_esp ? ", boot files on " : "",
                 have_esp ? esp_dev : " (no EFI partition found)");
    else
        snprintf(text, sizeof(text), "No installed Aegis system was found. The terminal and installer still work.");
    ui_set_text(ui_get(win, "system"), text);
    ui_list_select(tools, 0);
    ui_watch_fd(pipe_fds[0], job_progress, NULL);
    ui_window_show(win);
    dprintf(STDERR_FILENO, "recovery: ready (%s)\n", have_system ? sys_dev : "no system");
    return ui_run();
}
