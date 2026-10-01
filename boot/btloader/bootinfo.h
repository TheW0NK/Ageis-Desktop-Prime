#ifndef AEGIS_BOOTINFO_H
#define AEGIS_BOOTINFO_H

#include <stdint.h>

#define AEGIS_BOOT_MAGIC    0x4F4F425349474541ULL   // "AEGISBOO"
#define AEGIS_BOOT_VERSION  2

#define AEGIS_MAX_FRAMEBUFFERS 4

enum aegis_memory_type {
    AEGIS_MEM_USABLE = 1,
    AEGIS_MEM_RESERVED,
    AEGIS_MEM_ACPI_RECLAIMABLE,
    AEGIS_MEM_ACPI_NVS,
    AEGIS_MEM_BAD,
    AEGIS_MEM_BOOTLOADER_RECLAIMABLE, // boot info, stack, bootloader page tables
    AEGIS_MEM_KERNEL,               // the kernel's loaded segments
    AEGIS_MEM_FIRMWARE_RUNTIME,     // UEFI runtime services code/data
};

struct aegis_memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;                  // enum aegis_memory_type
    uint32_t reserved;
};

enum aegis_pixel_format {
    AEGIS_FB_NONE = 0,              // no linear framebuffer available
    AEGIS_FB_RGBX,                  // byte order R, G, B, reserved
    AEGIS_FB_BGRX,                  // byte order B, G, R, reserved
    AEGIS_FB_BITMASK,               // see the *_mask fields
};

struct aegis_framebuffer {
    uint64_t base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t format;                // enum aegis_pixel_format
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
};

struct aegis_boot_info {
    uint64_t magic;                 // AEGIS_BOOT_MAGIC
    uint32_t version;               // AEGIS_BOOT_VERSION
    uint32_t size;                  // sizeof(struct aegis_boot_info)

    uint64_t memory_map;            // struct aegis_memory_region[], sorted by base
    uint64_t memory_map_entries;

    uint32_t framebuffer_count;
    uint32_t reserved1;
    struct aegis_framebuffer framebuffers[AEGIS_MAX_FRAMEBUFFERS]; // [0] is the primary display

    uint64_t acpi_rsdp;             // 0 if not found
    uint32_t acpi_revision;         // 1 = ACPI 1.0 RSDP, 2 = ACPI 2.0+ XSDP
    uint32_t reserved0;

    uint64_t cmdline;               // NUL-terminated ASCII, never 0
    uint64_t kernel_physical_base;
    uint64_t kernel_virtual_base;   // equals kernel_physical_base for identity-mapped kernels
    uint64_t kernel_size;

    uint64_t efi_system_table;      // for runtime services; boot services are gone
};

#endif
