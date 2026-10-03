#include "vfs.h"
#include "bcache.h"
#include "mem.h"
#include "rtc.h"
#include "string.h"

#define ATTR_RO         0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME     0x08
#define ATTR_DIR        0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F

#define FAT_EOC         0x0FFFFFF8
#define FAT_MASK        0x0FFFFFFF
#define ROOT_INO        1
#define DIRENT_SIZE     32

struct fat_dirent {
    uint8_t name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crt_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t clus_lo;
    uint32_t size;
} __attribute__((packed));

struct fat_lfn {
    uint8_t ord;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} __attribute__((packed));

struct fat_fs {
    struct block_device *dev;
    uint32_t sector_size;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t fat_count;
    uint32_t data_start;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t next_free;
    // Where the last chain walk ended, so sequential I/O on a big file does
    // not walk its whole chain for every sector. Reset when chains shrink.
    uint32_t walk_first, walk_index, walk_cluster;
};

struct fat_node {
    uint32_t cluster;
    uint64_t dirent_sector;         // 0 for the root directory
    uint32_t dirent_offset;
    bool deleted;
};

#define FAT(m)          ((struct fat_fs *)(m)->data)
#define NODE(v)         ((struct fat_node *)(v)->data)

static uint64_t cluster_sector(struct fat_fs *fs, uint32_t c)
{
    return fs->data_start + (uint64_t)(c - 2) * fs->sectors_per_cluster;
}

static uint32_t fat_get(struct fat_fs *fs, uint32_t c)
{
    uint64_t off = (uint64_t)c * 4;
    struct buf *b = bread(fs->dev, fs->fat_start + off / fs->sector_size, fs->sector_size);
    uint32_t v;

    if (!b)
        return FAT_EOC;
    v = *(uint32_t *)(b->data + off % fs->sector_size) & FAT_MASK;
    brelse(b);
    return v;
}

static int fat_store(struct fat_fs *fs, uint32_t c, uint32_t val)
{
    uint64_t off = (uint64_t)c * 4;

    for (uint32_t i = 0; i < fs->fat_count; i++) {
        struct buf *b = bread(fs->dev, fs->fat_start + i * fs->fat_sectors + off / fs->sector_size,
                              fs->sector_size);
        uint32_t *p;

        if (!b)
            return -EIO;
        p = (uint32_t *)(b->data + off % fs->sector_size);
        *p = (*p & ~FAT_MASK) | (val & FAT_MASK);
        // Written back with the rest of the cache (sync, unmount).
        bdirty(b);
        brelse(b);
    }
    return 0;
}

// Changing an existing chain (truncating, freeing) forgets the last walk.
static int fat_set(struct fat_fs *fs, uint32_t c, uint32_t val)
{
    fs->walk_first = 0;
    return fat_store(fs, c, val);
}

static int zero_cluster(struct fat_fs *fs, uint32_t c)
{
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        struct buf *b = bget(fs->dev, cluster_sector(fs, c) + s, fs->sector_size);

        if (!b)
            return -EIO;
        memset(b->data, 0, fs->sector_size);
        bdirty(b);
        brelse(b);
    }
    return 0;
}

static int alloc_cluster(struct fat_fs *fs, uint32_t prev, uint32_t *out)
{
    for (uint32_t i = 0; i < fs->cluster_count; i++) {
        uint32_t c = 2 + (fs->next_free - 2 + i) % fs->cluster_count;

        if (fat_get(fs, c) != 0)
            continue;
        // Growing a chain keeps the last walk valid.
        if (fat_store(fs, c, FAT_MASK))
            return -EIO;
        if (prev && fat_store(fs, prev, c))
            return -EIO;
        fs->next_free = c + 1;
        *out = c;
        return 0;
    }
    return -ENOSPC;
}

static void free_chain(struct fat_fs *fs, uint32_t c)
{
    for (int guard = 0; c >= 2 && c < FAT_EOC && guard < (1 << 24); guard++) {
        uint32_t next = fat_get(fs, c);
        fat_set(fs, c, 0);
        c = next;
    }
}

static uint32_t chain_length(struct fat_fs *fs, uint32_t c)
{
    uint32_t n = 0;

    for (; c >= 2 && c < FAT_EOC && n < (1 << 24); n++)
        c = fat_get(fs, c);
    return n;
}

// Returns the cluster holding byte `off` of the chain, optionally extending it.
static int cluster_at(struct fat_fs *fs, uint32_t *first, uint64_t off, bool extend, uint32_t *out)
{
    uint32_t c = *first, prev = 0, index = off / fs->cluster_size, i = 0;
    int ret;

    if (c < 2) {
        if (!extend)
            return -ENOENT;
        if ((ret = alloc_cluster(fs, 0, &c)))
            return ret;
        zero_cluster(fs, c);
        *first = c;
    } else if (fs->walk_first == c && fs->walk_index <= index) {
        i = fs->walk_index;
        c = fs->walk_cluster;
    }
    for (; i < index; i++) {
        prev = c;
        c = fat_get(fs, c);
        if (c < 2 || c >= FAT_EOC) {
            if (!extend)
                return -ENOENT;
            if ((ret = alloc_cluster(fs, prev, &c)))
                return ret;
            zero_cluster(fs, c);
        }
    }
    fs->walk_first = *first;
    fs->walk_index = index;
    fs->walk_cluster = c;
    *out = c;
    return 0;
}

// Returns the FAT date in the high 16 bits and the time in the low 16 bits.
static uint32_t to_fat_time(int64_t t)
{
    struct rtc_time tm;

    rtc_civil(t, &tm);
    if (tm.year < 1980)
        return ((1 << 5) | 1) << 16;
    return (uint32_t)(((tm.year - 1980) << 9) | (tm.month << 5) | tm.day) << 16
         | ((tm.hour << 11) | (tm.minute << 5) | (tm.second / 2));
}

static int64_t from_fat_time(uint16_t date, uint16_t time)
{
    int64_t y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;

    if (m < 1 || m > 12 || d < 1)
        return 0;
    y -= m <= 2;
    int64_t era = y / 400, yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

static uint32_t dirent_cluster(const struct fat_dirent *de)
{
    return (uint32_t)de->clus_hi << 16 | de->clus_lo;
}

static void fill_vnode(struct mount *m, struct vnode *v, const struct fat_dirent *de)
{
    struct fat_fs *fs = FAT(m);
    struct fat_node *n = NODE(v);

    if (!de) {
        v->mode = S_IFDIR | 0755;
        v->size = (uint64_t)chain_length(fs, n->cluster) * fs->cluster_size;
        v->nlink = 2;
        return;
    }
    n->cluster = dirent_cluster(de);
    if (de->attr & ATTR_DIR) {
        v->mode = S_IFDIR | 0755;
        v->size = (uint64_t)chain_length(fs, n->cluster) * fs->cluster_size;
        v->nlink = 2;
    } else {
        v->mode = S_IFREG | ((de->attr & ATTR_RO) ? 0444 : 0644);
        v->size = de->size;
        v->nlink = 1;
    }
    v->blocks = ALIGN_UP(v->size, fs->cluster_size) / 512;
    v->mtime = v->ctime = from_fat_time(de->wrt_date, de->wrt_time);
    v->atime = from_fat_time(de->acc_date, 0);
}

static uint64_t make_ino(struct fat_fs *fs, uint64_t sector, uint32_t offset)
{
    return sector * (fs->sector_size / DIRENT_SIZE) + offset / DIRENT_SIZE + 2;
}

static void split_ino(struct fat_fs *fs, uint64_t ino, uint64_t *sector, uint32_t *offset)
{
    uint32_t per = fs->sector_size / DIRENT_SIZE;

    *sector = (ino - 2) / per;
    *offset = (ino - 2) % per * DIRENT_SIZE;
}

static int op_read_vnode(struct mount *m, uint64_t ino, struct vnode *v)
{
    struct fat_fs *fs = FAT(m);
    struct fat_node *n = kzalloc(sizeof(*n));
    struct buf *b;

    if (!n)
        return -ENOMEM;
    v->data = n;
    v->uid = v->gid = 0;
    if (ino == ROOT_INO) {
        n->cluster = fs->root_cluster;
        fill_vnode(m, v, NULL);
        return 0;
    }
    split_ino(fs, ino, &n->dirent_sector, &n->dirent_offset);
    if (!(b = bread(fs->dev, n->dirent_sector, fs->sector_size))) {
        kfree(n);
        return -EIO;
    }
    fill_vnode(m, v, (struct fat_dirent *)(b->data + n->dirent_offset));
    brelse(b);
    return 0;
}

static int update_dirent(struct vnode *v)
{
    struct fat_fs *fs = FAT(v->mount);
    struct fat_node *n = NODE(v);
    struct fat_dirent *de;
    struct buf *b;

    if (!n->dirent_sector || n->deleted)
        return 0;
    if (!(b = bread(fs->dev, n->dirent_sector, fs->sector_size)))
        return -EIO;
    de = (struct fat_dirent *)(b->data + n->dirent_offset);
    de->clus_hi = n->cluster >> 16;
    de->clus_lo = n->cluster;
    if (!(de->attr & ATTR_DIR))
        de->size = v->size;
    uint32_t stamp = to_fat_time(v->mtime);
    de->wrt_date = stamp >> 16;
    de->wrt_time = stamp;
    de->attr = (de->attr & ~ATTR_RO) | ((v->mode & 0222) ? 0 : ATTR_RO);
    bwrite(b);
    brelse(b);
    return 0;
}

static void op_release(struct vnode *v)
{
    struct fat_node *n = NODE(v);

    if (n && n->deleted)
        free_chain(FAT(v->mount), n->cluster);
    kfree(n);
    v->data = NULL;
}

struct dir_slot {
    uint64_t sector;
    uint32_t offset;
};

#define MAX_SLOTS 21

struct dir_entry {
    struct fat_dirent de;
    char name[AEGIS_NAME_MAX + 1];
    struct dir_slot slot;           // the short entry
    struct dir_slot slots[MAX_SLOTS];
    uint32_t count;                 // LFN entries + 1
};

static uint8_t lfn_checksum(const uint8_t *name)
{
    uint8_t sum = 0;

    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) << 7) + (sum >> 1) + name[i];
    return sum;
}

static void short_to_name(const struct fat_dirent *de, char *out)
{
    int n = 0;

    for (int i = 0; i < 8 && de->name[i] != ' '; i++)
        out[n++] = (de->ntres & 0x08) && de->name[i] >= 'A' && de->name[i] <= 'Z' ? de->name[i] + 32 : de->name[i];
    if (de->name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && de->name[i] != ' '; i++)
            out[n++] = (de->ntres & 0x10) && de->name[i] >= 'A' && de->name[i] <= 'Z' ? de->name[i] + 32 : de->name[i];
    }
    out[n] = '\0';
    if (out[0] == 0x05)
        out[0] = (char)0xE5;
}

typedef int (*entry_visit)(struct dir_entry *e, void *ctx);

// Iterates a directory's entries; stops when visit returns nonzero.
static int iterate(struct vnode *dir, uint64_t *pos, entry_visit visit, void *ctx)
{
    struct fat_fs *fs = FAT(dir->mount);
    uint32_t cluster = NODE(dir)->cluster;
    uint16_t lfn[260];
    int lfn_count = 0, lfn_expected = 0;
    uint8_t lfn_sum = 0;
    struct dir_slot lfn_slots[MAX_SLOTS];
    uint64_t start = pos ? *pos : 0;

    for (uint64_t off = 0;; off += DIRENT_SIZE) {
        uint32_t c, in_cluster = off % fs->cluster_size;
        uint64_t sector;
        struct buf *b;
        struct fat_dirent *de;

        if (in_cluster == 0 && off) {
            cluster = fat_get(fs, cluster);
            if (cluster < 2 || cluster >= FAT_EOC)
                return 0;
        }
        c = cluster;
        if (c < 2)
            return 0;
        sector = cluster_sector(fs, c) + in_cluster / fs->sector_size;
        if (!(b = bread(fs->dev, sector, fs->sector_size)))
            return -EIO;
        de = (struct fat_dirent *)(b->data + in_cluster % fs->sector_size);

        if (de->name[0] == 0) {
            brelse(b);
            return 0;
        }
        if (de->name[0] == 0xE5) {
            lfn_count = lfn_expected = 0;
            brelse(b);
            continue;
        }
        if (de->attr == ATTR_LFN) {
            struct fat_lfn *l = (struct fat_lfn *)de;
            int seq = l->ord & 0x1F;

            if (l->ord & 0x40) {
                lfn_expected = seq;
                lfn_count = 0;
                lfn_sum = l->checksum;
                memset(lfn, 0xFF, sizeof(lfn));
            }
            if (seq >= 1 && seq <= 20 && l->checksum == lfn_sum && lfn_count < MAX_SLOTS - 1) {
                lfn_slots[lfn_count] = (struct dir_slot){ sector, in_cluster % fs->sector_size };
                uint16_t *dst = lfn + (seq - 1) * 13;
                memcpy(dst, l->name1, 10);
                memcpy(dst + 5, l->name2, 12);
                memcpy(dst + 11, l->name3, 4);
                lfn_count++;
            }
            brelse(b);
            continue;
        }

        struct dir_entry e;
        int ret = 0;

        e.de = *de;
        e.slot = (struct dir_slot){ sector, in_cluster % fs->sector_size };
        brelse(b);

        if (!(e.de.attr & ATTR_VOLUME) && off >= start) {
            if (lfn_expected && lfn_count == lfn_expected && lfn_checksum(e.de.name) == lfn_sum) {
                int n = 0;
                for (int i = 0; i < lfn_expected * 13 && lfn[i] && lfn[i] != 0xFFFF && n < AEGIS_NAME_MAX; i++)
                    e.name[n++] = lfn[i] < 0x80 ? (char)lfn[i] : '?';
                e.name[n] = '\0';
                memcpy(e.slots, lfn_slots, lfn_count * sizeof(struct dir_slot));
                e.slots[lfn_count] = e.slot;
                e.count = lfn_expected + 1;
            } else {
                short_to_name(&e.de, e.name);
                e.slots[0] = e.slot;
                e.count = 1;
            }
            if (pos)
                *pos = off + DIRENT_SIZE;
            ret = visit(&e, ctx);
        }
        lfn_count = lfn_expected = 0;
        if (ret)
            return ret;
    }
}

static bool name_eq(const char *a, const char *b, size_t blen)
{
    size_t i;

    for (i = 0; i < blen && a[i]; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return false;
    }
    return i == blen && a[i] == '\0';
}

struct find_ctx {
    const char *name;
    size_t len;
    struct dir_entry found;
};

static int find_visit(struct dir_entry *e, void *arg)
{
    struct find_ctx *c = arg;

    if (!name_eq(e->name, c->name, c->len))
        return 0;
    c->found = *e;
    return 1;
}

static int find(struct vnode *dir, const char *name, size_t len, struct dir_entry *out)
{
    struct find_ctx c = { .name = name, .len = len };
    int ret = iterate(dir, NULL, find_visit, &c);

    if (ret < 0)
        return ret;
    if (!ret)
        return -ENOENT;
    *out = c.found;
    return 0;
}

struct cluster_ctx {
    uint32_t cluster;
    struct dir_entry found;
};

static int cluster_visit(struct dir_entry *e, void *arg)
{
    struct cluster_ctx *c = arg;

    if (dirent_cluster(&e->de) != c->cluster || e->name[0] == '.')
        return 0;
    c->found = *e;
    return 1;
}

static int op_lookup(struct vnode *dir, const char *name, size_t len, uint64_t *ino)
{
    struct fat_fs *fs = FAT(dir->mount);
    struct dir_entry e;
    int ret;

    if (len == 2 && name[0] == '.' && name[1] == '.') {
        struct vnode tmp = { .mount = dir->mount };
        struct fat_node gn = { 0 };
        struct cluster_ctx c;
        uint32_t parent, grand;

        if (NODE(dir)->cluster == fs->root_cluster) {
            *ino = ROOT_INO;
            return 0;
        }
        if ((ret = find(dir, "..", 2, &e)))
            return ret;
        parent = dirent_cluster(&e.de);
        if (parent == 0 || parent == fs->root_cluster) {
            *ino = ROOT_INO;
            return 0;
        }
        tmp.data = &gn;
        gn.cluster = parent;
        if ((ret = find(&tmp, "..", 2, &e)))
            return ret;
        grand = dirent_cluster(&e.de);
        gn.cluster = grand ? grand : fs->root_cluster;
        c.cluster = parent;
        ret = iterate(&tmp, NULL, cluster_visit, &c);
        if (ret <= 0)
            return ret < 0 ? ret : -ENOENT;
        *ino = make_ino(fs, c.found.slot.sector, c.found.slot.offset);
        return 0;
    }
    if (len == 1 && name[0] == '.') {
        *ino = dir->ino;
        return 0;
    }
    if ((ret = find(dir, name, len, &e)))
        return ret;
    *ino = make_ino(fs, e.slot.sector, e.slot.offset);
    return 0;
}

struct readdir_ctx {
    struct vfs_dirent *out;
    struct fat_fs *fs;
};

static int readdir_visit(struct dir_entry *e, void *arg)
{
    struct readdir_ctx *c = arg;
    size_t len = strlen(e->name);

    if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
        return 0;
    c->out->ino = make_ino(c->fs, e->slot.sector, e->slot.offset);
    c->out->type = (e->de.attr & ATTR_DIR) ? DT_DIR : DT_REG;
    c->out->namelen = len;
    memcpy(c->out->name, e->name, len + 1);
    return 1;
}

static int op_readdir(struct vnode *dir, uint64_t *pos, struct vfs_dirent *out)
{
    struct readdir_ctx c = { out, FAT(dir->mount) };

    return iterate(dir, pos, readdir_visit, &c);
}

static int64_t op_read(struct vnode *v, void *buf, size_t size, uint64_t off)
{
    struct fat_fs *fs = FAT(v->mount);
    size_t done = 0;

    while (done < size) {
        uint64_t pos = off + done;
        uint32_t c, in_cluster = pos % fs->cluster_size;
        uint32_t in_sector = in_cluster % fs->sector_size;
        size_t n = MIN((size_t)(fs->sector_size - in_sector), size - done);
        struct buf *b;

        if (cluster_at(fs, &NODE(v)->cluster, pos, false, &c))
            break;
        if (!(b = bread(fs->dev, cluster_sector(fs, c) + in_cluster / fs->sector_size, fs->sector_size)))
            return done ? (int64_t)done : -EIO;
        memcpy((uint8_t *)buf + done, b->data + in_sector, n);
        brelse(b);
        done += n;
    }
    return done;
}

static int64_t op_write(struct vnode *v, const void *buf, size_t size, uint64_t off)
{
    struct fat_fs *fs = FAT(v->mount);
    size_t done = 0;
    int ret = 0;

    if (off + size > 0xFFFFFFFFULL)
        return -EFBIG;
    while (done < size) {
        uint64_t pos = off + done;
        uint32_t c, in_cluster = pos % fs->cluster_size;
        uint32_t in_sector = in_cluster % fs->sector_size;
        size_t n = MIN((size_t)(fs->sector_size - in_sector), size - done);
        struct buf *b;

        if ((ret = cluster_at(fs, &NODE(v)->cluster, pos, true, &c)))
            break;
        b = (n == fs->sector_size) ? bget(fs->dev, cluster_sector(fs, c) + in_cluster / fs->sector_size, fs->sector_size)
                                   : bread(fs->dev, cluster_sector(fs, c) + in_cluster / fs->sector_size, fs->sector_size);
        if (!b) {
            ret = -EIO;
            break;
        }
        memcpy(b->data + in_sector, (const uint8_t *)buf + done, n);
        bdirty(b);
        brelse(b);
        done += n;
    }
    if (done) {
        if (off + done > v->size)
            v->size = off + done;
        v->mtime = v->ctime = rtc_now();
        update_dirent(v);
    }
    return done ? (int64_t)done : ret;
}

static int op_truncate(struct vnode *v, uint64_t size)
{
    struct fat_fs *fs = FAT(v->mount);
    struct fat_node *n = NODE(v);

    if (size == 0) {
        free_chain(fs, n->cluster);
        n->cluster = 0;
    } else if (size < v->size) {
        uint32_t last;

        if (cluster_at(fs, &n->cluster, size - 1, false, &last) == 0) {
            uint32_t next = fat_get(fs, last);
            fat_set(fs, last, FAT_MASK);
            free_chain(fs, next);
        }
    } else if (size > v->size) {
        static const uint8_t zeros[512];
        uint64_t pos = v->size;

        while (pos < size) {
            size_t chunk = MIN(sizeof(zeros), size - pos);
            if (op_write(v, zeros, chunk, pos) != (int64_t)chunk)
                return -EIO;
            pos += chunk;
        }
    }
    v->size = size;
    v->mtime = v->ctime = rtc_now();
    return update_dirent(v);
}


static bool memchr_char(const char *set, char c)
{
    for (; *set; set++) {
        if (*set == c)
            return true;
    }
    return false;
}

static bool short_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && memchr_char("$%'-_@~`!(){}^#&", c));
}

struct short_ctx {
    const uint8_t *name;
};

static int short_visit(struct dir_entry *e, void *arg)
{
    return !memcmp(e->de.name, ((struct short_ctx *)arg)->name, 11);
}

static bool short_exists(struct vnode *dir, const uint8_t name[11])
{
    struct short_ctx c = { name };

    return iterate(dir, NULL, short_visit, &c) > 0;
}

// Builds an 8.3 name. Returns true if a long-name entry is needed.
static bool make_short(struct vnode *dir, const char *name, size_t len, uint8_t out[11])
{
    const char *dot = NULL;
    bool lossy = false, exact = true;
    size_t b = 0, x = 0;

    for (size_t i = 1; i < len; i++) {
        if (name[i] == '.')
            dot = name + i;
    }
    memset(out, ' ', 11);

    for (const char *p = name; p < (dot ? dot : name + len); p++) {
        char c = *p;

        if (c == ' ' || c == '.') {
            lossy = true;
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c -= 32;
            exact = false;
        } else if (!short_char_ok(c)) {
            c = '_';
            lossy = true;
        }
        if (b < 8)
            out[b++] = c;
        else
            lossy = true;
    }
    for (const char *p = dot ? dot + 1 : name + len; p < name + len; p++) {
        char c = *p;

        if (c >= 'a' && c <= 'z') {
            c -= 32;
            exact = false;
        } else if (!short_char_ok(c)) {
            c = '_';
            lossy = true;
        }
        if (x < 3)
            out[8 + x++] = c;
        else
            lossy = true;
    }
    if (b == 0) {
        out[b++] = '_';
        lossy = true;
    }

    if (lossy || short_exists(dir, out)) {
        for (int n = 1; n < 1000000; n++) {
            char tail[8];
            int tl = ksnprintf(tail, sizeof(tail), "~%d", n);
            size_t keep = MIN(b, (size_t)(8 - tl));

            memcpy(out + keep, tail, tl);
            for (size_t k = keep + tl; k < 8; k++)
                out[k] = ' ';
            if (!short_exists(dir, out))
                break;
        }
        return true;
    }
    return !exact || lossy;
}

static int find_slots(struct vnode *dir, uint32_t count, struct dir_slot *slots)
{
    struct fat_fs *fs = FAT(dir->mount);
    struct fat_node *n = NODE(dir);
    uint32_t cluster = n->cluster, prev = 0, run = 0;

    for (uint64_t off = 0;; off += DIRENT_SIZE) {
        uint32_t in_cluster = off % fs->cluster_size;
        uint64_t sector;
        struct buf *b;
        uint8_t first;

        if (in_cluster == 0 && off) {
            prev = cluster;
            cluster = fat_get(fs, cluster);
            if (cluster < 2 || cluster >= FAT_EOC) {
                int ret = alloc_cluster(fs, prev, &cluster);
                if (ret)
                    return ret;
                zero_cluster(fs, cluster);
            }
        }
        sector = cluster_sector(fs, cluster) + in_cluster / fs->sector_size;
        if (!(b = bread(fs->dev, sector, fs->sector_size)))
            return -EIO;
        first = b->data[in_cluster % fs->sector_size];
        brelse(b);

        if (first == 0 || first == 0xE5) {
            slots[run++] = (struct dir_slot){ sector, in_cluster % fs->sector_size };
            if (run == count)
                return 0;
        } else {
            run = 0;
        }
    }
}

static int write_entries(struct vnode *dir, const char *name, size_t len, const uint8_t *short_name,
                         bool lfn, const struct fat_dirent *tmpl, struct dir_slot *short_slot)
{
    struct fat_fs *fs = FAT(dir->mount);
    uint32_t nlfn = lfn ? (len + 12) / 13 : 0;
    struct dir_slot slots[MAX_SLOTS];
    uint8_t sum = lfn_checksum(short_name);
    int ret;

    if (nlfn + 1 > MAX_SLOTS)
        return -ENAMETOOLONG;
    if ((ret = find_slots(dir, nlfn + 1, slots)))
        return ret;

    for (uint32_t i = 0; i <= nlfn; i++) {
        struct buf *b = bread(fs->dev, slots[i].sector, fs->sector_size);
        uint8_t *p;

        if (!b)
            return -EIO;
        p = b->data + slots[i].offset;
        if (i < nlfn) {
            struct fat_lfn *l = (struct fat_lfn *)p;
            uint32_t seq = nlfn - i;
            uint16_t chars[13];

            for (int k = 0; k < 13; k++) {
                size_t idx = (seq - 1) * 13 + k;
                chars[k] = idx < len ? (uint8_t)name[idx] : idx == len ? 0 : 0xFFFF;
            }
            memset(l, 0, sizeof(*l));
            l->ord = seq | (i == 0 ? 0x40 : 0);
            memcpy(l->name1, chars, 10);
            memcpy(l->name2, chars + 5, 12);
            memcpy(l->name3, chars + 11, 4);
            l->attr = ATTR_LFN;
            l->checksum = sum;
        } else {
            struct fat_dirent *de = (struct fat_dirent *)p;

            *de = *tmpl;
            memcpy(de->name, short_name, 11);
            *short_slot = slots[i];
        }
        bwrite(b);
        brelse(b);
    }
    return 0;
}

static void delete_entries(struct fat_fs *fs, const struct dir_entry *e)
{
    for (uint32_t i = 0; i < e->count; i++) {
        struct buf *b = bread(fs->dev, e->slots[i].sector, fs->sector_size);

        if (!b)
            continue;
        b->data[e->slots[i].offset] = 0xE5;
        bwrite(b);
        brelse(b);
    }
}

static void touch(struct vnode *dir)
{
    dir->mtime = dir->ctime = rtc_now();
    update_dirent(dir);
}

static uint32_t parent_ref(struct fat_fs *fs, struct vnode *dir)
{
    return NODE(dir)->cluster == fs->root_cluster ? 0 : NODE(dir)->cluster;
}

static int write_dot_entries(struct fat_fs *fs, uint32_t cluster, uint32_t parent, const struct fat_dirent *tmpl)
{
    struct buf *b = bread(fs->dev, cluster_sector(fs, cluster), fs->sector_size);
    struct fat_dirent *de;

    if (!b)
        return -EIO;
    de = (struct fat_dirent *)b->data;
    de[0] = *tmpl;
    memcpy(de[0].name, ".          ", 11);
    de[0].clus_hi = cluster >> 16;
    de[0].clus_lo = cluster;
    de[1] = *tmpl;
    memcpy(de[1].name, "..         ", 11);
    de[1].clus_hi = parent >> 16;
    de[1].clus_lo = parent;
    bwrite(b);
    brelse(b);
    return 0;
}

static int op_create(struct vnode *dir, const char *name, size_t len, uint32_t mode,
                     uint32_t uid, uint32_t gid, uint64_t *ino)
{
    struct fat_fs *fs = FAT(dir->mount);
    struct fat_dirent tmpl = { 0 };
    struct dir_slot slot;
    uint8_t short_name[11];
    uint32_t cluster = 0;
    bool lfn;
    int ret;

    (void)uid; (void)gid;
    if (!S_ISREG(mode) && !S_ISDIR(mode))
        return -ENOTSUP;
    lfn = make_short(dir, name, len, short_name);

    tmpl.attr = S_ISDIR(mode) ? ATTR_DIR : ATTR_ARCHIVE;
    if (!(mode & 0222))
        tmpl.attr |= ATTR_RO;
    uint32_t stamp = to_fat_time(rtc_now());
    tmpl.wrt_date = stamp >> 16;
    tmpl.wrt_time = stamp;
    tmpl.crt_date = tmpl.acc_date = tmpl.wrt_date;
    tmpl.crt_time = tmpl.wrt_time;

    if (S_ISDIR(mode)) {
        if ((ret = alloc_cluster(fs, 0, &cluster)))
            return ret;
        zero_cluster(fs, cluster);
        tmpl.clus_hi = cluster >> 16;
        tmpl.clus_lo = cluster;
        if ((ret = write_dot_entries(fs, cluster, parent_ref(fs, dir), &tmpl))) {
            free_chain(fs, cluster);
            return ret;
        }
    }
    if ((ret = write_entries(dir, name, len, short_name, lfn, &tmpl, &slot))) {
        free_chain(fs, cluster);
        return ret;
    }
    dir->size = (uint64_t)chain_length(fs, NODE(dir)->cluster) * fs->cluster_size;
    touch(dir);
    *ino = make_ino(fs, slot.sector, slot.offset);
    return 0;
}

static int empty_visit(struct dir_entry *e, void *ctx)
{
    (void)ctx;
    return strcmp(e->name, ".") && strcmp(e->name, "..");
}

static int remove_entry(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    struct fat_fs *fs = FAT(dir->mount);
    struct dir_entry e;
    int ret;

    if ((ret = find(dir, name, len, &e)))
        return ret;
    delete_entries(fs, &e);
    NODE(victim)->deleted = true;
    victim->nlink = 0;
    touch(dir);
    return 0;
}

static int op_unlink(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    return remove_entry(dir, name, len, victim);
}

static int op_rmdir(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    int ret = iterate(victim, NULL, empty_visit, NULL);

    if (ret)
        return ret < 0 ? ret : -ENOTEMPTY;
    return remove_entry(dir, name, len, victim);
}

static int op_rename(struct vnode *odir, const char *oname, size_t olen,
                     struct vnode *ndir, const char *nname, size_t nlen,
                     struct vnode *src, struct vnode *replaced)
{
    struct fat_fs *fs = FAT(odir->mount);
    struct dir_entry old, rep;
    struct dir_slot slot;
    uint8_t short_name[11];
    bool lfn;
    int ret;

    if ((ret = find(odir, oname, olen, &old)))
        return ret;
    if (replaced) {
        if (S_ISDIR(replaced->mode) && (ret = iterate(replaced, NULL, empty_visit, NULL)))
            return ret < 0 ? ret : -ENOTEMPTY;
        if ((ret = find(ndir, nname, nlen, &rep)))
            return ret;
        delete_entries(fs, &rep);
        NODE(replaced)->deleted = true;
        replaced->nlink = 0;
    }

    lfn = make_short(ndir, nname, nlen, short_name);
    if ((ret = write_entries(ndir, nname, nlen, short_name, lfn, &old.de, &slot)))
        return ret;
    delete_entries(fs, &old);
    NODE(src)->dirent_sector = slot.sector;
    NODE(src)->dirent_offset = slot.offset;

    if (S_ISDIR(src->mode) && odir != ndir) {
        struct buf *b = bread(fs->dev, cluster_sector(fs, NODE(src)->cluster), fs->sector_size);
        if (b) {
            struct fat_dirent *dd = (struct fat_dirent *)b->data + 1;
            uint32_t p = parent_ref(fs, ndir);
            dd->clus_hi = p >> 16;
            dd->clus_lo = p;
            bwrite(b);
            brelse(b);
        }
    }
    touch(odir);
    if (ndir != odir)
        touch(ndir);
    return 0;
}

static int op_setattr(struct vnode *v, const struct vattr *a, uint32_t mask)
{
    if (mask & (VATTR_UID | VATTR_GID))
        return -EPERM;
    if (mask & VATTR_MODE)
        v->mode = (v->mode & S_IFMT) | (S_ISDIR(v->mode) ? 0755 : ((a->mode & 0222) ? 0644 : 0444));
    if (mask & VATTR_MTIME)
        v->mtime = a->mtime;
    if (mask & VATTR_ATIME)
        v->atime = a->atime;
    return update_dirent(v);
}

static int op_statfs(struct mount *m, struct aegis_statfs *out)
{
    struct fat_fs *fs = FAT(m);
    uint64_t free = 0;

    for (uint32_t c = 2; c < fs->cluster_count + 2; c++)
        free += fat_get(fs, c) == 0;
    out->block_size = fs->cluster_size;
    out->blocks = fs->cluster_count;
    out->blocks_free = free;
    return 0;
}

static int op_sync(struct mount *m)
{
    return bcache_sync(FAT(m)->dev);
}

static const struct fs_ops fat_ops = {
    .read_vnode = op_read_vnode,
    .release = op_release,
    .lookup = op_lookup,
    .read = op_read,
    .write = op_write,
    .truncate = op_truncate,
    .readdir = op_readdir,
    .create = op_create,
    .unlink = op_unlink,
    .rmdir = op_rmdir,
    .rename = op_rename,
    .setattr = op_setattr,
    .statfs = op_statfs,
    .sync = op_sync,
};

static int fat_mount(struct block_device *dev, bool readonly, struct mount *m)
{
    uint8_t *bpb = kmalloc(MAX(512, dev->sector_size));
    struct fat_fs *fs;
    uint32_t total, fat_size, root_entries, data_sectors;
    int ret;

    if (!bpb)
        return -ENOMEM;
    if (block_read(dev, 0, bpb, MAX(512, dev->sector_size))) {
        kfree(bpb);
        return -EIO;
    }

    uint16_t bps = *(uint16_t *)(bpb + 11);
    uint8_t spc = bpb[13];
    uint16_t reserved = *(uint16_t *)(bpb + 14);
    uint8_t nfats = bpb[16];
    root_entries = *(uint16_t *)(bpb + 17);
    total = *(uint16_t *)(bpb + 19) ? *(uint16_t *)(bpb + 19) : *(uint32_t *)(bpb + 32);
    fat_size = *(uint16_t *)(bpb + 22) ? *(uint16_t *)(bpb + 22) : *(uint32_t *)(bpb + 36);
    uint32_t root_cluster = *(uint32_t *)(bpb + 44);
    uint16_t fsinfo = *(uint16_t *)(bpb + 48);
    bool sig = bpb[510] == 0x55 && bpb[511] == 0xAA;
    kfree(bpb);

    ret = -EINVAL;
    if (!sig || bps < 512 || (bps & (bps - 1)) || bps != dev->sector_size || !spc || (spc & (spc - 1))
        || !nfats || !fat_size || root_entries)
        return ret;
    data_sectors = total - (reserved + nfats * fat_size);
    if (data_sectors / spc < 65525)
        return -ENOTSUP;

    if (!(fs = kzalloc(sizeof(*fs))))
        return -ENOMEM;
    fs->dev = dev;
    fs->sector_size = bps;
    fs->sectors_per_cluster = spc;
    fs->cluster_size = bps * spc;
    fs->fat_start = reserved;
    fs->fat_sectors = fat_size;
    fs->fat_count = nfats;
    fs->data_start = reserved + nfats * fat_size;
    fs->cluster_count = data_sectors / spc;
    fs->root_cluster = root_cluster;
    fs->next_free = 2;

    // The free-cluster count in FSInfo is only a hint; mark it unknown rather
    // than keeping it exact on every allocation.
    if (!readonly && fsinfo && fsinfo < reserved) {
        struct buf *b = bread(dev, fsinfo, bps);
        if (b) {
            if (*(uint32_t *)b->data == 0x41615252 && *(uint32_t *)(b->data + 484) == 0x61417272) {
                *(uint32_t *)(b->data + 488) = 0xFFFFFFFF;
                bwrite(b);
            }
            brelse(b);
        }
    }

    m->ops = &fat_ops;
    m->data = fs;
    m->root = vget(m, ROOT_INO, &ret);
    if (!m->root) {
        kfree(fs);
        return ret;
    }
    kprintf("fat: mounted %s (%lu MiB)\n", dev->name, (uint64_t)fs->cluster_count * fs->cluster_size >> 20);
    return 0;
}

static struct filesystem fat_fstype = { "fat", fat_mount, NULL };

void fat_register(void)
{
    vfs_register(&fat_fstype);
}
