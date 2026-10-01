#include "commands.h"

static char previous_dir[AEGIS_PATH_MAX];

static bool flag(int argc, char **argv, int *first, char f)
{
    bool found = false;

    for (int i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strchr(argv[i] + 1, f))
            found = true;
        *first = i + 1;
    }
    return found;
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    size_t len = strlen(dir);

    snprintf(out, size, "%s%s%s", dir, len && dir[len - 1] == '/' ? "" : "/", name);
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash && slash[1] ? slash + 1 : path;
}

int cmd_cd(int argc, char **argv)
{
    char cwd[AEGIS_PATH_MAX];
    const char *target = argc > 1 ? argv[1] : home_dir;

    if (!strcmp(target, "-")) {
        if (!previous_dir[0]) {
            dprintf(STDERR_FILENO, "cd: no previous directory\n");
            return 1;
        }
        target = previous_dir;
    }
    if (!getcwd(cwd, sizeof(cwd)))
        cwd[0] = '\0';
    if (chdir(target) < 0) {
        fail("cd", target);
        return 1;
    }
    strlcpy(previous_dir, cwd, sizeof(previous_dir));
    return 0;
}

int cmd_pwd(int argc, char **argv)
{
    char cwd[AEGIS_PATH_MAX];

    (void)argc;
    (void)argv;
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static void mode_string(uint32_t mode, char *s)
{
    const char *rwx = "rwxrwxrwx";

    s[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISCHR(mode) ? 'c' : '-';
    for (int i = 0; i < 9; i++)
        s[i + 1] = (mode & (0400 >> i)) ? rwx[i] : '-';
    if (mode & S_ISUID)
        s[3] = s[3] == 'x' ? 's' : 'S';
    if (mode & S_ISGID)
        s[6] = s[6] == 'x' ? 's' : 'S';
    if (mode & S_ISVTX)
        s[9] = s[9] == 'x' ? 't' : 'T';
    s[10] = '\0';
}

static const char *color_for(uint32_t mode)
{
    if (S_ISDIR(mode))
        return "\x1b[94m";
    if (S_ISLNK(mode))
        return "\x1b[96m";
    if (mode & 0111)
        return "\x1b[92m";
    return "";
}

static void print_entry(const char *dir, const char *name, bool lng)
{
    char path[AEGIS_PATH_MAX], target[AEGIS_PATH_MAX], perms[11], un[32], gn[32];
    struct aegis_stat st;

    join(path, sizeof(path), dir, name);
    if (lstat(path, &st) < 0) {
        fail("ls", path);
        return;
    }
    if (!lng) {
        printf("%s%s\x1b[0m\n", color_for(st.mode), name);
        return;
    }

    int y, mo, d, h, mi;
    int64_t t = st.mtime, days = t / 86400;
    int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    mo = mp < 10 ? mp + 3 : mp - 9;
    y = yoe + era * 400 + (mo <= 2);
    h = t % 86400 / 3600;
    mi = t % 3600 / 60;

    mode_string(st.mode, perms);
    printf("%s %3u %-8s %-8s %9lu %04d-%02d-%02d %02d:%02d %s%s\x1b[0m", perms, st.nlink,
           uid_to_name(st.uid, un, sizeof(un)), gid_to_name(st.gid, gn, sizeof(gn)), st.size,
           y, mo, d, h, mi, color_for(st.mode), name);
    if (S_ISLNK(st.mode)) {
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n >= 0) {
            target[n] = '\0';
            printf(" -> %s", target);
        }
    }
    printf("\n");
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void sort_names(char **names, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        char *key = names[i];
        size_t j = i;

        while (j > 0 && name_cmp(&names[j - 1], &key) > 0) {
            names[j] = names[j - 1];
            j--;
        }
        names[j] = key;
    }
}

static int list_dir(const char *path, bool all, bool lng)
{
    struct dir_stream *d = opendir(path);
    struct aegis_dirent *e;
    char **names = NULL;
    size_t n = 0, cap = 0;

    if (!d) {
        fail("ls", path);
        return 1;
    }
    while ((e = readdir(d))) {
        if (e->name[0] == '.' && !all)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, cap * sizeof(char *));
        }
        names[n++] = strdup(e->name);
    }
    closedir(d);
    sort_names(names, n);
    for (size_t i = 0; i < n; i++) {
        print_entry(path, names[i], lng);
        free(names[i]);
    }
    free(names);
    return 0;
}

int cmd_ls(int argc, char **argv)
{
    int first = 1, ret = 0;
    bool all = flag(argc, argv, &first, 'a'), lng = flag(argc, argv, &first, 'l');
    struct aegis_stat st;

    if (first >= argc)
        return list_dir(".", all, lng);
    for (int i = first; i < argc; i++) {
        if (stat(argv[i], &st) < 0) {
            fail("ls", argv[i]);
            ret = 1;
            continue;
        }
        if (S_ISDIR(st.mode)) {
            if (argc - first > 1)
                printf("%s:\n", argv[i]);
            ret |= list_dir(argv[i], all, lng);
        } else {
            print_entry(".", argv[i], lng);
        }
    }
    return ret;
}

static int copy_fd(int in, int out)
{
    char buf[8192];
    ssize_t n;

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n)
            return -1;
    }
    return n < 0 ? -1 : 0;
}

int cmd_cat(int argc, char **argv)
{
    int ret = 0;

    if (argc < 2)
        return copy_fd(STDIN_FILENO, STDOUT_FILENO) < 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);

        if (fd < 0 || copy_fd(fd, STDOUT_FILENO) < 0) {
            fail("cat", argv[i]);
            ret = 1;
        }
        if (fd >= 0)
            close(fd);
    }
    return ret;
}

static int mkdir_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    return mkdir(path, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

int cmd_mkdir(int argc, char **argv)
{
    int first = 1, ret = 0;
    bool parents = flag(argc, argv, &first, 'p');

    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: mkdir [-p] DIR...\n");
        return 1;
    }
    for (int i = first; i < argc; i++) {
        if ((parents ? mkdir_parents(argv[i]) : mkdir(argv[i], 0755)) < 0) {
            fail("mkdir", argv[i]);
            ret = 1;
        }
    }
    return ret;
}

int cmd_rmdir(int argc, char **argv)
{
    int ret = 0;

    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            fail("rmdir", argv[i]);
            ret = 1;
        }
    }
    return ret;
}

static int remove_tree(const char *path)
{
    struct aegis_stat st;
    struct dir_stream *d;
    struct aegis_dirent *e;
    char child[AEGIS_PATH_MAX];
    int ret = 0;

    if (lstat(path, &st) < 0)
        return -1;
    if (!S_ISDIR(st.mode))
        return unlink(path);
    if (!(d = opendir(path)))
        return -1;
    while ((e = readdir(d))) {
        if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
            continue;
        join(child, sizeof(child), path, e->name);
        if (remove_tree(child) < 0)
            ret = -1;
    }
    closedir(d);
    return rmdir(path) < 0 ? -1 : ret;
}

int cmd_rm(int argc, char **argv)
{
    int first = 1, ret = 0;
    bool recursive = flag(argc, argv, &first, 'r'), force = flag(argc, argv, &first, 'f');

    for (int i = first; i < argc; i++) {
        int r = recursive ? remove_tree(argv[i]) : unlink(argv[i]);

        if (r < 0 && !(force && errno == ENOENT)) {
            fail("rm", argv[i]);
            ret = 1;
        }
    }
    return ret;
}

static int copy_file(const char *src, const char *dst, uint32_t mode)
{
    int in = open(src, O_RDONLY), out;
    int ret;

    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777);
    if (out < 0) {
        close(in);
        return -1;
    }
    ret = copy_fd(in, out);
    close(in);
    close(out);
    return ret;
}

static int copy_tree(const char *src, const char *dst, bool recursive)
{
    struct aegis_stat st;
    char s[AEGIS_PATH_MAX], d[AEGIS_PATH_MAX], target[AEGIS_PATH_MAX];

    if (lstat(src, &st) < 0)
        return -1;
    if (S_ISLNK(st.mode)) {
        ssize_t n = readlink(src, target, sizeof(target) - 1);
        if (n < 0)
            return -1;
        target[n] = '\0';
        return symlink(target, dst);
    }
    if (!S_ISDIR(st.mode))
        return copy_file(src, dst, st.mode);
    if (!recursive) {
        errno = EISDIR;
        return -1;
    }
    if (mkdir(dst, st.mode & 07777) < 0 && errno != EEXIST)
        return -1;

    struct dir_stream *dir = opendir(src);
    struct aegis_dirent *e;
    int ret = 0;

    if (!dir)
        return -1;
    while ((e = readdir(dir))) {
        if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
            continue;
        join(s, sizeof(s), src, e->name);
        join(d, sizeof(d), dst, e->name);
        if (copy_tree(s, d, true) < 0) {
            fail("cp", s);
            ret = -1;
        }
    }
    closedir(dir);
    return ret;
}

typedef int (*pair_fn)(const char *src, const char *dst, bool recursive);

static int over_sources(int argc, char **argv, int first, const char *name, pair_fn fn, bool recursive)
{
    const char *dest = argv[argc - 1];
    struct aegis_stat st;
    bool dest_dir = stat(dest, &st) == 0 && S_ISDIR(st.mode);
    char target[AEGIS_PATH_MAX];
    int ret = 0;

    if (argc - first < 2) {
        dprintf(STDERR_FILENO, "%s: missing destination\n", name);
        return 1;
    }
    if (argc - first > 2 && !dest_dir) {
        dprintf(STDERR_FILENO, "%s: %s is not a directory\n", name, dest);
        return 1;
    }
    for (int i = first; i < argc - 1; i++) {
        if (dest_dir)
            join(target, sizeof(target), dest, basename_of(argv[i]));
        else
            strlcpy(target, dest, sizeof(target));
        if (fn(argv[i], target, recursive) < 0) {
            fail(name, argv[i]);
            ret = 1;
        }
    }
    return ret;
}

int cmd_cp(int argc, char **argv)
{
    int first = 1;
    bool recursive = flag(argc, argv, &first, 'r');

    return over_sources(argc, argv, first, "cp", copy_tree, recursive);
}

static int move(const char *src, const char *dst, bool unused)
{
    (void)unused;
    if (rename(src, dst) == 0)
        return 0;
    if (errno != EXDEV)
        return -1;
    if (copy_tree(src, dst, true) < 0)
        return -1;
    return remove_tree(src);
}

int cmd_mv(int argc, char **argv)
{
    return over_sources(argc, argv, 1, "mv", move, false);
}

int cmd_touch(int argc, char **argv)
{
    int ret = 0;
    int64_t now = time(NULL);

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);

        if (fd < 0) {
            fail("touch", argv[i]);
            ret = 1;
            continue;
        }
        close(fd);
        utime(argv[i], now, now);
    }
    return ret;
}

int cmd_chmod(int argc, char **argv)
{
    char *end;
    uint32_t mode;
    int ret = 0;

    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: chmod MODE PATH...\n");
        return 1;
    }
    mode = strtoul(argv[1], &end, 8);
    if (*end || mode > 07777) {
        dprintf(STDERR_FILENO, "chmod: invalid mode: %s\n", argv[1]);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], mode) < 0) {
            fail("chmod", argv[i]);
            ret = 1;
        }
    }
    return ret;
}

int cmd_chown(int argc, char **argv)
{
    uint32_t uid = (uint32_t)-1, gid = (uint32_t)-1;
    char spec[64], *colon;
    int ret = 0;

    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: chown USER[:GROUP] PATH...\n");
        return 1;
    }
    strlcpy(spec, argv[1], sizeof(spec));
    colon = strchr(spec, ':');
    if (colon)
        *colon++ = '\0';
    if (spec[0] && name_to_uid(spec, &uid) < 0) {
        dprintf(STDERR_FILENO, "chown: unknown user: %s\n", spec);
        return 1;
    }
    if (colon && *colon && name_to_gid(colon, &gid) < 0) {
        dprintf(STDERR_FILENO, "chown: unknown group: %s\n", colon);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], uid, gid) < 0) {
            fail("chown", argv[i]);
            ret = 1;
        }
    }
    return ret;
}

int cmd_ln(int argc, char **argv)
{
    int first = 1;
    bool sym = flag(argc, argv, &first, 's');

    if (argc - first != 2) {
        dprintf(STDERR_FILENO, "usage: ln [-s] TARGET LINK\n");
        return 1;
    }
    if ((sym ? symlink(argv[first], argv[first + 1]) : link(argv[first], argv[first + 1])) < 0) {
        fail("ln", argv[first + 1]);
        return 1;
    }
    return 0;
}

int cmd_stat(int argc, char **argv)
{
    struct aegis_stat st;
    char perms[11], un[32], gn[32];
    int ret = 0;

    for (int i = 1; i < argc; i++) {
        if (lstat(argv[i], &st) < 0) {
            fail("stat", argv[i]);
            ret = 1;
            continue;
        }
        mode_string(st.mode, perms);
        printf("  File: %s\n  Size: %lu  Blocks: %lu  Links: %u  Inode: %lu  Device: %lu\n"
               "Access: (%04o/%s)  Uid: %u (%s)  Gid: %u (%s)\n"
               "Modify: %ld  Change: %ld  Access: %ld\n",
               argv[i], st.size, st.blocks, st.nlink, st.ino, st.dev, st.mode & 07777, perms,
               st.uid, uid_to_name(st.uid, un, sizeof(un)), st.gid, gid_to_name(st.gid, gn, sizeof(gn)),
               st.mtime, st.ctime, st.atime);
    }
    return ret;
}

int cmd_df(int argc, char **argv)
{
    static char *defaults[] = { "/", "/boot" };
    char **paths = argc > 1 ? argv + 1 : defaults;
    int n = argc > 1 ? argc - 1 : 2;

    printf("%-10s %-6s %10s %10s %10s %5s\n", "Mounted", "Type", "Size", "Used", "Free", "Use%");
    for (int i = 0; i < n; i++) {
        struct aegis_statfs st;

        if (statfs(paths[i], &st) < 0)
            continue;
        uint64_t size = st.blocks * st.block_size >> 10, free_kb = st.blocks_free * st.block_size >> 10;
        printf("%-10s %-6s %9luK %9luK %9luK %4lu%%\n", paths[i], st.fstype, size, size - free_kb,
               free_kb, size ? (size - free_kb) * 100 / size : 0);
    }
    return 0;
}

int cmd_sync(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (sync() < 0) {
        perror("sync");
        return 1;
    }
    return 0;
}
