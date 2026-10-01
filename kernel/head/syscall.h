#ifndef AEGIS_SYSCALL_H
#define AEGIS_SYSCALL_H

#include "kernel.h"
#include "abi/syscall.h"

struct syscall_frame {
    uint64_t r15, r14, r13, r12, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rbx, rax;
    uint64_t r11, rcx, user_rsp;
};

void syscall_init(void);
void syscall_set_kernel_stack(uint64_t rsp);
void syscall_dispatch(struct syscall_frame *frame);

#endif
