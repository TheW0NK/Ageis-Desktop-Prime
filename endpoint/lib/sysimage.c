#include "aegis.h"

// System images: a whole installed system (system files, settings,
// accounts and everyone's files) in one file, made and restored from
// recovery. Images are kept in /var/backups on the system itself, which a
// restore leaves alone, or anywhere else.
//
//   "AEGISIMG"  u32 version (1)  u32 0  i64 created
//   records:    u8 type (1 dir, 2 file, 3 link, 0 end)  u8 0  u16 path length
//               u32 mode  u32 uid  u32 gid  i64 mtime  u64 size  path  data
//
// Paths are relative to the system's root. A link's data is its target.

#define MAGIC       "AEGISIMG"
#define T_END       0
#define T_DIR       1
#define T_FILE      2
#define T_LINK      3

struct rec {
    uint8_t type, pad;
    uint16_t path_len;
    uint32_t mode, uid, gid;
    int64_t mtime;
    uint64_t size;
} __attribute__((packed));

struct job {
    int fd;
    sysimage_progress_fn progress;
    void *u;
    uint64_t done, total;
    char *error;
    size_t esize;
};

static char iobuf[64 * 1024];

// Not saved, and not removed by a restore.
static bool left_alone(const char *rel)
{
    static const char *const skip[] = { "dev", "tmp", "mnt", "boot", "lost+found", "var/backups", NULL };

    for (int i = 0; skip[i]; i++)
        if (!strcmp(rel, skip[i]))
            return true;
    return false;
}

static int job_fail(struct job *j, const char *what, const char *path)
{
    snprintf(j->error, j->esize, "%s %s: %s", what, path, strerror(errno));
    return -1;
}

static void tick(struct job *j, uint64_t bytes, const char *what)
{
    j->done += bytes;
    if (j->progress && j->total)
        j->progress((int)(j->done * 100 / j->total), what, j->u);
}

static uint64_t tree_bytes(const char *path, const char *rel)
{
    struct dir_stream *d = opendir(path);
    struct aegis_dirent *de;
    uint64_t n = 0;

    if (!d)
        return 0;
    while ((de = readdir(d))) {
        char p[1024], r[1024];
        struct aegis_stat st;

        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", path, de->name);
        snprintf(r, sizeof(r), "%s%s%s", rel, *rel ? "/" : "", de->name);
        if (left_alone(r) || lstat(p, &st) < 0)
            continue;
        n += S_ISDIR(st.mode) ? tree_bytes(p, r) + 1 : st.size + 1;
    }
    closedir(d);
    return n;
}

static int put_rec(struct job *j, uint8_t type, const char *rel, const struct aegis_stat *st, uint64_t size)
{
    struct rec r = { type, 0, (uint16_t)strlen(rel), st ? st->mode : 0, st ? st->uid : 0, st ? st->gid : 0,
                     st ? st->mtime : 0, size };

    return write(j->fd, &r, sizeof(r)) == sizeof(r) && write(j->fd, rel, r.path_len) == r.path_len ? 0 : -1;
}

static int save_tree(struct job *j, const char *path, const char *rel)
{
    struct dir_stream *d = opendir(path);
    struct aegis_dirent *de;

    if (!d)
        return job_fail(j, "Reading", path);
    while ((de = readdir(d))) {
        char p[1024], r[1024];
        struct aegis_stat st;

        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", path, de->name);
        snprintf(r, sizeof(r), "%s%s%s", rel, *rel ? "/" : "", de->name);
        if (left_alone(r) || lstat(p, &st) < 0)
            continue;
        if (S_ISDIR(st.mode)) {
            if (put_rec(j, T_DIR, r, &st, 0) < 0 || save_tree(j, p, r) < 0) {
                closedir(d);
                return -1;
            }
            tick(j, 1, "Saving");
        } else if (S_ISLNK(st.mode)) {
            char target[512];
            ssize_t n = readlink(p, target, sizeof(target));

            if (n < 0 || put_rec(j, T_LINK, r, &st, n) < 0 || write(j->fd, target, n) != n) {
                closedir(d);
                return job_fail(j, "Saving", p);
            }
            tick(j, 1, "Saving");
        } else if (S_ISREG(st.mode)) {
            int in = open(p, O_RDONLY);
            uint64_t left = st.size;

            if (in < 0 || put_rec(j, T_FILE, r, &st, st.size) < 0) {
                if (in >= 0)
                    close(in);
                closedir(d);
                return job_fail(j, "Saving", p);
            }
            while (left) {
                ssize_t n = read(in, iobuf, left < sizeof(iobuf) ? left : sizeof(iobuf));

                if (n <= 0 || write(j->fd, iobuf, n) != n) {
                    close(in);
                    closedir(d);
                    return job_fail(j, "Saving", p);
                }
                left -= n;
                tick(j, n, "Saving");
            }
            close(in);
            tick(j, 1, "Saving");
        }
    }
    closedir(d);
    return 0;
}

int sysimage_create(const char *root, const char *out, sysimage_progress_fn progress, void *u, char *error,
                    size_t esize)
{
    struct job j = { -1, progress, u, 0, 0, error, esize };
    char head[24];
    int64_t now = time(NULL);
    uint32_t v = 1, z = 0;

    j.total = tree_bytes(root, "");
    if ((j.fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
        return job_fail(&j, "Creating", out);
    memcpy(head, MAGIC, 8);
    memcpy(head + 8, &v, 4);
    memcpy(head + 12, &z, 4);
    memcpy(head + 16, &now, 8);
    if (write(j.fd, head, sizeof(head)) != sizeof(head) || save_tree(&j, root, "") < 0
        || put_rec(&j, T_END, "", NULL, 0) < 0) {
        if (!*error)
            job_fail(&j, "Writing", out);
        close(j.fd);
        unlink(out);
        return -1;
    }
    close(j.fd);
    sync();
    if (progress)
        progress(100, "Done", u);
    return 0;
}

int sysimage_info(const char *path, int64_t *created, uint64_t *size)
{
    char head[24];
    struct aegis_stat st;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    if (read(fd, head, sizeof(head)) != sizeof(head) || memcmp(head, MAGIC, 8) || fstat(fd, &st) < 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    close(fd);
    memcpy(created, head + 16, 8);
    *size = st.size;
    return 0;
}

// Empties root, except what images never hold (and the images themselves).
static int clear_tree(const char *root)
{
    struct dir_stream *d = opendir(root);
    struct aegis_dirent *de;
    char names[256][256];
    int n = 0, ret = 0;

    if (!d)
        return -1;
    while ((de = readdir(d)) && n < 256) {
        if (strcmp(de->name, ".") && strcmp(de->name, ".."))
            strlcpy(names[n++], de->name, sizeof(names[0]));
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char p[600];

        if (left_alone(names[i]))
            continue;
        snprintf(p, sizeof(p), "%s/%s", root, names[i]);
        if (!strcmp(names[i], "var")) {
            // Everything in /var but the backups.
            struct dir_stream *v = opendir(p);
            struct aegis_dirent *ve;
            char vn[64][256];
            int k = 0;

            if (!v)
                continue;
            while ((ve = readdir(v)) && k < 64)
                if (strcmp(ve->name, ".") && strcmp(ve->name, "..") && strcmp(ve->name, "backups"))
                    strlcpy(vn[k++], ve->name, sizeof(vn[0]));
            closedir(v);
            for (int m = 0; m < k; m++) {
                char q[900];

                snprintf(q, sizeof(q), "%s/%s", p, vn[m]);
                ret |= remove_path(q);
            }
            continue;
        }
        ret |= remove_path(p);
    }
    return ret;
}

int sysimage_restore(const char *image, const char *root, sysimage_progress_fn progress, void *u, char *error,
                     size_t esize)
{
    struct job j = { -1, progress, u, 0, 0, error, esize };
    struct aegis_stat st;
    char head[24];

    if (stat(image, &st) < 0 || (j.fd = open(image, O_RDONLY)) < 0)
        return job_fail(&j, "Opening", image);
    j.total = st.size;
    if (read(j.fd, head, sizeof(head)) != sizeof(head) || memcmp(head, MAGIC, 8)) {
        close(j.fd);
        snprintf(error, esize, "%s is not an Aegis system image.", image);
        return -1;
    }
    if (progress)
        progress(0, "Removing the current system", u);
    clear_tree(root);
    for (;;) {
        struct rec r;
        char rel[1024], path[1400];

        if (read(j.fd, &r, sizeof(r)) != sizeof(r))
            break;
        if (r.type == T_END) {
            close(j.fd);
            sync();
            if (progress)
                progress(100, "Done", u);
            return 0;
        }
        if (r.path_len == 0 || r.path_len >= sizeof(rel) || read(j.fd, rel, r.path_len) != r.path_len)
            break;
        rel[r.path_len] = 0;
        if (strstr(rel, "..") || rel[0] == '/')
            break;
        snprintf(path, sizeof(path), "%s/%s", root, rel);
        if (r.type == T_DIR) {
            mkdir(path, r.mode & 07777);
        } else if (r.type == T_LINK) {
            char target[512];

            if (r.size >= sizeof(target) || read(j.fd, target, r.size) != (ssize_t)r.size)
                break;
            target[r.size] = 0;
            unlink(path);
            symlink(target, path);
        } else if (r.type == T_FILE) {
            int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            uint64_t left = r.size;

            while (left) {
                ssize_t n = read(j.fd, iobuf, left < sizeof(iobuf) ? left : sizeof(iobuf));

                if (n <= 0)
                    break;
                if (out >= 0)
                    write(out, iobuf, n);
                left -= n;
                tick(&j, n, "Restoring");
            }
            if (out >= 0)
                close(out);
            if (left)
                break;
        } else {
            break;
        }
        if (r.type != T_LINK) {
            chown(path, r.uid, r.gid);
            chmod(path, r.mode & 07777);
            utime(path, r.mtime, r.mtime);
        }
    }
    close(j.fd);
    snprintf(error, esize, "The image is damaged or incomplete; the system may be partly restored.");
    return -1;
}
