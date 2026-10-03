#include "aegis.h"

// The accounts (ACCOUNTS_FILE) and per-user settings.
//
// Every user has /userfiles/<name>/home (their files: Desktop, Documents,
// Downloads, Images, Music) and /userfiles/<name>/system (settings, credentials,
// appdata). Their settings (name, theme, language, background) are in
// system/settings.aset.

static void fill(struct user_info *u, const struct rec_block *b)
{
    const char *v;
    size_t len;

    memset(u, 0, sizeof(*u));
    strlcpy(u->name, b->name, sizeof(u->name));
    u->uid = (v = rec_get(b, "id")) ? strtoul(v, NULL, 10) : 65534;
    u->gid = (v = rec_get(b, "group")) ? strtoul(v, NULL, 10) : u->uid;
    strlcpy(u->display, (v = rec_get(b, "display")) && *v ? v : b->name, sizeof(u->display));
    if ((v = rec_get(b, "home")))
        strlcpy(u->home, v, sizeof(u->home));
    else
        snprintf(u->home, sizeof(u->home), "/userfiles/%s/home", b->name);
    strlcpy(u->shell, (v = rec_get(b, "terminal")) ? v : "/sysapps/terminal", sizeof(u->shell));
    // The user's folder holds home/ and system/.
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

// Finds an account by name, or by id when name is NULL.
static int lookup(const char *name, uint32_t uid, struct user_info *out)
{
    struct records r;
    int ret = -1;

    records_load(ACCOUNTS_FILE, "accounts", &r);
    for (int i = 1; i < r.n; i++) {
        const char *id = rec_get(&r.b[i], "id");

        if (strcmp(r.b[i].kind, "account") || (name ? strcmp(r.b[i].name, name) : !id || strtoul(id, NULL, 10) != uid))
            continue;
        fill(out, &r.b[i]);
        ret = 0;
        break;
    }
    records_free(&r);
    if (ret)
        errno = ENOENT;
    return ret;
}

int user_by_name(const char *name, struct user_info *out)
{
    return lookup(name, 0, out);
}

int user_by_uid(uint32_t uid, struct user_info *out)
{
    return lookup(NULL, uid, out);
}

int user_list(struct user_info *out, int max)
{
    struct records r;
    int n = 0;

    records_load(ACCOUNTS_FILE, "accounts", &r);
    for (int i = 1; i < r.n && n < max; i++) {
        const char *id = rec_get(&r.b[i], "id");
        uint32_t uid = id ? strtoul(id, NULL, 10) : 0;

        // People, not system accounts.
        if (!strcmp(r.b[i].kind, "account") && uid >= 1000 && uid < 60000)
            fill(&out[n++], &r.b[i]);
    }
    records_free(&r);
    return n;
}

bool user_in_group(const char *name, const char *group)
{
    struct records r;
    bool found;

    records_load(ACCOUNTS_FILE, "accounts", &r);
    found = rec_list_has(rec_get(records_find(&r, "group", group), "members"), name);
    records_free(&r);
    return found;
}

void user_path(const struct user_info *u, const char *rel, char *buf, size_t size)
{
    snprintf(buf, size, "%s/%s", u->dir, rel);
}

int user_setting_get(const struct user_info *u, const char *key, char *buf, size_t size)
{
    char path[256];
    struct records r;
    const char *v;
    int n = -1;

    if (!size)
        return -1;
    snprintf(path, sizeof(path), "%s/system/settings.aset", u->dir);
    if (records_load(path, "settings", &r) == 0 && (v = rec_get(records_top(&r), key))) {
        strlcpy(buf, v, size);
        n = strlen(buf);
    }
    records_free(&r);
    return n;
}

int user_setting_set(const struct user_info *u, const char *key, const char *value)
{
    char path[256];
    struct records r;
    int ret;

    snprintf(path, sizeof(path), "%s/system/settings.aset", u->dir);
    records_load(path, "settings", &r);
    ret = rec_set(records_top(&r), key, value);
    if (ret == 0)
        ret = records_save(&r, path, 0600);
    records_free(&r);
    // Written by an administrator: the file still belongs to the user.
    if (ret == 0 && geteuid() == 0)
        chown(path, u->uid, u->gid);
    return ret;
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
