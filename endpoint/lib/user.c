#include "aegis.h"

// The user database (/msc/passwd, /msc/group) and per-user settings.
//
// Every user has /userfiles/<name>/home (their files: Desktop, Documents,
// Downloads, Images, Music) and /userfiles/<name>/system (settings, credentials,
// appdata). Settings are small files in system/settings: name, theme,
// language, background, picture.

static int split(char *line, char **f, int max)
{
    int n = 0;

    f[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == ':') {
            *p = 0;
            f[n++] = p + 1;
        }
    }
    return n;
}

static void fill(struct user_info *u, char **f)
{
    size_t len;

    memset(u, 0, sizeof(*u));
    strlcpy(u->name, f[0], sizeof(u->name));
    u->uid = strtoul(f[2], NULL, 10);
    u->gid = strtoul(f[3], NULL, 10);
    strlcpy(u->display, *f[4] ? f[4] : f[0], sizeof(u->display));
    strlcpy(u->home, f[5], sizeof(u->home));
    strlcpy(u->shell, f[6], sizeof(u->shell));
    // The user's directory holds home/ and system/.
    strlcpy(u->dir, u->home, sizeof(u->dir));
    len = strlen(u->dir);
    if (len > 5 && !strcmp(u->dir + len - 5, "/home"))
        u->dir[len - 5] = 0;
    {
        char name[64];

        if (user_setting_get(u, "name", name, sizeof(name)) > 0)
            strlcpy(u->display, name, sizeof(u->display));
    }
}

// Calls fn for each passwd entry until it returns true.
static bool each_user(bool (*fn)(char **f, void *ctx), void *ctx)
{
    int fd = open("/msc/passwd", O_RDONLY);
    char line[512];
    bool found = false;

    if (fd < 0)
        return false;
    while (!found && read_line(fd, line, sizeof(line)) >= 0) {
        char *f[7];

        if (line[0] == '#' || split(line, f, 7) != 7)
            continue;
        found = fn(f, ctx);
    }
    close(fd);
    return found;
}

struct lookup {
    const char *name;
    uint32_t uid;
    struct user_info *out;
};

static bool by_name(char **f, void *ctx)
{
    struct lookup *l = ctx;

    if (strcmp(f[0], l->name))
        return false;
    fill(l->out, f);
    return true;
}

static bool by_uid(char **f, void *ctx)
{
    struct lookup *l = ctx;

    if (strtoul(f[2], NULL, 10) != l->uid)
        return false;
    fill(l->out, f);
    return true;
}

int user_by_name(const char *name, struct user_info *out)
{
    struct lookup l = { name, 0, out };

    if (each_user(by_name, &l))
        return 0;
    errno = ENOENT;
    return -1;
}

int user_by_uid(uint32_t uid, struct user_info *out)
{
    struct lookup l = { NULL, uid, out };

    if (each_user(by_uid, &l))
        return 0;
    errno = ENOENT;
    return -1;
}

struct listing {
    struct user_info *out;
    int n, max;
};

static bool add_user(char **f, void *ctx)
{
    struct listing *l = ctx;
    uint32_t uid = strtoul(f[2], NULL, 10);

    // People, not system accounts.
    if (uid >= 1000 && uid < 60000 && l->n < l->max)
        fill(&l->out[l->n++], f);
    return false;
}

int user_list(struct user_info *out, int max)
{
    struct listing l = { out, 0, max };

    each_user(add_user, &l);
    return l.n;
}

bool user_in_group(const char *name, const char *group)
{
    int fd = open("/msc/group", O_RDONLY);
    char line[1024];
    bool found = false;

    if (fd < 0)
        return false;
    while (!found && read_line(fd, line, sizeof(line)) >= 0) {
        char *f[4], *m;

        if (split(line, f, 4) != 4 || strcmp(f[0], group))
            continue;
        for (m = f[3]; m && *m;) {
            char *comma = strchr(m, ',');
            size_t len = comma ? (size_t)(comma - m) : strlen(m);

            if (len == strlen(name) && !strncmp(m, name, len))
                found = true;
            m = comma ? comma + 1 : NULL;
        }
    }
    close(fd);
    return found;
}

void user_path(const struct user_info *u, const char *rel, char *buf, size_t size)
{
    snprintf(buf, size, "%s/%s", u->dir, rel);
}

int user_setting_get(const struct user_info *u, const char *key, char *buf, size_t size)
{
    char path[256];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/system/settings/%s", u->dir, key);
    if (!size || (fd = open(path, O_RDONLY)) < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        n--;
    buf[n] = 0;
    return n;
}

int user_setting_set(const struct user_info *u, const char *key, const char *value)
{
    char path[256], tmp[264];
    int fd;
    size_t len = strlen(value);

    snprintf(path, sizeof(path), "%s/system/settings/%s", u->dir, key);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
        return -1;
    if (write(fd, value, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    // Written by an administrator: the file still belongs to the user.
    if (geteuid() == 0)
        chown(tmp, u->uid, u->gid);
    // Replace in one step so readers never see half a value.
    return rename(tmp, path);
}

int user_current(struct user_info *out)
{
    return user_by_uid(getuid(), out);
}

static const char *const home_dirs[] = { "Desktop", "Documents", "Downloads", "Images", "Music", NULL };
static const char *const system_dirs[] = { "settings", "credentials", "appdata", NULL };

static int make_dir(const char *path, uint32_t mode, uint32_t uid, uint32_t gid, bool own)
{
    struct aegis_stat st;

    if (stat(path, &st) == 0)
        return 0;
    if (mkdir(path, mode) < 0)
        return -1;
    if (own && chown(path, uid, gid) < 0)
        return -1;
    return chmod(path, mode);
}

// Creates whatever is missing of the user's folders. Run as root (it sets
// ownership) or as the user (folders under an existing /userfiles/<name>).
int user_setup_dirs(const struct user_info *u)
{
    char path[256];
    bool root = geteuid() == 0;
    int bad = 0;

    make_dir("/userfiles", 0755, 0, 0, root);
    bad |= make_dir(u->dir, 0711, u->uid, u->gid, root);
    snprintf(path, sizeof(path), "%s/home", u->dir);
    bad |= make_dir(path, 0700, u->uid, u->gid, root);
    for (int i = 0; home_dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/home/%s", u->dir, home_dirs[i]);
        bad |= make_dir(path, 0755, u->uid, u->gid, root);
    }
    snprintf(path, sizeof(path), "%s/system", u->dir);
    bad |= make_dir(path, 0700, u->uid, u->gid, root);
    for (int i = 0; system_dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/system/%s", u->dir, system_dirs[i]);
        bad |= make_dir(path, 0700, u->uid, u->gid, root);
    }
    return bad ? -1 : 0;
}
