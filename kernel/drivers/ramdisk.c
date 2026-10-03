#include "block.h"
#include "string.h"
#include "abi/errno.h"

// The ramdisk: a disk image the bootloader loaded into memory (the live
// system on the install media). It is identity mapped like all RAM, and
// writable, so the live system can change files until it is switched off.

static struct block_device ram = { .name = "ramdisk", .kind = "Memory", .lock = MUTEX_INIT };
static uint8_t *ram_base;

static int ram_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (lba + count > dev->sector_count)
        return -EIO;
    memcpy(buf, ram_base + lba * 512, (size_t)count * 512);
    return 0;
}

static int ram_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (lba + count > dev->sector_count)
        return -EIO;
    memcpy(ram_base + lba * 512, buf, (size_t)count * 512);
    return 0;
}

static int ram_flush(struct block_device *dev)
{
    (void)dev;
    return 0;
}

static const struct block_ops ram_ops = { ram_read, ram_write, ram_flush };

void ramdisk_init(uint64_t base, uint64_t size)
{
    if (!base || size < 512)
        return;
    ram_base = (uint8_t *)base;
    ram.sector_size = 512;
    ram.sector_count = size / 512;
    ram.max_sectors = 1024;
    ram.ops = &ram_ops;
    block_register(&ram);
}

struct block_device *ramdisk_device(void)
{
    return ram_base ? &ram : NULL;
}
