#include "kernel.h"
#include "cpu.h"
#include "display.h"
#include "serial.h"
#include "spinlock.h"
#include "apic.h"

static spinlock_t console_lock = SPINLOCK_INIT;
static volatile bool console_forced;
static volatile bool console_quiet;

// The kernel log: every kprintf, timestamped, kept in a ring for /dev/kmsg.
#define LOG_SIZE    (256 * 1024)

static char log_buf[LOG_SIZE];
static volatile uint64_t log_head;      // total bytes ever written
static bool log_line_start = true;
static spinlock_t log_lock = SPINLOCK_INIT;

static void log_put(char c)
{
    log_buf[log_head % LOG_SIZE] = c;
    log_head++;
}

static void log_append(const char *s, size_t n)
{
    uint64_t flags = spin_lock_irqsave(&log_lock);

    for (size_t i = 0; i < n; i++) {
        if (log_line_start) {
            char stamp[24];
            uint64_t ms = timer_uptime_ms();
            int len = ksnprintf(stamp, sizeof(stamp), "[%5lu.%03lu] ", ms / 1000, ms % 1000);

            for (int k = 0; k < len; k++)
                log_put(stamp[k]);
            log_line_start = false;
        }
        log_put(s[i]);
        if (s[i] == '\n')
            log_line_start = true;
    }
    spin_unlock_irqrestore(&log_lock, flags);
}

uint64_t klog_head(void)
{
    return log_head;
}

// Copies log bytes starting at *pos (clamped to what is still kept).
size_t klog_read(uint64_t *pos, char *buf, size_t size)
{
    uint64_t flags = spin_lock_irqsave(&log_lock);
    uint64_t head = log_head, start = *pos;
    size_t n = 0;

    if (head > LOG_SIZE && start < head - LOG_SIZE)
        start = head - LOG_SIZE;
    while (start < head && n < size)
        buf[n++] = log_buf[start++ % LOG_SIZE];
    spin_unlock_irqrestore(&log_lock, flags);
    *pos = start;
    return n;
}

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
    log_append(buf, n);
    if (!console_quiet || console_forced)
        console_output(buf, n);
    else
        serial_write(buf, n);
}

// While quiet, kernel messages go only to the log and the serial port.
void console_set_quiet(bool quiet)
{
    console_quiet = quiet;
}

void kprintf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}
