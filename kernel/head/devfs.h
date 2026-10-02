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
int devfs_register(const char *name, uint32_t mode, uint32_t uid, uint32_t gid,
                   devfs_open_fn open, void *ctx);

#endif
