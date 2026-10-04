#include "aegis.h"
#include "bearssl.h"

// System updates: .upd files.
//
//   aegis update 1             the first line
//   version: 0.3.1             header lines, then a blank line
//   build: 2026-10-04 84ceaa0
//   description: ...
//   files: 412
//   size: 61234567             bytes after the blank line
//   sha256: 9f86d0...          of those bytes
//
//   D 755 /sysapps             then the files: folders,
//   F 755 52344 /sysapps/files files (followed by their bytes),
//   F 644 1234 esp:/EFI/Aegis/kernel.elf    boot files,
//   E                          and the end
//
// Paths are absolute in the system, or "esp:" and a path on the EFI
// partition. tools/mkupd.py makes them; Recovery installs them (see
// install_update), keeping accounts, settings, apps and everyone's files.

#define VERSION_FILE    "/osystem/version.aset"
#define PENDING_FILE    UPDATES_DIR "/pending.upd"
#define PENDING_INFO    UPDATES_DIR "/pending.aset"
#define HISTORY_FILE    UPDATES_DIR "/history"
#define SYSTEM_BCD      "/osystem/boot/EFI/Aegis/bcd"
#define MAX_PAYLOAD     ((uint64_t)2 << 30)

// ---- Reading .upd files ----

struct reader {
    int fd;
    char buf[64 * 1024];
    size_t pos, len;
    br_sha256_context sha;
    bool hashing;
    uint64_t consumed;              // bytes of the payload read so far
};

static bool fill(struct reader *r)
{
    ssize_t n;

    if (r->pos < r->len)
        return true;
    if ((n = read(r->fd, r->buf, sizeof(r->buf))) <= 0)
        return false;
    r->pos = 0;
    r->len = n;
    return true;
}

// Takes up to want bytes; returns how many (0 at the end).
static size_t take(struct reader *r, char *out, size_t want)
{
    size_t n;

    if (!fill(r))
        return 0;
    n = MIN(want, r->len - r->pos);
    if (out)
        memcpy(out, r->buf + r->pos, n);
    if (r->hashing)
        br_sha256_update(&r->sha, r->buf + r->pos, n);
    r->pos += n;
    r->consumed += n;
    return n;
}

static int take_line(struct reader *r, char *out, size_t size)
{
    size_t n = 0;
    char c;

    while (take(r, &c, 1) == 1) {
        if (c == '\n') {
            out[n] = 0;
            return n;
        }
        if (n + 1 < size)
            out[n++] = c;
    }
    out[n] = 0;
    return n ? (int)n : -1;
}

static int failure(char *error, size_t size, const char *text)
{
    if (error && size)
        strlcpy(error, text, size);
    return -1;
}

// Reads the header; the reader is left at the start of the payload.
static int read_header(struct reader *r, struct update_info *info, char *sha_hex, char *error, size_t esize)
{
    char line[400];

    memset(info, 0, sizeof(*info));
    sha_hex[0] = 0;
    if (take_line(r, line, sizeof(line)) < 0 || strcmp(line, "aegis update 1"))
        return failure(error, esize, "This is not an Aegis update file.");
    while (take_line(r, line, sizeof(line)) > 0) {
        char *v = strstr(line, ": ");

        if (!v)
            continue;
        *v = 0;
        v += 2;
        if (!strcmp(line, "version"))
            strlcpy(info->version, v, sizeof(info->version));
        else if (!strcmp(line, "build"))
            strlcpy(info->build, v, sizeof(info->build));
        else if (!strcmp(line, "description"))
            strlcpy(info->description, v, sizeof(info->description));
        else if (!strcmp(line, "files"))
            info->files = strtoul(v, NULL, 10);
        else if (!strcmp(line, "size"))
            info->size = strtoul(v, NULL, 10);
        else if (!strcmp(line, "sha256"))
            strlcpy(sha_hex, v, 65);
    }
    if (!*info->version || !info->size || info->size > MAX_PAYLOAD || strlen(sha_hex) != 64)
        return failure(error, esize, "The update file's header is incomplete.");
    return 0;
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = 0;
}

int update_check(const char *path, struct update_info *info, char *error, size_t size)
{
    struct reader *r = calloc(1, sizeof(*r));
    char want[65], got[65];
    uint8_t digest[32];
    struct update_info tmp;
    int ret = -1;

    if (!info)
        info = &tmp;
    if (!r)
        return failure(error, size, "Out of memory.");
    if ((r->fd = open(path, O_RDONLY)) < 0) {
        snprintf(error, size, "The file cannot be opened: %s.", strerror(errno));
        free(r);
        return -1;
    }
    if (read_header(r, info, want, error, size) < 0)
        goto out;
    br_sha256_init(&r->sha);
    r->hashing = true;
    r->consumed = 0;
    while (take(r, NULL, sizeof(r->buf)))
        ;
    br_sha256_out(&r->sha, digest);
    hex(digest, 32, got);
    if (r->consumed != info->size) {
        failure(error, size, "The update file is incomplete (it may not have finished downloading or copying).");
        goto out;
    }
    if (strcmp(got, want)) {
        failure(error, size, "The update file is damaged: its contents do not match its checksum.");
        goto out;
    }
    ret = 0;
out:
    close(r->fd);
    free(r);
    return ret;
}

// ---- Versions ----

int system_version(const char *root, char *out, size_t size)
{
    char path[300];

    snprintf(path, sizeof(path), "%s" VERSION_FILE, root ? root : "");
    if (aset_get(path, "version", out, size) > 0)
        return 0;
    strlcpy(out, "unknown", size);
    return -1;
}

int version_compare(const char *a, const char *b)
{
    while (*a || *b) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);

        if (x != y)
            return x < y ? -1 : 1;
        // Something other than numbers: compare the rest as text.
        if (ea == a && eb == b)
            return strcmp(a, b);
        a = *ea == '.' ? ea + 1 : ea;
        b = *eb == '.' ? eb + 1 : eb;
    }
    return 0;
}

// ---- The boot menu's default entry ----

// Sets default= in the BCD at path (FAT: written beside it, then swapped);
// the old value goes in old.
static int bcd_set_default(const char *path, const char *def, char *old, size_t old_size)
{
    char buf[8192], tmp[300];
    int fd = open(path, O_RDONLY), out;
    ssize_t n;

    if (old && old_size)
        *old = 0;
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
        return -1;
    for (char *line = buf; *line;) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);

        if (!strncmp(line, "default=", 8)) {
            if (old && old_size)
                snprintf(old, old_size, "%.*s", (int)(len - 8), line + 8);
            dprintf(out, "default=%s\n", def);
        } else {
            dprintf(out, "%.*s\n", (int)len, line);
        }
        line += len + (nl ? 1 : 0);
    }
    close(out);
    unlink(path);
    if (rename(tmp, path) < 0)
        return -1;
    sync();
    return 0;
}

static bool has_recovery(void)
{
    struct aegis_stat st;
    int fd = open(SYSTEM_BCD, O_RDONLY);
    char buf[8192];
    ssize_t n;

    if (fd < 0 || stat("/osystem/boot/EFI/Aegis/recovery.img", &st) < 0) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return strstr(buf, "[recovery]") != NULL;
}

// ---- Scheduling, from the running system ----

static int schedule(const char *what, const char *version, char *error, size_t size)
{
    char old[64] = "", prev[64] = "";
    struct records r;
    int ret;

    // Waiting already: keep the boot menu default it saved.
    if (aset_get(PENDING_INFO, "bootdefault", prev, sizeof(prev)) > 0)
        strlcpy(old, prev, sizeof(old));
    else if (bcd_set_default(SYSTEM_BCD, "recovery", old, sizeof(old)) < 0)
        return failure(error, size, "The boot menu could not be changed to start Recovery.");
    if (*prev && bcd_set_default(SYSTEM_BCD, "recovery", NULL, 0) < 0)
        return failure(error, size, "The boot menu could not be changed to start Recovery.");
    records_init(&r, "settings");
    rec_set(records_top(&r), "what", what);
    if (version)
        rec_set(records_top(&r), "version", version);
    rec_set(records_top(&r), "bootdefault", *old && strcmp(old, "recovery") ? old : "aegis");
    ret = records_save(&r, PENDING_INFO, 0644);
    records_free(&r);
    sync();
    if (ret < 0)
        return failure(error, size, "The update could not be saved for Recovery.");
    return 0;
}

int update_schedule(const char *path, char *error, size_t size)
{
    struct update_info info;
    struct aegis_stat st;

    if (geteuid() != 0) {
        errno = EPERM;
        return failure(error, size, "Installing updates needs an administrator.");
    }
    if (update_check(path, &info, error, size) < 0)
        return -1;
    if (!has_recovery())
        return failure(error, size, "This computer has no Aegis Recovery to install the update with. "
                                    "Reinstall it from the install media first.");
    // The update file itself is the superuser's; what is waiting and the
    // history anyone may read.
    mkdir(UPDATES_DIR, 0755);
    chmod(UPDATES_DIR, 0755);
    // Recovery unpacks it beside the system: room for it twice.
    {
        struct aegis_statfs fs;

        if (statfs(UPDATES_DIR, &fs) == 0 && fs.blocks_free * fs.block_size < info.size * 2 + (64 << 20))
            return failure(error, size, "There is not enough free disk space for this update.");
    }
    if (strcmp(path, PENDING_FILE)) {
        unlink(PENDING_FILE);
        if (copy_path(path, PENDING_FILE) < 0 || stat(PENDING_FILE, &st) < 0)
            return failure(error, size, "The update file could not be copied.");
        chmod(PENDING_FILE, 0600);
    }
    if (schedule("update", info.version, error, size) < 0) {
        unlink(PENDING_FILE);
        return -1;
    }
    syslog("update", "update to %s waits for Recovery", info.version);
    return 0;
}

int update_schedule_undo(char *error, size_t size)
{
    char ver[32];

    if (geteuid() != 0) {
        errno = EPERM;
        return failure(error, size, "Undoing an update needs an administrator.");
    }
    if (!update_can_undo("", ver, sizeof(ver)))
        return failure(error, size, "There is no update to undo.");
    if (!has_recovery())
        return failure(error, size, "This computer has no Aegis Recovery.");
    unlink(PENDING_FILE);
    if (schedule("undo", ver, error, size) < 0)
        return -1;
    syslog("update", "undoing the last update waits for Recovery");
    return 0;
}

int update_cancel(void)
{
    char def[64];

    if (geteuid() != 0) {
        errno = EPERM;
        return -1;
    }
    if (aset_get(PENDING_INFO, "bootdefault", def, sizeof(def)) > 0)
        bcd_set_default(SYSTEM_BCD, def, NULL, 0);
    unlink(PENDING_FILE);
    unlink(PENDING_INFO);
    sync();
    return 0;
}

int update_pending(const char *root, struct update_info *info)
{
    char path[300], what[16];

    snprintf(path, sizeof(path), "%s" PENDING_INFO, root ? root : "");
    if (aset_get(path, "what", what, sizeof(what)) <= 0)
        return 0;
    if (info) {
        memset(info, 0, sizeof(*info));
        aset_get(path, "version", info->version, sizeof(info->version));
    }
    return !strcmp(what, "undo") ? 2 : 1;
}

bool update_can_undo(const char *root, char *previous_version, size_t size)
{
    char path[300];
    struct aegis_stat st;

    snprintf(path, sizeof(path), "%s" UPDATES_DIR "/previous/root", root ? root : "");
    if (stat(path, &st) < 0)
        return false;
    snprintf(path, sizeof(path), "%s" UPDATES_DIR "/previous/info.aset", root ? root : "");
    if (aset_get(path, "from", previous_version, size) <= 0)
        strlcpy(previous_version, "the previous version", size);
    return true;
}

// ---- Installing, from Recovery ----

static void history(const char *root, const char *fmt, const char *a, const char *b)
{
    char path[300], when[32];
    int64_t now = time(NULL);
    struct tm tm;
    int fd;

    snprintf(path, sizeof(path), "%s" HISTORY_FILE, root);
    if ((fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644)) < 0)
        return;
    localtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    dprintf(fd, "%s  ", when);
    dprintf(fd, fmt, a, b);
    dprintf(fd, "\n");
    close(fd);
}

// No "..", nothing but plain absolute paths.
static bool safe_path(const char *p)
{
    if (*p != '/')
        return false;
    for (const char *s = p; *s; s++)
        if (s[0] == '/' && s[1] == '.' && s[2] == '.' && (s[3] == '/' || !s[3]))
            return false;
    return true;
}

// Unpacks the files into dest/root and dest/esp.
static int unpack(const char *path, const char *dest, install_progress_fn progress, void *u, char *error,
                  size_t esize)
{
    struct reader *r = calloc(1, sizeof(*r));
    struct update_info info;
    char sha[65], line[1200];
    uint32_t done = 0;
    int ret = -1;

    if (!r)
        return failure(error, esize, "Out of memory.");
    if ((r->fd = open(path, O_RDONLY)) < 0 || read_header(r, &info, sha, error, esize) < 0)
        goto out;
    for (;;) {
        char kind, target[1100], *rest, *p;
        uint32_t mode;
        uint64_t size = 0;
        bool esp;

        if (take_line(r, line, sizeof(line)) < 0) {
            failure(error, esize, "The update file ends too early.");
            goto out;
        }
        kind = line[0];
        if (kind == 'E')
            break;
        if ((kind != 'D' && kind != 'F') || line[1] != ' ') {
            failure(error, esize, "The update file is not laid out as expected.");
            goto out;
        }
        mode = strtoul(line + 2, &rest, 8) & 07777;
        if (kind == 'F')
            size = strtoul(rest, &rest, 10);
        if (*rest != ' ') {
            failure(error, esize, "The update file is not laid out as expected.");
            goto out;
        }
        p = rest + 1;
        esp = !strncmp(p, "esp:", 4);
        if (esp)
            p += 4;
        if (!safe_path(p)) {
            failure(error, esize, "The update file names a file outside the system.");
            goto out;
        }
        snprintf(target, sizeof(target), "%s/%s%s", dest, esp ? "esp" : "root", p);
        if (kind == 'D') {
            if (mkdir(target, mode) < 0 && errno != EEXIST) {
                snprintf(error, esize, "%s: %s", p, strerror(errno));
                goto out;
            }
            chown(target, 0, 0);
            chmod(target, mode);
            continue;
        }
        {
            int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, mode);
            char buf[16384];

            if (fd < 0) {
                snprintf(error, esize, "%s: %s", p, strerror(errno));
                goto out;
            }
            while (size) {
                size_t n = take(r, buf, MIN(size, sizeof(buf)));

                if (!n || write(fd, buf, n) != (ssize_t)n) {
                    close(fd);
                    snprintf(error, esize, "%s: %s", p, n ? strerror(errno) : "the update file ends too early");
                    goto out;
                }
                size -= n;
            }
            close(fd);
            chown(target, 0, 0);
            chmod(target, mode);
        }
        if (progress && info.files && ++done % 16 == 0)
            progress(8 + (int)((uint64_t)done * 30 / info.files), "Unpacking the update", u);
    }
    ret = 0;
out:
    if (r->fd >= 0)
        close(r->fd);
    free(r);
    return ret;
}

int update_run_pending(const char *root, const char *esp, install_progress_fn progress, void *u, char *error,
                       size_t esize)
{
    char info_path[300], upd[300], fresh[300], def[64] = "aegis", bcd[300], what[16] = "", from[32], to[32];
    struct update_info info;
    int ret = -1;

    snprintf(info_path, sizeof(info_path), "%s" PENDING_INFO, root);
    snprintf(upd, sizeof(upd), "%s" PENDING_FILE, root);
    snprintf(fresh, sizeof(fresh), "%s" UPDATES_DIR "/new", root);
    aset_get(info_path, "what", what, sizeof(what));
    aset_get(info_path, "bootdefault", def, sizeof(def));
    // First of all the boot menu goes back, so a failure cannot loop.
    if (esp) {
        snprintf(bcd, sizeof(bcd), "%s/EFI/Aegis/bcd", esp);
        bcd_set_default(bcd, def, NULL, 0);
    }
    system_version(root, from, sizeof(from));
    if (!strcmp(what, "undo")) {
        if (progress)
            progress(5, "Undoing the last update", u);
        ret = install_undo_update(root, esp, progress, u, error, esize);
        system_version(root, to, sizeof(to));
        if (ret == 0)
            history(root, "undid the update: %s back to %s", from, to);
        else
            history(root, "undoing the update failed: %s%s", error, "");
        unlink(info_path);
        sync();
        return ret;
    }
    if (strcmp(what, "update")) {
        unlink(info_path);
        return failure(error, esize, "Nothing is waiting to be installed.");
    }
    if (progress)
        progress(2, "Checking the update", u);
    if (update_check(upd, &info, error, esize) < 0) {
        history(root, "the update was not installed: %s%s", error, "");
        goto out;
    }
    remove_path(fresh);
    {
        char sub[320];

        mkdir(fresh, 0700);
        snprintf(sub, sizeof(sub), "%s/root", fresh);
        mkdir(sub, 0755);
        snprintf(sub, sizeof(sub), "%s/esp", fresh);
        mkdir(sub, 0755);
    }
    if (progress)
        progress(8, "Unpacking the update", u);
    if (unpack(upd, fresh, progress, u, error, esize) < 0) {
        history(root, "the update to %s was not installed: %s", info.version, error);
        goto out;
    }
    if (install_update(root, esp, progress, u, error, esize) < 0) {
        history(root, "the update to %s failed and was rolled back: %s", info.version, error);
        goto out;
    }
    {
        char path[300];
        struct records r;

        snprintf(path, sizeof(path), "%s" UPDATES_DIR "/previous/info.aset", root);
        records_init(&r, "settings");
        rec_set(records_top(&r), "from", from);
        rec_set(records_top(&r), "to", info.version);
        records_save(&r, path, 0644);
        records_free(&r);
    }
    {
        char dir[300];

        snprintf(dir, sizeof(dir), "%s" UPDATES_DIR, root);
        chmod(dir, 0755);
    }
    history(root, "updated %s to %s", from, info.version);
    ret = 0;
out:
    remove_path(fresh);
    unlink(upd);
    unlink(info_path);
    sync();
    return ret;
}
