#include "aegis.h"

// The recycle bin: /userfiles/<name>/system/trash/files holds what was deleted,
// and trash/info/<item>.info says where each one came from and when.

static int trash_dirs(char *files, char *info, size_t size)
{
    struct user_info me;
    char top[300];

    if (user_current(&me) < 0)
        return -1;
    user_path(&me, "system/trash", top, sizeof(top));
    snprintf(files, size, "%s/files", top);
    snprintf(info, size, "%s/info", top);
    mkdir(top, 0700);
    mkdir(files, 0700);
    mkdir(info, 0700);
    return 0;
}

void trash_dir(char *out, size_t size)
{
    char info[512];

    if (trash_dirs(out, info, size) < 0 && size)
        *out = 0;
}

int trash_put(const char *path)
{
    char files[512], info[512], dst[800], meta[900], line[700];
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    int fd, n;

    if (trash_dirs(files, info, sizeof(files)) < 0 || !*base)
        return -1;
    // Something already in the bin is deleted for good.
    if (!strncmp(path, files, strlen(files)) && path[strlen(files)] == '/')
        return remove_path(path);
    unique_name(files, base, dst, sizeof(dst));
    if (move_path(path, dst) < 0)
        return -1;
    snprintf(meta, sizeof(meta), "%s/%s.info", info, strrchr(dst, '/') + 1);
    if ((fd = open(meta, O_WRONLY | O_CREAT | O_TRUNC, 0600)) >= 0) {
        n = snprintf(line, sizeof(line), "%s\n%lld\n", path, (long long)time(NULL));
        write(fd, line, n);
        close(fd);
    }
    return 0;
}

int trash_list(struct trash_item **out)
{
    char files[512], info[512];
    struct dir_stream *d;
    struct aegis_dirent *de;
    struct trash_item *items = NULL;
    int n = 0, cap = 0;

    *out = NULL;
    if (trash_dirs(files, info, sizeof(files)) < 0 || !(d = opendir(files)))
        return -1;
    while ((de = readdir(d))) {
        struct trash_item *t;
        char meta[900], buf[700];
        struct aegis_stat st;
        int fd;

        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        if (n == cap) {
            struct trash_item *m = realloc(items, (cap = cap ? cap * 2 : 32) * sizeof(*m));

            if (!m)
                break;
            items = m;
        }
        t = &items[n++];
        memset(t, 0, sizeof(*t));
        strlcpy(t->name, de->name, sizeof(t->name));
        snprintf(t->path, sizeof(t->path), "%s/%s", files, de->name);
        if (stat(t->path, &st) == 0) {
            t->dir = S_ISDIR(st.mode);
            t->size = st.size;
            t->deleted = st.mtime;
        }
        snprintf(meta, sizeof(meta), "%s/%s.info", info, de->name);
        if ((fd = open(meta, O_RDONLY)) >= 0) {
            ssize_t len = read(fd, buf, sizeof(buf) - 1);
            char *nl;

            close(fd);
            if (len > 0) {
                buf[len] = 0;
                if ((nl = strchr(buf, '\n'))) {
                    *nl = 0;
                    t->deleted = strtol(nl + 1, NULL, 10);
                }
                strlcpy(t->origin, buf, sizeof(t->origin));
            }
        }
    }
    closedir(d);
    *out = items;
    return n;
}

static void drop_info(const char *name)
{
    char files[512], info[512], meta[900];

    if (trash_dirs(files, info, sizeof(files)) == 0) {
        snprintf(meta, sizeof(meta), "%s/%s.info", info, name);
        unlink(meta);
    }
}

int trash_restore(const struct trash_item *t)
{
    char dir[512], dst[800], *slash;

    if (!*t->origin)
        return -1;
    strlcpy(dir, t->origin, sizeof(dir));
    if (!(slash = strrchr(dir, '/')))
        return -1;
    *slash = 0;
    // Put it back under its old name, or beside it if that is taken now.
    unique_name(dir, slash + 1, dst, sizeof(dst));
    if (move_path(t->path, dst) < 0)
        return -1;
    drop_info(t->name);
    return 0;
}

int trash_delete(const struct trash_item *t)
{
    if (remove_path(t->path) < 0)
        return -1;
    drop_info(t->name);
    return 0;
}

int trash_empty(void)
{
    struct trash_item *items;
    int n = trash_list(&items), bad = 0;

    for (int i = 0; i < n; i++)
        bad |= trash_delete(&items[i]) < 0;
    free(items);
    return n < 0 || bad ? -1 : 0;
}

int trash_count(void)
{
    char files[512], info[512];
    struct dir_stream *d;
    struct aegis_dirent *de;
    int n = 0;

    if (trash_dirs(files, info, sizeof(files)) < 0 || !(d = opendir(files)))
        return 0;
    while ((de = readdir(d)))
        n += strcmp(de->name, ".") && strcmp(de->name, "..");
    closedir(d);
    return n;
}
