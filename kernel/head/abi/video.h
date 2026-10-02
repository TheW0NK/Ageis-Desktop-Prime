#ifndef AEGIS_ABI_VIDEO_H
#define AEGIS_ABI_VIDEO_H

#include <stdint.h>

// /dev/video0, /dev/video1, ...: cameras. read() waits for the next frame
// and copies it whole (width * height * 4 bytes of XRGB, row by row).

#define IOCTL_VIDEO_INFO    0x400   // arg: struct aegis_videoinfo *

#define VIDEO_FORMAT_XRGB32 1

struct aegis_videoinfo {
    uint32_t width, height, format, fps;
    char name[48];
};

#endif
