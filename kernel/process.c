#include "process.h"
#include "apic.h"
#include "futex.h"
#include "mem.h"
#include "string.h"
#include "sync.h"
#include "tty.h"

#define PT_LOAD     1
#define ET_EXEC     2
#define EM_X86_64   62
#define PF_X        1
#define PF_W        2
#define PF_R        4

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

// Its lock guards the process tree, every process's thread list and the
// exit and stop state. Parents waiting for children sleep on it.
struct wait_queue process_tree = WAIT_QUEUE_INIT;
struct process *process_all;
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

// Writes into another address space, faulting its pages in first.
static int space_write(struct mm *mm, uint64_t va, const void *src, size_t len)
{
    const uint8_t *s = src;

    while (len) {
        uint64_t phys;
        size_t n = MIN(len, PAGE_SIZE - (va & (PAGE_SIZE - 1)));

        if (!vm_fault(mm, va, true) || !(phys = paging_translate_in(mm->space, va)))
            return -EFAULT;
        memcpy((void *)phys, s, n);
        va += n;
        s += n;
        len -= n;
    }
    return 0;
}

static uint32_t elf_prot(uint32_t flags)
{
    return ((flags & PF_R) ? PROT_READ : 0) | ((flags & PF_W) ? PROT_WRITE : 0)
         | ((flags & PF_X) ? PROT_EXEC : 0);
}

static int load_elf(struct vnode *v, struct mm *mm, uint64_t *entry, uint64_t *brk)
{
    elf_header eh;
    elf_phdr *ph;
    uint64_t top = USER_REGION_BASE, mapped_end = 0, addr;
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

    // Map writable while loading; final protections are applied afterwards.
    for (int i = 0; i < eh.phnum; i++) {
        uint64_t va = ph[i].vaddr, end = va + ph[i].memsz, start;

        if (ph[i].type != PT_LOAD || !ph[i].memsz)
            continue;
        if (va < USER_REGION_BASE || end < va || end > MMAP_BASE || ph[i].filesz > ph[i].memsz)
            goto out;
        start = MAX(ALIGN_DOWN(va, PAGE_SIZE), mapped_end);
        if (ALIGN_UP(end, PAGE_SIZE) > start) {
            ret = vm_mmap(mm, start, ALIGN_UP(end, PAGE_SIZE) - start, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, NULL, 0, &addr);
            if (ret)
                goto out;
            mapped_end = ALIGN_UP(end, PAGE_SIZE);
        }
        for (uint64_t off = 0; off < ph[i].filesz; off += PAGE_SIZE) {
            size_t n = MIN(PAGE_SIZE, ph[i].filesz - off);

            if (vfs_read(v, buf, n, ph[i].offset + off) != (int64_t)n) {
                ret = -EIO;
                goto out;
            }
            if ((ret = space_write(mm, va + off, buf, n)))
                goto out;
        }
        top = MAX(top, end);
    }
    for (int i = 0; i < eh.phnum; i++) {
        uint64_t start = ALIGN_DOWN(ph[i].vaddr, PAGE_SIZE);

        if (ph[i].type == PT_LOAD && ph[i].memsz && !(ph[i].flags & PF_W))
            vm_mprotect(mm, start, ALIGN_UP(ph[i].vaddr + ph[i].memsz, PAGE_SIZE) - start,
                        elf_prot(ph[i].flags) | PROT_READ);
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
static int setup_stack(struct mm *mm, char *const argv[], char *const envp[], uint64_t *rsp)
{
    size_t argc = count_strings(argv), envc = count_strings(envp), strings = 0;
    uint64_t sp = USER_STACK_TOP, *ptrs, addr;
    size_t nptrs = 1 + argc + 1 + envc + 1 + 2;
    int ret;

    for (size_t i = 0; i < argc; i++)
        strings += strlen(argv[i]) + 1;
    for (size_t i = 0; i < envc; i++)
        strings += strlen(envp[i]) + 1;
    if (strings + nptrs * 8 > USER_ARG_MAX)
        return -E2BIG;
    ret = vm_mmap(mm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, NULL, 0, &addr);
    if (ret)
        return ret;
    if (!(ptrs = kmalloc(nptrs * 8)))
        return -ENOMEM;

    size_t k = 0;
    ptrs[k++] = argc;
    for (size_t i = 0; i < argc && !ret; i++) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        ret = space_write(mm, sp, argv[i], len);
        ptrs[k++] = sp;
    }
    ptrs[k++] = 0;
    for (size_t i = 0; i < envc && !ret; i++) {
        size_t len = strlen(envp[i]) + 1;
        sp -= len;
        ret = space_write(mm, sp, envp[i], len);
        ptrs[k++] = sp;
    }
    ptrs[k++] = 0;
    ptrs[k++] = 0;
    ptrs[k++] = 0;

    sp = ALIGN_DOWN(sp - nptrs * 8, 16);
    if (!ret)
        ret = space_write(mm, sp, ptrs, nptrs * 8);
    kfree(ptrs);
    *rsp = sp;
    return ret;
}

static struct thread *new_user_thread(struct process *p, uint64_t rip, uint64_t rsp)
{
    struct thread *t = thread_create_user_at(p->name, p->space, rip, rsp, p);

    if (!t)
        return NULL;
    if (thread_fpu_alloc(t)) {
        thread_free_unstarted(t);
        return NULL;
    }
    return t;
}

// argv and envp must be kernel memory.
int process_spawn(const char *path, char *const argv[], char *const envp[],
                  struct process *parent, int *pid_out)
{
    struct process *p;
    struct vnode *v;
    struct thread *t;
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
    p->stop_wq = (struct wait_queue)WAIT_QUEUE_INIT;
    if (!(p->mm = mm_create())) {
        ret = -ENOMEM;
        goto fail;
    }
    p->space = p->mm->space;
    if ((ret = load_elf(v, p->mm, &entry, &p->brk_start)))
        goto fail;
    p->brk = p->brk_start;
    if ((ret = setup_stack(p->mm, argv, envp, &rsp)))
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
    p->start_ms = timer_uptime_ms();
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
        // Handlers do not survive into a new program, but ignored signals do.
        for (int s = 0; s < NSIG; s++) {
            if (parent->sigactions[s].handler == SIG_IGN)
                p->sigactions[s].handler = SIG_IGN;
        }
    }

    if (!(t = new_user_thread(p, entry, rsp))) {
        ret = -ENOMEM;
        goto fail;
    }
    if (parent)
        t->sig_mask = sched_current()->sig_mask;

    flags = spin_lock_irqsave(&process_tree.lock);
    p->pid = next_pid++;
    p->parent = parent;
    if (parent) {
        p->sibling = parent->children;
        parent->children = p;
    } else if (!init_process) {
        init_process = p;
    }
    p->next_all = process_all;
    process_all = p;
    p->threads = t;
    p->nthreads = 1;
    p->mm->users = 1;
    *pid_out = p->pid;
    spin_unlock_irqrestore(&process_tree.lock, flags);

    thread_start_ready(t);
    return 0;

fail:
    vput(v);
    for (int fd = 0; fd < MAX_FDS; fd++)
        file_put(p->fds[fd]);
    vput(p->cwd);
    if (p->mm)
        mm_destroy(p->mm);
    kfree(p);
    return ret;
}

int process_thread_create(struct process *p, uint64_t entry, uint64_t stack, uint64_t arg,
                          uint64_t tls, uint64_t clear_tid)
{
    struct thread *t;
    uint64_t flags;
    int tid;

    if (!user_range_ok(entry, 1) || !user_range_ok(stack - 8, 8))
        return -EINVAL;
    if (!(t = new_user_thread(p, entry, ALIGN_DOWN(stack, 16) - 8)))
        return -ENOMEM;
    ((struct interrupt_frame *)t->frame)->rdi = arg;
    t->fs_base = tls;
    t->clear_tid = clear_tid;
    t->sig_mask = sched_current()->sig_mask;

    flags = spin_lock_irqsave(&process_tree.lock);
    if (p->exiting) {
        spin_unlock_irqrestore(&process_tree.lock, flags);
        thread_free_unstarted(t);
        return -EINTR;
    }
    t->proc_next = p->threads;
    p->threads = t;
    p->nthreads++;
    p->mm->users++;
    tid = t->id;
    spin_unlock_irqrestore(&process_tree.lock, flags);

    thread_start_ready(t);
    return tid;
}

// Runs in the last thread of a process, which no longer uses its space.
static void teardown(struct process *p)
{
    uint64_t flags;
    struct process *parent;

    for (int fd = 0; fd < MAX_FDS; fd++) {
        file_put(p->fds[fd]);
        p->fds[fd] = NULL;
    }
    vput(p->cwd);
    p->cwd = NULL;
    mm_destroy(p->mm);
    p->mm = NULL;
    p->space = 0;

    flags = spin_lock_irqsave(&process_tree.lock);
    while (p->children) {
        struct process *c = p->children;

        p->children = c->sibling;
        c->parent = init_process;
        c->sibling = init_process->children;
        init_process->children = c;
    }
    p->zombie = true;
    parent = p->parent;
    wake_up_locked(&process_tree);
    spin_unlock_irqrestore(&process_tree.lock, flags);
    if (parent)
        signal_send(parent->pid, SIGCHLD, NULL);
}

void process_thread_exit(int status)
{
    struct process *p = process_current();
    struct thread *t = sched_current();
    uint64_t flags;
    bool last;

    if (!p)
        thread_exit(status);
    if (t->clear_tid) {
        uint32_t zero = 0;

        if (copy_to_user(t->clear_tid, &zero, sizeof(zero)) == 0)
            futex_wake(p, t->clear_tid, 1);
    }

    // Leave the address space before it can be destroyed.
    flags = irq_save();
    write_cr3(paging_kernel_space());
    t->space = paging_kernel_space();
    irq_restore(flags);

    flags = spin_lock_irqsave(&process_tree.lock);
    for (struct thread **pp = &p->threads; *pp; pp = &(*pp)->proc_next) {
        if (*pp == t) {
            *pp = t->proc_next;
            break;
        }
    }
    p->nthreads--;
    p->mm->users--;
    p->dead_ticks += t->ticks;
    last = p->nthreads == 0;
    if (last && !p->exiting) {
        p->exiting = true;
        p->exit_status = status;
    }
    spin_unlock_irqrestore(&process_tree.lock, flags);

    if (last) {
        if (p == init_process)
            panic("init exited with status 0x%x", p->exit_status);
        teardown(p);
    }
    thread_exit(status);
}

void process_exit(int status)
{
    struct process *p = process_current();
    uint64_t flags;

    if (!p)
        thread_exit(status);
    flags = spin_lock_irqsave(&process_tree.lock);
    if (!p->exiting) {
        p->exiting = true;
        p->exit_status = status;
    }
    for (struct thread *t = p->threads; t; t = t->proc_next)
        sched_wake(t);
    wake_up_locked(&p->stop_wq);
    spin_unlock_irqrestore(&process_tree.lock, flags);
    process_thread_exit(p->exit_status);
}

static void unlink_all(struct process *p)
{
    for (struct process **pp = &process_all; *pp; pp = &(*pp)->next_all) {
        if (*pp == p) {
            *pp = p->next_all;
            return;
        }
    }
}

int process_wait(int pid, int *status, bool nohang)
{
    struct process *self = process_current();

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&process_tree.lock);
        bool any = false;

        wait_prepare();
        for (struct process **pp = &self->children, *c; (c = *pp); pp = &c->sibling) {
            if (pid > 0 && c->pid != pid)
                continue;
            any = true;
            if (!c->zombie)
                continue;

            int found = c->pid;
            *pp = c->sibling;
            unlink_all(c);
            spin_unlock_irqrestore(&process_tree.lock, flags);
            if (status)
                *status = c->exit_status;
            kfree(c);
            return found;
        }
        if (!any) {
            spin_unlock_irqrestore(&process_tree.lock, flags);
            return -ECHILD;
        }
        if (nohang) {
            spin_unlock_irqrestore(&process_tree.lock, flags);
            return 0;
        }
        if (signal_pending()) {
            spin_unlock_irqrestore(&process_tree.lock, flags);
            return -EINTR;
        }
        wait_queue_sleep_locked(&process_tree);
        irq_restore(flags);
    }
}

int process_count(void)
{
    int n = 0;

    for (struct process *p = process_all; p; p = p->next_all)
        n++;
    return n;
}

uint32_t process_thread_total(void)
{
    uint64_t flags = spin_lock_irqsave(&process_tree.lock);
    uint32_t n = 0;

    for (struct process *p = process_all; p; p = p->next_all)
        n += p->nthreads;
    spin_unlock_irqrestore(&process_tree.lock, flags);
    return n;
}

int process_info(struct aegis_procinfo *out, int max)
{
    uint64_t flags = spin_lock_irqsave(&process_tree.lock);
    int n = 0;

    for (struct process *p = process_all; p && n < max; p = p->next_all, n++) {
        struct aegis_procinfo *i = &out[n];
        uint64_t ticks = p->dead_ticks;
        bool running = false;

        memset(i, 0, sizeof(*i));
        i->pid = p->pid;
        i->ppid = p->parent ? p->parent->pid : 0;
        i->uid = p->cred.uid;
        i->euid = p->cred.euid;
        i->threads = p->nthreads;
        for (struct thread *t = p->threads; t; t = t->proc_next) {
            ticks += t->ticks;
            running |= t->state == THREAD_READY || t->state == THREAD_RUNNING;
        }
        i->state = p->zombie ? PROC_ZOMBIE : p->stopped ? PROC_STOPPED
                 : running ? PROC_RUNNING : PROC_SLEEPING;
        i->cpu_ms = ticks * 1000 / TIMER_HZ;
        if (p->mm) {
            i->memory = p->mm->resident * PAGE_SIZE;
            i->virtual_memory = vm_mapped_bytes(p->mm);
        }
        i->start_ms = p->start_ms;
        memcpy(i->name, p->name, sizeof(i->name));
    }
    spin_unlock_irqrestore(&process_tree.lock, flags);
    return n;
}

void process_list(void)
{
    uint64_t flags = spin_lock_irqsave(&process_tree.lock);

    for (struct process *p = process_all; p; p = p->next_all)
        kprintf("  %5d  %5d  %5u  %3u  %-8s  %s\n", p->pid, p->parent ? p->parent->pid : 0,
                p->cred.euid, p->nthreads, p->zombie ? "zombie" : p->stopped ? "stopped" : "running",
                p->name);
    spin_unlock_irqrestore(&process_tree.lock, flags);
}

// ---- Inspection (debuggers) ----

static bool may_inspect(struct process *self, struct process *p)
{
    return self->cred.euid == 0 || self->cred.uid == p->cred.uid;
}

static uint32_t file_kind(struct file *f)
{
    if (!f->vnode)
        return FILE_KIND_OTHER;
    if (S_ISDIR(f->vnode->mode))
        return FILE_KIND_DIR;
    if (S_ISCHR(f->vnode->mode) || (f->vnode->mode & S_IFMT) == S_IFBLK)
        return FILE_KIND_DEVICE;
    return S_ISREG(f->vnode->mode) ? FILE_KIND_FILE : FILE_KIND_OTHER;
}

// Fills out (in kernel memory) with up to max records; returns the count or
// an error.
int process_inspect(int pid, int what, void *out, int max)
{
    struct process *self = process_current(), *p;
    uint64_t flags = spin_lock_irqsave(&process_tree.lock);
    int n = 0;

    for (p = process_all; p && p->pid != pid; p = p->next_all)
        ;
    if (!p || p->zombie) {
        spin_unlock_irqrestore(&process_tree.lock, flags);
        return -ESRCH;
    }
    if (!may_inspect(self, p)) {
        spin_unlock_irqrestore(&process_tree.lock, flags);
        return -EPERM;
    }
    switch (what) {
    case INSPECT_THREADS:
        for (struct thread *t = p->threads; t && n < max; t = t->proc_next, n++) {
            struct aegis_threadinfo *i = (struct aegis_threadinfo *)out + n;
            struct interrupt_frame *fr = (struct interrupt_frame *)(t->kstack_top - sizeof(*fr));

            memset(i, 0, sizeof(*i));
            i->tid = t->id;
            i->state = p->stopped ? 3 : t->state == THREAD_RUNNING ? 0 : t->state == THREAD_READY ? 1
                     : t->state == THREAD_DEAD ? 4 : 2;
            i->on_cpu = t->on_cpu;
            i->cpu_ms = t->ticks * 1000 / TIMER_HZ;
            i->fs_base = t->fs_base;
            // The frame saved when the thread last came in from user mode.
            if (t->user && t->kstack) {
                i->rip = fr->rip;
                i->rsp = fr->rsp;
                i->rbp = fr->rbp;
                i->rflags = fr->rflags;
                i->rax = fr->rax;
                i->rbx = fr->rbx;
                i->rcx = fr->rcx;
                i->rdx = fr->rdx;
                i->rsi = fr->rsi;
                i->rdi = fr->rdi;
                i->r8 = fr->r8;
                i->r9 = fr->r9;
                i->r10 = fr->r10;
                i->r11 = fr->r11;
                i->r12 = fr->r12;
                i->r13 = fr->r13;
                i->r14 = fr->r14;
                i->r15 = fr->r15;
            }
            memcpy(i->name, t->name, MIN(sizeof(i->name) - 1, sizeof(t->name)));
        }
        break;
    case INSPECT_MAPS:
        if (!p->mm) {
            break;
        }
        // Never sleep under the tree lock: if the map is being changed, ask
        // the caller to try again.
        if (!mutex_trylock(&p->mm->lock)) {
            spin_unlock_irqrestore(&process_tree.lock, flags);
            return -EAGAIN;
        }
        for (struct vma *v = p->mm->vmas; v && n < max; v = v->next, n++) {
            struct aegis_mapinfo *i = (struct aegis_mapinfo *)out + n;

            memset(i, 0, sizeof(*i));
            i->start = v->start;
            i->end = v->end;
            i->prot = v->prot;
            i->kind = v->type == VMA_FILE ? MAP_KIND_FILE : v->type == VMA_SHM ? MAP_KIND_SHARED
                    : v->type == VMA_PHYS ? MAP_KIND_DEVICE : MAP_KIND_ANON;
            i->offset = v->type == VMA_PHYS ? 0 : v->offset;
            i->ino = v->vnode ? v->vnode->ino : 0;
        }
        mutex_unlock(&p->mm->lock);
        break;
    case INSPECT_FILES:
        for (int fd = 0; fd < MAX_FDS && n < max; fd++) {
            struct file *f = p->fds[fd];
            struct aegis_fileinfo *i;

            if (!f)
                continue;
            i = (struct aegis_fileinfo *)out + n++;
            memset(i, 0, sizeof(*i));
            i->fd = fd;
            i->flags = f->flags;
            i->offset = f->offset;
            i->kind = file_kind(f);
            if (f->vnode) {
                i->mode = f->vnode->mode;
                i->ino = f->vnode->ino;
                i->size = f->vnode->size;
            }
        }
        break;
    case INSPECT_MEMORY: {
        // out holds the address on entry and the bytes on return.
        uint64_t addr = *(uint64_t *)out, space = p->space;
        uint8_t *dst = out;
        int done = 0;

        while (done < max) {
            uint64_t va = addr + done, phys = paging_translate_in(space, va);
            int chunk = MIN(max - done, (int)(PAGE_SIZE - (va & (PAGE_SIZE - 1))));

            // Only pages that are present; this never faults anything in.
            if (!phys || !user_range_ok(va, chunk))
                break;
            memcpy(dst + done, (void *)phys, chunk);
            done += chunk;
        }
        n = done;
        break;
    }
    default:
        n = -EINVAL;
    }
    spin_unlock_irqrestore(&process_tree.lock, flags);
    return n;
}
