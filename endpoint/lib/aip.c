#include "aegis.h"
#include "bearssl.h"

// .aip, the Aegis installer package: a manifest and the app's files in one
// file, checked with SHA-256, read and installed by the system rather than
// run. Layout (little endian):
//
//   "AEGISAIP"  u32 version (1)  u32 flags  u32 manifest length  u32 files
//   manifest (key=value lines)
//   per file:   u16 path length  u16 0  u32 mode  u64 size  path  data
//   SHA-256 of everything above (32 bytes), then "AIPEND\0\0"
//
// The manifest's exec is the program and its arguments: a path inside the
// package, or an absolute path (a system program such as /bin/apprun),
// with %d standing for the folder the app is installed in.
//
// Installed apps go to /apps/<id> (everyone; needs an administrator) or the
// user's system/appdata/apps/<id>. Each install leaves a record (the
// manifest, the granted permissions, the file list and a copy of the
// package for repair) in /var/lib/aip/<id> or system/appdata/aip/<id>.

#define MAGIC       "AEGISAIP"
#define TRAILER     "AIPEND\0\0"
#define MAX_FILES   4096

struct aip_file_hdr {
    uint16_t path_len, reserved;
    uint32_t mode;
    uint64_t size;
} __attribute__((packed));

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void manifest_get(const char *m, const char *key, char *out, size_t size)
{
    size_t kl = strlen(key);

    out[0] = 0;
    for (const char *line = m; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);

        if (len > kl && !strncmp(line, key, kl) && line[kl] == '=') {
            size_t n = len - kl - 1;

            if (n >= size)
                n = size - 1;
            memcpy(out, line + kl + 1, n);
            out[n] = 0;
            return;
        }
        line = end ? end + 1 : NULL;
    }
}

static bool safe_rel_path(const char *p)
{
    if (!*p || *p == '/')
        return false;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);

        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return false;
        s = e ? e + 1 : s + n;
    }
    return true;
}

static bool valid_id(const char *id)
{
    if (!*id || strlen(id) > 31)
        return false;
    for (; *id; id++)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= '0' && *id <= '9') || *id == '-' || *id == '.'))
            return false;
    return true;
}

void aip_close(struct aip *p)
{
    free(p->data);
    memset(p, 0, sizeof(*p));
}

int aip_open(const char *path, struct aip *p, char *error, size_t esize)
{
    struct aegis_stat st;
    int fd;
    size_t off;
    uint32_t mlen;
    br_sha256_context c;
    uint8_t digest[32];

    memset(p, 0, sizeof(*p));
#define BAD(msg) do { snprintf(error, esize, "%s", msg); aip_close(p); return -1; } while (0)
    if (stat(path, &st) < 0 || (fd = open(path, O_RDONLY)) < 0)
        BAD("The package cannot be opened.");
    if (st.size < 32 + 40 || st.size > (256ULL << 20)) {
        close(fd);
        BAD("This is not an Aegis package, or it is too large.");
    }
    p->size = st.size;
    if (!(p->data = malloc(p->size))) {
        close(fd);
        BAD("Not enough memory to read the package.");
    }
    for (size_t got = 0; got < p->size;) {
        ssize_t n = read(fd, p->data + got, p->size - got);

        if (n <= 0) {
            close(fd);
            BAD("The package could not be read.");
        }
        got += n;
    }
    close(fd);
    if (memcmp(p->data, MAGIC, 8) || rd32(p->data + 8) != 1)
        BAD("This is not an Aegis package (or it needs a newer Aegis).");
    if (memcmp(p->data + p->size - 8, TRAILER, 8))
        BAD("The package is incomplete.");
    br_sha256_init(&c);
    br_sha256_update(&c, p->data, p->size - 40);
    br_sha256_out(&c, digest);
    if (memcmp(digest, p->data + p->size - 40, 32))
        BAD("The package is damaged: its checksum does not match.");
    mlen = rd32(p->data + 16);
    p->nfiles = rd32(p->data + 20);
    if (24 + (size_t)mlen > p->size - 40 || mlen > 64 * 1024 || p->nfiles > MAX_FILES)
        BAD("The package's manifest is damaged.");
    p->manifest = malloc(mlen + 1);
    if (!p->manifest)
        BAD("Not enough memory.");
    memcpy(p->manifest, p->data + 24, mlen);
    p->manifest[mlen] = 0;
    p->files_off = 24 + mlen;

    // Every file must fit and have a safe relative path.
    off = p->files_off;
    for (uint32_t i = 0; i < p->nfiles; i++) {
        struct aip_file_hdr h;
        char name[256];

        if (off + sizeof(h) > p->size - 40)
            BAD("The package's file list is damaged.");
        memcpy(&h, p->data + off, sizeof(h));
        off += sizeof(h);
        if (h.path_len == 0 || h.path_len >= sizeof(name) || off + h.path_len + h.size > p->size - 40)
            BAD("The package's file list is damaged.");
        memcpy(name, p->data + off, h.path_len);
        name[h.path_len] = 0;
        if (!safe_rel_path(name))
            BAD("The package contains a file with an unsafe name.");
        off += h.path_len + h.size;
        p->payload += h.size;
    }

    manifest_get(p->manifest, "id", p->id, sizeof(p->id));
    manifest_get(p->manifest, "name", p->name, sizeof(p->name));
    manifest_get(p->manifest, "version", p->version, sizeof(p->version));
    manifest_get(p->manifest, "publisher", p->publisher, sizeof(p->publisher));
    manifest_get(p->manifest, "description", p->description, sizeof(p->description));
    manifest_get(p->manifest, "exec", p->exec, sizeof(p->exec));
    manifest_get(p->manifest, "icon", p->icon, sizeof(p->icon));
    manifest_get(p->manifest, "suite", p->suite, sizeof(p->suite));
    manifest_get(p->manifest, "opens", p->opens, sizeof(p->opens));
    manifest_get(p->manifest, "permissions", p->permissions, sizeof(p->permissions));
    manifest_get(p->manifest, "command", p->command, sizeof(p->command));
    manifest_get(p->manifest, "scope", p->scope, sizeof(p->scope));
    if (!valid_id(p->id) || !*p->name || !*p->exec)
        BAD("The package's manifest is missing its id, name or program.");
    if (*p->command && !valid_id(p->command))
        BAD("The package's command name is not valid.");
    if (!*p->suite)
        strlcpy(p->suite, "Default", sizeof(p->suite));
    if (!*p->scope)
        strlcpy(p->scope, "either", sizeof(p->scope));
    return 0;
#undef BAD
}

// ---- Permissions ----

static const struct aip_permission perms[] = {
    { "appdata", "Its own data folder", AIP_TIER_BASIC },
    { "documents", "Your Documents, Downloads, Music and Images", AIP_TIER_BASIC },
    { "notifications", "Show notifications", AIP_TIER_BASIC },
    { "clipboard", "Use the clipboard", AIP_TIER_BASIC },
    { "network", "Use the network and the internet", AIP_TIER_ELEVATED },
    { "camera", "Use the camera", AIP_TIER_ELEVATED },
    { "microphone", "Use the microphone", AIP_TIER_ELEVATED },
    { "home", "Your whole home folder", AIP_TIER_ELEVATED },
    { "apps-data", "Other apps' data", AIP_TIER_ELEVATED },
    { "system-files", "System files", AIP_TIER_SYSTEM },
    { "users", "Other people's accounts and files", AIP_TIER_SYSTEM },
    { "devices", "Devices directly", AIP_TIER_SYSTEM },
    { "startup", "Start when the computer starts", AIP_TIER_SYSTEM },
    { "powersudo", "Run with no restrictions at all (asks each time)", AIP_TIER_POWERSUDO },
};

const struct aip_permission *aip_permission_find(const char *id)
{
    for (size_t i = 0; i < sizeof(perms) / sizeof(perms[0]); i++)
        if (!strcmp(perms[i].id, id))
            return &perms[i];
    return NULL;
}

const char *aip_tier_name(int tier)
{
    static const char *const names[] = { "Basic", "Elevated", "System", "powersudo" };

    return tier >= 0 && tier <= AIP_TIER_POWERSUDO ? names[tier] : "?";
}

// The permissions the package asks for, in its manifest order.
int aip_requested(const struct aip *p, const struct aip_permission **out, int max)
{
    char list[256], *s, *next;
    int n = 0;

    strlcpy(list, p->permissions, sizeof(list));
    for (s = list; s && *s && n < max; s = next) {
        const struct aip_permission *perm;

        if ((next = strchr(s, ',')))
            *next++ = 0;
        while (*s == ' ')
            s++;
        if ((perm = aip_permission_find(s)))
            out[n++] = perm;
    }
    return n;
}

// ---- Install, uninstall ----

static void scope_dirs(bool everyone, char *apps, char *records, char *registry, size_t size)
{
    struct user_info me;

    if (everyone) {
        strlcpy(apps, "/apps", size);
        strlcpy(records, "/var/lib/aip", size);
        strlcpy(registry, "/usr/share/applications", size);
        return;
    }
    user_current(&me);
    user_path(&me, "system/appdata/apps", apps, size);
    user_path(&me, "system/appdata/aip", records, size);
    user_path(&me, "system/appdata/applications", registry, size);
}

// exec with the app folder filled in: relative programs are inside it.
static void expand_exec(const char *exec, const char *dir, char *out, size_t size)
{
    size_t n = 0;

    if (*exec != '/')
        n = snprintf(out, size, "%s/", dir);
    for (const char *s = exec; *s && n + 1 < size; s++) {
        if (s[0] == '%' && s[1] == 'd') {
            n += snprintf(out + n, size - n, "%s", dir);
            s++;
        } else {
            out[n++] = *s;
        }
    }
    out[n < size ? n : size - 1] = 0;
}

static int mkdirs(const char *path, uint32_t mode)
{
    char p[600];

    strlcpy(p, path, sizeof(p));
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, mode);
            *s = '/';
        }
    }
    return mkdir(p, mode) == 0 || errno == EEXIST ? 0 : -1;
}

static int spill(const char *path, const void *data, size_t len, uint32_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (fd < 0)
        return -1;
    if (len && write(fd, data, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    close(fd);
    return chmod(path, mode);
}

int aip_install(const struct aip *p, bool everyone, const char *granted, char *error, size_t esize)
{
    char apps[300], records[300], registry[300], dir[400], rec[400], path[700], *list = NULL;
    size_t off = p->files_off, list_len = 0, list_cap = 0;
    int ret = -1;

    if (everyone && geteuid() != 0) {
        snprintf(error, esize, "Installing for everyone needs an administrator.");
        return -1;
    }
    if ((everyone && !strcmp(p->scope, "user")) || (!everyone && !strcmp(p->scope, "machine"))) {
        snprintf(error, esize, "This package can only be installed %s.", everyone ? "for one person" : "for everyone");
        return -1;
    }
    scope_dirs(everyone, apps, records, registry, sizeof(apps));
    snprintf(dir, sizeof(dir), "%s/%s", apps, p->id);
    snprintf(rec, sizeof(rec), "%s/%s", records, p->id);
    // Installing again replaces the old files (an upgrade or repair).
    remove_path(dir);
    if (mkdirs(dir, 0755) < 0 || mkdirs(rec, 0755) < 0 || mkdirs(registry, 0755) < 0) {
        snprintf(error, esize, "The app's folder cannot be made: %s.", strerror(errno));
        return -1;
    }
    for (uint32_t i = 0; i < p->nfiles; i++) {
        struct aip_file_hdr h;
        char name[256], *slash;
        size_t need;

        memcpy(&h, p->data + off, sizeof(h));
        off += sizeof(h);
        memcpy(name, p->data + off, h.path_len);
        name[h.path_len] = 0;
        off += h.path_len;
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if ((slash = strrchr(path, '/')) && slash > path + strlen(dir)) {
            *slash = 0;
            mkdirs(path, 0755);
            *slash = '/';
        }
        // Files are never setuid, and owned by whoever installs them.
        if (spill(path, p->data + off, h.size, h.mode & 0755) < 0) {
            snprintf(error, esize, "%s could not be written: %s.", name, strerror(errno));
            goto out;
        }
        off += h.size;
        need = list_len + strlen(name) + 2;
        if (need > list_cap) {
            char *n = realloc(list, list_cap = need * 2 + 256);

            if (!n)
                goto out;
            list = n;
        }
        list_len += snprintf(list + list_len, list_cap - list_len, "%s\n", name);
    }

    // The record: manifest, granted permissions, files, the package itself.
    snprintf(path, sizeof(path), "%s/manifest", rec);
    if (spill(path, p->manifest, strlen(p->manifest), 0644) < 0)
        goto write_fail;
    snprintf(path, sizeof(path), "%s/granted", rec);
    if (spill(path, granted, strlen(granted), 0644) < 0)
        goto write_fail;
    snprintf(path, sizeof(path), "%s/files", rec);
    if (spill(path, list ? list : "", list_len, 0644) < 0)
        goto write_fail;
    snprintf(path, sizeof(path), "%s/package.aip", rec);
    if (spill(path, p->data, p->size, 0644) < 0)
        goto write_fail;

    // The launcher entry.
    {
        char entry[1600], icon[400], exec[600];

        if (*p->icon && safe_rel_path(p->icon))
            snprintf(icon, sizeof(icon), "%s/%s", dir, p->icon);
        else
            strlcpy(icon, *p->icon ? p->icon : "package", sizeof(icon));
        snprintf(entry, sizeof(entry),
                 "name=%s\nexec=%s\nicon=%s\nsuite=%s\ndescription=%s\nopens=%s\npermissions=%s\npackage=%s\n",
                 p->name, (expand_exec(p->exec, dir, exec, sizeof(exec)), exec), icon, p->suite, p->description, p->opens, granted, p->version);
        snprintf(path, sizeof(path), "%s/aip-%s.app", registry, p->id);
        if (spill(path, entry, strlen(entry), 0644) < 0)
            goto write_fail;
    }
    // A command on everyone's PATH.
    if (everyone && *p->command) {
        char exec[600], script[800];

        // A small script, so arguments in exec are kept.
        mkdirs("/apps/bin", 0755);
        expand_exec(p->exec, dir, exec, sizeof(exec));
        snprintf(path, sizeof(path), "/apps/bin/%s", p->command);
        snprintf(script, sizeof(script), "#!/bin/terminal\n%s $@\n", exec);
        unlink(path);
        if (*p->exec != '/' && !strchr(p->exec, ' ')) {
            symlink(exec, path);
        } else if (spill(path, script, strlen(script), 0755) < 0) {
            goto write_fail;
        }
    }
    ret = 0;
    goto out;
write_fail:
    snprintf(error, esize, "The install record could not be written: %s.", strerror(errno));
out:
    free(list);
    return ret;
}

static int read_small(const char *path, char *out, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    out[0] = 0;
    if (fd < 0)
        return -1;
    n = read(fd, out, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = 0;
    return 0;
}

static int list_scope(bool everyone, struct aip_installed *out, int n, int max)
{
    char apps[300], records[300], registry[300];
    struct dir_stream *d;
    struct aegis_dirent *de;

    scope_dirs(everyone, apps, records, registry, sizeof(apps));
    if (!(d = opendir(records)))
        return n;
    while (n < max && (de = readdir(d))) {
        char path[600], manifest[4096];
        struct aip_installed *a = &out[n];

        if (de->name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s/manifest", records, de->name);
        if (read_small(path, manifest, sizeof(manifest)) < 0)
            continue;
        memset(a, 0, sizeof(*a));
        a->everyone = everyone;
        manifest_get(manifest, "id", a->id, sizeof(a->id));
        manifest_get(manifest, "name", a->name, sizeof(a->name));
        manifest_get(manifest, "version", a->version, sizeof(a->version));
        manifest_get(manifest, "publisher", a->publisher, sizeof(a->publisher));
        snprintf(path, sizeof(path), "%s/%s/granted", records, de->name);
        read_small(path, a->granted, sizeof(a->granted));
        snprintf(a->package, sizeof(a->package), "%s/%s/package.aip", records, de->name);
        if (*a->id)
            n++;
    }
    closedir(d);
    return n;
}

int aip_list_installed(struct aip_installed *out, int max)
{
    int n = list_scope(true, out, 0, max);

    return list_scope(false, out, n, max);
}

int aip_uninstall(const char *id, bool everyone, char *error, size_t esize)
{
    char apps[300], records[300], registry[300], path[600], manifest[4096], command[32];

    if (!valid_id(id)) {
        snprintf(error, esize, "Not a package name.");
        return -1;
    }
    if (everyone && geteuid() != 0) {
        snprintf(error, esize, "Removing an app installed for everyone needs an administrator.");
        return -1;
    }
    scope_dirs(everyone, apps, records, registry, sizeof(apps));
    snprintf(path, sizeof(path), "%s/%s/manifest", records, id);
    if (read_small(path, manifest, sizeof(manifest)) < 0) {
        snprintf(error, esize, "%s is not installed.", id);
        return -1;
    }
    manifest_get(manifest, "command", command, sizeof(command));
    if (everyone && *command && valid_id(command)) {
        snprintf(path, sizeof(path), "/apps/bin/%s", command);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/aip-%s.app", registry, id);
    unlink(path);
    snprintf(path, sizeof(path), "%s/%s", apps, id);
    if (remove_path(path) < 0 && errno != ENOENT) {
        snprintf(error, esize, "The app's files could not all be removed: %s.", strerror(errno));
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", records, id);
    remove_path(path);
    return 0;
}

// Puts back an installed app's files from the copy of its package.
int aip_repair(const struct aip_installed *a, char *error, size_t esize)
{
    struct aip p;
    int ret;

    if (aip_open(a->package, &p, error, esize) < 0)
        return -1;
    ret = aip_install(&p, a->everyone, a->granted, error, esize);
    aip_close(&p);
    return ret;
}
