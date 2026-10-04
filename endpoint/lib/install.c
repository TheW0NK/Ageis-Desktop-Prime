#include "aegis.h"

// Installing Aegis on a disk, erasing it: a GPT with an EFI system partition
// and the system partition, the running system copied over (without the
// live system's own pieces or anyone's files), the boot files and a boot
// configuration, and the account to create on first boot.

#define ESP_MB      256
#define TARGET      "/osystem/volumes/target"
#define TARGET_ESP  "/osystem/volumes/esp"

struct ctx {
    bool keep;                      // reinstalling: keep accounts, settings and files
    const struct install_options *o;
    install_progress_fn progress;
    void *u;
    char error[256];
    uint64_t files, copied;
};

static void step(struct ctx *c, int percent, const char *what)
{
    if (c->progress)
        c->progress(percent, what, c->u);
}

static int fail(struct ctx *c, const char *what)
{
    snprintf(c->error, sizeof(c->error), "%s: %s", what, strerror(errno));
    return -1;
}

// Paths under / that are not copied: devices, scratch space, mount points,
// the boot partition, the live system's marker and installer payload, and
// everyone's files and accounts (the new system starts with its own).
static bool keeping;                // see struct ctx.keep

static bool skipped(const char *rel)
{
    static const char *const skip[] = { "/osystem/devices", "/osystem/temp", "/osystem/volumes", "/osystem/boot", "/userfiles", "/msc/live", "/msc/firstboot.aset",
                                        "/osystem/installer", "/osystem/logs/shift.log", UPDATES_DIR, "/lost+found", NULL };
    // A reinstall leaves the computer's own settings and data alone.
    static const char *const keep[] = { ACCOUNTS_FILE, SECRETS_FILE, COMPUTER_FILE, "/msc/features.aset",
                                        "/msc/routines", "/msc/hosts", "/serve", "/osystem/logs",
                                        "/osystem/data", "/osystem/backups", "/userApps",
                                        "/userfiles/superuser", NULL };

    for (int i = 0; skip[i]; i++)
        if (!strcmp(rel, skip[i]))
            return true;
    for (int i = 0; keeping && keep[i]; i++)
        if (!strcmp(rel, keep[i]))
            return true;
    return false;
}

static uint64_t count_tree(const char *path, const char *rel)
{
    struct dir_stream *d;
    struct aegis_dirent *de;
    uint64_t n = 1;

    if (!(d = opendir(path)))
        return 1;
    while ((de = readdir(d))) {
        char p[1024], r[1024];
        struct aegis_stat st;

        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", strcmp(path, "/") ? path : "", de->name);
        snprintf(r, sizeof(r), "%s/%s", rel, de->name);
        if (skipped(r))
            continue;
        if (lstat(p, &st) == 0 && S_ISDIR(st.mode))
            n += count_tree(p, r);
        else
            n++;
    }
    closedir(d);
    return n;
}

static int copy_file(const char *src, const char *dst, uint32_t mode)
{
    static char buf[64 * 1024];
    int in = open(src, O_RDONLY), out;
    ssize_t n;

    if (in < 0)
        return -1;
    if ((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777)) < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            n = -1;
            break;
        }
    }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}

// Copies the tree at src (rel is its path from /) to dst, keeping owners,
// modes, times and links.
static int copy_tree(struct ctx *c, const char *src, const char *dst, const char *rel)
{
    struct dir_stream *d;
    struct aegis_dirent *de;

    if (!(d = opendir(src)))
        return fail(c, src);
    while ((de = readdir(d))) {
        char s[1024], t[1024], r[1024];
        struct aegis_stat st;

        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        snprintf(s, sizeof(s), "%s/%s", strcmp(src, "/") ? src : "", de->name);
        snprintf(t, sizeof(t), "%s/%s", dst, de->name);
        snprintf(r, sizeof(r), "%s/%s", rel, de->name);
        if (lstat(s, &st) < 0) {
            closedir(d);
            return fail(c, s);
        }
        if (skipped(r)) {
            // Mount points and scratch space exist, empty.
            if (!keeping && S_ISDIR(st.mode) && strcmp(r, "/userfiles") && mkdir(t, st.mode & 07777) == 0) {
                chown(t, st.uid, st.gid);
                chmod(t, st.mode & 07777);
            }
            continue;
        }
        if (S_ISDIR(st.mode)) {
            if (mkdir(t, 0700) < 0 && errno != EEXIST) {
                closedir(d);
                return fail(c, t);
            }
            if (copy_tree(c, s, t, r) < 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISLNK(st.mode)) {
            char target[512];
            ssize_t n = readlink(s, target, sizeof(target) - 1);

            if (n < 0 || (target[n] = 0, symlink(target, t) < 0)) {
                closedir(d);
                return fail(c, s);
            }
        } else if (S_ISREG(st.mode)) {
            if (copy_file(s, t, st.mode) < 0) {
                closedir(d);
                return fail(c, s);
            }
        } else {
            continue;               // devices and sockets are made at run time
        }
        if (!S_ISLNK(st.mode)) {
            chown(t, st.uid, st.gid);
            chmod(t, st.mode & 07777);
            utime(t, st.atime, st.mtime);
        }
        c->copied++;
        if (c->files && c->copied % 16 == 0)
            step(c, 30 + (int)(c->copied * 55 / c->files), "Copying system files");
    }
    closedir(d);
    return 0;
}

static int write_text(const char *path, const char *text, uint32_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    size_t n = strlen(text);

    if (fd < 0)
        return -1;
    if (write(fd, text, n) != (ssize_t)n) {
        close(fd);
        return -1;
    }
    close(fd);
    return chmod(path, mode);
}

static void format_progress(int percent, void *u)
{
    step(u, 8 + percent * 17 / 100, "Formatting the system partition");
}

// The accounts the new system starts with: the superuser and the system groups. The
// person's own account is made on first boot from /msc/firstboot.aset.
static int write_accounts(struct ctx *c)
{
    const struct install_options *o = c->o;
    char text[64], hash[160];
    struct records fb;
    int ret;

    if (write_text(TARGET ACCOUNTS_FILE, ACCOUNTS_START, 0644) < 0
        || write_text(TARGET SECRETS_FILE, "aegis secrets 1\nsuperuser: !\n", 0600) < 0)
        return fail(c, "Writing the account files");
    if (password_hash(o->password, hash, sizeof(hash)) < 0)
        return fail(c, "Protecting the password");
    // Read once by init, which makes the account.
    records_init(&fb, "settings");
    rec_set(records_top(&fb), "account", o->user);
    rec_set(records_top(&fb), "display", o->display && *o->display ? o->display : o->user);
    rec_set(records_top(&fb), "password", hash);
    rec_set(records_top(&fb), "administrator", o->admin ? "yes" : "no");
    rec_set(records_top(&fb), "language", o->language && *o->language ? o->language : "en");
    ret = records_save(&fb, TARGET "/msc/firstboot.aset", 0600);
    records_free(&fb);
    memset(hash, 0, sizeof(hash));
    if (ret < 0)
        return fail(c, "Writing the first-boot settings");
    if (o->hostname && *o->hostname)
        aset_set(TARGET COMPUTER_FILE, "name", o->hostname, 0644);
    if (o->timezone && *o->timezone) {
        snprintf(text, sizeof(text), "%d", o->tz_offset_min);
        aset_set(TARGET COMPUTER_FILE, "timezone", o->timezone, 0644);
        aset_set(TARGET COMPUTER_FILE, "timezone-offset", text, 0644);
    }
    return 0;
}

static int write_bcd(struct ctx *c, const char *root_guid)
{
    char bcd[2048];
    struct aegis_stat st;
    int n;

    n = snprintf(bcd, sizeof(bcd),
                 "# Aegis Boot Configuration Data, written by the installer.\n"
                 "timeout=3\ndefault=aegis\nloader=\\EFI\\Aegis\\btloader.efi\ndiagnostics=false\nresolution=auto\n\n"
                 "[aegis]\ntitle=Aegis\ntype=kernel\npath=\\EFI\\Aegis\\kernel.elf\ncmdline=root=PARTUUID=%s\n\n"
                 "[verbose]\ntitle=Aegis (verbose boot)\ntype=kernel\npath=\\EFI\\Aegis\\kernel.elf\n"
                 "cmdline=root=PARTUUID=%s verbose\n",
                 root_guid, root_guid);
    n += snprintf(bcd + n, sizeof(bcd) - n,
                  "\n[safe]\ntitle=Aegis (safe mode)\ntype=kernel\npath=\\EFI\\Aegis\\kernel.elf\n"
                  "cmdline=root=PARTUUID=%s safe\n", root_guid);
    if (stat(TARGET_ESP "/EFI/Aegis/recovery.img", &st) == 0)
        snprintf(bcd + n, sizeof(bcd) - n,
                 "\n[recovery]\ntitle=Aegis Recovery\ntype=kernel\npath=\\EFI\\Aegis\\kernel.elf\n"
                 "ramdisk=\\EFI\\Aegis\\recovery.img\ncmdline=root=ramdisk recovery\n");
    return write_text(TARGET_ESP "/EFI/Aegis/bcd", bcd, 0644) < 0 ? fail(c, "Writing the boot configuration") : 0;
}

// The recovery system is the live system: its image is the ramdisk this
// runs from (when it does), copied to the EFI partition.
static int copy_recovery_image(const char *esp_root)
{
    static char buf[64 * 1024];
    char dst[200];
    int in = open("/osystem/devices/ramdisk", O_RDONLY), out;
    ssize_t n;

    if (in < 0)
        return 0;                   // not running from install media
    sync();
    snprintf(dst, sizeof(dst), "%s/EFI/Aegis/recovery.img", esp_root);
    if ((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, n) != n) {
            n = -1;
            break;
        }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}

// The boot files: the installer's copy on the live system, else the
// running system's own EFI partition.
static const char *esp_source(void)
{
    struct aegis_stat st;

    return stat("/osystem/installer/esp/EFI", &st) == 0 ? "/osystem/installer/esp" : "/osystem/boot";
}

int install_system(const struct install_options *o, install_progress_fn progress, void *u, char *error,
                   size_t error_size)
{
    struct ctx c = { false, o, progress, u, "", 0, 0 };
    static const struct disk_part layout[2] = {
        { DISK_TYPE_ESP, "EFI system partition", ESP_MB },
        { DISK_TYPE_DATA, "Aegis", 0 },
    };
    char guids[2][37], esp[24], sys[24], src[64];
    bool root_mounted = false, esp_mounted = false;
    int ret = -1;

    if (geteuid() != 0) {
        errno = EPERM;
        fail(&c, "Installing");
        goto out;
    }
    snprintf(esp, sizeof(esp), "%s1", o->disk);
    snprintf(sys, sizeof(sys), "%s2", o->disk);

    step(&c, 2, "Creating partitions");
    if (disk_write_gpt(o->disk, layout, 2, guids) < 0) {
        fail(&c, "Creating partitions");
        goto out;
    }
    step(&c, 6, "Formatting the EFI system partition");
    if (mkfs_fat32(esp, "AEGIS ESP") < 0) {
        fail(&c, "Formatting the EFI system partition");
        goto out;
    }
    step(&c, 8, "Formatting the system partition");
    if (mkfs_ext4(sys, "Aegis", format_progress, &c) < 0) {
        fail(&c, "Formatting the system partition");
        goto out;
    }

    step(&c, 26, "Preparing the new system");
    mkdir("/osystem/volumes", 0755);
    mkdir(TARGET, 0755);
    mkdir(TARGET_ESP, 0755);
    if (mount("ext4", sys, TARGET, 0) < 0) {
        fail(&c, "Opening the system partition");
        goto out;
    }
    root_mounted = true;
    if (mount("fat", esp, TARGET_ESP, 0) < 0) {
        fail(&c, "Opening the EFI system partition");
        goto out;
    }
    esp_mounted = true;

    c.files = count_tree("/", "");
    step(&c, 30, "Copying system files");
    if (copy_tree(&c, "/", TARGET, "") < 0)
        goto out;
    chmod(TARGET, 0755);
    mkdir(TARGET "/userfiles", 0755);
    mkdir(TARGET "/userfiles/superuser", 0700);
    mkdir(TARGET "/serve", 0755);
    chmod(TARGET "/osystem/temp", 01777);

    step(&c, 86, "Setting up accounts");
    if (write_accounts(&c) < 0)
        goto out;

    step(&c, 90, "Installing the boot files");
    strlcpy(src, esp_source(), sizeof(src));
    {
        char from[96];

        snprintf(from, sizeof(from), "%s/EFI", src);
        if (copy_path(from, TARGET_ESP "/EFI") < 0) {
            fail(&c, "Copying the boot files");
            goto out;
        }
    }
    step(&c, 93, "Installing the recovery system");
    if (copy_recovery_image(TARGET_ESP) < 0) {
        fail(&c, "Copying the recovery system");
        goto out;
    }
    if (write_bcd(&c, guids[1]) < 0)
        goto out;

    step(&c, 96, "Finishing");
    sync();
    ret = 0;
out:
    if (esp_mounted && umount(TARGET_ESP) < 0 && !ret)
        ret = fail(&c, "Closing the EFI system partition");
    if (root_mounted && umount(TARGET) < 0 && !ret)
        ret = fail(&c, "Closing the system partition");
    if (!ret)
        step(&c, 100, "Done");
    if (error)
        strlcpy(error, c.error, error_size);
    return ret;
}

// Reinstalls the system files on an installed system mounted at root (its
// EFI partition at esp), keeping accounts, settings, apps and everyone's
// files: the recovery system's own files are copied over.
int install_refresh(const char *root, const char *esp, install_progress_fn progress, void *u, char *error,
                    size_t error_size)
{
    struct ctx c = { true, NULL, progress, u, "", 0, 0 };
    char from[96], to[300];
    int ret = -1;

    keeping = true;
    c.files = count_tree("/", "");
    step(&c, 30, "Copying system files");
    if (copy_tree(&c, "/", root, "") < 0)
        goto out;
    step(&c, 90, "Installing the boot files");
    (void)from;
    (void)to;
    // The boot manager, loader and kernel, file by file (the BCD stays).
    {
        static const char *const files[] = { "BOOT/BOOTX64.EFI", "Aegis/btloader.efi", "Aegis/kernel.elf", NULL };

        for (int i = 0; files[i]; i++) {
            char a[160], b[300];

            snprintf(a, sizeof(a), "%s/EFI/%s", esp_source(), files[i]);
            snprintf(b, sizeof(b), "%s/EFI/%s", esp, files[i]);
            unlink(b);
            if (copy_file(a, b, 0644) < 0) {
                fail(&c, "Copying the boot files");
                goto out;
            }
        }
    }
    sync();
    step(&c, 100, "Done");
    ret = 0;
out:
    keeping = false;
    if (error)
        strlcpy(error, c.error, error_size);
    return ret;
}

// ---- Updates (.upd): replacing the system files, and undoing it ----
//
// The system files are everything a reinstall would replace. An update
// moves them aside (into UPDATES_DIR/previous, by renaming, so it costs no
// space and is quick), moves the new ones (unpacked beforehand into
// UPDATES_DIR/new) into place, and replaces the boot files. What it moved
// aside is how it is undone, and how a failed update is rolled back.

// True if some kept or skipped path is inside rel: rel itself cannot be
// moved whole, only what is in it.
static bool holds_kept(const char *rel)
{
    static const char *const kept[] = { "/lost+found", "/osystem/devices", "/osystem/temp", "/osystem/volumes", "/osystem/boot",
                                        "/userfiles", "/msc/live", "/msc/firstboot.aset", "/osystem/installer",
                                        "/osystem/logs", UPDATES_DIR, ACCOUNTS_FILE, SECRETS_FILE, COMPUTER_FILE,
                                        "/msc/features.aset", "/msc/routines", "/msc/hosts", "/serve", "/osystem/data",
                                        "/osystem/backups", "/userApps", NULL };
    size_t n = strlen(rel);

    for (int i = 0; kept[i]; i++)
        if (!strncmp(kept[i], rel, n) && kept[i][n] == '/')
            return true;
    return false;
}

static void mkdir_all(char *path, uint32_t mode)
{
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(path, mode);
            *p = '/';
        }
    mkdir(path, mode);
}

static int make_dir_like(const char *path, const char *like)
{
    struct aegis_stat st;

    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return -1;
    if (stat(like, &st) == 0) {
        chown(path, st.uid, st.gid);
        chmod(path, st.mode & 07777);
    }
    return 0;
}

// Moves the system files of the tree at root (rel inside it) into aside,
// leaving everything kept where it is.
static int move_system(struct ctx *c, const char *root, const char *aside, const char *rel)
{
    char dir[1024];
    struct dir_stream *d;
    struct aegis_dirent *de;

    snprintf(dir, sizeof(dir), "%s%s", root, rel);
    if (!(d = opendir(dir)))
        return fail(c, dir);
    while ((de = readdir(d))) {
        char r[1024], from[1100], to[1100];
        struct aegis_stat st;

        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        snprintf(r, sizeof(r), "%s/%s", rel, de->name);
        if (skipped(r))
            continue;
        snprintf(from, sizeof(from), "%s%s", root, r);
        snprintf(to, sizeof(to), "%s%s", aside, r);
        if (lstat(from, &st) < 0)
            continue;
        if (S_ISDIR(st.mode) && holds_kept(r)) {
            if (make_dir_like(to, from) < 0 || move_system(c, root, aside, r) < 0) {
                closedir(d);
                return c->error[0] ? -1 : fail(c, to);
            }
            continue;
        }
        if (rename(from, to) < 0) {
            closedir(d);
            return fail(c, from);
        }
        c->copied++;
    }
    closedir(d);
    return 0;
}

// Moves the tree at from (rel inside it) into root where nothing is in the
// way: kept files already there win.
static int move_in(struct ctx *c, const char *from_root, const char *root, const char *rel)
{
    char dir[1024];
    struct dir_stream *d;
    struct aegis_dirent *de;

    snprintf(dir, sizeof(dir), "%s%s", from_root, rel);
    if (!(d = opendir(dir)))
        return fail(c, dir);
    while ((de = readdir(d))) {
        char r[1024], from[1100], to[1100];
        struct aegis_stat sf, st;

        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        snprintf(r, sizeof(r), "%s/%s", rel, de->name);
        snprintf(from, sizeof(from), "%s%s", from_root, r);
        snprintf(to, sizeof(to), "%s%s", root, r);
        if (lstat(from, &sf) < 0)
            continue;
        if (lstat(to, &st) < 0) {
            if (rename(from, to) < 0) {
                closedir(d);
                return fail(c, to);
            }
            c->copied++;
        } else if (S_ISDIR(sf.mode) && S_ISDIR(st.mode)) {
            if (move_in(c, from_root, root, r) < 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

static const char *const boot_files[] = { "EFI/BOOT/BOOTX64.EFI", "EFI/Aegis/btloader.efi", "EFI/Aegis/kernel.elf",
                                          "EFI/Aegis/recovery.img", NULL };

// Copies the boot files found under from_dir to the EFI partition, saving
// the ones they replace under save_dir (if given).
static int swap_boot_files(struct ctx *c, const char *from_dir, const char *esp, const char *save_dir)
{
    for (int i = 0; boot_files[i]; i++) {
        char a[600], b[600], s[600];
        struct aegis_stat st;

        snprintf(a, sizeof(a), "%s/%s", from_dir, boot_files[i]);
        snprintf(b, sizeof(b), "%s/%s", esp, boot_files[i]);
        if (stat(a, &st) < 0)
            continue;
        if (save_dir && stat(b, &st) == 0) {
            char parent[600];

            snprintf(s, sizeof(s), "%s/%s", save_dir, boot_files[i]);
            snprintf(parent, sizeof(parent), "%s", s);
            *strrchr(parent, '/') = 0;
            mkdir_all(parent, 0755);
            if (copy_file(b, s, 0644) < 0)
                return fail(c, "Saving the boot files");
        }
        // FAT cannot replace a file by renaming over it.
        unlink(b);
        if (copy_file(a, b, 0644) < 0)
            return fail(c, "Installing the boot files");
    }
    return 0;
}

int install_update(const char *root, const char *esp, install_progress_fn progress, void *u, char *error,
                   size_t error_size)
{
    struct ctx c = { true, NULL, progress, u, "", 0, 0 };
    char upd[300], fresh[300], prev[300], prev_root[300], prev_esp[300], failed[300];
    bool moved_aside = false;
    int ret = -1;

    keeping = true;
    snprintf(upd, sizeof(upd), "%s" UPDATES_DIR, root);
    snprintf(fresh, sizeof(fresh), "%s/new", upd);
    snprintf(prev, sizeof(prev), "%s/previous", upd);
    snprintf(prev_root, sizeof(prev_root), "%s/root", prev);
    snprintf(prev_esp, sizeof(prev_esp), "%s/esp", prev);
    snprintf(failed, sizeof(failed), "%s/failed", upd);

    // The update before this one can no longer be undone.
    step(&c, 40, "Making room");
    remove_path(prev);
    remove_path(failed);
    // Readable by everyone (the old system files are no secret), so anyone
    // can see that the update can be undone.
    if (mkdir(prev, 0755) < 0 || make_dir_like(prev_root, root) < 0) {
        fail(&c, "Preparing the update");
        goto out;
    }
    step(&c, 50, "Setting the old system aside");
    if (move_system(&c, root, prev_root, "") < 0)
        goto undo;
    moved_aside = true;
    step(&c, 65, "Putting the new system in place");
    {
        char new_root[320];

        snprintf(new_root, sizeof(new_root), "%s/root", fresh);
        if (move_in(&c, new_root, root, "") < 0)
            goto undo;
    }
    step(&c, 85, "Installing the boot files");
    if (esp) {
        char new_esp[320];

        snprintf(new_esp, sizeof(new_esp), "%s/esp", fresh);
        if (swap_boot_files(&c, new_esp, esp, prev_esp) < 0)
            goto undo;
    }
    step(&c, 95, "Finishing");
    remove_path(fresh);
    sync();
    step(&c, 100, "Done");
    ret = 0;
    goto out;
undo:
    // Put the old system back as it was.
    if (moved_aside) {
        struct ctx back = { true, NULL, NULL, NULL, "", 0, 0 };

        mkdir(failed, 0700);
        move_system(&back, root, failed, "");
        move_in(&back, prev_root, root, "");
        if (esp)
            swap_boot_files(&back, prev_esp, esp, NULL);
        remove_path(failed);
    } else {
        struct ctx back = { true, NULL, NULL, NULL, "", 0, 0 };

        move_in(&back, prev_root, root, "");
    }
    remove_path(prev);
    sync();
out:
    keeping = false;
    if (error)
        strlcpy(error, c.error, error_size);
    return ret;
}

int install_undo_update(const char *root, const char *esp, install_progress_fn progress, void *u, char *error,
                        size_t error_size)
{
    struct ctx c = { true, NULL, progress, u, "", 0, 0 };
    char prev[300], prev_root[320], prev_esp[320], undone[300];
    struct aegis_stat st;
    int ret = -1;

    keeping = true;
    snprintf(prev, sizeof(prev), "%s" UPDATES_DIR "/previous", root);
    snprintf(prev_root, sizeof(prev_root), "%s/root", prev);
    snprintf(prev_esp, sizeof(prev_esp), "%s/esp", prev);
    snprintf(undone, sizeof(undone), "%s" UPDATES_DIR "/undone", root);
    if (stat(prev_root, &st) < 0) {
        errno = ENOENT;
        fail(&c, "There is no update to undo");
        goto out;
    }
    remove_path(undone);
    if (mkdir(undone, 0700) < 0 || make_dir_like(undone, root) < 0) {
        fail(&c, "Preparing");
        goto out;
    }
    step(&c, 30, "Setting the updated system aside");
    if (move_system(&c, root, undone, "") < 0) {
        struct ctx back = { true, NULL, NULL, NULL, "", 0, 0 };

        move_in(&back, undone, root, "");
        goto out;
    }
    step(&c, 60, "Putting the previous system back");
    if (move_in(&c, prev_root, root, "") < 0)
        goto out;
    step(&c, 85, "Putting the previous boot files back");
    if (esp && swap_boot_files(&c, prev_esp, esp, NULL) < 0)
        goto out;
    step(&c, 95, "Finishing");
    remove_path(undone);
    remove_path(prev);
    sync();
    step(&c, 100, "Done");
    ret = 0;
out:
    keeping = false;
    if (error)
        strlcpy(error, c.error, error_size);
    return ret;
}
