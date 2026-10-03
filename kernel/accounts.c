#include "accounts.h"
#include "mem.h"
#include "sha256.h"
#include "string.h"

#define SUDO_GROUP  "admins"
#define FAIL_DELAY  1000

// The account files are Aegis record files (see endpoint/lib/records.c):
//
//   aegis accounts 1
//   account alex               a block: kind and name
//       id: 1000               its settings, indented
//
// SECRETS_PATH holds "name: hash" lines outside any block.

#define ACCOUNTS_PATH   "/msc/accounts.aacc"
#define SECRETS_PATH    "/msc/secrets.aacc"
#define MAX_FIELDS      8

struct block {
    const char *kind, *name;
    int n;
    const char *key[MAX_FIELDS], *val[MAX_FIELDS];
};

static uint32_t to_uint(const char *s)
{
    uint32_t v = 0;

    while (s && *s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static void copy_field(char *dst, size_t size, const char *src)
{
    size_t n = src ? strnlen(src, size - 1) : 0;

    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *field(const struct block *b, const char *key)
{
    for (int i = 0; i < b->n; i++)
        if (!strcmp(b->key[i], key))
            return b->val[i];
    return NULL;
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

// Splits "key: value" (or "key:") in place; false if it is not a setting.
static bool split_setting(char *s, char **key, char **val)
{
    size_t len = strlen(s);
    char *colon = NULL;

    for (char *p = s; *p; p++)
        if (*p == ':' && (p[1] == ' ' || !p[1])) {
            colon = p;
            break;
        }
    if (!colon || !len)
        return false;
    *colon = '\0';
    *key = trim(s);
    *val = trim(colon + 1);
    return true;
}

typedef bool (*block_fn)(const struct block *b, void *ctx);

// Calls fn for each block, and for each setting outside any block (as a
// block of kind "" with that one setting), until it returns true. 0 if it did.
static int each_block(const char *path, block_fn fn, void *ctx)
{
    char *data, *line;
    struct block cur = { "", "", 0, { 0 }, { 0 } };
    bool first = true, in_block = false, found = false;
    int ret;

    if ((ret = vfs_read_file(path, &data, NULL)))
        return ret;
    line = data;
    while (*line && !found) {
        char *end = line, *s, *k, *v;
        bool indented = *line == ' ' || *line == '\t', last;

        while (*end && *end != '\n')
            end++;
        last = !*end;
        *end = '\0';
        s = trim(line);
        if (first) {
            first = false;
            if (strncmp(s, "aegis ", 6))
                break;
        } else if (*s && *s != '#') {
            if (!indented && split_setting(s, &k, &v)) {
                struct block one = { "", "", 1, { k }, { v } };

                if (fn(&one, ctx))
                    found = true;
            } else if (!indented) {
                char *sp = s;

                if (in_block && fn(&cur, ctx))
                    found = true;
                while (*sp && *sp != ' ')
                    sp++;
                if (*sp)
                    *sp++ = '\0';
                cur = (struct block){ s, trim(sp), 0, { 0 }, { 0 } };
                in_block = true;
            } else if (in_block && cur.n < MAX_FIELDS && split_setting(s, &k, &v)) {
                cur.key[cur.n] = k;
                cur.val[cur.n++] = v;
            }
        }
        if (last)
            break;
        line = end + 1;
    }
    if (!found && in_block && fn(&cur, ctx))
        found = true;
    kfree(data);
    return found ? 0 : -ENOENT;
}

struct account_query {
    const char *name;
    uint32_t uid;
    struct account *out;
};

static bool account_match(const struct block *b, void *arg)
{
    struct account_query *q = arg;
    const char *id = field(b, "id");

    if (strcmp(b->kind, "account") || !id || (q->name ? strcmp(b->name, q->name) : to_uint(id) != q->uid))
        return false;
    copy_field(q->out->name, sizeof(q->out->name), b->name);
    q->out->uid = to_uint(id);
    q->out->gid = field(b, "group") ? to_uint(field(b, "group")) : q->out->uid;
    copy_field(q->out->home, sizeof(q->out->home), field(b, "home"));
    copy_field(q->out->shell, sizeof(q->out->shell), field(b, "terminal"));
    return true;
}

int account_by_name(const char *name, struct account *out)
{
    struct account_query q = { name, 0, out };

    return each_block(ACCOUNTS_PATH, account_match, &q);
}

int account_by_uid(uint32_t uid, struct account *out)
{
    struct account_query q = { NULL, uid, out };

    return each_block(ACCOUNTS_PATH, account_match, &q);
}

struct group_query {
    const char *user;
    struct cred *cred;
    const char *want;
    bool member;
};

// Is user in "alex, user"?
static bool listed(const char *members, const char *user)
{
    size_t n = strlen(user);

    for (const char *p = members; p && *p;) {
        const char *end = p;
        size_t len;

        while (*p == ' ')
            p++;
        end = p;
        while (*end && *end != ',')
            end++;
        len = end - p;
        while (len && p[len - 1] == ' ')
            len--;
        if (len == n && !strncmp(p, user, n))
            return true;
        p = *end ? end + 1 : NULL;
    }
    return false;
}

static bool group_scan(const struct block *b, void *arg)
{
    struct group_query *q = arg;

    if (strcmp(b->kind, "group") || !listed(field(b, "members"), q->user))
        return false;
    if (q->want) {
        if (!strcmp(b->name, q->want))
            q->member = true;
        return q->member;
    }
    if (q->cred->ngroups < NGROUPS_MAX)
        q->cred->groups[q->cred->ngroups++] = to_uint(field(b, "id"));
    return false;
}

static bool in_group(const char *user, const char *group)
{
    struct group_query q = { user, NULL, group, false };

    each_block(ACCOUNTS_PATH, group_scan, &q);
    return q.member;
}

struct secret_query {
    const char *name;
    char hash[256];
};

static bool secret_match(const struct block *b, void *arg)
{
    struct secret_query *q = arg;
    const char *h;

    if (*b->kind || !(h = field(b, q->name)))
        return false;
    copy_field(q->hash, sizeof(q->hash), h);
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
    struct secret_query q = { name, { 0 } };
    char *fields[5], *s;
    uint8_t salt[64], want[32], h[32];
    int saltlen, n;
    uint32_t iterations;
    uint8_t diff = 0;
    struct sha256 ctx;

    if (each_block(SECRETS_PATH, secret_match, &q))
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
    each_block(ACCOUNTS_PATH, group_scan, &q);
    p->cred = c;
    return 0;
}

// The superuser becomes another account without its password (services that run
// jobs for users). Real and effective ids and groups all change.
int account_become(struct process *p, uint32_t uid)
{
    struct account a;
    struct cred c = { 0 };
    struct group_query q;

    if (p->cred.euid != 0)
        return -EPERM;
    if (account_by_uid(uid, &a))
        return -ENOENT;
    c.uid = c.euid = a.uid;
    c.gid = c.egid = a.gid;
    q = (struct group_query){ a.name, &c, NULL, false };
    each_block(ACCOUNTS_PATH, group_scan, &q);
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
