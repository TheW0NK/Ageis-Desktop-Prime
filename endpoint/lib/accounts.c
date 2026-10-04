#include "aegis.h"
#include "bearssl.h"

// Account administration: password hashes and the account files,
// ACCOUNTS_FILE (accounts and groups) and SECRETS_FILE (password hashes).
// Changing these needs the superuser.
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
    int fd = open("/osystem/devices/random", O_RDONLY);

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

// ---- The account files ----

static int load_accounts(struct records *r)
{
    if (records_load(ACCOUNTS_FILE, "accounts", r) < 0) {
        records_free(r);
        return -1;
    }
    return 0;
}

static int save_and_free(struct records *r, const char *path, uint32_t mode)
{
    int ret = records_save(r, path, mode);

    records_free(r);
    return ret;
}

int account_set_password(const char *name, const char *password)
{
    char hash[160];
    struct records r;

    if (password_hash(password, hash, sizeof(hash)) < 0)
        return -1;
    if (records_load(SECRETS_FILE, "secrets", &r) < 0 || !rec_get(records_top(&r), name)) {
        records_free(&r);
        errno = ENOENT;
        return -1;
    }
    rec_set(records_top(&r), name, hash);
    return save_and_free(&r, SECRETS_FILE, 0600);
}

int account_check_password(const char *name, const char *password)
{
    struct records r;
    const char *hash;
    bool ok;

    if (records_load(SECRETS_FILE, "secrets", &r) < 0) {
        records_free(&r);
        return -1;
    }
    ok = (hash = rec_get(records_top(&r), name)) && password_matches(hash, password);
    records_free(&r);
    return ok ? 0 : -1;
}

int account_set_display_name(const char *name, const char *display)
{
    struct records r;
    struct rec_block *b;

    if (load_accounts(&r) < 0)
        return -1;
    if (!(b = records_find(&r, "account", name))) {
        records_free(&r);
        errno = ENOENT;
        return -1;
    }
    rec_set(b, "display", display);
    return save_and_free(&r, ACCOUNTS_FILE, 0644);
}

int group_set_member(const char *group, const char *user, bool member)
{
    struct records r;
    struct rec_block *b;

    if (load_accounts(&r) < 0)
        return -1;
    if (!(b = records_find(&r, "group", group))) {
        records_free(&r);
        errno = ENOENT;
        return -1;
    }
    rec_list_edit(b, "members", user, member);
    return save_and_free(&r, ACCOUNTS_FILE, 0644);
}

static bool valid_name(const char *name)
{
    // "guest" is kept for the guest account.
    if (!*name || strlen(name) > 31 || !islower((unsigned char)name[0]) || !strcmp(name, GUEST_NAME))
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

static int add_account(const char *name, const char *display, const char *hash, bool admin, bool guest);

// As account_add, with the password already hashed (the installer's
// first-boot settings).
int account_add_hashed(const char *name, const char *display, const char *hash, bool admin)
{
    return add_account(name, display, hash, admin, false);
}

static int add_account(const char *name, const char *display, const char *hash, bool admin, bool guest)
{
    struct user_info u;
    uint32_t uid;

    if (!guest && !valid_name(name)) {
        errno = EINVAL;
        return -1;
    }
    if (user_by_name(name, &u) == 0) {
        errno = EEXIST;
        return -1;
    }
    uid = next_uid();
    {
        struct records r, sec;
        struct rec_block *acc, *grp;
        char num[16], home[128];
        static const char *const groups[] = { "audio", "video", "admins", "logs" };

        if (load_accounts(&r) < 0)
            return -1;
        snprintf(num, sizeof(num), "%u", uid);
        snprintf(home, sizeof(home), "/userfiles/%s/home", name);
        acc = records_add(&r, "account", name);
        rec_set(acc, "id", num);
        rec_set(acc, "group", num);
        rec_set(acc, "display", display && *display ? display : name);
        rec_set(acc, "home", home);
        rec_set(acc, "terminal", "/sysapps/terminal");
        if (guest)
            rec_set(acc, "guest", "yes");
        grp = records_add(&r, "group", name);
        rec_set(grp, "id", num);
        rec_set(grp, "members", name);
        for (int i = 0; i < (admin ? 4 : 2); i++)
            if ((grp = records_find(&r, "group", groups[i])))
                rec_list_edit(grp, "members", name, true);
        if (save_and_free(&r, ACCOUNTS_FILE, 0644) < 0)
            return -1;
        records_load(SECRETS_FILE, "secrets", &sec);
        rec_set(records_top(&sec), name, hash);
        if (save_and_free(&sec, SECRETS_FILE, 0600) < 0)
            return -1;
    }
    if (user_by_name(name, &u) == 0) {
        user_setup_dirs(&u);
        user_setting_set(&u, "name", display && *display ? display : name);
        user_setting_set(&u, "theme", "light");
        user_setting_set(&u, "language", "en");
        user_setting_set(&u, "background", "default");
    }
    return 0;
}

int account_remove(const char *name, bool remove_files)
{
    struct user_info u;
    struct records r, sec;
    bool have = user_by_name(name, &u) == 0;

    if (!have || u.uid < 1000) {
        errno = have ? EPERM : ENOENT;
        return -1;
    }
    if (load_accounts(&r) < 0)
        return -1;
    records_remove(&r, "account", name);
    records_remove(&r, "group", name);
    for (int i = 1; i < r.n; i++)
        if (!strcmp(r.b[i].kind, "group"))
            rec_list_edit(&r.b[i], "members", name, false);
    if (save_and_free(&r, ACCOUNTS_FILE, 0644) < 0)
        return -1;
    if (records_load(SECRETS_FILE, "secrets", &sec) == 0) {
        rec_unset(records_top(&sec), name);
        if (save_and_free(&sec, SECRETS_FILE, 0600) < 0)
            return -1;
    } else {
        records_free(&sec);
    }
    if (remove_files)
        remove_path(u.dir);
    return 0;
}

bool account_is_admin(const char *name)
{
    return user_in_group(name, "admins");
}

// ---- The guest account ----

// True if the account called "guest" is the guest account (and not one
// made before the name was kept for it).
static bool is_guest_account(void)
{
    struct records r;
    struct rec_block *b;
    bool yes;

    if (load_accounts(&r) < 0)
        return false;
    yes = (b = records_find(&r, "account", GUEST_NAME)) && rec_get(b, "guest") && !strcmp(rec_get(b, "guest"), "yes");
    records_free(&r);
    return yes;
}

int guest_remove(void)
{
    struct user_info u;
    struct aegis_procinfo procs[256];
    int n;

    if (user_by_name(GUEST_NAME, &u) < 0) {
        // Files left behind by a guest whose account is already gone.
        if (access("/userfiles/" GUEST_NAME, 0) == 0)
            remove_path("/userfiles/" GUEST_NAME);
        return 0;
    }
    if (!is_guest_account()) {
        errno = EEXIST;
        return -1;
    }
    // Nothing the guest started keeps running.
    n = procinfo(procs, 256);
    for (int i = 0; i < n; i++)
        if (procs[i].uid == u.uid || procs[i].euid == u.uid)
            kill(procs[i].pid, SIGKILL);
    return account_remove(GUEST_NAME, true);
}

int guest_create(struct user_info *out)
{
    if (guest_remove() < 0)
        return -1;
    // "!" is no password hash: nobody can sign in to it with a password.
    if (add_account(GUEST_NAME, "Guest", "!", false, true) < 0)
        return -1;
    return user_by_name(GUEST_NAME, out);
}

bool user_is_guest(const struct user_info *u)
{
    return !strcmp(u->name, GUEST_NAME) && is_guest_account();
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
