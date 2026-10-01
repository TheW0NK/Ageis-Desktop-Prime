#ifndef AEGIS_BCACHE_H
#define AEGIS_BCACHE_H

#include "block.h"

struct buf {
    struct block_device *dev;
    uint64_t block;
    uint32_t size;
    uint8_t *data;
    bool valid;
    bool dirty;
    bool pinned;                    // owned by the journal; never written back by the cache
    uint32_t refs;
    struct buf *lru_prev, *lru_next;
    struct buf *hash_next;
};

struct buf *bread(struct block_device *dev, uint64_t block, uint32_t size);
struct buf *bget(struct block_device *dev, uint64_t block, uint32_t size);
void brelse(struct buf *b);
void bhold(struct buf *b);
void bdirty(struct buf *b);
int bwrite(struct buf *b);
int bcache_sync(struct block_device *dev);
void bcache_invalidate(struct block_device *dev);

#endif
