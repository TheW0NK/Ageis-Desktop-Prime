#include "block.h"
#include "mem.h"
#include "string.h"

struct gpt_header {
    char signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t type_guid[16];
    uint8_t part_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} __attribute__((packed));

void guid_format(const uint8_t g[16], char out[37])
{
    ksnprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
              g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool guid_parse(const char *s, uint8_t g[16])
{
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int n = 0;

    for (; *s && n < 16; s++) {
        int hi, lo;

        if (*s == '-')
            continue;
        if ((hi = hexval(s[0])) < 0 || (lo = hexval(s[1])) < 0)
            return false;
        g[order[n++]] = hi << 4 | lo;
        s++;
    }
    return n == 16;
}

void partition_scan(struct block_device *disk)
{
    uint32_t ss = disk->sector_size;
    uint8_t *sector = kmalloc(ss);
    struct gpt_header hdr;
    uint8_t *entries = NULL;
    size_t entries_size;
    int index = 1;

    if (!sector)
        return;
    // Scanning again (a new partition table): old partitions vanish until
    // found again, and ones found again keep their device.
    for (size_t i = 0; i < block_count(); i++) {
        if (block_at(i)->parent == disk)
            block_at(i)->sector_count = 0;
    }
    if (block_read(disk, ss, sector, ss) != 0)
        goto out;
    memcpy(&hdr, sector, sizeof(hdr));
    if (memcmp(hdr.signature, "EFI PART", 8) != 0 || hdr.entry_size < sizeof(struct gpt_entry)
        || hdr.entry_count > 1024)
        goto out;

    entries_size = ALIGN_UP((uint64_t)hdr.entry_count * hdr.entry_size, ss);
    entries = kmalloc(entries_size);
    if (!entries || block_read(disk, hdr.entries_lba * ss, entries, entries_size) != 0)
        goto out;

    for (uint32_t i = 0; i < hdr.entry_count; i++) {
        struct gpt_entry *e = (struct gpt_entry *)(entries + (uint64_t)i * hdr.entry_size);
        struct block_device *part;
        static const uint8_t zero[16];

        if (!memcmp(e->type_guid, zero, 16) || e->last_lba < e->first_lba
            || e->last_lba >= disk->sector_count)
            continue;
        {
            char name[BLOCK_NAME_MAX];
            struct block_device *old;

            ksnprintf(name, sizeof(name), "%sp%d", disk->name, index++);
            if ((old = block_find(name)) && old->parent == disk) {
                old->sector_count = e->last_lba - e->first_lba + 1;
                old->start_lba = e->first_lba;
                memcpy(old->type_guid, e->type_guid, 16);
                memcpy(old->part_guid, e->part_guid, 16);
                continue;
            }
        }
        if (!(part = kzalloc(sizeof(*part))))
            break;

        ksnprintf(part->name, sizeof(part->name), "%sp%d", disk->name, index - 1);
        part->sector_size = ss;
        part->sector_count = e->last_lba - e->first_lba + 1;
        part->max_sectors = disk->max_sectors;
        part->ops = disk->ops;
        part->parent = disk;
        part->start_lba = e->first_lba;
        memcpy(part->type_guid, e->type_guid, 16);
        memcpy(part->part_guid, e->part_guid, 16);
        block_register(part);
    }

out:
    kfree(entries);
    kfree(sector);
}
