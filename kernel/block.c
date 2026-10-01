#include "block.h"
#include "string.h"

static struct block_device *devices[BLOCK_MAX_DEVICES];
static size_t count;
static spinlock_t lock = SPINLOCK_INIT;

int block_register(struct block_device *dev)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    int ret = -1;

    if (count < BLOCK_MAX_DEVICES) {
        devices[count++] = dev;
        ret = 0;
    }
    spin_unlock_irqrestore(&lock, flags);
    if (ret == 0)
        kprintf("Block: %s, %lu MiB (%u-byte sectors)\n", dev->name,
                dev->sector_count * dev->sector_size >> 20, dev->sector_size);
    return ret;
}

size_t block_count(void)
{
    return count;
}

struct block_device *block_at(size_t index)
{
    return index < count ? devices[index] : NULL;
}

struct block_device *block_find(const char *name)
{
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(devices[i]->name, name))
            return devices[i];
    }
    return NULL;
}

static int transfer(struct block_device *dev, uint64_t offset, void *buf, size_t size, bool write)
{
    uint64_t lba, sectors;
    int ret = 0;

    if (offset % dev->sector_size || size % dev->sector_size)
        return -1;
    lba = offset / dev->sector_size;
    sectors = size / dev->sector_size;
    if (lba > dev->sector_count || sectors > dev->sector_count - lba)
        return -1;

    while (dev->parent) {
        lba += dev->start_lba;
        dev = dev->parent;
    }

    mutex_lock(&dev->lock);
    while (sectors && ret == 0) {
        uint32_t n = MIN(sectors, dev->max_sectors);

        ret = write ? dev->ops->write(dev, lba, n, buf) : dev->ops->read(dev, lba, n, buf);
        lba += n;
        sectors -= n;
        buf = (uint8_t *)buf + (uint64_t)n * dev->sector_size;
    }
    mutex_unlock(&dev->lock);
    return ret;
}

int block_read(struct block_device *dev, uint64_t offset, void *buf, size_t size)
{
    return transfer(dev, offset, buf, size, false);
}

int block_write(struct block_device *dev, uint64_t offset, const void *buf, size_t size)
{
    return transfer(dev, offset, (void *)buf, size, true);
}

int block_flush(struct block_device *dev)
{
    int ret;

    while (dev->parent)
        dev = dev->parent;
    if (!dev->ops->flush)
        return 0;
    mutex_lock(&dev->lock);
    ret = dev->ops->flush(dev);
    mutex_unlock(&dev->lock);
    return ret;
}
