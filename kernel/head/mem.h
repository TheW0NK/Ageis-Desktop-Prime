#ifndef AEGIS_MEM_H
#define AEGIS_MEM_H

#include "kernel.h"
#include "bootinfo.h"

#define PAGE_SIZE       ((uint64_t)4096)

#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_WRITETHROUGH (1ULL << 3)
#define PTE_NOCACHE     (1ULL << 4)
#define PTE_LARGE       (1ULL << 7)
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

#define USER_REGION_BASE 0x0000700000000000ULL
#define USER_REGION_END  0x0000800000000000ULL

#define PTE_MMIO        (PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH)

// The boot info and memory map live in bootloader-reclaimable memory, which
// mem_init frees. Copy anything still needed from them before calling it.
void mem_init(const struct aegis_boot_info *info);
void mem_reclaim_acpi(void);
bool mem_reserve_low(uint64_t base, uint64_t size);
uint64_t kernel_virt_to_phys(uint64_t virt);

uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(uint64_t count);
void pmm_free_page(uint64_t addr);
void pmm_free_pages(uint64_t addr, uint64_t count);
uint64_t pmm_free_count(void);
uint64_t pmm_total_count(void);

void paging_init(uint64_t max_addr, uint64_t kernel_virt, uint64_t kernel_phys, uint64_t kernel_size);
bool paging_map_mmio(uint64_t phys, uint64_t size);
bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void paging_unmap_page(uint64_t virt);
uint64_t paging_translate(uint64_t virt);

// A space is the physical address of its PML4; 0 means the kernel's.
uint64_t paging_kernel_space(void);
uint64_t paging_create_space(void);
void paging_destroy_space(uint64_t space);
bool paging_map_page_in(uint64_t space, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t paging_translate_in(uint64_t space, uint64_t virt);

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

#endif
