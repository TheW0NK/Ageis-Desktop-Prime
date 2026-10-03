#include "commands.h"

// Finds the line in an /msc file whose field `key` equals `value`, and copies
// field `want` of that line into out.
static bool lookup(const char *path, int key, const char *value, int want, char *out, size_t size)
{
    char line[512];
    int fd = open(path, O_RDONLY);
    bool found = false;

    if (fd < 0)
        return false;
    while (!found && read_line(fd, line, sizeof(line)) >= 0) {
        char *f[7];
        int k = 0;

        f[k++] = line;
        for (char *p = line; *p && k < 7; p++) {
            if (*p == ':') {
                *p = '\0';
                f[k++] = p + 1;
            }
        }
        if (k > key && k > want && !strcmp(f[key], value)) {
            strlcpy(out, f[want], size);
            found = true;
        }
    }
    close(fd);
    return found;
}

int name_to_uid(const char *name, uint32_t *uid)
{
    char buf[16];

    if (isdigit(*name)) {
        *uid = atoi(name);
        return 0;
    }
    if (!lookup("/msc/passwd", 0, name, 2, buf, sizeof(buf)))
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
    if (!lookup("/msc/group", 0, name, 2, buf, sizeof(buf)))
        return -1;
    *gid = atoi(buf);
    return 0;
}

const char *uid_to_name(uint32_t uid, char *buf, size_t size)
{
    char id[16];

    snprintf(id, sizeof(id), "%u", uid);
    if (!lookup("/msc/passwd", 2, id, 0, buf, size))
        strlcpy(buf, id, size);
    return buf;
}

const char *gid_to_name(uint32_t gid, char *buf, size_t size)
{
    char id[16];

    snprintf(id, sizeof(id), "%u", gid);
    if (!lookup("/msc/group", 2, id, 0, buf, size))
        strlcpy(buf, id, size);
    return buf;
}
