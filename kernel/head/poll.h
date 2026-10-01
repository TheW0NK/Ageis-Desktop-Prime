#ifndef AEGIS_POLL_H
#define AEGIS_POLL_H

#include "vfs.h"
#include "abi/poll.h"

struct process;

// fds is kernel memory; returns the number ready, 0 on timeout, or -errno.
// A negative timeout waits forever.
int do_poll(struct process *p, struct pollfd *fds, int n, int64_t timeout_ms);

#endif
