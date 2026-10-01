#ifndef AEGIS_TTY_H
#define AEGIS_TTY_H

#include "vfs.h"

void tty_input(const char *s, size_t n);
struct file *tty_open(void);
int64_t tty_read(char *buf, size_t size, bool nonblock);
int64_t tty_write(const char *buf, size_t size);

#endif
