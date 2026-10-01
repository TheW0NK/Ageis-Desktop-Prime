#include "mem.h"
#include "cpu.h"
#include "smp.h"
#include "spinlock.h"
#include "string.h"

#define LARGE_PAGE_SIZE (2ULL << 20)
#define INDEX(v, level) (((v) >> (12 + 9 * (level))) & 0x1FF)
#define USER_PML4_FIRST INDEX(USER_REGION_BASE, 3)
#define USER_PML4_END   (INDEX(USER_REGION_END - 1, 3) + 1)

static uint64_t *kernel_pml4;
static spinlock_t paging_lock = SPINLOCK_INIT;

static uint64_t *table_alloc(void)
{
    uint64_t page = pmm_alloc_page();

    if (!page)
        return NULL;
    memset((void *)page, 0, PAGE_SIZE);
    return (uint64_t *)page;
}

static uint64_t *next_level(uint64_t *table, unsigned index, bool create, uint64_t flags)
{
    uint64_t entry = table[index];
    uint64_t *next;

    if (entry & PTE_PRESENT)
        return (entry & PTE_LARGE) ? NULL : (uint64_t *)(entry & PTE_ADDR_MASK);
    if (!create || !(next = table_alloc()))
        return NULL;

    table[index] = (uint64_t)next | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    return next;
}

static bool split_large(uint64_t *pde)
{
    uint64_t entry = *pde;
    uint64_t base = entry & PTE_ADDR_MASK & ~(LARGE_PAGE_SIZE - 1);
    uint64_t flags = entry & (PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NOCACHE | PTE_WRITETHROUGH);
    uint64_t *pt = table_alloc();

    if (!pt)
        return false;
    for (unsigned i = 0; i < 512; i++)
        pt[i] = (base + i * PAGE_SIZE) | flags;

    *pde = (uint64_t)pt | PTE_PRESENT | PTE_WRITABLE | (entry & PTE_USER);
    write_cr3(read_cr3());
    return true;
}

static uint64_t *walk(uint64_t *pml4, uint64_t virt, bool create, uint64_t flags)
{
    uint64_t *pdpt, *pd;

    if (!(pdpt = next_level(pml4, INDEX(virt, 3), create, flags)))
        return NULL;
    if (!(pd = next_level(pdpt, INDEX(virt, 2), create, flags)))
        return NULL;

    uint64_t *pde = &pd[INDEX(virt, 1)];
    if ((*pde & PTE_PRESENT) && (*pde & PTE_LARGE) && !split_large(pde))
        return NULL;

    uint64_t *pt = next_level(pd, INDEX(virt, 1), create, flags);
    return pt ? &pt[INDEX(virt, 0)] : NULL;
}

static uint64_t *space_pml4(uint64_t space)
{
    return space ? (uint64_t *)space : kernel_pml4;
}

void paging_init(uint64_t max_addr, uint64_t kernel_virt, uint64_t kernel_phys, uint64_t kernel_size)
{
    if (!(kernel_pml4 = table_alloc()))
        panic("Out of memory allocating the PML4");

    max_addr = ALIGN_UP(max_addr, LARGE_PAGE_SIZE);
    for (uint64_t addr = 0; addr < max_addr; addr += LARGE_PAGE_SIZE) {
        uint64_t *pdpt = next_level(kernel_pml4, INDEX(addr, 3), true, 0);
        uint64_t *pd = pdpt ? next_level(pdpt, INDEX(addr, 2), true, 0) : NULL;

        if (!pd)
            panic("Out of memory building the identity map at 0x%lx", addr);
        pd[INDEX(addr, 1)] = addr | PTE_PRESENT | PTE_WRITABLE | PTE_LARGE;
    }

    for (uint64_t off = 0; kernel_virt != kernel_phys && off < kernel_size; off += PAGE_SIZE) {
        uint64_t *pte = walk(kernel_pml4, kernel_virt + off, true, 0);

        if (!pte)
            panic("Out of memory mapping the kernel");
        *pte = (kernel_phys + off) | PTE_PRESENT | PTE_WRITABLE;
    }

    write_cr3((uint64_t)kernel_pml4);

    // Leave page 0 unmapped so NULL dereferences fault.
    paging_unmap_page(0);
}

uint64_t paging_kernel_space(void)
{
    return (uint64_t)kernel_pml4;
}

// User address spaces share every kernel PML4 entry and own the user region.
uint64_t paging_create_space(void)
{
    uint64_t *pml4 = table_alloc();

    if (!pml4)
        return 0;
    for (unsigned i = 0; i < 512; i++) {
        if (i < USER_PML4_FIRST || i >= USER_PML4_END)
            pml4[i] = kernel_pml4[i];
    }
    return (uint64_t)pml4;
}

static void free_table(uint64_t *table, int level)
{
    for (unsigned i = 0; level > 0 && i < 512; i++) {
        if ((table[i] & PTE_PRESENT) && !(table[i] & PTE_LARGE))
            free_table((uint64_t *)(table[i] & PTE_ADDR_MASK), level - 1);
    }
    if (level == 0) {
        for (unsigned i = 0; i < 512; i++) {
            if (table[i] & PTE_PRESENT)
                pmm_free_page(table[i] & PTE_ADDR_MASK);
        }
    }
    pmm_free_page((uint64_t)table);
}

// Frees the space's user pages and page tables. It must not be the active space.
void paging_destroy_space(uint64_t space)
{
    uint64_t *pml4 = (uint64_t *)space;
    uint64_t flags = spin_lock_irqsave(&paging_lock);

    for (unsigned i = USER_PML4_FIRST; i < USER_PML4_END; i++) {
        if (pml4[i] & PTE_PRESENT)
            free_table((uint64_t *)(pml4[i] & PTE_ADDR_MASK), 2);
    }
    pmm_free_page(space);
    spin_unlock_irqrestore(&paging_lock, flags);
}

// Other CPUs only need a TLB flush for kernel mappings: a user address space
// is only ever loaded on the CPU running its single thread.
static void kernel_mapping_changed(uint64_t space, uint64_t irq)
{
    if ((!space || space == (uint64_t)kernel_pml4) && (irq & 0x200))
        tlb_shootdown();
}

bool paging_map_page_in(uint64_t space, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t irq = spin_lock_irqsave(&paging_lock);
    uint64_t *pte = walk(space_pml4(space), virt, true, flags);

    if (pte) {
        *pte = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
        invlpg(virt);
    }
    spin_unlock_irqrestore(&paging_lock, irq);
    kernel_mapping_changed(space, irq);
    return pte != NULL;
}

bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    return paging_map_page_in(0, virt, phys, flags);
}

void paging_unmap_page(uint64_t virt)
{
    uint64_t irq = spin_lock_irqsave(&paging_lock);
    uint64_t *pte = walk(kernel_pml4, virt, false, 0);

    if (pte) {
        *pte = 0;
        invlpg(virt);
    }
    spin_unlock_irqrestore(&paging_lock, irq);
    kernel_mapping_changed(0, irq);
}

bool paging_map_mmio(uint64_t phys, uint64_t size)
{
    uint64_t end = ALIGN_UP(phys + size, PAGE_SIZE);

    for (uint64_t addr = ALIGN_DOWN(phys, PAGE_SIZE); addr < end; addr += PAGE_SIZE) {
        if (!paging_map_page(addr, addr, PTE_MMIO))
            return false;
    }
    return true;
}

uint64_t paging_translate_in(uint64_t space, uint64_t virt)
{
    uint64_t *pdpt, *pd, *pt, pde, pte;

    if (!(pdpt = next_level(space_pml4(space), INDEX(virt, 3), false, 0)))
        return 0;
    if (!(pd = next_level(pdpt, INDEX(virt, 2), false, 0)))
        return 0;

    pde = pd[INDEX(virt, 1)];
    if (!(pde & PTE_PRESENT))
        return 0;
    if (pde & PTE_LARGE)
        return (pde & PTE_ADDR_MASK & ~(LARGE_PAGE_SIZE - 1)) + (virt & (LARGE_PAGE_SIZE - 1));

    pt = (uint64_t *)(pde & PTE_ADDR_MASK);
    pte = pt[INDEX(virt, 0)];
    return (pte & PTE_PRESENT) ? (pte & PTE_ADDR_MASK) + (virt & (PAGE_SIZE - 1)) : 0;
}

uint64_t paging_translate(uint64_t virt)
{
    return paging_translate_in(0, virt);
}
