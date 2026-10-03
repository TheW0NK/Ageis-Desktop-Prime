#include "aegis.h"

// Disks for the installer and recovery: listing them, writing a GPT and
// creating FAT32 (the EFI system partition) and ext4 (the system) on
// partitions. Everything goes through /osystem/devices/<name> as root.

// ---- Helpers ----

static uint32_t crc_table[256];

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFF;

    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;

            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320 ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    }
    while (len--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}

static void random_fill(void *buf, size_t n)
{
    int fd = open("/osystem/devices/urandom", O_RDONLY);

    if (fd < 0 || read(fd, buf, n) != (ssize_t)n) {
        uint64_t x = uptime_ms() * 6364136223846793005ULL + 1442695040888963407ULL;

        for (size_t i = 0; i < n; i++) {
            x = x * 6364136223846793005ULL + 1;
            ((uint8_t *)buf)[i] = (uint8_t)(x >> 33);
        }
    }
    if (fd >= 0)
        close(fd);
}

static void random_guid(uint8_t g[16])
{
    random_fill(g, 16);
    g[7] = (g[7] & 0x0F) | 0x40;    // version 4 (stored little endian)
    g[8] = (g[8] & 0x3F) | 0x80;
}

void guid_to_string(const uint8_t g[16], char out[37])
{
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static void guid_from_string(const char *s, uint8_t g[16])
{
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int k = 0;

    for (const char *p = s; *p && k < 16; p++) {
        if (*p == '-')
            continue;
        g[order[k++]] = (uint8_t)strtol((char[]){ p[0], p[1], 0 }, NULL, 16);
        p++;
    }
}

static int open_dev(const char *name, int flags)
{
    char path[64];

    snprintf(path, sizeof(path), "/osystem/devices/%s", name);
    return open(path, flags);
}

// Writes len bytes (a multiple of 512) at byte offset off.
static int put(int fd, uint64_t off, const void *buf, size_t len)
{
    if (lseek(fd, (int64_t)off, SEEK_SET) < 0)
        return -1;
    return write(fd, buf, len) == (ssize_t)len ? 0 : -1;
}

// Writes zeros over [off, off + len).
static int zero(int fd, uint64_t off, uint64_t len)
{
    static uint8_t zeros[64 * 1024];

    if (lseek(fd, (int64_t)off, SEEK_SET) < 0)
        return -1;
    while (len) {
        size_t n = len > sizeof(zeros) ? sizeof(zeros) : len;

        if (write(fd, zeros, n) != (ssize_t)n)
            return -1;
        len -= n;
    }
    return 0;
}

// ---- Listing ----

int disk_list(struct disk_info *out, int max)
{
    struct dir_stream *d = opendir("/osystem/devices");
    struct aegis_dirent *de;
    int n = 0;

    if (!d)
        return -1;
    while ((de = readdir(d)) && n < max) {
        struct aegis_blockinfo bi;
        int fd;

        if (de->type != DT_BLK || (fd = open_dev(de->name, O_RDONLY)) < 0)
            continue;
        if (ioctl(fd, IOCTL_BLOCK_INFO, (uint64_t)&bi) == 0 && !(bi.flags & (BLOCK_INFO_PARTITION | BLOCK_INFO_RAMDISK))
            && bi.sector_count) {
            struct disk_info *di = &out[n++];

            memset(di, 0, sizeof(*di));
            strlcpy(di->name, de->name, sizeof(di->name));
            di->size = bi.sector_count * bi.sector_size;
            di->sector_size = bi.sector_size;
            di->mounted = bi.flags & BLOCK_INFO_MOUNTED;
            // A short description from the name the driver gave it.
            snprintf(di->description, sizeof(di->description), "%s disk",
                     !strncmp(de->name, "vd", 2) ? "Virtual (virtio)" : !strncmp(de->name, "nvme", 4) ? "NVMe"
                     : !strncmp(de->name, "sata", 4) ? "SATA" : "Storage");
        }
        close(fd);
    }
    closedir(d);
    return n;
}

// ---- GPT ----

struct gpt_header {
    char signature[8];
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t current_lba, backup_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t type_guid[16], part_guid[16];
    uint64_t first_lba, last_lba, attributes;
    uint16_t name[36];
} __attribute__((packed));

#define GPT_ENTRIES     128
#define ESP_TYPE        "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
#define LINUX_FS_TYPE   "0fc63daf-8483-4772-8e79-3d69d8477de4"

static void set_name(struct gpt_entry *e, const char *name)
{
    for (int i = 0; name[i] && i < 35; i++)
        e->name[i] = (uint8_t)name[i];
}

int disk_write_gpt(const char *disk, const struct disk_part *parts, int nparts, char guids[][37])
{
    int fd = open_dev(disk, O_RDWR);
    struct aegis_blockinfo bi;
    uint8_t mbr[512] = { 0 }, hdr_sec[512], disk_guid[16];
    struct gpt_entry *ent = NULL;
    struct gpt_header h;
    uint64_t sectors, lba = 2048, entries_bytes = GPT_ENTRIES * sizeof(struct gpt_entry);
    int ret = -1;

    if (fd < 0)
        return -1;
    if (ioctl(fd, IOCTL_BLOCK_INFO, (uint64_t)&bi) < 0 || bi.sector_size != 512 || nparts > 8)
        goto out;
    sectors = bi.sector_count;
    if (!(ent = calloc(GPT_ENTRIES, sizeof(*ent))))
        goto out;
    for (int i = 0; i < nparts; i++) {
        uint64_t count = parts[i].size_mb ? parts[i].size_mb * 2048
                       : ((sectors - 34) - lba) / 2048 * 2048;          // the rest, whole MiB

        if (lba + count > sectors - 34 || !count) {
            errno = ENOSPC;
            goto out;
        }
        guid_from_string(parts[i].type, ent[i].type_guid);
        random_guid(ent[i].part_guid);
        ent[i].first_lba = lba;
        ent[i].last_lba = lba + count - 1;
        set_name(&ent[i], parts[i].name);
        if (guids)
            guid_to_string(ent[i].part_guid, guids[i]);
        lba += count;
    }
    random_guid(disk_guid);

    // Protective MBR: one partition of type 0xEE over the disk.
    mbr[446 + 4] = 0xEE;
    mbr[446 + 1] = 0x02;
    mbr[446 + 2] = 0x00;
    mbr[446 + 5] = mbr[446 + 6] = mbr[446 + 7] = 0xFF;
    *(uint32_t *)&mbr[446 + 8] = 1;
    *(uint32_t *)&mbr[446 + 12] = sectors - 1 > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)(sectors - 1);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    memset(&h, 0, sizeof(h));
    memcpy(h.signature, "EFI PART", 8);
    h.revision = 0x00010000;
    h.header_size = sizeof(h);
    h.first_usable = 34;
    h.last_usable = sectors - 34;
    memcpy(h.disk_guid, disk_guid, 16);
    h.entry_count = GPT_ENTRIES;
    h.entry_size = sizeof(struct gpt_entry);
    h.entries_crc = crc32(ent, entries_bytes);

    // Backup first, then primary, so a half-written table is never valid.
    h.current_lba = sectors - 1;
    h.backup_lba = 1;
    h.entries_lba = sectors - 33;
    h.header_crc = 0;
    h.header_crc = crc32(&h, sizeof(h));
    memset(hdr_sec, 0, sizeof(hdr_sec));
    memcpy(hdr_sec, &h, sizeof(h));
    if (put(fd, (sectors - 33) * 512, ent, entries_bytes) < 0 || put(fd, (sectors - 1) * 512, hdr_sec, 512) < 0)
        goto out;

    h.current_lba = 1;
    h.backup_lba = sectors - 1;
    h.entries_lba = 2;
    h.header_crc = 0;
    h.header_crc = crc32(&h, sizeof(h));
    memset(hdr_sec, 0, sizeof(hdr_sec));
    memcpy(hdr_sec, &h, sizeof(h));
    if (put(fd, 0, mbr, 512) < 0 || put(fd, 2 * 512, ent, entries_bytes) < 0 || put(fd, 512, hdr_sec, 512) < 0)
        goto out;
    // Old filesystems' signatures at the start of each partition go too.
    for (int i = 0; i < nparts; i++)
        if (zero(fd, ent[i].first_lba * 512, 64 * 1024) < 0)
            goto out;
    ioctl(fd, IOCTL_BLOCK_FLUSH, 0);
    ret = ioctl(fd, IOCTL_BLOCK_RESCAN, 0);
out:
    free(ent);
    close(fd);
    return ret;
}

// ---- FAT32 ----

struct fat_boot {
    uint8_t jump[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fats;
    uint16_t root_entries, total16;
    uint8_t media;
    uint16_t fat16_size, sectors_per_track, heads;
    uint32_t hidden, total32, fat_size;
    uint16_t flags, version;
    uint32_t root_cluster;
    uint16_t fsinfo, backup_boot;
    uint8_t reserved[12];
    uint8_t drive, reserved1, signature;
    uint32_t volume_id;
    char label[11];
    char fstype[8];
} __attribute__((packed));

int mkfs_fat32(const char *part, const char *label)
{
    int fd = open_dev(part, O_RDWR);
    struct aegis_blockinfo bi;
    uint8_t sec[512];
    struct fat_boot *b = (struct fat_boot *)sec;
    uint32_t total, spc, fat_size, clusters, reserved = 32;
    int ret = -1;

    if (fd < 0)
        return -1;
    if (ioctl(fd, IOCTL_BLOCK_INFO, (uint64_t)&bi) < 0)
        goto out;
    total = bi.sector_count > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)bi.sector_count;
    // Small clusters for a small partition, but enough clusters to be FAT32.
    spc = total < 1024 * 1024 ? 1 : total < 16 * 1024 * 1024 ? 8 : 16;
    clusters = total / spc;
    fat_size = (clusters + 2) * 4 / 512 + 1;
    clusters = (total - reserved - 2 * fat_size) / spc;
    if (clusters < 65525) {
        errno = ENOSPC;
        goto out;
    }

    memset(sec, 0, sizeof(sec));
    memcpy(b->jump, "\xEB\x58\x90", 3);
    memcpy(b->oem, "AEGIS   ", 8);
    b->bytes_per_sector = 512;
    b->sectors_per_cluster = spc;
    b->reserved_sectors = reserved;
    b->fats = 2;
    b->media = 0xF8;
    b->sectors_per_track = 63;
    b->heads = 255;
    b->hidden = (uint32_t)bi.start_lba;
    b->total32 = total;
    b->fat_size = fat_size;
    b->root_cluster = 2;
    b->fsinfo = 1;
    b->backup_boot = 6;
    b->drive = 0x80;
    b->signature = 0x29;
    random_fill(&b->volume_id, 4);
    memset(b->label, ' ', 11);
    memcpy(b->label, label, strlen(label) > 11 ? 11 : strlen(label));
    memcpy(b->fstype, "FAT32   ", 8);
    sec[510] = 0x55;
    sec[511] = 0xAA;
    // Reserved area, both FATs and the root directory's cluster start empty.
    if (zero(fd, 0, ((uint64_t)reserved + 2 * fat_size + spc) * 512) < 0)
        goto out;
    if (put(fd, 0, sec, 512) < 0 || put(fd, 6 * 512, sec, 512) < 0)
        goto out;

    memset(sec, 0, sizeof(sec));
    *(uint32_t *)&sec[0] = 0x41615252;
    *(uint32_t *)&sec[484] = 0x61417272;
    *(uint32_t *)&sec[488] = clusters - 1;         // free clusters (root uses one)
    *(uint32_t *)&sec[492] = 3;                    // next free hint
    sec[510] = 0x55;
    sec[511] = 0xAA;
    if (put(fd, 512, sec, 512) < 0 || put(fd, 7 * 512, sec, 512) < 0)
        goto out;

    // FAT entries 0 and 1 are reserved; 2 is the root directory (end of chain).
    memset(sec, 0, sizeof(sec));
    *(uint32_t *)&sec[0] = 0x0FFFFFF8;
    *(uint32_t *)&sec[4] = 0x0FFFFFFF;
    *(uint32_t *)&sec[8] = 0x0FFFFFFF;
    for (int f = 0; f < 2; f++)
        if (put(fd, ((uint64_t)reserved + (uint64_t)f * fat_size) * 512, sec, 512) < 0)
            goto out;
    ioctl(fd, IOCTL_BLOCK_FLUSH, 0);
    ret = 0;
out:
    close(fd);
    return ret;
}

// ---- ext4 ----

#define BLOCK           4096
#define BLOCKS_PER_GROUP 32768
#define INODE_SIZE      256
#define BYTES_PER_INODE (32 * 1024)

struct e4_super {
    uint32_t inodes_count, blocks_count_lo, r_blocks_count_lo, free_blocks_count_lo, free_inodes_count;
    uint32_t first_data_block, log_block_size, log_cluster_size, blocks_per_group, clusters_per_group;
    uint32_t inodes_per_group, mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev_level;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;
    uint16_t inode_size, block_group_nr;
    uint32_t feature_compat, feature_incompat, feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t prealloc_blocks, prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;
    uint8_t journal_uuid[16];
    uint32_t journal_inum, journal_dev, last_orphan, hash_seed[4];
    uint8_t def_hash_version, jnl_backup_type;
    uint16_t desc_size;
    uint32_t default_mount_opts, first_meta_bg, mkfs_time, jnl_blocks[17];
    uint32_t blocks_count_hi, r_blocks_count_hi, free_blocks_count_hi;
    uint16_t min_extra_isize, want_extra_isize;
    uint32_t flags;
    uint8_t pad[1024 - 0x164];
} __attribute__((packed));

struct e4_gd {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, flags;
    uint32_t exclude_bitmap;
    uint16_t block_bitmap_csum, inode_bitmap_csum, itable_unused, checksum;
} __attribute__((packed));

struct e4_inode {
    uint16_t mode, uid;
    uint32_t size_lo, atime, ctime, mtime, dtime;
    uint16_t gid, links_count;
    uint32_t blocks_lo, flags, version;
    uint32_t block[15];
    uint32_t generation, file_acl_lo, size_high, obso_faddr;
    uint16_t blocks_high, file_acl_high, uid_high, gid_high, checksum_lo, reserved;
    uint16_t extra_isize, checksum_hi;
    uint32_t ctime_extra, mtime_extra, atime_extra, crtime, crtime_extra, version_hi, projid;
    uint8_t pad[INODE_SIZE - 160];
} __attribute__((packed));

static void be32_put(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static bool has_backup(uint32_t g)
{
    if (g <= 1)
        return true;
    for (uint32_t p = 3; p <= 7; p += 2) {
        uint32_t x = p;

        while (x < g)
            x *= p;
        if (x == g)
            return true;
    }
    return false;
}

static void set_bit(uint8_t *map, uint32_t bit)
{
    map[bit / 8] |= 1 << (bit % 8);
}

// A directory inode with one data block holding the given entries.
static void dir_inode(struct e4_inode *in, uint32_t block, uint16_t links, uint32_t now)
{
    memset(in, 0, sizeof(*in));
    in->mode = 040755;
    in->links_count = links;
    in->size_lo = BLOCK;
    in->atime = in->ctime = in->mtime = in->crtime = now;
    in->blocks_lo = BLOCK / 512;
    in->flags = 0x00080000;         // extents
    in->block[0] = 0xF30A | (1u << 16);     // extent header: magic, 1 entry
    in->block[1] = 4;                        // max 4, depth 0
    in->block[2] = 0;
    in->block[3] = 0;                        // extent: logical block 0
    in->block[4] = 1;                        // length 1, start_hi 0
    in->block[5] = block;                    // start_lo
    in->extra_isize = 32;
}

static int dirent(uint8_t *buf, int off, uint32_t ino, uint16_t rec_len, const char *name, uint8_t type)
{
    size_t n = strlen(name);

    *(uint32_t *)&buf[off] = ino;
    *(uint16_t *)&buf[off + 4] = rec_len;
    buf[off + 6] = (uint8_t)n;
    buf[off + 7] = type;
    memcpy(&buf[off + 8], name, n);
    return off + rec_len;
}

int mkfs_ext4(const char *part, const char *label, void (*progress)(int percent, void *u), void *u)
{
    int fd = open_dev(part, O_RDWR);
    struct aegis_blockinfo bi;
    uint64_t blocks;
    uint32_t groups, ipg, itable_blocks, gdt_blocks, now = (uint32_t)time(NULL);
    struct e4_gd *gd = NULL;
    uint8_t *buf = NULL;
    struct e4_super sb;
    uint64_t free_total = 0;
    uint32_t root_block = 0, lf_block = 0, journal_block = 0, journal_len;
    int ret = -1;

    if (fd < 0)
        return -1;
    if (ioctl(fd, IOCTL_BLOCK_INFO, (uint64_t)&bi) < 0)
        goto out;
    blocks = bi.sector_count * bi.sector_size / BLOCK;
    if (blocks > 0xFFFFFFFFULL)
        blocks = 0xFFFFFFFFULL;     // 16 TiB: enough without the 64bit feature
    groups = (uint32_t)((blocks + BLOCKS_PER_GROUP - 1) / BLOCKS_PER_GROUP);
    gdt_blocks = (groups * 32 + BLOCK - 1) / BLOCK;
    ipg = (uint32_t)((uint64_t)BLOCKS_PER_GROUP * BLOCK / BYTES_PER_INODE);
    itable_blocks = ipg * INODE_SIZE / BLOCK;
    // A last group too small for its own metadata is left off.
    if (groups > 1 && blocks - (uint64_t)(groups - 1) * BLOCKS_PER_GROUP < 1 + gdt_blocks + 2 + itable_blocks + 16) {
        groups--;
        blocks = (uint64_t)groups * BLOCKS_PER_GROUP;
        gdt_blocks = (groups * 32 + BLOCK - 1) / BLOCK;
    }
    if (blocks < 1024) {
        errno = ENOSPC;
        goto out;
    }
    if (!(gd = calloc(groups, sizeof(*gd))) || !(buf = malloc(BLOCK)))
        goto out;

    memset(&sb, 0, sizeof(sb));
    sb.inodes_count = groups * ipg;
    sb.blocks_count_lo = (uint32_t)blocks;
    sb.r_blocks_count_lo = (uint32_t)(blocks / 50);         // 2% for root
    sb.first_data_block = 0;
    sb.log_block_size = sb.log_cluster_size = 2;            // 4096
    sb.blocks_per_group = sb.clusters_per_group = BLOCKS_PER_GROUP;
    sb.inodes_per_group = ipg;
    sb.mtime = 0;
    sb.wtime = sb.mkfs_time = sb.lastcheck = now;
    sb.max_mnt_count = 0xFFFF;
    sb.magic = 0xEF53;
    sb.state = 1;
    sb.errors = 1;
    sb.rev_level = 1;
    sb.first_ino = 11;
    sb.inode_size = INODE_SIZE;
    sb.feature_incompat = 0x0002 | 0x0040;                  // filetype, extents
    // A journal (inode 8), so a crash or power cut cannot leave the file
    // system half-updated: 1/64 of the disk, from 16 MiB to 64 MiB, in
    // the first block group.
    journal_len = (uint32_t)(blocks / 64);
    journal_len = journal_len < 4096 ? 4096 : journal_len > 16384 ? 16384 : journal_len;
    if (journal_len > blocks / 4)
        journal_len = 1024;
    sb.feature_compat = 0x0004;                             // has_journal
    sb.journal_inum = 8;
    sb.feature_ro_compat = 0x0001 | 0x0002 | 0x0020 | 0x0040; // sparse_super, large_file, dir_nlink, extra_isize
    random_fill(sb.uuid, 16);
    random_fill(sb.hash_seed, sizeof(sb.hash_seed));
    strlcpy(sb.volume_name, label, sizeof(sb.volume_name));
    sb.def_hash_version = 1;                                // half_md4
    sb.desc_size = 32;
    sb.min_extra_isize = sb.want_extra_isize = 32;

    // Lay out each group and write its bitmaps and (zeroed) inode table.
    for (uint32_t g = 0; g < groups; g++) {
        uint64_t start = (uint64_t)g * BLOCKS_PER_GROUP;
        uint32_t in_group = (uint32_t)(blocks - start < BLOCKS_PER_GROUP ? blocks - start : BLOCKS_PER_GROUP);
        uint32_t meta = has_backup(g) ? 1 + gdt_blocks : 0, used;

        gd[g].block_bitmap = (uint32_t)start + meta;
        gd[g].inode_bitmap = (uint32_t)start + meta + 1;
        gd[g].inode_table = (uint32_t)start + meta + 2;
        used = meta + 2 + itable_blocks;
        if (g == 0) {
            root_block = (uint32_t)start + used++;
            lf_block = (uint32_t)start + used++;
            journal_block = (uint32_t)start + used;
            used += journal_len;
            if (used >= in_group) {
                errno = ENOSPC;
                goto out;
            }
            gd[g].used_dirs = 2;
            gd[g].free_inodes = ipg - 11;
        } else {
            gd[g].free_inodes = ipg;
        }
        gd[g].free_blocks = in_group - used;
        free_total += in_group - used;

        memset(buf, 0, BLOCK);
        for (uint32_t b = 0; b < used; b++)
            set_bit(buf, b);
        for (uint32_t b = in_group; b < BLOCKS_PER_GROUP; b++)
            set_bit(buf, b);
        if (put(fd, (uint64_t)gd[g].block_bitmap * BLOCK, buf, BLOCK) < 0)
            goto out;
        memset(buf, 0, BLOCK);
        if (g == 0)
            for (uint32_t i = 0; i < 11; i++)
                set_bit(buf, i);
        for (uint32_t i = ipg; i < BLOCK * 8; i++)
            set_bit(buf, i);
        if (put(fd, (uint64_t)gd[g].inode_bitmap * BLOCK, buf, BLOCK) < 0
            || zero(fd, (uint64_t)gd[g].inode_table * BLOCK, (uint64_t)itable_blocks * BLOCK) < 0)
            goto out;
        if (progress)
            progress((int)((uint64_t)(g + 1) * 90 / groups), u);
    }
    sb.free_blocks_count_lo = (uint32_t)free_total;
    sb.free_inodes_count = groups * ipg - 11;

    // The root directory (inode 2) and lost+found (inode 11).
    {
        struct e4_inode in;
        uint8_t *itab = calloc(1, BLOCK * 3);

        if (!itab)
            goto out;
        dir_inode(&in, root_block, 3, now);
        memcpy(itab + (2 - 1) * INODE_SIZE, &in, INODE_SIZE);
        dir_inode(&in, lf_block, 2, now);
        in.mode = 040700;
        memcpy(itab + (11 - 1) * INODE_SIZE, &in, INODE_SIZE);
        // The journal: one extent over its blocks.
        memset(&in, 0, sizeof(in));
        in.mode = 0100600;
        in.links_count = 1;
        in.size_lo = journal_len * BLOCK;
        in.atime = in.ctime = in.mtime = in.crtime = now;
        in.blocks_lo = journal_len * (BLOCK / 512);
        in.flags = 0x00080000;
        in.block[0] = 0xF30A | (1u << 16);
        in.block[1] = 4;
        in.block[3] = 0;
        in.block[4] = journal_len;              // length (high 16 bits: start_hi 0)
        in.block[5] = journal_block;
        in.extra_isize = 32;
        memcpy(itab + (8 - 1) * INODE_SIZE, &in, INODE_SIZE);
        // The superblock keeps a copy of where the journal is.
        memcpy(sb.jnl_blocks, in.block, sizeof(in.block));
        sb.jnl_blocks[16] = in.size_lo;
        sb.jnl_backup_type = 1;
        if (put(fd, (uint64_t)gd[0].inode_table * BLOCK, itab, BLOCK * 3) < 0) {
            free(itab);
            goto out;
        }
        free(itab);

        memset(buf, 0, BLOCK);
        dirent(buf, dirent(buf, dirent(buf, 0, 2, 12, ".", 2), 2, 12, "..", 2), 11, BLOCK - 24, "lost+found", 2);
        if (put(fd, (uint64_t)root_block * BLOCK, buf, BLOCK) < 0)
            goto out;
        memset(buf, 0, BLOCK);
        dirent(buf, dirent(buf, 0, 11, 12, ".", 2), 2, BLOCK - 12, "..", 2);
        if (put(fd, (uint64_t)lf_block * BLOCK, buf, BLOCK) < 0)
            goto out;

        // An empty journal: zeroed, with a clean JBD2 v2 superblock first.
        if (zero(fd, (uint64_t)journal_block * BLOCK, (uint64_t)journal_len * BLOCK) < 0)
            goto out;
        memset(buf, 0, BLOCK);
        be32_put(buf + 0x00, 0xC03B3998);       // magic
        be32_put(buf + 0x04, 4);                // superblock, version 2
        be32_put(buf + 0x0C, BLOCK);
        be32_put(buf + 0x10, journal_len);      // maxlen
        be32_put(buf + 0x14, 1);                // first log block
        be32_put(buf + 0x18, 1);                // first transaction's sequence
        be32_put(buf + 0x1C, 0);                // start: 0 = nothing to replay
        be32_put(buf + 0x28, 0x1);              // incompat: revoke records
        memcpy(buf + 0x30, sb.uuid, 16);
        be32_put(buf + 0x40, 1);                // one file system uses it
        memcpy(buf + 0x100, sb.uuid, 16);
        if (put(fd, (uint64_t)journal_block * BLOCK, buf, BLOCK) < 0)
            goto out;
    }

    // Superblock and descriptors in group 0 and the backups.
    for (uint32_t g = 0; g < groups; g++) {
        uint64_t start = (uint64_t)g * BLOCKS_PER_GROUP * BLOCK;

        if (!has_backup(g))
            continue;
        sb.block_group_nr = (uint16_t)g;
        memset(buf, 0, BLOCK);
        memcpy(buf + (g == 0 ? 1024 : 0), &sb, sizeof(sb));
        if (put(fd, start, buf, BLOCK) < 0)
            goto out;
        for (uint32_t k = 0; k < gdt_blocks; k++) {
            uint32_t per = BLOCK / 32, n = groups - k * per < per ? groups - k * per : per;

            memset(buf, 0, BLOCK);
            memcpy(buf, &gd[k * per], n * 32);
            if (put(fd, start + (uint64_t)(1 + k) * BLOCK, buf, BLOCK) < 0)
                goto out;
        }
    }
    ioctl(fd, IOCTL_BLOCK_FLUSH, 0);
    if (progress)
        progress(100, u);
    ret = 0;
out:
    free(gd);
    free(buf);
    close(fd);
    return ret;
}
