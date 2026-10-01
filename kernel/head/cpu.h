#ifndef AEGIS_CPU_H
#define AEGIS_CPU_H

#include "kernel.h"

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

#define VECTOR_TIMER    0x20
#define VECTOR_ISA_BASE 0x30
#define VECTOR_YIELD    0x81
#define VECTOR_SPURIOUS 0xFF

#define MSR_APIC_BASE   0x1B
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

struct cpu;

void cpu_init(void);
void cpu_setup(struct cpu *c);
void tss_set_kernel_stack(uint64_t rsp0);
void irq_register(uint8_t vector, irq_handler_t handler);
uint64_t isr_dispatch(struct interrupt_frame *frame);
NORETURN void exception_report(struct interrupt_frame *frame);
const char *exception_report_name(uint64_t vector);

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void cli(void)
{
    __asm__ volatile ("cli" : : : "memory");
}

static inline void sti(void)
{
    __asm__ volatile ("sti" : : : "memory");
}

static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & 0x200)
        sti();
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint64_t read_cr0(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val) : "memory");
}

static inline uint64_t read_cr4(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val) : "memory");
}

static inline uint64_t read_cr2(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline void invlpg(uint64_t addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline NORETURN void halt_forever(void)
{
    for (;;)
        __asm__ volatile ("cli; hlt");
}

#endif
