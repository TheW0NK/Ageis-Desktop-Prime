#include <efi.h>
#include <efilib.h>
#include <cpuid.h>

#include "btloader.h"

static void report_memory(void)
{
    EFI_MEMORY_DESCRIPTOR *map, *d;
    UINTN count, key, desc_size;
    UINT32 desc_version;
    UINT64 usable = 0, total = 0;

    map = LibMemoryMap(&count, &key, &desc_size, &desc_version);
    if (!map) {
        Print(L"  Memory:   unavailable\n");
        return;
    }

    d = map;
    for (UINTN i = 0; i < count; i++) {
        UINT64 bytes = d->NumberOfPages * 4096;

        switch (d->Type) {
        case EfiConventionalMemory:
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
            usable += bytes;
            total += bytes;
            break;
        case EfiReservedMemoryType:
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace:
            break;
        default:
            total += bytes;
            break;
        }
        d = NextMemoryDescriptor(d, desc_size);
    }
    FreePool(map);

    Print(L"  Memory:   %ld MiB usable of %ld MiB\n", usable >> 20, total >> 20);
}

static void report_cpu(void)
{
    UINT32 a = 0, b = 0, c = 0, d = 0, max_ext;
    UINT32 vendor[4] = { 0 };
    UINT32 brand[13] = { 0 };

    __get_cpuid(0, &a, &vendor[0], &vendor[2], &vendor[1]);
    Print(L"  CPU:      %a", (CHAR8 *)vendor);

    max_ext = __get_cpuid_max(0x80000000, NULL);
    if (max_ext >= 0x80000004) {
        for (UINT32 i = 0; i < 3; i++)
            __get_cpuid(0x80000002 + i, &brand[i * 4], &brand[i * 4 + 1],
                        &brand[i * 4 + 2], &brand[i * 4 + 3]);
        Print(L", %a", (CHAR8 *)brand);
    }
    Print(L"\n");

    __get_cpuid(1, &a, &b, &c, &d);
    Print(L"  Features:");
    if (d & (1u << 9))  Print(L" APIC");
    if (c & (1u << 21)) Print(L" x2APIC");
    if (d & bit_SSE2)   Print(L" SSE2");
    if (c & bit_SSE4_2) Print(L" SSE4.2");
    if (c & bit_AVX)    Print(L" AVX");
    if (max_ext >= 0x80000001) {
        __get_cpuid(0x80000001, &a, &b, &c, &d);
        if (d & bit_LM) Print(L" LM");
        if (d & (1u << 20)) Print(L" NX");
    }
    Print(L"\n");
}

static void report_graphics(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

    if (EFI_ERROR(LibLocateProtocol(&GraphicsOutputProtocol, (void **)&gop))) {
        Print(L"  Graphics: no GOP device\n");
        return;
    }

    info = gop->Mode->Info;
    Print(L"  Graphics: %dx%d, framebuffer at 0x%lx, %d modes\n",
          info->HorizontalResolution, info->VerticalResolution,
          gop->Mode->FrameBufferBase, gop->Mode->MaxMode);
}

void diag_report(void)
{
    Print(L"Diagnostics\n");
    report_cpu();
    report_memory();
    report_graphics();
    Print(L"\n");
}
