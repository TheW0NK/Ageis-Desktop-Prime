#include "aegis.h"

// The job scheduler. Started by init as root. Once a minute it reads
// /msc/crontab (lines name a user) and every user's crontab, and starts
// the jobs that are due. Each job runs as its owner through
// "crond --run UID COMMAND", which drops to that account and logs the
// outcome.

#define LOG_MAX (64 * 1024)

static void append_log(const char *path, uint32_t uid, uint32_t gid, const char *text)
{
    struct aegis_stat st;
    int fd;

    // Keep logs short: start over once they grow large.
    if (stat(path, &st) == 0 && st.size > LOG_MAX)
        unlink(path);
    if ((fd = open(path, O_WRONLY | O_CREAT, 0600)) < 0)
        return;
    lseek(fd, 0, SEEK_END);
    write(fd, text, strlen(text));
    close(fd);
    chown(path, uid, gid);
}

// crond --run UID COMMAND
static int run_job(uint32_t uid, const char *command)
{
    struct user_info u;
    char log[256], line[512], when[32], *argv[] = { "terminal", "-c", (char *)command, NULL };
    int64_t now = time(NULL);
    struct tm tm;
    int fd, pid, status = 0;
    uint64_t started = uptime_ms();

    if (uid == 0) {
        strlcpy(log, "/osystem/logs/cron.log", sizeof(log));
        strlcpy(u.home, "/userfiles/superuser", sizeof(u.home));
        strlcpy(u.name, "root", sizeof(u.name));
    } else {
        if (user_by_uid(uid, &u) < 0)
            return 1;
        cron_user_path(&u, "log", log, sizeof(log));
        if (become(uid) < 0)
            return 1;
    }
    localtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    snprintf(line, sizeof(line), "%s started: %s\n", when, command);
    append_log(log, uid, uid ? u.gid : 0, line);
    setenv("HOME", u.home);
    setenv("USER", u.name);
    setenv("PATH", "/sysapps:/osystem/core:/userApps/commands");
    chdir(u.home);
    // The job's output goes into the log as well.
    if ((fd = open(log, O_WRONLY | O_CREAT, 0600)) >= 0) {
        lseek(fd, 0, SEEK_END);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    if ((fd = open("/osystem/devices/null", O_RDONLY)) >= 0) {
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if ((pid = spawn("/sysapps/terminal", argv, environ)) < 0)
        status = 127 << 8;
    else
        while (waitpid(pid, &status, 0) != pid)
            ;
    snprintf(line, sizeof(line), "%s finished with status %d after %lu s: %s\n", when,
             WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (status & 0x7F),
             (unsigned long)((uptime_ms() - started) / 1000), command);
    append_log(log, uid, uid ? u.gid : 0, line);
    return 0;
}

static void start_job(uint32_t uid, const char *command)
{
    char id[16], *argv[] = { "crond", "--run", id, (char *)command, NULL };

    snprintf(id, sizeof(id), "%u", uid);
    spawn("/osystem/core/crond", argv, environ);
}

static void run_file(const char *path, bool system, uint32_t owner, const struct tm *tm, bool boot)
{
    int fd = open(path, O_RDONLY);
    char line[512];
    struct cron_job job;
    struct aegis_stat st;

    if (fd < 0)
        return;
    // A user's crontab must be theirs, so nobody can run jobs as them.
    if (!system && (fstat(fd, &st) < 0 || st.uid != owner)) {
        close(fd);
        return;
    }
    while (read_line(fd, line, sizeof(line)) >= 0) {
        bool due;

        if (!cron_parse(line, system, &job) || !job.enabled)
            continue;
        due = !strcmp(job.schedule, "@reboot") ? boot : !boot && cron_matches(job.schedule, tm);
        if (!due)
            continue;
        if (system) {
            struct user_info u;
            uint32_t uid = 0;

            if (strcmp(job.user, "root")) {
                if (user_by_name(job.user, &u) < 0)
                    continue;
                uid = u.uid;
            }
            start_job(uid, job.command);
        } else {
            start_job(owner, job.command);
        }
    }
    close(fd);
}

static void run_due(bool boot)
{
    struct user_info users[128];
    int n = user_list(users, 128);
    int64_t now = time(NULL);
    struct tm tm;
    char path[256];

    localtime_r(&now, &tm);
    run_file("/msc/crontab", true, 0, &tm, boot);
    for (int i = 0; i < n; i++) {
        cron_user_path(&users[i], "crontab", path, sizeof(path));
        run_file(path, false, users[i].uid, &tm, boot);
    }
}

int main(int argc, char **argv)
{
    int64_t last = -1;

    if (argc == 4 && !strcmp(argv[1], "--run"))
        return run_job(strtoul(argv[2], NULL, 10), argv[3]);
    if (geteuid() != 0) {
        dprintf(STDERR_FILENO, "crond: must run as root\n");
        return 1;
    }
    mkdir("/osystem", 0755);
    mkdir("/osystem/logs", 0755);
    syslog("crond", "started");
    run_due(true);
    for (;;) {
        int64_t now = time(NULL), minute = now / 60;
        int status;

        while (waitpid(-1, &status, WNOHANG) > 0)
            ;
        if (minute != last) {
            if (last >= 0)
                run_due(false);
            last = minute;
        }
        // Wake just after the next minute starts.
        msleep((60 - now % 60) * 1000 + 200);
    }
}
