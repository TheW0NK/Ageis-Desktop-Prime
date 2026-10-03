#include "terminal.h"

#define HISTORY_MAX 64

char user_name[32];
static char computer_name[64] = "aegis";
char home_dir[256];

static char *history[HISTORY_MAX];
static int history_count;
static int last_status;
static int last_job;

void set_raw(bool raw)
{
    ioctl(STDIN_FILENO, IOCTL_CONSOLE_RAW, raw);
}

static void history_add(const char *line)
{
    if (!*line || (history_count && !strcmp(history[history_count - 1], line)))
        return;
    if (history_count == HISTORY_MAX) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (HISTORY_MAX - 1));
        history_count--;
    }
    history[history_count++] = strdup(line);
}

void history_print(void)
{
    for (int i = 0; i < history_count; i++)
        printf("%4d  %s\n", i + 1, history[i]);
}

static void redraw(const char *prompt, const char *buf, size_t len, size_t pos)
{
    printf("\r%s%.*s\x1b[K", prompt, (int)len, buf);
    if (len > pos)
        printf("\x1b[%zuD", len - pos);
}

static int read_key(void)
{
    char c;

    return read(STDIN_FILENO, &c, 1) == 1 ? (unsigned char)c : -1;
}

// Line editor. Returns the length, or -1 on end of input (Ctrl+D on an empty line).
static ssize_t edit_line(const char *prompt, char *buf, size_t size)
{
    size_t len = 0, pos = 0;
    int hist = history_count;
    char saved[LINE_MAX] = "";

    printf("%s", prompt);
    for (;;) {
        int c = read_key();

        if (c < 0)
            return -1;
        if (c == '\r' || c == '\n') {
            buf[len] = '\0';
            printf("\n");
            return len;
        }
        if (c == 0x04 && len == 0) {
            printf("\n");
            return -1;
        }
        if (c == 0x03) {
            printf("^C\n");
            len = pos = 0;
            printf("%s", prompt);
            continue;
        }
        if (c == 0x1B) {
            int a = read_key(), b = read_key();

            if (a != '[')
                continue;
            if (b == 'D' && pos > 0)
                pos--;
            else if (b == 'C' && pos < len)
                pos++;
            else if (b == 'H')
                pos = 0;
            else if (b == 'F')
                pos = len;
            else if (b == '3' && read_key() == '~' && pos < len) {
                memmove(buf + pos, buf + pos + 1, len - pos - 1);
                len--;
            } else if ((b == 'A' && hist > 0) || (b == 'B' && hist < history_count)) {
                if (hist == history_count) {
                    memcpy(saved, buf, len);
                    saved[len] = '\0';
                }
                hist += b == 'A' ? -1 : 1;
                const char *src = hist == history_count ? saved : history[hist];
                len = pos = strlcpy(buf, src, size);
                if (len >= size)
                    len = pos = size - 1;
            }
            redraw(prompt, buf, len, pos);
            continue;
        }
        switch (c) {
        case 0x7F:
        case '\b':
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos);
                pos--;
                len--;
            }
            break;
        case 0x01:
            pos = 0;
            break;
        case 0x05:
            pos = len;
            break;
        case 0x15:
            memmove(buf, buf + pos, len - pos);
            len -= pos;
            pos = 0;
            break;
        case 0x0B:
            len = pos;
            break;
        case 0x0C:
            printf("\x1b[2J\x1b[H");
            break;
        default:
            if (c >= 0x20 && c < 0x7F && len + 1 < size) {
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos++] = c;
                len++;
            }
        }
        redraw(prompt, buf, len, pos);
    }
}

ssize_t read_secret(const char *prompt, char *buf, size_t size)
{
    size_t len = 0;

    printf("%s", prompt);
    for (;;) {
        int c = read_key();

        if (c < 0 || c == 0x03 || c == 0x04) {
            printf("\n");
            return -1;
        }
        if (c == '\r' || c == '\n') {
            buf[len] = '\0';
            printf("\n");
            return len;
        }
        if ((c == 0x7F || c == '\b') && len)
            len--;
        else if (c >= 0x20 && len + 1 < size)
            buf[len++] = c;
    }
}

// ---- Parsing ----
//
// A line is a list of pipelines joined by ;, && and || (a trailing & runs
// the last one in the background). A pipeline is commands joined by |, each
// with optional redirections: < FILE, > FILE, >> FILE, 2> FILE, 2>&1.

enum token { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_AMP, T_IN, T_OUT, T_APPEND, T_ERR, T_ERR_OUT, T_END };

#define MAX_CMDS    16

struct cmd {
    char *argv[ARGS_MAX];
    int argc;
    char *in, *out, *err;
    bool append, err_to_out;
};

struct pipeline {
    struct cmd cmds[MAX_CMDS];
    int count;
    bool background;
};

struct parser {
    const char *p;
    char *out, *end;
};

// Reads one token; words are copied (unquoted and expanded) into storage.
static enum token next_token(struct parser *ps, char **word)
{
    const char *p = ps->p;

    while (isspace(*p))
        p++;
    if (!*p || *p == '#') {
        ps->p = p;
        return T_END;
    }
    if (p[0] == '|' && p[1] == '|') { ps->p = p + 2; return T_OR; }
    if (p[0] == '&' && p[1] == '&') { ps->p = p + 2; return T_AND; }
    if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') { ps->p = p + 4; return T_ERR_OUT; }
    if (p[0] == '2' && p[1] == '>') { ps->p = p + 2; return T_ERR; }
    if (p[0] == '>' && p[1] == '>') { ps->p = p + 2; return T_APPEND; }
    switch (*p) {
    case '|': ps->p = p + 1; return T_PIPE;
    case ';': ps->p = p + 1; return T_SEMI;
    case '&': ps->p = p + 1; return T_AMP;
    case '<': ps->p = p + 1; return T_IN;
    case '>': ps->p = p + 1; return T_OUT;
    }

    char quote = 0, *out = ps->out;
    *word = out;
    if (*p == '~' && (p[1] == '/' || !p[1] || isspace(p[1]))) {
        for (const char *h = home_dir; *h && out < ps->end; h++)
            *out++ = *h;
        p++;
    }
    while (*p && out < ps->end) {
        if (!quote && (isspace(*p) || strchr("|;&<>", *p)))
            break;
        if (quote && *p == quote) {
            quote = 0;
            p++;
        } else if (!quote && (*p == '"' || *p == '\'')) {
            quote = *p++;
        } else if (*p == '\\' && quote != '\'' && p[1]) {
            *out++ = p[1];
            p += 2;
        } else if (*p == '$' && quote != '\'' && (isalpha(p[1]) || p[1] == '_' || p[1] == '?' || p[1] == '!')) {
            char name[64], status[16];
            size_t n = 0;
            const char *val;

            p++;
            if (*p == '?' || *p == '!') {
                snprintf(status, sizeof(status), "%d", *p == '?' ? last_status : last_job);
                val = status;
                p++;
            } else {
                while ((isalnum(*p) || *p == '_') && n < sizeof(name) - 1)
                    name[n++] = *p++;
                name[n] = '\0';
                val = getenv(name);
            }
            for (; val && *val && out < ps->end; val++)
                *out++ = *val;
        } else {
            *out++ = *p++;
        }
    }
    *out++ = '\0';
    ps->out = out;
    ps->p = p;
    return T_WORD;
}

// Parses one pipeline. Returns the token that ended it, or -1 on a syntax error.
static int parse_pipeline(struct parser *ps, struct pipeline *pl)
{
    struct cmd *c;
    char *word;
    enum token t;

    memset(pl, 0, sizeof(*pl));
    pl->count = 1;
    c = &pl->cmds[0];
    for (;;) {
        t = next_token(ps, &word);
        switch (t) {
        case T_WORD:
            if (c->argc == ARGS_MAX - 1)
                return -1;
            c->argv[c->argc++] = word;
            continue;
        case T_IN:
        case T_OUT:
        case T_APPEND:
        case T_ERR:
            if (next_token(ps, &word) != T_WORD)
                return -1;
            if (t == T_IN)
                c->in = word;
            else if (t == T_ERR)
                c->err = word;
            else {
                c->out = word;
                c->append = t == T_APPEND;
            }
            continue;
        case T_ERR_OUT:
            c->err_to_out = true;
            continue;
        case T_PIPE:
            if (!c->argc || pl->count == MAX_CMDS)
                return -1;
            c = &pl->cmds[pl->count++];
            continue;
        case T_AMP:
            pl->background = true;
            /* fall through */
        default:
            if (!c->argc)
                return (pl->count == 1 && t != T_AMP) ? (int)t : -1;
            return t;
        }
    }
}

// "ls: command not found", plus what it is called here if it was renamed.
static void not_found(int fd, const char *name)
{
    for (const struct renamed_command *r = renamed_commands; r->old; r++)
        if (!strcmp(r->old, name)) {
            dprintf(fd, "%s: not an Aegis command; try '%s' (type 'help' for a list)\n", name, r->now);
            return;
        }
    dprintf(fd, "%s: command not found (type 'help' for a list)\n", name);
}

const struct command *find_command(const char *name)
{
    for (size_t i = 0; i < command_count; i++) {
        if (!strcmp(commands[i].name, name))
            return &commands[i];
    }
    return NULL;
}

static bool find_in_path(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH");
    struct aegis_stat st;

    if (strchr(name, '/')) {
        strlcpy(out, name, size);
        return true;
    }
    while (path && *path) {
        const char *colon = strchr(path, ':');
        size_t len = colon ? (size_t)(colon - path) : strlen(path);

        snprintf(out, size, "%.*s/%s", (int)len, path, name);
        if (stat(out, &st) == 0 && S_ISREG(st.mode))
            return true;
        path += len + (colon ? 1 : 0);
    }
    return false;
}

// Converts a wait() status to a shell status, reporting abnormal ends.
int report_status(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    switch (WTERMSIG(status)) {
    case SIGINT:    printf("\n"); break;
    case SIGKILL:   printf("[killed]\n"); break;
    case SIGTERM:   printf("[terminated]\n"); break;
    case SIGPIPE:   break;
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:    printf("[crashed: signal %d]\n", WTERMSIG(status)); break;
    default:        printf("[signal %d]\n", WTERMSIG(status));
    }
    return 128 + WTERMSIG(status);
}

static int wait_for(int pid)
{
    int status = 0;

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return status;
}

int run_external(int argc, char **argv)
{
    char path[AEGIS_PATH_MAX];
    int pid, status;

    (void)argc;
    if (!find_in_path(argv[0], path, sizeof(path))) {
        not_found(STDERR_FILENO, argv[0]);
        return 127;
    }
    set_raw(false);
    pid = spawn(path, argv, environ);
    if (pid < 0) {
        set_raw(true);
        fail(argv[0], path);
        return 126;
    }
    ioctl(STDIN_FILENO, IOCTL_CONSOLE_FOREGROUND, pid);
    status = wait_for(pid);
    ioctl(STDIN_FILENO, IOCTL_CONSOLE_FOREGROUND, 0);
    set_raw(true);
    return report_status(status);
}

int run_args(int argc, char **argv)
{
    const struct command *c = find_command(argv[0]);

    if (!c)
        return run_external(argc, argv);
    set_raw(false);
    int ret = c->fn(argc, argv);
    set_raw(true);
    return ret;
}

// ---- Running pipelines ----

static int saved_fds[3] = { -1, -1, -1 };

// Points fd at target (closing nothing the shell still needs).
static void redirect(int fd, int target)
{
    if (target >= 0 && target != fd)
        dup2(target, fd);
}

static void save_std(void)
{
    for (int i = 0; i < 3; i++) {
        saved_fds[i] = fcntl(i, F_DUPFD, 10);
        fcntl(saved_fds[i], F_SETFD, FD_CLOEXEC);
    }
}

static void restore_std(void)
{
    for (int i = 0; i < 3; i++) {
        dup2(saved_fds[i], i);
        close(saved_fds[i]);
        saved_fds[i] = -1;
    }
}

// Opens a command's redirections; returns false (after reporting) on failure.
static bool open_redirects(struct cmd *c, int *in, int *out, int *err)
{
    *in = *out = *err = -1;
    if (c->in && (*in = open(c->in, O_RDONLY | O_CLOEXEC)) < 0) {
        fail("open", c->in);
        return false;
    }
    if (c->out && (*out = open(c->out, O_WRONLY | O_CREAT | O_CLOEXEC | (c->append ? O_APPEND : O_TRUNC),
                               0644)) < 0) {
        fail("open", c->out);
        return false;
    }
    if (c->err && (*err = open(c->err, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
        fail("open", c->err);
        return false;
    }
    return true;
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static int run_pipeline(struct pipeline *pl)
{
    int pipes[MAX_CMDS][2], pids[MAX_CMDS], status = 0, last_pid = -1;
    int in[MAX_CMDS], out[MAX_CMDS], err[MAX_CMDS];
    bool ok = true;

    // A lone builtin without redirections runs as before.
    if (pl->count == 1 && !pl->background && !pl->cmds[0].in && !pl->cmds[0].out
        && !pl->cmds[0].err && !pl->cmds[0].err_to_out)
        return run_args(pl->cmds[0].argc, pl->cmds[0].argv);

    for (int i = 0; i < pl->count; i++) {
        pids[i] = -1;
        pipes[i][0] = pipes[i][1] = -1;
        in[i] = out[i] = err[i] = -1;
    }
    for (int i = 0; i + 1 < pl->count && ok; i++) {
        if (pipe2(pipes[i], O_CLOEXEC) < 0) {
            fail("pipe", "");
            ok = false;
        }
    }
    for (int i = 0; i < pl->count && ok; i++)
        ok = open_redirects(&pl->cmds[i], &in[i], &out[i], &err[i]);
    if (!ok)
        goto cleanup;

    set_raw(false);
    save_std();
    // Start the programs first, so builtins writing into pipes have readers.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < pl->count; i++) {
            struct cmd *c = &pl->cmds[i];
            const struct command *b = find_command(c->argv[0]);
            char path[AEGIS_PATH_MAX];

            if ((pass == 0) == (b != NULL))
                continue;
            redirect(0, in[i] >= 0 ? in[i] : i > 0 ? pipes[i - 1][0] : saved_fds[0]);
            redirect(1, out[i] >= 0 ? out[i] : i + 1 < pl->count ? pipes[i][1] : saved_fds[1]);
            redirect(2, c->err_to_out ? 1 : err[i] >= 0 ? err[i] : saved_fds[2]);
            if (b) {
                status = b->fn(c->argc, c->argv) << 8;
            } else if (!find_in_path(c->argv[0], path, sizeof(path))) {
                not_found(saved_fds[2], c->argv[0]);
                status = 127 << 8;
            } else if ((pids[i] = spawn(path, c->argv, environ)) < 0) {
                dprintf(saved_fds[2], "%s: %s\n", c->argv[0], strerror(errno));
                status = 126 << 8;
            }
            if (i == pl->count - 1)
                last_pid = pids[i];
            // A builtin's output is complete: let its reader see the end.
            if (b && i + 1 < pl->count)
                close_fd(&pipes[i][1]);
        }
        // Programs now hold their own write ends; builtins reading from
        // them must see end-of-file when the programs exit.
        for (int i = 0; pass == 0 && i + 1 < pl->count; i++) {
            if (pids[i] > 0)
                close_fd(&pipes[i][1]);
        }
    }
    restore_std();

cleanup:
    for (int i = 0; i < pl->count; i++) {
        close_fd(&pipes[i][0]);
        close_fd(&pipes[i][1]);
        close_fd(&in[i]);
        close_fd(&out[i]);
        close_fd(&err[i]);
    }
    if (!ok)
        return 1;
    if (pl->background) {
        for (int i = 0; i < pl->count; i++) {
            if (pids[i] > 0) {
                printf("[%d] %s\n", pids[i], pl->cmds[i].argv[0]);
                last_job = pids[i];
            }
        }
        set_raw(true);
        return 0;
    }
    if (last_pid > 0)
        ioctl(STDIN_FILENO, IOCTL_CONSOLE_FOREGROUND, last_pid);
    for (int i = 0; i < pl->count; i++) {
        if (pids[i] > 0) {
            int s = wait_for(pids[i]);
            if (pids[i] == last_pid)
                status = s;
        }
    }
    ioctl(STDIN_FILENO, IOCTL_CONSOLE_FOREGROUND, 0);
    set_raw(true);
    return report_status(status);
}

// Reports background jobs that have finished.
static void reap_jobs(void)
{
    int status, pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("[%d] done", pid);
        if (!WIFEXITED(status))
            printf(" (signal %d)", WTERMSIG(status));
        else if (WEXITSTATUS(status))
            printf(" (exit %d)", WEXITSTATUS(status));
        printf("\n");
    }
}

// Runs a whole line. Returns false if the shell should exit.
static bool run_line(const char *line, int *exit_code)
{
    static char storage[LINE_MAX * 2];
    struct parser ps = { line, storage, storage + sizeof(storage) - 1 };
    static struct pipeline pl;
    int skip = 0;               // 0: run, 1: skip because of && / ||

    for (;;) {
        int t = parse_pipeline(&ps, &pl);

        if (t < 0) {
            dprintf(STDERR_FILENO, "syntax error\n");
            last_status = 2;
            return true;
        }
        if (pl.cmds[0].argc && !skip) {
            char **argv = pl.cmds[0].argv;

            if (pl.count == 1 && (!strcmp(argv[0], "exit") || !strcmp(argv[0], "signout"))) {
                *exit_code = pl.cmds[0].argc > 1 ? atoi(argv[1]) : 0;
                return false;
            }
            last_status = run_pipeline(&pl);
        }
        if (t == T_END)
            return true;
        skip = (t == T_AND && last_status != 0) || (t == T_OR && last_status == 0);
    }
}

static void prompt_string(char *buf, size_t size)
{
    char cwd[AEGIS_PATH_MAX], shown[AEGIS_PATH_MAX];
    size_t hl = strlen(home_dir);

    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");
    if (hl > 1 && !strncmp(cwd, home_dir, hl) && (cwd[hl] == '/' || !cwd[hl]))
        snprintf(shown, sizeof(shown), "~%s", cwd + hl);
    else
        strlcpy(shown, cwd, sizeof(shown));
    // user@computer@folder - :   (the superuser's name is shown in red)
    snprintf(buf, size, "\x1b[%sm%s@%s\x1b[0m@\x1b[94m%s\x1b[0m - : ", geteuid() == 0 ? "91" : "92", user_name,
             computer_name, shown);
}

static void load_identity(void)
{
    char line[512];
    int hfd = open("/msc/hostname", O_RDONLY);

    if (hfd >= 0) {
        if (read_line(hfd, line, sizeof(line)) > 0 && line[0])
            strlcpy(computer_name, line, sizeof(computer_name));
        close(hfd);
    }
    int fd = open("/msc/passwd", O_RDONLY);
    uint32_t uid = getuid();

    snprintf(user_name, sizeof(user_name), "%u", uid);
    strcpy(home_dir, "/");
    if (fd < 0)
        return;
    for (;;) {
        ssize_t n = read_line(fd, line, sizeof(line));
        char *f[7];
        int k = 0;

        if (n < 0)
            break;
        f[k++] = line;
        for (char *p = line; *p && k < 7; p++) {
            if (*p == ':') {
                *p = '\0';
                f[k++] = p + 1;
            }
        }
        if (k == 7 && (uint32_t)atoi(f[2]) == uid) {
            strlcpy(user_name, f[0], sizeof(user_name));
            strlcpy(home_dir, f[5], sizeof(home_dir));
            break;
        }
    }
    close(fd);
}

static void show_file(const char *path)
{
    char buf[512];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, n);
    close(fd);
}

static void do_login(void)
{
    char name[32], pass[256];

    for (;;) {
        printf("\nAegis login: ");
        set_raw(false);
        ssize_t n = read_line(STDIN_FILENO, name, sizeof(name));
        set_raw(true);
        if (n <= 0)
            continue;
        if (read_secret("Password: ", pass, sizeof(pass)) < 0)
            continue;
        if (login(name, pass) == 0) {
            struct user_info u;
            char key[65];

            // Unlock the user's stored passwords for this session.
            if (user_by_name(name, &u) == 0 && cred_unlock(&u, pass, key, sizeof(key)) == 0)
                setenv("AEGIS_CRED_KEY", key);
            memset(key, 0, sizeof(key));
            memset(pass, 0, sizeof(pass));
            break;
        }
        memset(pass, 0, sizeof(pass));
        printf("Login incorrect\n");
    }

    load_identity();
    if (chdir(home_dir) < 0)
        chdir("/");
    setenv("HOME", home_dir);
    setenv("USER", user_name);
    setenv("PATH", "/sysapps:/osystem/core:/userApps/commands");
    printf("\n");
    show_file("/msc/motd");
}

int main(int argc, char **argv)
{
    char line[LINE_MAX], prompt[AEGIS_PATH_MAX + 64];
    struct aegis_utsname u;

    // terminal -c COMMAND: run one command line and exit with its status.
    if (argc > 2 && !strcmp(argv[1], "-c")) {
        int code = 0;

        load_identity();
        if (!getenv("HOME"))
            setenv("HOME", home_dir);
        if (!getenv("USER"))
            setenv("USER", user_name);
        if (!getenv("PATH"))
            setenv("PATH", "/sysapps:/osystem/core:/userApps/commands");
        if (!run_line(argv[2], &code))
            return code;
        return last_status;
    }
    // terminal SCRIPT [ARGS...]: run each line of a script ("#!/sysapps/terminal");
    // $@ in a line stands for the arguments.
    if (argc > 1 && argv[1][0] != '-') {
        struct aegis_stat st;
        int fd, code = 0;
        static char text[16384];
        ssize_t n;

        if (stat(argv[1], &st) < 0 || !S_ISREG(st.mode) || (fd = open(argv[1], O_RDONLY)) < 0) {
            dprintf(STDERR_FILENO, "terminal: %s: cannot run this script\n", argv[1]);
            return 127;
        }
        n = read(fd, text, sizeof(text) - 1);
        close(fd);
        text[n > 0 ? n : 0] = 0;
        load_identity();
        if (!getenv("HOME"))
            setenv("HOME", home_dir);
        if (!getenv("PATH"))
            setenv("PATH", "/sysapps:/osystem/core:/userApps/commands");
        for (char *l = text, *next; l && *l; l = next) {
            char expanded[LINE_MAX], *at;

            if ((next = strchr(l, '\n')))
                *next++ = 0;
            if (*l == '#' || !*l)
                continue;
            strlcpy(expanded, l, sizeof(expanded));
            if ((at = strstr(expanded, "$@"))) {
                char rest[LINE_MAX];

                strlcpy(rest, at + 2, sizeof(rest));
                *at = 0;
                for (int i = 2; i < argc; i++) {
                    strlcat(expanded, argv[i], sizeof(expanded));
                    if (i + 1 < argc)
                        strlcat(expanded, " ", sizeof(expanded));
                }
                strlcat(expanded, rest, sizeof(expanded));
            }
            if (!run_line(expanded, &code))
                return code;
        }
        return last_status;
    }
    set_raw(true);
    // In a graphical session the user is already signed in.
    if (argc > 1 && !strcmp(argv[1], "--no-login")) {
        load_identity();
        setenv("HOME", home_dir);
        setenv("USER", user_name);
        if (!getenv("PATH"))
            setenv("PATH", "/sysapps:/osystem/core:/userApps/commands");
    } else {
        if (uname(&u) == 0)
            printf("\n%s %s (%s)\n", u.sysname, u.release, u.machine);
        do_login();
    }

    for (;;) {
        int code;

        reap_jobs();
        prompt_string(prompt, sizeof(prompt));
        if (edit_line(prompt, line, sizeof(line)) < 0)
            break;
        history_add(line);
        if (!run_line(line, &code))
            return code;
    }
    return 0;
}
