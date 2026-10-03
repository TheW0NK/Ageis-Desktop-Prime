#ifndef AEGIS_DEVFS_H
#define AEGIS_DEVFS_H

#include "vfs.h"

#define DEVFS_GID_ADM       4
#define DEVFS_GID_INPUT     50
#define DEVFS_GID_AUDIO     63

// Returns 0 and a new file in *out, or a negative errno.
typedef int (*devfs_open_fn)(void *ctx, uint32_t flags, struct file **out);

void devfs_register_fs(void);
void devfs_set_time(int64_t now);
// mode may carry a file type (S_IFBLK); the default is a character device.
int devfs_register(const char *name, uint32_t mode, uint32_t uid, uint32_t gid,
                   devfs_open_fn open, void *ctx);
// Block devices report their size in bytes through this.
int devfs_set_size_fn(const char *name, uint64_t (*size)(void *ctx));
void blockdev_publish(struct block_device *dev);
void cmdline_devfs_init(void);

#endif
