#include "stopcodes.h"
#include "mem.h"
#include "cpu.h"
#include "spinlock.h"
#include "string.h"

// Kernel heap. Small requests come from per-size free lists filled from
// 64 KiB slabs; large ones get their own run of pages. Every allocation has
// a 16-byte header recording its class, so kfree is constant time and
// catches double frees.

#define HEADER          16
#define SLAB_BYTES      (64 * 1024)
#define MAGIC_USED      0xA110CA7EU
#define MAGIC_FREE      0xF4EEF4EEU
#define CLASS_PAGES     0xFFFF

static const uint32_t sizes[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
#define NCLASSES        (sizeof(sizes) / sizeof(sizes[0]))

struct header {
    uint32_t magic;
    uint32_t cls;                   // index into sizes[], or CLASS_PAGES
    uint64_t pages;                 // for page allocations
};

struct free_obj {
    struct header h;
    struct free_obj *next;
};

static struct {
    spinlock_t lock;
    struct free_obj *free;
} classes[NCLASSES];

_Static_assert(sizeof(struct header) == HEADER, "heap header size");

static bool refill(unsigned c)
{
    uint64_t base = pmm_alloc_pages(SLAB_BYTES / PAGE_SIZE);
    uint32_t size = sizes[c];

    if (!base)
        return false;
    for (uint64_t off = 0; off + size <= SLAB_BYTES; off += size) {
        struct free_obj *o = (struct free_obj *)(base + off);

        o->h.magic = MAGIC_FREE;
        o->h.cls = c;
        o->next = classes[c].free;
        classes[c].free = o;
    }
    return true;
}

void *kmalloc(size_t size)
{
    size_t need;
    uint64_t flags;

    if (size == 0 || size > (1ULL << 36))
        return NULL;
    need = size + HEADER;
    for (unsigned c = 0; c < NCLASSES; c++) {
        struct free_obj *o;

        if (need > sizes[c])
            continue;
        flags = spin_lock_irqsave(&classes[c].lock);
        if (!classes[c].free && !refill(c)) {
            spin_unlock_irqrestore(&classes[c].lock, flags);
            return NULL;
        }
        o = classes[c].free;
        classes[c].free = o->next;
        spin_unlock_irqrestore(&classes[c].lock, flags);
        o->h.magic = MAGIC_USED;
        return (uint8_t *)o + HEADER;
    }

    uint64_t pages = ALIGN_UP(need, PAGE_SIZE) / PAGE_SIZE;
    struct header *h = (struct header *)pmm_alloc_pages(pages);

    if (!h)
        return NULL;
    h->magic = MAGIC_USED;
    h->cls = CLASS_PAGES;
    h->pages = pages;
    return (uint8_t *)h + HEADER;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    struct header *h;
    uint64_t flags;

    if (!ptr)
        return;
    h = (struct header *)((uint8_t *)ptr - HEADER);
    if (h->magic != MAGIC_USED)
        panic_code(STOP_HEAP_CORRUPTION, "kfree: %p was not allocated or is already free", ptr);
    if (h->cls == CLASS_PAGES) {
        h->magic = MAGIC_FREE;
        pmm_free_pages((uint64_t)h, h->pages);
        return;
    }
    if (h->cls >= NCLASSES)
        panic_code(STOP_HEAP_CORRUPTION, "kfree: %p has a corrupt header", ptr);

    struct free_obj *o = (struct free_obj *)h;
    unsigned c = h->cls;

    o->h.magic = MAGIC_FREE;
    flags = spin_lock_irqsave(&classes[c].lock);
    o->next = classes[c].free;
    classes[c].free = o;
    spin_unlock_irqrestore(&classes[c].lock, flags);
}
