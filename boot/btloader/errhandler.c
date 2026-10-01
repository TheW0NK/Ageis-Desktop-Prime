#include <efi.h>
#include <efilib.h>

#include "btloader.h"

void btl_halt(void)
{
    uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    for (;;)
        __asm__ volatile ("cli; hlt");
}

void btl_fatal(EFI_STATUS status, const CHAR16 *fmt, ...)
{
    va_list args;

    Print(L"\nbtloader error: ");
    va_start(args, fmt);
    VPrint(fmt, args);
    va_end(args);

    if (EFI_ERROR(status))
        Print(L": %r", status);

    Print(L"\nSystem halted.\n");
    btl_halt();
}
