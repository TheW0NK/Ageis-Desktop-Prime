#include "terminal.h"

#define HISTORY_MAX 64

char user_name[32];
char home_dir[256];

static char *history[HISTORY_MAX];
static int history_count;
static int last_status;

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

// Splits a line into words, handling quotes, backslashes, $VAR and ~.
static int parse(char *line, char **argv, char *storage, size_t storage_size)
{
    char *out = storage, *end = storage + storage_size - 1;
    int argc = 0;
    char *p = line;

    for (;;) {
        while (isspace(*p))
            p++;
        if (!*p || *p == '#' || argc == ARGS_MAX - 1)
            break;

        char quote = 0;
        argv[argc++] = out;
        if (*p == '~' && (p[1] == '/' || !p[1] || isspace(p[1]))) {
            for (const char *h = home_dir; *h && out < end; h++)
                *out++ = *h;
            p++;
        }
        while (*p && (quote || !isspace(*p)) && out < end) {
            if (quote && *p == quote) {
                quote = 0;
                p++;
            } else if (!quote && (*p == '"' || *p == '\'')) {
                quote = *p++;
            } else if (*p == '\\' && quote != '\'' && p[1]) {
                *out++ = p[1];
                p += 2;
            } else if (*p == '$' && quote != '\'' && (isalpha(p[1]) || p[1] == '_' || p[1] == '?')) {
                char name[64];
                size_t n = 0;
                const char *val;
                char status[16];

                p++;
                if (*p == '?') {
                    snprintf(status, sizeof(status), "%d", last_status);
                    val = status;
                    p++;
                } else {
                    while ((isalnum(*p) || *p == '_') && n < sizeof(name) - 1)
                        name[n++] = *p++;
                    name[n] = '\0';
                    val = getenv(name);
                }
                for (; val && *val && out < end; val++)
                    *out++ = *val;
            } else {
                *out++ = *p++;
            }
        }
        *out++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
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

int run_external(int argc, char **argv)
{
    char path[AEGIS_PATH_MAX];
    int pid, status = 0;

    (void)argc;
    if (!find_in_path(argv[0], path, sizeof(path))) {
        dprintf(STDERR_FILENO, "%s: command not found (type 'help' for a list)\n", argv[0]);
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
    while (waitpid(pid, &status) < 0 && errno == EINTR)
        ;
    ioctl(STDIN_FILENO, IOCTL_CONSOLE_FOREGROUND, 0);
    set_raw(true);
    if (status == -9)
        printf("[killed]\n");
    else if (status == -11)
        printf("[crashed]\n");
    return status;
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
    snprintf(buf, size, "\x1b[92m%s@aegis\x1b[0m:\x1b[94m%s\x1b[0m%c ",
             user_name, shown, geteuid() == 0 ? '#' : '$');
}

static void load_identity(void)
{
    char line[512];
    int fd = open("/etc/passwd", O_RDONLY);
    uint32_t uid = getuid();

    snprintf(user_name, sizeof(user_name), "%u", uid);
    strcpy(home_dir, "/");
    if (fd < 0)
        return;
    for (;;) {
        ssize_t n = read_line(fd, line, sizeof(line));
        char *f[7];
        int k = 0;

        if (n <= 0)
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
    setenv("PATH", "/bin:/sbin");
    printf("\n");
    show_file("/etc/motd");
}

int main(void)
{
    char line[LINE_MAX], storage[LINE_MAX * 2], prompt[AEGIS_PATH_MAX + 64];
    char *argv[ARGS_MAX];
    struct aegis_utsname u;

    set_raw(true);
    if (uname(&u) == 0)
        printf("\n%s %s (%s)\n", u.sysname, u.release, u.machine);
    do_login();

    for (;;) {
        int argc;

        prompt_string(prompt, sizeof(prompt));
        if (edit_line(prompt, line, sizeof(line)) < 0)
            break;
        history_add(line);
        argc = parse(line, argv, storage, sizeof(storage));
        if (argc == 0)
            continue;
        if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "logout"))
            return argc > 1 ? atoi(argv[1]) : 0;
        last_status = run_args(argc, argv);
    }
    return 0;
}
