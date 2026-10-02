#include "cpu.h"
#include "apic.h"
#include "mem.h"
#include "percpu.h"
#include "process.h"
#include "sched.h"
#include "vm.h"

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

#define IST_DOUBLE_FAULT    1
#define IST_STACK_SIZE      (16 * 1024)

#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

extern const uint64_t isr_table[256];

struct cpu cpus[CPU_MAX];
uint32_t cpu_count = 1;

static struct idt_entry idt[256];
static irq_handler_t handlers[256];
static uint8_t bsp_df_stack[IST_STACK_SIZE] __attribute__((aligned(16)));

static void gdt_init(struct cpu *c)
{
    uint64_t *gdt = c->gdt;
    struct tss *tss = &c->tss;
    uint64_t base = (uint64_t)tss;
    uint64_t limit = sizeof(*tss) - 1;
    struct gdt_ptr ptr = { sizeof(c->gdt) - 1, (uint64_t)gdt };

    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFF;
    gdt[2] = 0x00CF92000000FFFF;
    gdt[3] = 0x00CFF2000000FFFF;
    gdt[4] = 0x00AFFA000000FFFF;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40)
           | (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    tss->ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(c->df_stack + IST_STACK_SIZE);
    tss->iomap_base = sizeof(*tss);

    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov %2, %%ds\n\t"
        "mov %2, %%es\n\t"
        "mov %2, %%ss\n\t"
        "mov %3, %%fs\n\t"
        "mov %3, %%gs\n\t"
        "ltr %4"
        : : "m"(ptr), "i"(GDT_KERNEL_CODE), "r"((uint16_t)GDT_KERNEL_DATA),
            "r"((uint16_t)0), "r"((uint16_t)GDT_TSS)
        : "rax", "memory");
}

static void idt_load(void)
{
    struct gdt_ptr ptr = { sizeof(idt) - 1, (uint64_t)idt };

    __asm__ volatile ("lidt %0" : : "m"(ptr) : "memory");
}

static void idt_init(void)
{
    for (unsigned v = 0; v < 256; v++) {
        uint64_t handler = isr_table[v];

        idt[v] = (struct idt_entry) {
            .offset_low = handler & 0xFFFF,
            .selector = GDT_KERNEL_CODE,
            .ist = v == 8 ? IST_DOUBLE_FAULT : 0,
            .type = 0x8E,
            .offset_mid = (handler >> 16) & 0xFFFF,
            .offset_high = handler >> 32,
        };
    }
}

// Loads the CPU's own GDT and TSS and points %gs at its struct cpu.
void cpu_setup(struct cpu *c)
{
    // FPU and SSE for user programs: no emulation, native error reporting,
    // FXSAVE/FXRSTOR and SSE exceptions enabled.
    write_cr0((read_cr0() & ~((1ULL << 2) | (1ULL << 3))) | (1ULL << 1) | (1ULL << 5));
    write_cr4(read_cr4() | (1ULL << 9) | (1ULL << 10));

    c->self = c;
    gdt_init(c);
    idt_load();
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    c->online = true;
}

void cpu_init(void)
{
    struct cpu *bsp = &cpus[0];

    bsp->id = 0;
    bsp->df_stack = bsp_df_stack;
    idt_init();
    cpu_setup(bsp);
    fpu_init();
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    this_cpu()->tss.rsp[0] = rsp0;
}

void irq_register(uint8_t vector, irq_handler_t handler)
{
    handlers[vector] = handler;
}

static int exception_signal(uint64_t vector)
{
    switch (vector) {
    case 0: case 16: case 19:   return SIGFPE;
    case 1: case 3:             return SIGTRAP;
    case 6:                     return SIGILL;
    case 17:                    return SIGBUS;
    }
    return SIGSEGV;
}

// Registers and likely return addresses (words on the stack that point into
// the program's code) of a crashed thread, for the log and the debugger.
static void report_user_state(struct interrupt_frame *f)
{
    uint64_t words[96];
    int shown = 0;

    kprintf("  crash: rax=%lx rbx=%lx rcx=%lx rdx=%lx rsi=%lx rdi=%lx\n", f->rax, f->rbx, f->rcx, f->rdx,
            f->rsi, f->rdi);
    kprintf("  crash: rsp=%lx rbp=%lx r12=%lx r13=%lx r14=%lx r15=%lx\n", f->rsp, f->rbp, f->r12, f->r13,
            f->r14, f->r15);
    if (copy_from_user(words, f->rsp, sizeof(words)))
        return;
    kprintf("  crash: called from");
    for (int i = 0; i < 96 && shown < 10; i++)
        if (words[i] >= USER_REGION_BASE && words[i] < USER_REGION_BASE + 0x10000000ULL) {
            kprintf(" %lx", words[i]);
            shown++;
        }
    kprintf("\n");
}

static void user_exception(struct interrupt_frame *frame)
{
    struct process *p = process_current();
    struct thread *t = sched_current();
    int sig = exception_signal(frame->vector);
    struct aegis_sigaction *act;

    if (frame->vector == 14) {
        bool ok;

        sti();
        ok = vm_fault(p ? p->mm : NULL, read_cr2(), frame->error_code & 2);
        cli();
        if (ok)
            return;
    }
    if (!p)
        exception_report(frame);
    act = &p->sigactions[sig - 1];
    // A fault that is blocked or ignored would repeat forever: take the
    // default action instead.
    if ((t->sig_mask & SIGBIT(sig)) || act->handler == SIG_IGN) {
        t->sig_mask &= ~SIGBIT(sig);
        act->handler = SIG_DFL;
    }
    if (act->handler == SIG_DFL) {
        kprintf("%s (pid %d, thread %lu) crashed: %s at 0x%lx", p->name, p->pid, t->id,
                exception_report_name(frame->vector), frame->rip);
        if (frame->vector == 14)
            kprintf(", address 0x%lx (%s, %s)", read_cr2(), frame->error_code & 1 ? "protection" : "not present",
                    frame->error_code & 2 ? "write" : "read");
        kprintf("\n");
        sti();
        report_user_state(frame);
        cli();
    }
    signal_thread(t, sig);
}

uint64_t isr_dispatch(struct interrupt_frame *frame)
{
    uint64_t vector = frame->vector;
    bool user = (frame->cs & 3) == 3;

    if (vector < 32) {
        if (user)
            user_exception(frame);
        else if (!(vector == 14 && vm_kernel_fault(frame, read_cr2())))
            exception_report(frame);
        if (user)
            signal_deliver(frame);
        return (uint64_t)frame;
    }
    if (vector == VECTOR_SPURIOUS)
        return (uint64_t)frame;
    if (vector == VECTOR_YIELD)
        return sched_switch(frame);

    if (handlers[vector])
        handlers[vector](frame);
    apic_eoi();

    if (user)
        signal_deliver(frame);
    if (vector == VECTOR_TIMER)
        return sched_tick(frame);
    return (uint64_t)frame;
}
