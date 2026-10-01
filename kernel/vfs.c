#include "vfs.h"
#include "mem.h"
#include "rtc.h"
#include "string.h"

#define VCACHE_SIZE 256

#define FSCALL(m, expr) ({                  \
    mutex_lock(&(m)->lock);                 \
    __auto_type __r = (expr);               \
    mutex_unlock(&(m)->lock);               \
    __r;                                    \
})

const struct cred root_cred = { 0, 0, 0, 0, 0, { 0 } };

static struct filesystem *filesystems;
static struct mount *mounts;
static struct vnode *root;
static uint32_t next_mount_id = 1;
static struct mutex mount_lock = MUTEX_INIT;

static struct vnode *vcache[VCACHE_SIZE];
static struct mutex vcache_lock = MUTEX_INIT;

void vfs_register(struct filesystem *fs)
{
    fs->next = filesystems;
    filesystems = fs;
}

struct vnode *vfs_root(void)
{
    return root;
}

struct mount *vfs_mounts(void)
{
    return mounts;
}

static unsigned vhash(struct mount *m, uint64_t ino)
{
    return (m->id * 31 + ino) % VCACHE_SIZE;
}

struct vnode *vget(struct mount *m, uint64_t ino, int *err)
{
    struct vnode *v;
    int ret;

    mutex_lock(&vcache_lock);
    for (v = vcache[vhash(m, ino)]; v; v = v->hash_next) {
        if (v->mount == m && v->ino == ino) {
            v->refs++;
            mutex_unlock(&vcache_lock);
            return v;
        }
    }

    v = kzalloc(sizeof(*v));
    if (!v) {
        mutex_unlock(&vcache_lock);
        if (err)
            *err = -ENOMEM;
        return NULL;
    }
    v->mount = m;
    v->ino = ino;
    v->refs = 1;

    ret = FSCALL(m, m->ops->read_vnode(m, ino, v));
    if (ret < 0) {
        mutex_unlock(&vcache_lock);
        kfree(v);
        if (err)
            *err = ret;
        return NULL;
    }
    v->hash_next = vcache[vhash(m, ino)];
    vcache[vhash(m, ino)] = v;
    mutex_unlock(&vcache_lock);
    return v;
}

void vref(struct vnode *v)
{
    mutex_lock(&vcache_lock);
    v->refs++;
    mutex_unlock(&vcache_lock);
}

void vput(struct vnode *v)
{
    struct vnode **pp;

    if (!v)
        return;
    mutex_lock(&vcache_lock);
    if (--v->refs) {
        mutex_unlock(&vcache_lock);
        return;
    }
    for (pp = &vcache[vhash(v->mount, v->ino)]; *pp && *pp != v; pp = &(*pp)->hash_next)
        ;
    if (*pp)
        *pp = v->hash_next;

    if (v->mount->ops->release)
        FSCALL(v->mount, (v->mount->ops->release(v), 0));
    mutex_unlock(&vcache_lock);
    kfree(v);
}

bool cred_in_group(const struct cred *c, uint32_t gid)
{
    if (c->egid == gid)
        return true;
    for (uint32_t i = 0; i < c->ngroups; i++) {
        if (c->groups[i] == gid)
            return true;
    }
    return false;
}

int vfs_permission(struct vnode *v, const struct cred *c, int mask)
{
    uint32_t bits;

    if (c->euid == 0) {
        if ((mask & X_OK) && !S_ISDIR(v->mode) && !(v->mode & 0111))
            return -EACCES;
        return 0;
    }
    if (c->euid == v->uid)
        bits = v->mode >> 6;
    else if (cred_in_group(c, v->gid))
        bits = v->mode >> 3;
    else
        bits = v->mode;
    return ((bits & 7) & (uint32_t)mask) == (uint32_t)mask ? 0 : -EACCES;
}

static int may_delete(struct vnode *dir, struct vnode *victim, const struct cred *c)
{
    int ret = vfs_permission(dir, c, W_OK | X_OK);

    if (ret)
        return ret;
    if ((dir->mode & S_ISVTX) && c->euid != 0 && c->euid != victim->uid && c->euid != dir->uid)
        return -EPERM;
    return 0;
}

static struct vnode *cross_mounts(struct vnode *v)
{
    while (v->covered_by) {
        struct vnode *r = v->covered_by->root;

        vref(r);
        vput(v);
        v = r;
    }
    return v;
}

static int lookup_child(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    uint64_t ino;
    int ret;

    if (!S_ISDIR(dir->mode))
        return -ENOTDIR;
    ret = FSCALL(dir->mount, dir->mount->ops->lookup(dir, name, len, &ino));
    if (ret < 0)
        return ret;
    *out = vget(dir->mount, ino, &ret);
    return *out ? 0 : ret;
}

static int resolve(const char *path, struct vnode *start, const struct cred *c, bool follow,
                   int *links, struct vnode **out, char *last)
{
    struct vnode *v = (path[0] == '/' || !start) ? root : start;
    const char *p = path;
    int ret;

    if (!v)
        return -ENOENT;
    vref(v);

    for (;;) {
        const char *q;
        size_t len;
        bool is_last;
        struct vnode *child;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        for (q = p; *q && *q != '/'; q++)
            ;
        len = q - p;
        if (len > AEGIS_NAME_MAX) {
            vput(v);
            return -ENAMETOOLONG;
        }
        for (is_last = true; *q; q++) {
            if (*q != '/') {
                is_last = false;
                break;
            }
        }
        q = p + len;

        if (!S_ISDIR(v->mode)) {
            vput(v);
            return -ENOTDIR;
        }
        if (last && is_last) {
            memcpy(last, p, len);
            last[len] = '\0';
            *out = v;
            return 0;
        }
        if ((ret = vfs_permission(v, c, X_OK))) {
            vput(v);
            return ret;
        }

        if (len == 1 && p[0] == '.') {
            p = q;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (v == v->mount->root && v->mount->covered) {
                struct vnode *cv = v->mount->covered;

                vref(cv);
                vput(v);
                v = cv;
            }
            if (v == root) {
                p = q;
                continue;
            }
        }

        ret = lookup_child(v, p, len, &child);
        if (ret) {
            vput(v);
            return ret;
        }
        child = cross_mounts(child);

        if (S_ISLNK(child->mode) && (!is_last || follow)) {
            char *target = kmalloc(AEGIS_PATH_MAX * 2);
            int n;

            if (++*links > VFS_MAX_SYMLINKS || !target) {
                kfree(target);
                vput(child);
                vput(v);
                return target ? -ELOOP : -ENOMEM;
            }
            n = FSCALL(child->mount, child->mount->ops->readlink(child, target, AEGIS_PATH_MAX));
            vput(child);
            if (n < 0) {
                kfree(target);
                vput(v);
                return n;
            }
            target[n] = '\0';
            if (*q) {
                size_t rest = strlen(q);
                if (n + rest >= AEGIS_PATH_MAX * 2) {
                    kfree(target);
                    vput(v);
                    return -ENAMETOOLONG;
                }
                memcpy(target + n, q, rest + 1);
            }
            ret = resolve(target, v, c, follow, links, out, last);
            kfree(target);
            vput(v);
            return ret;
        }

        vput(v);
        v = child;
        p = q;
    }

    if (last) {
        vput(v);
        return -EEXIST;
    }
    *out = v;
    return 0;
}

int vfs_lookup(const char *path, struct vnode *cwd, const struct cred *c, bool follow, struct vnode **out)
{
    int links = 0;

    if (!*path)
        return -ENOENT;
    return resolve(path, cwd, c, follow, &links, out, NULL);
}

int vfs_lookup_parent(const char *path, struct vnode *cwd, const struct cred *c,
                      struct vnode **dir, char *name)
{
    int links = 0;
    int ret;

    if (!*path)
        return -ENOENT;
    ret = resolve(path, cwd, c, true, &links, dir, name);
    if (ret == 0 && (!strcmp(name, ".") || !strcmp(name, ".."))) {
        vput(*dir);
        return -EINVAL;
    }
    return ret;
}

int vfs_mount(const char *fstype, struct block_device *dev, const char *path, bool readonly)
{
    struct filesystem *fs;
    struct mount *m;
    struct vnode *point = NULL;
    int ret;

    for (fs = filesystems; fs && strcmp(fs->name, fstype); fs = fs->next)
        ;
    if (!fs)
        return -ENODEV;

    if (root) {
        if ((ret = vfs_lookup(path, NULL, &root_cred, true, &point)))
            return ret;
        if (!S_ISDIR(point->mode) || point->covered_by) {
            vput(point);
            return !S_ISDIR(point->mode) ? -ENOTDIR : -EBUSY;
        }
    } else if (strcmp(path, "/")) {
        return -ENOENT;
    }

    m = kzalloc(sizeof(*m));
    if (!m) {
        vput(point);
        return -ENOMEM;
    }
    mutex_lock(&mount_lock);
    m->id = next_mount_id++;
    mutex_unlock(&mount_lock);
    m->fstype = fs->name;
    m->dev = dev;
    m->readonly = readonly;

    ret = fs->mount(dev, readonly, m);
    if (ret < 0) {
        kfree(m);
        vput(point);
        return ret;
    }

    mutex_lock(&mount_lock);
    if (point) {
        m->covered = point;
        point->covered_by = m;
    } else {
        root = m->root;
    }
    struct mount **pp = &mounts;
    while (*pp)
        pp = &(*pp)->next;
    *pp = m;
    mutex_unlock(&mount_lock);
    return 0;
}

int vfs_sync_all(void)
{
    int ret = 0;

    for (struct mount *m = mounts; m; m = m->next) {
        if (m->ops->sync && FSCALL(m, m->ops->sync(m)) < 0)
            ret = -EIO;
    }
    return ret;
}

int vfs_unmount_all(void)
{
    int ret = vfs_sync_all();

    for (struct mount *m = mounts; m; m = m->next) {
        if (m->ops->unmount && FSCALL(m, m->ops->unmount(m)) < 0)
            ret = -EIO;
    }
    return ret;
}

int64_t vfs_read(struct vnode *v, void *buf, size_t size, uint64_t off)
{
    if (S_ISDIR(v->mode))
        return -EISDIR;
    if (!S_ISREG(v->mode) && !S_ISLNK(v->mode))
        return -EINVAL;
    if (off >= v->size)
        return 0;
    size = MIN(size, v->size - off);
    return FSCALL(v->mount, v->mount->ops->read(v, buf, size, off));
}

int64_t vfs_write(struct vnode *v, const void *buf, size_t size, uint64_t off)
{
    if (S_ISDIR(v->mode))
        return -EISDIR;
    if (!S_ISREG(v->mode))
        return -EINVAL;
    if (v->mount->readonly)
        return -EROFS;
    if (!v->mount->ops->write)
        return -ENOTSUP;
    return FSCALL(v->mount, v->mount->ops->write(v, buf, size, off));
}

int vfs_readdir(struct vnode *dir, uint64_t *pos, struct vfs_dirent *out)
{
    if (!S_ISDIR(dir->mode))
        return -ENOTDIR;
    return FSCALL(dir->mount, dir->mount->ops->readdir(dir, pos, out));
}

int vfs_truncate(struct vnode *v, uint64_t size, const struct cred *c)
{
    int ret;

    if (S_ISDIR(v->mode))
        return -EISDIR;
    if (!S_ISREG(v->mode))
        return -EINVAL;
    if (v->mount->readonly)
        return -EROFS;
    if (c && (ret = vfs_permission(v, c, W_OK)))
        return ret;
    if (!v->mount->ops->truncate)
        return -ENOTSUP;
    return FSCALL(v->mount, v->mount->ops->truncate(v, size));
}

static int create_node(const char *path, struct vnode *cwd, const struct cred *c, uint32_t mode,
                       const char *link_target, struct vnode **out)
{
    char name[AEGIS_NAME_MAX + 1];
    struct vnode *dir, *existing;
    uint64_t ino;
    uint32_t gid;
    int ret;

    if ((ret = vfs_lookup_parent(path, cwd, c, &dir, name)))
        return ret;
    if (dir->mount->readonly) {
        ret = -EROFS;
        goto out;
    }
    if (lookup_child(dir, name, strlen(name), &existing) == 0) {
        vput(existing);
        ret = -EEXIST;
        goto out;
    }
    if ((ret = vfs_permission(dir, c, W_OK | X_OK)))
        goto out;

    gid = (dir->mode & S_ISGID) ? dir->gid : c->egid;
    if (S_ISDIR(mode) && (dir->mode & S_ISGID))
        mode |= S_ISGID;

    if (link_target)
        ret = dir->mount->ops->symlink
            ? FSCALL(dir->mount, dir->mount->ops->symlink(dir, name, strlen(name), link_target,
                                                          c->euid, gid, &ino))
            : -ENOTSUP;
    else
        ret = FSCALL(dir->mount, dir->mount->ops->create(dir, name, strlen(name), mode,
                                                         c->euid, gid, &ino));
    if (ret == 0 && out)
        *out = vget(dir->mount, ino, &ret);
out:
    vput(dir);
    return ret;
}

int vfs_mkdir(const char *path, struct vnode *cwd, const struct cred *c, uint32_t mode)
{
    return create_node(path, cwd, c, S_IFDIR | (mode & 07777), NULL, NULL);
}

int vfs_symlink(const char *target, const char *path, struct vnode *cwd, const struct cred *c)
{
    if (strlen(target) >= AEGIS_PATH_MAX)
        return -ENAMETOOLONG;
    return create_node(path, cwd, c, S_IFLNK | 0777, target, NULL);
}

static int remove_node(const char *path, struct vnode *cwd, const struct cred *c, bool dir_wanted)
{
    char name[AEGIS_NAME_MAX + 1];
    struct vnode *dir, *victim;
    int ret;

    if ((ret = vfs_lookup_parent(path, cwd, c, &dir, name)))
        return ret;
    if ((ret = lookup_child(dir, name, strlen(name), &victim)))
        goto out;

    if (victim->covered_by || victim == victim->mount->root)
        ret = -EBUSY;
    else if (dir_wanted && !S_ISDIR(victim->mode))
        ret = -ENOTDIR;
    else if (!dir_wanted && S_ISDIR(victim->mode))
        ret = -EISDIR;
    else if (dir->mount->readonly)
        ret = -EROFS;
    else if (!(ret = may_delete(dir, victim, c)))
        ret = dir_wanted
            ? FSCALL(dir->mount, dir->mount->ops->rmdir(dir, name, strlen(name), victim))
            : FSCALL(dir->mount, dir->mount->ops->unlink(dir, name, strlen(name), victim));
    vput(victim);
out:
    vput(dir);
    return ret;
}

int vfs_rmdir(const char *path, struct vnode *cwd, const struct cred *c)
{
    return remove_node(path, cwd, c, true);
}

int vfs_unlink(const char *path, struct vnode *cwd, const struct cred *c)
{
    return remove_node(path, cwd, c, false);
}

// True if `dir` is `ancestor` or lies below it on the same filesystem.
static bool is_within(struct vnode *dir, struct vnode *ancestor)
{
    struct vnode *v = dir;
    bool found = false;

    vref(v);
    for (int depth = 0; depth < 256; depth++) {
        struct vnode *parent;

        if (v->ino == ancestor->ino) {
            found = true;
            break;
        }
        if (v == v->mount->root || lookup_child(v, "..", 2, &parent))
            break;
        vput(v);
        v = parent;
    }
    vput(v);
    return found;
}

int vfs_rename(const char *from, const char *to, struct vnode *cwd, const struct cred *c)
{
    char oname[AEGIS_NAME_MAX + 1], nname[AEGIS_NAME_MAX + 1];
    struct vnode *odir = NULL, *ndir = NULL, *src = NULL, *dst = NULL;
    int ret;

    if ((ret = vfs_lookup_parent(from, cwd, c, &odir, oname)))
        return ret;
    if ((ret = vfs_lookup_parent(to, cwd, c, &ndir, nname)))
        goto out;
    if (odir->mount != ndir->mount) {
        ret = -EXDEV;
        goto out;
    }
    if (odir->mount->readonly) {
        ret = -EROFS;
        goto out;
    }
    if ((ret = lookup_child(odir, oname, strlen(oname), &src)))
        goto out;
    if ((ret = may_delete(odir, src, c)) || (ret = vfs_permission(ndir, c, W_OK | X_OK)))
        goto out;
    if (src->covered_by || src == src->mount->root) {
        ret = -EBUSY;
        goto out;
    }
    if (lookup_child(ndir, nname, strlen(nname), &dst) == 0) {
        if (dst == src) {
            ret = 0;
            goto out;
        }
        if (S_ISDIR(src->mode) != S_ISDIR(dst->mode)) {
            ret = S_ISDIR(src->mode) ? -ENOTDIR : -EISDIR;
            goto out;
        }
        if ((ret = may_delete(ndir, dst, c)))
            goto out;
        if (dst->covered_by) {
            ret = -EBUSY;
            goto out;
        }
    } else {
        dst = NULL;
    }
    if (S_ISDIR(src->mode) && is_within(ndir, src)) {
        ret = -EINVAL;
        goto out;
    }

    ret = FSCALL(odir->mount, odir->mount->ops->rename(odir, oname, strlen(oname), ndir, nname,
                                                       strlen(nname), src, dst));
out:
    vput(dst);
    vput(src);
    vput(ndir);
    vput(odir);
    return ret;
}

int vfs_link(const char *existing, const char *path, struct vnode *cwd, const struct cred *c)
{
    char name[AEGIS_NAME_MAX + 1];
    struct vnode *target, *dir, *tmp;
    int ret;

    if ((ret = vfs_lookup(existing, cwd, c, false, &target)))
        return ret;
    if (S_ISDIR(target->mode)) {
        vput(target);
        return -EPERM;
    }
    if ((ret = vfs_lookup_parent(path, cwd, c, &dir, name))) {
        vput(target);
        return ret;
    }
    if (dir->mount != target->mount)
        ret = -EXDEV;
    else if (dir->mount->readonly)
        ret = -EROFS;
    else if (lookup_child(dir, name, strlen(name), &tmp) == 0) {
        vput(tmp);
        ret = -EEXIST;
    } else if (!(ret = vfs_permission(dir, c, W_OK | X_OK)))
        ret = dir->mount->ops->link
            ? FSCALL(dir->mount, dir->mount->ops->link(dir, name, strlen(name), target))
            : -ENOTSUP;
    vput(dir);
    vput(target);
    return ret;
}

int vfs_readlink(const char *path, struct vnode *cwd, const struct cred *c, char *buf, size_t size)
{
    struct vnode *v;
    int ret;

    if ((ret = vfs_lookup(path, cwd, c, false, &v)))
        return ret;
    if (!S_ISLNK(v->mode))
        ret = -EINVAL;
    else
        ret = FSCALL(v->mount, v->mount->ops->readlink(v, buf, size));
    vput(v);
    return ret;
}

static int setattr(const char *path, struct vnode *cwd, const struct cred *c,
                   const struct vattr *a, uint32_t mask)
{
    struct vnode *v;
    int ret;

    if ((ret = vfs_lookup(path, cwd, c, true, &v)))
        return ret;

    if (v->mount->readonly) {
        ret = -EROFS;
    } else if (mask & VATTR_MODE) {
        if (c->euid != 0 && c->euid != v->uid)
            ret = -EPERM;
    } else if (mask & (VATTR_UID | VATTR_GID)) {
        if (c->euid != 0) {
            if ((mask & VATTR_UID) && a->uid != v->uid)
                ret = -EPERM;
            else if (c->euid != v->uid || ((mask & VATTR_GID) && !cred_in_group(c, a->gid)))
                ret = -EPERM;
        }
    } else if (mask & (VATTR_ATIME | VATTR_MTIME)) {
        if (c->euid != 0 && c->euid != v->uid && vfs_permission(v, c, W_OK))
            ret = -EPERM;
    }

    if (ret == 0)
        ret = v->mount->ops->setattr ? FSCALL(v->mount, v->mount->ops->setattr(v, a, mask)) : -ENOTSUP;
    vput(v);
    return ret;
}

int vfs_chmod(const char *path, struct vnode *cwd, const struct cred *c, uint32_t mode)
{
    struct vattr a = { .mode = mode & 07777 };

    return setattr(path, cwd, c, &a, VATTR_MODE);
}

int vfs_chown(const char *path, struct vnode *cwd, const struct cred *c, uint32_t uid, uint32_t gid)
{
    struct vattr a = { .uid = uid, .gid = gid };
    uint32_t mask = (uid != (uint32_t)-1 ? VATTR_UID : 0) | (gid != (uint32_t)-1 ? VATTR_GID : 0);

    return mask ? setattr(path, cwd, c, &a, mask) : 0;
}

int vfs_utime(const char *path, struct vnode *cwd, const struct cred *c, int64_t atime, int64_t mtime)
{
    struct vattr a = { .atime = atime, .mtime = mtime };

    return setattr(path, cwd, c, &a, VATTR_ATIME | VATTR_MTIME);
}

int vfs_statfs(struct vnode *v, struct aegis_statfs *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->fstype, v->mount->fstype, strnlen(v->mount->fstype, sizeof(out->fstype) - 1));
    return v->mount->ops->statfs ? FSCALL(v->mount, v->mount->ops->statfs(v->mount, out)) : 0;
}

void vfs_stat(struct vnode *v, struct aegis_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->dev = v->mount->id;
    st->ino = v->ino;
    st->mode = v->mode;
    st->nlink = v->nlink;
    st->uid = v->uid;
    st->gid = v->gid;
    st->size = v->size;
    st->blocks = v->blocks;
    st->blksize = 4096;
    st->atime = v->atime;
    st->mtime = v->mtime;
    st->ctime = v->ctime;
}

static int name_in_parent(struct vnode *parent, uint64_t ino, char *name)
{
    struct vfs_dirent de;
    uint64_t pos = 0;
    int ret;

    while ((ret = vfs_readdir(parent, &pos, &de)) > 0) {
        if (de.ino == ino && strcmp(de.name, ".") && strcmp(de.name, "..")) {
            memcpy(name, de.name, de.namelen + 1);
            return 0;
        }
    }
    return ret < 0 ? ret : -ENOENT;
}

int vfs_getcwd(struct vnode *cwd, char *buf, size_t size)
{
    char *path = kmalloc(AEGIS_PATH_MAX);
    char name[AEGIS_NAME_MAX + 1];
    size_t pos = AEGIS_PATH_MAX - 1;
    struct vnode *v = cwd;
    int ret = 0;

    if (!path)
        return -ENOMEM;
    path[pos] = '\0';
    vref(v);

    while (v != root) {
        struct vnode *parent;
        size_t len;

        while (v == v->mount->root && v->mount->covered) {
            struct vnode *cv = v->mount->covered;
            vref(cv);
            vput(v);
            v = cv;
        }
        if (v == root)
            break;
        if ((ret = lookup_child(v, "..", 2, &parent)))
            break;
        parent = cross_mounts(parent);
        if ((ret = name_in_parent(parent, v->ino, name))) {
            vput(parent);
            break;
        }
        len = strlen(name);
        if (len + 1 > pos) {
            vput(parent);
            ret = -ERANGE;
            break;
        }
        pos -= len;
        memcpy(path + pos, name, len);
        path[--pos] = '/';
        vput(v);
        v = parent;
    }
    vput(v);

    if (ret == 0) {
        if (pos == AEGIS_PATH_MAX - 1)
            path[--pos] = '/';
        if (AEGIS_PATH_MAX - pos > size)
            ret = -ERANGE;
        else
            memcpy(buf, path + pos, AEGIS_PATH_MAX - pos);
    }
    kfree(path);
    return ret;
}

int vfs_read_file(const char *path, char **data, size_t *size)
{
    struct vnode *v;
    char *buf;
    int64_t n;
    int ret;

    if ((ret = vfs_lookup(path, NULL, &root_cred, true, &v)))
        return ret;
    if (!S_ISREG(v->mode) || v->size > (16 << 20)) {
        vput(v);
        return -EINVAL;
    }
    buf = kmalloc(v->size + 1);
    if (!buf) {
        vput(v);
        return -ENOMEM;
    }
    n = vfs_read(v, buf, v->size, 0);
    vput(v);
    if (n < 0) {
        kfree(buf);
        return n;
    }
    buf[n] = '\0';
    *data = buf;
    if (size)
        *size = n;
    return 0;
}

struct file *file_alloc(const struct file_ops *ops, uint32_t flags)
{
    struct file *f = kzalloc(sizeof(*f));

    if (f) {
        f->ops = ops;
        f->flags = flags;
        f->refs = 1;
    }
    return f;
}

void file_ref(struct file *f)
{
    __atomic_fetch_add(&f->refs, 1, __ATOMIC_RELAXED);
}

void file_put(struct file *f)
{
    if (!f || __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL))
        return;
    if (f->ops && f->ops->close)
        f->ops->close(f);
    vput(f->vnode);
    kfree(f);
}

int vfs_open(const char *path, struct vnode *cwd, const struct cred *c, uint32_t flags,
             uint32_t mode, struct file **out)
{
    struct vnode *v = NULL;
    uint32_t acc = flags & O_ACCMODE;
    bool follow = !(flags & O_NOFOLLOW);
    struct file *f;
    int ret;

    if (flags & O_CREAT) {
        ret = create_node(path, cwd, c, S_IFREG | (mode & 07777), NULL, &v);
        if (ret == -EEXIST && !(flags & O_EXCL))
            ret = vfs_lookup(path, cwd, c, follow, &v);
    } else {
        ret = vfs_lookup(path, cwd, c, follow, &v);
    }
    if (ret)
        return ret;

    if (S_ISLNK(v->mode))
        ret = -ELOOP;
    else if ((flags & O_DIRECTORY) && !S_ISDIR(v->mode))
        ret = -ENOTDIR;
    else if (S_ISDIR(v->mode) && acc != O_RDONLY)
        ret = -EISDIR;
    else if ((acc == O_RDONLY || acc == O_RDWR) && (ret = vfs_permission(v, c, R_OK)))
        ;
    else if ((acc == O_WRONLY || acc == O_RDWR) && (ret = vfs_permission(v, c, W_OK)))
        ;
    else if (acc != O_RDONLY && v->mount->readonly)
        ret = -EROFS;
    else if ((flags & O_TRUNC) && acc != O_RDONLY && S_ISREG(v->mode))
        ret = vfs_truncate(v, 0, NULL);
    if (ret) {
        vput(v);
        return ret;
    }

    f = NULL;
    if (v->mount->ops->open && (ret = v->mount->ops->open(v, flags, &f))) {
        vput(v);
        return ret;
    }
    if (f) {
        f->vnode = v;
        f->flags = flags;
        *out = f;
        return 0;
    }
    f = file_alloc(NULL, flags);
    if (!f) {
        vput(v);
        return -ENOMEM;
    }
    f->vnode = v;
    *out = f;
    return 0;
}

int64_t file_read(struct file *f, void *buf, size_t size)
{
    int64_t n;

    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (f->ops && f->ops->read)
        return f->ops->read(f, buf, size);
    if (!f->vnode)
        return -EINVAL;

    mutex_lock(&f->lock);
    n = vfs_read(f->vnode, buf, size, f->offset);
    if (n > 0)
        f->offset += n;
    mutex_unlock(&f->lock);
    return n;
}

int64_t file_write(struct file *f, const void *buf, size_t size)
{
    int64_t n;

    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (f->ops && f->ops->write)
        return f->ops->write(f, buf, size);
    if (!f->vnode)
        return -EINVAL;

    mutex_lock(&f->lock);
    if (f->flags & O_APPEND)
        f->offset = f->vnode->size;
    n = vfs_write(f->vnode, buf, size, f->offset);
    if (n > 0)
        f->offset += n;
    mutex_unlock(&f->lock);
    return n;
}

int64_t file_seek(struct file *f, int64_t off, int whence)
{
    int64_t base;

    if (!f->vnode || S_ISCHR(f->vnode->mode))
        return -ESPIPE;
    mutex_lock(&f->lock);
    base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->offset
         : whence == SEEK_END ? (int64_t)f->vnode->size : -1;
    if (base < 0 || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        || base + off < 0) {
        mutex_unlock(&f->lock);
        return -EINVAL;
    }
    f->offset = base + off;
    mutex_unlock(&f->lock);
    return f->offset;
}

int64_t file_getdents(struct file *f, void *buf, size_t size)
{
    struct vfs_dirent de;
    size_t used = 0;
    int ret = 0;

    if (!f->vnode || !S_ISDIR(f->vnode->mode))
        return -ENOTDIR;

    mutex_lock(&f->lock);
    for (;;) {
        uint64_t pos = f->offset;
        struct aegis_dirent *out;
        size_t reclen;

        ret = vfs_readdir(f->vnode, &pos, &de);
        if (ret <= 0)
            break;
        reclen = ALIGN_UP(sizeof(struct aegis_dirent) + de.namelen + 1, 8);
        if (used + reclen > size) {
            if (used == 0)
                ret = -EINVAL;
            break;
        }
        out = (struct aegis_dirent *)((uint8_t *)buf + used);
        out->ino = de.ino;
        out->reclen = reclen;
        out->type = de.type;
        out->namelen = de.namelen;
        memcpy(out->name, de.name, de.namelen + 1);
        used += reclen;
        f->offset = pos;
    }
    mutex_unlock(&f->lock);
    return ret < 0 && used == 0 ? ret : (int64_t)used;
}
