#include "block.h"
#include "cpu.h"
#include "mem.h"
#include "pci.h"
#include "sched.h"
#include "string.h"

#define HBA_CAP         0x00
#define HBA_GHC         0x04
#define HBA_PI          0x0C
#define GHC_AE          (1U << 31)

#define PORT_BASE(n)    (0x100 + (n) * 0x80)
#define PX_CLB          0x00
#define PX_CLBU         0x04
#define PX_FB           0x08
#define PX_FBU          0x0C
#define PX_IS           0x10
#define PX_IE           0x14
#define PX_CMD          0x18
#define PX_TFD          0x20
#define PX_SIG          0x24
#define PX_SSTS         0x28
#define PX_SERR         0x30
#define PX_CI           0x38

#define CMD_ST          (1U << 0)
#define CMD_FRE         (1U << 4)
#define CMD_FR          (1U << 14)
#define CMD_CR          (1U << 15)

#define TFD_ERR         (1U << 0)
#define TFD_DRQ         (1U << 3)
#define TFD_BSY         (1U << 7)
#define IS_TFES         (1U << 30)

#define ATA_IDENTIFY    0xEC
#define ATA_READ_EXT    0x25
#define ATA_WRITE_EXT   0x35
#define ATA_FLUSH_EXT   0xEA

#define SIG_ATA         0x00000101
#define MAX_SECTORS     128
#define BOUNCE_SIZE     (MAX_SECTORS * 512)

struct cmd_header {
    uint16_t flags;
    uint16_t prdt_length;
    uint32_t prdbc;
    uint64_t ctba;
    uint32_t reserved[4];
} __attribute__((packed));

struct prdt_entry {
    uint64_t dba;
    uint32_t reserved;
    uint32_t dbc;
} __attribute__((packed));

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prdt_entry prdt[1];
} __attribute__((packed));

struct ahci_port {
    struct block_device dev;
    volatile uint8_t *regs;
    struct cmd_header *clist;
    struct cmd_table *table;
    uint8_t *bounce;
};

static int ahci_count;

static inline uint32_t rd(volatile uint8_t *base, uint32_t reg)
{
    return *(volatile uint32_t *)(base + reg);
}

static inline void wr(volatile uint8_t *base, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(base + reg) = val;
}

static bool wait_clear(volatile uint8_t *regs, uint32_t reg, uint32_t mask, uint64_t spins)
{
    while (spins--) {
        if (!(rd(regs, reg) & mask))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static int issue(struct ahci_port *p, uint8_t command, uint64_t lba, uint32_t count,
                 uint32_t bytes, bool write)
{
    struct cmd_table *t = p->table;
    uint8_t *fis = t->cfis;

    if (!wait_clear(p->regs, PX_TFD, TFD_BSY | TFD_DRQ, 10000000))
        return -1;

    memset(t, 0, sizeof(*t));
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = command;
    fis[4] = lba;
    fis[5] = lba >> 8;
    fis[6] = lba >> 16;
    fis[7] = 1 << 6;
    fis[8] = lba >> 24;
    fis[9] = lba >> 32;
    fis[10] = lba >> 40;
    fis[12] = count;
    fis[13] = count >> 8;

    p->clist[0].flags = 5 | (write ? 1 << 6 : 0);
    p->clist[0].prdt_length = bytes ? 1 : 0;
    p->clist[0].prdbc = 0;
    if (bytes) {
        t->prdt[0].dba = (uint64_t)p->bounce;
        t->prdt[0].dbc = (bytes - 1);
    }

    wr(p->regs, PX_IS, 0xFFFFFFFF);
    wr(p->regs, PX_CI, 1);

    for (uint64_t spins = 0; rd(p->regs, PX_CI) & 1; spins++) {
        if (rd(p->regs, PX_IS) & IS_TFES)
            return -1;
        if (spins > 1000)
            sched_yield();
        if (spins > 50000000)
            return -1;
    }
    return (rd(p->regs, PX_TFD) & TFD_ERR) ? -1 : 0;
}

static int ahci_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = dev->driver;

    if (issue(p, ATA_READ_EXT, lba, count, count * 512, false) != 0)
        return -1;
    memcpy(buf, p->bounce, count * 512);
    return 0;
}

static int ahci_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct ahci_port *p = dev->driver;

    memcpy(p->bounce, buf, count * 512);
    return issue(p, ATA_WRITE_EXT, lba, count, count * 512, true);
}

static int ahci_flush(struct block_device *dev)
{
    return issue(dev->driver, ATA_FLUSH_EXT, 0, 0, 0, false);
}

static const struct block_ops ahci_ops = { ahci_read, ahci_write, ahci_flush };

static void port_stop(volatile uint8_t *regs)
{
    wr(regs, PX_CMD, rd(regs, PX_CMD) & ~CMD_ST);
    wait_clear(regs, PX_CMD, CMD_CR, 5000000);
    wr(regs, PX_CMD, rd(regs, PX_CMD) & ~CMD_FRE);
    wait_clear(regs, PX_CMD, CMD_FR, 5000000);
}

static void port_init(volatile uint8_t *regs)
{
    uint8_t *mem = (uint8_t *)pmm_alloc_pages(2 + BOUNCE_SIZE / PAGE_SIZE);
    struct ahci_port *p;
    uint16_t *id;

    if (!mem || !(p = kzalloc(sizeof(*p))))
        return;
    memset(mem, 0, 2 * PAGE_SIZE);

    p->regs = regs;
    p->clist = (struct cmd_header *)mem;
    p->table = (struct cmd_table *)(mem + 1024);
    p->bounce = mem + 2 * PAGE_SIZE;
    p->clist[0].ctba = (uint64_t)p->table;

    port_stop(regs);
    wr(regs, PX_CLB, (uint64_t)p->clist);
    wr(regs, PX_CLBU, (uint64_t)p->clist >> 32);
    wr(regs, PX_FB, (uint64_t)(mem + PAGE_SIZE));
    wr(regs, PX_FBU, (uint64_t)(mem + PAGE_SIZE) >> 32);
    wr(regs, PX_SERR, 0xFFFFFFFF);
    wr(regs, PX_IS, 0xFFFFFFFF);
    wr(regs, PX_IE, 0);
    wr(regs, PX_CMD, rd(regs, PX_CMD) | CMD_FRE);
    wr(regs, PX_CMD, rd(regs, PX_CMD) | CMD_ST);

    if (issue(p, ATA_IDENTIFY, 0, 0, 512, false) != 0)
        return;
    id = (uint16_t *)p->bounce;

    ksnprintf(p->dev.name, sizeof(p->dev.name), "sata%d", ahci_count++);
    p->dev.kind = "SATA";
    p->dev.sector_size = 512;
    p->dev.sector_count = (uint64_t)id[100] | (uint64_t)id[101] << 16
                        | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
    if (!p->dev.sector_count)
        p->dev.sector_count = (uint32_t)id[60] | (uint32_t)id[61] << 16;
    p->dev.max_sectors = MAX_SECTORS;
    p->dev.ops = &ahci_ops;
    p->dev.driver = p;

    if (block_register(&p->dev) == 0)
        partition_scan(&p->dev);
}

void ahci_init(void)
{
    const struct pci_device *pci;

    for (size_t n = 0; (pci = pci_find_class(0x01, 0x06, 0x01, n)); n++) {
        uint64_t abar = pci_bar(pci, 5, NULL);
        volatile uint8_t *hba = (volatile uint8_t *)abar;
        uint32_t pi;

        if (!abar || !paging_map_mmio(abar, 0x1100))
            continue;
        pci_enable(pci);
        wr(hba, HBA_GHC, rd(hba, HBA_GHC) | GHC_AE);
        pi = rd(hba, HBA_PI);

        for (int port = 0; port < 32; port++) {
            volatile uint8_t *regs = hba + PORT_BASE(port);
            uint32_t ssts = rd(regs, PX_SSTS);

            if (!(pi & (1U << port)) || (ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
                continue;
            if (rd(regs, PX_SIG) == SIG_ATA)
                port_init(regs);
        }
    }
}
