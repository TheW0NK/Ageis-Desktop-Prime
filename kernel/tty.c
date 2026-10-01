#include "tty.h"
#include "abi/syscall.h"
#include "display.h"
#include "process.h"
#include "serial.h"
#include "string.h"
#include "sync.h"

#define READY_SIZE  4096
#define LINE_MAX    1024

static struct {
    struct wait_queue q;            // q.lock guards everything here
    char ready[READY_SIZE];
    size_t head, tail;
    char line[LINE_MAX];
    size_t line_len;
    bool raw;
    int eof;
    int esc;
    int fg_pid;
} tty = { .q = WAIT_QUEUE_INIT };

static void echo(const char *s, size_t n)
{
    console_output(s, n);
}

static void push(char c)
{
    size_t next = (tty.head + 1) % READY_SIZE;

    if (next != tty.tail) {
        tty.ready[tty.head] = c;
        tty.head = next;
    }
}

static void input_char(char c)
{
    if (c == 0x03 && tty.fg_pid) {
        process_kill(tty.fg_pid, NULL);
        if (!tty.raw) {
            tty.line_len = 0;
            echo("^C\n", 3);
        }
        return;
    }
    if (tty.raw) {
        push(c);
        return;
    }

    if (tty.esc) {
        if (tty.esc == 1)
            tty.esc = c == '[' ? 2 : 0;
        else if ((c >= 'A' && c <= 'Z') || c == '~')
            tty.esc = 0;
        return;
    }
    switch (c) {
    case 0x1B:
        tty.esc = 1;
        return;
    case '\r':
    case '\n':
        for (size_t i = 0; i < tty.line_len; i++)
            push(tty.line[i]);
        push('\n');
        tty.line_len = 0;
        echo("\n", 1);
        return;
    case '\b':
    case 0x7F:
        if (tty.line_len) {
            tty.line_len--;
            echo("\b \b", 3);
        }
        return;
    case 0x04:
        for (size_t i = 0; i < tty.line_len; i++)
            push(tty.line[i]);
        if (tty.line_len == 0)
            tty.eof++;
        tty.line_len = 0;
        return;
    case 0x15:
        while (tty.line_len) {
            tty.line_len--;
            echo("\b \b", 3);
        }
        return;
    }
    if ((unsigned char)c >= 0x20 && tty.line_len < LINE_MAX - 1) {
        tty.line[tty.line_len++] = c;
        echo(&c, 1);
    }
}

void tty_input(const char *s, size_t n)
{
    uint64_t flags = spin_lock_irqsave(&tty.q.lock);

    while (n--)
        input_char(*s++);
    wake_up_locked(&tty.q);
    spin_unlock_irqrestore(&tty.q.lock, flags);
}

int64_t tty_read(char *buf, size_t size)
{
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&tty.q.lock);
    size_t n = 0;

    while (tty.head == tty.tail && !tty.eof) {
        if (p && p->killed) {
            spin_unlock_irqrestore(&tty.q.lock, flags);
            return -EINTR;
        }
        wait_queue_sleep_locked(&tty.q);
        spin_lock(&tty.q.lock);
    }
    if (tty.head == tty.tail) {
        tty.eof--;
        spin_unlock_irqrestore(&tty.q.lock, flags);
        return 0;
    }
    while (n < size && tty.head != tty.tail) {
        char c = tty.ready[tty.tail];

        tty.tail = (tty.tail + 1) % READY_SIZE;
        buf[n++] = c;
        if (!tty.raw && c == '\n')
            break;
    }
    spin_unlock_irqrestore(&tty.q.lock, flags);
    return n;
}

int64_t tty_write(const char *buf, size_t size)
{
    echo(buf, size);
    return size;
}

static int64_t f_read(struct file *f, void *buf, size_t size)
{
    (void)f;
    return tty_read(buf, size);
}

static int64_t f_write(struct file *f, const void *buf, size_t size)
{
    (void)f;
    return tty_write(buf, size);
}

static int64_t f_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    uint64_t flags;
    uint32_t rows, cols;

    (void)f;
    switch (cmd) {
    case IOCTL_CONSOLE_RAW:
        flags = spin_lock_irqsave(&tty.q.lock);
        tty.raw = arg != 0;
        tty.line_len = 0;
        tty.esc = 0;
        spin_unlock_irqrestore(&tty.q.lock, flags);
        return 0;
    case IOCTL_CONSOLE_FOREGROUND:
        tty.fg_pid = (int)arg;
        return 0;
    case IOCTL_CONSOLE_SIZE:
        console_size(&rows, &cols);
        return (int64_t)rows << 16 | cols;
    case IOCTL_DISPLAY_MODE:
        return display_set_mode(arg >> 16, arg & 0xFFFF);
    default:
        return -ENOTTY;
    }
}

static const struct file_ops tty_ops = { f_read, f_write, f_ioctl, NULL };

struct file *tty_open(void)
{
    return file_alloc(&tty_ops, O_RDWR);
}
