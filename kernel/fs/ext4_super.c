#include "ext4.h"
#include "crc.h"
#include "mem.h"
#include "rtc.h"
#include "sched.h"
#include "string.h"

#define COMMIT_INTERVAL_MS  5000

static struct ext4_fs *mounted[8];
static bool commit_thread_started;

int64_t ext4_now(void)
{
    return rtc_now();
}

static bool has_super(struct ext4_fs *fs, uint32_t g)
{
    if (!(fs->sb->feature_ro_compat & EXT4_RO_COMPAT_SPARSE_SUPER) || g <= 1)
        return true;
    for (uint32_t base = 3; base <= 7; base += 2) {
        uint64_t p = base;
        while (p < g)
            p *= base;
        if (p == g)
            return true;
    }
    return false;
}

static uint64_t group_first_block(struct ext4_fs *fs, uint32_t g)
{
    return fs->first_data_block + (uint64_t)g * fs->blocks_per_group;
}

static uint32_t group_blocks(struct ext4_fs *fs, uint32_t g)
{
    if (g == fs->group_count - 1)
        return fs->blocks_count - group_first_block(fs, g);
    return fs->blocks_per_group;
}

static void sb_csum(struct ext4_fs *fs)
{
    if (fs->metadata_csum)
        fs->sb->checksum = crc32c(0xFFFFFFFF, fs->sb, offsetof(struct ext4_super, checksum));
}

void ext4_dirty(struct ext4_fs *fs, struct buf *b)
{
    b->valid = true;
    if (!fs->journal) {
        bdirty(b);
        return;
    }
    b->dirty = true;
    if (b->pinned)
        return;
    if (fs->txn_count == fs->txn_capacity) {
        size_t cap = fs->txn_capacity ? fs->txn_capacity * 2 : 64;
        struct buf **n = kmalloc(cap * sizeof(*n));

        if (!n) {
            ext4_commit(fs);
            return ext4_dirty(fs, b);
        }
        memcpy(n, fs->txn, fs->txn_count * sizeof(*n));
        kfree(fs->txn);
        fs->txn = n;
        fs->txn_capacity = cap;
    }
    b->pinned = true;
    bhold(b);
    fs->txn[fs->txn_count++] = b;
}

void ext4_sb_dirty(struct ext4_fs *fs)
{
    sb_csum(fs);
    ext4_dirty(fs, fs->sb_buf);
}

struct ext4_group_desc *ext4_gd(struct ext4_fs *fs, uint32_t g)
{
    uint32_t per_block = fs->block_size / fs->desc_size;

    return (struct ext4_group_desc *)(fs->gdt[g / per_block]->data + (g % per_block) * fs->desc_size);
}

static uint16_t gd_csum(struct ext4_fs *fs, uint32_t g, struct ext4_group_desc *gd)
{
    uint16_t saved = gd->checksum;
    uint32_t le = g;
    uint16_t crc;

    gd->checksum = 0;
    if (fs->metadata_csum) {
        uint32_t c = crc32c(fs->csum_seed, &le, 4);
        crc = crc32c(c, gd, fs->desc_size) & 0xFFFF;
    } else {
        size_t off = offsetof(struct ext4_group_desc, checksum);
        crc = crc16(0xFFFF, fs->sb->uuid, 16);
        crc = crc16(crc, &le, 4);
        crc = crc16(crc, gd, off);
        if (fs->desc_size > off + 2)
            crc = crc16(crc, (uint8_t *)gd + off + 2, fs->desc_size - off - 2);
    }
    gd->checksum = saved;
    return crc;
}

void ext4_gd_dirty(struct ext4_fs *fs, uint32_t g)
{
    struct ext4_group_desc *gd = ext4_gd(fs, g);
    uint32_t per_block = fs->block_size / fs->desc_size;

    if (fs->metadata_csum || fs->gdt_csum)
        gd->checksum = gd_csum(fs, g, gd);
    ext4_dirty(fs, fs->gdt[g / per_block]);
}

#define GD_GET(fs, gd, f) ((uint64_t)(gd)->f##_lo | ((fs)->desc_size >= 64 ? (uint64_t)(gd)->f##_hi << 32 : 0))
#define GD_GET16(fs, gd, f) ((uint32_t)(gd)->f##_lo | ((fs)->desc_size >= 64 ? (uint32_t)(gd)->f##_hi << 16 : 0))
#define GD_SET16(fs, gd, f, v) do {                         \
        (gd)->f##_lo = (v) & 0xFFFF;                        \
        if ((fs)->desc_size >= 64) (gd)->f##_hi = (v) >> 16; \
    } while (0)

static uint64_t gd_block_bitmap(struct ext4_fs *fs, struct ext4_group_desc *gd)
{
    return GD_GET(fs, gd, block_bitmap);
}

static uint64_t gd_inode_bitmap(struct ext4_fs *fs, struct ext4_group_desc *gd)
{
    return GD_GET(fs, gd, inode_bitmap);
}

uint64_t ext4_inode_table(struct ext4_fs *fs, uint32_t g)
{
    return GD_GET(fs, ext4_gd(fs, g), inode_table);
}

static uint64_t sb_free_blocks(struct ext4_fs *fs)
{
    return (uint64_t)fs->sb->free_blocks_count_lo | (fs->is64 ? (uint64_t)fs->sb->free_blocks_count_hi << 32 : 0);
}

static void sb_set_free_blocks(struct ext4_fs *fs, uint64_t v)
{
    fs->sb->free_blocks_count_lo = v;
    if (fs->is64)
        fs->sb->free_blocks_count_hi = v >> 32;
}

static void bitmap_csum(struct ext4_fs *fs, uint32_t g, struct buf *b, bool inode)
{
    struct ext4_group_desc *gd = ext4_gd(fs, g);
    uint32_t c;

    if (!fs->metadata_csum)
        return;
    if (inode) {
        c = crc32c(fs->csum_seed, b->data, fs->inodes_per_group / 8);
        gd->inode_bitmap_csum_lo = c;
        if (fs->desc_size >= 64)
            gd->inode_bitmap_csum_hi = c >> 16;
    } else {
        c = crc32c(fs->csum_seed, b->data, fs->blocks_per_group / 8);
        gd->block_bitmap_csum_lo = c;
        if (fs->desc_size >= 64)
            gd->block_bitmap_csum_hi = c >> 16;
    }
}

static inline bool test_bit(const uint8_t *map, uint64_t bit)
{
    return map[bit / 8] & (1 << (bit % 8));
}

static inline void set_bit(uint8_t *map, uint64_t bit)
{
    map[bit / 8] |= 1 << (bit % 8);
}

static inline void clear_bit(uint8_t *map, uint64_t bit)
{
    map[bit / 8] &= ~(1 << (bit % 8));
}

static void mark_range(struct ext4_fs *fs, uint8_t *map, uint64_t first, uint64_t start, uint64_t count)
{
    for (uint64_t b = start; b < start + count; b++) {
        if (b >= first && b < first + fs->blocks_per_group)
            set_bit(map, b - first);
    }
}

static struct buf *block_bitmap(struct ext4_fs *fs, uint32_t g)
{
    struct ext4_group_desc *gd = ext4_gd(fs, g);
    uint64_t blk = gd_block_bitmap(fs, gd);
    uint64_t first = group_first_block(fs, g);
    struct buf *b;

    if (!(gd->flags & EXT4_BG_BLOCK_UNINIT))
        return bread(fs->dev, blk, fs->block_size);

    if (!(b = bget(fs->dev, blk, fs->block_size)))
        return NULL;
    memset(b->data, 0, fs->block_size);
    if (has_super(fs, g))
        mark_range(fs, b->data, first, first, 1 + fs->gdt_blocks + fs->sb->reserved_gdt_blocks);
    for (uint32_t h = 0; h < fs->group_count; h++) {
        struct ext4_group_desc *hd = ext4_gd(fs, h);
        mark_range(fs, b->data, first, gd_block_bitmap(fs, hd), 1);
        mark_range(fs, b->data, first, gd_inode_bitmap(fs, hd), 1);
        mark_range(fs, b->data, first, ext4_inode_table(fs, h), fs->itable_blocks);
    }
    for (uint64_t bit = group_blocks(fs, g); bit < (uint64_t)fs->block_size * 8; bit++)
        set_bit(b->data, bit);

    gd->flags &= ~EXT4_BG_BLOCK_UNINIT;
    bitmap_csum(fs, g, b, false);
    ext4_dirty(fs, b);
    ext4_gd_dirty(fs, g);
    return b;
}

static struct buf *inode_bitmap(struct ext4_fs *fs, uint32_t g)
{
    struct ext4_group_desc *gd = ext4_gd(fs, g);
    uint64_t blk = gd_inode_bitmap(fs, gd);
    struct buf *b;

    if (!(gd->flags & EXT4_BG_INODE_UNINIT))
        return bread(fs->dev, blk, fs->block_size);

    if (!(b = bget(fs->dev, blk, fs->block_size)))
        return NULL;
    memset(b->data, 0, fs->block_size);
    for (uint64_t bit = fs->inodes_per_group; bit < (uint64_t)fs->block_size * 8; bit++)
        set_bit(b->data, bit);
    gd->flags &= ~EXT4_BG_INODE_UNINIT;
    bitmap_csum(fs, g, b, true);
    ext4_dirty(fs, b);
    ext4_gd_dirty(fs, g);
    return b;
}

static bool pending(struct ext4_fs *fs, uint64_t block)
{
    for (size_t i = 0; i < fs->pending_count; i++) {
        if (block >= fs->pending_free[i].start
            && block < fs->pending_free[i].start + fs->pending_free[i].count)
            return true;
    }
    return false;
}

static void add_pending(struct ext4_fs *fs, uint64_t start, uint64_t count)
{
    if (!fs->journal)
        return;
    if (fs->pending_count == fs->pending_capacity) {
        size_t cap = fs->pending_capacity ? fs->pending_capacity * 2 : 32;
        struct ext4_range *n = kmalloc(cap * sizeof(*n));

        if (!n)
            return;
        memcpy(n, fs->pending_free, fs->pending_count * sizeof(*n));
        kfree(fs->pending_free);
        fs->pending_free = n;
        fs->pending_capacity = cap;
    }
    fs->pending_free[fs->pending_count++] = (struct ext4_range){ start, count };
}

static int64_t find_run(struct ext4_fs *fs, uint32_t g, const uint8_t *map, uint32_t from,
                        uint32_t nbits, uint64_t want, uint64_t *len)
{
    uint64_t first = group_first_block(fs, g);

    for (uint32_t bit = from; bit < nbits; bit++) {
        if ((bit & 7) == 0 && map[bit / 8] == 0xFF && bit + 8 <= nbits) {
            bit += 7;
            continue;
        }
        if (test_bit(map, bit) || pending(fs, first + bit))
            continue;

        uint64_t n = 1;
        while (n < want && bit + n < nbits && !test_bit(map, bit + n) && !pending(fs, first + bit + n))
            n++;
        *len = n;
        return bit;
    }
    return -1;
}

int ext4_alloc_blocks(struct ext4_fs *fs, uint64_t goal, uint64_t want, uint64_t *start, uint64_t *got)
{
    uint32_t g0;

    if (fs->readonly)
        return -EROFS;
    if (!want)
        want = 1;
    if (goal < fs->first_data_block || goal >= fs->blocks_count)
        goal = group_first_block(fs, fs->last_group);
    g0 = (goal - fs->first_data_block) / fs->blocks_per_group;

    for (uint32_t i = 0; i <= fs->group_count; i++) {
        uint32_t g = (g0 + i) % fs->group_count;
        struct ext4_group_desc *gd = ext4_gd(fs, g);
        uint32_t free = GD_GET16(fs, gd, free_blocks_count);
        uint32_t from = (i == 0) ? (goal - fs->first_data_block) % fs->blocks_per_group : 0;
        struct buf *b;
        int64_t bit;
        uint64_t len;

        if (!free)
            continue;
        if (!(b = block_bitmap(fs, g)))
            return -EIO;
        bit = find_run(fs, g, b->data, from, group_blocks(fs, g), want, &len);
        if (bit < 0 && from)
            bit = find_run(fs, g, b->data, 0, group_blocks(fs, g), want, &len);
        if (bit < 0) {
            brelse(b);
            continue;
        }

        len = MIN(len, free);
        for (uint64_t k = 0; k < len; k++)
            set_bit(b->data, bit + k);
        GD_SET16(fs, gd, free_blocks_count, free - len);
        bitmap_csum(fs, g, b, false);
        ext4_dirty(fs, b);
        ext4_gd_dirty(fs, g);
        sb_set_free_blocks(fs, sb_free_blocks(fs) - len);
        ext4_sb_dirty(fs);
        brelse(b);

        fs->last_group = g;
        *start = group_first_block(fs, g) + bit;
        *got = len;
        return 0;
    }
    return -ENOSPC;
}

void ext4_free_blocks(struct ext4_fs *fs, uint64_t start, uint64_t count)
{
    add_pending(fs, start, count);

    while (count) {
        uint32_t g = (start - fs->first_data_block) / fs->blocks_per_group;
        uint32_t bit = (start - fs->first_data_block) % fs->blocks_per_group;
        uint64_t n = MIN(count, (uint64_t)fs->blocks_per_group - bit);
        struct ext4_group_desc *gd = ext4_gd(fs, g);
        struct buf *b = block_bitmap(fs, g);
        uint32_t freed = 0;

        if (!b)
            return;
        for (uint64_t k = 0; k < n; k++) {
            if (test_bit(b->data, bit + k)) {
                clear_bit(b->data, bit + k);
                freed++;
            }
        }
        GD_SET16(fs, gd, free_blocks_count, GD_GET16(fs, gd, free_blocks_count) + freed);
        bitmap_csum(fs, g, b, false);
        ext4_dirty(fs, b);
        ext4_gd_dirty(fs, g);
        sb_set_free_blocks(fs, sb_free_blocks(fs) + freed);
        brelse(b);
        start += n;
        count -= n;
    }
    ext4_sb_dirty(fs);
}

int ext4_alloc_inode(struct ext4_fs *fs, uint32_t goal, bool dir, uint32_t *ino)
{
    if (fs->readonly)
        return -EROFS;

    for (uint32_t i = 0; i < fs->group_count; i++) {
        uint32_t g = (goal + i) % fs->group_count;
        struct ext4_group_desc *gd = ext4_gd(fs, g);
        uint32_t free = GD_GET16(fs, gd, free_inodes_count);
        uint32_t start = g == 0 ? fs->first_ino - 1 : 0;
        struct buf *b;

        if (!free)
            continue;
        if (!(b = inode_bitmap(fs, g)))
            return -EIO;

        for (uint32_t bit = start; bit < fs->inodes_per_group; bit++) {
            if (test_bit(b->data, bit))
                continue;

            uint32_t unused = GD_GET16(fs, gd, itable_unused);
            set_bit(b->data, bit);
            GD_SET16(fs, gd, free_inodes_count, free - 1);
            if (dir)
                GD_SET16(fs, gd, used_dirs_count, GD_GET16(fs, gd, used_dirs_count) + 1);
            if ((fs->metadata_csum || fs->gdt_csum) && bit >= fs->inodes_per_group - unused)
                GD_SET16(fs, gd, itable_unused, fs->inodes_per_group - bit - 1);
            bitmap_csum(fs, g, b, true);
            ext4_dirty(fs, b);
            ext4_gd_dirty(fs, g);
            fs->sb->free_inodes_count--;
            ext4_sb_dirty(fs);
            brelse(b);
            *ino = g * fs->inodes_per_group + bit + 1;
            return 0;
        }
        brelse(b);
    }
    return -ENOSPC;
}

void ext4_free_inode(struct ext4_fs *fs, uint32_t ino, bool dir)
{
    uint32_t g = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;
    struct ext4_group_desc *gd = ext4_gd(fs, g);
    struct buf *b = inode_bitmap(fs, g);

    if (!b)
        return;
    if (test_bit(b->data, bit)) {
        clear_bit(b->data, bit);
        GD_SET16(fs, gd, free_inodes_count, GD_GET16(fs, gd, free_inodes_count) + 1);
        if (dir)
            GD_SET16(fs, gd, used_dirs_count, GD_GET16(fs, gd, used_dirs_count) - 1);
        fs->sb->free_inodes_count++;
        bitmap_csum(fs, g, b, true);
        ext4_dirty(fs, b);
        ext4_gd_dirty(fs, g);
        ext4_sb_dirty(fs);
    }
    brelse(b);
}

int ext4_commit(struct ext4_fs *fs)
{
    int ret;

    if (!fs->journal)
        return bcache_sync(fs->dev);
    if (fs->txn_count == 0) {
        fs->pending_count = 0;
        return 0;
    }

    ret = jbd2_commit(fs, fs->txn, fs->txn_count);
    for (size_t i = 0; i < fs->txn_count; i++) {
        fs->txn[i]->pinned = false;
        if (ret == 0)
            fs->txn[i]->dirty = false;
        brelse(fs->txn[i]);
    }
    fs->txn_count = 0;
    fs->pending_count = 0;
    return ret;
}

void ext4_op_end(struct ext4_fs *fs)
{
    if (fs->journal && fs->txn_count >= jbd2_max_transaction(fs) / 2)
        ext4_commit(fs);
}

static void commit_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sched_sleep(COMMIT_INTERVAL_MS);
        for (size_t i = 0; i < ARRAY_SIZE(mounted); i++) {
            struct ext4_fs *fs = mounted[i];

            if (!fs)
                continue;
            mutex_lock(&fs->mount->lock);
            ext4_commit(fs);
            mutex_unlock(&fs->mount->lock);
        }
    }
}

static int write_sb_now(struct ext4_fs *fs)
{
    sb_csum(fs);
    if (bwrite(fs->sb_buf) != 0)
        return -EIO;
    return block_flush(fs->dev) == 0 ? 0 : -EIO;
}

static int load_metadata(struct ext4_fs *fs)
{
    uint32_t sb_block = fs->block_size == 1024 ? 1 : 0;
    uint32_t sb_off = fs->block_size == 1024 ? 0 : 1024;

    if (!(fs->sb_buf = bread(fs->dev, sb_block, fs->block_size)))
        return -EIO;
    fs->sb = (struct ext4_super *)(fs->sb_buf->data + sb_off);

    fs->gdt = kzalloc(fs->gdt_blocks * sizeof(struct buf *));
    if (!fs->gdt)
        return -ENOMEM;
    for (uint32_t i = 0; i < fs->gdt_blocks; i++) {
        if (!(fs->gdt[i] = bread(fs->dev, fs->first_data_block + 1 + i, fs->block_size)))
            return -EIO;
    }
    return 0;
}

static void drop_metadata(struct ext4_fs *fs)
{
    for (uint32_t i = 0; fs->gdt && i < fs->gdt_blocks; i++)
        brelse(fs->gdt[i]);
    kfree(fs->gdt);
    fs->gdt = NULL;
    brelse(fs->sb_buf);
    fs->sb_buf = NULL;
}

static int ext4_mount(struct block_device *dev, bool readonly, struct mount *m)
{
    size_t probe = MAX(2048, dev->sector_size);
    uint8_t *raw = kmalloc(probe);
    struct ext4_super *sb;
    struct ext4_fs *fs;
    int ret = -EINVAL;

    if (!raw)
        return -ENOMEM;
    if (block_read(dev, 0, raw, probe) != 0) {
        kfree(raw);
        return -EIO;
    }
    sb = (struct ext4_super *)(raw + 1024);
    if (sb->magic != EXT4_MAGIC || sb->rev_level < 1 || sb->log_block_size > 6) {
        kfree(raw);
        return -EINVAL;
    }
    if ((sb->feature_incompat & ~EXT4_INCOMPAT_SUPPORTED)
        || ((1024U << sb->log_block_size) < dev->sector_size)) {
        kprintf("ext4: %s uses unsupported features (incompat 0x%x)\n", dev->name,
                sb->feature_incompat & ~EXT4_INCOMPAT_SUPPORTED);
        kfree(raw);
        return -ENOTSUP;
    }

    fs = kzalloc(sizeof(*fs));
    if (!fs) {
        kfree(raw);
        return -ENOMEM;
    }
    fs->dev = dev;
    fs->mount = m;
    fs->block_size = 1024U << sb->log_block_size;
    fs->is64 = sb->feature_incompat & EXT4_INCOMPAT_64BIT;
    fs->blocks_count = sb->blocks_count_lo | (fs->is64 ? (uint64_t)sb->blocks_count_hi << 32 : 0);
    fs->inodes_count = sb->inodes_count;
    fs->inodes_per_group = sb->inodes_per_group;
    fs->blocks_per_group = sb->blocks_per_group;
    fs->first_data_block = sb->first_data_block;
    fs->inode_size = sb->inode_size;
    fs->first_ino = sb->first_ino;
    fs->desc_size = fs->is64 ? sb->desc_size : 32;
    fs->group_count = (fs->blocks_count - fs->first_data_block + fs->blocks_per_group - 1) / fs->blocks_per_group;
    fs->gdt_blocks = ((uint64_t)fs->group_count * fs->desc_size + fs->block_size - 1) / fs->block_size;
    fs->itable_blocks = ((uint64_t)fs->inodes_per_group * fs->inode_size + fs->block_size - 1) / fs->block_size;
    fs->metadata_csum = sb->feature_ro_compat & EXT4_RO_COMPAT_METADATA_CSUM;
    fs->gdt_csum = !fs->metadata_csum && (sb->feature_ro_compat & EXT4_RO_COMPAT_GDT_CSUM);
    fs->csum_seed = (sb->feature_incompat & EXT4_INCOMPAT_CSUM_SEED) ? sb->checksum_seed
                  : crc32c(0xFFFFFFFF, sb->uuid, 16);
    fs->readonly = readonly || (sb->feature_ro_compat & ~EXT4_RO_COMPAT_SUPPORTED);
    m->readonly = fs->readonly;
    kfree(raw);

    if (fs->desc_size < 32 || fs->inode_size < 128 || fs->blocks_per_group != fs->block_size * 8U
        || !fs->inodes_per_group)
        goto fail;
    if ((ret = load_metadata(fs)))
        goto fail;
    if (fs->metadata_csum
        && fs->sb->checksum != crc32c(0xFFFFFFFF, fs->sb, offsetof(struct ext4_super, checksum)))
        kprintf("ext4: %s superblock checksum mismatch\n", dev->name);

    m->ops = &ext4_ops;
    m->data = fs;

    if (fs->sb->feature_compat & EXT4_COMPAT_HAS_JOURNAL) {
        if ((ret = jbd2_load(fs, fs->sb->journal_inum)))
            goto fail;
        if ((ret = jbd2_recover(fs)) < 0)
            goto fail;
        if (ret > 0) {
            drop_metadata(fs);
            bcache_invalidate(dev);
            if ((ret = load_metadata(fs)))
                goto fail;
        }
    }

    if (!fs->readonly) {
        if (fs->journal)
            fs->sb->feature_incompat |= EXT4_INCOMPAT_RECOVER;
        fs->sb->mnt_count++;
        fs->sb->mtime = ext4_now();
        if ((ret = write_sb_now(fs)))
            goto fail;
        ext4_process_orphans(fs);
        ext4_commit(fs);

        for (size_t i = 0; i < ARRAY_SIZE(mounted); i++) {
            if (!mounted[i]) {
                mounted[i] = fs;
                break;
            }
        }
        if (!commit_thread_started && fs->journal) {
            commit_thread_started = true;
            thread_create("jbd2", commit_thread, NULL);
        }
    }

    m->root = vget(m, EXT4_ROOT_INO, &ret);
    if (!m->root)
        goto fail;
    kprintf("ext4: mounted %s (%lu MiB, %s%s)\n", dev->name,
            fs->blocks_count * fs->block_size >> 20, fs->readonly ? "read-only" : "read-write",
            fs->journal ? ", journaled" : "");
    return 0;

fail:
    drop_metadata(fs);
    kfree(fs);
    m->data = NULL;
    return ret ? ret : -EINVAL;
}

int ext4_fs_sync(struct mount *m)
{
    struct ext4_fs *fs = EXT4_FS(m);
    int ret;

    if (fs->readonly)
        return 0;
    ret = ext4_commit(fs);
    if (!fs->journal)
        ret |= bcache_sync(fs->dev);
    return block_flush(fs->dev) == 0 ? ret : -EIO;
}

int ext4_fs_unmount(struct mount *m)
{
    struct ext4_fs *fs = EXT4_FS(m);
    int ret;

    if (fs->readonly)
        return 0;
    if ((ret = ext4_fs_sync(m)))
        return ret;
    fs->sb->feature_incompat &= ~EXT4_INCOMPAT_RECOVER;
    fs->sb->state |= 1;
    fs->sb->wtime = ext4_now();
    for (size_t i = 0; i < ARRAY_SIZE(mounted); i++) {
        if (mounted[i] == fs)
            mounted[i] = NULL;
    }
    fs->readonly = true;
    return write_sb_now(fs);
}

int ext4_fs_statfs(struct mount *m, struct aegis_statfs *out)
{
    struct ext4_fs *fs = EXT4_FS(m);

    out->block_size = fs->block_size;
    out->blocks = fs->blocks_count;
    out->blocks_free = sb_free_blocks(fs);
    out->files = fs->inodes_count;
    out->files_free = fs->sb->free_inodes_count;
    return 0;
}

static struct filesystem ext4_fstype = { "ext4", ext4_mount, NULL };

void ext4_register(void)
{
    vfs_register(&ext4_fstype);
}
