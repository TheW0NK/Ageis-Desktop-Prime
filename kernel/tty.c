#include "tty.h"
#include "abi/syscall.h"
#include "display.h"
#include "mem.h"
#include "process.h"
#include "serial.h"
#include "string.h"
#include "sync.h"
#include "abi/poll.h"

// Terminals: the text console, and pseudo-terminals whose master side is
// held by a terminal emulator. Both share the line discipline below.

#define READY_SIZE  4096
#define LINE_MAX    1024
#define OUT_SIZE    16384

struct tty {
    struct wait_queue q;            // q.lock guards everything here
    char ready[READY_SIZE];
    size_t head, tail;
    char line[LINE_MAX];
    size_t line_len;
    bool raw;
    int eof;
    int esc;
    int fg_pid;
    uint16_t rows, cols;
    bool is_pty;
    // Pseudo-terminal: output waiting for the master, and who is open.
    char *out;
    size_t out_head, out_count;
    struct wait_queue mq;
    bool master_open;
    uint32_t slave_refs;
};

static struct tty console = { .q = WAIT_QUEUE_INIT };

// Called with t->q.lock held.
static void emit(struct tty *t, const char *s, size_t n)
{
    if (!t->is_pty) {
        console_output(s, n);
        return;
    }
    for (size_t i = 0; i < n && t->out_count < OUT_SIZE; i++) {
        // Output is in "raw" form: the emulator gets \n as \r\n.
        if (s[i] == '\n' && t->out_count + 1 < OUT_SIZE) {
            t->out[(t->out_head + t->out_count++) % OUT_SIZE] = '\r';
        }
        t->out[(t->out_head + t->out_count++) % OUT_SIZE] = s[i];
    }
    wake_up(&t->mq);
}

static void push(struct tty *t, char c)
{
    size_t next = (t->head + 1) % READY_SIZE;

    if (next != t->tail) {
        t->ready[t->head] = c;
        t->head = next;
    }
}

static void input_char(struct tty *t, char c)
{
    if (c == 0x03 && t->fg_pid) {
        signal_send(t->fg_pid, SIGINT, NULL);
        if (!t->raw) {
            t->line_len = 0;
            emit(t, "^C\n", 3);
        }
        return;
    }
    if (t->raw) {
        push(t, c);
        return;
    }

    if (t->esc) {
        if (t->esc == 1)
            t->esc = c == '[' ? 2 : 0;
        else if ((c >= 'A' && c <= 'Z') || c == '~')
            t->esc = 0;
        return;
    }
    switch (c) {
    case 0x1B:
        t->esc = 1;
        return;
    case '\r':
    case '\n':
        for (size_t i = 0; i < t->line_len; i++)
            push(t, t->line[i]);
        push(t, '\n');
        t->line_len = 0;
        emit(t, "\n", 1);
        return;
    case '\b':
    case 0x7F:
        if (t->line_len) {
            t->line_len--;
            emit(t, "\b \b", 3);
        }
        return;
    case 0x04:
        for (size_t i = 0; i < t->line_len; i++)
            push(t, t->line[i]);
        if (t->line_len == 0)
            t->eof++;
        t->line_len = 0;
        return;
    case 0x15:
        while (t->line_len) {
            t->line_len--;
            emit(t, "\b \b", 3);
        }
        return;
    }
    if ((unsigned char)c >= 0x20 && t->line_len < LINE_MAX - 1) {
        t->line[t->line_len++] = c;
        emit(t, &c, 1);
    }
}

static void tty_feed(struct tty *t, const char *s, size_t n)
{
    uint64_t flags = spin_lock_irqsave(&t->q.lock);

    while (n--)
        input_char(t, *s++);
    wake_up_locked(&t->q);
    spin_unlock_irqrestore(&t->q.lock, flags);
}

void tty_input(const char *s, size_t n)
{
    tty_feed(&console, s, n);
}

static int64_t read_tty(struct tty *t, char *buf, size_t size, bool nonblock)
{
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&t->q.lock);
    size_t n = 0;

    for (;;) {
        wait_prepare();
        if (t->head != t->tail || t->eof)
            break;
        if (t->is_pty && !t->master_open) {
            spin_unlock_irqrestore(&t->q.lock, flags);
            return 0;               // hung up
        }
        if (nonblock) {
            spin_unlock_irqrestore(&t->q.lock, flags);
            return -EAGAIN;
        }
        if (p && signal_pending()) {
            spin_unlock_irqrestore(&t->q.lock, flags);
            return -EINTR;
        }
        wait_queue_sleep_locked(&t->q);
        spin_lock(&t->q.lock);
    }
    if (t->head == t->tail) {
        t->eof--;
        spin_unlock_irqrestore(&t->q.lock, flags);
        return 0;
    }
    while (n < size && t->head != t->tail) {
        char c = t->ready[t->tail];

        t->tail = (t->tail + 1) % READY_SIZE;
        buf[n++] = c;
        if (!t->raw && c == '\n')
            break;
    }
    spin_unlock_irqrestore(&t->q.lock, flags);
    return n;
}

int64_t tty_read(char *buf, size_t size, bool nonblock)
{
    return read_tty(&console, buf, size, nonblock);
}

static int64_t write_tty(struct tty *t, const char *buf, size_t size)
{
    uint64_t flags;

    if (!t->is_pty) {
        splash_user_output();
        console_output(buf, size);
        return size;
    }
    flags = spin_lock_irqsave(&t->q.lock);
    if (!t->master_open) {
        spin_unlock_irqrestore(&t->q.lock, flags);
        return -EIO;
    }
    emit(t, buf, size);
    spin_unlock_irqrestore(&t->q.lock, flags);
    return size;
}

int64_t tty_write(const char *buf, size_t size)
{
    return write_tty(&console, buf, size);
}

static int64_t f_read(struct file *f, void *buf, size_t size)
{
    return read_tty(f->priv, buf, size, f->flags & O_NONBLOCK);
}

static int64_t f_write(struct file *f, const void *buf, size_t size)
{
    return write_tty(f->priv, buf, size);
}

static uint32_t f_poll(struct file *f, struct poll_table *pt)
{
    struct tty *t = f->priv;
    uint64_t flags = spin_lock_irqsave(&t->q.lock);
    uint32_t ev = POLLOUT;

    if (t->head != t->tail || t->eof)
        ev |= POLLIN;
    if (t->is_pty && !t->master_open)
        ev |= POLLIN | POLLHUP;
    spin_unlock_irqrestore(&t->q.lock, flags);
    poll_wait(pt, &t->q);
    return ev;
}

static int64_t f_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    struct tty *t = f->priv;
    uint64_t flags;
    uint32_t rows, cols;

    switch (cmd) {
    case IOCTL_CONSOLE_RAW:
        flags = spin_lock_irqsave(&t->q.lock);
        // Keep what was typed ahead on a line that was never finished.
        if (arg && !t->raw) {
            for (size_t i = 0; i < t->line_len; i++)
                push(t, t->line[i]);
        }
        t->raw = arg != 0;
        t->line_len = 0;
        t->esc = 0;
        spin_unlock_irqrestore(&t->q.lock, flags);
        return 0;
    case IOCTL_CONSOLE_FOREGROUND:
        t->fg_pid = (int)arg;
        return 0;
    case IOCTL_CONSOLE_SIZE:
        if (t->is_pty)
            return (int64_t)t->rows << 16 | t->cols;
        console_size(&rows, &cols);
        return (int64_t)rows << 16 | cols;
    case IOCTL_DISPLAY_MODE:
        if (t->is_pty)
            return -ENOTTY;
        return display_set_mode(arg >> 16, arg & 0xFFFF);
    default:
        return -ENOTTY;
    }
}

static void slave_close(struct file *f);

static const struct file_ops tty_ops = {
    .read = f_read, .write = f_write, .ioctl = f_ioctl, .poll = f_poll,
};
static const struct file_ops slave_ops = {
    .read = f_read, .write = f_write, .ioctl = f_ioctl, .poll = f_poll, .close = slave_close,
};

struct file *tty_open(void)
{
    struct file *f = file_alloc(&tty_ops, O_RDWR);

    if (f)
        f->priv = &console;
    return f;
}

// ---- Pseudo-terminals ----

static void maybe_free(struct tty *t)
{
    if (!t->master_open && !t->slave_refs) {
        kfree(t->out);
        kfree(t);
    }
}

static void slave_close(struct file *f)
{
    struct tty *t = f->priv;
    uint64_t flags = spin_lock_irqsave(&t->q.lock);
    bool last;

    t->slave_refs--;
    last = !t->slave_refs;
    wake_up(&t->mq);
    spin_unlock_irqrestore(&t->q.lock, flags);
    if (last)
        maybe_free(t);
}

static int64_t m_read(struct file *f, void *buf, size_t size)
{
    struct tty *t = f->priv;
    uint64_t flags = spin_lock_irqsave(&t->q.lock);
    size_t n = 0;

    for (;;) {
        wait_prepare();
        if (t->out_count || !t->slave_refs)
            break;
        if (f->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&t->q.lock, flags);
            return -EAGAIN;
        }
        if (signal_pending()) {
            spin_unlock_irqrestore(&t->q.lock, flags);
            return -EINTR;
        }
        spin_lock(&t->mq.lock);
        spin_unlock(&t->q.lock);
        wait_queue_sleep_locked(&t->mq);
        spin_lock(&t->q.lock);
    }
    while (n < size && t->out_count) {
        ((char *)buf)[n++] = t->out[t->out_head];
        t->out_head = (t->out_head + 1) % OUT_SIZE;
        t->out_count--;
    }
    spin_unlock_irqrestore(&t->q.lock, flags);
    return n;
}

static int64_t m_write(struct file *f, const void *buf, size_t size)
{
    tty_feed(f->priv, buf, size);
    return size;
}

static uint32_t m_poll(struct file *f, struct poll_table *pt)
{
    struct tty *t = f->priv;
    uint64_t flags = spin_lock_irqsave(&t->q.lock);
    uint32_t ev = POLLOUT;

    if (t->out_count)
        ev |= POLLIN;
    if (!t->slave_refs)
        ev |= POLLHUP;
    spin_unlock_irqrestore(&t->q.lock, flags);
    poll_wait(pt, &t->mq);
    return ev;
}

static int64_t m_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    struct tty *t = f->priv;

    if (cmd != IOCTL_PTY_SET_SIZE)
        return -ENOTTY;
    t->rows = arg >> 16;
    t->cols = arg & 0xFFFF;
    if (t->fg_pid)
        signal_send(t->fg_pid, SIGWINCH, NULL);
    return 0;
}

static void m_close(struct file *f)
{
    struct tty *t = f->priv;
    uint64_t flags = spin_lock_irqsave(&t->q.lock);
    int fg = t->fg_pid;

    t->master_open = false;
    wake_up_locked(&t->q);
    spin_unlock_irqrestore(&t->q.lock, flags);
    if (fg)
        signal_send(fg, SIGHUP, NULL);
    maybe_free(t);
}

static const struct file_ops master_ops = {
    .read = m_read, .write = m_write, .ioctl = m_ioctl, .poll = m_poll, .close = m_close,
};

static void slave_ref(struct file *f)
{
    struct tty *t = f->priv;
    uint64_t flags = spin_lock_irqsave(&t->q.lock);

    t->slave_refs++;
    spin_unlock_irqrestore(&t->q.lock, flags);
}

int pty_create(struct file **master, struct file **slave)
{
    struct tty *t = kzalloc(sizeof(*t));

    if (!t || !(t->out = kmalloc(OUT_SIZE))) {
        kfree(t);
        return -ENOMEM;
    }
    t->q = (struct wait_queue)WAIT_QUEUE_INIT;
    t->mq = (struct wait_queue)WAIT_QUEUE_INIT;
    t->is_pty = true;
    t->rows = 24;
    t->cols = 80;
    t->master_open = true;
    *master = file_alloc(&master_ops, O_RDWR);
    *slave = file_alloc(&slave_ops, O_RDWR);
    if (!*master || !*slave) {
        kfree(*master);
        kfree(*slave);
        kfree(t->out);
        kfree(t);
        return -ENOMEM;
    }
    (*master)->priv = (*slave)->priv = t;
    slave_ref(*slave);
    return 0;
}
