#include <efi.h>
#include <efilib.h>

#include "btloader.h"

EFI_HANDLE btl_image;
EFI_LOADED_IMAGE *btl_self;

static struct bcd bcd;

EFI_STATUS btl_read_file(const CHAR16 *path, void **data, UINTN *size)
{
    EFI_FILE_HANDLE root, file;
    EFI_FILE_INFO *info;
    EFI_STATUS status;
    Print(L"Prismonian Aegis 0.0.0 Build 000aaa000\n");
    Print(L"Bootloader handoff successful!\n");
    root = LibOpenRoot(btl_self->DeviceHandle);
    if (!root)
        return EFI_NOT_FOUND;

    status = uefi_call_wrapper(root->Open, 5, root, &file, (CHAR16 *)path,
                               EFI_FILE_MODE_READ, 0);
    uefi_call_wrapper(root->Close, 1, root);
    if (EFI_ERROR(status))
        return status;

    info = LibFileInfo(file);
    if (!info) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_DEVICE_ERROR;
    }
    *size = info->FileSize;
    FreePool(info);

    *data = AllocatePool(*size ? *size : 1);
    if (!*data) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_OUT_OF_RESOURCES;
    }

    status = uefi_call_wrapper(file->Read, 3, file, size, *data);
    uefi_call_wrapper(file->Close, 1, file);
    if (EFI_ERROR(status))
        FreePool(*data);
    return status;
}

static CHAR16 *requested_entry(void)
{
    static CHAR16 id[BCD_ID_MAX];
    CHAR16 *options = btl_self->LoadOptions;
    UINTN len = btl_self->LoadOptionsSize / sizeof(CHAR16);
    UINTN i;

    if (!options || len == 0)
        return NULL;

    for (i = 0; i < len && i < BCD_ID_MAX - 1 && options[i]; i++)
        id[i] = options[i];
    id[i] = 0;
    return i ? id : NULL;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    struct bcd_entry *entry;
    CHAR16 *id;
    UINTN line;
    EFI_STATUS status;

    InitializeLib(image, systab);
    btl_image = image;

    status = uefi_call_wrapper(BS->HandleProtocol, 3, image,
                               &LoadedImageProtocol, (void **)&btl_self);
    if (EFI_ERROR(status))
        btl_fatal(status, L"Cannot access the loaded image");

    status = bcd_load(btl_self->DeviceHandle, &bcd, &line);
    if (line)
        btl_fatal(status, L"Syntax error in %s at line %d", BCD_PATH, line);
    if (EFI_ERROR(status))
        btl_fatal(status, L"Cannot load %s", BCD_PATH);

    id = requested_entry();
    entry = id ? bcd_find(&bcd, id) : &bcd.entries[bcd.default_index];
    if (!entry)
        btl_fatal(EFI_NOT_FOUND, L"Unknown boot entry '%s'", id);

    if (bcd.diagnostics)
        diag_report();

    Print(L"Booting %s...\n", entry->title);

    switch (entry->type) {
    case BCD_TYPE_KERNEL:
        status = handoff_kernel(&bcd, entry);
        break;
    case BCD_TYPE_EFI:
        status = handoff_efi(entry);
        break;
    default:
        status = EFI_UNSUPPORTED;
        break;
    }

    btl_fatal(status, L"Failed to boot %s (%s)", entry->title, entry->path);
}
