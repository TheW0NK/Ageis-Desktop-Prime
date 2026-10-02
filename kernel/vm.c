#include "vm.h"
#include "apic.h"
#include "sched.h"
#include "mem.h"
#include "process.h"
#include "smp.h"
#include "string.h"
#include "vfs.h"

extern const uint8_t uaccess_start[], uaccess_end[], uaccess_fixup[];
int uaccess_copy(void *dst, const void *src, size_t n);
long uaccess_strncpy(char *dst, const char *src, size_t max);

// ---- Shared memory objects ----

struct shm_object *shm_create(uint64_t size)
{
    struct shm_object *o;

    if (!size || size > (1ULL << 34))
        return NULL;
    if (!(o = kzalloc(sizeof(*o))))
        return NULL;
    o->npages = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    if (!(o->pages = kzalloc(o->npages * sizeof(uint64_t)))) {
        kfree(o);
        return NULL;
    }
    o->refs = 1;
    return o;
}

void shm_ref(struct shm_object *o)
{
    __atomic_fetch_add(&o->refs, 1, __ATOMIC_RELAXED);
}

void shm_put(struct shm_object *o)
{
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL))
        return;
    for (uint64_t i = 0; i < o->npages; i++) {
        if (o->pages[i])
            pmm_free_page(o->pages[i]);
    }
    kfree(o->pages);
    kfree(o);
}

static uint64_t shm_page(struct shm_object *o, uint64_t index)
{
    uint64_t flags, page;

    if (index >= o->npages)
        return 0;
    flags = spin_lock_irqsave(&o->lock);
    if (!o->pages[index] && (page = pmm_alloc_page())) {
        memset((void *)page, 0, PAGE_SIZE);
        o->pages[index] = page;
    }
    page = o->pages[index];
    spin_unlock_irqrestore(&o->lock, flags);
    return page;
}

// ---- Address spaces ----

struct mm *mm_create(void)
{
    struct mm *mm = kzalloc(sizeof(*mm));

    if (!mm)
        return NULL;
    mm->lock = (struct mutex)MUTEX_INIT;
    if (!(mm->space = paging_create_space())) {
        kfree(mm);
        return NULL;
    }
    return mm;
}

static void vma_free(struct vma *v)
{
    shm_put(v->shm);
    vput(v->vnode);
    kfree(v);
}

static void flush_others(struct mm *mm)
{
    if (mm->users > 1)
        tlb_shootdown();
}

// Removes the pages of [start, end), all inside v. Called with mm->lock held.
static void unmap_pages(struct mm *mm, struct vma *v, uint64_t start, uint64_t end)
{
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t old = paging_unmap_page_in(mm->space, va);

        if (!(old & PTE_PRESENT))
            continue;
        mm->resident--;
        if (v->type == VMA_ANON || v->type == VMA_FILE)
            pmm_free_page(old & PTE_ADDR_MASK);
    }
}

void mm_destroy(struct mm *mm)
{
    struct vma *v = mm->vmas;

    while (v) {
        struct vma *next = v->next;

        unmap_pages(mm, v, v->start, v->end);
        vma_free(v);
        v = next;
    }
    paging_destroy_space(mm->space);
    kfree(mm);
}

static uint64_t pte_flags(const struct vma *v)
{
    uint64_t f = PTE_USER;

    if (v->prot & PROT_WRITE)
        f |= PTE_WRITABLE;
    if (v->type == VMA_PHYS)
        f |= PTE_WRITETHROUGH;
    return f;
}

static struct vma *find_vma(struct mm *mm, uint64_t addr)
{
    for (struct vma *v = mm->vmas; v && v->start <= addr; v = v->next) {
        if (addr < v->end)
            return v;
    }
    return NULL;
}

// Splits v at addr (page aligned, strictly inside v). Returns false on ENOMEM.
static bool split(struct vma *v, uint64_t addr)
{
    struct vma *n = kmalloc(sizeof(*n));

    if (!n)
        return false;
    *n = *v;
    n->start = addr;
    if (v->type != VMA_ANON)
        n->offset += addr - v->start;
    if (n->shm)
        shm_ref(n->shm);
    if (n->vnode)
        vref(n->vnode);
    v->end = addr;
    v->next = n;
    return true;
}

// Splits VMAs so that start and end fall on VMA boundaries.
static bool split_range(struct mm *mm, uint64_t start, uint64_t end)
{
    for (struct vma *v = mm->vmas; v; v = v->next) {
        if (v->start < start && start < v->end && !split(v, start))
            return false;
        if (v->start < end && end < v->end && !split(v, end))
            return false;
    }
    return true;
}

static int unmap_locked(struct mm *mm, uint64_t start, uint64_t end)
{
    bool any = false;

    if (!split_range(mm, start, end))
        return -ENOMEM;
    for (struct vma **pp = &mm->vmas, *v; (v = *pp);) {
        if (v->start >= start && v->end <= end) {
            *pp = v->next;
            mm->mapped -= v->end - v->start;
            unmap_pages(mm, v, v->start, v->end);
            vma_free(v);
            any = true;
        } else {
            pp = &v->next;
        }
    }
    if (any)
        flush_others(mm);
    return 0;
}

static uint64_t find_gap(struct mm *mm, uint64_t len)
{
    uint64_t addr = MMAP_BASE;

    for (struct vma *v = mm->vmas; v; v = v->next) {
        if (v->end <= addr)
            continue;
        if (v->start >= addr + len)
            break;
        addr = v->end;
    }
    return addr + len <= MMAP_END ? addr : 0;
}

static void insert(struct mm *mm, struct vma *n)
{
    struct vma **pp = &mm->vmas;

    mm->mapped += n->end - n->start;
    while (*pp && (*pp)->start < n->start)
        pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
}

static bool fault_locked(struct mm *mm, struct vma *v, uint64_t addr, bool write);

int vm_mmap(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags,
            struct file *f, uint64_t offset, uint64_t *out)
{
    struct vma *v;
    int ret = 0;

    if (!len || (offset & (PAGE_SIZE - 1)) || (prot & ~7U))
        return -EINVAL;
    if (!(flags & (MAP_SHARED | MAP_PRIVATE)) || ((flags & MAP_SHARED) && (flags & MAP_PRIVATE)))
        return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (len > USER_REGION_END - USER_REGION_BASE)
        return -ENOMEM;
    if ((flags & MAP_FIXED) && ((addr & (PAGE_SIZE - 1)) || addr < USER_REGION_BASE
                                || addr + len > USER_REGION_END || addr + len < addr))
        return -EINVAL;
    if (!(v = kzalloc(sizeof(*v))))
        return -ENOMEM;
    v->prot = prot;
    v->flags = flags;
    v->offset = offset;
    v->type = VMA_ANON;

    if (!(flags & MAP_ANONYMOUS)) {
        if (!f) {
            kfree(v);
            return -EBADF;
        }
        if ((f->flags & O_ACCMODE) == O_WRONLY) {
            kfree(v);
            return -EACCES;
        }
        if (f->ops && f->ops->mmap) {
            ret = f->ops->mmap(f, v);
        } else if (f->vnode && S_ISREG(f->vnode->mode)) {
            // Files are mapped privately: a shared writable mapping would need
            // a page cache that writes back, which does not exist yet.
            if ((flags & MAP_SHARED) && (prot & PROT_WRITE))
                ret = -ENODEV;
            v->type = VMA_FILE;
            v->vnode = f->vnode;
            vref(v->vnode);
        } else {
            ret = -ENODEV;
        }
        if (ret) {
            vma_free(v);
            return ret;
        }
    }

    mutex_lock(&mm->lock);
    if (flags & MAP_FIXED) {
        if ((ret = unmap_locked(mm, addr, addr + len))) {
            mutex_unlock(&mm->lock);
            vma_free(v);
            return ret;
        }
    } else if (!(addr = find_gap(mm, len))) {
        mutex_unlock(&mm->lock);
        vma_free(v);
        return -ENOMEM;
    }
    v->start = addr;
    v->end = addr + len;
    insert(mm, v);
    if (flags & MAP_POPULATE) {
        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
            if (!fault_locked(mm, v, va, false)) {
                unmap_locked(mm, v->start, v->end);
                mutex_unlock(&mm->lock);
                return -ENOMEM;
            }
        }
    }
    mutex_unlock(&mm->lock);
    *out = addr;
    return 0;
}

int vm_munmap(struct mm *mm, uint64_t addr, uint64_t len)
{
    int ret;

    if ((addr & (PAGE_SIZE - 1)) || !len || addr < USER_REGION_BASE || addr + len > USER_REGION_END)
        return -EINVAL;
    mutex_lock(&mm->lock);
    ret = unmap_locked(mm, addr, addr + ALIGN_UP(len, PAGE_SIZE));
    {
        unsigned i = mm->unmapped_next++ % 8;

        mm->unmapped[i].start = addr;
        mm->unmapped[i].end = addr + ALIGN_UP(len, PAGE_SIZE);
        mm->unmapped[i].when = timer_uptime_ms();
        mm->unmapped[i].tid = sched_current() ? sched_current()->id : 0;
    }
    mutex_unlock(&mm->lock);
    return ret;
}

int vm_mprotect(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot)
{
    uint64_t end = addr + ALIGN_UP(len, PAGE_SIZE);
    int ret = 0;

    if ((addr & (PAGE_SIZE - 1)) || addr < USER_REGION_BASE || end > USER_REGION_END || (prot & ~7U))
        return -EINVAL;
    mutex_lock(&mm->lock);
    // Every page in the range must be mapped.
    for (uint64_t a = addr; a < end;) {
        struct vma *v = find_vma(mm, a);

        if (!v) {
            ret = -ENOMEM;
            break;
        }
        a = v->end;
    }
    if (!ret && !split_range(mm, addr, end))
        ret = -ENOMEM;
    for (struct vma *v = mm->vmas; !ret && v; v = v->next) {
        if (v->start < addr || v->end > end)
            continue;
        if ((prot & PROT_WRITE) && v->type == VMA_FILE && (v->flags & MAP_SHARED)) {
            ret = -EACCES;
            break;
        }
        v->prot = prot;
        // PROT_NONE pages stay present but supervisor-only, so they keep
        // their contents and fault on any user access.
        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE)
            paging_protect_in(mm->space, va, prot == PROT_NONE ? 0 : pte_flags(v));
    }
    flush_others(mm);
    mutex_unlock(&mm->lock);
    return ret;
}

uint64_t vm_mapped_bytes(struct mm *mm)
{
    return mm->mapped;
}

static bool fault_locked(struct mm *mm, struct vma *v, uint64_t addr, bool write)
{
    uint64_t va = ALIGN_DOWN(addr, PAGE_SIZE), phys = 0, existing;
    bool fresh = false;

    if (write && !(v->prot & PROT_WRITE))
        return false;
    if (v->prot == PROT_NONE)
        return false;
    existing = paging_translate_in(mm->space, va);
    if (existing)
        return true;                // another thread mapped it first

    switch (v->type) {
    case VMA_ANON:
    case VMA_FILE:
        if (!(phys = pmm_alloc_page())) {
            kprintf("vm: out of memory for a page at 0x%lx (%lu pages free)\n", va, pmm_free_count());
            return false;
        }
        memset((void *)phys, 0, PAGE_SIZE);
        fresh = true;
        if (v->type == VMA_FILE) {
            uint64_t off = v->offset + (va - v->start);

            if (off < v->vnode->size && vfs_read(v->vnode, (void *)phys,
                                                 MIN(PAGE_SIZE, v->vnode->size - off), off) < 0) {
                pmm_free_page(phys);
                return false;
            }
        }
        break;
    case VMA_SHM:
        phys = shm_page(v->shm, (v->offset + (va - v->start)) / PAGE_SIZE);
        break;
    case VMA_PHYS:
        phys = v->offset + (va - v->start);
        break;
    }
    if (!phys)
        return false;
    if (!paging_map_page_in(mm->space, va, phys, pte_flags(v))) {
        kprintf("vm: cannot map 0x%lx (page tables: %lu pages free)\n", va, pmm_free_count());
        if (fresh)
            pmm_free_page(phys);
        return false;
    }
    mm->resident++;
    return true;
}

bool vm_fault(struct mm *mm, uint64_t addr, bool write)
{
    struct vma *v;
    bool ok = false;

    if (!mm || addr < USER_REGION_BASE || addr >= USER_REGION_END)
        return false;
    mutex_lock(&mm->lock);
    if ((v = find_vma(mm, addr)))
        ok = fault_locked(mm, v, addr, write);
    else if (addr >= MMAP_BASE && addr < MMAP_END) {
        kprintf("vm: no mapping at 0x%lx (thread %lu, now %lu ms)\n", addr,
                sched_current() ? sched_current()->id : 0, timer_uptime_ms());
        for (int i = 0; i < 8; i++)
            if (mm->unmapped[i].end && addr >= mm->unmapped[i].start && addr < mm->unmapped[i].end)
                kprintf("vm:   unmapped 0x%lx-0x%lx by thread %u at %lu ms\n", mm->unmapped[i].start,
                        mm->unmapped[i].end, mm->unmapped[i].tid, mm->unmapped[i].when);
        {
            uint64_t sum = 0;
            int count = 0;

            for (struct vma *n = mm->vmas; n; n = n->next) {
                sum += n->end - n->start;
                count++;
                if (n->start >= MMAP_BASE)
                    kprintf("vm:   vma 0x%lx-0x%lx type %d\n", n->start, n->end, n->type);
            }
            kprintf("vm:   %d vmas, %lu bytes listed, %lu bytes recorded, %u users\n", count, sum, mm->mapped,
                    mm->users);
        }
    }
    mutex_unlock(&mm->lock);
    return ok;
}

bool vm_kernel_fault(struct interrupt_frame *frame, uint64_t addr)
{
    struct process *p = process_current();
    bool in_uaccess = frame->rip >= (uint64_t)uaccess_start && frame->rip < (uint64_t)uaccess_end;

    if (addr < USER_REGION_BASE || addr >= USER_REGION_END)
        return false;
    if (p && (frame->rflags & 0x200)) {
        sti();
        if (vm_fault(p->mm, addr, frame->error_code & 2)) {
            cli();
            return true;
        }
        cli();
    }
    if (in_uaccess) {
        frame->rip = (uint64_t)uaccess_fixup;
        return true;
    }
    return false;
}

// ---- Copying ----

bool user_range_ok(uint64_t addr, uint64_t len)
{
    return addr >= USER_REGION_BASE && addr <= USER_REGION_END && len <= USER_REGION_END - addr;
}

int copy_from_user(void *dst, uint64_t src, size_t n)
{
    if (!n)
        return 0;
    if (!user_range_ok(src, n))
        return -EFAULT;
    return uaccess_copy(dst, (const void *)src, n);
}

int copy_to_user(uint64_t dst, const void *src, size_t n)
{
    if (!n)
        return 0;
    if (!user_range_ok(dst, n))
        return -EFAULT;
    return uaccess_copy((void *)dst, src, n);
}

int string_from_user(char *dst, uint64_t src, size_t max)
{
    long n;

    if (!max || src < USER_REGION_BASE || src >= USER_REGION_END)
        return -EFAULT;
    n = uaccess_strncpy(dst, (const char *)src, MIN(max, (size_t)(USER_REGION_END - src)));
    if (n < 0)
        return n;
    if ((size_t)n >= max) {
        dst[max - 1] = 0;
        return -ENAMETOOLONG;
    }
    return 0;
}
