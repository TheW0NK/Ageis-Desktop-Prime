#include "ext4.h"
#include "crc.h"
#include "mem.h"
#include "string.h"

#define LINK_MAX        65000
#define TAIL_SIZE       12
#define TAIL_FT         0xDE

static struct ext4_fs *fs_of(struct vnode *v)
{
    return EXT4_FS(v->mount);
}

static uint32_t rec_size(uint32_t name_len)
{
    return (8 + name_len + 3) & ~3U;
}

static uint32_t usable_end(struct ext4_fs *fs)
{
    return fs->block_size - (fs->metadata_csum ? TAIL_SIZE : 0);
}

static uint8_t mode_to_ft(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFREG: return EXT4_FT_REG;
    case S_IFDIR: return EXT4_FT_DIR;
    case S_IFCHR: return EXT4_FT_CHR;
    case S_IFBLK: return EXT4_FT_BLK;
    case S_IFIFO: return EXT4_FT_FIFO;
    case S_IFSOCK: return EXT4_FT_SOCK;
    case S_IFLNK: return EXT4_FT_SYMLINK;
    default: return EXT4_FT_UNKNOWN;
    }
}

static uint8_t ft_to_dt(uint8_t ft)
{
    static const uint8_t map[] = { DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK };

    return ft < sizeof(map) ? map[ft] : DT_UNKNOWN;
}

static void init_tail(struct ext4_fs *fs, uint8_t *block)
{
    struct ext4_dirent *t;

    if (!fs->metadata_csum)
        return;
    t = (struct ext4_dirent *)(block + fs->block_size - TAIL_SIZE);
    memset(t, 0, TAIL_SIZE);
    t->rec_len = TAIL_SIZE;
    t->file_type = TAIL_FT;
}

void ext4_csum_dir_block(struct vnode *dir, struct buf *b)
{
    struct ext4_fs *fs = fs_of(dir);
    struct ext4_dirent *t = (struct ext4_dirent *)(b->data + fs->block_size - TAIL_SIZE);

    if (fs->metadata_csum && t->inode == 0 && t->rec_len == TAIL_SIZE && t->file_type == TAIL_FT)
        *(uint32_t *)(b->data + fs->block_size - 4) =
            crc32c(EXT4_I(dir)->csum_seed, b->data, fs->block_size - TAIL_SIZE);
}

static void dir_dirty(struct vnode *dir, struct buf *b)
{
    ext4_csum_dir_block(dir, b);
    ext4_dirty(fs_of(dir), b);
}

static struct buf *dir_block(struct vnode *dir, uint32_t lblk)
{
    uint64_t pblk;
    uint32_t run = 0;

    if (ext4_map(dir, lblk, false, &pblk, &run) || !pblk)
        return NULL;
    return bread(fs_of(dir)->dev, pblk, fs_of(dir)->block_size);
}

static bool valid_entry(struct ext4_fs *fs, struct ext4_dirent *de, uint32_t off)
{
    return de->rec_len >= 8 && off + de->rec_len <= fs->block_size && de->name_len + 8U <= de->rec_len;
}

typedef bool (*dir_visit)(struct vnode *dir, struct buf *b, struct ext4_dirent *de,
                          struct ext4_dirent *prev, void *ctx);

// Calls visit for every live entry until it returns true. Returns 1 if stopped.
static int walk_dir(struct vnode *dir, dir_visit visit, void *ctx)
{
    struct ext4_fs *fs = fs_of(dir);
    uint32_t blocks = dir->size / fs->block_size;

    if (EXT4_RAW(dir)->flags & EXT4_INLINE_DATA_FL)
        return -ENOTSUP;

    for (uint32_t lblk = 0; lblk < blocks; lblk++) {
        struct buf *b = dir_block(dir, lblk);
        struct ext4_dirent *prev = NULL;

        if (!b)
            return -EIO;
        for (uint32_t off = 0; off + 8 <= fs->block_size;) {
            struct ext4_dirent *de = (struct ext4_dirent *)(b->data + off);

            if (!valid_entry(fs, de, off))
                break;
            if (de->inode && visit(dir, b, de, prev, ctx)) {
                brelse(b);
                return 1;
            }
            prev = de;
            off += de->rec_len;
        }
        brelse(b);
    }
    return 0;
}

struct find_ctx {
    const char *name;
    size_t len;
    uint32_t ino;
    uint32_t new_ino;
    uint8_t new_ft;
    int action;
};

enum { FIND, REMOVE, RETARGET };

static bool find_visit(struct vnode *dir, struct buf *b, struct ext4_dirent *de,
                       struct ext4_dirent *prev, void *arg)
{
    struct find_ctx *c = arg;

    if (de->name_len != c->len || memcmp(de->name, c->name, c->len))
        return false;
    c->ino = de->inode;
    if (c->action == REMOVE) {
        if (prev)
            prev->rec_len += de->rec_len;
        else
            de->inode = 0;
        dir_dirty(dir, b);
    } else if (c->action == RETARGET) {
        de->inode = c->new_ino;
        if (fs_of(dir)->sb->feature_incompat & EXT4_INCOMPAT_FILETYPE)
            de->file_type = c->new_ft;
        dir_dirty(dir, b);
    }
    return true;
}

static int dir_find(struct vnode *dir, const char *name, size_t len, int action,
                    uint32_t new_ino, uint8_t new_ft, uint32_t *ino)
{
    struct find_ctx c = { name, len, 0, new_ino, new_ft, action };
    int ret = walk_dir(dir, find_visit, &c);

    if (ret < 0)
        return ret;
    if (ret == 0)
        return -ENOENT;
    if (ino)
        *ino = c.ino;
    return 0;
}

// Converts an htree directory to a linear one so entries can be changed
// without maintaining the hash index.
static int deindex(struct vnode *dir)
{
    struct ext4_fs *fs = fs_of(dir);
    struct ext4_inode *in = EXT4_RAW(dir);
    uint32_t nodes[64], count = 0, end = usable_end(fs);
    struct buf *b;

    if (!(in->flags & EXT4_INDEX_FL))
        return 0;
    if (!(b = dir_block(dir, 0)))
        return -EIO;

    uint8_t levels = b->data[24 + 6];
    uint8_t info_len = b->data[24 + 5];
    uint16_t n = *(uint16_t *)(b->data + 24 + info_len + 2);
    uint8_t *entries = b->data + 24 + info_len;

    for (uint16_t i = 0; levels >= 1 && i < n && count < 64; i++)
        nodes[count++] = *(uint32_t *)(entries + i * 8 + 4);
    for (uint32_t k = 0; levels >= 2 && k < count && count < 64; k++) {
        struct buf *nb = dir_block(dir, nodes[k]);
        if (!nb)
            continue;
        uint16_t nn = *(uint16_t *)(nb->data + 8 + 2);
        for (uint16_t i = 0; i < nn && count < 64; i++)
            nodes[count++] = *(uint32_t *)(nb->data + 8 + i * 8 + 4);
        brelse(nb);
    }

    struct ext4_dirent *dotdot = (struct ext4_dirent *)(b->data + 12);
    dotdot->rec_len = end - 12;
    memset(b->data + 24, 0, fs->block_size - 24);
    init_tail(fs, b->data);
    dir_dirty(dir, b);
    brelse(b);

    for (uint32_t k = 0; k < count; k++) {
        struct buf *nb = dir_block(dir, nodes[k]);
        struct ext4_dirent *de;

        if (!nb)
            continue;
        memset(nb->data, 0, fs->block_size);
        de = (struct ext4_dirent *)nb->data;
        de->rec_len = end;
        init_tail(fs, nb->data);
        dir_dirty(dir, nb);
        brelse(nb);
    }

    in->flags &= ~EXT4_INDEX_FL;
    return ext4_write_inode(dir);
}

static int add_entry(struct vnode *dir, const char *name, size_t len, uint32_t ino, uint8_t ft)
{
    struct ext4_fs *fs = fs_of(dir);
    uint32_t need = rec_size(len), end = usable_end(fs);
    uint32_t blocks = dir->size / fs->block_size;
    bool filetype = fs->sb->feature_incompat & EXT4_INCOMPAT_FILETYPE;
    struct ext4_dirent *de;
    struct buf *b;
    uint64_t pblk;
    uint32_t run = 1;
    int ret;

    if ((ret = deindex(dir)))
        return ret;

    for (uint32_t lblk = 0; lblk < blocks; lblk++) {
        if (!(b = dir_block(dir, lblk)))
            return -EIO;
        for (uint32_t off = 0; off < end;) {
            de = (struct ext4_dirent *)(b->data + off);
            if (!valid_entry(fs, de, off))
                break;
            uint32_t used = de->inode ? rec_size(de->name_len) : 0;

            if (off + de->rec_len <= end && de->rec_len >= used + need) {
                struct ext4_dirent *nde = de;

                if (de->inode) {
                    nde = (struct ext4_dirent *)(b->data + off + used);
                    nde->rec_len = de->rec_len - used;
                    de->rec_len = used;
                }
                nde->inode = ino;
                nde->name_len = len;
                nde->file_type = filetype ? ft : 0;
                memcpy(nde->name, name, len);
                dir_dirty(dir, b);
                brelse(b);
                return 0;
            }
            off += de->rec_len;
        }
        brelse(b);
    }

    if ((ret = ext4_map(dir, blocks, true, &pblk, &run)))
        return ret;
    if (!(b = bget(fs->dev, pblk, fs->block_size)))
        return -EIO;
    memset(b->data, 0, fs->block_size);
    de = (struct ext4_dirent *)b->data;
    de->inode = ino;
    de->rec_len = end;
    de->name_len = len;
    de->file_type = filetype ? ft : 0;
    memcpy(de->name, name, len);
    init_tail(fs, b->data);
    dir_dirty(dir, b);
    brelse(b);

    ext4_set_isize(EXT4_RAW(dir), (uint64_t)(blocks + 1) * fs->block_size);
    return ext4_write_inode(dir);
}

static bool empty_visit(struct vnode *dir, struct buf *b, struct ext4_dirent *de,
                        struct ext4_dirent *prev, void *ctx)
{
    (void)dir; (void)b; (void)prev; (void)ctx;
    return !((de->name_len == 1 && de->name[0] == '.')
             || (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.'));
}

static void inc_links(struct ext4_fs *fs, struct ext4_inode *in)
{
    if (S_ISDIR(in->mode) && in->links_count >= LINK_MAX - 1
        && (fs->sb->feature_ro_compat & EXT4_RO_COMPAT_DIR_NLINK))
        in->links_count = 1;
    else if (!(S_ISDIR(in->mode) && in->links_count == 1))
        in->links_count++;
}

static void dec_links(struct ext4_inode *in)
{
    if (S_ISDIR(in->mode) && in->links_count == 1)
        return;
    if (in->links_count)
        in->links_count--;
}

static int temp_vnode(struct ext4_fs *fs, uint32_t ino, struct vnode *tv)
{
    memset(tv, 0, sizeof(*tv));
    tv->mount = fs->mount;
    tv->ino = ino;
    return ext4_read_inode(fs, ino, tv);
}

static void orphan_add(struct ext4_fs *fs, struct vnode *v)
{
    EXT4_RAW(v)->dtime = fs->sb->last_orphan;
    fs->sb->last_orphan = v->ino;
    ext4_sb_dirty(fs);
}

static void orphan_remove(struct ext4_fs *fs, struct vnode *v)
{
    struct ext4_inode *in = EXT4_RAW(v);
    uint32_t cur = fs->sb->last_orphan;

    if (cur == v->ino) {
        fs->sb->last_orphan = in->dtime;
        ext4_sb_dirty(fs);
        return;
    }
    for (int guard = 0; cur && guard < 100000; guard++) {
        struct vnode tv;
        uint32_t next;

        if (temp_vnode(fs, cur, &tv))
            return;
        next = EXT4_RAW(&tv)->dtime;
        if (next == v->ino) {
            EXT4_RAW(&tv)->dtime = in->dtime;
            ext4_write_inode(&tv);
            kfree(tv.data);
            return;
        }
        kfree(tv.data);
        cur = next;
    }
}

static void delete_inode(struct ext4_fs *fs, struct vnode *v)
{
    struct ext4_inode *in = EXT4_RAW(v);
    bool dir = S_ISDIR(in->mode);

    ext4_file_truncate(v, 0);
    in->dtime = ext4_now();
    ext4_write_inode(v);
    ext4_free_inode(fs, v->ino, dir);
}

int ext4_process_orphans(struct ext4_fs *fs)
{
    uint32_t cur = fs->sb->last_orphan;

    for (int guard = 0; cur && guard < 100000; guard++) {
        struct vnode tv;
        uint32_t next;

        if (temp_vnode(fs, cur, &tv))
            break;
        next = EXT4_RAW(&tv)->dtime;
        if (EXT4_RAW(&tv)->links_count == 0) {
            delete_inode(fs, &tv);
        } else {
            EXT4_RAW(&tv)->dtime = 0;
            ext4_write_inode(&tv);
        }
        kfree(tv.data);
        cur = next;
    }
    if (fs->sb->last_orphan) {
        fs->sb->last_orphan = 0;
        ext4_sb_dirty(fs);
    }
    return 0;
}

static int op_read_vnode(struct mount *m, uint64_t ino, struct vnode *v)
{
    return ext4_read_inode(EXT4_FS(m), ino, v);
}

static void op_release(struct vnode *v)
{
    struct ext4_fs *fs = fs_of(v);

    if (v->data && EXT4_RAW(v)->links_count == 0 && EXT4_RAW(v)->mode && !fs->readonly) {
        orphan_remove(fs, v);
        delete_inode(fs, v);
        ext4_op_end(fs);
    }
    kfree(v->data);
    v->data = NULL;
}

static int op_lookup(struct vnode *dir, const char *name, size_t len, uint64_t *ino)
{
    uint32_t i;
    int ret = dir_find(dir, name, len, FIND, 0, 0, &i);

    if (ret == 0)
        *ino = i;
    return ret;
}

static int64_t op_write(struct vnode *v, const void *buf, size_t size, uint64_t off)
{
    int64_t ret = ext4_file_write(v, buf, size, off);

    ext4_op_end(fs_of(v));
    return ret;
}

static int op_truncate(struct vnode *v, uint64_t size)
{
    int ret = ext4_file_truncate(v, size);

    ext4_op_end(fs_of(v));
    return ret;
}

static int op_readdir(struct vnode *dir, uint64_t *pos, struct vfs_dirent *out)
{
    struct ext4_fs *fs = fs_of(dir);

    if (EXT4_RAW(dir)->flags & EXT4_INLINE_DATA_FL)
        return -ENOTSUP;

    while (*pos < dir->size) {
        uint32_t lblk = *pos / fs->block_size, off = *pos % fs->block_size;
        struct buf *b = dir_block(dir, lblk);
        struct ext4_dirent *de;

        if (!b)
            return -EIO;
        de = (struct ext4_dirent *)(b->data + off);
        if (off + 8 > fs->block_size || !valid_entry(fs, de, off)) {
            brelse(b);
            *pos = (uint64_t)(lblk + 1) * fs->block_size;
            continue;
        }
        *pos += de->rec_len;
        if (de->inode && de->name_len) {
            out->ino = de->inode;
            out->type = ft_to_dt(de->file_type);
            out->namelen = de->name_len;
            memcpy(out->name, de->name, de->name_len);
            out->name[de->name_len] = '\0';
            brelse(b);
            return 1;
        }
        brelse(b);
    }
    return 0;
}

static int new_inode(struct vnode *dir, uint32_t mode, uint32_t uid, uint32_t gid, struct vnode *tv)
{
    struct ext4_fs *fs = fs_of(dir);
    struct ext4_inode_info *info;
    struct ext4_inode *in;
    uint32_t ino, lo, hi;
    int64_t now = ext4_now();
    int ret;

    if ((ret = ext4_alloc_inode(fs, EXT4_I(dir)->group, S_ISDIR(mode), &ino)))
        return ret;
    info = kzalloc(sizeof(*info) + fs->inode_size);
    if (!info) {
        ext4_free_inode(fs, ino, S_ISDIR(mode));
        return -ENOMEM;
    }

    in = (struct ext4_inode *)info->raw;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    in->mode = mode;
    in->uid = uid;
    in->uid_high = uid >> 16;
    in->gid = gid;
    in->gid_high = gid >> 16;
    in->links_count = S_ISDIR(mode) ? 2 : 1;
    in->atime = in->ctime = in->mtime = now;
    in->generation = lo ^ hi ^ ino;
    if (fs->inode_size > 128) {
        in->extra_isize = MAX(32, fs->sb->want_extra_isize);
        if (128U + in->extra_isize > fs->inode_size)
            in->extra_isize = fs->inode_size - 128;
        in->crtime = now;
    }
    if (!S_ISLNK(mode))
        ext4_init_extents(in);

    info->group = (ino - 1) / fs->inodes_per_group;
    info->csum_seed = crc32c(crc32c(fs->csum_seed, &ino, 4), &in->generation, 4);
    memset(tv, 0, sizeof(*tv));
    tv->mount = dir->mount;
    tv->ino = ino;
    tv->data = info;
    return 0;
}

static int init_dir_block(struct vnode *tv, uint32_t parent)
{
    struct ext4_fs *fs = fs_of(tv);
    bool filetype = fs->sb->feature_incompat & EXT4_INCOMPAT_FILETYPE;
    struct ext4_dirent *dot, *dotdot;
    uint64_t pblk;
    uint32_t run = 1;
    struct buf *b;
    int ret;

    if ((ret = ext4_map(tv, 0, true, &pblk, &run)))
        return ret;
    if (!(b = bget(fs->dev, pblk, fs->block_size)))
        return -EIO;
    memset(b->data, 0, fs->block_size);
    dot = (struct ext4_dirent *)b->data;
    dot->inode = tv->ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = filetype ? EXT4_FT_DIR : 0;
    dot->name[0] = '.';
    dotdot = (struct ext4_dirent *)(b->data + 12);
    dotdot->inode = parent;
    dotdot->rec_len = usable_end(fs) - 12;
    dotdot->name_len = 2;
    dotdot->file_type = filetype ? EXT4_FT_DIR : 0;
    dotdot->name[0] = dotdot->name[1] = '.';
    init_tail(fs, b->data);
    dir_dirty(tv, b);
    brelse(b);
    ext4_set_isize(EXT4_RAW(tv), fs->block_size);
    return 0;
}

static void touch_dir(struct vnode *dir)
{
    struct ext4_inode *in = EXT4_RAW(dir);

    in->mtime = in->ctime = ext4_now();
    ext4_write_inode(dir);
}

static int op_create(struct vnode *dir, const char *name, size_t len, uint32_t mode,
                     uint32_t uid, uint32_t gid, uint64_t *ino)
{
    struct ext4_fs *fs = fs_of(dir);
    struct vnode tv;
    int ret;

    if ((ret = new_inode(dir, mode, uid, gid, &tv)))
        return ret;
    if (S_ISDIR(mode) && (ret = init_dir_block(&tv, dir->ino)))
        goto fail;
    if ((ret = ext4_write_inode(&tv)))
        goto fail;
    if ((ret = add_entry(dir, name, len, tv.ino, mode_to_ft(mode))))
        goto fail;
    if (S_ISDIR(mode))
        inc_links(fs, EXT4_RAW(dir));
    touch_dir(dir);
    *ino = tv.ino;
    kfree(tv.data);
    ext4_op_end(fs);
    return 0;

fail:
    EXT4_RAW(&tv)->links_count = 0;
    delete_inode(fs, &tv);
    kfree(tv.data);
    return ret;
}

static int op_symlink(struct vnode *dir, const char *name, size_t len, const char *target,
                      uint32_t uid, uint32_t gid, uint64_t *ino)
{
    struct ext4_fs *fs = fs_of(dir);
    size_t tlen = strlen(target);
    struct ext4_inode *in;
    struct vnode tv;
    int ret;

    if ((ret = new_inode(dir, S_IFLNK | 0777, uid, gid, &tv)))
        return ret;
    in = EXT4_RAW(&tv);
    if (tlen < sizeof(in->block)) {
        memcpy(in->block, target, tlen);
        ext4_set_isize(in, tlen);
    } else {
        ext4_init_extents(in);
        if (ext4_file_write(&tv, target, tlen, 0) != (int64_t)tlen) {
            ret = -EIO;
            goto fail;
        }
    }
    if ((ret = ext4_write_inode(&tv)) || (ret = add_entry(dir, name, len, tv.ino, EXT4_FT_SYMLINK)))
        goto fail;
    touch_dir(dir);
    *ino = tv.ino;
    kfree(tv.data);
    ext4_op_end(fs);
    return 0;

fail:
    in->links_count = 0;
    delete_inode(fs, &tv);
    kfree(tv.data);
    return ret;
}

static int op_link(struct vnode *dir, const char *name, size_t len, struct vnode *target)
{
    struct ext4_inode *in = EXT4_RAW(target);
    int ret;

    if (in->links_count >= LINK_MAX)
        return -EMLINK;
    if ((ret = add_entry(dir, name, len, target->ino, mode_to_ft(in->mode))))
        return ret;
    in->links_count++;
    in->ctime = ext4_now();
    ext4_write_inode(target);
    touch_dir(dir);
    ext4_op_end(fs_of(dir));
    return 0;
}

static int op_unlink(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    struct ext4_fs *fs = fs_of(dir);
    struct ext4_inode *in = EXT4_RAW(victim);
    int ret;

    if ((ret = deindex(dir)) || (ret = dir_find(dir, name, len, REMOVE, 0, 0, NULL)))
        return ret;
    dec_links(in);
    in->ctime = ext4_now();
    if (in->links_count == 0)
        orphan_add(fs, victim);
    ext4_write_inode(victim);
    touch_dir(dir);
    ext4_op_end(fs);
    return 0;
}

static int op_rmdir(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    struct ext4_fs *fs = fs_of(dir);
    struct ext4_inode *in = EXT4_RAW(victim);
    int ret = walk_dir(victim, empty_visit, NULL);

    if (ret < 0)
        return ret;
    if (ret > 0)
        return -ENOTEMPTY;
    if ((ret = deindex(dir)) || (ret = dir_find(dir, name, len, REMOVE, 0, 0, NULL)))
        return ret;

    in->links_count = 0;
    in->ctime = ext4_now();
    orphan_add(fs, victim);
    ext4_write_inode(victim);
    dec_links(EXT4_RAW(dir));
    touch_dir(dir);
    ext4_op_end(fs);
    return 0;
}

static int op_rename(struct vnode *odir, const char *oname, size_t olen,
                     struct vnode *ndir, const char *nname, size_t nlen,
                     struct vnode *src, struct vnode *replaced)
{
    struct ext4_fs *fs = fs_of(odir);
    uint8_t ft = mode_to_ft(src->mode);
    int ret;

    if (replaced && S_ISDIR(replaced->mode)) {
        ret = walk_dir(replaced, empty_visit, NULL);
        if (ret)
            return ret < 0 ? ret : -ENOTEMPTY;
    }
    if ((ret = deindex(odir)) || (ret = deindex(ndir)))
        return ret;

    if (replaced) {
        struct ext4_inode *rin = EXT4_RAW(replaced);

        if ((ret = dir_find(ndir, nname, nlen, RETARGET, src->ino, ft, NULL)))
            return ret;
        if (S_ISDIR(rin->mode)) {
            rin->links_count = 0;
            dec_links(EXT4_RAW(ndir));
        } else {
            dec_links(rin);
        }
        rin->ctime = ext4_now();
        if (rin->links_count == 0)
            orphan_add(fs, replaced);
        ext4_write_inode(replaced);
    } else if ((ret = add_entry(ndir, nname, nlen, src->ino, ft))) {
        return ret;
    }

    if ((ret = dir_find(odir, oname, olen, REMOVE, 0, 0, NULL)))
        return ret;

    if (S_ISDIR(src->mode) && odir != ndir) {
        dir_find(src, "..", 2, RETARGET, ndir->ino, EXT4_FT_DIR, NULL);
        dec_links(EXT4_RAW(odir));
        inc_links(fs, EXT4_RAW(ndir));
    }
    EXT4_RAW(src)->ctime = ext4_now();
    ext4_write_inode(src);
    touch_dir(odir);
    if (ndir != odir)
        touch_dir(ndir);
    ext4_op_end(fs);
    return 0;
}

static int op_readlink(struct vnode *v, char *buf, size_t size)
{
    size_t n = MIN(size, v->size);

    return ext4_file_read(v, buf, n, 0);
}

static int op_setattr(struct vnode *v, const struct vattr *a, uint32_t mask)
{
    struct ext4_inode *in = EXT4_RAW(v);
    int ret;

    if (mask & VATTR_MODE)
        in->mode = (in->mode & S_IFMT) | (a->mode & 07777);
    if (mask & VATTR_UID) {
        in->uid = a->uid;
        in->uid_high = a->uid >> 16;
    }
    if (mask & VATTR_GID) {
        in->gid = a->gid;
        in->gid_high = a->gid >> 16;
    }
    if (mask & VATTR_ATIME)
        in->atime = a->atime;
    if (mask & VATTR_MTIME)
        in->mtime = a->mtime;
    in->ctime = ext4_now();
    ret = ext4_write_inode(v);
    ext4_op_end(fs_of(v));
    return ret;
}

int ext4_fs_sync(struct mount *m);
int ext4_fs_unmount(struct mount *m);
int ext4_fs_statfs(struct mount *m, struct aegis_statfs *out);

const struct fs_ops ext4_ops = {
    .read_vnode = op_read_vnode,
    .release = op_release,
    .lookup = op_lookup,
    .read = ext4_file_read,
    .write = op_write,
    .truncate = op_truncate,
    .readdir = op_readdir,
    .create = op_create,
    .symlink = op_symlink,
    .link = op_link,
    .unlink = op_unlink,
    .rmdir = op_rmdir,
    .rename = op_rename,
    .readlink = op_readlink,
    .setattr = op_setattr,
    .statfs = ext4_fs_statfs,
    .sync = ext4_fs_sync,
    .unmount = ext4_fs_unmount,
};
