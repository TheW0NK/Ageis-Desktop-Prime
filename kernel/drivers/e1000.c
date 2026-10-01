#include "net.h"
#include "cpu.h"
#include "mem.h"
#include "pci.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"

// Intel 8254x/8257x gigabit Ethernet (QEMU's e1000 and e1000e, and many
// real cards) using legacy descriptors.

#define REG_CTRL        0x0000
#define REG_STATUS      0x0008
#define REG_EERD        0x0014
#define REG_ICR         0x00C0
#define REG_IMS         0x00D0
#define REG_IMC         0x00D8
#define REG_RCTL        0x0100
#define REG_TCTL        0x0400
#define REG_TIPG        0x0410
#define REG_RDBAL       0x2800
#define REG_RDBAH       0x2804
#define REG_RDLEN       0x2808
#define REG_RDH         0x2810
#define REG_RDT         0x2818
#define REG_TDBAL       0x3800
#define REG_TDBAH       0x3804
#define REG_TDLEN       0x3808
#define REG_TDH         0x3810
#define REG_TDT         0x3818
#define REG_MTA         0x5200
#define REG_RAL         0x5400
#define REG_RAH         0x5404

#define CTRL_SLU        (1U << 6)
#define CTRL_ASDE       (1U << 5)
#define CTRL_RST        (1U << 26)
#define STATUS_LU       (1U << 1)
#define RCTL_EN         (1U << 1)
#define RCTL_BAM        (1U << 15)
#define RCTL_SECRC      (1U << 26)
#define TCTL_EN         (1U << 1)
#define TCTL_PSP        (1U << 3)
#define ICR_LSC         (1U << 2)
#define ICR_RXDMT0      (1U << 4)
#define ICR_RXO         (1U << 6)
#define ICR_RXT0        (1U << 7)
#define TX_EOP          0x01
#define TX_IFCS         0x02
#define TX_RS           0x08
#define DESC_DD         0x01

#define RX_COUNT        128
#define TX_COUNT        128
#define BUF_SIZE        2048

struct rx_desc {
    uint64_t addr;
    uint16_t len, csum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t len;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

struct e1000 {
    struct netif nif;
    volatile uint8_t *mmio;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rx_bufs, *tx_bufs;
    uint32_t rx_next, tx_next;
    spinlock_t lock;
    bool irq;
};

static struct e1000 *devices[4];
static int ndevices;

static uint32_t rd(struct e1000 *e, uint32_t reg)
{
    return *(volatile uint32_t *)(e->mmio + reg);
}

static void wr(struct e1000 *e, uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(e->mmio + reg) = v;
}

static void poll_rx(struct e1000 *e)
{
    uint64_t flags = spin_lock_irqsave(&e->lock);

    for (;;) {
        struct rx_desc *d = &e->rx[e->rx_next];
        struct pkt *p;

        if (!(d->status & DESC_DD))
            break;
        if (!d->errors && d->len <= BUF_SIZE && (p = pkt_alloc())) {
            memcpy(p->data, e->rx_bufs + (size_t)e->rx_next * BUF_SIZE, d->len);
            p->len = d->len;
            netif_rx(&e->nif, p);
        } else {
            e->nif.rx_dropped++;
        }
        d->status = 0;
        wr(e, REG_RDT, e->rx_next);
        e->rx_next = (e->rx_next + 1) % RX_COUNT;
    }
    spin_unlock_irqrestore(&e->lock, flags);
}

static void irq(struct interrupt_frame *frame)
{
    (void)frame;
    for (int i = 0; i < ndevices; i++) {
        uint32_t icr = rd(devices[i], REG_ICR);

        if (icr & ICR_LSC)
            netif_link_changed(&devices[i]->nif, rd(devices[i], REG_STATUS) & STATUS_LU);
        poll_rx(devices[i]);
    }
}

static int xmit(struct netif *nif, struct pkt *p)
{
    struct e1000 *e = nif->driver;
    uint64_t flags;
    struct tx_desc *d;
    int ret = 0;

    if (p->len > BUF_SIZE) {
        pkt_free(p);
        return -EMSGSIZE;
    }
    flags = spin_lock_irqsave(&e->lock);
    d = &e->tx[e->tx_next];
    if (d->cmd && !(d->status & DESC_DD)) {
        nif->tx_errors++;
        ret = -ENOBUFS;
    } else {
        memcpy(e->tx_bufs + (size_t)e->tx_next * BUF_SIZE, p->data, p->len);
        d->len = p->len;
        d->cso = 0;
        d->status = 0;
        d->cmd = TX_EOP | TX_IFCS | TX_RS;
        e->tx_next = (e->tx_next + 1) % TX_COUNT;
        wr(e, REG_TDT, e->tx_next);
    }
    spin_unlock_irqrestore(&e->lock, flags);
    pkt_free(p);
    return ret;
}

static void poll_thread(void *arg)
{
    struct e1000 *e = arg;
    bool was_up = false;

    for (;;) {
        bool up = rd(e, REG_STATUS) & STATUS_LU;

        // Link interrupts may be missed before the handler was installed.
        if (up != was_up) {
            netif_link_changed(&e->nif, up);
            was_up = up;
        }
        if (!e->irq)
            poll_rx(e);
        sched_sleep(e->irq ? 500 : 2);
    }
}

static uint16_t eeprom_read(struct e1000 *e, uint8_t addr)
{
    wr(e, REG_EERD, 1 | ((uint32_t)addr << 8));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(e, REG_EERD);

        if (v & (1U << 4))
            return v >> 16;
    }
    return 0;
}

static void device_init(const struct pci_device *pci)
{
    uint64_t bar = pci_bar(pci, 0, NULL);
    struct e1000 *e;
    uint32_t ral, rah;

    if (!bar || ndevices == (int)ARRAY_SIZE(devices) || !paging_map_mmio(bar, 0x20000)
        || !(e = kzalloc(sizeof(*e))))
        return;
    pci_enable(pci);
    e->mmio = (volatile uint8_t *)bar;
    e->lock = (spinlock_t)SPINLOCK_INIT;

    wr(e, REG_IMC, 0xFFFFFFFF);
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000 && (rd(e, REG_CTRL) & CTRL_RST); i++)
        __asm__ volatile ("pause");
    wr(e, REG_IMC, 0xFFFFFFFF);
    rd(e, REG_ICR);
    wr(e, REG_CTRL, (rd(e, REG_CTRL) | CTRL_SLU | CTRL_ASDE) & ~(1U << 3));

    // The address is in the receive address registers, or else the EEPROM.
    ral = rd(e, REG_RAL);
    rah = rd(e, REG_RAH);
    if (ral || (rah & 0xFFFF)) {
        for (int i = 0; i < 4; i++)
            e->nif.mac[i] = ral >> (8 * i);
        e->nif.mac[4] = rah;
        e->nif.mac[5] = rah >> 8;
    } else {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom_read(e, i);
            e->nif.mac[2 * i] = w;
            e->nif.mac[2 * i + 1] = w >> 8;
        }
        wr(e, REG_RAL, e->nif.mac[0] | e->nif.mac[1] << 8 | e->nif.mac[2] << 16 | (uint32_t)e->nif.mac[3] << 24);
        wr(e, REG_RAH, e->nif.mac[4] | e->nif.mac[5] << 8 | (1U << 31));
    }
    for (int i = 0; i < 128; i++)
        wr(e, REG_MTA + i * 4, 0);

    e->rx = (struct rx_desc *)pmm_alloc_page();
    e->tx = (struct tx_desc *)pmm_alloc_page();
    e->rx_bufs = (uint8_t *)pmm_alloc_pages(RX_COUNT * BUF_SIZE / PAGE_SIZE);
    e->tx_bufs = (uint8_t *)pmm_alloc_pages(TX_COUNT * BUF_SIZE / PAGE_SIZE);
    if (!e->rx || !e->tx || !e->rx_bufs || !e->tx_bufs)
        return;
    memset(e->rx, 0, PAGE_SIZE);
    memset(e->tx, 0, PAGE_SIZE);
    for (int i = 0; i < RX_COUNT; i++)
        e->rx[i].addr = (uint64_t)(e->rx_bufs + (size_t)i * BUF_SIZE);
    for (int i = 0; i < TX_COUNT; i++)
        e->tx[i].addr = (uint64_t)(e->tx_bufs + (size_t)i * BUF_SIZE);

    wr(e, REG_RDBAL, (uint64_t)e->rx);
    wr(e, REG_RDBAH, (uint64_t)e->rx >> 32);
    wr(e, REG_RDLEN, RX_COUNT * sizeof(struct rx_desc));
    wr(e, REG_RDH, 0);
    wr(e, REG_RDT, RX_COUNT - 1);
    wr(e, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    wr(e, REG_TDBAL, (uint64_t)e->tx);
    wr(e, REG_TDBAH, (uint64_t)e->tx >> 32);
    wr(e, REG_TDLEN, TX_COUNT * sizeof(struct tx_desc));
    wr(e, REG_TDH, 0);
    wr(e, REG_TDT, 0);
    wr(e, REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) | (0x40 << 12));
    wr(e, REG_TIPG, 0x0060200A);

    devices[ndevices++] = e;
    e->irq = pci_enable_msi(pci, irq) >= 0 || pci_enable_msix(pci, 1, irq) >= 0;
    if (e->irq)
        wr(e, REG_IMS, ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0);
    e->nif.xmit = xmit;
    e->nif.driver = e;
    netif_register(&e->nif);
    if (!e->irq)
        kprintf("e1000: no MSI; polling\n");
    thread_create("e1000", poll_thread, e);
}

void e1000_init(void)
{
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x10D3, 0x10EA, 0x1502, 0x153A, 0x15B8 };
    const struct pci_device *pci;

    for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
        for (size_t n = 0; (pci = pci_find_id(0x8086, ids[i], n)); n++)
            device_init(pci);
    }
}
