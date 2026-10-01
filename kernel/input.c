#include "input.h"
#include "apic.h"
#include "mem.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "tty.h"
#include "abi/poll.h"

#define MAX_DEVICES     16
#define QUEUE_SIZE      1024
#define REPEAT_DELAY    500
#define REPEAT_RATE     33

struct device {
    bool used;
    char name[32];
    uint32_t kind;
    void (*set_leds)(void *ctx, uint32_t mods);
    void *ctx;
};

struct reader {
    struct input_event q[QUEUE_SIZE];
    uint32_t head, tail;
    bool grab;
    struct reader *next;
};

static struct {
    struct wait_queue wq;           // wq.lock guards everything here
    struct device devs[MAX_DEVICES];
    int ndevs;
    uint8_t down[KEY_CODE_MAX / 8];
    uint32_t mods;
    struct reader *readers;
    int grabs;
    uint16_t repeat_code;
    int repeat_dev;
    uint64_t repeat_at;
} in = { .wq = WAIT_QUEUE_INIT };

static bool is_down(uint16_t code)
{
    return code < KEY_CODE_MAX && (in.down[code / 8] & (1 << (code % 8)));
}

static void set_down(uint16_t code, bool down)
{
    if (code >= KEY_CODE_MAX)
        return;
    if (down)
        in.down[code / 8] |= 1 << (code % 8);
    else
        in.down[code / 8] &= ~(1 << (code % 8));
}

static void queue(int dev, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event e = {
        .time_ms = timer_uptime_ms(), .device = dev, .type = type,
        .code = code, .value = value, .modifiers = in.mods,
    };

    for (struct reader *r = in.readers; r; r = r->next) {
        uint32_t next = (r->head + 1) % QUEUE_SIZE;

        if (next == r->tail)
            continue;               // a reader that falls behind loses events
        r->q[r->head] = e;
        r->head = next;
    }
}

// ---- Text console translation (US layout) ----

static const char plain[0x39] = {
    [0x04] = 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 27, 0x7F, '\t', ' ', '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/',
};

static const char shifted[0x39] = {
    [0x04] = 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27, 0x7F, '\t', ' ', '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?',
};

static const char *const fkeys[20] = {
    "\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS", "\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~",
    "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~", "\x1b[25~", "\x1b[26~", "\x1b[28~",
    "\x1b[29~", "\x1b[31~", "\x1b[32~", "\x1b[33~", "\x1b[34~",
};

static const char *nav_sequence(uint16_t code)
{
    switch (code) {
    case KEY_UP:        return "\x1b[A";
    case KEY_DOWN:      return "\x1b[B";
    case KEY_RIGHT:     return "\x1b[C";
    case KEY_LEFT:      return "\x1b[D";
    case KEY_HOME:      return "\x1b[H";
    case KEY_END:       return "\x1b[F";
    case KEY_INSERT:    return "\x1b[2~";
    case KEY_DELETE:    return "\x1b[3~";
    case KEY_PAGEUP:    return "\x1b[5~";
    case KEY_PAGEDOWN:  return "\x1b[6~";
    }
    return NULL;
}

// The keypad acts as navigation keys when Num Lock is off.
static uint16_t keypad_nav(uint16_t code)
{
    static const uint16_t nav[] = {
        KEY_END, KEY_DOWN, KEY_PAGEDOWN, KEY_LEFT, 0, KEY_RIGHT,
        KEY_HOME, KEY_UP, KEY_PAGEUP, KEY_INSERT, KEY_DELETE,
    };

    return (code >= KEY_KP1 && code <= KEY_KPDOT) ? nav[code - KEY_KP1] : 0;
}

static void to_console(uint16_t code)
{
    uint32_t m = in.mods;
    const char *seq = NULL;
    char buf[2];
    char c = 0;

    if (code >= KEY_KP1 && code <= KEY_KPDOT && !(m & MOD_NUMLOCK)) {
        if (!(code = keypad_nav(code)))
            return;
    }
    if ((seq = nav_sequence(code))) {
        tty_input(seq, strlen(seq));
        return;
    }
    if (code >= KEY_F1 && code <= KEY_F12)
        seq = fkeys[code - KEY_F1];
    else if (code >= KEY_F13 && code <= KEY_F13 + 7)
        seq = fkeys[12 + code - KEY_F13];
    if (seq) {
        tty_input(seq, strlen(seq));
        return;
    }

    if (code < sizeof(plain)) {
        c = (m & MOD_SHIFT) ? shifted[code] : plain[code];
        if ((m & MOD_CAPSLOCK) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            c ^= 0x20;
    } else if (code >= KEY_KP1 && code <= KEY_KP0) {
        c = code == KEY_KP0 ? '0' : '1' + (code - KEY_KP1);
    } else {
        switch (code) {
        case KEY_KPDOT:      c = '.'; break;
        case KEY_KPSLASH:    c = '/'; break;
        case KEY_KPASTERISK: c = '*'; break;
        case KEY_KPMINUS:    c = '-'; break;
        case KEY_KPPLUS:     c = '+'; break;
        case KEY_KPENTER:    c = '\n'; break;
        case KEY_KPEQUAL:    c = '='; break;
        case KEY_KPCOMMA:    c = ','; break;
        case KEY_102ND:      c = (m & MOD_SHIFT) ? '|' : '\\'; break;
        }
    }
    if (!c)
        return;
    if (m & MOD_CTRL) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '[' && c <= '_'))
            c &= 0x1F;
        else if (c == ' ' || c == '@')
            c = 0;
        else if (c == '?')
            c = 0x7F;
    }
    if (m & MOD_ALT) {
        buf[0] = 27;
        buf[1] = c;
        tty_input(buf, 2);
        return;
    }
    tty_input(&c, 1);
}

// ---- Reporting ----

static uint32_t modifier_bit(uint16_t code)
{
    return (code >= KEY_LEFTCTRL && code <= KEY_RIGHTMETA) ? 1U << (code - KEY_LEFTCTRL) : 0;
}

static bool is_lock(uint16_t code)
{
    return code == KEY_CAPSLOCK || code == KEY_NUMLOCK || code == KEY_SCROLLLOCK;
}

int input_register(const char *name, uint32_t kind, void (*set_leds)(void *ctx, uint32_t mods), void *ctx)
{
    uint64_t flags = spin_lock_irqsave(&in.wq.lock);
    int dev = -1;

    for (int i = 0; i < in.ndevs && dev < 0; i++) {
        if (!in.devs[i].used)
            dev = i;
    }
    if (dev < 0 && in.ndevs < MAX_DEVICES)
        dev = in.ndevs++;
    if (dev >= 0) {
        struct device *d = &in.devs[dev];

        memset(d, 0, sizeof(*d));
        d->used = true;
        memcpy(d->name, name, strnlen(name, sizeof(d->name) - 1));
        d->kind = kind;
        d->set_leds = set_leds;
        d->ctx = ctx;
    }
    spin_unlock_irqrestore(&in.wq.lock, flags);
    if (dev >= 0 && set_leds)
        set_leds(ctx, in.mods);
    return dev;
}

void input_unregister(int dev)
{
    uint64_t flags;

    if (dev < 0 || dev >= in.ndevs)
        return;
    flags = spin_lock_irqsave(&in.wq.lock);
    in.devs[dev].used = false;
    in.devs[dev].set_leds = NULL;
    if (in.repeat_dev == dev)
        in.repeat_code = 0;
    spin_unlock_irqrestore(&in.wq.lock, flags);
}

static void update_leds(void)
{
    for (int i = 0; i < in.ndevs; i++) {
        if (in.devs[i].used && in.devs[i].set_leds)
            in.devs[i].set_leds(in.devs[i].ctx, in.mods);
    }
}

static void key_locked(int dev, uint16_t code, int value)
{
    queue(dev, EV_KEY, code, value);
    queue(dev, EV_SYN, 0, 0);
    if (value && !in.grabs && !modifier_bit(code) && !is_lock(code) && code < BTN_LEFT)
        to_console(code);
    wake_up_locked(&in.wq);
}

void input_key(int dev, uint16_t code, bool pressed)
{
    uint64_t flags;
    bool leds = false;

    if (dev < 0 || code >= KEY_CODE_MAX)
        return;
    flags = spin_lock_irqsave(&in.wq.lock);
    if (pressed == is_down(code)) {
        // Hardware typematic repeat (PS/2) or a duplicate release: autorepeat
        // is generated by the repeat thread instead.
        spin_unlock_irqrestore(&in.wq.lock, flags);
        return;
    }
    set_down(code, pressed);
    if (modifier_bit(code)) {
        if (pressed)
            in.mods |= modifier_bit(code);
        else
            in.mods &= ~modifier_bit(code);
    }
    if (pressed && is_lock(code)) {
        in.mods ^= code == KEY_CAPSLOCK ? MOD_CAPSLOCK : code == KEY_NUMLOCK ? MOD_NUMLOCK : MOD_SCROLLLOCK;
        leds = true;
    }
    if (pressed && code < BTN_LEFT && !modifier_bit(code) && !is_lock(code)) {
        in.repeat_code = code;
        in.repeat_dev = dev;
        in.repeat_at = timer_uptime_ms() + REPEAT_DELAY;
    } else if (!pressed && code == in.repeat_code) {
        in.repeat_code = 0;
    }
    key_locked(dev, code, pressed);
    spin_unlock_irqrestore(&in.wq.lock, flags);
    if (leds)
        update_leds();
}

void input_rel(int dev, uint16_t code, int32_t delta)
{
    uint64_t flags;

    if (dev < 0 || !delta)
        return;
    flags = spin_lock_irqsave(&in.wq.lock);
    queue(dev, EV_REL, code, delta);
    spin_unlock_irqrestore(&in.wq.lock, flags);
}

void input_abs(int dev, uint16_t code, int32_t value)
{
    uint64_t flags;

    if (dev < 0)
        return;
    flags = spin_lock_irqsave(&in.wq.lock);
    queue(dev, EV_ABS, code, MAX(0, MIN(value, INPUT_ABS_MAX)));
    spin_unlock_irqrestore(&in.wq.lock, flags);
}

void input_sync(int dev)
{
    uint64_t flags;

    if (dev < 0)
        return;
    flags = spin_lock_irqsave(&in.wq.lock);
    queue(dev, EV_SYN, 0, 0);
    wake_up_locked(&in.wq);
    spin_unlock_irqrestore(&in.wq.lock, flags);
}

uint32_t input_modifiers(void)
{
    return in.mods;
}

bool input_key_down(uint16_t code)
{
    return is_down(code);
}

static void repeat_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t flags, now;

        sched_sleep(10);
        now = timer_uptime_ms();
        flags = spin_lock_irqsave(&in.wq.lock);
        if (in.repeat_code && is_down(in.repeat_code) && now >= in.repeat_at) {
            key_locked(in.repeat_dev, in.repeat_code, 2);
            in.repeat_at = now + REPEAT_RATE;
        }
        spin_unlock_irqrestore(&in.wq.lock, flags);
    }
}

void input_init(void)
{
    in.mods = MOD_NUMLOCK;
    if (!thread_create("inputrep", repeat_thread, NULL))
        kprintf("input: cannot start the autorepeat thread\n");
}

int input_device_count(void)
{
    int n = 0;

    for (int i = 0; i < in.ndevs; i++)
        n += in.devs[i].used;
    return n;
}

const char *input_device_name(int dev, uint32_t *kind)
{
    if (dev < 0 || dev >= in.ndevs || !in.devs[dev].used)
        return NULL;
    if (kind)
        *kind = in.devs[dev].kind;
    return in.devs[dev].name;
}

// ---- /dev/input ----

static int64_t f_read(struct file *f, void *buf, size_t size)
{
    struct reader *r = f->priv;
    struct process *p = process_current();
    struct input_event *out = buf;
    size_t n = 0, max = size / sizeof(*out);
    uint64_t flags;

    if (!max)
        return -EINVAL;
    flags = spin_lock_irqsave(&in.wq.lock);
    for (;;) {
        wait_prepare();
        if (r->head != r->tail)
            break;
        if (f->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&in.wq.lock, flags);
            return -EAGAIN;
        }
        if (p && signal_pending()) {
            spin_unlock_irqrestore(&in.wq.lock, flags);
            return -EINTR;
        }
        wait_queue_sleep_locked(&in.wq);
        spin_lock(&in.wq.lock);
    }
    while (n < max && r->tail != r->head) {
        out[n++] = r->q[r->tail];
        r->tail = (r->tail + 1) % QUEUE_SIZE;
    }
    spin_unlock_irqrestore(&in.wq.lock, flags);
    return n * sizeof(*out);
}

static int64_t f_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    struct reader *r = f->priv;
    uint64_t flags;

    switch (cmd) {
    case IOCTL_INPUT_GRAB:
        flags = spin_lock_irqsave(&in.wq.lock);
        if (r->grab != (arg != 0)) {
            r->grab = arg != 0;
            in.grabs += r->grab ? 1 : -1;
        }
        spin_unlock_irqrestore(&in.wq.lock, flags);
        return 0;
    case IOCTL_INPUT_DEVICES:
        return input_device_count();
    }
    return -ENOTTY;
}

static void f_close(struct file *f)
{
    struct reader *r = f->priv;
    uint64_t flags = spin_lock_irqsave(&in.wq.lock);

    for (struct reader **pp = &in.readers; *pp; pp = &(*pp)->next) {
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    }
    if (r->grab)
        in.grabs--;
    spin_unlock_irqrestore(&in.wq.lock, flags);
    kfree(r);
}

static uint32_t f_poll(struct file *f, struct poll_table *pt)
{
    struct reader *r = f->priv;
    uint64_t flags = spin_lock_irqsave(&in.wq.lock);
    uint32_t ev = r->head != r->tail ? POLLIN : 0;

    spin_unlock_irqrestore(&in.wq.lock, flags);
    poll_wait(pt, &in.wq);
    return ev;
}

static const struct file_ops input_ops = { f_read, NULL, f_ioctl, f_close, f_poll, NULL };

struct file *input_open(void)
{
    struct reader *r = kzalloc(sizeof(*r));
    struct file *f;
    uint64_t flags;

    if (!r)
        return NULL;
    if (!(f = file_alloc(&input_ops, O_RDONLY))) {
        kfree(r);
        return NULL;
    }
    f->priv = r;
    flags = spin_lock_irqsave(&in.wq.lock);
    r->next = in.readers;
    in.readers = r;
    spin_unlock_irqrestore(&in.wq.lock, flags);
    return f;
}
