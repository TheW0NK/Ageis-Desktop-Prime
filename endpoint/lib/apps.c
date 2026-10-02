#include "aegis.h"

// The application registry: one /usr/share/applications/<id>.app file per
// app, with key=value lines:
//
//   name=Terminal
//   exec=/bin/term
//   icon=terminal
//   suite=System
//   description=Command line
//   opens=.txt;.md          (file types it can open)
//   tier=basic              (basic, elevated, system, powersudo)
//   hidden=true             (not shown in the launcher)

#define APPS_DIR "/usr/share/applications"

static void parse(const char *path, const char *id, struct app_info *a)
{
    int fd = open(path, O_RDONLY);
    char line[256];

    memset(a, 0, sizeof(*a));
    strlcpy(a->id, id, sizeof(a->id));
    strlcpy(a->name, id, sizeof(a->name));
    strlcpy(a->icon, id, sizeof(a->icon));
    strlcpy(a->tier, "basic", sizeof(a->tier));
    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) >= 0) {
        char *eq = strchr(line, '=');
        char *key = line, *val;

        if (line[0] == '#' || !eq)
            continue;
        *eq = 0;
        val = eq + 1;
        if (!strcmp(key, "name"))
            strlcpy(a->name, val, sizeof(a->name));
        else if (!strcmp(key, "exec"))
            strlcpy(a->exec, val, sizeof(a->exec));
        else if (!strcmp(key, "icon"))
            strlcpy(a->icon, val, sizeof(a->icon));
        else if (!strcmp(key, "suite"))
            strlcpy(a->suite, val, sizeof(a->suite));
        else if (!strcmp(key, "description"))
            strlcpy(a->description, val, sizeof(a->description));
        else if (!strcmp(key, "opens"))
            strlcpy(a->opens, val, sizeof(a->opens));
        else if (!strcmp(key, "tier"))
            strlcpy(a->tier, val, sizeof(a->tier));
        else if (!strcmp(key, "hidden"))
            a->hidden = !strcmp(val, "true");
        else if (!strcmp(key, "feature"))
            strlcpy(a->feature, val, sizeof(a->feature));
    }
    close(fd);
}

static int by_name(const void *x, const void *y)
{
    return strcasecmp(((const struct app_info *)x)->name, ((const struct app_info *)y)->name);
}

int app_list(struct app_info *out, int max)
{
    struct dir_stream *d = opendir(APPS_DIR);
    struct aegis_dirent *e;
    int n = 0;

    if (!d)
        return 0;
    while (n < max && (e = readdir(d))) {
        size_t len = strlen(e->name);
        char path[300], id[64];

        if (len < 5 || strcmp(e->name + len - 4, ".app") || len - 4 >= sizeof(id))
            continue;
        memcpy(id, e->name, len - 4);
        id[len - 4] = 0;
        snprintf(path, sizeof(path), APPS_DIR "/%s", e->name);
        parse(path, id, &out[n]);
        // Apps that belong to a switched-off feature are left out.
        if (out[n].exec[0] && (!out[n].feature[0] || feature_enabled(out[n].feature)))
            n++;
    }
    closedir(d);
    qsort(out, n, sizeof(*out), by_name);
    return n;
}

int app_find(const char *id, struct app_info *out)
{
    char path[300];
    struct aegis_stat st;

    snprintf(path, sizeof(path), APPS_DIR "/%s.app", id);
    if (stat(path, &st) < 0)
        return -1;
    parse(path, id, out);
    return out->exec[0] ? 0 : -1;
}

// The app that opens a file of this name, by extension, or -1.
int app_for_file(const char *name, struct app_info *out)
{
    struct app_info apps[64];
    const char *dot = strrchr(name, '.');
    int n;

    if (!dot)
        return -1;
    n = app_list(apps, 64);
    for (int i = 0; i < n; i++) {
        const char *p = apps[i].opens;
        size_t el = strlen(dot);

        while (*p) {
            const char *semi = strchr(p, ';');
            size_t len = semi ? (size_t)(semi - p) : strlen(p);

            if (len == el && !strncasecmp(p, dot, len)) {
                *out = apps[i];
                return 0;
            }
            if (!semi)
                break;
            p = semi + 1;
        }
    }
    return -1;
}

// Starts a program detached from the caller's terminal. Returns the pid.
int launch(const char *path, const char *arg)
{
    char *argv[3];
    const char *base = strrchr(path, '/');
    int saved[3], null = open("/dev/null", O_RDWR), pid;

    argv[0] = (char *)(base ? base + 1 : path);
    argv[1] = (char *)arg;
    argv[2] = NULL;
    if (null < 0)
        return spawn(path, argv, environ);
    for (int i = 0; i < 3; i++) {
        saved[i] = dup(i);
        dup2(null, i);
    }
    pid = spawn(path, argv, environ);
    for (int i = 0; i < 3; i++) {
        dup2(saved[i], i);
        close(saved[i]);
    }
    close(null);
    return pid;
}

int app_launch(const struct app_info *a, const char *arg)
{
    return launch(a->exec, arg);
}
