#ifndef AEGIS_VFS_H
#define AEGIS_VFS_H

#include "kernel.h"
#include "block.h"
#include "sync.h"
#include "abi/errno.h"
#include "abi/fs.h"

#define VFS_MAX_SYMLINKS    8
#define NGROUPS_MAX         16

struct cred {
    uint32_t uid, gid;
    uint32_t euid, egid;
    uint32_t ngroups;
    uint32_t groups[NGROUPS_MAX];
};

extern const struct cred root_cred;

#define VATTR_MODE      0x01
#define VATTR_UID       0x02
#define VATTR_GID       0x04
#define VATTR_ATIME     0x08
#define VATTR_MTIME     0x10

struct vattr {
    uint32_t mode;
    uint32_t uid, gid;
    int64_t atime, mtime;
};

struct mount;
struct file;

struct vnode {
    struct mount *mount;
    uint64_t ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t blocks;
    int64_t atime, mtime, ctime;
    uint32_t refs;
    void *data;
    struct mount *covered_by;
    struct vnode *hash_next;
};

struct vfs_dirent {
    uint64_t ino;
    uint8_t type;
    uint8_t namelen;
    char name[AEGIS_NAME_MAX + 1];
};

// Every call is made with the mount's lock held.
struct fs_ops {
    int (*read_vnode)(struct mount *m, uint64_t ino, struct vnode *v);
    void (*release)(struct vnode *v);
    int (*lookup)(struct vnode *dir, const char *name, size_t len, uint64_t *ino);
    int64_t (*read)(struct vnode *v, void *buf, size_t size, uint64_t off);
    int64_t (*write)(struct vnode *v, const void *buf, size_t size, uint64_t off);
    int (*truncate)(struct vnode *v, uint64_t size);
    int (*readdir)(struct vnode *dir, uint64_t *pos, struct vfs_dirent *out);
    int (*create)(struct vnode *dir, const char *name, size_t len, uint32_t mode,
                  uint32_t uid, uint32_t gid, uint64_t *ino);
    int (*symlink)(struct vnode *dir, const char *name, size_t len, const char *target,
                   uint32_t uid, uint32_t gid, uint64_t *ino);
    int (*link)(struct vnode *dir, const char *name, size_t len, struct vnode *target);
    int (*unlink)(struct vnode *dir, const char *name, size_t len, struct vnode *victim);
    int (*rmdir)(struct vnode *dir, const char *name, size_t len, struct vnode *victim);
    int (*rename)(struct vnode *odir, const char *oname, size_t olen,
                  struct vnode *ndir, const char *nname, size_t nlen,
                  struct vnode *src, struct vnode *replaced);
    int (*readlink)(struct vnode *v, char *buf, size_t size);
    int (*setattr)(struct vnode *v, const struct vattr *a, uint32_t mask);
    int (*statfs)(struct mount *m, struct aegis_statfs *out);
    int (*sync)(struct mount *m);
    int (*unmount)(struct mount *m);
    // Optional: device filesystems return their own struct file (0 with *out
    // left NULL falls back to an ordinary vnode file).
    int (*open)(struct vnode *v, uint32_t flags, struct file **out);
};

struct mount {
    uint32_t id;
    const char *fstype;
    const struct fs_ops *ops;
    struct block_device *dev;
    struct vnode *root;
    struct vnode *covered;
    void *data;
    bool readonly;
    struct mutex lock;
    struct mount *next;
};

struct filesystem {
    const char *name;
    int (*mount)(struct block_device *dev, bool readonly, struct mount *m);
    struct filesystem *next;
};

void vfs_register(struct filesystem *fs);
int vfs_mount(const char *fstype, struct block_device *dev, const char *path, bool readonly);
int vfs_unmount_all(void);
int vfs_unmount(const char *path);
bool vfs_device_mounted(struct block_device *dev);
int vfs_sync_all(void);
struct vnode *vfs_root(void);
struct mount *vfs_mounts(void);

struct vnode *vget(struct mount *m, uint64_t ino, int *err);
void vref(struct vnode *v);
void vput(struct vnode *v);

int vfs_permission(struct vnode *v, const struct cred *c, int mask);
bool cred_in_group(const struct cred *c, uint32_t gid);

int vfs_lookup(const char *path, struct vnode *cwd, const struct cred *c, bool follow, struct vnode **out);
int vfs_lookup_parent(const char *path, struct vnode *cwd, const struct cred *c,
                      struct vnode **dir, char *name);

int64_t vfs_read(struct vnode *v, void *buf, size_t size, uint64_t off);
int64_t vfs_write(struct vnode *v, const void *buf, size_t size, uint64_t off);
int vfs_readdir(struct vnode *dir, uint64_t *pos, struct vfs_dirent *out);
int vfs_truncate(struct vnode *v, uint64_t size, const struct cred *c);
int vfs_mkdir(const char *path, struct vnode *cwd, const struct cred *c, uint32_t mode);
int vfs_mknod(const char *path, struct vnode *cwd, const struct cred *c, uint32_t mode,
              struct vnode **out);
int vfs_rmdir(const char *path, struct vnode *cwd, const struct cred *c);
int vfs_unlink(const char *path, struct vnode *cwd, const struct cred *c);
int vfs_rename(const char *from, const char *to, struct vnode *cwd, const struct cred *c);
int vfs_symlink(const char *target, const char *path, struct vnode *cwd, const struct cred *c);
int vfs_link(const char *existing, const char *path, struct vnode *cwd, const struct cred *c);
int vfs_readlink(const char *path, struct vnode *cwd, const struct cred *c, char *buf, size_t size);
int vfs_chmod(const char *path, struct vnode *cwd, const struct cred *c, uint32_t mode);
int vfs_chown(const char *path, struct vnode *cwd, const struct cred *c, uint32_t uid, uint32_t gid);
int vfs_utime(const char *path, struct vnode *cwd, const struct cred *c, int64_t atime, int64_t mtime);
int vfs_statfs(struct vnode *v, struct aegis_statfs *out);
void vfs_stat(struct vnode *v, struct aegis_stat *st);
int vfs_getcwd(struct vnode *cwd, char *buf, size_t size);

// Reads a whole file into a kmalloc'd, NUL-terminated buffer.
int vfs_read_file(const char *path, char **data, size_t *size);

struct file;

struct poll_table;
struct vma;

struct file_ops {
    int64_t (*read)(struct file *f, void *buf, size_t size);
    int64_t (*write)(struct file *f, const void *buf, size_t size);
    int64_t (*ioctl)(struct file *f, uint64_t cmd, uint64_t arg);
    void (*close)(struct file *f);
    // Returns the POLL* events ready now, and registers on the wait queues
    // that will announce changes (poll_wait). Without it a file is always
    // readable and writable.
    uint32_t (*poll)(struct file *f, struct poll_table *pt);
    // Fills in a mapping's backing (type, shm or offset).
    int (*mmap)(struct file *f, struct vma *v);
};

// poll() collects wait queue entries here; pt is NULL when only the ready
// events are wanted.
#define POLL_MAX_WAITS  128

struct poll_table {
    int count;
    struct {
        struct wait_queue *q;
        struct wait_entry e;
    } waits[POLL_MAX_WAITS];
};

void poll_wait(struct poll_table *pt, struct wait_queue *q);
uint32_t file_poll(struct file *f, struct poll_table *pt);

struct file {
    struct vnode *vnode;
    const struct file_ops *ops;
    uint64_t offset;
    uint32_t flags;
    uint32_t refs;
    struct mutex lock;
    void *priv;
};

int vfs_open(const char *path, struct vnode *cwd, const struct cred *c, uint32_t flags,
             uint32_t mode, struct file **out);
struct file *file_alloc(const struct file_ops *ops, uint32_t flags);
void file_ref(struct file *f);
void file_put(struct file *f);
int64_t file_read(struct file *f, void *buf, size_t size);
int64_t file_write(struct file *f, const void *buf, size_t size);
int64_t file_seek(struct file *f, int64_t off, int whence);
int64_t file_getdents(struct file *f, void *buf, size_t size);

void ext4_register(void);
void fat_register(void);

#endif
