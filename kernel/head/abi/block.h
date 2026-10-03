#ifndef AEGIS_ABI_BLOCK_H
#define AEGIS_ABI_BLOCK_H

#include <stdint.h>

// /dev/<disk> and /dev/<disk>p<N>: disks and their partitions, for root.
// read() and write() work at the file offset (lseek) and must be whole
// sectors at sector-aligned offsets.

#define IOCTL_BLOCK_INFO    0x500   // arg: struct aegis_blockinfo *
#define IOCTL_BLOCK_RESCAN  0x501   // whole disks: read the partition table again
#define IOCTL_BLOCK_FLUSH   0x502

#define BLOCK_INFO_PARTITION    1   // a partition of another device
#define BLOCK_INFO_RAMDISK      2   // memory, not a disk (the live system)
#define BLOCK_INFO_MOUNTED      4   // it, or one of its partitions, is mounted

struct aegis_blockinfo {
    uint64_t sector_count;
    uint32_t sector_size;
    uint32_t flags;                 // BLOCK_INFO_*
    uint64_t start_lba;             // partitions: where on the parent
    char parent[16];                // partitions: the whole disk's name
    uint8_t type_guid[16];
    uint8_t part_guid[16];
};

#endif
