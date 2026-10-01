#include "process.h"
#include "mem.h"
#include "string.h"
#include "sync.h"
#include "tty.h"

#define PT_LOAD     1
#define ET_EXEC     2
#define EM_X86_64   62

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf_phdr;

static struct wait_queue tree = WAIT_QUEUE_INIT;    // its lock guards the process tree
static struct process *all;
static struct process *init_process;
static int next_pid = 1;

struct process *process_current(void)
{
    struct thread *t = sched_current();

    return t ? t->process : NULL;
}

int fd_alloc(struct process *p, struct file *f, int min)
{
    for (int fd = min; fd < MAX_FDS; fd++) {
        if (!p->fds[fd]) {
            p->fds[fd] = f;
            p->cloexec[fd] = false;
            return fd;
        }
    }
    return -EMFILE;
}

struct file *fd_get(struct process *p, int fd)
{
    return (fd >= 0 && fd < MAX_FDS) ? p->fds[fd] : NULL;
}

// Copies into another address space through the identity map.
static int copy_to_space(uint64_t space, uint64_t va, const void *src, size_t len)
{
    const uint8_t *s = src;

    while (len) {
        uint64_t phys = paging_translate_in(space, va);
        size_t n = MIN(len, PAGE_SIZE - (va & (PAGE_SIZE - 1)));

        if (!phys)
            return -EFAULT;
        memcpy((void *)phys, s, n);
        va += n;
        s += n;
        len -= n;
    }
    return 0;
}

static int map_zero(uint64_t space, uint64_t start, uint64_t end)
{
    for (uint64_t va = ALIGN_DOWN(start, PAGE_SIZE); va < end; va += PAGE_SIZE) {
        uint64_t phys;

        if (paging_translate_in(space, va))
            continue;
        if (!(phys = pmm_alloc_page()))
            return -ENOMEM;
        memset((void *)phys, 0, PAGE_SIZE);
        if (!paging_map_page_in(space, va, phys, PTE_USER | PTE_WRITABLE)) {
            pmm_free_page(phys);
            return -ENOMEM;
        }
    }
    return 0;
}

static int load_elf(struct vnode *v, uint64_t space, uint64_t *entry, uint64_t *brk)
{
    elf_header eh;
    elf_phdr *ph;
    uint64_t top = USER_REGION_BASE;
    uint8_t *buf;
    int ret = -ENOEXEC;

    if (vfs_read(v, &eh, sizeof(eh), 0) != sizeof(eh))
        return -ENOEXEC;
    if (memcmp(eh.ident, "\x7f" "ELF", 4) || eh.ident[4] != 2 || eh.type != ET_EXEC
        || eh.machine != EM_X86_64 || eh.phentsize != sizeof(elf_phdr) || eh.phnum > 32)
        return -ENOEXEC;

    ph = kmalloc(eh.phnum * sizeof(*ph));
    buf = kmalloc(PAGE_SIZE);
    if (!ph || !buf) {
        ret = -ENOMEM;
        goto out;
    }
    if (vfs_read(v, ph, eh.phnum * sizeof(*ph), eh.phoff) != (int64_t)(eh.phnum * sizeof(*ph)))
        goto out;

    for (int i = 0; i < eh.phnum; i++) {
        uint64_t va = ph[i].vaddr, end = va + ph[i].memsz;

        if (ph[i].type != PT_LOAD || !ph[i].memsz)
            continue;
        if (va < USER_REGION_BASE || end < va || end > USER_STACK_TOP - USER_STACK_SIZE
            || ph[i].filesz > ph[i].memsz)
            goto out;
        if ((ret = map_zero(space, va, end)))
            goto out;
        for (uint64_t off = 0; off < ph[i].filesz; off += PAGE_SIZE) {
            size_t n = MIN(PAGE_SIZE, ph[i].filesz - off);

            if (vfs_read(v, buf, n, ph[i].offset + off) != (int64_t)n) {
                ret = -EIO;
                goto out;
            }
            if ((ret = copy_to_space(space, va + off, buf, n)))
                goto out;
        }
        top = MAX(top, end);
    }
    if (eh.entry < USER_REGION_BASE || eh.entry >= top) {
        ret = -ENOEXEC;
        goto out;
    }
    *entry = eh.entry;
    *brk = ALIGN_UP(top, PAGE_SIZE);
    ret = 0;
out:
    kfree(buf);
    kfree(ph);
    return ret;
}

static size_t count_strings(char *const *list)
{
    size_t n = 0;

    while (list && list[n])
        n++;
    return n;
}

// Builds argc, argv[], envp[] and an empty auxv at the top of the stack.
static int setup_stack(uint64_t space, char *const argv[], char *const envp[], uint64_t *rsp)
{
    size_t argc = count_strings(argv), envc = count_strings(envp), strings = 0;
    uint64_t sp = USER_STACK_TOP, *ptrs;
    size_t nptrs = 1 + argc + 1 + envc + 1 + 2;
    int ret;

    for (size_t i = 0; i < argc; i++)
        strings += strlen(argv[i]) + 1;
    for (size_t i = 0; i < envc; i++)
        strings += strlen(envp[i]) + 1;
    if (strings + nptrs * 8 > USER_ARG_MAX)
        return -E2BIG;
    if ((ret = map_zero(space, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP)))
        return ret;
    if (!(ptrs = kmalloc(nptrs * 8)))
        return -ENOMEM;

    size_t k = 0;
    ptrs[k++] = argc;
    for (size_t i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        copy_to_space(space, sp, argv[i], len);
        ptrs[k++] = sp;
    }
    ptrs[k++] = 0;
    for (size_t i = 0; i < envc; i++) {
        size_t len = strlen(envp[i]) + 1;
        sp -= len;
        copy_to_space(space, sp, envp[i], len);
        ptrs[k++] = sp;
    }
    ptrs[k++] = 0;
    ptrs[k++] = 0;
    ptrs[k++] = 0;

    sp = ALIGN_DOWN(sp - nptrs * 8, 16);
    ret = copy_to_space(space, sp, ptrs, nptrs * 8);
    kfree(ptrs);
    *rsp = sp;
    return ret;
}

// argv and envp must be kernel memory.
int process_spawn(const char *path, char *const argv[], char *const envp[],
                  struct process *parent, int *pid_out)
{
    struct process *p;
    struct vnode *v;
    const struct cred *cred = parent ? &parent->cred : &root_cred;
    struct vnode *cwd = parent ? parent->cwd : NULL;
    uint64_t entry = 0, rsp = 0, flags;
    const char *base;
    int ret;

    if ((ret = vfs_lookup(path, cwd, cred, true, &v)))
        return ret;
    if (!S_ISREG(v->mode)) {
        vput(v);
        return -EACCES;
    }
    if ((ret = vfs_permission(v, cred, X_OK))) {
        vput(v);
        return ret;
    }

    if (!(p = kzalloc(sizeof(*p)))) {
        vput(v);
        return -ENOMEM;
    }
    p->space = paging_create_space();
    if (!p->space) {
        ret = -ENOMEM;
        goto fail;
    }
    if ((ret = load_elf(v, p->space, &entry, &p->brk_start)))
        goto fail;
    p->brk = p->brk_start;
    if ((ret = setup_stack(p->space, argv, envp, &rsp)))
        goto fail;
    vput(v);
    v = NULL;

    base = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/')
            base = s + 1;
    }
    memcpy(p->name, base, strnlen(base, PROC_NAME_MAX - 1));
    p->cred = *cred;
    p->cwd = cwd ? cwd : vfs_root();
    vref(p->cwd);
    if (!parent) {
        struct file *console = tty_open();

        if (console) {
            p->fds[0] = p->fds[1] = p->fds[2] = console;
            console->refs = 3;
        }
    } else {
        for (int fd = 0; fd < MAX_FDS; fd++) {
            if (parent->fds[fd] && !parent->cloexec[fd]) {
                file_ref(parent->fds[fd]);
                p->fds[fd] = parent->fds[fd];
            }
        }
    }

    p->thread = thread_create_user_at(p->name, p->space, entry, rsp, p);
    if (!p->thread) {
        ret = -ENOMEM;
        goto fail;
    }

    flags = spin_lock_irqsave(&tree.lock);
    p->pid = next_pid++;
    p->parent = parent;
    if (parent) {
        p->sibling = parent->children;
        parent->children = p;
    } else if (!init_process) {
        init_process = p;
    }
    p->next_all = all;
    all = p;
    *pid_out = p->pid;
    spin_unlock_irqrestore(&tree.lock, flags);

    thread_start_ready(p->thread);
    return 0;

fail:
    vput(v);
    for (int fd = 0; fd < MAX_FDS; fd++)
        file_put(p->fds[fd]);
    vput(p->cwd);
    if (p->space)
        paging_destroy_space(p->space);
    kfree(p);
    return ret;
}

void process_exit(int status)
{
    struct process *p = process_current();
    uint64_t flags;

    if (!p)
        thread_exit(status);
    if (p == init_process)
        panic("init exited with status %d", status);

    for (int fd = 0; fd < MAX_FDS; fd++) {
        file_put(p->fds[fd]);
        p->fds[fd] = NULL;
    }
    vput(p->cwd);
    p->cwd = NULL;

    write_cr3(paging_kernel_space());
    sched_current()->space = paging_kernel_space();
    paging_destroy_space(p->space);
    p->space = 0;

    flags = spin_lock_irqsave(&tree.lock);
    while (p->children) {
        struct process *c = p->children;

        p->children = c->sibling;
        c->parent = init_process;
        c->sibling = init_process->children;
        init_process->children = c;
    }
    p->exit_status = status;
    p->zombie = true;
    p->thread = NULL;
    wake_up_locked(&tree);
    spin_unlock_irqrestore(&tree.lock, flags);

    thread_exit(status);
}

static void unlink_all(struct process *p)
{
    for (struct process **pp = &all; *pp; pp = &(*pp)->next_all) {
        if (*pp == p) {
            *pp = p->next_all;
            return;
        }
    }
}

int process_wait(int pid, int *status)
{
    struct process *self = process_current();

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&tree.lock);
        bool any = false;

        for (struct process **pp = &self->children, *c; (c = *pp); pp = &c->sibling) {
            if (pid > 0 && c->pid != pid)
                continue;
            any = true;
            if (!c->zombie)
                continue;

            int found = c->pid;
            *pp = c->sibling;
            unlink_all(c);
            spin_unlock_irqrestore(&tree.lock, flags);
            if (status)
                *status = c->exit_status;
            kfree(c);
            return found;
        }
        if (!any) {
            spin_unlock_irqrestore(&tree.lock, flags);
            return -ECHILD;
        }
        if (self->killed) {
            spin_unlock_irqrestore(&tree.lock, flags);
            return -EINTR;
        }
        wait_queue_sleep_locked(&tree);
        irq_restore(flags);
    }
}

// `by` is the sender's credentials, or NULL for the kernel.
int process_kill(int pid, const struct cred *by)
{
    uint64_t flags = spin_lock_irqsave(&tree.lock);
    int ret = -ESRCH;

    for (struct process *p = all; p; p = p->next_all) {
        if (p->pid == pid && !p->zombie) {
            if (p == init_process || (by && by->euid != 0 && by->uid != p->cred.uid)) {
                ret = -EPERM;
                break;
            }
            p->killed = true;
            if (p->thread)
                sched_wake(p->thread);
            ret = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&tree.lock, flags);
    return ret;
}

void process_check_killed(void)
{
    struct process *p = process_current();

    if (p && p->killed)
        process_exit(-9);
}

int process_count(void)
{
    int n = 0;

    for (struct process *p = all; p; p = p->next_all)
        n++;
    return n;
}

void process_list(void)
{
    uint64_t flags = spin_lock_irqsave(&tree.lock);

    for (struct process *p = all; p; p = p->next_all)
        kprintf("  %5d  %5d  %5u  %-8s  %s\n", p->pid, p->parent ? p->parent->pid : 0, p->cred.euid,
                p->zombie ? "zombie" : "running", p->name);
    spin_unlock_irqrestore(&tree.lock, flags);
}
