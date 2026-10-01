#ifndef AEGIS_EXT4_H
#define AEGIS_EXT4_H

#include "bcache.h"
#include "vfs.h"

#define EXT4_MAGIC                  0xEF53
#define EXT4_ROOT_INO               2
#define EXT4_N_BLOCKS               15

#define EXT4_COMPAT_HAS_JOURNAL     0x0004
#define EXT4_INCOMPAT_FILETYPE      0x0002
#define EXT4_INCOMPAT_RECOVER       0x0004
#define EXT4_INCOMPAT_JOURNAL_DEV   0x0008
#define EXT4_INCOMPAT_META_BG       0x0010
#define EXT4_INCOMPAT_EXTENTS       0x0040
#define EXT4_INCOMPAT_64BIT         0x0080
#define EXT4_INCOMPAT_MMP           0x0100
#define EXT4_INCOMPAT_FLEX_BG       0x0200
#define EXT4_INCOMPAT_CSUM_SEED     0x2000
#define EXT4_INCOMPAT_LARGEDIR      0x4000
#define EXT4_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT4_RO_COMPAT_LARGE_FILE   0x0002
#define EXT4_RO_COMPAT_HUGE_FILE    0x0008
#define EXT4_RO_COMPAT_GDT_CSUM     0x0010
#define EXT4_RO_COMPAT_DIR_NLINK    0x0020
#define EXT4_RO_COMPAT_EXTRA_ISIZE  0x0040
#define EXT4_RO_COMPAT_METADATA_CSUM 0x0400

#define EXT4_INCOMPAT_SUPPORTED     (EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_RECOVER | \
                                     EXT4_INCOMPAT_EXTENTS | EXT4_INCOMPAT_64BIT | \
                                     EXT4_INCOMPAT_FLEX_BG | EXT4_INCOMPAT_CSUM_SEED | \
                                     EXT4_INCOMPAT_LARGEDIR)
#define EXT4_RO_COMPAT_SUPPORTED    (EXT4_RO_COMPAT_SPARSE_SUPER | EXT4_RO_COMPAT_LARGE_FILE | \
                                     EXT4_RO_COMPAT_HUGE_FILE | EXT4_RO_COMPAT_GDT_CSUM | \
                                     EXT4_RO_COMPAT_DIR_NLINK | EXT4_RO_COMPAT_EXTRA_ISIZE | \
                                     EXT4_RO_COMPAT_METADATA_CSUM)

#define EXT4_BG_INODE_UNINIT        0x0001
#define EXT4_BG_BLOCK_UNINIT        0x0002

#define EXT4_INDEX_FL               0x00001000
#define EXT4_EXTENTS_FL             0x00080000
#define EXT4_INLINE_DATA_FL         0x10000000

#define EXT4_EXT_MAGIC              0xF30A
#define EXT4_EXT_INIT_MAX_LEN       32768

#define EXT4_FT_UNKNOWN             0
#define EXT4_FT_REG                 1
#define EXT4_FT_DIR                 2
#define EXT4_FT_CHR                 3
#define EXT4_FT_BLK                 4
#define EXT4_FT_FIFO                5
#define EXT4_FT_SOCK                6
#define EXT4_FT_SYMLINK             7

struct ext4_super {
    uint32_t inodes_count;
    uint32_t blocks_count_lo;
    uint32_t r_blocks_count_lo;
    uint32_t free_blocks_count_lo;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_cluster_size;
    uint32_t blocks_per_group;
    uint32_t clusters_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;
    uint8_t journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;
    uint32_t hash_seed[4];
    uint8_t def_hash_version;
    uint8_t jnl_backup_type;
    uint16_t desc_size;
    uint32_t default_mount_opts;
    uint32_t first_meta_bg;
    uint32_t mkfs_time;
    uint32_t jnl_blocks[17];
    uint32_t blocks_count_hi;
    uint32_t r_blocks_count_hi;
    uint32_t free_blocks_count_hi;
    uint16_t min_extra_isize;
    uint16_t want_extra_isize;
    uint32_t flags;
    uint8_t pad1[0x270 - 0x164];
    uint32_t checksum_seed;
    uint8_t pad2[0x3FC - 0x274];
    uint32_t checksum;
} __attribute__((packed));

struct ext4_group_desc {
    uint32_t block_bitmap_lo;
    uint32_t inode_bitmap_lo;
    uint32_t inode_table_lo;
    uint16_t free_blocks_count_lo;
    uint16_t free_inodes_count_lo;
    uint16_t used_dirs_count_lo;
    uint16_t flags;
    uint32_t exclude_bitmap_lo;
    uint16_t block_bitmap_csum_lo;
    uint16_t inode_bitmap_csum_lo;
    uint16_t itable_unused_lo;
    uint16_t checksum;
    uint32_t block_bitmap_hi;
    uint32_t inode_bitmap_hi;
    uint32_t inode_table_hi;
    uint16_t free_blocks_count_hi;
    uint16_t free_inodes_count_hi;
    uint16_t used_dirs_count_hi;
    uint16_t itable_unused_hi;
    uint32_t exclude_bitmap_hi;
    uint16_t block_bitmap_csum_hi;
    uint16_t inode_bitmap_csum_hi;
    uint32_t reserved;
} __attribute__((packed));

struct ext4_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks_lo;
    uint32_t flags;
    uint32_t version;
    uint32_t block[EXT4_N_BLOCKS];
    uint32_t generation;
    uint32_t file_acl_lo;
    uint32_t size_high;
    uint32_t obso_faddr;
    uint16_t blocks_high;
    uint16_t file_acl_high;
    uint16_t uid_high;
    uint16_t gid_high;
    uint16_t checksum_lo;
    uint16_t reserved;
    uint16_t extra_isize;
    uint16_t checksum_hi;
    uint32_t ctime_extra;
    uint32_t mtime_extra;
    uint32_t atime_extra;
    uint32_t crtime;
    uint32_t crtime_extra;
    uint32_t version_hi;
    uint32_t projid;
} __attribute__((packed));

struct ext4_extent_header {
    uint16_t magic;
    uint16_t entries;
    uint16_t max;
    uint16_t depth;
    uint32_t generation;
} __attribute__((packed));

struct ext4_extent {
    uint32_t block;
    uint16_t len;
    uint16_t start_hi;
    uint32_t start_lo;
} __attribute__((packed));

struct ext4_extent_idx {
    uint32_t block;
    uint32_t leaf_lo;
    uint16_t leaf_hi;
    uint16_t unused;
} __attribute__((packed));

struct ext4_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
} __attribute__((packed));

struct jbd2;

struct ext4_range {
    uint64_t start;
    uint64_t count;
};

struct ext4_fs {
    struct mount *mount;
    struct block_device *dev;
    uint32_t block_size;
    uint64_t blocks_count;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_data_block;
    uint32_t inode_size;
    uint32_t desc_size;
    uint32_t group_count;
    uint32_t first_ino;
    uint32_t gdt_blocks;
    uint32_t itable_blocks;
    uint32_t csum_seed;
    bool metadata_csum;
    bool gdt_csum;
    bool is64;
    bool readonly;

    struct ext4_super *sb;          // points into sb_buf
    struct buf *sb_buf;
    struct buf **gdt;

    struct jbd2 *journal;
    struct buf **txn;               // metadata buffers in the running transaction
    size_t txn_count, txn_capacity;
    struct ext4_range *pending_free;
    size_t pending_count, pending_capacity;
    uint32_t last_group;
};

struct ext4_inode_info {
    uint32_t csum_seed;
    uint32_t group;
    uint8_t raw[];
};

#define EXT4_FS(m)      ((struct ext4_fs *)(m)->data)
#define EXT4_I(v)       ((struct ext4_inode_info *)(v)->data)
#define EXT4_RAW(v)     ((struct ext4_inode *)EXT4_I(v)->raw)

// ext4_super.c
void ext4_dirty(struct ext4_fs *fs, struct buf *b);
void ext4_sb_dirty(struct ext4_fs *fs);
struct ext4_group_desc *ext4_gd(struct ext4_fs *fs, uint32_t group);
void ext4_gd_dirty(struct ext4_fs *fs, uint32_t group);
uint64_t ext4_inode_table(struct ext4_fs *fs, uint32_t group);
int ext4_alloc_blocks(struct ext4_fs *fs, uint64_t goal, uint64_t want, uint64_t *start, uint64_t *got);
void ext4_free_blocks(struct ext4_fs *fs, uint64_t start, uint64_t count);
int ext4_alloc_inode(struct ext4_fs *fs, uint32_t goal_group, bool dir, uint32_t *ino);
void ext4_free_inode(struct ext4_fs *fs, uint32_t ino, bool dir);
int ext4_commit(struct ext4_fs *fs);
void ext4_op_end(struct ext4_fs *fs);
int64_t ext4_now(void);

// ext4_inode.c
int ext4_read_inode(struct ext4_fs *fs, uint32_t ino, struct vnode *v);
int ext4_write_inode(struct vnode *v);
void ext4_sync_vnode(struct vnode *v);
uint64_t ext4_isize(struct ext4_inode *in);
void ext4_set_isize(struct ext4_inode *in, uint64_t size);
int ext4_map(struct vnode *v, uint32_t lblk, bool create, uint64_t *pblk, uint32_t *run);
int64_t ext4_file_read(struct vnode *v, void *buf, size_t size, uint64_t off);
int64_t ext4_file_write(struct vnode *v, const void *buf, size_t size, uint64_t off);
int ext4_file_truncate(struct vnode *v, uint64_t size);
void ext4_init_extents(struct ext4_inode *in);
void ext4_csum_extent_block(struct vnode *v, struct buf *b);

// ext4_dir.c
extern const struct fs_ops ext4_ops;
void ext4_csum_dir_block(struct vnode *dir, struct buf *b);
int ext4_process_orphans(struct ext4_fs *fs);

// jbd2.c
int jbd2_load(struct ext4_fs *fs, uint32_t journal_ino);
int jbd2_recover(struct ext4_fs *fs);
int jbd2_commit(struct ext4_fs *fs, struct buf **bufs, size_t count);
uint32_t jbd2_max_transaction(struct ext4_fs *fs);

#endif
