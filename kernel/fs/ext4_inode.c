#include "ext4.h"
#include "crc.h"
#include "mem.h"
#include "string.h"

#define MAX_DEPTH       5
#define HUGE_FILE_FL    0x00040000
#define ENTRY(h, i)     ((uint8_t *)((h) + 1) + (size_t)(i) * 12)
#define KEY(h, i)       (*(uint32_t *)ENTRY(h, i))

struct ext_path {
    int depth;
    struct {
        struct buf *b;
        struct ext4_extent_header *h;
        int idx;
    } p[MAX_DEPTH + 1];
};

static struct ext4_fs *fs_of(struct vnode *v)
{
    return EXT4_FS(v->mount);
}

uint64_t ext4_isize(struct ext4_inode *in)
{
    return (uint64_t)in->size_lo | (uint64_t)in->size_high << 32;
}

void ext4_set_isize(struct ext4_inode *in, uint64_t size)
{
    in->size_lo = size;
    in->size_high = size >> 32;
}

static uint64_t iblocks(struct ext4_fs *fs, struct ext4_inode *in)
{
    uint64_t n = in->blocks_lo;

    if (fs->sb->feature_ro_compat & EXT4_RO_COMPAT_HUGE_FILE)
        n |= (uint64_t)in->blocks_high << 32;
    if (in->flags & HUGE_FILE_FL)
        n *= fs->block_size / 512;
    return n;
}

static void add_iblocks(struct ext4_fs *fs, struct ext4_inode *in, int64_t fs_blocks)
{
    uint64_t n = iblocks(fs, in) + fs_blocks * (int64_t)(fs->block_size / 512);

    in->flags &= ~HUGE_FILE_FL;
    in->blocks_lo = n;
    in->blocks_high = n >> 32;
}

void ext4_sync_vnode(struct vnode *v)
{
    struct ext4_inode *in = EXT4_RAW(v);

    v->mode = in->mode;
    v->uid = in->uid | (uint32_t)in->uid_high << 16;
    v->gid = in->gid | (uint32_t)in->gid_high << 16;
    v->nlink = in->links_count;
    v->size = ext4_isize(in);
    v->blocks = iblocks(fs_of(v), in);
    v->atime = in->atime;
    v->mtime = in->mtime;
    v->ctime = in->ctime;
}

static uint32_t inode_csum(struct ext4_fs *fs, uint32_t seed, struct ext4_inode *in)
{
    uint16_t lo = in->checksum_lo, hi = in->checksum_hi;
    bool has_hi = fs->inode_size > 128 && in->extra_isize >= 4;
    uint32_t c;

    in->checksum_lo = 0;
    if (has_hi)
        in->checksum_hi = 0;
    c = crc32c(seed, in, fs->inode_size);
    in->checksum_lo = lo;
    in->checksum_hi = hi;
    return c;
}

int ext4_read_inode(struct ext4_fs *fs, uint32_t ino, struct vnode *v)
{
    uint32_t g, idx;
    uint64_t byte;
    struct buf *b;
    struct ext4_inode_info *info;
    struct ext4_inode *in;

    if (ino < 1 || ino > fs->inodes_count)
        return -EINVAL;
    g = (ino - 1) / fs->inodes_per_group;
    idx = (ino - 1) % fs->inodes_per_group;
    byte = (uint64_t)idx * fs->inode_size;

    if (!(b = bread(fs->dev, ext4_inode_table(fs, g) + byte / fs->block_size, fs->block_size)))
        return -EIO;
    info = kmalloc(sizeof(*info) + fs->inode_size);
    if (!info) {
        brelse(b);
        return -ENOMEM;
    }
    memcpy(info->raw, b->data + byte % fs->block_size, fs->inode_size);
    brelse(b);

    in = (struct ext4_inode *)info->raw;
    info->group = g;
    info->csum_seed = crc32c(crc32c(fs->csum_seed, &ino, 4), &in->generation, 4);
    if (fs->metadata_csum) {
        uint32_t c = inode_csum(fs, info->csum_seed, in);
        uint32_t stored = in->checksum_lo | ((fs->inode_size > 128 && in->extra_isize >= 4)
                                             ? (uint32_t)in->checksum_hi << 16 : (c & 0xFFFF0000));
        if (stored != c && in->mode)
            kprintf("ext4: inode %u checksum mismatch\n", ino);
    }

    v->data = info;
    ext4_sync_vnode(v);
    return 0;
}

int ext4_write_inode(struct vnode *v)
{
    struct ext4_fs *fs = fs_of(v);
    struct ext4_inode_info *info = EXT4_I(v);
    struct ext4_inode *in = EXT4_RAW(v);
    uint32_t idx = (v->ino - 1) % fs->inodes_per_group;
    uint64_t byte = (uint64_t)idx * fs->inode_size;
    struct buf *b;

    if (fs->metadata_csum) {
        uint32_t c = inode_csum(fs, info->csum_seed, in);
        in->checksum_lo = c;
        if (fs->inode_size > 128 && in->extra_isize >= 4)
            in->checksum_hi = c >> 16;
    }
    if (!(b = bread(fs->dev, ext4_inode_table(fs, info->group) + byte / fs->block_size, fs->block_size)))
        return -EIO;
    memcpy(b->data + byte % fs->block_size, in, fs->inode_size);
    ext4_dirty(fs, b);
    brelse(b);
    ext4_sync_vnode(v);
    return 0;
}

static uint32_t block_max(struct ext4_fs *fs)
{
    return (fs->block_size - sizeof(struct ext4_extent_header)) / 12;
}

void ext4_init_extents(struct ext4_inode *in)
{
    struct ext4_extent_header *h = (struct ext4_extent_header *)in->block;

    memset(in->block, 0, sizeof(in->block));
    h->magic = EXT4_EXT_MAGIC;
    h->max = 4;
    in->flags |= EXT4_EXTENTS_FL;
}

static uint64_t ext_pblk(const struct ext4_extent *e)
{
    return e->start_lo | (uint64_t)e->start_hi << 32;
}

static void ext_set_pblk(struct ext4_extent *e, uint64_t p)
{
    e->start_lo = p;
    e->start_hi = p >> 32;
}

static uint32_t ext_len(const struct ext4_extent *e)
{
    return e->len > EXT4_EXT_INIT_MAX_LEN ? e->len - EXT4_EXT_INIT_MAX_LEN : e->len;
}

static bool ext_uninit(const struct ext4_extent *e)
{
    return e->len > EXT4_EXT_INIT_MAX_LEN;
}

static uint64_t idx_pblk(const struct ext4_extent_idx *ix)
{
    return ix->leaf_lo | (uint64_t)ix->leaf_hi << 32;
}

void ext4_csum_extent_block(struct vnode *v, struct buf *b)
{
    struct ext4_extent_header *h = (struct ext4_extent_header *)b->data;
    size_t off = sizeof(*h) + (size_t)h->max * 12;

    if (fs_of(v)->metadata_csum && off + 4 <= b->size)
        *(uint32_t *)(b->data + off) = crc32c(EXT4_I(v)->csum_seed, b->data, off);
}

static void node_dirty(struct vnode *v, struct ext_path *path, int level)
{
    if (level == 0 || !path->p[level].b) {
        ext4_write_inode(v);
        return;
    }
    ext4_csum_extent_block(v, path->p[level].b);
    ext4_dirty(fs_of(v), path->p[level].b);
}

static void release_path(struct ext_path *path)
{
    for (int i = 0; i <= path->depth; i++)
        brelse(path->p[i].b);
}

static int find_path(struct vnode *v, uint32_t lblk, struct ext_path *path)
{
    struct ext4_fs *fs = fs_of(v);
    struct ext4_extent_header *h = (struct ext4_extent_header *)EXT4_RAW(v)->block;
    int level = 0;

    memset(path, 0, sizeof(*path));
    for (;;) {
        int idx = -1;

        if (h->magic != EXT4_EXT_MAGIC || h->entries > h->max || level > MAX_DEPTH) {
            path->depth = level > MAX_DEPTH ? MAX_DEPTH : level;
            release_path(path);
            return -EIO;
        }
        for (int i = 0; i < h->entries && KEY(h, i) <= lblk; i++)
            idx = i;
        path->p[level].h = h;

        if (h->depth == 0) {
            path->p[level].idx = idx;
            path->depth = level;
            return 0;
        }
        if (idx < 0)
            idx = 0;
        path->p[level].idx = idx;
        if (h->entries == 0) {
            path->depth = level;
            release_path(path);
            return -EIO;
        }

        struct buf *b = bread(fs->dev, idx_pblk((struct ext4_extent_idx *)ENTRY(h, idx)), fs->block_size);
        if (!b) {
            path->depth = level;
            release_path(path);
            return -EIO;
        }
        level++;
        path->p[level].b = b;
        h = (struct ext4_extent_header *)b->data;
    }
}

static void update_keys(struct vnode *v, struct ext_path *path, int level)
{
    for (int l = level; l > 0; l--) {
        struct ext4_extent_header *parent = path->p[l - 1].h;
        int idx = path->p[l - 1].idx;

        KEY(parent, idx) = KEY(path->p[l].h, 0);
        node_dirty(v, path, l - 1);
        if (idx != 0)
            break;
    }
}

static int alloc_tree_block(struct vnode *v, struct buf **out, uint64_t *blk)
{
    struct ext4_fs *fs = fs_of(v);
    uint64_t got;
    int ret;

    if ((ret = ext4_alloc_blocks(fs, 0, 1, blk, &got)))
        return ret;
    if (!(*out = bget(fs->dev, *blk, fs->block_size))) {
        ext4_free_blocks(fs, *blk, 1);
        return -EIO;
    }
    memset((*out)->data, 0, fs->block_size);
    add_iblocks(fs, EXT4_RAW(v), 1);
    return 0;
}

static int grow_root(struct vnode *v)
{
    struct ext4_fs *fs = fs_of(v);
    struct ext4_extent_header *root = (struct ext4_extent_header *)EXT4_RAW(v)->block;
    struct ext4_extent_header *h;
    struct ext4_extent_idx *ix;
    struct buf *b;
    uint64_t blk;
    int ret;

    if (root->depth >= MAX_DEPTH)
        return -EFBIG;
    if ((ret = alloc_tree_block(v, &b, &blk)))
        return ret;

    h = (struct ext4_extent_header *)b->data;
    memcpy(h, root, sizeof(*root) + (size_t)root->entries * 12);
    h->max = block_max(fs);
    ext4_csum_extent_block(v, b);
    ext4_dirty(fs, b);
    brelse(b);

    ix = (struct ext4_extent_idx *)ENTRY(root, 0);
    memset(ix, 0, 12 * 4);
    ix->block = h->entries ? KEY(h, 0) : 0;
    ix->leaf_lo = blk;
    ix->leaf_hi = blk >> 32;
    root->depth++;
    root->entries = 1;
    return ext4_write_inode(v);
}

static int make_room(struct vnode *v, struct ext_path *path, int level, uint32_t key)
{
    struct ext4_fs *fs = fs_of(v);
    struct ext4_extent_header *h = path->p[level].h, *parent, *h2;
    struct ext4_extent_idx *ix;
    struct buf *b;
    uint64_t blk;
    int mid, pos, ret;

    if (h->entries < h->max)
        return 0;
    if (level == 0)
        return grow_root(v);
    parent = path->p[level - 1].h;
    if (parent->entries >= parent->max)
        return make_room(v, path, level - 1, key);

    if ((ret = alloc_tree_block(v, &b, &blk)))
        return ret;
    h2 = (struct ext4_extent_header *)b->data;
    // Appending past the last key starts a new leaf instead of halving, so
    // files that grow sequentially get full leaves.
    mid = (h->depth == 0 && key > KEY(h, h->entries - 1)) ? h->entries : h->entries / 2;
    h2->magic = EXT4_EXT_MAGIC;
    h2->depth = h->depth;
    h2->max = block_max(fs);
    h2->entries = h->entries - mid;
    memcpy(ENTRY(h2, 0), ENTRY(h, mid), (size_t)h2->entries * 12);
    h->entries = mid;
    ext4_csum_extent_block(v, b);
    ext4_dirty(fs, b);
    node_dirty(v, path, level);

    pos = path->p[level - 1].idx + 1;
    memmove(ENTRY(parent, pos + 1), ENTRY(parent, pos), (size_t)(parent->entries - pos) * 12);
    ix = (struct ext4_extent_idx *)ENTRY(parent, pos);
    memset(ix, 0, sizeof(*ix));
    ix->block = h2->entries ? KEY(h2, 0) : key;
    ix->leaf_lo = blk;
    ix->leaf_hi = blk >> 32;
    parent->entries++;
    brelse(b);
    node_dirty(v, path, level - 1);
    return ext4_write_inode(v);
}

static int ext_insert(struct vnode *v, uint32_t lblk, uint64_t pblk, uint32_t len)
{
    for (int attempt = 0; attempt < 4 * MAX_DEPTH; attempt++) {
        struct ext_path path;
        struct ext4_extent_header *h;
        struct ext4_extent *ex, *e;
        int d, idx, ret;

        if ((ret = find_path(v, lblk, &path)))
            return ret;
        d = path.depth;
        h = path.p[d].h;
        idx = path.p[d].idx;
        ex = (struct ext4_extent *)ENTRY(h, 0);

        if (idx >= 0) {
            e = &ex[idx];
            if (!ext_uninit(e) && e->block + ext_len(e) == lblk && ext_pblk(e) + ext_len(e) == pblk
                && ext_len(e) + len <= EXT4_EXT_INIT_MAX_LEN) {
                e->len += len;
                node_dirty(v, &path, d);
                release_path(&path);
                return 0;
            }
        }
        if (idx + 1 < h->entries) {
            e = &ex[idx + 1];
            if (!ext_uninit(e) && lblk + len == e->block && pblk + len == ext_pblk(e)
                && ext_len(e) + len <= EXT4_EXT_INIT_MAX_LEN) {
                e->block = lblk;
                ext_set_pblk(e, pblk);
                e->len += len;
                node_dirty(v, &path, d);
                if (idx + 1 == 0)
                    update_keys(v, &path, d);
                release_path(&path);
                return 0;
            }
        }
        if (h->entries < h->max) {
            int pos = idx + 1;

            memmove(&ex[pos + 1], &ex[pos], (size_t)(h->entries - pos) * 12);
            e = &ex[pos];
            e->block = lblk;
            e->len = len;
            ext_set_pblk(e, pblk);
            h->entries++;
            node_dirty(v, &path, d);
            if (pos == 0)
                update_keys(v, &path, d);
            release_path(&path);
            return 0;
        }

        ret = make_room(v, &path, d, lblk);
        release_path(&path);
        if (ret)
            return ret;
    }
    return -EIO;
}

static int zero_blocks(struct ext4_fs *fs, uint64_t start, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        struct buf *b = bget(fs->dev, start + i, fs->block_size);

        if (!b)
            return -EIO;
        memset(b->data, 0, fs->block_size);
        if (bwrite(b) != 0) {
            brelse(b);
            return -EIO;
        }
        brelse(b);
    }
    return 0;
}

// Maps a logical block. *pblk is 0 for holes. With `create`, allocates up to
// *run blocks for a hole and reports in *fresh whether the blocks are new.
static int map_block(struct vnode *v, uint32_t lblk, bool create, uint64_t *pblk, uint32_t *run, bool *fresh)
{
    struct ext4_fs *fs = fs_of(v);
    struct ext4_inode *in = EXT4_RAW(v);
    struct ext_path path;
    struct ext4_extent_header *h;
    struct ext4_extent *ex;
    uint64_t goal, start, got;
    uint32_t want = *run ? *run : 1, hole = 0xFFFFFFFF;
    int idx, ret;

    if (fresh)
        *fresh = false;
    if (!(in->flags & EXT4_EXTENTS_FL)) {
        if (!create && iblocks(fs, in) == 0) {
            *pblk = 0;
            *run = 0xFFFFFFFF;
            return 0;
        }
        if (create && iblocks(fs, in) == 0 && ext4_isize(in) == 0)
            ext4_init_extents(in);
        else
            return -ENOTSUP;
    }

    if ((ret = find_path(v, lblk, &path)))
        return ret;
    h = path.p[path.depth].h;
    idx = path.p[path.depth].idx;
    ex = (struct ext4_extent *)ENTRY(h, 0);

    if (idx >= 0 && lblk < ex[idx].block + ext_len(&ex[idx])) {
        struct ext4_extent *e = &ex[idx];
        uint32_t off = lblk - e->block;

        if (ext_uninit(e) && create) {
            ret = zero_blocks(fs, ext_pblk(e), ext_len(e));
            if (ret == 0) {
                e->len = ext_len(e);
                node_dirty(v, &path, path.depth);
            }
            release_path(&path);
            if (ret)
                return ret;
            return map_block(v, lblk, create, pblk, run, fresh);
        }
        *pblk = ext_uninit(e) ? 0 : ext_pblk(e) + off;
        *run = ext_len(e) - off;
        release_path(&path);
        return 0;
    }

    if (idx + 1 < h->entries)
        hole = ex[idx + 1].block - lblk;
    goal = idx >= 0 ? ext_pblk(&ex[idx]) + (lblk - ex[idx].block)
                    : fs->first_data_block + (uint64_t)EXT4_I(v)->group * fs->blocks_per_group;
    release_path(&path);

    if (!create) {
        *pblk = 0;
        *run = hole;
        return 0;
    }

    want = MIN(want, hole);
    want = MIN(want, EXT4_EXT_INIT_MAX_LEN);
    if ((ret = ext4_alloc_blocks(fs, goal, want, &start, &got)))
        return ret;
    if ((ret = ext_insert(v, lblk, start, got))) {
        ext4_free_blocks(fs, start, got);
        return ret;
    }
    add_iblocks(fs, in, got);
    ext4_write_inode(v);
    *pblk = start;
    *run = got;
    if (fresh)
        *fresh = true;
    return 0;
}

int ext4_map(struct vnode *v, uint32_t lblk, bool create, uint64_t *pblk, uint32_t *run)
{
    return map_block(v, lblk, create, pblk, run, NULL);
}

static bool fast_symlink(struct vnode *v)
{
    struct ext4_inode *in = EXT4_RAW(v);

    return S_ISLNK(in->mode) && !(in->flags & EXT4_EXTENTS_FL) && ext4_isize(in) < sizeof(in->block);
}

int64_t ext4_file_read(struct vnode *v, void *buf, size_t size, uint64_t off)
{
    struct ext4_fs *fs = fs_of(v);
    size_t done = 0;

    if (fast_symlink(v)) {
        memcpy(buf, (uint8_t *)EXT4_RAW(v)->block + off, size);
        return size;
    }

    while (done < size) {
        uint64_t pos = off + done;
        uint32_t lblk = pos / fs->block_size, boff = pos % fs->block_size, run = 0;
        size_t n = MIN((size_t)(fs->block_size - boff), size - done);
        uint64_t pblk;
        int ret;

        if ((ret = ext4_map(v, lblk, false, &pblk, &run)))
            return done ? (int64_t)done : ret;
        if (!pblk) {
            memset((uint8_t *)buf + done, 0, n);
        } else {
            struct buf *b = bread(fs->dev, pblk, fs->block_size);

            if (!b)
                return done ? (int64_t)done : -EIO;
            memcpy((uint8_t *)buf + done, b->data + boff, n);
            brelse(b);
        }
        done += n;
    }
    return done;
}

int64_t ext4_file_write(struct vnode *v, const void *buf, size_t size, uint64_t off)
{
    struct ext4_fs *fs = fs_of(v);
    struct ext4_inode *in = EXT4_RAW(v);
    size_t done = 0;
    int ret = 0;

    if (off + size < off || off + size > (16ULL << 40))
        return -EFBIG;

    while (done < size) {
        uint64_t pos = off + done;
        uint32_t lblk = pos / fs->block_size, boff = pos % fs->block_size;
        size_t n = MIN((size_t)(fs->block_size - boff), size - done);
        uint32_t run = (boff + size - done + fs->block_size - 1) / fs->block_size;
        uint64_t pblk;
        bool fresh;
        struct buf *b;

        if ((ret = map_block(v, lblk, true, &pblk, &run, &fresh)))
            break;
        if (n == fs->block_size || fresh) {
            b = bget(fs->dev, pblk, fs->block_size);
            if (b && n != fs->block_size)
                memset(b->data, 0, fs->block_size);
        } else {
            b = bread(fs->dev, pblk, fs->block_size);
        }
        if (!b) {
            ret = -EIO;
            break;
        }
        memcpy(b->data + boff, (const uint8_t *)buf + done, n);
        ret = bwrite(b);
        brelse(b);
        if (ret) {
            ret = -EIO;
            break;
        }
        done += n;
    }

    if (done) {
        if (off + done > ext4_isize(in))
            ext4_set_isize(in, off + done);
        in->mtime = in->ctime = ext4_now();
        ext4_write_inode(v);
    }
    return done ? (int64_t)done : ret;
}

static uint64_t free_subtree(struct vnode *v, uint64_t blk, int depth)
{
    struct ext4_fs *fs = fs_of(v);
    struct buf *b = bread(fs->dev, blk, fs->block_size);
    uint64_t freed = 1;

    if (b) {
        struct ext4_extent_header *h = (struct ext4_extent_header *)b->data;

        for (int i = 0; h->magic == EXT4_EXT_MAGIC && i < h->entries; i++) {
            if (depth == 0) {
                struct ext4_extent *e = (struct ext4_extent *)ENTRY(h, i);
                ext4_free_blocks(fs, ext_pblk(e), ext_len(e));
                freed += ext_len(e);
            } else {
                freed += free_subtree(v, idx_pblk((struct ext4_extent_idx *)ENTRY(h, i)), depth - 1);
            }
        }
        brelse(b);
    }
    ext4_free_blocks(fs, blk, 1);
    return freed;
}

static uint64_t truncate_node(struct vnode *v, struct ext4_extent_header *h, uint32_t from)
{
    struct ext4_fs *fs = fs_of(v);
    uint64_t freed = 0;

    if (h->depth == 0) {
        while (h->entries) {
            struct ext4_extent *e = (struct ext4_extent *)ENTRY(h, h->entries - 1);
            uint32_t len = ext_len(e);

            if (e->block >= from) {
                ext4_free_blocks(fs, ext_pblk(e), len);
                freed += len;
                h->entries--;
            } else {
                if (e->block + len > from) {
                    uint32_t keep = from - e->block;
                    ext4_free_blocks(fs, ext_pblk(e) + keep, len - keep);
                    freed += len - keep;
                    e->len = ext_uninit(e) ? keep + EXT4_EXT_INIT_MAX_LEN : keep;
                }
                break;
            }
        }
        return freed;
    }

    while (h->entries) {
        int i = h->entries - 1;
        struct ext4_extent_idx *ix = (struct ext4_extent_idx *)ENTRY(h, i);
        uint64_t child = idx_pblk(ix);

        if (i > 0 && ix->block >= from) {
            freed += free_subtree(v, child, h->depth - 1);
            h->entries--;
            continue;
        }

        struct buf *b = bread(fs->dev, child, fs->block_size);
        if (!b)
            break;
        struct ext4_extent_header *ch = (struct ext4_extent_header *)b->data;
        freed += truncate_node(v, ch, from);
        if (ch->entries == 0) {
            brelse(b);
            ext4_free_blocks(fs, child, 1);
            freed++;
            h->entries--;
            continue;
        }
        ext4_csum_extent_block(v, b);
        ext4_dirty(fs, b);
        brelse(b);
        break;
    }
    return freed;
}

int ext4_file_truncate(struct vnode *v, uint64_t size)
{
    struct ext4_fs *fs = fs_of(v);
    struct ext4_inode *in = EXT4_RAW(v);
    uint64_t old = ext4_isize(in);

    if (size < old && (in->flags & EXT4_EXTENTS_FL)) {
        struct ext4_extent_header *root = (struct ext4_extent_header *)in->block;
        uint32_t from = (size + fs->block_size - 1) / fs->block_size;
        uint64_t freed = truncate_node(v, root, from);

        if (root->entries == 0) {
            root->depth = 0;
            root->max = 4;
        }
        add_iblocks(fs, in, -(int64_t)freed);

        if (size % fs->block_size) {
            uint64_t pblk;
            uint32_t run = 0;

            if (ext4_map(v, size / fs->block_size, false, &pblk, &run) == 0 && pblk) {
                struct buf *b = bread(fs->dev, pblk, fs->block_size);
                if (b) {
                    memset(b->data + size % fs->block_size, 0, fs->block_size - size % fs->block_size);
                    bwrite(b);
                    brelse(b);
                }
            }
        }
    } else if (size < old && fast_symlink(v)) {
        memset((uint8_t *)in->block + size, 0, sizeof(in->block) - size);
    }

    ext4_set_isize(in, size);
    in->mtime = in->ctime = ext4_now();
    return ext4_write_inode(v);
}
