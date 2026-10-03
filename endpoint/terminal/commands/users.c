#include "commands.h"

// Finds an account or group ("account" or "group") by name or by id
// (exactly one of name and id is set): its id or name goes into out.
static bool lookup(const char *kind, const char *name, const char *id, char *out, size_t size)
{
    struct records r;
    bool found = false;

    records_load(ACCOUNTS_FILE, "accounts", &r);
    for (int i = 1; i < r.n && !found; i++) {
        const char *v = rec_get(&r.b[i], "id");

        if (strcmp(r.b[i].kind, kind) || !v)
            continue;
        if (name && !strcmp(r.b[i].name, name)) {
            strlcpy(out, v, size);
            found = true;
        } else if (id && !strcmp(v, id)) {
            strlcpy(out, r.b[i].name, size);
            found = true;
        }
    }
    records_free(&r);
    return found;
}

int name_to_uid(const char *name, uint32_t *uid)
{
    char buf[16];

    if (isdigit(*name)) {
        *uid = atoi(name);
        return 0;
    }
    if (!lookup("account", name, NULL, buf, sizeof(buf)))
        return -1;
    *uid = atoi(buf);
    return 0;
}

int name_to_gid(const char *name, uint32_t *gid)
{
    char buf[16];

    if (isdigit(*name)) {
        *gid = atoi(name);
        return 0;
    }
    if (!lookup("group", name, NULL, buf, sizeof(buf)))
        return -1;
    *gid = atoi(buf);
    return 0;
}

const char *uid_to_name(uint32_t uid, char *buf, size_t size)
{
    char id[16];

    snprintf(id, sizeof(id), "%u", uid);
    if (!lookup("account", NULL, id, buf, size))
        strlcpy(buf, id, size);
    return buf;
}

const char *gid_to_name(uint32_t gid, char *buf, size_t size)
{
    char id[16];

    snprintf(id, sizeof(id), "%u", gid);
    if (!lookup("group", NULL, id, buf, size))
        strlcpy(buf, id, size);
    return buf;
}
