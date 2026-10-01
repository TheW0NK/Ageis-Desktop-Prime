#include "bcache.h"
#include "mem.h"
#include "string.h"

#define BCACHE_MAX      4096
#define HASH_SIZE       1024
#define MERGE_BYTES     (256 * 1024)

static struct mutex cache_lock = MUTEX_INIT;
static struct buf *hash[HASH_SIZE];
static struct buf *lru_head, *lru_tail;
static size_t buf_count;

static unsigned hash_of(struct block_device *dev, uint64_t block)
{
    return ((uint64_t)dev / 64 ^ block * 2654435761U) % HASH_SIZE;
}

static void lru_remove(struct buf *b)
{
    if (b->lru_prev)
        b->lru_prev->lru_next = b->lru_next;
    else
        lru_head = b->lru_next;
    if (b->lru_next)
        b->lru_next->lru_prev = b->lru_prev;
    else
        lru_tail = b->lru_prev;
    b->lru_prev = b->lru_next = NULL;
}

static void lru_push_front(struct buf *b)
{
    b->lru_next = lru_head;
    b->lru_prev = NULL;
    if (lru_head)
        lru_head->lru_prev = b;
    lru_head = b;
    if (!lru_tail)
        lru_tail = b;
}

static void hash_remove(struct buf *b)
{
    struct buf **pp = &hash[hash_of(b->dev, b->block)];

    while (*pp && *pp != b)
        pp = &(*pp)->hash_next;
    if (*pp)
        *pp = b->hash_next;
}

static int write_out(struct buf *b)
{
    if (block_write(b->dev, b->block * b->size, b->data, b->size) != 0)
        return -1;
    b->dirty = false;
    return 0;
}

static int flush_locked(struct block_device *dev);

static struct buf *evict(void)
{
    // When the oldest buffers are dirty, write everything out in large
    // batches rather than one block at a time.
    if (lru_tail && lru_tail->dirty && !lru_tail->pinned)
        flush_locked(NULL);
    for (struct buf *b = lru_tail; b; b = b->lru_prev) {
        if (b->refs || b->pinned)
            continue;
        if (b->dirty && write_out(b) != 0)
            continue;
        hash_remove(b);
        lru_remove(b);
        return b;
    }
    return NULL;
}

static struct buf *lookup(struct block_device *dev, uint64_t block, uint32_t size)
{
    for (struct buf *b = hash[hash_of(dev, block)]; b; b = b->hash_next) {
        if (b->dev == dev && b->block == block && b->size == size)
            return b;
    }
    return NULL;
}

static struct buf *get_locked(struct block_device *dev, uint64_t block, uint32_t size)
{
    struct buf *b = lookup(dev, block, size);

    if (b) {
        b->refs++;
        lru_remove(b);
        lru_push_front(b);
        return b;
    }

    if (buf_count < BCACHE_MAX) {
        b = kzalloc(sizeof(*b));
        if (b)
            buf_count++;
    }
    if (!b) {
        b = evict();
        if (!b)
            return NULL;
    }

    if (b->size != size) {
        kfree(b->data);
        b->data = kmalloc(size);
        if (!b->data) {
            kfree(b);
            buf_count--;
            return NULL;
        }
    }

    b->dev = dev;
    b->block = block;
    b->size = size;
    b->valid = false;
    b->dirty = false;
    b->pinned = false;
    b->refs = 1;
    b->hash_next = hash[hash_of(dev, block)];
    hash[hash_of(dev, block)] = b;
    lru_push_front(b);
    return b;
}

struct buf *bget(struct block_device *dev, uint64_t block, uint32_t size)
{
    struct buf *b;

    mutex_lock(&cache_lock);
    b = get_locked(dev, block, size);
    mutex_unlock(&cache_lock);
    return b;
}

struct buf *bread(struct block_device *dev, uint64_t block, uint32_t size)
{
    struct buf *b;

    mutex_lock(&cache_lock);
    b = get_locked(dev, block, size);
    if (b && !b->valid) {
        if (block_read(dev, block * size, b->data, size) == 0) {
            b->valid = true;
        } else {
            b->refs--;
            b = NULL;
        }
    }
    mutex_unlock(&cache_lock);
    return b;
}

void brelse(struct buf *b)
{
    if (!b)
        return;
    mutex_lock(&cache_lock);
    if (b->refs)
        b->refs--;
    mutex_unlock(&cache_lock);
}

void bhold(struct buf *b)
{
    mutex_lock(&cache_lock);
    b->refs++;
    mutex_unlock(&cache_lock);
}

void bdirty(struct buf *b)
{
    b->valid = true;
    b->dirty = true;
}

int bwrite(struct buf *b)
{
    int ret;

    mutex_lock(&cache_lock);
    b->valid = true;
    ret = write_out(b);
    mutex_unlock(&cache_lock);
    return ret;
}

static int compare_bufs(const struct buf *a, const struct buf *b)
{
    if (a->dev != b->dev)
        return a->dev < b->dev ? -1 : 1;
    return a->block < b->block ? -1 : a->block > b->block;
}

// Writes dirty, unpinned buffers in block order, merging neighbours into
// larger requests. Called with cache_lock held.
static int flush_locked(struct block_device *dev)
{
    struct buf **list;
    size_t n = 0, cap = 0;
    uint8_t *staging = NULL;
    int ret = 0;

    for (struct buf *b = lru_head; b; b = b->lru_next) {
        if (b->dirty && !b->pinned && (!dev || b->dev == dev))
            cap++;
    }
    if (!cap)
        return 0;
    if (!(list = kmalloc(cap * sizeof(*list))) || !(staging = kmalloc(MERGE_BYTES))) {
        kfree(list);
        // Not enough memory to batch: write one at a time.
        for (struct buf *b = lru_head; b; b = b->lru_next) {
            if (b->dirty && !b->pinned && (!dev || b->dev == dev) && write_out(b) != 0)
                ret = -1;
        }
        return ret;
    }
    for (struct buf *b = lru_head; b; b = b->lru_next) {
        if (b->dirty && !b->pinned && (!dev || b->dev == dev))
            list[n++] = b;
    }
    // Insertion sort is fine for the few thousand buffers the cache holds.
    for (size_t i = 1; i < n; i++) {
        struct buf *x = list[i];
        size_t j = i;

        while (j && compare_bufs(list[j - 1], x) > 0) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = x;
    }
    for (size_t i = 0; i < n;) {
        size_t j = i + 1, bytes = list[i]->size;

        while (j < n && list[j]->dev == list[i]->dev && list[j]->size == list[i]->size
               && list[j]->block == list[j - 1]->block + 1 && bytes + list[j]->size <= MERGE_BYTES)
            bytes += list[j++]->size;
        if (j - i == 1) {
            if (write_out(list[i]) != 0)
                ret = -1;
        } else {
            for (size_t k = i; k < j; k++)
                memcpy(staging + (k - i) * list[i]->size, list[k]->data, list[k]->size);
            if (block_write(list[i]->dev, list[i]->block * list[i]->size, staging, bytes) != 0) {
                ret = -1;
            } else {
                for (size_t k = i; k < j; k++)
                    list[k]->dirty = false;
            }
        }
        i = j;
    }
    kfree(staging);
    kfree(list);
    return ret;
}

int bcache_sync(struct block_device *dev)
{
    int ret;

    mutex_lock(&cache_lock);
    ret = flush_locked(dev);
    mutex_unlock(&cache_lock);
    if (dev && block_flush(dev) != 0)
        ret = -1;
    return ret;
}

// Like bcache_sync without the device cache flush.
int bcache_writeback(struct block_device *dev)
{
    int ret;

    mutex_lock(&cache_lock);
    ret = flush_locked(dev);
    mutex_unlock(&cache_lock);
    return ret;
}

void bcache_invalidate(struct block_device *dev)
{
    mutex_lock(&cache_lock);
    for (struct buf *b = lru_head; b; b = b->lru_next) {
        if (b->dev == dev && !b->refs && !b->dirty)
            b->valid = false;
    }
    mutex_unlock(&cache_lock);
}
