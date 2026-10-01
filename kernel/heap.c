#include "mem.h"
#include "cpu.h"
#include "spinlock.h"
#include "string.h"

#define HEAP_ALIGN      16
#define HEAP_GROW_MIN   (64 * 1024)
#define HEAP_USED       ((struct block *)0xA5A5A5A5A5A5A5A5ULL)

struct block {
    size_t size;
    struct block *next;
};

struct chunk {
    struct chunk *next;
    uint64_t pages;
};

static spinlock_t heap_lock = SPINLOCK_INIT;
static struct block *free_list;
static struct chunk *chunks;
static uint64_t chunk_count;

static void insert_free(struct block *b)
{
    struct block **pp = &free_list;

    while (*pp && *pp < b)
        pp = &(*pp)->next;

    b->next = *pp;
    *pp = b;

    if (b->next && (char *)b + b->size == (char *)b->next) {
        b->size += b->next->size;
        b->next = b->next->next;
    }
    if (pp != &free_list) {
        struct block *prev = (struct block *)((char *)pp - offsetof(struct block, next));

        if ((char *)prev + prev->size == (char *)b) {
            prev->size += b->size;
            prev->next = b->next;
        }
    }
}

static bool grow(size_t need)
{
    uint64_t pages = ALIGN_UP(MAX(need + sizeof(struct chunk), HEAP_GROW_MIN), PAGE_SIZE) / PAGE_SIZE;
    uint64_t addr = pmm_alloc_pages(pages);
    struct chunk *c = (struct chunk *)addr;
    struct block *b = (struct block *)(c + 1);

    if (!addr)
        return false;
    c->pages = pages;
    c->next = chunks;
    chunks = c;
    chunk_count++;

    b->size = pages * PAGE_SIZE - sizeof(struct chunk);
    insert_free(b);
    return true;
}

// Returns chunks that are entirely free to the page allocator, keeping one.
static void release_chunks(void)
{
    for (struct chunk **cp = &chunks, *c; chunk_count > 1 && (c = *cp);) {
        struct block *first = (struct block *)(c + 1);
        size_t usable = c->pages * PAGE_SIZE - sizeof(struct chunk);
        struct block **bp = &free_list;

        while (*bp && *bp != first)
            bp = &(*bp)->next;

        if (*bp && first->size == usable) {
            *bp = first->next;
            *cp = c->next;
            chunk_count--;
            pmm_free_pages((uint64_t)c, c->pages);
        } else {
            cp = &c->next;
        }
    }
}

static void *alloc_locked(size_t size)
{
    size_t need;

    if (size == 0 || size > (1ULL << 40))
        return NULL;
    need = ALIGN_UP(size, HEAP_ALIGN) + sizeof(struct block);

    for (;;) {
        for (struct block **pp = &free_list, *b; (b = *pp); pp = &b->next) {
            if (b->size < need)
                continue;

            if (b->size - need >= sizeof(struct block) + HEAP_ALIGN) {
                struct block *rest = (struct block *)((char *)b + need);

                rest->size = b->size - need;
                rest->next = b->next;
                *pp = rest;
                b->size = need;
            } else {
                *pp = b->next;
            }
            b->next = HEAP_USED;
            return b + 1;
        }
        if (!grow(need))
            return NULL;
    }
}

void *kmalloc(size_t size)
{
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    void *p = alloc_locked(size);

    spin_unlock_irqrestore(&heap_lock, flags);
    return p;
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
    struct block *b;

    if (!ptr)
        return;
    b = (struct block *)ptr - 1;
    if (b->next != HEAP_USED)
        panic("kfree: %p was not allocated or is already free", ptr);

    uint64_t flags = spin_lock_irqsave(&heap_lock);
    insert_free(b);
    release_chunks();
    spin_unlock_irqrestore(&heap_lock, flags);
}
