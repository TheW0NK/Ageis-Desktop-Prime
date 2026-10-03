#ifndef AEGIS_BLOCK_H
#define AEGIS_BLOCK_H

#include "kernel.h"
#include "sync.h"

#define BLOCK_MAX_DEVICES   32
#define BLOCK_NAME_MAX      16

struct block_device;

struct block_ops {
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct block_device *dev);
};

struct block_device {
    char name[BLOCK_NAME_MAX];
    uint32_t sector_size;
    uint64_t sector_count;
    uint32_t max_sectors;           // per request
    const struct block_ops *ops;
    void *driver;
    struct mutex lock;

    struct block_device *parent;    // set for partitions
    uint64_t start_lba;
    uint8_t type_guid[16];
    uint8_t part_guid[16];
};

int block_register(struct block_device *dev);
size_t block_count(void);
struct block_device *block_at(size_t index);
struct block_device *block_find(const char *name);

// Byte-addressed I/O; offset and size must be sector aligned.
int block_read(struct block_device *dev, uint64_t offset, void *buf, size_t size);
int block_write(struct block_device *dev, uint64_t offset, const void *buf, size_t size);
int block_flush(struct block_device *dev);

void partition_scan(struct block_device *disk);
void guid_format(const uint8_t guid[16], char out[37]);
bool guid_parse(const char *s, uint8_t guid[16]);

void ahci_init(void);
void nvme_init(void);
void virtio_blk_init(void);
void ramdisk_init(uint64_t base, uint64_t size);
struct block_device *ramdisk_device(void);    // NULL without one

#endif
