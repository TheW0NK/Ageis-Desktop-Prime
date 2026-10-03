#include "aegis.h"

// Installing Aegis on a disk, erasing it: a GPT with an EFI system partition
// and the system partition, the running system copied over (without the
// live system's own pieces or anyone's files), the boot files and a boot
// configuration, and the account to create on first boot.

#define ESP_MB      256
#define TARGET      "/mnt/target"
#define TARGET_ESP  "/mnt/esp"

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
    static const char *const skip[] = { "/dev", "/tmp", "/mnt", "/boot", "/users", "/etc/live", "/etc/firstboot",
                                        "/usr/share/installer", "/var/log/session.log", NULL };
    // A reinstall leaves the computer's own settings and data alone.
    static const char *const keep[] = { "/etc/passwd", "/etc/group", "/etc/shadow", "/etc/hostname",
                                        "/etc/timezone", "/etc/features.conf", "/etc/crontab", "/var", "/apps",
                                        "/root", NULL };

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
            if (!keeping && S_ISDIR(st.mode) && strcmp(r, "/users") && mkdir(t, st.mode & 07777) == 0) {
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

// The accounts the new system starts with: root and the system groups. The
// person's own account is made on first boot from /etc/firstboot.
static int write_accounts(struct ctx *c)
{
    const struct install_options *o = c->o;
    char text[1024], hash[160];

    if (write_text(TARGET "/etc/passwd", "root:x:0:0:root:/root:/bin/terminal\n", 0644) < 0
        || write_text(TARGET "/etc/group", "root:x:0:root\nadm:x:4:\nsudo:x:27:\nvideo:x:44:\naudio:x:63:\ninput:x:50:\n",
                      0644) < 0
        || write_text(TARGET "/etc/shadow", "root:!:\n", 0600) < 0)
        return fail(c, "Writing the account files");
    if (password_hash(o->password, hash, sizeof(hash)) < 0)
        return fail(c, "Protecting the password");
    // name, display name, password hash, administrator: read once by init.
    snprintf(text, sizeof(text), "user=%s\ndisplay=%s\nhash=%s\nadmin=%d\nlanguage=%s\n", o->user,
             o->display && *o->display ? o->display : o->user, hash, o->admin ? 1 : 0,
             o->language && *o->language ? o->language : "en");
    if (write_text(TARGET "/etc/firstboot", text, 0600) < 0)
        return fail(c, "Writing the first-boot settings");
    memset(hash, 0, sizeof(hash));
    if (o->hostname && *o->hostname) {
        snprintf(text, sizeof(text), "%s\n", o->hostname);
        write_text(TARGET "/etc/hostname", text, 0644);
    }
    if (o->timezone && *o->timezone) {
        snprintf(text, sizeof(text), "%s %d\n", o->timezone, o->tz_offset_min);
        write_text(TARGET "/etc/timezone", text, 0644);
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
                 "ramdisk=\\EFI\\Aegis\\recovery.img\ncmdline=root=ram0 recovery\n");
    return write_text(TARGET_ESP "/EFI/Aegis/bcd", bcd, 0644) < 0 ? fail(c, "Writing the boot configuration") : 0;
}

// The recovery system is the live system: its image is the ramdisk this
// runs from (when it does), copied to the EFI partition.
static int copy_recovery_image(const char *esp_root)
{
    static char buf[64 * 1024];
    char dst[200];
    int in = open("/dev/ram0", O_RDONLY), out;
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

    return stat("/usr/share/installer/esp/EFI", &st) == 0 ? "/usr/share/installer/esp" : "/boot";
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
    snprintf(esp, sizeof(esp), "%sp1", o->disk);
    snprintf(sys, sizeof(sys), "%sp2", o->disk);

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
    mkdir("/mnt", 0755);
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
    mkdir(TARGET "/users", 0755);
    chmod(TARGET "/tmp", 01777);

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
