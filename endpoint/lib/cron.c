#include "aegis.h"

// Cron schedules, shared by crond and the Cron Jobs app.
//
// A crontab line is:   [#off ]SCHEDULE COMMAND[  #: NAME]
// SCHEDULE is five fields (minute hour day month weekday; *, lists, ranges,
// steps) or one of @hourly @daily @weekly @monthly @yearly @reboot.
// "#off " keeps a job without running it. /msc/crontab lines have a user
// name after the schedule.

static bool field_matches(const char *f, int value, int lo, int hi)
{
    char part[64];

    while (*f) {
        const char *comma = strchr(f, ',');
        size_t len = comma ? (size_t)(comma - f) : strlen(f);
        int a = lo, b = hi, step = 1;
        char *slash, *dash;

        snprintf(part, sizeof(part), "%.*s", (int)len, f);
        if ((slash = strchr(part, '/'))) {
            *slash = 0;
            step = MAX(atoi(slash + 1), 1);
        }
        if (strcmp(part, "*")) {
            a = atoi(part);
            b = (dash = strchr(part, '-')) ? atoi(dash + 1) : (slash ? hi : a);
        }
        if (value >= a && value <= b && (value - a) % step == 0)
            return true;
        if (!comma)
            break;
        f = comma + 1;
    }
    return false;
}

static const char *expand(const char *s)
{
    if (!strcmp(s, "@hourly")) return "0 * * * *";
    if (!strcmp(s, "@daily") || !strcmp(s, "@midnight")) return "0 0 * * *";
    if (!strcmp(s, "@weekly")) return "0 0 * * 0";
    if (!strcmp(s, "@monthly")) return "0 0 1 * *";
    if (!strcmp(s, "@yearly") || !strcmp(s, "@annually")) return "0 0 1 1 *";
    return s;
}

bool cron_matches(const char *schedule, const struct tm *tm)
{
    char copy[128], *f[5];
    int n = 0;
    bool dom_any, dow_any, dom, dow;

    strlcpy(copy, expand(schedule), sizeof(copy));
    for (char *p = copy; n < 5 && *p;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        f[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = 0;
    }
    if (n != 5)
        return false;
    if (!field_matches(f[0], tm->tm_min, 0, 59) || !field_matches(f[1], tm->tm_hour, 0, 23)
        || !field_matches(f[3], tm->tm_mon + 1, 1, 12))
        return false;
    dom_any = !strcmp(f[2], "*");
    dow_any = !strcmp(f[4], "*");
    dom = field_matches(f[2], tm->tm_mday, 1, 31);
    dow = field_matches(f[4], tm->tm_wday, 0, 7) || (tm->tm_wday == 0 && field_matches(f[4], 7, 0, 7));
    // Classic cron: when both day fields are set, either may match.
    if (!dom_any && !dow_any)
        return dom || dow;
    return dom && dow;
}

// Splits a crontab line. Returns false for blank lines and comments.
bool cron_parse(char *line, bool with_user, struct cron_job *job)
{
    char *p = line, *name;
    int fields;

    memset(job, 0, sizeof(*job));
    while (*p == ' ' || *p == '\t')
        p++;
    job->enabled = true;
    if (!strncmp(p, "#off ", 5)) {
        job->enabled = false;
        p += 5;
    } else if (*p == '#' || !*p) {
        return false;
    }
    if ((name = strstr(p, "  #: "))) {
        strlcpy(job->name, name + 5, sizeof(job->name));
        *name = 0;
    }
    fields = *p == '@' ? 1 : 5;
    {
        char *s = p;

        for (int i = 0; i < fields; i++) {
            while (*s == ' ' || *s == '\t')
                s++;
            while (*s && *s != ' ' && *s != '\t')
                s++;
        }
        snprintf(job->schedule, sizeof(job->schedule), "%.*s", (int)(s - p), p);
        p = s;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    if (with_user) {
        char *s = p;

        while (*s && *s != ' ' && *s != '\t')
            s++;
        snprintf(job->user, sizeof(job->user), "%.*s", (int)(s - p), p);
        p = s;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    strlcpy(job->command, p, sizeof(job->command));
    {
        size_t n = strlen(job->command);

        while (n && (job->command[n - 1] == ' ' || job->command[n - 1] == '\t'))
            job->command[--n] = 0;
    }
    return *job->schedule && *job->command;
}

void cron_format(const struct cron_job *job, bool with_user, char *out, size_t size)
{
    snprintf(out, size, "%s%s%s%s %s%s%s", job->enabled ? "" : "#off ", job->schedule, with_user ? " " : "",
             with_user ? job->user : "", job->command, *job->name ? "  #: " : "", job->name);
}

// The next time (to the minute) after `after` that the schedule runs, or -1.
int64_t cron_next(const char *schedule, int64_t after)
{
    int64_t t = (after / 60 + 1) * 60;
    struct tm tm;

    if (!strcmp(schedule, "@reboot"))
        return -1;
    // Search minute by minute, at most a year (cheap enough for a UI).
    for (int i = 0; i < 366 * 24 * 60; i++, t += 60) {
        localtime_r(&t, &tm);
        if (cron_matches(schedule, &tm))
            return t;
    }
    return -1;
}

void cron_user_path(const struct user_info *u, const char *file, char *out, size_t size)
{
    snprintf(out, size, "%s/system/appdata/cron/%s", u->dir, file);
}
