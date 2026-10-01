#include "block.h"
#include "cpu.h"
#include "mem.h"
#include "pci.h"
#include "sched.h"
#include "string.h"

#define NVME_CAP        0x00
#define NVME_CC         0x14
#define NVME_CSTS       0x1C
#define NVME_AQA        0x24
#define NVME_ASQ        0x28
#define NVME_ACQ        0x30

#define QUEUE_DEPTH     64
#define BOUNCE_PAGES    16

struct nvme_cmd {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed));

struct nvme_cqe {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

struct nvme_queue {
    struct nvme_cmd *sq;
    volatile struct nvme_cqe *cq;
    uint16_t sq_tail, cq_head;
    uint8_t phase;
    uint16_t id;
};

struct nvme_ctrl {
    volatile uint8_t *regs;
    uint32_t stride;
    struct nvme_queue admin, io;
    uint8_t *bounce;
    uint64_t *prp_list;
    uint16_t next_cid;
    uint32_t max_bytes;
};

struct nvme_ns {
    struct block_device dev;
    struct nvme_ctrl *ctrl;
    uint32_t nsid;
};

static int ctrl_count;

static inline uint32_t rd32(struct nvme_ctrl *c, uint32_t reg)
{
    return *(volatile uint32_t *)(c->regs + reg);
}

static inline void wr32(struct nvme_ctrl *c, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(c->regs + reg) = val;
}

static inline void wr64(struct nvme_ctrl *c, uint32_t reg, uint64_t val)
{
    *(volatile uint32_t *)(c->regs + reg) = val;
    *(volatile uint32_t *)(c->regs + reg + 4) = val >> 32;
}

static bool queue_alloc(struct nvme_queue *q, uint16_t id)
{
    uint8_t *mem = (uint8_t *)pmm_alloc_pages(2);

    if (!mem)
        return false;
    memset(mem, 0, 2 * PAGE_SIZE);
    q->sq = (struct nvme_cmd *)mem;
    q->cq = (volatile struct nvme_cqe *)(mem + PAGE_SIZE);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    q->id = id;
    return true;
}

static int submit(struct nvme_ctrl *c, struct nvme_queue *q, struct nvme_cmd *cmd, uint32_t *result)
{
    volatile struct nvme_cqe *e;
    uint16_t status;

    cmd->cid = c->next_cid++;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (q->sq_tail + 1) % QUEUE_DEPTH;
    wr32(c, 0x1000 + (2 * q->id) * c->stride, q->sq_tail);

    e = &q->cq[q->cq_head];
    for (uint64_t spins = 0; (e->status & 1) != q->phase; spins++) {
        if (spins > 1000)
            sched_yield();
        if (spins > 50000000)
            return -1;
    }

    status = e->status >> 1;
    if (result)
        *result = e->result;
    if (++q->cq_head == QUEUE_DEPTH) {
        q->cq_head = 0;
        q->phase ^= 1;
    }
    wr32(c, 0x1000 + (2 * q->id + 1) * c->stride, q->cq_head);
    return status ? -1 : 0;
}

static void set_prps(struct nvme_ctrl *c, struct nvme_cmd *cmd, uint32_t bytes)
{
    uint32_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;

    cmd->prp1 = (uint64_t)c->bounce;
    cmd->prp2 = 0;
    if (pages == 2) {
        cmd->prp2 = (uint64_t)c->bounce + PAGE_SIZE;
    } else if (pages > 2) {
        for (uint32_t i = 1; i < pages; i++)
            c->prp_list[i - 1] = (uint64_t)c->bounce + i * PAGE_SIZE;
        cmd->prp2 = (uint64_t)c->prp_list;
    }
}

static int rw(struct block_device *dev, uint64_t lba, uint32_t count, bool write)
{
    struct nvme_ns *ns = dev->driver;
    struct nvme_cmd cmd = {
        .opcode = write ? 0x01 : 0x02,
        .nsid = ns->nsid,
        .cdw10 = lba,
        .cdw11 = lba >> 32,
        .cdw12 = count - 1,
    };

    set_prps(ns->ctrl, &cmd, count * dev->sector_size);
    return submit(ns->ctrl, &ns->ctrl->io, &cmd, NULL);
}

static int nvme_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct nvme_ns *ns = dev->driver;

    if (rw(dev, lba, count, false) != 0)
        return -1;
    memcpy(buf, ns->ctrl->bounce, count * dev->sector_size);
    return 0;
}

static int nvme_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct nvme_ns *ns = dev->driver;

    memcpy(ns->ctrl->bounce, buf, count * dev->sector_size);
    return rw(dev, lba, count, true);
}

static int nvme_flush(struct block_device *dev)
{
    struct nvme_ns *ns = dev->driver;
    struct nvme_cmd cmd = { .opcode = 0x00, .nsid = ns->nsid };

    return submit(ns->ctrl, &ns->ctrl->io, &cmd, NULL);
}

static const struct block_ops nvme_ops = { nvme_read, nvme_write, nvme_flush };

static bool wait_ready(struct nvme_ctrl *c, bool ready, uint64_t timeout_ms)
{
    for (uint64_t spins = 0; spins < timeout_ms * 10000; spins++) {
        if (((rd32(c, NVME_CSTS) & 1) != 0) == ready)
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void add_namespace(struct nvme_ctrl *c, uint32_t nsid, int index)
{
    struct nvme_cmd cmd = { .opcode = 0x06, .nsid = nsid, .cdw10 = 0 };
    struct nvme_ns *ns;
    uint64_t size;
    uint8_t flbas, lbads;

    set_prps(c, &cmd, PAGE_SIZE);
    if (submit(c, &c->admin, &cmd, NULL) != 0)
        return;
    size = *(uint64_t *)c->bounce;
    flbas = c->bounce[26] & 0xF;
    lbads = c->bounce[128 + flbas * 4 + 2];
    if (!size || lbads < 9 || lbads > 12 || !(ns = kzalloc(sizeof(*ns))))
        return;

    ns->ctrl = c;
    ns->nsid = nsid;
    ksnprintf(ns->dev.name, sizeof(ns->dev.name), "nvme%dn%d", index, nsid);
    ns->dev.sector_size = 1U << lbads;
    ns->dev.sector_count = size;
    ns->dev.max_sectors = c->max_bytes / ns->dev.sector_size;
    ns->dev.ops = &nvme_ops;
    ns->dev.driver = ns;

    if (block_register(&ns->dev) == 0)
        partition_scan(&ns->dev);
}

static void controller_init(const struct pci_device *pci)
{
    uint64_t bar = pci_bar(pci, 0, NULL);
    struct nvme_ctrl *c;
    uint64_t cap;
    uint32_t nn;
    struct nvme_cmd cmd;

    if (!bar || !paging_map_mmio(bar, 0x2000) || !(c = kzalloc(sizeof(*c))))
        return;
    pci_enable(pci);
    c->regs = (volatile uint8_t *)bar;
    cap = *(volatile uint64_t *)(c->regs + NVME_CAP);
    c->stride = 4U << ((cap >> 32) & 0xF);

    c->bounce = (uint8_t *)pmm_alloc_pages(BOUNCE_PAGES);
    c->prp_list = (uint64_t *)pmm_alloc_page();
    if (!c->bounce || !c->prp_list || !queue_alloc(&c->admin, 0) || !queue_alloc(&c->io, 1))
        return;

    wr32(c, NVME_CC, 0);
    if (!wait_ready(c, false, 5000))
        return;
    wr32(c, NVME_AQA, (QUEUE_DEPTH - 1) | (QUEUE_DEPTH - 1) << 16);
    wr64(c, NVME_ASQ, (uint64_t)c->admin.sq);
    wr64(c, NVME_ACQ, (uint64_t)c->admin.cq);
    wr32(c, NVME_CC, 1 | (6 << 16) | (4 << 20));
    if (!wait_ready(c, true, 5000))
        return;

    cmd = (struct nvme_cmd){ .opcode = 0x06, .cdw10 = 1 };
    set_prps(c, &cmd, PAGE_SIZE);
    if (submit(c, &c->admin, &cmd, NULL) != 0)
        return;
    nn = *(uint32_t *)(c->bounce + 516);
    c->max_bytes = BOUNCE_PAGES * PAGE_SIZE;
    if (c->bounce[77] && c->bounce[77] < 4)
        c->max_bytes = MIN(c->max_bytes, (PAGE_SIZE << c->bounce[77]));

    cmd = (struct nvme_cmd){ .opcode = 0x05, .prp1 = (uint64_t)c->io.cq,
                             .cdw10 = 1 | (QUEUE_DEPTH - 1) << 16, .cdw11 = 1 };
    if (submit(c, &c->admin, &cmd, NULL) != 0)
        return;
    cmd = (struct nvme_cmd){ .opcode = 0x01, .prp1 = (uint64_t)c->io.sq,
                             .cdw10 = 1 | (QUEUE_DEPTH - 1) << 16, .cdw11 = 1 | 1 << 16 };
    if (submit(c, &c->admin, &cmd, NULL) != 0)
        return;

    int index = ctrl_count++;
    for (uint32_t nsid = 1; nsid <= nn && nsid <= 16; nsid++)
        add_namespace(c, nsid, index);
}

void nvme_init(void)
{
    const struct pci_device *pci;

    for (size_t n = 0; (pci = pci_find_class(0x01, 0x08, 0x02, n)); n++)
        controller_init(pci);
}
