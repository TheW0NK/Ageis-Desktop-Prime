#ifndef AEGIS_FUTEX_H
#define AEGIS_FUTEX_H

#include "kernel.h"

struct process;

// Sleeps while the 32-bit value at uaddr equals val. timeout_ms of
// UINT64_MAX waits forever. Returns 0, -EAGAIN, -ETIMEDOUT or -EINTR.
int futex_wait(struct process *p, uint64_t uaddr, uint32_t val, uint64_t timeout_ms);
// Wakes up to count waiters; returns how many were woken.
int futex_wake(struct process *p, uint64_t uaddr, int count);

#endif
