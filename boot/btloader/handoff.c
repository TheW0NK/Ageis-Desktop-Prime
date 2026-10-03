// Hands off to other .bin, .efi, or other executable files like Recovery mode, or the kernel.

#include <efi.h>
#include <efilib.h>

#include "btloader.h"
#include "bootinfo.h"

// Memory type for the kernel's segments, from the range UEFI reserves for
// OS loaders, so the memory map can report them as AEGIS_MEM_KERNEL.
#define KERNEL_MEMORY_TYPE  ((EFI_MEMORY_TYPE)0x80000001)
#define RAMDISK_MEMORY_TYPE ((EFI_MEMORY_TYPE)0x80000002)
#define KERNEL_STACK_SIZE   (64 * 1024)

#define PAGE_SIZE           4096ULL
#define PAGE_DOWN(x)        ((x) & ~(PAGE_SIZE - 1))
#define PAGE_UP(x)          PAGE_DOWN((x) + PAGE_SIZE - 1)
#define LARGE_PAGE_SIZE     (2ULL << 20)

#define PTE_PRESENT         (1ULL << 0)
#define PTE_WRITABLE        (1ULL << 1)
#define PTE_LARGE           (1ULL << 7)
#define PTE_ADDR_MASK       0x000FFFFFFFFFF000ULL
#define HIGHER_HALF         0xFFFF800000000000ULL

#define PT_LOAD             1
#define ET_EXEC             2
#define EM_X86_64           62

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

static void *alloc_pages(UINTN bytes)
{
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status;

    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                               EfiLoaderData, PAGE_UP(bytes) / PAGE_SIZE, &addr);
    return EFI_ERROR(status) ? NULL : (void *)addr;
}

static BOOLEAN elf_valid(const UINT8 *file, UINTN size)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;

    return size >= sizeof(*eh)
        && eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E'
        && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F'
        && eh->e_ident[4] == 2                  // ELFCLASS64
        && eh->e_ident[5] == 1                  // little endian
        && eh->e_type == ET_EXEC
        && eh->e_machine == EM_X86_64
        && eh->e_phentsize == sizeof(Elf64_Phdr)
        && eh->e_phoff <= size
        && eh->e_phnum <= (size - eh->e_phoff) / sizeof(Elf64_Phdr);
}

struct kernel_image {
    UINT64 entry;
    UINT64 phys;
    UINT64 virt;
    UINT64 span;
};

// Identity-mapped kernels (p_vaddr == p_paddr) are loaded at their physical
// address. Higher-half kernels are loaded anywhere and mapped at p_vaddr.
static EFI_STATUS load_elf(const UINT8 *file, UINTN size, struct kernel_image *k)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    const Elf64_Phdr *ph;
    UINT64 lo = ~0ULL, hi = 0;
    UINTN loads = 0, identity = 0;
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status;

    if (!elf_valid(file, size)) {
        Print(L"Kernel is not an x86-64 ELF64 executable.\n");
        return EFI_LOAD_ERROR;
    }

    ph = (const Elf64_Phdr *)(file + eh->e_phoff);
    for (UINTN i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
            continue;

        if (ph[i].p_filesz > ph[i].p_memsz
            || ph[i].p_offset > size || ph[i].p_filesz > size - ph[i].p_offset
            || ph[i].p_vaddr + ph[i].p_memsz < ph[i].p_vaddr) {
            Print(L"Kernel segment %d is malformed.\n", i);
            return EFI_LOAD_ERROR;
        }

        loads++;
        if (ph[i].p_vaddr == ph[i].p_paddr)
            identity++;
        else if (ph[i].p_vaddr < HIGHER_HALF) {
            Print(L"Kernel segment %d must be identity mapped or in the higher half.\n", i);
            return EFI_UNSUPPORTED;
        }

        if (ph[i].p_vaddr < lo)
            lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi)
            hi = ph[i].p_vaddr + ph[i].p_memsz;
    }

    if (loads == 0) {
        Print(L"Kernel has no loadable segments.\n");
        return EFI_LOAD_ERROR;
    }
    if (identity && identity != loads) {
        Print(L"Kernel mixes identity-mapped and higher-half segments.\n");
        return EFI_UNSUPPORTED;
    }
    if (eh->e_entry < lo || eh->e_entry >= hi) {
        Print(L"Kernel entry point 0x%lx is outside its segments.\n", eh->e_entry);
        return EFI_LOAD_ERROR;
    }

    lo = PAGE_DOWN(lo);
    hi = PAGE_UP(hi);
    addr = lo;
    status = uefi_call_wrapper(BS->AllocatePages, 4,
                               identity ? AllocateAddress : AllocateAnyPages,
                               KERNEL_MEMORY_TYPE, (hi - lo) / PAGE_SIZE, &addr);
    if (EFI_ERROR(status)) {
        Print(L"Cannot allocate kernel memory for 0x%lx-0x%lx.\n", lo, hi);
        return status;
    }

    ZeroMem((void *)addr, hi - lo);
    for (UINTN i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_filesz)
            CopyMem((void *)(addr + ph[i].p_vaddr - lo), (void *)(file + ph[i].p_offset),
                    ph[i].p_filesz);
    }

    k->entry = eh->e_entry;
    k->phys = addr;
    k->virt = lo;
    k->span = hi - lo;
    return EFI_SUCCESS;
}

static UINT64 *table_alloc(void)
{
    UINT64 *t = alloc_pages(PAGE_SIZE);

    if (t)
        ZeroMem(t, PAGE_SIZE);
    return t;
}

static UINT64 *table_next(UINT64 *table, UINTN index)
{
    if (!(table[index] & PTE_PRESENT)) {
        UINT64 *t = table_alloc();

        if (!t)
            return NULL;
        table[index] = (UINT64)t | PTE_PRESENT | PTE_WRITABLE;
    }
    return (UINT64 *)(table[index] & PTE_ADDR_MASK);
}

static UINT64 highest_address(struct aegis_boot_info *info)
{
    EFI_MEMORY_DESCRIPTOR *map, *d;
    UINTN count, key, desc_size;
    UINT32 desc_version;
    UINT64 max = 4ULL << 30;

    map = LibMemoryMap(&count, &key, &desc_size, &desc_version);
    if (map) {
        d = map;
        for (UINTN i = 0; i < count; i++) {
            UINT64 end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
            if (end > max)
                max = end;
            d = NextMemoryDescriptor(d, desc_size);
        }
        FreePool(map);
    }

    for (UINT32 i = 0; i < info->framebuffer_count; i++) {
        UINT64 end = info->framebuffers[i].base + info->framebuffers[i].size;
        if (end > max)
            max = end;
    }
    return max;
}

// Identity maps all physical memory with 2 MiB pages, and maps a higher-half
// kernel at its virtual address with 4 KiB pages.
static UINT64 *build_page_tables(struct aegis_boot_info *info, struct kernel_image *k)
{
    UINT64 *pml4 = table_alloc();
    UINT64 max = PAGE_UP(highest_address(info));

    if (!pml4)
        return NULL;

    for (UINT64 addr = 0; addr < max; addr += LARGE_PAGE_SIZE) {
        UINT64 *pdpt = table_next(pml4, (addr >> 39) & 0x1FF);
        UINT64 *pd = pdpt ? table_next(pdpt, (addr >> 30) & 0x1FF) : NULL;

        if (!pd)
            return NULL;
        pd[(addr >> 21) & 0x1FF] = addr | PTE_PRESENT | PTE_WRITABLE | PTE_LARGE;
    }

    if (k->virt == k->phys)
        return pml4;

    for (UINT64 off = 0; off < k->span; off += PAGE_SIZE) {
        UINT64 virt = k->virt + off;
        UINT64 *pdpt = table_next(pml4, (virt >> 39) & 0x1FF);
        UINT64 *pd = pdpt ? table_next(pdpt, (virt >> 30) & 0x1FF) : NULL;
        UINT64 *pt = pd ? table_next(pd, (virt >> 21) & 0x1FF) : NULL;

        if (!pt)
            return NULL;
        pt[(virt >> 12) & 0x1FF] = (k->phys + off) | PTE_PRESENT | PTE_WRITABLE;
    }
    return pml4;
}

static BOOLEAN read_framebuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, struct aegis_framebuffer *fb)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode = gop->Mode->Info;

    ZeroMem(fb, sizeof(*fb));
    switch (mode->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        fb->format = AEGIS_FB_RGBX;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        fb->format = AEGIS_FB_BGRX;
        break;
    case PixelBitMask:
        fb->format = AEGIS_FB_BITMASK;
        fb->red_mask = mode->PixelInformation.RedMask;
        fb->green_mask = mode->PixelInformation.GreenMask;
        fb->blue_mask = mode->PixelInformation.BlueMask;
        fb->reserved_mask = mode->PixelInformation.ReservedMask;
        break;
    default:
        return FALSE;               // BltOnly: no linear framebuffer
    }

    fb->base = gop->Mode->FrameBufferBase;
    fb->size = gop->Mode->FrameBufferSize;
    fb->width = mode->HorizontalResolution;
    fb->height = mode->VerticalResolution;
    fb->pixels_per_scanline = mode->PixelsPerScanLine;
    return fb->base != 0;
}

static void set_resolution(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, struct bcd *bcd)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    UINT32 best = gop->Mode->Mode;
    UINT64 best_area = 0;
    UINTN size;

    if (!bcd->resolution_max && !bcd->resolution_width)
        return;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &size, &info))
            || info->PixelFormat == PixelBltOnly)
            continue;

        UINT64 area = (UINT64)info->HorizontalResolution * info->VerticalResolution;
        BOOLEAN match = bcd->resolution_max
            ? area > best_area
            : info->HorizontalResolution == bcd->resolution_width
              && info->VerticalResolution == bcd->resolution_height;

        if (match) {
            best = m;
            best_area = area;
            if (!bcd->resolution_max)
                break;
        }
    }

    if (!bcd->resolution_max && best_area == 0) {
        Print(L"Resolution %dx%d is not available; keeping the current mode.\n",
              bcd->resolution_width, bcd->resolution_height);
        return;
    }
    if (best != gop->Mode->Mode)
        uefi_call_wrapper(gop->SetMode, 2, gop, best);
}

// The console's GOP becomes framebuffers[0]; other displays follow.
static void get_framebuffers(struct aegis_boot_info *info, struct bcd *bcd)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;

    if (!EFI_ERROR(LibLocateProtocol(&GraphicsOutputProtocol, (void **)&gop))) {
        set_resolution(gop, bcd);
        if (read_framebuffer(gop, &info->framebuffers[0]))
            info->framebuffer_count = 1;
    }

    if (EFI_ERROR(LibLocateHandle(ByProtocol, &GraphicsOutputProtocol, NULL, &count, &handles)))
        return;

    for (UINTN i = 0; i < count && info->framebuffer_count < AEGIS_MAX_FRAMEBUFFERS; i++) {
        struct aegis_framebuffer fb;
        BOOLEAN seen = FALSE;

        if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, handles[i],
                                        &GraphicsOutputProtocol, (void **)&gop))
            || !read_framebuffer(gop, &fb))
            continue;

        for (UINT32 j = 0; j < info->framebuffer_count; j++)
            seen |= info->framebuffers[j].base == fb.base;
        if (!seen)
            info->framebuffers[info->framebuffer_count++] = fb;
    }
    FreePool(handles);
}

static void get_acpi(struct aegis_boot_info *info)
{
    EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = ACPI_TABLE_GUID;

    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];

        if (CompareGuid(&t->VendorGuid, &acpi20) == 0) {
            info->acpi_rsdp = (UINT64)t->VendorTable;
            info->acpi_revision = 2;
            return;
        }
        if (CompareGuid(&t->VendorGuid, &acpi10) == 0) {
            info->acpi_rsdp = (UINT64)t->VendorTable;
            info->acpi_revision = 1;
        }
    }
}

static UINT32 convert_type(UINT32 type)
{
    switch (type) {
    case EfiConventionalMemory:
        return AEGIS_MEM_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        return AEGIS_MEM_BOOTLOADER_RECLAIMABLE;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
        return AEGIS_MEM_FIRMWARE_RUNTIME;
    case EfiACPIReclaimMemory:
        return AEGIS_MEM_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS:
        return AEGIS_MEM_ACPI_NVS;
    case EfiUnusableMemory:
        return AEGIS_MEM_BAD;
    case KERNEL_MEMORY_TYPE:
        return AEGIS_MEM_KERNEL;
    case RAMDISK_MEMORY_TYPE:
        return AEGIS_MEM_RAMDISK;
    default:
        return AEGIS_MEM_RESERVED;
    }
}

// Converts the UEFI memory map into sorted, merged Aegis regions. Runs after
// ExitBootServices, so it must not allocate or call firmware.
static UINT64 build_memory_map(const UINT8 *map, UINTN map_size, UINTN desc_size,
                               struct aegis_memory_region *out)
{
    UINT64 n = 0, m = 0;

    for (UINTN off = 0; off + desc_size <= map_size; off += desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(map + off);
        struct aegis_memory_region r = {
            .base = d->PhysicalStart,
            .length = d->NumberOfPages * PAGE_SIZE,
            .type = convert_type(d->Type),
        };
        UINT64 i = n++;

        while (i > 0 && out[i - 1].base > r.base) {
            out[i] = out[i - 1];
            i--;
        }
        out[i] = r;
    }

    for (UINT64 i = 0; i < n; i++) {
        if (m && out[m - 1].type == out[i].type
            && out[m - 1].base + out[m - 1].length == out[i].base)
            out[m - 1].length += out[i].length;
        else
            out[m++] = out[i];
    }
    return m;
}

// Reads the entry's ramdisk into memory the kernel will leave alone.
static EFI_STATUS load_ramdisk(const CHAR16 *path, struct aegis_boot_info *info)
{
    EFI_FILE_HANDLE root, file;
    EFI_FILE_INFO *fi;
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status;
    UINTN size, done = 0;

    Print(L"Loading %s...\n", path);
    if (!(root = LibOpenRoot(btl_self->DeviceHandle)))
        return EFI_NOT_FOUND;
    status = uefi_call_wrapper(root->Open, 5, root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    uefi_call_wrapper(root->Close, 1, root);
    if (EFI_ERROR(status))
        return status;
    if (!(fi = LibFileInfo(file))) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_DEVICE_ERROR;
    }
    size = fi->FileSize;
    FreePool(fi);
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, RAMDISK_MEMORY_TYPE,
                               PAGE_UP(size) / PAGE_SIZE, &addr);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }
    // In pieces: some firmware (CD-ROM drivers especially) fails huge reads.
    while (done < size) {
        UINTN chunk = size - done > (4 << 20) ? (4 << 20) : size - done;

        status = uefi_call_wrapper(file->Read, 3, file, &chunk, (UINT8 *)addr + done);
        if (EFI_ERROR(status) || !chunk)
            break;
        done += chunk;
    }
    uefi_call_wrapper(file->Close, 1, file);
    if (done != size)
        return EFI_ERROR(status) ? status : EFI_END_OF_FILE;
    info->ramdisk_base = addr;
    info->ramdisk_size = size;
    return EFI_SUCCESS;
}

static void __attribute__((noreturn)) jump_to_kernel(UINT64 entry,
                                                     struct aegis_boot_info *info,
                                                     void *stack_top, UINT64 *pml4)
{
    __asm__ volatile (
        "cli\n\t"
        "mov %3, %%cr3\n\t"
        "mov %0, %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "call *%1\n\t"
        "1: hlt\n\t"
        "jmp 1b"
        : : "r"(stack_top), "r"(entry), "D"(info), "r"(pml4) : "memory");
    __builtin_unreachable();
}

EFI_STATUS handoff_kernel(struct bcd *bcd, struct bcd_entry *entry)
{
    struct aegis_boot_info *info;
    struct aegis_memory_region *regions;
    struct kernel_image kernel;
    UINT8 *file, *map, *stack;
    UINT64 *pml4;
    CHAR8 *cmdline;
    UINTN size, map_size, capacity, key, desc_size;
    UINT32 desc_version;
    EFI_STATUS status;

    status = btl_read_file(entry->path, (void **)&file, &size);
    if (EFI_ERROR(status))
        return status;

    status = load_elf(file, size, &kernel);
    FreePool(file);
    if (EFI_ERROR(status))
        return status;

    info = alloc_pages(sizeof(*info) + BCD_CMDLINE_MAX);
    stack = alloc_pages(KERNEL_STACK_SIZE);
    if (!info || !stack)
        return EFI_OUT_OF_RESOURCES;

    ZeroMem(info, sizeof(*info));
    info->magic = AEGIS_BOOT_MAGIC;
    info->version = AEGIS_BOOT_VERSION;
    info->size = sizeof(*info);
    info->kernel_physical_base = kernel.phys;
    info->kernel_virtual_base = kernel.virt;
    info->kernel_size = kernel.span;
    info->efi_system_table = (UINT64)ST;

    cmdline = (CHAR8 *)(info + 1);
    CopyMem(cmdline, entry->cmdline, BCD_CMDLINE_MAX);
    info->cmdline = (UINT64)cmdline;

    if (entry->ramdisk[0]) {
        status = load_ramdisk(entry->ramdisk, info);
        if (EFI_ERROR(status))
            return status;
    }
    get_framebuffers(info, bcd);
    get_acpi(info);

    pml4 = build_page_tables(info, &kernel);
    if (!pml4)
        return EFI_OUT_OF_RESOURCES;

    // Size the memory map buffers up front: nothing can be allocated after
    // ExitBootServices, and these allocations add entries of their own.
    map_size = 0;
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, NULL, &key,
                               &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL)
        return EFI_ERROR(status) ? status : EFI_DEVICE_ERROR;

    capacity = map_size + 16 * desc_size;
    map = alloc_pages(capacity);
    regions = alloc_pages(capacity / desc_size * sizeof(*regions));
    if (!map || !regions)
        return EFI_OUT_OF_RESOURCES;

    Print(L"Starting kernel at 0x%lx\n", kernel.entry);
    Print(L"Init Kernel Handoff...\n");

    for (UINTN attempt = 0;; attempt++) {
        map_size = capacity;
        status = uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, map, &key,
                                   &desc_size, &desc_version);
        if (EFI_ERROR(status))
            return status;

        // Fails if the map changed since GetMemoryMap; just try again.
        status = uefi_call_wrapper(BS->ExitBootServices, 2, btl_image, key);
        if (!EFI_ERROR(status))
            break;
        if (attempt == 3)
            return status;
    }

    // Boot services are gone: no allocations or console output past here.
    info->memory_map = (UINT64)regions;
    info->memory_map_entries = build_memory_map(map, map_size, desc_size, regions);
    jump_to_kernel(kernel.entry, info, stack + KERNEL_STACK_SIZE, pml4);
}

EFI_STATUS handoff_efi(struct bcd_entry *entry)
{
    static CHAR16 options[BCD_CMDLINE_MAX];
    EFI_DEVICE_PATH *path;
    EFI_LOADED_IMAGE *child_image;
    EFI_HANDLE child;
    EFI_STATUS status;
    UINTN i;

    path = FileDevicePath(btl_self->DeviceHandle, entry->path);
    if (!path)
        return EFI_OUT_OF_RESOURCES;

    status = uefi_call_wrapper(BS->LoadImage, 6, FALSE, btl_image, path,
                               NULL, 0, &child);
    FreePool(path);
    if (EFI_ERROR(status))
        return status;

    // Pass the entry's cmdline to the application as its load options.
    for (i = 0; entry->cmdline[i]; i++)
        options[i] = entry->cmdline[i];
    options[i] = 0;

    status = uefi_call_wrapper(BS->HandleProtocol, 3, child,
                               &LoadedImageProtocol, (void **)&child_image);
    if (!EFI_ERROR(status) && i) {
        child_image->LoadOptions = options;
        child_image->LoadOptionsSize = (i + 1) * sizeof(CHAR16);
    }

    status = uefi_call_wrapper(BS->StartImage, 3, child, NULL, NULL);
    return EFI_ERROR(status) ? status : EFI_ABORTED;
}
