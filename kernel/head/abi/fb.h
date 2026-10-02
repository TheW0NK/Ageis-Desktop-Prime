#ifndef AEGIS_ABI_FB_H
#define AEGIS_ABI_FB_H

#include <stdint.h>

// /dev/fb0: the primary display. Opening it for writing takes the display
// over from the text console until the descriptor is closed. mmap it with
// MAP_SHARED to draw; pixels are 32-bit with the channel shifts below.

struct aegis_fbinfo {
    uint32_t width, height;
    uint32_t pitch;                 // bytes per row
    uint32_t bpp;                   // bits per pixel (32, 24 or 16)
    uint8_t red_shift, green_shift, blue_shift;
    uint8_t red_bits, green_bits, blue_bits;
    uint16_t reserved;
    uint64_t size;                  // bytes to map
};

#define IOCTL_FB_INFO       0x200   // arg: struct aegis_fbinfo *
#define IOCTL_FB_SET_MODE   0x201   // arg: (width << 16) | height; remap afterwards
#define IOCTL_FB_VT_RELEASED 0x202  // the owner stopped drawing after SYN_VT_LEAVE

#endif
