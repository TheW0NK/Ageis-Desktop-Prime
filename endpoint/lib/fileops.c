#include "aegis.h"

// Whole-tree file operations for file managers and installers.

static int copy_file(const char *src, const char *dst, uint32_t mode)
{
    char *buf = malloc(65536);
    int in = open(src, O_RDONLY), out = -1, ret = -1;
    ssize_t n;

    if (!buf || in < 0)
        goto done;
    if ((out = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode & 0777)) < 0)
        goto done;
    while ((n = read(in, buf, 65536)) > 0) {
        char *p = buf;

        while (n > 0) {
            ssize_t w = write(out, p, n);

            if (w <= 0)
                goto done;
            p += w;
            n -= w;
        }
    }
    ret = n < 0 ? -1 : 0;
done:
    free(buf);
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    if (ret < 0 && out >= 0)
        unlink(dst);
    return ret;
}

int copy_path(const char *src, const char *dst)
{
    struct aegis_stat st;

    if (lstat(src, &st) < 0)
        return -1;
    // Never copy a folder into itself.
    {
        size_t n = strlen(src);

        if (!strncmp(dst, src, n) && (dst[n] == '/' || !dst[n])) {
            errno = EINVAL;
            return -1;
        }
    }
    if (S_ISLNK(st.mode)) {
        char target[512];
        ssize_t n = readlink(src, target, sizeof(target) - 1);

        if (n < 0)
            return -1;
        target[n] = 0;
        return symlink(target, dst);
    }
    if (S_ISDIR(st.mode)) {
        struct dir_stream *d;
        struct aegis_dirent *e;
        int ret = 0;

        if (mkdir(dst, st.mode & 0777) < 0)
            return -1;
        if (!(d = opendir(src)))
            return -1;
        while ((e = readdir(d))) {
            char *a, *b;
            size_t la, lb;

            if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
                continue;
            la = strlen(src) + strlen(e->name) + 2;
            lb = strlen(dst) + strlen(e->name) + 2;
            a = malloc(la);
            b = malloc(lb);
            if (!a || !b) {
                free(a);
                free(b);
                ret = -1;
                break;
            }
            snprintf(a, la, "%s/%s", src, e->name);
            snprintf(b, lb, "%s/%s", dst, e->name);
            if (copy_path(a, b) < 0)
                ret = -1;
            free(a);
            free(b);
        }
        closedir(d);
        return ret;
    }
    return copy_file(src, dst, st.mode);
}

int remove_path(const char *path)
{
    struct aegis_stat st;

    if (lstat(path, &st) < 0)
        return -1;
    if (S_ISDIR(st.mode)) {
        struct dir_stream *d = opendir(path);
        struct aegis_dirent *e;
        int ret = 0;

        if (!d)
            return -1;
        // Collect names first: removing while reading would skip entries.
        for (;;) {
            char *names[64];
            int n = 0;

            while (n < 64 && (e = readdir(d))) {
                if (strcmp(e->name, ".") && strcmp(e->name, ".."))
                    names[n++] = strdup(e->name);
            }
            for (int i = 0; i < n; i++) {
                char *child;
                size_t len = strlen(path) + (names[i] ? strlen(names[i]) : 0) + 2;

                if (names[i] && (child = malloc(len))) {
                    snprintf(child, len, "%s/%s", path, names[i]);
                    if (remove_path(child) < 0)
                        ret = -1;
                    free(child);
                }
                free(names[i]);
            }
            if (n < 64)
                break;
            // The directory changed under us: read it again from the start.
            closedir(d);
            if (!(d = opendir(path)))
                return -1;
            if (ret < 0)
                break;
        }
        closedir(d);
        if (ret < 0)
            return -1;
        return rmdir(path);
    }
    return unlink(path);
}

int move_path(const char *src, const char *dst)
{
    if (rename(src, dst) == 0)
        return 0;
    // Across filesystems: copy, then remove the original.
    if (errno != EXDEV && errno != EPERM && errno != ENOSYS)
        return -1;
    if (copy_path(src, dst) < 0)
        return -1;
    return remove_path(src);
}

// A name in dir that does not exist yet: "name", "name (2)", ...
void unique_name(const char *dir, const char *name, char *out, size_t size)
{
    const char *dot = strrchr(name, '.');
    int base = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    struct aegis_stat st;

    snprintf(out, size, "%s/%s", dir, name);
    for (int i = 2; stat(out, &st) == 0 && i < 1000; i++)
        snprintf(out, size, "%s/%.*s (%d)%s", dir, base, name, i, name + base);
}
