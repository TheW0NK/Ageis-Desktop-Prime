#include "net.h"
#include "cpu.h"
#include "mem.h"
#include "pci.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"

// virtio-net over the legacy (transitional) virtio-pci interface, with an
// MSI-X interrupt when available and polling otherwise.

#define VIRTIO_FEATURES     0x00
#define VIRTIO_GUEST_FEAT   0x04
#define VIRTIO_QUEUE_PFN    0x08
#define VIRTIO_QUEUE_SIZE   0x0C
#define VIRTIO_QUEUE_SELECT 0x0E
#define VIRTIO_QUEUE_NOTIFY 0x10
#define VIRTIO_STATUS       0x12
#define VIRTIO_ISR          0x13
#define VIRTIO_MSI_CONFIG   0x14
#define VIRTIO_MSI_QUEUE    0x16

#define STATUS_ACK          1
#define STATUS_DRIVER       2
#define STATUS_DRIVER_OK    4
#define STATUS_FAILED       128

#define NET_F_MAC           (1U << 5)
#define NET_F_STATUS        (1U << 16)

#define DESC_WRITE          2
#define BUF_SIZE            2048
#define HDR_LEN             10

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vq {
    uint16_t size;
    struct vring_desc *desc;
    volatile uint16_t *avail;
    volatile uint8_t *used;
    uint16_t last_used;
    uint8_t *bufs;                  // one BUF_SIZE buffer per descriptor
    uint16_t free_head, nfree;
};

struct vnet {
    struct netif nif;
    uint16_t io, config;
    struct vq rx, tx;
    spinlock_t lock;
    bool has_status;
    bool msix;
};

static struct vnet *devices[4];
static int ndevices;

static bool vq_init(struct vnet *v, int index, struct vq *q)
{
    size_t avail_size, ring_bytes;
    uint8_t *ring;

    outw(v->io + VIRTIO_QUEUE_SELECT, index);
    q->size = inw(v->io + VIRTIO_QUEUE_SIZE);
    if (q->size < 2)
        return false;
    avail_size = 6 + 2 * q->size;
    ring_bytes = ALIGN_UP(16 * q->size + avail_size, PAGE_SIZE) + ALIGN_UP(6 + 8 * q->size, PAGE_SIZE);
    ring = (uint8_t *)pmm_alloc_pages(ring_bytes / PAGE_SIZE);
    q->bufs = (uint8_t *)pmm_alloc_pages(ALIGN_UP((size_t)q->size * BUF_SIZE, PAGE_SIZE) / PAGE_SIZE);
    if (!ring || !q->bufs)
        return false;
    memset(ring, 0, ring_bytes);
    q->desc = (struct vring_desc *)ring;
    q->avail = (volatile uint16_t *)(ring + 16 * q->size);
    q->used = ring + ALIGN_UP(16 * q->size + avail_size, PAGE_SIZE);
    for (uint16_t i = 0; i < q->size; i++) {
        q->desc[i].addr = (uint64_t)(q->bufs + (size_t)i * BUF_SIZE);
        q->desc[i].next = i + 1;
    }
    q->free_head = 0;
    q->nfree = q->size;
    if (v->msix)
        outw(v->io + VIRTIO_MSI_QUEUE, 0);
    outl(v->io + VIRTIO_QUEUE_PFN, (uint64_t)ring / PAGE_SIZE);
    return true;
}

static void vq_publish(struct vnet *v, struct vq *q, int index, uint16_t id)
{
    uint16_t idx = q->avail[1];

    q->avail[2 + idx % q->size] = id;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail[1] = idx + 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    outw(v->io + VIRTIO_QUEUE_NOTIFY, index);
}

static void rx_post(struct vnet *v, uint16_t id)
{
    v->rx.desc[id].len = BUF_SIZE;
    v->rx.desc[id].flags = DESC_WRITE;
    vq_publish(v, &v->rx, 0, id);
}

static void reclaim_tx(struct vnet *v)
{
    struct vq *q = &v->tx;

    while (*(volatile uint16_t *)(q->used + 2) != q->last_used) {
        uint32_t id = *(volatile uint32_t *)(q->used + 4 + (q->last_used % q->size) * 8);

        q->desc[id].next = q->free_head;
        q->free_head = id;
        q->nfree++;
        q->last_used++;
    }
}

static void poll_rx(struct vnet *v)
{
    struct vq *q = &v->rx;
    uint64_t flags = spin_lock_irqsave(&v->lock);

    while (*(volatile uint16_t *)(q->used + 2) != q->last_used) {
        volatile uint8_t *e = q->used + 4 + (q->last_used % q->size) * 8;
        uint32_t id = *(volatile uint32_t *)e, len = *(volatile uint32_t *)(e + 4);
        struct pkt *p;

        q->last_used++;
        if (id < q->size && len > HDR_LEN && len <= BUF_SIZE && (p = pkt_alloc())) {
            memcpy(p->data, q->bufs + (size_t)id * BUF_SIZE + HDR_LEN, len - HDR_LEN);
            p->len = len - HDR_LEN;
            netif_rx(&v->nif, p);
        } else {
            v->nif.rx_dropped++;
        }
        if (id < q->size)
            rx_post(v, id);
    }
    reclaim_tx(v);
    spin_unlock_irqrestore(&v->lock, flags);
}

static bool link_up(struct vnet *v)
{
    return !v->has_status || (inw(v->io + v->config + 6) & 1);
}

static void irq(struct interrupt_frame *frame)
{
    (void)frame;
    for (int i = 0; i < ndevices; i++) {
        if (!devices[i]->msix)
            inb(devices[i]->io + VIRTIO_ISR);
        poll_rx(devices[i]);
    }
}

static int xmit(struct netif *nif, struct pkt *p)
{
    struct vnet *v = nif->driver;
    struct vq *q = &v->tx;
    uint64_t flags;
    uint16_t id;
    int ret = 0;

    if (p->len + HDR_LEN > BUF_SIZE) {
        pkt_free(p);
        return -EMSGSIZE;
    }
    flags = spin_lock_irqsave(&v->lock);
    reclaim_tx(v);
    if (!q->nfree) {
        nif->tx_errors++;
        ret = -ENOBUFS;
    } else {
        id = q->free_head;
        q->free_head = q->desc[id].next;
        q->nfree--;
        memset(q->bufs + (size_t)id * BUF_SIZE, 0, HDR_LEN);
        memcpy(q->bufs + (size_t)id * BUF_SIZE + HDR_LEN, p->data, p->len);
        q->desc[id].len = p->len + HDR_LEN;
        q->desc[id].flags = 0;
        vq_publish(v, q, 1, id);
    }
    spin_unlock_irqrestore(&v->lock, flags);
    pkt_free(p);
    return ret;
}

// Without an interrupt, and to notice link changes, a thread polls.
static void poll_thread(void *arg)
{
    struct vnet *v = arg;
    bool was_up = false;

    for (;;) {
        bool up = link_up(v);

        if (up != was_up) {
            netif_link_changed(&v->nif, up);
            was_up = up;
        }
        if (!v->msix)
            poll_rx(v);
        sched_sleep(v->msix ? 500 : 2);
    }
}

static void device_init(const struct pci_device *pci)
{
    bool is_io;
    uint16_t io = pci_bar(pci, 0, &is_io);
    struct vnet *v;
    uint32_t features;

    if (!is_io || !io || ndevices == (int)ARRAY_SIZE(devices) || !(v = kzalloc(sizeof(*v))))
        return;
    pci_enable(pci);
    v->io = io;
    v->lock = (spinlock_t)SPINLOCK_INIT;

    outb(io + VIRTIO_STATUS, 0);
    outb(io + VIRTIO_STATUS, STATUS_ACK);
    outb(io + VIRTIO_STATUS, STATUS_ACK | STATUS_DRIVER);
    features = inl(io + VIRTIO_FEATURES);
    v->has_status = features & NET_F_STATUS;
    outl(io + VIRTIO_GUEST_FEAT, features & (NET_F_MAC | NET_F_STATUS));

    devices[ndevices++] = v;
    // With MSI-X on, the legacy layout grows two vector registers and the
    // device configuration moves from 0x14 to 0x18.
    v->msix = pci_enable_msix(pci, 1, irq) >= 0;
    v->config = v->msix ? 0x18 : 0x14;
    if (v->msix)
        outw(io + VIRTIO_MSI_CONFIG, 0xFFFF);

    if (!vq_init(v, 0, &v->rx) || !vq_init(v, 1, &v->tx)) {
        outb(io + VIRTIO_STATUS, STATUS_FAILED);
        ndevices--;
        return;
    }
    for (int i = 0; i < 6; i++)
        v->nif.mac[i] = (features & NET_F_MAC) ? inb(io + v->config + i) : (uint8_t)(0x52 + i);
    v->nif.xmit = xmit;
    v->nif.driver = v;
    outb(io + VIRTIO_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_DRIVER_OK);
    for (uint16_t i = 0; i < v->rx.size; i++)
        rx_post(v, i);
    netif_register(&v->nif);
    if (!v->msix)
        kprintf("virtio-net: no MSI-X; polling\n");
    thread_create("virtio-net", poll_thread, v);
}

void virtio_net_init(void)
{
    const struct pci_device *pci;

    for (size_t n = 0; (pci = pci_find_id(0x1AF4, 0x1000, n)); n++)
        device_init(pci);
}
