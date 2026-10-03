#include "block.h"
#include "cpu.h"
#include "mem.h"
#include "pci.h"
#include "sched.h"
#include "string.h"

// Legacy (transitional) virtio-pci interface, which QEMU provides by default.
#define VIRTIO_FEATURES     0x00
#define VIRTIO_GUEST_FEAT   0x04
#define VIRTIO_QUEUE_PFN    0x08
#define VIRTIO_QUEUE_SIZE   0x0C
#define VIRTIO_QUEUE_SELECT 0x0E
#define VIRTIO_QUEUE_NOTIFY 0x10
#define VIRTIO_STATUS       0x12
#define VIRTIO_CONFIG       0x14

#define STATUS_ACK          1
#define STATUS_DRIVER       2
#define STATUS_DRIVER_OK    4
#define STATUS_FAILED       128

#define DESC_NEXT           1
#define DESC_WRITE          2

#define BLK_T_IN            0
#define BLK_T_OUT           1
#define BLK_T_FLUSH         4
#define BLK_F_FLUSH         (1U << 9)

#define MAX_SECTORS         128

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct virtio_blk {
    struct block_device dev;
    uint16_t io;
    uint16_t qsize;
    struct vring_desc *desc;
    volatile uint16_t *avail;
    volatile uint8_t *used;
    uint16_t last_used;
    struct blk_req *req;
    volatile uint8_t *status;
    uint8_t *bounce;
    bool can_flush;
};

static int vblk_count;

static int request(struct virtio_blk *v, uint32_t type, uint64_t sector, uint32_t bytes)
{
    uint16_t idx;

    v->req->type = type;
    v->req->reserved = 0;
    v->req->sector = sector;
    *v->status = 0xFF;

    v->desc[0] = (struct vring_desc){ (uint64_t)v->req, sizeof(*v->req), DESC_NEXT, 1 };
    if (bytes) {
        v->desc[1] = (struct vring_desc){ (uint64_t)v->bounce, bytes,
                                          DESC_NEXT | (type == BLK_T_IN ? DESC_WRITE : 0), 2 };
        v->desc[2] = (struct vring_desc){ (uint64_t)v->status, 1, DESC_WRITE, 0 };
    } else {
        v->desc[0].next = 2;
        v->desc[2] = (struct vring_desc){ (uint64_t)v->status, 1, DESC_WRITE, 0 };
    }

    idx = v->avail[1];
    v->avail[2 + idx % v->qsize] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->avail[1] = idx + 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    outw(v->io + VIRTIO_QUEUE_NOTIFY, 0);

    for (uint64_t spins = 0; *(volatile uint16_t *)(v->used + 2) == v->last_used; spins++) {
        if (spins > 1000)
            sched_yield();
        if (spins > 50000000)
            return -1;
    }
    v->last_used++;
    return *v->status == 0 ? 0 : -1;
}

static int vblk_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct virtio_blk *v = dev->driver;

    if (request(v, BLK_T_IN, lba, count * 512) != 0)
        return -1;
    memcpy(buf, v->bounce, count * 512);
    return 0;
}

static int vblk_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct virtio_blk *v = dev->driver;

    memcpy(v->bounce, buf, count * 512);
    return request(v, BLK_T_OUT, lba, count * 512);
}

static int vblk_flush(struct block_device *dev)
{
    struct virtio_blk *v = dev->driver;

    return v->can_flush ? request(v, BLK_T_FLUSH, 0, 0) : 0;
}

static const struct block_ops vblk_ops = { vblk_read, vblk_write, vblk_flush };

static void device_init(const struct pci_device *pci)
{
    bool is_io;
    uint16_t io = pci_bar(pci, 0, &is_io);
    struct virtio_blk *v;
    uint32_t features;
    size_t avail_size, ring_bytes;
    uint8_t *ring;

    if (!is_io || !io || !(v = kzalloc(sizeof(*v))))
        return;
    pci_enable(pci);
    v->io = io;

    outb(io + VIRTIO_STATUS, 0);
    outb(io + VIRTIO_STATUS, STATUS_ACK);
    outb(io + VIRTIO_STATUS, STATUS_ACK | STATUS_DRIVER);
    features = inl(io + VIRTIO_FEATURES);
    v->can_flush = features & BLK_F_FLUSH;
    outl(io + VIRTIO_GUEST_FEAT, features & BLK_F_FLUSH);

    outw(io + VIRTIO_QUEUE_SELECT, 0);
    v->qsize = inw(io + VIRTIO_QUEUE_SIZE);
    if (v->qsize < 3)
        goto fail;

    avail_size = 6 + 2 * v->qsize;
    ring_bytes = ALIGN_UP(16 * v->qsize + avail_size, PAGE_SIZE) + ALIGN_UP(6 + 8 * v->qsize, PAGE_SIZE);
    ring = (uint8_t *)pmm_alloc_pages(ring_bytes / PAGE_SIZE);
    v->bounce = (uint8_t *)pmm_alloc_pages(MAX_SECTORS * 512 / PAGE_SIZE + 1);
    if (!ring || !v->bounce)
        goto fail;
    memset(ring, 0, ring_bytes);

    v->desc = (struct vring_desc *)ring;
    v->avail = (volatile uint16_t *)(ring + 16 * v->qsize);
    v->used = ring + ALIGN_UP(16 * v->qsize + avail_size, PAGE_SIZE);
    v->req = (struct blk_req *)(v->bounce + MAX_SECTORS * 512);
    v->status = (volatile uint8_t *)(v->req + 1);
    outl(io + VIRTIO_QUEUE_PFN, (uint64_t)ring / PAGE_SIZE);
    outb(io + VIRTIO_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_DRIVER_OK);

    ksnprintf(v->dev.name, sizeof(v->dev.name), "vd%c", 'a' + vblk_count++);
    v->dev.kind = "Virtual";
    v->dev.sector_size = 512;
    v->dev.sector_count = (uint64_t)inl(io + VIRTIO_CONFIG) | (uint64_t)inl(io + VIRTIO_CONFIG + 4) << 32;
    v->dev.max_sectors = MAX_SECTORS;
    v->dev.ops = &vblk_ops;
    v->dev.driver = v;

    if (block_register(&v->dev) == 0)
        partition_scan(&v->dev);
    return;

fail:
    outb(io + VIRTIO_STATUS, STATUS_FAILED);
}

void virtio_blk_init(void)
{
    const struct pci_device *pci;

    for (size_t n = 0; (pci = pci_find_id(0x1AF4, 0x1001, n)); n++)
        device_init(pci);
}
