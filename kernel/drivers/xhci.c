#include "kernel.h"
#include "apic.h"
#include "cpu.h"
#include "mem.h"
#include "pci.h"
#include "sched.h"
#include "string.h"
#include "tty.h"

#define RING_SIZE       256
#define MAX_KEYBOARDS   4
#define POLL_MS         8
#define REPEAT_DELAY    500
#define REPEAT_RATE     40

#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS     11
#define TRB_CONFIGURE   12
#define TRB_TRANSFER_EV 32
#define TRB_COMMAND_EV  33

#define TRB_IOC         (1U << 5)
#define TRB_IDT         (1U << 6)

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

struct ring {
    struct trb *trbs;
    uint32_t index;
    uint32_t cycle;
};

struct keyboard {
    uint32_t slot;
    uint32_t dci;
    struct ring ring;
    uint8_t *report;
    uint8_t last[8];
    uint8_t held;
    uint64_t held_since, last_repeat;
};

static struct {
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    uint32_t max_slots, max_ports, ctx_size;
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *events;
    uint32_t ev_index, ev_cycle;
    struct keyboard kbd[MAX_KEYBOARDS];
    int nkbd;
    bool present;
} x;

static inline uint32_t rd(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static inline void wr64(volatile uint8_t *base, uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(base + off) = v;
    *(volatile uint32_t *)(base + off + 4) = v >> 32;
}

static void *alloc_page_zero(void)
{
    uint64_t p = pmm_alloc_page();

    if (p)
        memset((void *)p, 0, PAGE_SIZE);
    return (void *)p;
}

static bool ring_init(struct ring *r)
{
    if (!(r->trbs = alloc_page_zero()))
        return false;
    r->index = 0;
    r->cycle = 1;
    r->trbs[RING_SIZE - 1].param = (uint64_t)r->trbs;
    r->trbs[RING_SIZE - 1].control = (TRB_LINK << 10) | (1 << 1);
    return true;
}

static void ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control)
{
    struct trb *t = &r->trbs[r->index];

    t->param = param;
    t->status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t->control = control | r->cycle;
    if (++r->index == RING_SIZE - 1) {
        struct trb *link = &r->trbs[RING_SIZE - 1];
        link->control = (link->control & ~1U) | r->cycle;
        r->index = 0;
        r->cycle ^= 1;
    }
}

// Returns the next event, or NULL. The caller must call event_done().
static struct trb *event_peek(void)
{
    struct trb *e = &x.events[x.ev_index];

    return (e->control & 1) == x.ev_cycle ? e : NULL;
}

static void event_done(void)
{
    if (++x.ev_index == RING_SIZE) {
        x.ev_index = 0;
        x.ev_cycle ^= 1;
    }
    wr64(x.rt, 0x20 + 0x18, (uint64_t)&x.events[x.ev_index] | (1 << 3));
}

static struct trb xhci_wait_event(uint32_t type, uint32_t slot, uint32_t timeout_ms)
{
    struct trb none = { 0 };

    for (uint64_t spins = 0; spins < (uint64_t)timeout_ms * 1000; spins++) {
        struct trb *e = event_peek();

        if (!e) {
            for (int i = 0; i < 100; i++)
                __asm__ volatile ("pause");
            continue;
        }
        struct trb copy = *e;
        event_done();
        if (((copy.control >> 10) & 0x3F) == type && (!slot || (copy.control >> 24) == slot))
            return copy;
    }
    return none;
}

static int command(uint64_t param, uint32_t control, uint32_t *slot_out)
{
    struct trb ev;

    ring_push(&x.cmd, param, 0, control);
    x.db[0] = 0;
    ev = xhci_wait_event(TRB_COMMAND_EV, 0, 500);
    if (((ev.status >> 24) & 0xFF) != 1)
        return -1;
    if (slot_out)
        *slot_out = ev.control >> 24;
    return 0;
}

static uint32_t *ctx(void *base, int index)
{
    return (uint32_t *)((uint8_t *)base + index * x.ctx_size);
}

static int control(uint32_t slot, struct ring *ep0, uint8_t type, uint8_t req, uint16_t value,
                   uint16_t index, void *data, uint16_t len)
{
    uint64_t setup = type | (uint64_t)req << 8 | (uint64_t)value << 16 | (uint64_t)index << 32
                   | (uint64_t)len << 48;
    bool in = type & 0x80;
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    struct trb ev;

    ring_push(ep0, setup, 8, (TRB_SETUP << 10) | TRB_IDT | (trt << 16));
    if (len)
        ring_push(ep0, (uint64_t)data, len, (TRB_DATA << 10) | (in ? 1 << 16 : 0));
    ring_push(ep0, 0, 0, (TRB_STATUS << 10) | TRB_IOC | ((len && in) ? 0 : 1 << 16));
    x.db[slot] = 1;
    ev = xhci_wait_event(TRB_TRANSFER_EV, slot, 500);
    uint32_t code = (ev.status >> 24) & 0xFF;
    return (code == 1 || code == 13) ? 0 : -1;
}

static uint32_t interval_for(uint32_t speed, uint8_t b_interval)
{
    uint32_t v;

    if (speed >= 3)
        return b_interval ? b_interval - 1 : 0;
    for (v = 3; v < 10 && (1U << (v - 3)) < b_interval; v++)
        ;
    return v;
}

static void setup_device(uint32_t port, uint32_t speed)
{
    uint8_t *buf = alloc_page_zero();
    void *input = alloc_page_zero(), *output = alloc_page_zero();
    struct ring ep0;
    uint32_t slot, mps = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    uint8_t config_value = 0, iface = 0, ep_addr = 0, ep_interval = 0;
    uint16_t ep_mps = 8, total;
    bool found = false;

    if (!buf || !input || !output || !ring_init(&ep0) || x.nkbd == MAX_KEYBOARDS)
        return;
    if (command(0, TRB_ENABLE_SLOT << 10, &slot) || !slot || slot > x.max_slots)
        return;

    x.dcbaa[slot] = (uint64_t)output;
    ctx(input, 0)[1] = 0x3;
    ctx(input, 1)[0] = (1U << 27) | (speed << 20);
    ctx(input, 1)[1] = port << 16;
    ctx(input, 2)[1] = (3 << 1) | (4 << 3) | (mps << 16);
    *(uint64_t *)&ctx(input, 2)[2] = (uint64_t)ep0.trbs | 1;
    ctx(input, 2)[4] = 8;
    if (command((uint64_t)input, (TRB_ADDRESS << 10) | (slot << 24), NULL))
        return;

    if (control(slot, &ep0, 0x80, 6, 0x0100, 0, buf, 8))
        return;
    if (buf[7] && buf[7] != mps && speed < 3) {
        mps = buf[7];
        ctx(input, 0)[1] = 0x2;
        ctx(input, 2)[1] = (3 << 1) | (4 << 3) | (mps << 16);
        command((uint64_t)input, (13 << 10) | (slot << 24), NULL);
    }
    if (control(slot, &ep0, 0x80, 6, 0x0200, 0, buf, 9))
        return;
    total = MIN(*(uint16_t *)(buf + 2), (uint16_t)PAGE_SIZE);
    config_value = buf[5];
    if (control(slot, &ep0, 0x80, 6, 0x0200, 0, buf, total))
        return;

    for (uint32_t off = 0; off + 2 <= total && buf[off] >= 2; off += buf[off]) {
        uint8_t *d = buf + off;

        if (d[1] == 4) {
            found = d[5] == 3 && d[6] == 1 && d[7] == 1;
            iface = d[2];
        } else if (d[1] == 5 && found && (d[2] & 0x80) && (d[3] & 3) == 3) {
            ep_addr = d[2];
            ep_mps = *(uint16_t *)(d + 4) & 0x7FF;
            ep_interval = d[6];
            break;
        }
    }
    if (!ep_addr)
        return;

    control(slot, &ep0, 0x00, 9, config_value, 0, NULL, 0);
    control(slot, &ep0, 0x21, 0x0B, 0, iface, NULL, 0);
    control(slot, &ep0, 0x21, 0x0A, 0, iface, NULL, 0);

    struct keyboard *k = &x.kbd[x.nkbd];
    uint32_t dci = (ep_addr & 0xF) * 2 + 1;

    if (!ring_init(&k->ring))
        return;
    memset(input, 0, PAGE_SIZE);
    ctx(input, 0)[1] = 1 | (1U << dci);
    memcpy(ctx(input, 1), ctx(output, 0), x.ctx_size);
    ctx(input, 1)[0] = (ctx(input, 1)[0] & ~(0x1FU << 27)) | (dci << 27);
    ctx(input, 1 + dci)[0] = interval_for(speed, ep_interval) << 16;
    ctx(input, 1 + dci)[1] = (3 << 1) | (7 << 3) | ((uint32_t)ep_mps << 16);
    *(uint64_t *)&ctx(input, 1 + dci)[2] = (uint64_t)k->ring.trbs | 1;
    ctx(input, 1 + dci)[4] = 8 | ((uint32_t)ep_mps << 16);
    if (command((uint64_t)input, (TRB_CONFIGURE << 10) | (slot << 24), NULL))
        return;

    k->slot = slot;
    k->dci = dci;
    k->report = buf;
    memset(buf, 0, 64);
    ring_push(&k->ring, (uint64_t)k->report, 8, (TRB_NORMAL << 10) | TRB_IOC);
    x.db[slot] = dci;
    x.nkbd++;
    kprintf("USB: keyboard on port %u\n", port);
}

static const char hid_normal[] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
    'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/',
};

static const char hid_shift[] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?',
};

static bool caps_lock;

static void emit(uint8_t mods, uint8_t code)
{
    bool shift = mods & 0x22, ctrl = mods & 0x11;
    const char *seq = NULL;
    char c = 0;

    switch (code) {
    case 0x39: caps_lock = !caps_lock; return;
    case 0x4F: seq = "\x1b[C"; break;
    case 0x50: seq = "\x1b[D"; break;
    case 0x51: seq = "\x1b[B"; break;
    case 0x52: seq = "\x1b[A"; break;
    case 0x4A: seq = "\x1b[H"; break;
    case 0x4D: seq = "\x1b[F"; break;
    case 0x4C: seq = "\x1b[3~"; break;
    case 0x58: c = '\n'; break;
    default:
        if (code < sizeof(hid_normal))
            c = shift ? hid_shift[code] : hid_normal[code];
    }
    if (seq) {
        tty_input(seq, strlen(seq));
        return;
    }
    if (caps_lock && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c &= 0x1F;
    if (c)
        tty_input(&c, 1);
}

static void handle_report(struct keyboard *k)
{
    uint8_t *r = k->report;

    for (int i = 2; i < 8; i++) {
        bool was_down = false;

        if (r[i] < 4)
            continue;
        for (int j = 2; j < 8; j++)
            was_down |= k->last[j] == r[i];
        if (!was_down) {
            emit(r[0], r[i]);
            k->held = r[i];
            k->held_since = k->last_repeat = timer_uptime_ms();
        }
    }
    bool still = false;
    for (int i = 2; i < 8; i++)
        still |= r[i] == k->held;
    if (!still)
        k->held = 0;
    memcpy(k->last, r, 8);
}

static void poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct trb *e;

        sched_sleep(POLL_MS);
        while ((e = event_peek())) {
            uint32_t type = (e->control >> 10) & 0x3F, slot = e->control >> 24;
            uint32_t dci = (e->control >> 16) & 0x1F;

            event_done();
            if (type != TRB_TRANSFER_EV)
                continue;
            for (int i = 0; i < x.nkbd; i++) {
                struct keyboard *k = &x.kbd[i];

                if (k->slot != slot || k->dci != dci)
                    continue;
                handle_report(k);
                ring_push(&k->ring, (uint64_t)k->report, 8, (TRB_NORMAL << 10) | TRB_IOC);
                x.db[slot] = dci;
            }
        }

        uint64_t now = timer_uptime_ms();
        for (int i = 0; i < x.nkbd; i++) {
            struct keyboard *k = &x.kbd[i];

            if (k->held && now - k->held_since >= REPEAT_DELAY && now - k->last_repeat >= REPEAT_RATE) {
                emit(k->last[0], k->held);
                k->last_repeat = now;
            }
        }
    }
}

static void bios_handoff(void)
{
    uint32_t off = (rd(x.cap, 0x10) >> 16) << 2;

    while (off) {
        volatile uint8_t *c = x.cap + off;
        uint32_t v = rd(c, 0);

        if ((v & 0xFF) == 1) {
            wr(c, 0, v | (1U << 24));
            for (int i = 0; i < 1000000 && (rd(c, 0) & (1U << 16)); i++)
                __asm__ volatile ("pause");
            wr(c, 4, rd(c, 4) & 0xFFFF1FEE);
        }
        off = ((v >> 8) & 0xFF) ? off + (((v >> 8) & 0xFF) << 2) : 0;
    }
}

static bool wait_bits(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, uint32_t ms)
{
    for (uint64_t i = 0; i < (uint64_t)ms * 10000; i++) {
        if ((rd(base, off) & mask) == want)
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void controller_init(const struct pci_device *pci)
{
    uint64_t bar = pci_bar(pci, 0, NULL);
    uint32_t sp, nsp;
    uint64_t *erst;

    if (!bar || !paging_map_mmio(bar, 0x10000))
        return;
    pci_enable(pci);
    x.cap = (volatile uint8_t *)bar;
    x.op = x.cap + (rd(x.cap, 0) & 0xFF);
    x.rt = x.cap + (rd(x.cap, 0x18) & ~0x1FU);
    x.db = (volatile uint32_t *)(x.cap + (rd(x.cap, 0x14) & ~0x3U));
    x.max_slots = rd(x.cap, 0x04) & 0xFF;
    x.max_ports = rd(x.cap, 0x04) >> 24;
    x.ctx_size = (rd(x.cap, 0x10) & 4) ? 64 : 32;

    bios_handoff();
    wr(x.op, 0x00, rd(x.op, 0x00) & ~1U);
    if (!wait_bits(x.op, 0x04, 1, 1, 100))
        return;
    wr(x.op, 0x00, 2);
    if (!wait_bits(x.op, 0x00, 2, 0, 500) || !wait_bits(x.op, 0x04, 1 << 11, 0, 500))
        return;

    wr(x.op, 0x38, x.max_slots);
    if (!(x.dcbaa = alloc_page_zero()))
        return;
    sp = rd(x.cap, 0x08);
    nsp = ((sp >> 21) & 0x1F) << 5 | (sp >> 27);
    if (nsp) {
        uint64_t *array = alloc_page_zero();
        if (!array)
            return;
        for (uint32_t i = 0; i < nsp && i < 512; i++)
            array[i] = (uint64_t)alloc_page_zero();
        x.dcbaa[0] = (uint64_t)array;
    }
    wr64(x.op, 0x30, (uint64_t)x.dcbaa);

    if (!ring_init(&x.cmd))
        return;
    wr64(x.op, 0x18, (uint64_t)x.cmd.trbs | 1);

    x.events = alloc_page_zero();
    erst = alloc_page_zero();
    if (!x.events || !erst)
        return;
    erst[0] = (uint64_t)x.events;
    erst[1] = RING_SIZE;
    x.ev_cycle = 1;
    wr(x.rt, 0x20 + 0x08, 1);
    wr64(x.rt, 0x20 + 0x18, (uint64_t)x.events);
    wr64(x.rt, 0x20 + 0x10, (uint64_t)erst);
    wr(x.rt, 0x20 + 0x00, 0);

    wr(x.op, 0x00, 1);
    if (!wait_bits(x.op, 0x04, 1, 0, 100))
        return;
    x.present = true;

    for (uint32_t port = 1; port <= x.max_ports; port++) {
        uint32_t off = 0x400 + 0x10 * (port - 1);
        uint32_t sc = rd(x.op, off);

        if (!(sc & 1))
            continue;
        if (!(sc & 2)) {
            wr(x.op, off, (sc & 0x0E00C3E0) | (1 << 4));
            wait_bits(x.op, off, 1 << 21, 1 << 21, 200);
            sc = rd(x.op, off);
            wr(x.op, off, (sc & 0x0E00C3E0) | (1 << 21));
            sc = rd(x.op, off);
        }
        if (sc & 2)
            setup_device(port, (sc >> 10) & 0xF);
    }
    if (x.nkbd)
        thread_create("usbkbd", poll_thread, NULL);
}

void xhci_init(void)
{
    const struct pci_device *pci = pci_find_class(0x0C, 0x03, 0x30, 0);

    if (pci)
        controller_init(pci);
}

int xhci_keyboard_count(void)
{
    return x.nkbd;
}
