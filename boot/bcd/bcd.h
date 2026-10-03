// Boot Configuration Data. Shared by btmngr and btloader.
//
// The BCD is a text file at BCD_PATH on the EFI system partition:
//
//   # Global settings come before the first section.
//   timeout=5
//   default=aegis
//   loader=\EFI\Aegis\btloader.efi
//   diagnostics=false
//   resolution=auto                  # auto (firmware mode), max, or WIDTHxHEIGHT
//
//   # Each section is one boot entry; the name in brackets is its id.
//   [aegis]
//   title=Aegis
//   type=kernel                      # kernel (ELF64) or efi (UEFI app)
//   path=\EFI\Aegis\kernel.elf
//   cmdline=quiet
//   ramdisk=\EFI\Aegis\live.img     # optional: loaded into memory for the kernel
//
// Blank lines and lines starting with '#' or ';' are ignored. Unknown keys
// are errors, so typos are caught instead of silently ignored.

#ifndef AEGIS_BCD_H
#define AEGIS_BCD_H

#include <efi.h>

#define BCD_PATH            L"\\EFI\\Aegis\\bcd"
#define BCD_DEFAULT_LOADER  L"\\EFI\\Aegis\\btloader.efi"

#define BCD_MAX_ENTRIES     16
#define BCD_ID_MAX          32
#define BCD_TITLE_MAX       64
#define BCD_PATH_MAX        256
#define BCD_CMDLINE_MAX     512

enum bcd_entry_type {
    BCD_TYPE_NONE,
    BCD_TYPE_KERNEL,                // ELF64 kernel started with an aegis_boot_info
    BCD_TYPE_EFI,                   // UEFI application, chainloaded
};

struct bcd_entry {
    CHAR16 id[BCD_ID_MAX];
    CHAR16 title[BCD_TITLE_MAX];
    enum bcd_entry_type type;
    CHAR16 path[BCD_PATH_MAX];
    CHAR8 cmdline[BCD_CMDLINE_MAX];
    CHAR16 ramdisk[BCD_PATH_MAX];   // empty: none
};

struct bcd {
    UINTN timeout;                  // seconds
    UINTN default_index;
    BOOLEAN diagnostics;
    BOOLEAN resolution_max;
    UINT32 resolution_width;        // 0 = keep the firmware's mode
    UINT32 resolution_height;
    CHAR16 loader[BCD_PATH_MAX];
    UINTN count;
    struct bcd_entry entries[BCD_MAX_ENTRIES];
};

// Reads and parses the BCD from the root of `device`. On a syntax error,
// returns EFI_INVALID_PARAMETER and sets *error_line (1-based).
EFI_STATUS bcd_load(EFI_HANDLE device, struct bcd *bcd, UINTN *error_line);

// Parses BCD text that is already in memory.
EFI_STATUS bcd_parse(const CHAR8 *text, UINTN size, struct bcd *bcd, UINTN *error_line);

// Returns the entry with the given id, or NULL.
struct bcd_entry *bcd_find(struct bcd *bcd, const CHAR16 *id);

#endif
