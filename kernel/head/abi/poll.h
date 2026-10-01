#ifndef AEGIS_ABI_POLL_H
#define AEGIS_ABI_POLL_H

#include <stdint.h>

#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020

struct pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

#endif
