#ifndef AEGIS_BTLOADER_H
#define AEGIS_BTLOADER_H

#include <efi.h>
#include <efilib.h>

#include "bcd.h"

extern EFI_HANDLE btl_image;
extern EFI_LOADED_IMAGE *btl_self;

EFI_STATUS btl_read_file(const CHAR16 *path, void **data, UINTN *size);

void __attribute__((noreturn)) btl_fatal(EFI_STATUS status, const CHAR16 *fmt, ...);
void __attribute__((noreturn)) btl_halt(void);

void diag_report(void);

EFI_STATUS handoff_kernel(struct bcd *bcd, struct bcd_entry *entry);
EFI_STATUS handoff_efi(struct bcd_entry *entry);

EFI_STATUS recovery_console(void);

#endif
