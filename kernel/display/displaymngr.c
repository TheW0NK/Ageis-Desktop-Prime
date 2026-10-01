#include "display.h"
#include "mem.h"
#include "string.h"
#include "cpu.h"
#include "pci.h"
#include "abi/errno.h"

static struct display displays[DISPLAY_MAX];
static uint32_t count;

static void mask_info(uint32_t mask, uint8_t *shift, uint8_t *bits)
{
    *shift = 0;
    *bits = 0;
    if (!mask)
        return;
    while (!(mask & 1)) {
        mask >>= 1;
        (*shift)++;
    }
    while (mask & 1) {
        mask >>= 1;
        (*bits)++;
    }
}

static uint32_t highest_bit(uint32_t v)
{
    uint32_t n = 0;

    while (v) {
        v >>= 1;
        n++;
    }
    return n;
}

static bool setup(struct display *d, const struct aegis_framebuffer *fb)
{
    switch (fb->format) {
    case AEGIS_FB_RGBX:
        d->red_shift = 0;
        d->green_shift = 8;
        d->blue_shift = 16;
        d->red_bits = d->green_bits = d->blue_bits = 8;
        d->bytes_per_pixel = 4;
        break;
    case AEGIS_FB_BGRX:
        d->red_shift = 16;
        d->green_shift = 8;
        d->blue_shift = 0;
        d->red_bits = d->green_bits = d->blue_bits = 8;
        d->bytes_per_pixel = 4;
        break;
    case AEGIS_FB_BITMASK:
        mask_info(fb->red_mask, &d->red_shift, &d->red_bits);
        mask_info(fb->green_mask, &d->green_shift, &d->green_bits);
        mask_info(fb->blue_mask, &d->blue_shift, &d->blue_bits);
        d->bytes_per_pixel = (highest_bit(fb->red_mask | fb->green_mask | fb->blue_mask
                                          | fb->reserved_mask) + 7) / 8;
        break;
    default:
        return false;
    }

    if (d->bytes_per_pixel < 2 || d->bytes_per_pixel > 4)
        return false;
    if (!fb->base || !fb->width || !fb->height || fb->pixels_per_scanline < fb->width)
        return false;

    d->vram = (volatile uint8_t *)fb->base;
    d->target = (uint8_t *)fb->base;
    d->width = fb->width;
    d->height = fb->height;
    d->pitch = fb->pixels_per_scanline * d->bytes_per_pixel;
    d->present = true;
    return true;
}

void display_init(const struct aegis_framebuffer *fbs, uint32_t n)
{
    for (uint32_t i = 0; i < n && count < DISPLAY_MAX; i++) {
        if (setup(&displays[count], &fbs[i]))
            count++;
    }

    for (uint32_t i = 1; i < count; i++)
        fb_fill_rect(&displays[i], 0, 0, displays[i].width, displays[i].height, COLOR_BLACK);

    console_init(display_primary());
}

void display_enable_backbuffers(void)
{
    for (uint32_t i = 0; i < count; i++) {
        struct display *d = &displays[i];
        size_t size = (size_t)d->pitch * d->height;
        uint8_t *back = kmalloc(size);

        if (!back)
            continue;
        memcpy(back, (const void *)d->vram, size);
        d->back = back;
        d->target = back;
    }
}

uint32_t display_count(void)
{
    return count;
}

struct display *display_get(uint32_t index)
{
    return index < count ? &displays[index] : NULL;
}

struct display *display_primary(void)
{
    return count ? &displays[0] : NULL;
}

#define DISPI_XRES      1
#define DISPI_YRES      2
#define DISPI_BPP       3
#define DISPI_ENABLE    4
#define DISPI_VIRT_W    6
#define DISPI_VIRT_H    7
#define DISPI_X_OFF     8
#define DISPI_Y_OFF     9

static volatile uint16_t *dispi_mmio;

static void dispi_write(uint16_t index, uint16_t value)
{
    if (dispi_mmio) {
        dispi_mmio[index] = value;
    } else {
        outw(0x1CE, index);
        outw(0x1CF, value);
    }
}

// Mode switching for the Bochs/QEMU display adapter. Other GPUs need their
// own drivers, so they keep the mode the bootloader chose.
int display_set_mode(uint32_t width, uint32_t height)
{
    const struct pci_device *pci = pci_find_id(0x1234, 0x1111, 0);
    struct display *d = display_primary();
    uint64_t fb, flags;
    uint8_t *back = NULL;

    if (!pci || !d)
        return -ENOTSUP;
    fb = pci_bar(pci, 0, NULL);
    if ((uint64_t)d->vram != fb)
        return -ENOTSUP;
    if (width < 320 || height < 200 || width > 4096 || height > 4096 || width % 8)
        return -EINVAL;
    if ((uint64_t)width * height * 4 > 16ULL << 20)
        return -EINVAL;
    if (d->back && !(back = kmalloc((size_t)width * height * 4)))
        return -ENOMEM;

    if (!dispi_mmio && pci_bar(pci, 2, NULL)) {
        uint64_t mmio = pci_bar(pci, 2, NULL);
        if (paging_map_mmio(mmio, PAGE_SIZE))
            dispi_mmio = (volatile uint16_t *)(mmio + 0x500);
    }

    flags = irq_save();
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, width);
    dispi_write(DISPI_YRES, height);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_W, width);
    dispi_write(DISPI_VIRT_H, height);
    dispi_write(DISPI_X_OFF, 0);
    dispi_write(DISPI_Y_OFF, 0);
    dispi_write(DISPI_ENABLE, 0x01 | 0x40);

    kfree(d->back);
    d->back = back;
    d->target = back ? back : (uint8_t *)d->vram;
    d->width = width;
    d->height = height;
    d->pitch = width * 4;
    d->bytes_per_pixel = 4;
    d->red_shift = 16;
    d->green_shift = 8;
    d->blue_shift = 0;
    d->red_bits = d->green_bits = d->blue_bits = 8;
    d->dirty_top = d->dirty_bottom = 0;
    console_init(d);
    irq_restore(flags);
    return 0;
}
