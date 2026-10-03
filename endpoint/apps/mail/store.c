#include "mail.h"

// The local copy of the mailbox, in the user's appdata:
//
//   system/appdata/mail/account.conf       the account (no password)
//   system/appdata/mail/folders            folder names, one per line
//   system/appdata/mail/<folder>/index     uid, flags, date, sender, subject
//   system/appdata/mail/<folder>/<uid>.eml each message as it came

static void base_dir(char *out, size_t size)
{
    struct user_info me;

    if (user_current(&me) < 0) {
        strlcpy(out, "/osystem/temp/mail", size);
        return;
    }
    user_path(&me, "system/appdata/mail", out, size);
}

static void ensure_dir(const char *path)
{
    char tmp[512];

    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0700);
            *p = '/';
        }
    mkdir(tmp, 0700);
}

void store_dir(const char *folder, char *out, size_t size)
{
    char base[300], safe[200];
    size_t o = 0;

    base_dir(base, sizeof(base));
    for (const char *p = folder; *p && o + 4 < sizeof(safe); p++) {
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ' ' || (unsigned char)*p >= 0x80)
            safe[o++] = *p;
        else
            o += snprintf(safe + o, sizeof(safe) - o, "%%%02X", (unsigned char)*p);
    }
    safe[o] = 0;
    snprintf(out, size, "%s/%s", base, safe);
}

// ---- The account ----

static void account_path(char *out, size_t size)
{
    char base[300];

    base_dir(base, sizeof(base));
    snprintf(out, size, "%s/account.conf", base);
}

int account_load(struct account *a)
{
    char path[400], line[512];
    int fd;

    memset(a, 0, sizeof(*a));
    a->imap_port = 993;
    a->imap_security = SEC_TLS;
    a->smtp_port = 587;
    a->smtp_security = SEC_STARTTLS;
    account_path(path, sizeof(path));
    if ((fd = open(path, O_RDONLY)) < 0)
        return -1;
    while (read_line(fd, line, sizeof(line)) >= 0) {
        char *eq = strchr(line, '='), *v;

        if (!eq)
            continue;
        *eq = 0;
        v = eq + 1;
        if (!strcmp(line, "name"))
            strlcpy(a->name, v, sizeof(a->name));
        else if (!strcmp(line, "email"))
            strlcpy(a->email, v, sizeof(a->email));
        else if (!strcmp(line, "imap_host"))
            strlcpy(a->imap_host, v, sizeof(a->imap_host));
        else if (!strcmp(line, "imap_port"))
            a->imap_port = atoi(v);
        else if (!strcmp(line, "imap_security"))
            a->imap_security = atoi(v);
        else if (!strcmp(line, "smtp_host"))
            strlcpy(a->smtp_host, v, sizeof(a->smtp_host));
        else if (!strcmp(line, "smtp_port"))
            a->smtp_port = atoi(v);
        else if (!strcmp(line, "smtp_security"))
            a->smtp_security = atoi(v);
        else if (!strcmp(line, "user"))
            strlcpy(a->user, v, sizeof(a->user));
        else if (!strcmp(line, "ca_file"))
            strlcpy(a->ca_file, v, sizeof(a->ca_file));
    }
    close(fd);
    return a->email[0] && a->imap_host[0] ? 0 : -1;
}

int account_save(const struct account *a)
{
    char path[400], base[300];
    int fd;

    base_dir(base, sizeof(base));
    ensure_dir(base);
    account_path(path, sizeof(path));
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
        return -1;
    dprintf(fd, "name=%s\nemail=%s\nimap_host=%s\nimap_port=%d\nimap_security=%d\nsmtp_host=%s\nsmtp_port=%d\n"
                "smtp_security=%d\nuser=%s\nca_file=%s\n",
            a->name, a->email, a->imap_host, a->imap_port, a->imap_security, a->smtp_host, a->smtp_port,
            a->smtp_security, a->user, a->ca_file);
    close(fd);
    return 0;
}

void account_secret_name(const struct account *a, char *out, size_t size)
{
    size_t o = 0;

    o = snprintf(out, size, "mail-");
    for (const char *p = a->user[0] ? a->user : a->email; *p && o + 1 < size; p++)
        out[o++] = isalnum((unsigned char)*p) || *p == '@' || *p == '.' || *p == '-' ? *p : '_';
    out[o] = 0;
}

// ---- Folders ----

int store_folders(char names[][128], int max)
{
    char base[300], path[400], line[256];
    int fd, n = 0;

    base_dir(base, sizeof(base));
    snprintf(path, sizeof(path), "%s/folders", base);
    if ((fd = open(path, O_RDONLY)) < 0)
        return 0;
    while (n < max && read_line(fd, line, sizeof(line)) >= 0)
        if (*line)
            strlcpy(names[n++], line, 128);
    close(fd);
    return n;
}

void store_set_folders(char names[][128], int n)
{
    char base[300], path[400];
    int fd;

    base_dir(base, sizeof(base));
    ensure_dir(base);
    snprintf(path, sizeof(path), "%s/folders", base);
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
        return;
    for (int i = 0; i < n; i++)
        dprintf(fd, "%s\n", names[i]);
    close(fd);
}

// ---- Index ----

static void index_path(const char *folder, char *out, size_t size)
{
    char dir[400];

    store_dir(folder, dir, sizeof(dir));
    snprintf(out, size, "%s/index", dir);
}

static int by_date(const void *a, const void *b)
{
    const struct summary *x = a, *y = b;

    if (x->when != y->when)
        return x->when > y->when ? -1 : 1;
    return x->uid > y->uid ? -1 : 1;
}

static void parse_flags(struct summary *s, const char *flags)
{
    s->seen = strstr(flags, "\\Seen") != NULL;
    s->flagged = strstr(flags, "\\Flagged") != NULL;
    s->answered = strstr(flags, "\\Answered") != NULL;
}

int store_list(const char *folder, struct summary **out)
{
    char path[400], line[1024];
    struct summary *list = NULL;
    int fd, n = 0;

    *out = NULL;
    index_path(folder, path, sizeof(path));
    if ((fd = open(path, O_RDONLY)) < 0)
        return 0;
    while (read_line(fd, line, sizeof(line)) >= 0) {
        char *f[5];
        int k = 0;
        struct summary *m;

        f[k++] = line;
        for (char *p = line; *p && k < 5; p++)
            if (*p == '\t') {
                *p = 0;
                f[k++] = p + 1;
            }
        if (k < 5 || !(m = realloc(list, (n + 1) * sizeof(*m))))
            continue;
        list = m;
        m = &list[n++];
        memset(m, 0, sizeof(*m));
        m->uid = strtoul(f[0], NULL, 10);
        parse_flags(m, f[1]);
        m->when = strtol(f[2], NULL, 10);
        strlcpy(m->from, f[3], sizeof(m->from));
        strlcpy(m->subject, f[4], sizeof(m->subject));
    }
    close(fd);
    qsort(list, n, sizeof(*list), by_date);
    *out = list;
    return n;
}

// Rewrites the index through a filter: keep(uid) says whether a line stays;
// flags (when not NULL) replaces the flags of `uid`.
static void rewrite_index(const char *folder, bool (*keep)(uint32_t, void *), void *u, uint32_t uid,
                          const char *flags)
{
    char path[400], tmp[420], line[1024];
    int in, out;

    index_path(folder, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((in = open(path, O_RDONLY)) < 0)
        return;
    if ((out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) {
        close(in);
        return;
    }
    while (read_line(in, line, sizeof(line)) >= 0) {
        uint32_t id = strtoul(line, NULL, 10);
        char *tab = strchr(line, '\t'), *rest = tab ? strchr(tab + 1, '\t') : NULL;

        if (!tab || !rest || (keep && !keep(id, u)))
            continue;
        if (flags && id == uid)
            dprintf(out, "%u\t%s%s\n", id, flags, rest);
        else
            dprintf(out, "%s\n", line);
    }
    close(in);
    close(out);
    rename(tmp, path);
}

static void sanitize(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

int store_add(const char *folder, uint32_t uid, const char *flags, const char *raw, size_t len)
{
    char dir[400], path[450];
    char *from = mime_header(raw, len, "From"), *subject = mime_header(raw, len, "Subject");
    char *date = mime_header(raw, len, "Date");
    char name[128];
    int fd;

    store_dir(folder, dir, sizeof(dir));
    ensure_dir(dir);
    snprintf(path, sizeof(path), "%s/%u.eml", dir, uid);
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)) >= 0) {
        write(fd, raw, len);
        close(fd);
    }
    mime_display_name(from, name, sizeof(name));
    sanitize(name);
    if (subject)
        sanitize(subject);
    index_path(folder, path, sizeof(path));
    if ((fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600)) >= 0) {
        dprintf(fd, "%u\t%s\t%ld\t%s\t%s\n", uid, flags && *flags ? flags : "-", (long)mime_date(date), name,
                subject && *subject ? subject : "(no subject)");
        close(fd);
    }
    free(from);
    free(subject);
    free(date);
    return 0;
}

bool store_has(const char *folder, uint32_t uid)
{
    char dir[400], path[450];
    struct aegis_stat st;

    store_dir(folder, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%u.eml", dir, uid);
    return stat(path, &st) == 0;
}

char *store_read(const char *folder, uint32_t uid, size_t *len)
{
    char dir[400], path[450];
    struct aegis_stat st;
    char *data;
    int fd;
    size_t got = 0;

    store_dir(folder, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%u.eml", dir, uid);
    if ((fd = open(path, O_RDONLY)) < 0)
        return NULL;
    if (fstat(fd, &st) < 0 || !(data = malloc(st.size + 1))) {
        close(fd);
        return NULL;
    }
    while (got < st.size) {
        ssize_t n = read(fd, data + got, st.size - got);

        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    data[got] = 0;
    *len = got;
    return data;
}

void store_set_flags(const char *folder, uint32_t uid, const char *flags)
{
    rewrite_index(folder, NULL, NULL, uid, flags && *flags ? flags : "-");
}

static bool not_this(uint32_t id, void *u)
{
    return id != *(uint32_t *)u;
}

void store_remove(const char *folder, uint32_t uid)
{
    char dir[400], path[450];

    store_dir(folder, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%u.eml", dir, uid);
    unlink(path);
    rewrite_index(folder, not_this, &uid, 0, NULL);
}

struct keep_list {
    const uint32_t *uids;
    int n;
};

static bool in_list(uint32_t id, void *u)
{
    struct keep_list *k = u;

    for (int i = 0; i < k->n; i++)
        if (k->uids[i] == id)
            return true;
    return false;
}

void store_keep_only(const char *folder, const uint32_t *uids, int n)
{
    struct summary *list;
    int count = store_list(folder, &list);
    struct keep_list k = { uids, n };

    for (int i = 0; i < count; i++)
        if (!in_list(list[i].uid, &k)) {
            char dir[400], path[450];

            store_dir(folder, dir, sizeof(dir));
            snprintf(path, sizeof(path), "%s/%u.eml", dir, list[i].uid);
            unlink(path);
        }
    free(list);
    rewrite_index(folder, in_list, &k, 0, NULL);
}
