#include "aegis.h"
#include "bearssl.h"

// Account administration: password hashes, /msc/passwd, /msc/shadow and
// /msc/group. Changing these needs root.
//
// Hash format (checked by the kernel): $aegis-sha256$ITER$SALT_HEX$HASH_HEX,
// H1 = SHA256(salt || password), Hn = SHA256(Hn-1 || salt || password).

#define ITERATIONS 10000

static void hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int unhex(const char *s, uint8_t *out, size_t max)
{
    size_t len = strlen(s);

    if (len % 2 || len / 2 > max)
        return -1;
    for (size_t i = 0; i < len / 2; i++) {
        char b[3] = { s[2 * i], s[2 * i + 1], 0 };
        char *end;

        out[i] = strtoul(b, &end, 16);
        if (*end)
            return -1;
    }
    return len / 2;
}

static void digest(const uint8_t *salt, size_t saltlen, const char *pw, uint32_t iterations, uint8_t h[32])
{
    br_sha256_context c;

    br_sha256_init(&c);
    br_sha256_update(&c, salt, saltlen);
    br_sha256_update(&c, pw, strlen(pw));
    br_sha256_out(&c, h);
    for (uint32_t i = 1; i < iterations; i++) {
        br_sha256_init(&c);
        br_sha256_update(&c, h, 32);
        br_sha256_update(&c, salt, saltlen);
        br_sha256_update(&c, pw, strlen(pw));
        br_sha256_out(&c, h);
    }
}

int password_hash(const char *password, char *out, size_t size)
{
    uint8_t salt[16], h[32];
    char salt_hex[33], h_hex[65];
    int fd = open("/osystem/devices/urandom", O_RDONLY);

    if (fd < 0 || read(fd, salt, sizeof(salt)) != sizeof(salt)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    digest(salt, sizeof(salt), password, ITERATIONS, h);
    hex(salt, sizeof(salt), salt_hex);
    hex(h, sizeof(h), h_hex);
    snprintf(out, size, "$aegis-sha256$%d$%s$%s", ITERATIONS, salt_hex, h_hex);
    memset(h, 0, sizeof(h));
    return 0;
}

bool password_matches(const char *hash, const char *password)
{
    char copy[256], *f[4], *p;
    uint8_t salt[64], want[32], h[32], diff = 0;
    int saltlen, n = 0;
    uint32_t iterations;

    if (strlcpy(copy, hash, sizeof(copy)) >= sizeof(copy) || copy[0] != '$')
        return false;
    for (p = copy + 1; n < 4;) {
        f[n++] = p;
        if (!(p = strchr(p, '$')))
            break;
        *p++ = 0;
    }
    if (n != 4 || strcmp(f[0], "aegis-sha256"))
        return false;
    iterations = strtoul(f[1], NULL, 10);
    saltlen = unhex(f[2], salt, sizeof(salt));
    if (!iterations || iterations > 1000000 || saltlen < 0 || unhex(f[3], want, sizeof(want)) != 32)
        return false;
    digest(salt, saltlen, password, iterations, h);
    for (int i = 0; i < 32; i++)
        diff |= h[i] ^ want[i];
    return diff == 0;
}

// ---- Rewriting the account files ----

// Rewrites path line by line: edit() returns the new line (or NULL to drop
// it); extra, if set, is appended. The file keeps its mode.
static int rewrite(const char *path, const char *(*edit)(const char *line, void *ctx), void *ctx,
                   const char *extra)
{
    char tmp[64], line[1024];
    struct aegis_stat st;
    int in, out;

    if (stat(path, &st) < 0 || (in = open(path, O_RDONLY)) < 0)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, st.mode & 0777)) < 0) {
        close(in);
        return -1;
    }
    while (read_line(in, line, sizeof(line)) >= 0) {
        const char *r = edit ? edit(line, ctx) : line;

        if (r)
            dprintf(out, "%s\n", r);
    }
    if (extra)
        dprintf(out, "%s\n", extra);
    close(in);
    chmod(tmp, st.mode & 0777);
    chown(tmp, st.uid, st.gid);
    close(out);
    sync();
    return rename(tmp, path);
}

struct field_edit {
    const char *name;
    int field;                      // which field to replace
    const char *value;
    char buf[1024];
    bool drop, found;
};

static const char *edit_field(const char *line, void *ctx)
{
    struct field_edit *e = ctx;
    size_t n = strlen(e->name);
    const char *p = line;
    int f = 0;

    if (strncmp(line, e->name, n) || line[n] != ':')
        return line;
    e->found = true;
    if (e->drop)
        return NULL;
    // Copy fields, swapping the chosen one.
    e->buf[0] = 0;
    while (true) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);

        if (f)
            strlcat(e->buf, ":", sizeof(e->buf));
        if (f == e->field)
            strlcat(e->buf, e->value, sizeof(e->buf));
        else {
            char part[512];

            snprintf(part, sizeof(part), "%.*s", (int)len, p);
            strlcat(e->buf, part, sizeof(e->buf));
        }
        if (!colon)
            break;
        p = colon + 1;
        f++;
    }
    return e->buf;
}

static int set_field(const char *path, const char *name, int field, const char *value)
{
    struct field_edit e = { name, field, value, { 0 }, false, false };

    if (rewrite(path, edit_field, &e, NULL) < 0)
        return -1;
    if (!e.found) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int account_set_password(const char *name, const char *password)
{
    char hash[160];

    if (password_hash(password, hash, sizeof(hash)) < 0)
        return -1;
    return set_field("/msc/shadow", name, 1, hash);
}

int account_check_password(const char *name, const char *password)
{
    int fd = open("/msc/shadow", O_RDONLY);
    char line[512];
    size_t n = strlen(name);
    bool ok = false;

    if (fd < 0)
        return -1;
    while (read_line(fd, line, sizeof(line)) >= 0) {
        if (!strncmp(line, name, n) && line[n] == ':') {
            char *end = strchr(line + n + 1, ':');

            if (end)
                *end = 0;
            ok = password_matches(line + n + 1, password);
            break;
        }
    }
    close(fd);
    return ok ? 0 : -1;
}

int account_set_display_name(const char *name, const char *display)
{
    return set_field("/msc/passwd", name, 4, display);
}

// Adds or removes name from a group's member list.
struct member_edit {
    const char *group, *user;
    bool add;
    char buf[1024];
};

static const char *edit_member(const char *line, void *ctx)
{
    struct member_edit *m = ctx;
    size_t gl = strlen(m->group);
    const char *members;
    char out[1024] = "";
    bool present = false;

    if (strncmp(line, m->group, gl) || line[gl] != ':')
        return line;
    // members are the fourth field
    members = line;
    for (int i = 0; i < 3 && members; i++) {
        members = strchr(members, ':');
        if (members)
            members++;
    }
    if (!members)
        return line;
    snprintf(m->buf, sizeof(m->buf), "%.*s", (int)(members - line), line);
    for (const char *p = members; *p;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        bool me = len == strlen(m->user) && !strncmp(p, m->user, len);

        present |= me;
        if (len && !(me && !m->add)) {
            if (*out)
                strlcat(out, ",", sizeof(out));
            strncat(out, p, len);
        }
        if (!comma)
            break;
        p = comma + 1;
    }
    if (m->add && !present) {
        if (*out)
            strlcat(out, ",", sizeof(out));
        strlcat(out, m->user, sizeof(out));
    }
    strlcat(m->buf, out, sizeof(m->buf));
    return m->buf;
}

int group_set_member(const char *group, const char *user, bool member)
{
    struct member_edit m = { group, user, member, { 0 } };

    return rewrite("/msc/group", edit_member, &m, NULL);
}

static bool valid_name(const char *name)
{
    if (!*name || strlen(name) > 31 || !islower((unsigned char)name[0]))
        return false;
    for (const char *p = name; *p; p++)
        if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '-' && *p != '_')
            return false;
    return true;
}

static uint32_t next_uid(void)
{
    struct user_info users[128];
    int n = user_list(users, 128);
    uint32_t uid = 1000;

    for (int i = 0; i < n; i++)
        if (users[i].uid >= uid)
            uid = users[i].uid + 1;
    return uid;
}

int account_add(const char *name, const char *display, const char *password, bool admin)
{
    char hash[160];

    if (password_hash(password, hash, sizeof(hash)) < 0)
        return -1;
    return account_add_hashed(name, display, hash, admin);
}

// As account_add, with the password already hashed (the installer's
// first-boot settings).
int account_add_hashed(const char *name, const char *display, const char *hash, bool admin)
{
    struct user_info u;
    char line[512];
    uint32_t uid;

    if (!valid_name(name)) {
        errno = EINVAL;
        return -1;
    }
    if (user_by_name(name, &u) == 0) {
        errno = EEXIST;
        return -1;
    }
    uid = next_uid();
    snprintf(line, sizeof(line), "%s:x:%u:%u:%s:/userfiles/%s/home:/sysapps/terminal", name, uid, uid,
             display && *display ? display : name, name);
    if (rewrite("/msc/passwd", NULL, NULL, line) < 0)
        return -1;
    snprintf(line, sizeof(line), "%s:x:%u:%s", name, uid, name);
    if (rewrite("/msc/group", NULL, NULL, line) < 0)
        return -1;
    snprintf(line, sizeof(line), "%s:%s:", name, hash);
    if (rewrite("/msc/shadow", NULL, NULL, line) < 0)
        return -1;
    group_set_member("audio", name, true);
    group_set_member("video", name, true);
    if (admin) {
        group_set_member("sudo", name, true);
        group_set_member("adm", name, true);
    }
    if (user_by_name(name, &u) == 0) {
        user_setup_dirs(&u);
        user_setting_set(&u, "name", display && *display ? display : name);
        user_setting_set(&u, "theme", "light");
        user_setting_set(&u, "language", "en");
        user_setting_set(&u, "background", "default");
        // Settings files belong to the user.
        {
            static const char *const keys[] = { "name", "theme", "language", "background" };

            for (int i = 0; i < 4; i++) {
                char p[256];

                snprintf(p, sizeof(p), "%s/system/settings/%s", u.dir, keys[i]);
                chown(p, u.uid, u.gid);
            }
        }
    }
    return 0;
}

static const char *drop_line(const char *line, void *ctx)
{
    const char *name = ctx;
    size_t n = strlen(name);

    return !strncmp(line, name, n) && line[n] == ':' ? NULL : line;
}

int account_remove(const char *name, bool remove_files)
{
    struct user_info u;
    bool have = user_by_name(name, &u) == 0;

    if (!have || u.uid < 1000) {
        errno = have ? EPERM : ENOENT;
        return -1;
    }
    group_set_member("sudo", name, false);
    group_set_member("adm", name, false);
    group_set_member("audio", name, false);
    group_set_member("video", name, false);
    if (rewrite("/msc/passwd", drop_line, (void *)name, NULL) < 0
        || rewrite("/msc/shadow", drop_line, (void *)name, NULL) < 0
        || rewrite("/msc/group", drop_line, (void *)name, NULL) < 0)
        return -1;
    if (remove_files)
        remove_path(u.dir);
    return 0;
}

bool account_is_admin(const char *name)
{
    return user_in_group(name, "sudo");
}

// Changes the calling user's password through the privilege helper.
int change_own_password(const char *old_password, const char *new_password, char *error, size_t size)
{
    char req[600], rep[200];
    int fd = unix_connect(PRIVD_SOCKET, SOCK_SEQPACKET);
    ssize_t n;

    if (fd < 0) {
        snprintf(error, size, "The password service is not running.");
        return -1;
    }
    snprintf(req, sizeof(req), "passwd\n%s\n%s", old_password, new_password);
    n = send(fd, req, strlen(req), MSG_NOSIGNAL);
    memset(req, 0, sizeof(req));
    if (n < 0 || (n = recv(fd, rep, sizeof(rep) - 1, 0)) <= 0) {
        close(fd);
        snprintf(error, size, "The password service did not answer.");
        return -1;
    }
    close(fd);
    rep[n] = 0;
    if (!strcmp(rep, "ok"))
        return 0;
    snprintf(error, size, "%s", !strncmp(rep, "error ", 6) ? rep + 6 : rep);
    return -1;
}

int verify_own_password(const char *password)
{
    char req[400], rep[200];
    int fd = unix_connect(PRIVD_SOCKET, SOCK_SEQPACKET);
    ssize_t n;

    if (fd < 0)
        return -1;
    snprintf(req, sizeof(req), "verify\n%s", password);
    n = send(fd, req, strlen(req), MSG_NOSIGNAL);
    memset(req, 0, sizeof(req));
    if (n < 0 || (n = recv(fd, rep, sizeof(rep) - 1, 0)) <= 0) {
        close(fd);
        return -1;
    }
    close(fd);
    rep[n] = 0;
    return strcmp(rep, "ok") ? -1 : 0;
}
