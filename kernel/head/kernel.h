#ifndef AEGIS_KERNEL_H
#define AEGIS_KERNEL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AEGIS_VERSION   "0.2.0"

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

#define NORETURN        __attribute__((noreturn))
#define PRINTF(f, a)    __attribute__((format(printf, f, a)))

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) PRINTF(3, 4);

void kprintf(const char *fmt, ...) PRINTF(1, 2);
void kvprintf(const char *fmt, va_list args);

NORETURN void panic(const char *fmt, ...) PRINTF(1, 2);
void console_output(const char *s, size_t n);
void console_force(void);
void console_set_quiet(bool quiet);
uint64_t klog_head(void);
size_t klog_read(uint64_t *pos, char *buf, size_t size);

#endif
