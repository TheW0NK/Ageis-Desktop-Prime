#include "ext4.h"
#include "crc.h"
#include "mem.h"
#include "string.h"

#define JBD2_MAGIC              0xC03B3998U
#define JBD2_DESCRIPTOR         1
#define JBD2_COMMIT             2
#define JBD2_SB_V1              3
#define JBD2_SB_V2              4
#define JBD2_REVOKE             5

#define JBD2_COMPAT_CHECKSUM    0x01
#define JBD2_INCOMPAT_REVOKE    0x01
#define JBD2_INCOMPAT_64BIT     0x02
#define JBD2_INCOMPAT_ASYNC     0x04
#define JBD2_INCOMPAT_CSUM_V2   0x08
#define JBD2_INCOMPAT_CSUM_V3   0x10
#define JBD2_INCOMPAT_FAST      0x20
#define JBD2_INCOMPAT_KNOWN     0x3F

#define TAG_ESCAPE              1
#define TAG_SAME_UUID           2
#define TAG_LAST                8

#define JSB_BLOCKTYPE           0x04
#define JSB_SEQUENCE_H          0x08
#define JSB_BLOCKSIZE           0x0C
#define JSB_MAXLEN              0x10
#define JSB_FIRST               0x14
#define JSB_SEQUENCE            0x18
#define JSB_START               0x1C
#define JSB_COMPAT              0x24
#define JSB_INCOMPAT            0x28
#define JSB_UUID                0x30
#define JSB_CSUM_TYPE           0x50
#define JSB_NUM_FC              0x54
#define JSB_CHECKSUM            0xFC

struct jbd2 {
    uint32_t block_size;
    uint32_t first;
    uint32_t last;
    uint32_t sequence;
    uint32_t csum_seed;
    uint64_t *map;
    uint32_t map_len;
    uint8_t *jsb;                   // whole journal block 0
    uint8_t *scratch;
    uint8_t *scratch2;
};

struct revoke {
    uint64_t block;
    uint32_t seq;
};

static uint32_t be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void put_be32(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}

static uint32_t jincompat(struct jbd2 *j)
{
    return be32(j->jsb + JSB_BLOCKTYPE) == JBD2_SB_V2 ? be32(j->jsb + JSB_INCOMPAT) : 0;
}

static bool csum_v23(struct jbd2 *j)
{
    return jincompat(j) & (JBD2_INCOMPAT_CSUM_V2 | JBD2_INCOMPAT_CSUM_V3);
}

static size_t tag_bytes(struct jbd2 *j)
{
    uint32_t inc = jincompat(j);
    size_t sz;

    if (inc & JBD2_INCOMPAT_CSUM_V3)
        return 16;
    sz = 12;
    if (inc & JBD2_INCOMPAT_CSUM_V2)
        sz += 2;
    return (inc & JBD2_INCOMPAT_64BIT) ? sz : sz - 4;
}

static uint32_t next_block(struct jbd2 *j, uint32_t b)
{
    return ++b >= j->last ? j->first : b;
}

static int jread(struct ext4_fs *fs, struct jbd2 *j, uint32_t blk, void *buf)
{
    if (blk >= j->map_len || !j->map[blk])
        return -EIO;
    return block_read(fs->dev, j->map[blk] * j->block_size, buf, j->block_size) ? -EIO : 0;
}

static int jwrite(struct ext4_fs *fs, struct jbd2 *j, uint32_t blk, const void *buf)
{
    if (blk >= j->map_len || !j->map[blk])
        return -EIO;
    return block_write(fs->dev, j->map[blk] * j->block_size, buf, j->block_size) ? -EIO : 0;
}

static int write_jsb(struct ext4_fs *fs, struct jbd2 *j)
{
    if (csum_v23(j)) {
        put_be32(j->jsb + JSB_CHECKSUM, 0);
        put_be32(j->jsb + JSB_CHECKSUM, crc32c(0xFFFFFFFF, j->jsb, 1024));
    }
    if (jwrite(fs, j, 0, j->jsb))
        return -EIO;
    return block_flush(fs->dev) ? -EIO : 0;
}

int jbd2_load(struct ext4_fs *fs, uint32_t ino)
{
    struct vnode tv = { .mount = fs->mount, .ino = ino };
    struct jbd2 *j;
    uint32_t type, maxlen, fc;
    int ret;

    if ((ret = ext4_read_inode(fs, ino, &tv)))
        return ret;
    j = kzalloc(sizeof(*j));
    if (!j) {
        kfree(tv.data);
        return -ENOMEM;
    }
    j->block_size = fs->block_size;
    j->map_len = tv.size / fs->block_size;
    j->map = kzalloc(j->map_len * sizeof(uint64_t));
    j->jsb = kmalloc(fs->block_size);
    j->scratch = kmalloc(fs->block_size);
    j->scratch2 = kmalloc(fs->block_size);
    ret = -ENOMEM;
    if (!j->map || !j->jsb || !j->scratch || !j->scratch2)
        goto fail;

    for (uint32_t l = 0; l < j->map_len;) {
        uint64_t pblk;
        uint32_t run = 0;

        if ((ret = ext4_map(&tv, l, false, &pblk, &run)) || !pblk) {
            ret = -EIO;
            goto fail;
        }
        for (uint32_t k = 0; k < run && l < j->map_len; k++)
            j->map[l++] = pblk + k;
    }
    kfree(tv.data);
    tv.data = NULL;

    ret = -EINVAL;
    if (jread(fs, j, 0, j->jsb) || be32(j->jsb) != JBD2_MAGIC)
        goto fail;
    type = be32(j->jsb + JSB_BLOCKTYPE);
    maxlen = be32(j->jsb + JSB_MAXLEN);
    if ((type != JBD2_SB_V1 && type != JBD2_SB_V2) || be32(j->jsb + JSB_BLOCKSIZE) != fs->block_size
        || maxlen > j->map_len || (jincompat(j) & ~JBD2_INCOMPAT_KNOWN))
        goto fail;

    fc = 0;
    if (jincompat(j) & JBD2_INCOMPAT_FAST)
        fc = be32(j->jsb + JSB_NUM_FC) ? be32(j->jsb + JSB_NUM_FC) : 256;
    j->first = be32(j->jsb + JSB_FIRST);
    j->last = maxlen - fc;
    j->sequence = be32(j->jsb + JSB_SEQUENCE);
    if (j->first == 0 || j->first >= j->last)
        goto fail;
    j->csum_seed = crc32c(0xFFFFFFFF, j->jsb + JSB_UUID, 16);
    fs->journal = j;
    return 0;

fail:
    kfree(tv.data);
    kfree(j->map);
    kfree(j->jsb);
    kfree(j->scratch);
    kfree(j->scratch2);
    kfree(j);
    return ret;
}

uint32_t jbd2_max_transaction(struct ext4_fs *fs)
{
    struct jbd2 *j = fs->journal;
    uint32_t len = j->last - j->first;

    return len > 64 ? len - len / 8 - 8 : len / 2;
}

static bool block_csum_ok(struct jbd2 *j, uint8_t *block, size_t csum_off)
{
    uint32_t stored, c;

    if (!csum_v23(j))
        return true;
    stored = be32(block + csum_off);
    put_be32(block + csum_off, 0);
    c = crc32c(j->csum_seed, block, j->block_size);
    put_be32(block + csum_off, stored);
    return c == stored;
}

static uint32_t count_tags(struct jbd2 *j, const uint8_t *desc)
{
    size_t tb = tag_bytes(j), tail = csum_v23(j) ? 4 : 0;
    uint32_t n = 0;

    for (size_t off = 12; off + tb <= j->block_size - tail;) {
        uint32_t flags = (jincompat(j) & JBD2_INCOMPAT_CSUM_V3) ? be32(desc + off + 4)
                                                                 : (uint32_t)desc[off + 6] << 8 | desc[off + 7];
        n++;
        off += tb;
        if (!(flags & TAG_SAME_UUID))
            off += 16;
        if (flags & TAG_LAST)
            break;
    }
    return n;
}

static bool revoked(struct revoke *r, size_t n, uint64_t block, uint32_t seq)
{
    for (size_t i = 0; i < n; i++) {
        if (r[i].block == block && (int32_t)(r[i].seq - seq) >= 0)
            return true;
    }
    return false;
}

int jbd2_recover(struct ext4_fs *fs)
{
    struct jbd2 *j = fs->journal;
    uint32_t start = be32(j->jsb + JSB_START);
    uint32_t first_seq = be32(j->jsb + JSB_SEQUENCE), end_seq = first_seq, blk, seq;
    bool v3 = jincompat(j) & JBD2_INCOMPAT_CSUM_V3, is64 = jincompat(j) & JBD2_INCOMPAT_64BIT;
    struct revoke *revokes = NULL;
    size_t nrev = 0, caprev = 0, tb = tag_bytes(j);
    uint32_t replayed = 0;
    int pass;

    if (start == 0)
        return 0;
    if (jincompat(j) & JBD2_INCOMPAT_FAST)
        return -ENOTSUP;

    for (pass = 0; pass < 3; pass++) {
        blk = start;
        seq = first_seq;
        for (;;) {
            uint8_t *b = j->scratch;
            uint32_t type;

            if (pass > 0 && seq == end_seq)
                break;
            if (jread(fs, j, blk, b) || be32(b) != JBD2_MAGIC || be32(b + JSB_SEQUENCE_H) != seq)
                break;
            type = be32(b + 4);

            if (type == JBD2_DESCRIPTOR) {
                uint32_t n = count_tags(j, b);

                if (pass == 0 && !block_csum_ok(j, b, j->block_size - 4))
                    break;
                if (pass == 2) {
                    size_t off = 12;
                    uint32_t dblk = blk;
                    uint8_t *desc = j->scratch2;

                    memcpy(desc, b, j->block_size);
                    for (uint32_t i = 0; i < n; i++) {
                        uint64_t target = be32(desc + off);
                        uint32_t flags;
                        uint8_t *data = j->scratch;

                        if (v3) {
                            flags = be32(desc + off + 4);
                            if (is64)
                                target |= (uint64_t)be32(desc + off + 8) << 32;
                        } else {
                            flags = (uint32_t)desc[off + 6] << 8 | desc[off + 7];
                            if (is64)
                                target |= (uint64_t)be32(desc + off + 8) << 32;
                        }
                        dblk = next_block(j, dblk);
                        if (jread(fs, j, dblk, data) == 0 && !revoked(revokes, nrev, target, seq)) {
                            bool ok = true;

                            if (v3) {
                                uint32_t s = 0;
                                put_be32(&s, seq);
                                ok = crc32c(crc32c(j->csum_seed, &s, 4), data, j->block_size)
                                     == be32(desc + off + 12);
                            }
                            if (flags & TAG_ESCAPE)
                                put_be32(data, JBD2_MAGIC);
                            if (ok && target < fs->blocks_count
                                && block_write(fs->dev, target * j->block_size, data, j->block_size) == 0)
                                replayed++;
                        }
                        off += tb;
                        if (!(flags & TAG_SAME_UUID))
                            off += 16;
                    }
                }
                for (uint32_t i = 0; i <= n; i++)
                    blk = next_block(j, blk);
            } else if (type == JBD2_COMMIT) {
                if (pass == 0 && !block_csum_ok(j, b, 16))
                    break;
                seq++;
                blk = next_block(j, blk);
            } else if (type == JBD2_REVOKE) {
                if (pass == 1) {
                    uint32_t used = be32(b + 12);
                    size_t rs = is64 ? 8 : 4;

                    for (size_t off = 16; off + rs <= used && off + rs <= j->block_size; off += rs) {
                        if (nrev == caprev) {
                            size_t cap = caprev ? caprev * 2 : 64;
                            struct revoke *n = kmalloc(cap * sizeof(*n));
                            if (!n)
                                break;
                            memcpy(n, revokes, nrev * sizeof(*n));
                            kfree(revokes);
                            revokes = n;
                            caprev = cap;
                        }
                        revokes[nrev].block = is64 ? (uint64_t)be32(b + off) << 32 | be32(b + off + 4)
                                                   : be32(b + off);
                        revokes[nrev++].seq = seq;
                    }
                }
                blk = next_block(j, blk);
            } else {
                break;
            }
        }
        if (pass == 0)
            end_seq = seq;
    }
    kfree(revokes);

    if (block_flush(fs->dev))
        return -EIO;
    put_be32(j->jsb + JSB_START, 0);
    put_be32(j->jsb + JSB_SEQUENCE, end_seq);
    j->sequence = end_seq;
    if (write_jsb(fs, j))
        return -EIO;
    if (end_seq != first_seq)
        kprintf("ext4: replayed %u journal block(s) from %u transaction(s)\n", replayed, end_seq - first_seq);
    return 1;
}

static void enable_features(struct ext4_fs *fs, struct jbd2 *j)
{
    uint32_t inc;

    if (be32(j->jsb + JSB_BLOCKTYPE) != JBD2_SB_V2)
        return;
    inc = be32(j->jsb + JSB_INCOMPAT);
    if (fs->metadata_csum && !(inc & (JBD2_INCOMPAT_CSUM_V2 | JBD2_INCOMPAT_CSUM_V3))) {
        inc |= JBD2_INCOMPAT_CSUM_V3;
        put_be32(j->jsb + JSB_COMPAT, be32(j->jsb + JSB_COMPAT) & ~JBD2_COMPAT_CHECKSUM);
        j->jsb[JSB_CSUM_TYPE] = 4;
    }
    if (fs->is64)
        inc |= JBD2_INCOMPAT_64BIT;
    put_be32(j->jsb + JSB_INCOMPAT, inc);
}

static int commit_chunk(struct ext4_fs *fs, struct jbd2 *j, struct buf **bufs, size_t count)
{
    bool v3 = jincompat(j) & JBD2_INCOMPAT_CSUM_V3, csum = csum_v23(j);
    bool is64 = jincompat(j) & JBD2_INCOMPAT_64BIT;
    size_t tb = tag_bytes(j), tail = csum ? 4 : 0;
    uint32_t tid = j->sequence, pos = j->first;
    uint8_t *desc = j->scratch, *data = j->scratch2;
    size_t i = 0;

    put_be32(j->jsb + JSB_START, j->first);
    put_be32(j->jsb + JSB_SEQUENCE, tid);
    if (write_jsb(fs, j))
        return -EIO;

    while (i < count) {
        uint32_t desc_pos = pos;
        size_t off = 12, first = i;

        memset(desc, 0, j->block_size);
        put_be32(desc, JBD2_MAGIC);
        put_be32(desc + 4, JBD2_DESCRIPTOR);
        put_be32(desc + 8, tid);
        pos++;

        while (i < count) {
            size_t need = tb + (i == first ? 16 : 0);
            uint32_t flags = i == first ? 0 : TAG_SAME_UUID;
            uint8_t *tag = desc + off;

            if (off + need > j->block_size - tail)
                break;
            memcpy(data, bufs[i]->data, j->block_size);
            if (be32(data) == JBD2_MAGIC) {
                put_be32(data, 0);
                flags |= TAG_ESCAPE;
            }
            if (i + 1 == count || off + need + tb > j->block_size - tail)
                flags |= TAG_LAST;

            put_be32(tag, bufs[i]->block);
            if (v3) {
                uint32_t s = 0;
                put_be32(tag + 4, flags);
                put_be32(tag + 8, bufs[i]->block >> 32);
                put_be32(&s, tid);
                put_be32(tag + 12, crc32c(crc32c(j->csum_seed, &s, 4), data, j->block_size));
            } else {
                tag[6] = flags >> 8;
                tag[7] = flags;
                if (is64)
                    put_be32(tag + 8, bufs[i]->block >> 32);
            }
            if (i == first)
                memcpy(tag + tb, j->jsb + JSB_UUID, 16);
            off += need;

            if (jwrite(fs, j, pos++, data))
                return -EIO;
            i++;
            if (flags & TAG_LAST)
                break;
        }
        if (csum)
            put_be32(desc + j->block_size - 4, crc32c(j->csum_seed, desc, j->block_size));
        if (jwrite(fs, j, desc_pos, desc))
            return -EIO;
    }
    if (block_flush(fs->dev))
        return -EIO;

    memset(desc, 0, j->block_size);
    put_be32(desc, JBD2_MAGIC);
    put_be32(desc + 4, JBD2_COMMIT);
    put_be32(desc + 8, tid);
    put_be32(desc + 48, 0);
    put_be32(desc + 52, (uint32_t)ext4_now());
    if (csum)
        put_be32(desc + 16, crc32c(j->csum_seed, desc, j->block_size));
    if (jwrite(fs, j, pos, desc) || block_flush(fs->dev))
        return -EIO;

    for (i = 0; i < count; i++) {
        if (bwrite(bufs[i]))
            return -EIO;
    }
    if (block_flush(fs->dev))
        return -EIO;

    j->sequence = tid + 1;
    put_be32(j->jsb + JSB_START, 0);
    put_be32(j->jsb + JSB_SEQUENCE, j->sequence);
    return write_jsb(fs, j);
}

int jbd2_commit(struct ext4_fs *fs, struct buf **bufs, size_t count)
{
    struct jbd2 *j = fs->journal;
    size_t per_desc, chunk;
    int ret = 0;

    enable_features(fs, j);
    per_desc = (j->block_size - 12 - 16 - (csum_v23(j) ? 4 : 0)) / tag_bytes(j);
    chunk = (j->last - j->first) - (j->last - j->first) / per_desc - 4;

    for (size_t done = 0; done < count && ret == 0; done += chunk)
        ret = commit_chunk(fs, j, bufs + done, MIN(chunk, count - done));
    if (ret)
        kprintf("ext4: journal commit failed\n");
    return ret;
}
