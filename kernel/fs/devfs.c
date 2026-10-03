#include "devfs.h"
#include "input.h"
#include "mem.h"
#include "random.h"
#include "string.h"
#include "tty.h"
#include "process.h"
#include "sched.h"
#include "abi/poll.h"

// /dev: a flat, in-memory directory of device nodes. Opening a node asks its
// driver for a struct file with the driver's own file_ops.

#define MAX_NODES   64
#define ROOT_INO    1

struct node {
    char name[32];
    uint32_t mode, uid, gid;
    devfs_open_fn open;
    void *ctx;
    uint64_t (*size)(void *ctx);    // block devices: bytes
};

static struct node nodes[MAX_NODES];
static int nnodes;
static int64_t mount_time;

int devfs_register(const char *name, uint32_t mode, uint32_t uid, uint32_t gid,
                   devfs_open_fn open, void *ctx)
{
    struct node *n;

    if (nnodes == MAX_NODES)
        return -ENOSPC;
    n = &nodes[nnodes++];
    memcpy(n->name, name, strnlen(name, sizeof(n->name) - 1));
    n->mode = (mode & S_IFMT ? mode & S_IFMT : S_IFCHR) | (mode & 07777);
    n->uid = uid;
    n->gid = gid;
    n->open = open;
    n->ctx = ctx;
    return 0;
}

int devfs_set_size_fn(const char *name, uint64_t (*size)(void *ctx))
{
    for (int i = 0; i < nnodes; i++) {
        if (!strcmp(nodes[i].name, name)) {
            nodes[i].size = size;
            return 0;
        }
    }
    return -ENOENT;
}

static int read_vnode(struct mount *m, uint64_t ino, struct vnode *v)
{
    (void)m;
    v->atime = v->mtime = v->ctime = mount_time;
    if (ino == ROOT_INO) {
        v->mode = S_IFDIR | 0755;
        v->nlink = 2;
        return 0;
    }
    if (ino < 2 || ino - 2 >= (uint64_t)nnodes)
        return -ENOENT;
    v->mode = nodes[ino - 2].mode;
    v->size = nodes[ino - 2].size ? nodes[ino - 2].size(nodes[ino - 2].ctx) : 0;
    v->uid = nodes[ino - 2].uid;
    v->gid = nodes[ino - 2].gid;
    v->nlink = 1;
    return 0;
}

static int lookup(struct vnode *dir, const char *name, size_t len, uint64_t *ino)
{
    (void)dir;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) {
        *ino = ROOT_INO;
        return 0;
    }
    for (int i = 0; i < nnodes; i++) {
        if (strlen(nodes[i].name) == len && !memcmp(nodes[i].name, name, len)) {
            *ino = i + 2;
            return 0;
        }
    }
    return -ENOENT;
}

static int readdir(struct vnode *dir, uint64_t *pos, struct vfs_dirent *out)
{
    (void)dir;
    if (*pos >= (uint64_t)nnodes)
        return 0;
    out->ino = *pos + 2;
    out->type = S_ISBLK(nodes[*pos].mode) ? DT_BLK : DT_CHR;
    out->namelen = strlen(nodes[*pos].name);
    memcpy(out->name, nodes[*pos].name, out->namelen + 1);
    (*pos)++;
    return 1;
}

static int setattr(struct vnode *v, const struct vattr *a, uint32_t mask)
{
    struct node *n;

    if (v->ino < 2)
        return -EPERM;
    n = &nodes[v->ino - 2];
    if (mask & VATTR_MODE)
        n->mode = v->mode = (n->mode & S_IFMT) | (a->mode & 07777);
    if (mask & VATTR_UID)
        n->uid = v->uid = a->uid;
    if (mask & VATTR_GID)
        n->gid = v->gid = a->gid;
    return 0;
}

static int statfs(struct mount *m, struct aegis_statfs *out)
{
    (void)m;
    memset(out, 0, sizeof(*out));
    out->block_size = PAGE_SIZE;
    out->files = nnodes;
    memcpy(out->fstype, "devfs", 6);
    return 0;
}

static int open(struct vnode *v, uint32_t flags, struct file **out)
{
    struct node *n;

    if (v->ino < 2)
        return 0;
    n = &nodes[v->ino - 2];
    return n->open(n->ctx, flags, out);
}

static const struct fs_ops devfs_ops = {
    .read_vnode = read_vnode,
    .lookup = lookup,
    .readdir = readdir,
    .setattr = setattr,
    .statfs = statfs,
    .open = open,
};

static int devfs_mount(struct block_device *dev, bool readonly, struct mount *m)
{
    int err = 0;

    (void)dev;
    (void)readonly;
    m->ops = &devfs_ops;
    m->root = vget(m, ROOT_INO, &err);
    return m->root ? 0 : err;
}

// ---- Built-in devices ----

static int64_t null_read(struct file *f, void *buf, size_t size)
{
    (void)f; (void)buf; (void)size;
    return 0;
}

static int64_t sink_write(struct file *f, const void *buf, size_t size)
{
    (void)f; (void)buf;
    return size;
}

static int64_t zero_read(struct file *f, void *buf, size_t size)
{
    (void)f;
    memset(buf, 0, size);
    return size;
}

static int64_t random_read(struct file *f, void *buf, size_t size)
{
    (void)f;
    random_bytes(buf, size);
    return size;
}

static int64_t random_write(struct file *f, const void *buf, size_t size)
{
    const uint8_t *p = buf;

    (void)f;
    for (size_t i = 0; i < size; i += 8) {
        uint64_t v = 0;
        memcpy(&v, p + i, MIN((size_t)8, size - i));
        random_mix(v);
    }
    return size;
}

static const struct file_ops null_ops = { .read = null_read, .write = sink_write };
static const struct file_ops zero_ops = { .read = zero_read, .write = sink_write };
static const struct file_ops random_ops = { .read = random_read, .write = random_write };

static int open_simple(void *ctx, uint32_t flags, struct file **out)
{
    *out = file_alloc(ctx, flags);
    return *out ? 0 : -ENOMEM;
}

static int open_tty(void *ctx, uint32_t flags, struct file **out)
{
    (void)ctx;
    (void)flags;
    *out = tty_open();
    return *out ? 0 : -ENOMEM;
}

static int open_input(void *ctx, uint32_t flags, struct file **out)
{
    (void)ctx;
    if ((flags & O_ACCMODE) != O_RDONLY)
        return -EINVAL;
    *out = input_open();
    return *out ? 0 : -ENOMEM;
}

// /dev/kmsg: the kernel log, from the oldest message still kept.
static struct wait_queue kmsg_wq = WAIT_QUEUE_INIT;

static int64_t kmsg_read(struct file *f, void *buf, size_t size)
{
    for (;;) {
        size_t n;

        wait_prepare();
        if ((n = klog_read(&f->offset, buf, size)))
            return n;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        uint64_t flags = spin_lock_irqsave(&kmsg_wq.lock);
        wait_queue_sleep_locked(&kmsg_wq);
        irq_restore(flags);
    }
}

static uint32_t kmsg_poll(struct file *f, struct poll_table *pt)
{
    poll_wait(pt, &kmsg_wq);
    return f->offset < klog_head() ? POLLIN : 0;
}

// Root's programs (services) add lines to the log by writing here.
static int64_t kmsg_write(struct file *f, const void *buf, size_t size)
{
    char line[512];
    size_t n = MIN(size, sizeof(line) - 1);

    (void)f;
    memcpy(line, buf, n);
    line[n] = 0;
    // One message per write; drop a trailing newline, add our own.
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)line[i] < 0x20 && line[i] != '\t')
            line[i] = ' ';
    if (n)
        kprintf("%s\n", line);
    return size;
}

static const struct file_ops kmsg_ops = { .read = kmsg_read, .write = kmsg_write, .poll = kmsg_poll };

// Waking readers from kprintf itself is unsafe (it runs under scheduler
// locks), so a thread announces new log data instead.
static void kmsg_thread(void *arg)
{
    uint64_t seen = 0;

    (void)arg;
    for (;;) {
        sched_sleep(100);
        if (klog_head() != seen) {
            seen = klog_head();
            wake_up(&kmsg_wq);
        }
    }
}

static struct filesystem devfs = { .name = "devfs", .mount = devfs_mount };

void devfs_register_fs(void)
{
    vfs_register(&devfs);
    devfs_register("null", 0666, 0, 0, open_simple, (void *)&null_ops);
    devfs_register("zero", 0666, 0, 0, open_simple, (void *)&zero_ops);
    devfs_register("random", 0666, 0, 0, open_simple, (void *)&random_ops);
    devfs_register("urandom", 0666, 0, 0, open_simple, (void *)&random_ops);
    devfs_register("tty", 0666, 0, 0, open_tty, NULL);
    devfs_register("console", 0600, 0, 0, open_tty, NULL);
    // Raw key events would let any program log keystrokes, so only root and
    // the "input" group (the window system) may read them.
    devfs_register("input", 0640, 0, DEVFS_GID_INPUT, open_input, NULL);
    // The kernel log is for administrators (group adm) and the log viewer.
    devfs_register("kmsg", 0640, 0, DEVFS_GID_ADM, open_simple, (void *)&kmsg_ops);
    thread_create("kmsgd", kmsg_thread, NULL);
}

void devfs_set_time(int64_t now)
{
    mount_time = now;
}
