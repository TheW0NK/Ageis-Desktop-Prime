#include "kernel.h"
#include "apic.h"
#include "cpu.h"
#include "hid.h"
#include "input.h"
#include "keyboard.h"
#include "mem.h"
#include "pci.h"
#include "random.h"
#include "sched.h"
#include "string.h"

// xHCI host controller with USB HID devices (keyboards, mice, tablets, media
// keys) on root ports. Devices may be plugged and unplugged at any time; a
// single polling thread owns the controller once it is running.

#define RING_SIZE       256
#define MAX_DEVICES     16
#define MAX_IFACES      32
#define MAX_PORTS       64
#define POLL_MS         4

#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS     11
#define TRB_CONFIGURE   12
#define TRB_EVALUATE    13
#define TRB_TRANSFER_EV 32
#define TRB_COMMAND_EV  33
#define TRB_PORT_EV     34

#define TRB_IOC         (1U << 5)
#define TRB_IDT         (1U << 6)

#define PORTSC_KEEP     0x0E00C3E0U     // read-write bits, excluding PED and change bits
#define PORTSC_CHANGES  0x00FE0000U

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

struct usb_device {
    bool used;
    uint32_t slot, port, speed;
    struct ring ep0;
    void *input, *output;
    uint8_t *buf;
};

struct iface {
    bool active;
    struct usb_device *dev;
    uint8_t number, dci;
    struct ring ring;
    uint8_t *report;
    uint16_t report_len;
    struct hid_device hid;
    volatile bool leds_pending;
    volatile uint32_t leds;
};

static struct {
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    uint32_t max_slots, max_ports, ctx_size;
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *events;
    uint32_t ev_index, ev_cycle;
    struct usb_device devs[MAX_DEVICES];
    struct iface ifaces[MAX_IFACES];
    volatile uint64_t port_changed;
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

static void free_page(void *p)
{
    if (p)
        pmm_free_page((uint64_t)p);
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

static void queue_report(struct iface *f)
{
    ring_push(&f->ring, (uint64_t)f->report, f->report_len, (TRB_NORMAL << 10) | TRB_IOC);
    x.db[f->dev->slot] = f->dci;
}

// Handles an event that nobody is waiting for.
static void dispatch(const struct trb *e)
{
    uint32_t type = (e->control >> 10) & 0x3F, slot = e->control >> 24;
    uint32_t dci = (e->control >> 16) & 0x1F;

    if (type == TRB_PORT_EV) {
        uint32_t port = (e->param >> 24) & 0xFF;

        if (port >= 1 && port <= MAX_PORTS)
            __atomic_fetch_or(&x.port_changed, 1ULL << (port - 1), __ATOMIC_RELAXED);
        return;
    }
    if (type != TRB_TRANSFER_EV)
        return;
    for (int i = 0; i < MAX_IFACES; i++) {
        struct iface *f = &x.ifaces[i];
        uint32_t code = (e->status >> 24) & 0xFF;

        if (!f->active || f->dev->slot != slot || f->dci != dci)
            continue;
        if (code == 1 || code == 13) {      // success, short packet
            uint32_t got = f->report_len - MIN(e->status & 0xFFFFFF, (uint32_t)f->report_len);

            random_mix(*(uint64_t *)f->report);
            hid_report(&f->hid, f->report, got);
        }
        queue_report(f);
        return;
    }
}

static struct trb wait_event(uint32_t type, uint32_t slot, uint32_t timeout_ms)
{
    struct trb none = { 0 };
    uint64_t deadline = timer_uptime_ms() + timeout_ms;

    for (uint64_t spins = 0;; spins++) {
        struct trb *e = event_peek();

        if (!e) {
            // The timer may not be running yet during early setup.
            if (spins > (uint64_t)timeout_ms * 20000 || timer_uptime_ms() > deadline)
                return none;
            for (int i = 0; i < 50; i++)
                __asm__ volatile ("pause");
            continue;
        }
        struct trb copy = *e;
        event_done();
        if (((copy.control >> 10) & 0x3F) == type && (!slot || (copy.control >> 24) == slot))
            return copy;
        dispatch(&copy);
    }
}

static int command(uint64_t param, uint32_t control, uint32_t *slot_out)
{
    struct trb ev;

    ring_push(&x.cmd, param, 0, control);
    x.db[0] = 0;
    ev = wait_event(TRB_COMMAND_EV, 0, 500);
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

static int control(struct usb_device *d, uint8_t type, uint8_t req, uint16_t value,
                   uint16_t index, void *data, uint16_t len)
{
    uint64_t setup = type | (uint64_t)req << 8 | (uint64_t)value << 16 | (uint64_t)index << 32
                   | (uint64_t)len << 48;
    bool in = type & 0x80;
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    struct trb ev;
    uint32_t code;

    ring_push(&d->ep0, setup, 8, (TRB_SETUP << 10) | TRB_IDT | (trt << 16));
    if (len)
        ring_push(&d->ep0, (uint64_t)data, len, (TRB_DATA << 10) | (in ? 1 << 16 : 0));
    ring_push(&d->ep0, 0, 0, (TRB_STATUS << 10) | TRB_IOC | ((len && in) ? 0 : 1 << 16));
    x.db[d->slot] = 1;
    ev = wait_event(TRB_TRANSFER_EV, d->slot, 500);
    code = (ev.status >> 24) & 0xFF;
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

static void set_leds(void *ctxp, uint32_t mods)
{
    struct iface *f = ctxp;

    f->leds = mods;
    f->leds_pending = true;
}

static void send_leds(struct iface *f)
{
    uint8_t *buf = f->dev->buf + 2048;
    size_t n;

    f->leds_pending = false;
    n = hid_led_report(&f->hid, f->leds, buf, 64);
    if (n)
        control(f->dev, 0x21, 0x09, 0x0200 | f->hid.led_report_id, f->number, buf, n);
}

struct pending_iface {
    uint8_t number, subclass, protocol;
    uint8_t ep_addr, ep_interval;
    uint16_t ep_mps, report_desc_len;
};

static void release_device(struct usb_device *d)
{
    for (int i = 0; i < MAX_IFACES; i++) {
        struct iface *f = &x.ifaces[i];

        if (f->active && f->dev == d) {
            f->active = false;
            hid_free(&f->hid);
            input_unregister(f->hid.input);
            free_page(f->ring.trbs);
            free_page(f->report);
        }
    }
    if (d->slot) {
        command(0, (TRB_DISABLE_SLOT << 10) | (d->slot << 24), NULL);
        x.dcbaa[d->slot] = 0;
    }
    free_page(d->ep0.trbs);
    free_page(d->input);
    free_page(d->output);
    free_page(d->buf);
    memset(d, 0, sizeof(*d));
}

static const char *kind_name(uint32_t kind)
{
    if (kind & INPUT_KIND_KEYBOARD)
        return "keyboard";
    if (kind & INPUT_KIND_TABLET)
        return "tablet";
    if (kind & INPUT_KIND_POINTER)
        return "mouse";
    return "input device";
}

static bool start_iface(struct usb_device *d, const struct pending_iface *p)
{
    struct iface *f = NULL;
    uint8_t *desc = d->buf + 1024;
    char name[32];

    for (int i = 0; i < MAX_IFACES && !f; i++) {
        if (!x.ifaces[i].active)
            f = &x.ifaces[i];
    }
    if (!f)
        return false;
    memset(f, 0, sizeof(*f));
    f->dev = d;
    f->number = p->number;
    f->dci = (p->ep_addr & 0xF) * 2 + 1;
    f->report_len = MIN(MAX(p->ep_mps, (uint16_t)8), (uint16_t)1024);

    control(d, 0x21, 0x0A, 0, p->number, NULL, 0);          // SET_IDLE: report on change only
    if (!(p->report_desc_len && p->report_desc_len <= 1024
          && control(d, 0x81, 6, 0x2200, p->number, desc, p->report_desc_len) == 0
          && hid_parse(&f->hid, desc, p->report_desc_len))) {
        // Fall back to the boot protocol, whose layout is fixed.
        hid_free(&f->hid);
        if (p->subclass != 1 || (p->protocol != 1 && p->protocol != 2))
            return false;
        control(d, 0x21, 0x0B, 0, p->number, NULL, 0);
        if (!hid_parse(&f->hid, p->protocol == 1 ? hid_boot_keyboard_desc : hid_boot_mouse_desc,
                       p->protocol == 1 ? hid_boot_keyboard_desc_len : hid_boot_mouse_desc_len))
            return false;
    }
    if (!ring_init(&f->ring) || !(f->report = alloc_page_zero())) {
        hid_free(&f->hid);
        free_page(f->ring.trbs);
        return false;
    }
    ksnprintf(name, sizeof(name), "USB %s (port %u)", kind_name(f->hid.kind), d->port);
    f->hid.input = input_register(name, f->hid.kind,
                                  f->hid.led_report_bytes ? set_leds : NULL, f);
    f->active = true;
    kprintf("USB: %s on port %u\n", kind_name(f->hid.kind), d->port);
    return true;
}

static void setup_device(uint32_t port, uint32_t speed)
{
    struct usb_device *d = NULL;
    struct pending_iface ifs[8], *cur = NULL;
    uint32_t mps = speed == 4 ? 512 : speed == 3 ? 64 : 8, slot, max_dci = 1;
    bool any = false;
    uint16_t total;
    int nifs = 0;
    uint8_t config_value;
    uint8_t *buf;

    for (int i = 0; i < MAX_DEVICES && !d; i++) {
        if (!x.devs[i].used)
            d = &x.devs[i];
    }
    if (!d)
        return;
    d->used = true;
    d->port = port;
    d->speed = speed;
    if (!(d->buf = alloc_page_zero()) || !(d->input = alloc_page_zero())
        || !(d->output = alloc_page_zero()) || !ring_init(&d->ep0))
        goto fail;
    buf = d->buf;
    if (command(0, TRB_ENABLE_SLOT << 10, &slot) || !slot || slot > x.max_slots)
        goto fail;
    d->slot = slot;

    x.dcbaa[slot] = (uint64_t)d->output;
    ctx(d->input, 0)[1] = 0x3;
    ctx(d->input, 1)[0] = (1U << 27) | (speed << 20);
    ctx(d->input, 1)[1] = port << 16;
    ctx(d->input, 2)[1] = (3 << 1) | (4 << 3) | (mps << 16);
    *(uint64_t *)&ctx(d->input, 2)[2] = (uint64_t)d->ep0.trbs | 1;
    ctx(d->input, 2)[4] = 8;
    if (command((uint64_t)d->input, (TRB_ADDRESS << 10) | (slot << 24), NULL))
        goto fail;

    if (control(d, 0x80, 6, 0x0100, 0, buf, 8))
        goto fail;
    if (buf[7] && buf[7] != mps && speed < 3) {
        mps = buf[7];
        ctx(d->input, 0)[1] = 0x2;
        ctx(d->input, 2)[1] = (3 << 1) | (4 << 3) | (mps << 16);
        command((uint64_t)d->input, (TRB_EVALUATE << 10) | (slot << 24), NULL);
    }
    if (control(d, 0x80, 6, 0x0200, 0, buf, 9))
        goto fail;
    total = MIN(*(uint16_t *)(buf + 2), (uint16_t)1024);
    config_value = buf[5];
    if (control(d, 0x80, 6, 0x0200, 0, buf, total))
        goto fail;

    for (uint32_t off = 0; off + 2 <= total && buf[off] >= 2; off += buf[off]) {
        uint8_t *p = buf + off;

        if (p[1] == 4) {
            cur = NULL;
            if (p[5] == 3 && nifs < 8) {
                cur = &ifs[nifs++];
                memset(cur, 0, sizeof(*cur));
                cur->number = p[2];
                cur->subclass = p[6];
                cur->protocol = p[7];
            }
        } else if (p[1] == 0x21 && cur && p[0] >= 9) {
            cur->report_desc_len = p[7] | p[8] << 8;
        } else if (p[1] == 5 && cur && !cur->ep_addr && (p[2] & 0x80) && (p[3] & 3) == 3) {
            cur->ep_addr = p[2];
            cur->ep_mps = (p[4] | p[5] << 8) & 0x7FF;
            cur->ep_interval = p[6];
        }
    }
    if (control(d, 0x00, 9, config_value, 0, NULL, 0))
        goto fail;

    // One Configure Endpoint command adds every interrupt endpoint.
    memset(d->input, 0, PAGE_SIZE);
    memcpy(ctx(d->input, 1), ctx(d->output, 0), x.ctx_size);
    for (int i = 0; i < nifs; i++) {
        uint32_t dci = (ifs[i].ep_addr & 0xF) * 2 + 1;

        if (!ifs[i].ep_addr)
            continue;
        any = true;
        max_dci = MAX(max_dci, dci);
    }
    if (!any)
        goto fail;

    int started = 0;
    for (int i = 0; i < nifs; i++) {
        if (ifs[i].ep_addr && start_iface(d, &ifs[i]))
            started++;
    }
    if (!started)
        goto fail;

    ctx(d->input, 0)[1] = 1;
    ctx(d->input, 1)[0] = (ctx(d->input, 1)[0] & ~(0x1FU << 27)) | (max_dci << 27);
    for (int i = 0; i < MAX_IFACES; i++) {
        struct iface *f = &x.ifaces[i];
        const struct pending_iface *p = NULL;

        if (!f->active || f->dev != d)
            continue;
        for (int j = 0; j < nifs && !p; j++) {
            if (ifs[j].number == f->number)
                p = &ifs[j];
        }
        ctx(d->input, 0)[1] |= 1U << f->dci;
        ctx(d->input, 1 + f->dci)[0] = interval_for(speed, p->ep_interval) << 16;
        ctx(d->input, 1 + f->dci)[1] = (3 << 1) | (7 << 3) | ((uint32_t)p->ep_mps << 16);
        *(uint64_t *)&ctx(d->input, 1 + f->dci)[2] = (uint64_t)f->ring.trbs | 1;
        ctx(d->input, 1 + f->dci)[4] = p->ep_mps | ((uint32_t)p->ep_mps << 16);
    }
    if (command((uint64_t)d->input, (TRB_CONFIGURE << 10) | (slot << 24), NULL))
        goto fail;

    for (int i = 0; i < MAX_IFACES; i++) {
        struct iface *f = &x.ifaces[i];

        if (f->active && f->dev == d)
            queue_report(f);
    }
    return;

fail:
    release_device(d);
}

// Resets a newly connected port; returns its speed, or 0.
static uint32_t reset_port(uint32_t port)
{
    uint32_t off = 0x400 + 0x10 * (port - 1);
    uint32_t sc = rd(x.op, off);

    if (!(sc & 1))
        return 0;
    if (!(sc & 2)) {
        wr(x.op, off, (sc & PORTSC_KEEP) | (1 << 4));
        for (int i = 0; i < 2000000 && !(rd(x.op, off) & (1 << 21)); i++)
            __asm__ volatile ("pause");
        sc = rd(x.op, off);
        wr(x.op, off, (sc & PORTSC_KEEP) | (1 << 21));
        sc = rd(x.op, off);
    }
    return (sc & 2) ? (sc >> 10) & 0xF : 0;
}

static void port_changed(uint32_t port)
{
    uint32_t off = 0x400 + 0x10 * (port - 1);
    uint32_t sc = rd(x.op, off);
    struct usb_device *d = NULL;
    uint32_t speed;

    wr(x.op, off, (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
    for (int i = 0; i < MAX_DEVICES && !d; i++) {
        if (x.devs[i].used && x.devs[i].port == port)
            d = &x.devs[i];
    }
    if (!(sc & 1)) {
        if (d) {
            kprintf("USB: device on port %u removed\n", port);
            release_device(d);
        }
        return;
    }
    if (!d && (sc & (1 << 17)) && (speed = reset_port(port)))
        setup_device(port, speed);
}

static void poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct trb *e;
        uint64_t changed;

        sched_sleep(POLL_MS);
        while ((e = event_peek())) {
            struct trb copy = *e;

            event_done();
            dispatch(&copy);
        }
        changed = __atomic_exchange_n(&x.port_changed, 0, __ATOMIC_RELAXED);
        for (uint32_t port = 1; changed && port <= x.max_ports && port <= MAX_PORTS; port++) {
            if (changed & (1ULL << (port - 1)))
                port_changed(port);
        }
        for (int i = 0; i < MAX_IFACES; i++) {
            if (x.ifaces[i].active && x.ifaces[i].leds_pending)
                send_leds(&x.ifaces[i]);
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
        uint32_t speed = reset_port(port);

        wr(x.op, off, (rd(x.op, off) & PORTSC_KEEP) | PORTSC_CHANGES);
        if (speed)
            setup_device(port, speed);
    }
    // Port events raised by the resets above describe devices already set up.
    struct trb *e;
    while ((e = event_peek())) {
        struct trb copy = *e;

        event_done();
        dispatch(&copy);
    }
    x.port_changed = 0;
    thread_create("usb", poll_thread, NULL);
}

void xhci_init(void)
{
    const struct pci_device *pci = pci_find_class(0x0C, 0x03, 0x30, 0);

    if (pci)
        controller_init(pci);
}

int xhci_device_count(void)
{
    int n = 0;

    for (int i = 0; i < MAX_IFACES; i++)
        n += x.ifaces[i].active;
    return n;
}
