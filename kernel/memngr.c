#include "mem.h"
#include "cpu.h"
#include "spinlock.h"
#include "display.h"
#include "string.h"

#define MAX_REGIONS     512
#define LOW_MEMORY      0x100000ULL

static struct aegis_memory_region regions[MAX_REGIONS];
static uint64_t region_count;

static uint8_t *bitmap;
static uint64_t bitmap_pages;
static uint64_t free_pages;
static uint64_t total_pages;
static uint64_t next_hint;

static uint64_t kernel_virt, kernel_phys, kernel_size;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline bool page_used(uint64_t page)
{
    return bitmap[page / 8] & (1 << (page % 8));
}

static inline void set_used(uint64_t page)
{
    bitmap[page / 8] |= 1 << (page % 8);
}

static inline void set_free(uint64_t page)
{
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static void mark_free(uint64_t base, uint64_t length)
{
    uint64_t first = ALIGN_UP(MAX(base, LOW_MEMORY), PAGE_SIZE) / PAGE_SIZE;
    uint64_t last = MIN(ALIGN_DOWN(base + length, PAGE_SIZE) / PAGE_SIZE, bitmap_pages);

    for (uint64_t p = first; p < last; p++) {
        if (page_used(p)) {
            set_free(p);
            free_pages++;
            total_pages++;
        }
    }
}

static void mark_used(uint64_t base, uint64_t length)
{
    uint64_t first = ALIGN_DOWN(base, PAGE_SIZE) / PAGE_SIZE;
    uint64_t last = MIN(ALIGN_UP(base + length, PAGE_SIZE) / PAGE_SIZE, bitmap_pages);

    for (uint64_t p = first; p < last; p++) {
        if (!page_used(p)) {
            set_used(p);
            free_pages--;
            total_pages--;
        }
    }
}

static void pmm_init(void)
{
    uint64_t max_addr = 0, bytes;

    for (uint64_t i = 0; i < region_count; i++) {
        if (regions[i].type == AEGIS_MEM_USABLE
            || regions[i].type == AEGIS_MEM_BOOTLOADER_RECLAIMABLE)
            max_addr = MAX(max_addr, regions[i].base + regions[i].length);
    }

    bitmap_pages = max_addr / PAGE_SIZE;
    bytes = ALIGN_UP(bitmap_pages, 8) / 8;

    for (uint64_t i = 0; i < region_count && !bitmap; i++) {
        uint64_t base = ALIGN_UP(MAX(regions[i].base, LOW_MEMORY), PAGE_SIZE);
        uint64_t end = regions[i].base + regions[i].length;

        if (regions[i].type == AEGIS_MEM_USABLE && base < end && end - base >= bytes)
            bitmap = (uint8_t *)base;
    }
    if (!bitmap)
        panic("No usable memory region can hold the page bitmap (%lu bytes)", bytes);

    memset(bitmap, 0xFF, bytes);
    for (uint64_t i = 0; i < region_count; i++) {
        if (regions[i].type == AEGIS_MEM_USABLE)
            mark_free(regions[i].base, regions[i].length);
    }
    mark_used((uint64_t)bitmap, bytes);
}

static uint64_t alloc_locked(uint64_t count)
{
    uint64_t run = 0;

    if (count == 0 || count > free_pages)
        return 0;

    for (uint64_t scanned = 0, p = next_hint; scanned < bitmap_pages + count; scanned++, p++) {
        if (p >= bitmap_pages) {
            p = 0;
            run = 0;
        }
        if (page_used(p)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            uint64_t first = p + 1 - count;

            for (uint64_t i = first; i <= p; i++)
                set_used(i);
            free_pages -= count;
            next_hint = p + 1;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_pages(uint64_t count)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t addr = alloc_locked(count);

    spin_unlock_irqrestore(&pmm_lock, flags);
    return addr;
}

uint64_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

void pmm_free_pages(uint64_t addr, uint64_t count)
{
    uint64_t first = addr / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    if (addr % PAGE_SIZE || first + count > bitmap_pages)
        panic("pmm_free_pages: bad range 0x%lx (+%lu pages)", addr, count);

    for (uint64_t p = first; p < first + count; p++) {
        if (!page_used(p))
            panic("pmm_free_pages: page 0x%lx is already free", p * PAGE_SIZE);
        set_free(p);
    }
    free_pages += count;
    if (first < next_hint)
        next_hint = first;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_free_page(uint64_t addr)
{
    pmm_free_pages(addr, 1);
}

uint64_t pmm_free_count(void)
{
    return free_pages;
}

uint64_t pmm_total_count(void)
{
    return total_pages;
}

uint64_t kernel_virt_to_phys(uint64_t virt)
{
    if (virt >= kernel_virt && virt - kernel_virt < kernel_size)
        return virt - kernel_virt + kernel_phys;
    return virt;
}

void mem_init(const struct aegis_boot_info *info)
{
    const struct aegis_memory_region *map = (const struct aegis_memory_region *)info->memory_map;
    uint64_t count = info->memory_map_entries;
    uint64_t max_addr = 4ULL << 30;

    if (count > MAX_REGIONS)
        panic("Memory map has %lu regions, the kernel supports %d", count, MAX_REGIONS);
    memcpy(regions, map, count * sizeof(*map));
    region_count = count;
    kernel_virt = info->kernel_virtual_base;
    kernel_phys = info->kernel_physical_base;
    kernel_size = info->kernel_size;

    pmm_init();

    for (uint64_t i = 0; i < region_count; i++)
        max_addr = MAX(max_addr, regions[i].base + regions[i].length);
    for (uint32_t i = 0; i < display_count(); i++) {
        const struct display *d = display_get(i);
        max_addr = MAX(max_addr, (uint64_t)d->vram + (uint64_t)d->height * d->pitch);
    }
    paging_init(max_addr, kernel_virt, kernel_phys, kernel_size);

    for (uint64_t i = 0; i < region_count; i++) {
        if (regions[i].type == AEGIS_MEM_BOOTLOADER_RECLAIMABLE)
            mark_free(regions[i].base, regions[i].length);
    }
}

// Only call once everything needed from the ACPI tables has been copied.
void mem_reclaim_acpi(void)
{
    for (uint64_t i = 0; i < region_count; i++) {
        if (regions[i].type == AEGIS_MEM_ACPI_RECLAIMABLE)
            mark_free(regions[i].base, regions[i].length);
    }
}

// True if [base, base + size) is RAM below 1 MiB that nothing else uses.
// The page allocator never hands out memory below 1 MiB.
bool mem_reserve_low(uint64_t base, uint64_t size)
{
    if (base + size > LOW_MEMORY)
        return false;
    for (uint64_t i = 0; i < region_count; i++) {
        if ((regions[i].type == AEGIS_MEM_USABLE || regions[i].type == AEGIS_MEM_BOOTLOADER_RECLAIMABLE)
            && base >= regions[i].base && base + size <= regions[i].base + regions[i].length)
            return true;
    }
    return false;
}
