#include "display.h"
#include "devfs.h"
#include "mem.h"
#include "process.h"
#include "string.h"
#include "vm.h"
#include "abi/fb.h"
#include "input.h"

#define DEVFS_GID_VIDEO 44

static struct file *owner;
static spinlock_t lock = SPINLOCK_INIT;

bool display_claimed(void)
{
    return owner != NULL;
}

static void info(struct display *d, struct aegis_fbinfo *i)
{
    memset(i, 0, sizeof(*i));
    i->width = d->width;
    i->height = d->height;
    i->pitch = d->pitch;
    i->bpp = d->bytes_per_pixel * 8;
    i->red_shift = d->red_shift;
    i->green_shift = d->green_shift;
    i->blue_shift = d->blue_shift;
    i->red_bits = d->red_bits;
    i->green_bits = d->green_bits;
    i->blue_bits = d->blue_bits;
    i->size = ALIGN_UP((uint64_t)d->pitch * d->height, PAGE_SIZE);
}

static int64_t fb_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    struct display *d = display_primary();
    struct aegis_fbinfo i;
    int ret;

    if (!d)
        return -ENODEV;
    switch (cmd) {
    case IOCTL_FB_INFO:
        info(d, &i);
        return copy_to_user(arg, &i, sizeof(i));
    case IOCTL_FB_SET_MODE:
        if (f != owner)
            return -EBUSY;
        if ((ret = display_set_mode(arg >> 16, arg & 0xFFFF)))
            return ret;
        // The console redraw that a mode change does must not show.
        if (!input_text_mode())
            console_set_hidden(true);
        return 0;
    case IOCTL_FB_VT_RELEASED:
        // Repaint the console over anything drawn before the owner stopped.
        if (f == owner && input_text_mode())
            console_redraw();
        return 0;
    }
    return -ENOTTY;
}

static int fb_mmap(struct file *f, struct vma *v)
{
    struct display *d = display_primary();
    uint64_t size;

    (void)f;
    if (!d)
        return -ENODEV;
    size = ALIGN_UP((uint64_t)d->pitch * d->height, PAGE_SIZE);
    if (v->offset + (v->end - v->start) > size && v->end)
        return -EINVAL;
    v->type = VMA_PHYS;
    v->offset += (uint64_t)d->vram;
    return 0;
}

static void fb_close(struct file *f)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    bool was_owner = owner == f;

    if (was_owner)
        owner = NULL;
    spin_unlock_irqrestore(&lock, flags);
    if (was_owner && !splash_active())
        console_set_hidden(false);
}

static const struct file_ops fb_ops = { .ioctl = fb_ioctl, .close = fb_close, .mmap = fb_mmap };

static int fb_open(void *ctx, uint32_t flags, struct file **out)
{
    struct file *f;
    uint64_t irq;

    (void)ctx;
    if (!display_primary())
        return -ENODEV;
    if (!(f = file_alloc(&fb_ops, flags)))
        return -ENOMEM;
    if ((flags & O_ACCMODE) != O_RDONLY) {
        irq = spin_lock_irqsave(&lock);
        if (owner) {
            spin_unlock_irqrestore(&lock, irq);
            kfree(f);
            return -EBUSY;
        }
        owner = f;
        spin_unlock_irqrestore(&lock, irq);
        if (!input_text_mode()) {
            console_set_hidden(true);
            splash_user_output();
        }
    }
    *out = f;
    return 0;
}

void display_devfs_init(void)
{
    devfs_register("fb0", 0660, 0, DEVFS_GID_VIDEO, fb_open, NULL);
}
