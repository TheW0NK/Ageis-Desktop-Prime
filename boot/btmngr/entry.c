// Boot manager. Shows the boot menu and hands off to the bootloader (btloader.efi).

#include <efi.h>
#include <efilib.h>

#include "bcd.h"

static struct bcd bcd;

static void draw_menu(UINTN selected, UINTN remaining, BOOLEAN counting)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out = ST->ConOut;

    uefi_call_wrapper(out->SetCursorPosition, 3, out, 0, 0);
    uefi_call_wrapper(out->SetAttribute, 2, out, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
    Print(L"Aegis Boot Manager\n\n");

    for (UINTN i = 0; i < bcd.count; i++) {
        if (i == selected)
            uefi_call_wrapper(out->SetAttribute, 2, out, EFI_BLACK | EFI_BACKGROUND_LIGHTGRAY);
        Print(L"  %-60s\n", bcd.entries[i].title);
        uefi_call_wrapper(out->SetAttribute, 2, out, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
    }

    Print(L"\nUp/Down to choose, Enter to boot.\n");
    if (counting)
        Print(L"Booting the selected entry in %d s.   \n", remaining);
    else
        Print(L"                                      \n");
}

static struct bcd_entry *boot_menu(void)
{
    UINTN selected = bcd.default_index, remaining = bcd.timeout, index;
    BOOLEAN counting = TRUE;
    EFI_EVENT events[2];
    EFI_INPUT_KEY key;

    if (bcd.timeout == 0)
        return &bcd.entries[selected];

    // The menu can wait indefinitely, so the firmware watchdog must not fire.
    uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);
    uefi_call_wrapper(BS->CreateEvent, 5, EVT_TIMER, 0, NULL, NULL, &events[1]);
    uefi_call_wrapper(BS->SetTimer, 3, events[1], TimerPeriodic, 10000000);
    events[0] = ST->ConIn->WaitForKey;

    for (;;) {
        draw_menu(selected, remaining, counting);
        uefi_call_wrapper(BS->WaitForEvent, 3, 2, events, &index);

        if (index == 1) {
            if (counting && --remaining == 0)
                break;
            continue;
        }

        if (EFI_ERROR(uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key)))
            continue;
        counting = FALSE;

        if (key.ScanCode == SCAN_UP)
            selected = (selected + bcd.count - 1) % bcd.count;
        else if (key.ScanCode == SCAN_DOWN)
            selected = (selected + 1) % bcd.count;
        else if (key.UnicodeChar == CHAR_CARRIAGE_RETURN)
            break;
        else if (key.UnicodeChar >= L'1' && key.UnicodeChar < L'1' + bcd.count)
            selected = key.UnicodeChar - L'1';
    }

    uefi_call_wrapper(BS->CloseEvent, 1, events[1]);
    uefi_call_wrapper(BS->SetWatchdogTimer, 4, 300, 0, 0, NULL);
    Print(L"\n");
    return &bcd.entries[selected];
}

// Starts btloader, passing the chosen entry's id as its load options.
static EFI_STATUS handoff(EFI_HANDLE image, EFI_LOADED_IMAGE *self, struct bcd_entry *entry)
{
    EFI_DEVICE_PATH *path;
    EFI_LOADED_IMAGE *loader_image;
    EFI_HANDLE loader;
    EFI_STATUS status;
    Print(L"Init Bootloader Handoff...");
    path = FileDevicePath(self->DeviceHandle, bcd.loader);
    if (!path)
        return EFI_OUT_OF_RESOURCES;

    status = uefi_call_wrapper(BS->LoadImage, 6, FALSE, image, path,
                               NULL, 0, &loader);
    FreePool(path);
    if (EFI_ERROR(status))
        return status;

    status = uefi_call_wrapper(BS->HandleProtocol, 3, loader,
                               &LoadedImageProtocol, (void **)&loader_image);
    if (EFI_ERROR(status))
        return status;
    loader_image->LoadOptions = entry->id;
    loader_image->LoadOptionsSize = (StrLen(entry->id) + 1) * sizeof(CHAR16);

    status = uefi_call_wrapper(BS->StartImage, 3, loader, NULL, NULL);

    // The bootloader should never return; if it does, treat it as a failure.
    return EFI_ERROR(status) ? status : EFI_ABORTED;
}

static void __attribute__((noreturn)) halt(void)
{
    // Stop the firmware watchdog from rebooting the machine while halted.
    uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    for (;;)
        __asm__ volatile ("cli; hlt");
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_LOADED_IMAGE *self;
    struct bcd_entry *entry;
    UINTN line;
    EFI_STATUS status;

    InitializeLib(image, systab);
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    status = uefi_call_wrapper(BS->HandleProtocol, 3, image,
                               &LoadedImageProtocol, (void **)&self);
    if (EFI_ERROR(status)) {
        Print(L"Error: cannot access the loaded image: %r\nSystem halted.\n", status);
        halt();
    }

    status = bcd_load(self->DeviceHandle, &bcd, &line);
    if (EFI_ERROR(status)) {
        if (line)
            Print(L"Error: syntax error in %s at line %d\n", BCD_PATH, line);
        else
            Print(L"Error: cannot load %s: %r\n", BCD_PATH, status);
        Print(L"System halted.\n");
        halt();
    }

    entry = boot_menu();

    Print(L"Loading %s...\n", entry->title);
    status = handoff(image, self, entry);
    Print(L"----- HALT AND CATCH FIRE ASSHOLE! -----");
    Print(L"\nError: failed to start %s (%s): %r\n", bcd.loader, entry->title, status);
    Print(L"System halted.\n");
    halt();
}
