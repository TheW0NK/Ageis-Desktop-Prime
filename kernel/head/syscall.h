#ifndef AEGIS_SYSCALL_H
#define AEGIS_SYSCALL_H

#include "kernel.h"
#include "cpu.h"
#include "abi/syscall.h"

void syscall_init(void);
void syscall_set_kernel_stack(uint64_t rsp);
// Returns nonzero to make the entry code return with iretq.
int syscall_dispatch(struct interrupt_frame *frame);

#endif
