#include "kernel.h"
#include "apic.h"
#include "devfs.h"
#include "mem.h"
#include "pci.h"
#include "process.h"
#include "vm.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "vfs.h"
#include "abi/audio.h"
#include "abi/errno.h"

// Intel High Definition Audio: the controller (CORB/RIRB command rings and
// one output stream) and the codec path from an output pin to its DAC.
//
// Playback is a 32 KiB cyclic DMA buffer. write() fills it ahead of the
// hardware's position; a kernel thread follows the position, zeroes what has
// played (so running dry plays silence) and wakes writers.

#define REG_GCAP        0x00
#define REG_GCTL        0x08
#define REG_STATESTS    0x0E
#define REG_INTCTL      0x20
#define REG_CORBLBASE   0x40
#define REG_CORBUBASE   0x44
#define REG_CORBWP      0x48
#define REG_CORBRP      0x4A
#define REG_CORBCTL     0x4C
#define REG_CORBSIZE    0x4E
#define REG_RIRBLBASE   0x50
#define REG_RIRBUBASE   0x54
#define REG_RIRBWP      0x58
#define REG_RINTCNT     0x5A
#define REG_RIRBCTL     0x5C
#define REG_RIRBSTS     0x5D
#define REG_RIRBSIZE    0x5E

// Stream descriptor registers (from the descriptor's base).
#define SD_CTL          0x00
#define SD_STS          0x03
#define SD_LPIB         0x04
#define SD_CBL          0x08
#define SD_LVI          0x0C
#define SD_FMT          0x12
#define SD_BDPL         0x18
#define SD_BDPU         0x1C

#define BUF_PAGES       8
#define BUF_SIZE        (BUF_PAGES * PAGE_SIZE)
#define GUARD           2048            // kept clear in front of the hardware
#define STREAM_TAG      1

#define FMT_48K_16_2    0x0011
#define FMT_44K_16_2    0x4011

struct bdl_entry {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc;
};

static struct hda {
    volatile uint8_t *mmio;
    volatile uint8_t *sd;
    uint32_t *corb;
    volatile uint64_t *rirb;
    int corb_entries, rirb_entries;
    uint16_t corb_wp, rirb_rp;
    int codec, afg, dac, pin, volume_nid;
    bool volume_out;                // the volume amp is an output amp
    int amp_steps, amp_offset;
    uint8_t *buf;
    struct bdl_entry *bdl;
    uint64_t written, played;
    uint32_t last_pos;
    bool running, writer;
    uint64_t idle_since;
    int volume;
    uint32_t rate;
    char name[48];
    spinlock_t lock;
    struct wait_queue wq;
    struct mutex cmd_lock;
} hda;

static bool present;

static inline void cpu_relax(void)
{
    __asm__ volatile("pause");
}

static uint32_t rd32(uint32_t reg) { return *(volatile uint32_t *)(hda.mmio + reg); }
static uint16_t rd16(uint32_t reg) { return *(volatile uint16_t *)(hda.mmio + reg); }
static uint8_t rd8(uint32_t reg) { return *(volatile uint8_t *)(hda.mmio + reg); }
static void wr32(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(hda.mmio + reg) = v; }
static void wr16(uint32_t reg, uint16_t v) { *(volatile uint16_t *)(hda.mmio + reg) = v; }
static void wr8(uint32_t reg, uint8_t v) { *(volatile uint8_t *)(hda.mmio + reg) = v; }

static bool wait_for(bool (*cond)(void), uint64_t ms)
{
    uint64_t end = timer_uptime_ms() + ms;

    while (!cond()) {
        if (timer_uptime_ms() > end)
            return false;
        cpu_relax();
    }
    return true;
}

static bool in_reset(void) { return !(rd32(REG_GCTL) & 1); }
static bool out_of_reset(void) { return rd32(REG_GCTL) & 1; }

// ---- Codec commands ----

static uint32_t command(int nid, uint32_t verb)
{
    uint32_t cmd = ((uint32_t)hda.codec << 28) | ((uint32_t)nid << 20) | (verb & 0xFFFFF), resp = 0;
    uint64_t end;

    mutex_lock(&hda.cmd_lock);
    hda.corb_wp = (hda.corb_wp + 1) % hda.corb_entries;
    hda.corb[hda.corb_wp] = cmd;
    wr16(REG_CORBWP, hda.corb_wp);
    end = timer_uptime_ms() + 50;
    while ((rd16(REG_RIRBWP) & 0xFF) == hda.rirb_rp) {
        if (timer_uptime_ms() > end) {
            mutex_unlock(&hda.cmd_lock);
            return 0xFFFFFFFF;
        }
        cpu_relax();
    }
    hda.rirb_rp = (hda.rirb_rp + 1) % hda.rirb_entries;
    resp = (uint32_t)hda.rirb[hda.rirb_rp];
    // Some controllers hold further responses until the count is cleared.
    wr8(REG_RIRBSTS, 0x05);
    mutex_unlock(&hda.cmd_lock);
    return resp;
}

static uint32_t param(int nid, int id) { return command(nid, 0xF0000 | id); }
static void verb12(int nid, uint32_t verb, uint8_t payload) { command(nid, (verb << 8) | payload); }
static void verb4(int nid, uint32_t verb, uint16_t payload) { command(nid, (verb << 16) | payload); }

#define PARAM_NODES         0x04
#define PARAM_FG_TYPE       0x05
#define PARAM_WIDGET_CAPS   0x09
#define PARAM_PIN_CAPS      0x0C
#define PARAM_CONN_LEN      0x0E
#define PARAM_OUT_AMP       0x12

static int widget_type(int nid)
{
    return (param(nid, PARAM_WIDGET_CAPS) >> 20) & 0xF;
}

static int connections(int nid, int *out, int max)
{
    uint32_t len = param(nid, PARAM_CONN_LEN);
    int n = len & 0x7F, got = 0;
    bool longform = len & 0x80;

    for (int i = 0; i < n && got < max; i += longform ? 2 : 4) {
        uint32_t r = command(nid, 0xF0200 | i);

        for (int k = 0; k < (longform ? 2 : 4) && i + k < n && got < max; k++)
            out[got++] = longform ? (r >> (16 * k)) & 0xFFFF : (r >> (8 * k)) & 0xFF;
    }
    return got;
}

// Unmutes a widget's amplifiers at their 0 dB step.
static void unmute(int nid, bool out_amp, int index)
{
    uint32_t caps = param(nid, out_amp ? PARAM_OUT_AMP : 0x0D);
    int offset = caps & 0x7F;

    // Set amp: output/input, left and right, index, gain.
    verb4(nid, 0x3, (out_amp ? 0x8000 : 0x4000) | 0x3000 | (index << 8) | offset);
}

// Depth-first from a pin to a DAC; fills path. Returns its length or 0.
static int find_path(int nid, int *path, int depth)
{
    int conn[32], n;

    if (depth >= 6)
        return 0;
    path[depth] = nid;
    if (widget_type(nid) == 0)
        return depth + 1;       // an audio output converter
    n = connections(nid, conn, 32);
    for (int i = 0; i < n; i++) {
        int len = find_path(conn[i], path, depth + 1);

        if (len) {
            // Route through the input that leads there.
            if (widget_type(nid) != 2)
                verb12(nid, 0x701, i);
            else
                unmute(nid, false, i);
            return len;
        }
    }
    return 0;
}

static int pin_rank(uint32_t config)
{
    int device = (config >> 20) & 0xF, connectivity = config >> 30;

    if (connectivity == 1)
        return -1;          // nothing attached
    switch (device) {
    case 1:
        return 3;           // speaker
    case 0:
        return 2;           // line out
    case 2:
        return 1;           // headphones
    default:
        return -1;
    }
}

static bool setup_codec(void)
{
    uint32_t nodes = param(0, PARAM_NODES);
    int start = (nodes >> 16) & 0xFF, count = nodes & 0xFF, best = -1, best_rank = -1;
    int path[8], len;

    hda.afg = 0;
    for (int i = 0; i < count; i++)
        if ((param(start + i, PARAM_FG_TYPE) & 0xFF) == 1)
            hda.afg = start + i;
    if (!hda.afg)
        return false;
    verb12(hda.afg, 0x705, 0);      // power state D0
    nodes = param(hda.afg, PARAM_NODES);
    start = (nodes >> 16) & 0xFF;
    count = nodes & 0xFF;
    for (int i = 0; i < count; i++) {
        int nid = start + i, rank;
        if (widget_type(nid) != 4 || !(param(nid, PARAM_PIN_CAPS) & (1 << 4)))
            continue;
        rank = pin_rank(command(nid, 0xF1C00));
        if (rank > best_rank) {
            best_rank = rank;
            best = nid;
        }
    }
    if (best < 0 || !(len = find_path(best, path, 0)))
        return false;
    hda.pin = best;
    hda.dac = path[len - 1];
    // Power and unmute everything on the way.
    for (int i = 0; i < len; i++) {
        uint32_t caps = param(path[i], PARAM_WIDGET_CAPS);

        verb12(path[i], 0x705, 0);
        if (caps & (1 << 2))
            unmute(path[i], true, 0);
        if ((caps & (1 << 2)) && !hda.volume_nid) {
            uint32_t amp = param(path[i], PARAM_OUT_AMP);

            if ((amp >> 8) & 0x7F) {
                hda.volume_nid = path[i];
                hda.volume_out = true;
                hda.amp_steps = (amp >> 8) & 0x7F;
                hda.amp_offset = amp & 0x7F;
            }
        }
    }
    verb12(hda.pin, 0x707, best_rank == 1 ? 0xC0 : 0x40);   // output on (and headphone drive)
    if (param(hda.pin, PARAM_PIN_CAPS) & (1 << 16))
        verb12(hda.pin, 0x70C, 0x02);                     // external amplifier on
    verb12(hda.dac, 0x706, STREAM_TAG << 4);
    verb4(hda.dac, 0x2, FMT_48K_16_2);
    return true;
}

static void set_volume(int v)
{
    int gain;

    hda.volume = MIN(MAX(v, 0), 100);
    if (!hda.volume_nid)
        return;
    // A gentle curve: the top of the range around 0 dB.
    gain = hda.amp_steps * hda.volume / 100;
    verb4(hda.volume_nid, 0x3, (hda.volume_out ? 0x8000 : 0x4000) | 0x3000 | (hda.volume ? 0 : 0x80) | gain);
}

// ---- The stream ----

static bool sd_reset_set(void) { return hda.sd[SD_CTL] & 1; }
static bool sd_reset_clear(void) { return !(hda.sd[SD_CTL] & 1); }

static void stream_program(void)
{
    // Reset the descriptor, then describe the buffer.
    hda.sd[SD_CTL] = 0;
    hda.sd[SD_CTL] = 1;
    wait_for(sd_reset_set, 10);
    hda.sd[SD_CTL] = 0;
    wait_for(sd_reset_clear, 10);
    *(volatile uint32_t *)(hda.sd + SD_BDPL) = (uint32_t)(uint64_t)hda.bdl;
    *(volatile uint32_t *)(hda.sd + SD_BDPU) = (uint32_t)((uint64_t)hda.bdl >> 32);
    *(volatile uint32_t *)(hda.sd + SD_CBL) = BUF_SIZE;
    *(volatile uint16_t *)(hda.sd + SD_LVI) = BUF_PAGES - 1;
    *(volatile uint16_t *)(hda.sd + SD_FMT) = hda.rate == 44100 ? FMT_44K_16_2 : FMT_48K_16_2;
    hda.sd[SD_CTL + 2] = STREAM_TAG << 4;
    hda.sd[SD_STS] = 0x1C;      // clear status bits
}

static void stream_start(void)
{
    memset(hda.buf, 0, BUF_SIZE);
    hda.written = hda.played = 0;
    hda.last_pos = 0;
    stream_program();
    hda.sd[SD_CTL] |= 2;        // run
    hda.running = true;
}

static void stream_stop(void)
{
    hda.sd[SD_CTL] &= ~2;
    hda.running = false;
}

static void pump_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&hda.lock);

        if (hda.running) {
            uint32_t pos = *(volatile uint32_t *)(hda.sd + SD_LPIB) % BUF_SIZE;
            uint32_t delta = (pos + BUF_SIZE - hda.last_pos) % BUF_SIZE;

            // Clear what has played, so a writer that falls behind gives silence.
            if (pos >= hda.last_pos) {
                memset(hda.buf + hda.last_pos, 0, pos - hda.last_pos);
            } else {
                memset(hda.buf + hda.last_pos, 0, BUF_SIZE - hda.last_pos);
                memset(hda.buf, 0, pos);
            }
            hda.played += delta;
            hda.last_pos = pos;
            if (hda.written > hda.played)
                hda.idle_since = timer_uptime_ms();
            // Nobody has played anything for a while: stop the DMA.
            else if (!hda.writer && timer_uptime_ms() - hda.idle_since > 1000)
                stream_stop();
            wake_up(&hda.wq);
        }
        spin_unlock_irqrestore(&hda.lock, flags);
        sched_sleep(hda.running ? 4 : 50);
    }
}

// ---- /dev/audio ----

static int64_t audio_write(struct file *f, const void *buf, size_t size)
{
    const uint8_t *p = buf;
    size_t done = 0;

    (void)f;
    size &= ~(size_t)3;         // whole frames
    while (done < size) {
        uint64_t flags;
        uint64_t room;

        flags = spin_lock_irqsave(&hda.lock);
        if (!hda.running)
            stream_start();
        if (hda.written < hda.played + GUARD) {
            // Underrun: start again just ahead of the hardware.
            hda.written = (hda.played + GUARD) & ~3ULL;
        }
        room = BUF_SIZE - GUARD - (hda.written - hda.played);
        if (room == 0 || room > BUF_SIZE) {
            // Full: wait for the hardware to move on.
            wait_prepare();
            wait_queue_sleep_locked_timeout(&hda.wq, 100);
            spin_lock(&hda.lock);
            spin_unlock_irqrestore(&hda.lock, flags);
            if (signal_pending())
                return done ? (int64_t)done : -EINTR;
            continue;
        }
        {
            size_t n = MIN(size - done, room);
            uint32_t at = hda.written % BUF_SIZE, first = MIN(n, BUF_SIZE - at);

            memcpy(hda.buf + at, p + done, first);
            if (n > first)
                memcpy(hda.buf, p + done + first, n - first);
            hda.written += n;
            done += n;
            hda.idle_since = timer_uptime_ms();
        }
        spin_unlock_irqrestore(&hda.lock, flags);
    }
    return done;
}

static int64_t audio_ioctl(struct file *f, uint64_t cmd, uint64_t arg)
{
    (void)f;
    switch (cmd) {
    case IOCTL_AUDIO_INFO: {
        struct aegis_audioinfo info = { 0 };
        uint64_t flags = spin_lock_irqsave(&hda.lock);

        info.rate = hda.rate;
        info.channels = 2;
        info.bits = 16;
        info.buffer_bytes = BUF_SIZE;
        info.queued_bytes = hda.running && hda.written > hda.played ? hda.written - hda.played : 0;
        info.played_bytes = hda.played;
        info.volume = hda.volume;
        spin_unlock_irqrestore(&hda.lock, flags);
        memcpy(info.device, hda.name, sizeof(info.device));
        return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
    }
    case IOCTL_AUDIO_SET_RATE:
        if (arg != 48000 && arg != 44100)
            return -EINVAL;
        if (arg != hda.rate) {
            uint64_t flags = spin_lock_irqsave(&hda.lock);

            hda.rate = arg;
            verb4(hda.dac, 0x2, arg == 44100 ? FMT_44K_16_2 : FMT_48K_16_2);
            if (hda.running) {
                stream_stop();
                stream_start();
            }
            spin_unlock_irqrestore(&hda.lock, flags);
        }
        return 0;
    case IOCTL_AUDIO_VOLUME:
        if ((int64_t)arg >= 0)
            set_volume((int)arg);
        return hda.volume;
    case IOCTL_AUDIO_DRAIN:
        for (;;) {
            uint64_t flags = spin_lock_irqsave(&hda.lock);
            bool done = !hda.running || hda.played >= hda.written;

            spin_unlock_irqrestore(&hda.lock, flags);
            if (done || signal_pending())
                return 0;
            sched_sleep(10);
        }
    }
    return -ENOTTY;
}

static void audio_close(struct file *f)
{
    if ((f->flags & O_ACCMODE) != O_RDONLY) {
        uint64_t flags = spin_lock_irqsave(&hda.lock);

        hda.writer = false;
        spin_unlock_irqrestore(&hda.lock, flags);
    }
}

static const struct file_ops audio_ops = { .write = audio_write, .ioctl = audio_ioctl, .close = audio_close };

static int audio_open(void *ctx, uint32_t flags, struct file **out)
{
    struct file *f;

    (void)ctx;
    if (!present)
        return -ENODEV;
    if ((flags & O_ACCMODE) != O_RDONLY) {
        uint64_t irq = spin_lock_irqsave(&hda.lock);

        if (hda.writer) {
            spin_unlock_irqrestore(&hda.lock, irq);
            return -EBUSY;
        }
        hda.writer = true;
        spin_unlock_irqrestore(&hda.lock, irq);
    }
    if (!(f = file_alloc(&audio_ops, flags)))
        return -ENOMEM;
    *out = f;
    return 0;
}

// ---- Setup ----

static bool setup_rings(void)
{
    static const int sizes[] = { 2, 16, 256 };
    uint8_t cap;

    wr8(REG_CORBCTL, 0);
    wr8(REG_RIRBCTL, 0);
    hda.corb = (uint32_t *)pmm_alloc_page();
    hda.rirb = (volatile uint64_t *)pmm_alloc_page();
    if (!hda.corb || !hda.rirb)
        return false;
    memset(hda.corb, 0, PAGE_SIZE);
    memset((void *)hda.rirb, 0, PAGE_SIZE);
    cap = rd8(REG_CORBSIZE) >> 4;
    for (int i = 2; i >= 0; i--)
        if (cap & (1 << i)) {
            wr8(REG_CORBSIZE, i);
            hda.corb_entries = sizes[i];
            break;
        }
    cap = rd8(REG_RIRBSIZE) >> 4;
    for (int i = 2; i >= 0; i--)
        if (cap & (1 << i)) {
            wr8(REG_RIRBSIZE, i);
            hda.rirb_entries = sizes[i];
            break;
        }
    if (!hda.corb_entries || !hda.rirb_entries)
        return false;
    wr32(REG_CORBLBASE, (uint32_t)(uint64_t)hda.corb);
    wr32(REG_CORBUBASE, (uint32_t)((uint64_t)hda.corb >> 32));
    wr32(REG_RIRBLBASE, (uint32_t)(uint64_t)hda.rirb);
    wr32(REG_RIRBUBASE, (uint32_t)((uint64_t)hda.rirb >> 32));
    // Reset the read pointer of the CORB and the write pointer of the RIRB.
    wr16(REG_CORBRP, 0x8000);
    for (int i = 0; i < 1000 && !(rd16(REG_CORBRP) & 0x8000); i++)
        cpu_relax();
    wr16(REG_CORBRP, 0);
    for (int i = 0; i < 1000 && (rd16(REG_CORBRP) & 0x8000); i++)
        cpu_relax();
    wr16(REG_CORBWP, 0);
    wr16(REG_RIRBWP, 0x8000);
    wr16(REG_RINTCNT, 0xFF);
    hda.corb_wp = 0;
    hda.rirb_rp = 0;
    wr8(REG_CORBCTL, 2);
    wr8(REG_RIRBCTL, 2);
    return true;
}

static void device_init(const struct pci_device *pci)
{
    uint64_t bar = pci_bar(pci, 0, NULL);
    uint16_t gcap, statests;
    int iss;

    if (!bar || !paging_map_mmio(bar, 0x4000)) {
        kprintf("hda: cannot map the controller (BAR 0x%lx)\n", bar);
        return;
    }
    pci_enable(pci);
    hda.mmio = (volatile uint8_t *)bar;
    // Controller reset.
    wr32(REG_GCTL, rd32(REG_GCTL) & ~1U);
    if (!wait_for(in_reset, 100)) {
        kprintf("hda: the controller does not reset\n");
        return;
    }
    wr32(REG_GCTL, rd32(REG_GCTL) | 1);
    if (!wait_for(out_of_reset, 100)) {
        kprintf("hda: the controller does not come out of reset\n");
        return;
    }
    sched_sleep(2);             // codecs announce themselves
    statests = rd16(REG_STATESTS);
    if (!statests) {
        kprintf("hda: no codecs\n");
        return;
    }
    hda.codec = __builtin_ctz(statests);
    wr32(REG_INTCTL, 0);
    if (!setup_rings()) {
        kprintf("hda: cannot set up the command rings\n");
        return;
    }
    if (!setup_codec()) {
        kprintf("hda: no output path on codec %d\n", hda.codec);
        return;
    }
    gcap = rd16(REG_GCAP);
    iss = (gcap >> 8) & 0xF;
    if (!((gcap >> 12) & 0xF)) {
        kprintf("hda: no output streams\n");
        return;
    }
    hda.sd = hda.mmio + 0x80 + 0x20 * iss;
    hda.buf = (uint8_t *)pmm_alloc_pages(BUF_PAGES);
    hda.bdl = (struct bdl_entry *)pmm_alloc_page();
    if (!hda.buf || !hda.bdl)
        return;
    memset(hda.bdl, 0, PAGE_SIZE);
    for (int i = 0; i < BUF_PAGES; i++) {
        hda.bdl[i].addr = (uint64_t)hda.buf + (uint64_t)i * PAGE_SIZE;
        hda.bdl[i].len = PAGE_SIZE;
        hda.bdl[i].ioc = 0;
    }
    hda.rate = 48000;
    {
        uint32_t vendor = param(0, 0x00);

        ksnprintf(hda.name, sizeof(hda.name), "HD Audio codec %04x:%04x", vendor >> 16, vendor & 0xFFFF);
    }
    set_volume(80);
    present = true;
    kprintf("Audio: %s (pin %d, DAC %d)\n", hda.name, hda.pin, hda.dac);
    thread_create("hda", pump_thread, NULL);
}

void hda_init(void)
{
    const struct pci_device *pci = pci_find_class(0x04, 0x03, 0x00, 0);

    hda.lock = (spinlock_t)SPINLOCK_INIT;
    hda.wq = (struct wait_queue)WAIT_QUEUE_INIT;
    hda.cmd_lock = (struct mutex)MUTEX_INIT;
    if (pci)
        device_init(pci);
    else
        kprintf("hda: no HD Audio controller\n");
    devfs_register("audio", 0660, 0, DEVFS_GID_AUDIO, audio_open, NULL);
}
