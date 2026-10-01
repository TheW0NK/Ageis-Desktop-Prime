#ifndef AEGIS_PIPE_H
#define AEGIS_PIPE_H

#include "vfs.h"

int pipe_create(uint32_t flags, struct file **read_end, struct file **write_end);

#endif
