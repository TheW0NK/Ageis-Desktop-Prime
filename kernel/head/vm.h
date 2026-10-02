#ifndef AEGIS_VM_H
#define AEGIS_VM_H

#include "kernel.h"
#include "cpu.h"
#include "sync.h"
#include "abi/mman.h"

struct file;
struct vnode;

#define MMAP_BASE       0x0000720000000000ULL
#define MMAP_END        0x00007F0000000000ULL

enum vma_type { VMA_ANON, VMA_FILE, VMA_SHM, VMA_PHYS };

// Memory shared between address spaces; its pages are allocated on first use.
struct shm_object {
    uint32_t refs;
    uint64_t npages;
    uint64_t *pages;
    spinlock_t lock;
};

struct vma {
    uint64_t start, end;
    uint32_t prot, flags;
    enum vma_type type;
    struct shm_object *shm;
    struct vnode *vnode;
    uint64_t offset;                // file offset, offset into shm, or physical base
    struct vma *next;
};

struct mm {
    struct mutex lock;
    uint64_t space;
    struct vma *vmas;               // sorted by address
    uint64_t resident;              // pages present
    uint64_t mapped;                // bytes covered by VMAs
    uint32_t users;                 // threads using this space
    // The last ranges removed, for reports of faults on missing mappings.
    struct {
        uint64_t start, end, when;
        uint32_t tid;
    } unmapped[8];
    unsigned unmapped_next;
};

struct mm *mm_create(void);
void mm_destroy(struct mm *mm);

// Maps len bytes. Without MAP_FIXED, addr is ignored and a free range is
// chosen. f (may be NULL for MAP_ANONYMOUS) supplies the backing.
int vm_mmap(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags,
            struct file *f, uint64_t offset, uint64_t *out);
int vm_munmap(struct mm *mm, uint64_t addr, uint64_t len);
int vm_mprotect(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot);
uint64_t vm_mapped_bytes(struct mm *mm);

// Handles a page fault; returns true if the access may be retried.
bool vm_fault(struct mm *mm, uint64_t addr, bool write);
// Called for faults in kernel mode: retries, or redirects user copies to
// their error path. Returns false if the fault is a kernel bug.
bool vm_kernel_fault(struct interrupt_frame *frame, uint64_t addr);

struct shm_object *shm_create(uint64_t size);
void shm_ref(struct shm_object *o);
void shm_put(struct shm_object *o);

// Fault-safe access to the current process's memory.
bool user_range_ok(uint64_t addr, uint64_t len);
int copy_from_user(void *dst, uint64_t src, size_t n);
int copy_to_user(uint64_t dst, const void *src, size_t n);
// Copies a NUL-terminated string of at most max-1 characters. Returns 0,
// -EFAULT, or -ENAMETOOLONG.
int string_from_user(char *dst, uint64_t src, size_t max);

#endif
