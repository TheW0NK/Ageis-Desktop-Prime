#ifndef AEGIS_PERCPU_H
#define AEGIS_PERCPU_H

#include "kernel.h"

#define CPU_MAX         64

struct thread;

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

// The first fields are accessed from assembly through %gs; keep their offsets.
struct cpu {
    struct cpu *self;               // 0
    uint64_t kernel_rsp;            // 8
    uint64_t user_rsp;              // 16
    volatile uint8_t *switch_done;  // 24
    uint32_t id;
    uint32_t apic_id;
    bool online;
    struct thread *current;
    struct thread *idle;
    uint64_t gdt[7];
    struct tss tss;
    uint8_t *df_stack;
};

extern struct cpu cpus[CPU_MAX];
extern uint32_t cpu_count;

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;

    __asm__ volatile ("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#endif
