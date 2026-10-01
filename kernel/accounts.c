#include "accounts.h"
#include "mem.h"
#include "sha256.h"
#include "string.h"

#define SUDO_GROUP  "sudo"
#define FAIL_DELAY  1000

// Splits `line` at ':' in place, keeping pointers to the first `max` fields.
static int split(char *line, char **fields, int max)
{
    int n = 0;

    fields[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p == ':') {
            *p = '\0';
            if (n < max)
                fields[n++] = p + 1;
        }
    }
    return n;
}

static uint32_t to_uint(const char *s)
{
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static void copy_field(char *dst, size_t size, const char *src)
{
    size_t n = strnlen(src, size - 1);

    memcpy(dst, src, n);
    dst[n] = '\0';
}

typedef bool (*line_fn)(char **fields, int n, void *ctx);

static int each_line(const char *path, int nfields, line_fn fn, void *ctx)
{
    char *data, *line;
    int ret;

    if ((ret = vfs_read_file(path, &data, NULL)))
        return ret;
    ret = -ENOENT;
    line = data;
    while (*line) {
        char *end = line, *fields[8];

        while (*end && *end != '\n')
            end++;
        bool last = !*end;
        *end = '\0';
        if (*line && *line != '#' && fn(fields, split(line, fields, nfields), ctx)) {
            ret = 0;
            break;
        }
        if (last)
            break;
        line = end + 1;
    }
    kfree(data);
    return ret;
}

struct account_query {
    const char *name;
    uint32_t uid;
    struct account *out;
};

static bool passwd_match(char **f, int n, void *arg)
{
    struct account_query *q = arg;

    if (n < 7 || (q->name ? strcmp(f[0], q->name) : to_uint(f[2]) != q->uid))
        return false;
    copy_field(q->out->name, sizeof(q->out->name), f[0]);
    q->out->uid = to_uint(f[2]);
    q->out->gid = to_uint(f[3]);
    copy_field(q->out->home, sizeof(q->out->home), f[5]);
    copy_field(q->out->shell, sizeof(q->out->shell), f[6]);
    return true;
}

int account_by_name(const char *name, struct account *out)
{
    struct account_query q = { name, 0, out };

    return each_line("/etc/passwd", 7, passwd_match, &q);
}

int account_by_uid(uint32_t uid, struct account *out)
{
    struct account_query q = { NULL, uid, out };

    return each_line("/etc/passwd", 7, passwd_match, &q);
}

struct group_query {
    const char *user;
    struct cred *cred;
    const char *want;
    bool member;
};

static bool listed(char *members, const char *user)
{
    char *p = members;

    while (*p) {
        char *end = p;

        while (*end && *end != ',')
            end++;
        if ((size_t)(end - p) == strlen(user) && !strncmp(p, user, end - p))
            return true;
        if (!*end)
            break;
        p = end + 1;
    }
    return false;
}

static bool group_scan(char **f, int n, void *arg)
{
    struct group_query *q = arg;

    if (n < 4 || !listed(f[3], q->user))
        return false;
    if (q->want) {
        if (!strcmp(f[0], q->want))
            q->member = true;
        return q->member;
    }
    if (q->cred->ngroups < NGROUPS_MAX)
        q->cred->groups[q->cred->ngroups++] = to_uint(f[2]);
    return false;
}

static bool in_group(const char *user, const char *group)
{
    struct group_query q = { user, NULL, group, false };

    each_line("/etc/group", 4, group_scan, &q);
    return q.member;
}

struct shadow_query {
    const char *name;
    char hash[256];
};

static bool shadow_match(char **f, int n, void *arg)
{
    struct shadow_query *q = arg;

    if (n < 2 || strcmp(f[0], q->name))
        return false;
    copy_field(q->hash, sizeof(q->hash), f[1]);
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static int unhex(const char *s, size_t len, uint8_t *out, size_t max)
{
    if (len % 2 || len / 2 > max)
        return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = hi << 4 | lo;
    }
    return len / 2;
}

// Format: $aegis-sha256$ITERATIONS$SALT_HEX$HASH_HEX, where
// H1 = SHA256(salt || password) and Hn = SHA256(Hn-1 || salt || password).
static bool verify_password(const char *name, const char *password)
{
    struct shadow_query q = { name, { 0 } };
    char *fields[5], *s;
    uint8_t salt[64], want[32], h[32];
    int saltlen, n;
    uint32_t iterations;
    uint8_t diff = 0;
    struct sha256 ctx;

    if (each_line("/etc/shadow", 2, shadow_match, &q))
        return false;
    s = q.hash;
    if (*s != '$')
        return false;
    n = 0;
    for (char *p = s + 1; n < 5; ) {
        fields[n++] = p;
        while (*p && *p != '$')
            p++;
        if (!*p)
            break;
        *p++ = '\0';
    }
    if (n != 4 || strcmp(fields[0], "aegis-sha256"))
        return false;
    iterations = to_uint(fields[1]);
    saltlen = unhex(fields[2], strlen(fields[2]), salt, sizeof(salt));
    if (iterations == 0 || iterations > 1000000 || saltlen < 0
        || unhex(fields[3], strlen(fields[3]), want, sizeof(want)) != 32)
        return false;

    sha256_init(&ctx);
    sha256_update(&ctx, salt, saltlen);
    sha256_update(&ctx, password, strlen(password));
    sha256_final(&ctx, h);
    for (uint32_t i = 1; i < iterations; i++) {
        sha256_init(&ctx);
        sha256_update(&ctx, h, 32);
        sha256_update(&ctx, salt, saltlen);
        sha256_update(&ctx, password, strlen(password));
        sha256_final(&ctx, h);
    }
    for (int i = 0; i < 32; i++)
        diff |= h[i] ^ want[i];
    return diff == 0;
}

int account_login(struct process *p, const char *name, const char *password)
{
    struct account a;
    struct cred c = { 0 };
    struct group_query q;

    if (account_by_name(name, &a) || !verify_password(name, password)) {
        sched_sleep(FAIL_DELAY);
        return -EACCES;
    }
    c.uid = c.euid = a.uid;
    c.gid = c.egid = a.gid;
    q = (struct group_query){ a.name, &c, NULL, false };
    each_line("/etc/group", 4, group_scan, &q);
    p->cred = c;
    return 0;
}

int account_sudo(struct process *p, const char *password)
{
    struct account a;

    if (p->cred.uid == 0) {
        p->cred.euid = p->cred.egid = 0;
        return 0;
    }
    if (account_by_uid(p->cred.uid, &a))
        return -EACCES;
    if (!in_group(a.name, SUDO_GROUP))
        return -EPERM;
    if (!verify_password(a.name, password)) {
        sched_sleep(FAIL_DELAY);
        return -EACCES;
    }
    p->cred.euid = 0;
    p->cred.egid = 0;
    return 0;
}
