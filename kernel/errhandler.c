#include "kernel.h"
#include "cpu.h"
#include "display.h"
#include "process.h"
#include "sched.h"
#include "smp.h"

static const char *const exception_names[32] = {
    "Divide error", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point error", "Alignment check", "Machine check", "SIMD floating-point error",
    "Virtualization exception", "Control protection exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection exception", "VMM communication exception", "Security exception", "Reserved",
};

static bool panicking;

static void panic_begin(void)
{
    cli();
    if (__atomic_exchange_n(&panicking, true, __ATOMIC_ACQ_REL))
        halt_forever();
    smp_halt_others();
    console_force();

    console_set_color(COLOR_WHITE, COLOR_RED);
    console_clear();
    kprintf("\n  *** AEGIS KERNEL PANIC ***\n\n");
}

static NORETURN void panic_end(void)
{
    kprintf("\n  System halted.\n");
    halt_forever();
}

void panic(const char *fmt, ...)
{
    va_list args;

    panic_begin();
    kprintf("  ");
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
    kprintf("\n");
    panic_end();
}

void exception_report(struct interrupt_frame *f)
{
    if ((f->cs & 3) == 3) {
        struct thread *t = sched_current();

        kprintf("Thread %lu (%s) killed: %s at 0x%lx", t->id, t->name,
                exception_names[f->vector], f->rip);
        if (f->vector == 14)
            kprintf(", address 0x%lx", read_cr2());
        kprintf("\n");
        process_exit(-11);
    }

    panic_begin();
    kprintf("  %s (vector %lu, error code 0x%lx)\n\n",
            exception_names[f->vector], f->vector, f->error_code);

    if (f->vector == 14)
        kprintf("  Faulting address: 0x%016lx (%s, %s, %s)\n\n", read_cr2(),
                f->error_code & 1 ? "protection" : "not present",
                f->error_code & 2 ? "write" : "read",
                f->error_code & 4 ? "user" : "kernel");

    kprintf("  RIP=%016lx  CS=%04lx  RFLAGS=%016lx\n", f->rip, f->cs, f->rflags);
    kprintf("  RSP=%016lx  SS=%04lx  CR3=%016lx\n\n", f->rsp, f->ss, read_cr3());
    kprintf("  RAX=%016lx  RBX=%016lx  RCX=%016lx\n", f->rax, f->rbx, f->rcx);
    kprintf("  RDX=%016lx  RSI=%016lx  RDI=%016lx\n", f->rdx, f->rsi, f->rdi);
    kprintf("  RBP=%016lx  R8 =%016lx  R9 =%016lx\n", f->rbp, f->r8, f->r9);
    kprintf("  R10=%016lx  R11=%016lx  R12=%016lx\n", f->r10, f->r11, f->r12);
    kprintf("  R13=%016lx  R14=%016lx  R15=%016lx\n", f->r13, f->r14, f->r15);
    panic_end();
}
