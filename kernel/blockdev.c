#include "block.h"
#include "devfs.h"
#include "mem.h"
#include "string.h"
#include "vm.h"
#include "abi/block.h"

// /osystem/devices nodes for disks and partitions: raw sector access for the installer
// and disk tools (root only).

#define BOUNCE  (64 * 1024)

static uint64_t dev_size(void *ctx)
{
    struct block_device *d = ctx;

    return d->sector_count * d->sector_size;
}

// buf is a kernel buffer: the system call layer copies to and from the caller.
static int64_t transfer(struct file *f, uint64_t ubuf, size_t size, bool write)
{
    struct block_device *d = f->priv;
    uint8_t *bounce;
    size_t done = 0;
    int64_t ret = 0;

    if (f->offset % d->sector_size || size % d->sector_size)
        return -EINVAL;
    if (f->offset >= dev_size(d))
        return 0;
    size = MIN(size, dev_size(d) - f->offset);
    if (!(bounce = kmalloc(BOUNCE)))
        return -ENOMEM;
    mutex_lock(&f->lock);
    while (done < size) {
        size_t n = MIN(size - done, BOUNCE);

        if (write) {
            memcpy(bounce, (const uint8_t *)ubuf + done, n);
            if (block_write(d, f->offset, bounce, n)) {
                ret = -EIO;
                break;
            }
        } else if (block_read(d, f->offset, bounce, n)) {
            ret = -EIO;
            break;
        } else {
            memcpy((uint8_t *)ubuf + done, bounce, n);
        }
        f->offset += n;
        done += n;
    }
    mutex_unlock(&f->lock);
    kfree(bounce);
    return done ? (int64_t)done : ret;
}

static int64_t bd_read(struct file *f, void *buf, size_t size)
{
    return transfer(f, (uint64_t)buf, size, false);
}

static int64_t bd_write(struct file *f, const void *buf, size_t size)
{
    return transfer(f, (uint64_t)buf, size, true);
}

static int64_t bd_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    struct block_device *d = f->priv;

    switch (cmd) {
    case IOCTL_BLOCK_INFO: {
        struct aegis_blockinfo bi;

        memset(&bi, 0, sizeof(bi));
        bi.sector_count = d->sector_count;
        bi.sector_size = d->sector_size;
        bi.start_lba = d->start_lba;
        if (d->parent) {
            bi.flags |= BLOCK_INFO_PARTITION;
            memcpy(bi.parent, d->parent->name, sizeof(bi.parent));
        }
        if (d == ramdisk_device())
            bi.flags |= BLOCK_INFO_RAMDISK;
        if (vfs_device_mounted(d))
            bi.flags |= BLOCK_INFO_MOUNTED;
        memcpy(bi.type_guid, d->type_guid, 16);
        memcpy(bi.part_guid, d->part_guid, 16);
        if (d->kind)
            memcpy(bi.kind, d->kind, strlen(d->kind) < sizeof(bi.kind) ? strlen(d->kind) : sizeof(bi.kind) - 1);
        return copy_to_user(arg, &bi, sizeof(bi)) ? -EFAULT : 0;
    }
    case IOCTL_BLOCK_RESCAN:
        if (d->parent)
            return -EINVAL;
        if (vfs_device_mounted(d))
            return -EBUSY;
        partition_scan(d);
        return 0;
    case IOCTL_BLOCK_FLUSH:
        return block_flush(d) ? -EIO : 0;
    }
    return -ENOTTY;
}

static const struct file_ops bd_ops = { .read = bd_read, .write = bd_write, .ioctl = bd_ioctl };

static int bd_open(void *ctx, uint32_t flags, struct file **out)
{
    struct file *f = file_alloc(&bd_ops, flags);

    if (!f)
        return -ENOMEM;
    f->priv = ctx;
    *out = f;
    return 0;
}

void blockdev_publish(struct block_device *dev)
{
    if (devfs_register(dev->name, S_IFBLK | 0600, 0, 0, bd_open, dev) == 0)
        devfs_set_size_fn(dev->name, dev_size);
}
