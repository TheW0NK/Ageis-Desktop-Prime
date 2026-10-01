#include "kernel.h"
#include "cpu.h"
#include "display.h"
#include "serial.h"
#include "spinlock.h"

static spinlock_t console_lock = SPINLOCK_INIT;
static volatile bool console_forced;

// Writes to the serial port and screen as one unit across CPUs.
void console_output(const char *s, size_t n)
{
    uint64_t flags = irq_save();
    bool locked = false;

    for (uint64_t spins = 0; !console_forced; spins++) {
        if (!__atomic_exchange_n(&console_lock.locked, 1, __ATOMIC_ACQUIRE)) {
            locked = true;
            break;
        }
        __asm__ volatile ("pause");
    }
    serial_write(s, n);
    console_write(s, n);
    if (locked)
        spin_unlock(&console_lock);
    irq_restore(flags);
}

// Used by panic: other CPUs are halted, so the lock may never be released.
void console_force(void)
{
    console_forced = true;
}

void kvprintf(const char *fmt, va_list args)
{
    char buf[512];
    int n = kvsnprintf(buf, sizeof(buf), fmt, args);
    if (n >= (int)sizeof(buf))
        n = sizeof(buf) - 1;
    console_output(buf, n);
}

void kprintf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}
