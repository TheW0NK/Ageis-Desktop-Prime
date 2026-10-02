#include "aegis.h"
#include "abi/video.h"

// Cameras: /dev/video<N>, frames of 32-bit XRGB.

int camera_open(int index, struct camera_info *info)
{
    char path[32];
    struct aegis_videoinfo v;
    int fd;

    snprintf(path, sizeof(path), "/dev/video%d", index);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    if (ioctl(fd, IOCTL_VIDEO_INFO, (unsigned long)&v) < 0 || v.format != VIDEO_FORMAT_XRGB32) {
        close(fd);
        errno = ENODEV;
        return -1;
    }
    info->width = v.width;
    info->height = v.height;
    info->fps = v.fps;
    strlcpy(info->name, v.name, sizeof(info->name));
    return fd;
}

int camera_read(int fd, uint32_t *pixels, size_t bytes)
{
    size_t got = 0;

    // A frame arrives in pieces.
    while (got < bytes) {
        ssize_t n = read(fd, (char *)pixels + got, bytes - got);

        if (n <= 0)
            return -1;
        got += n;
    }
    return 0;
}
