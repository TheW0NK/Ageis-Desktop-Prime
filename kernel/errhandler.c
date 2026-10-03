#include "kernel.h"
#include "cpu.h"
#include "display.h"
#include "process.h"
#include "sched.h"
#include "smp.h"
#include "stopcodes.h"
#include "string.h"

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

static bool panicking, graphical;
static char report[8192];

void printk_capture(char *buf, size_t size);
void crash_screen_draw(uint32_t code, const char *report);

// Once the desktop or sign-in screen owns the display, a panic shows the
// graphical crash screen; earlier (while starting up) the red text screen.
static void panic_begin(void)
{
    cli();
    if (__atomic_exchange_n(&panicking, true, __ATOMIC_ACQ_REL))
        halt_forever();
    smp_halt_others();
    console_force();

    graphical = display_claimed() && display_primary();
    if (graphical) {
        printk_capture(report, sizeof(report));
        kprintf("\n  *** AEGIS KERNEL PANIC ***\n\n");
        return;
    }
    // The splash screen hides the console: show it again.
    console_set_hidden(false);
    console_set_color(COLOR_WHITE, COLOR_RED);
    console_clear();
    kprintf("\n  *** AEGIS KERNEL PANIC ***\n\n");
}

// Walks the frame-pointer chain (the kernel keeps frame pointers).
static void backtrace(uint64_t rbp, uint64_t rip)
{
    extern char __kernel_start[], __kernel_end[];

    kprintf("\n  Backtrace (resolve with addr2line -e kernel/build/kernel.elf):\n   ");
    if (rip)
        kprintf(" %lx", rip);
    for (int depth = 0; depth < 24 && rbp >= (uint64_t)__kernel_start - 0x100000000ULL; depth++) {
        uint64_t *frame = (uint64_t *)rbp;

        if (rbp & 7 || rbp < 0x1000)
            break;
        if (frame[1] < (uint64_t)__kernel_start || frame[1] >= (uint64_t)__kernel_end)
            break;
        kprintf(" %lx", frame[1]);
        rbp = frame[0];
    }
    kprintf("\n");
    (void)__kernel_end;
}

static NORETURN void panic_end(uint32_t code)
{
    const struct stop_code *sc = stop_code_find(code);

    if (graphical)
        printk_capture(NULL, 0);    // the screen says this itself
    kprintf("\n  Stop code: %s (0x%03x)\n  System halted.\n", sc->name, code);
    if (graphical) {
        // The screen shows the report without its banner line.
        const char *body = strstr(report, "PANIC ***");

        crash_screen_draw(code, body ? body + 9 : report);
    }
    halt_forever();
}

static NORETURN void panic_va(uint32_t code, const char *fmt, va_list args)
{
    panic_begin();
    kprintf("  ");
    kvprintf(fmt, args);
    kprintf("\n");
    backtrace((uint64_t)__builtin_frame_address(0), 0);
    panic_end(code);
}

// A panic with a stop code (kernel/head/stopcodes.h).
void panic_code(uint32_t code, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    panic_va(code, fmt, args);
}

void panic(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    panic_va(strstr(fmt, "ut of memory") ? STOP_OUT_OF_MEMORY : STOP_KERNEL_PANIC, fmt, args);
}

const char *exception_report_name(uint64_t vector)
{
    return vector < 32 ? exception_names[vector] : "Unknown exception";
}

void exception_report(struct interrupt_frame *f)
{
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
    backtrace(f->rbp, f->rip);
    panic_end((uint32_t)f->vector);
}
