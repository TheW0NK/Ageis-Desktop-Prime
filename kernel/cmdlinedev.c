#include "devfs.h"
#include "string.h"

// /dev/cmdline: the kernel command line from the boot entry, for init and
// the recovery tools ("recovery", "safe").

static int64_t cmdline_read(struct file *f, void *buf, size_t size)
{
    const char *s = kernel_cmdline();
    size_t len = strlen(s), n;

    if (f->offset >= len)
        return 0;
    n = MIN(size, len - f->offset);
    memcpy(buf, s + f->offset, n);
    f->offset += n;
    return n;
}

static const struct file_ops cmdline_ops = { .read = cmdline_read };

static int cmdline_open(void *ctx, uint32_t flags, struct file **out)
{
    (void)ctx;
    if (!(*out = file_alloc(&cmdline_ops, flags)))
        return -ENOMEM;
    return 0;
}

void cmdline_devfs_init(void)
{
    devfs_register("cmdline", 0444, 0, 0, cmdline_open, NULL);
}
