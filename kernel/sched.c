#include "sched.h"
#include "apic.h"
#include "mem.h"
#include "spinlock.h"
#include "string.h"
#include "abi/errno.h"

#define MSR_FS_BASE     0xC0000100

static spinlock_t sched_lock = SPINLOCK_INIT;
static struct thread *threads;
static uint64_t next_id;
static uint8_t fpu_template[512] __attribute__((aligned(16)));
static uint64_t idle_ticks[CPU_MAX], busy_ticks[CPU_MAX];

// Captures a clean FPU/SSE state for new threads. User threads own the FPU;
// the kernel never touches it (it is built with -mgeneral-regs-only).
void fpu_init(void)
{
    uint32_t mxcsr = 0x1F80;

    __asm__ volatile ("fninit; ldmxcsr %1; fxsave64 %0" : "=m"(fpu_template) : "m"(mxcsr));
}

int thread_fpu_alloc(struct thread *t)
{
    if (!(t->fpu_alloc = kmalloc(sizeof(fpu_template) + 16)))
        return -ENOMEM;
    t->fpu = (uint8_t *)ALIGN_UP((uint64_t)t->fpu_alloc, 16);
    memcpy(t->fpu, fpu_template, sizeof(fpu_template));
    return 0;
}

uint64_t sched_idle_ticks(uint32_t cpu)
{
    return cpu < CPU_MAX ? idle_ticks[cpu] : 0;
}

uint64_t sched_busy_ticks(uint32_t cpu)
{
    return cpu < CPU_MAX ? busy_ticks[cpu] : 0;
}

static void link_thread(struct thread *t)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct thread **pp = &threads;

    while (*pp)
        pp = &(*pp)->next;
    *pp = t;
    spin_unlock_irqrestore(&sched_lock, flags);
}

// Turns the code running on `c` into that CPU's idle thread.
void sched_init_cpu(struct cpu *c)
{
    struct thread *idle = kzalloc(sizeof(*idle));

    if (!idle)
        panic("Out of memory creating the idle thread for CPU %u", c->id);
    ksnprintf(idle->name, sizeof(idle->name), "idle%u", c->id);
    idle->idle = true;
    idle->state = THREAD_RUNNING;
    idle->on_cpu = 1;
    idle->space = paging_kernel_space();
    c->idle = idle;
    c->current = idle;
}

void sched_init(void)
{
    next_id = 1;
    sched_init_cpu(this_cpu());
}

struct thread *sched_current(void)
{
    return this_cpu()->current;
}

static struct thread *new_thread(const char *name)
{
    struct thread *t = kzalloc(sizeof(*t));

    if (!t)
        return NULL;
    t->kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!t->kstack) {
        kfree(t);
        return NULL;
    }

    t->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
    memcpy(t->name, name, strnlen(name, THREAD_NAME_MAX - 1));
    t->kstack_top = ALIGN_DOWN((uint64_t)t->kstack + KERNEL_STACK_SIZE, 16);
    t->space = paging_kernel_space();
    return t;
}

static struct interrupt_frame *initial_frame(struct thread *t)
{
    struct interrupt_frame *f = (struct interrupt_frame *)(t->kstack_top - sizeof(*f));

    memset(f, 0, sizeof(*f));
    f->rflags = 0x202;
    t->frame = (uint64_t)f;
    return f;
}

static NORETURN void thread_start(thread_fn fn, void *arg)
{
    fn(arg);
    thread_exit(0);
}

struct thread *thread_create(const char *name, thread_fn fn, void *arg)
{
    struct thread *t = new_thread(name);
    struct interrupt_frame *f;

    if (!t)
        return NULL;

    f = initial_frame(t);
    f->rip = (uint64_t)thread_start;
    f->rdi = (uint64_t)fn;
    f->rsi = (uint64_t)arg;
    f->cs = GDT_KERNEL_CODE;
    f->ss = GDT_KERNEL_DATA;
    f->rsp = t->kstack_top - 8;

    t->state = THREAD_READY;
    link_thread(t);
    return t;
}

// Creates a user thread that does not run until thread_start_ready().
struct thread *thread_create_user_at(const char *name, uint64_t space, uint64_t rip, uint64_t rsp,
                                     struct process *process)
{
    struct thread *t = new_thread(name);
    struct interrupt_frame *f;

    if (!t)
        return NULL;
    t->user = true;
    t->space = space;
    t->process = process;

    f = initial_frame(t);
    f->rip = rip;
    f->cs = GDT_USER_CODE | 3;
    f->ss = GDT_USER_DATA | 3;
    f->rsp = rsp;
    t->state = THREAD_BLOCKED;
    return t;
}

// Frees a thread made by thread_create_user_at that never started.
void thread_free_unstarted(struct thread *t)
{
    kfree(t->fpu_alloc);
    kfree(t->kstack);
    kfree(t);
}

void thread_start_ready(struct thread *t)
{
    t->state = THREAD_READY;
    link_thread(t);
}

void thread_exit(int code)
{
    struct thread *t;

    cli();
    t = sched_current();
    t->exit_code = code;
    t->state = THREAD_DEAD;
    sched_yield();
    halt_forever();
}

void sched_yield(void)
{
    __asm__ volatile ("int %0" : : "i"(VECTOR_YIELD) : "memory");
}

void sched_sleep(uint64_t ms)
{
    uint64_t flags = irq_save();
    uint64_t ticks = ms * TIMER_HZ / 1000;
    struct thread *t = sched_current();

    spin_lock(&sched_lock);
    t->wake_tick = timer_ticks() + (ticks ? ticks : 1);
    t->state = THREAD_SLEEPING;
    spin_unlock(&sched_lock);
    sched_yield();
    irq_restore(flags);
}

void sched_block(void)
{
    uint64_t flags = irq_save();

    sched_current()->state = THREAD_BLOCKED;
    sched_yield();
    irq_restore(flags);
}

bool sched_block_timeout(uint64_t ms)
{
    uint64_t flags = irq_save();
    struct thread *t = sched_current();
    bool woken;

    spin_lock(&sched_lock);
    if (__atomic_load_n(&t->wake_pending, __ATOMIC_SEQ_CST)) {
        spin_unlock(&sched_lock);
        irq_restore(flags);
        return true;
    }
    if (ms == UINT64_MAX) {
        t->state = THREAD_BLOCKED;
    } else {
        uint64_t ticks = ms * TIMER_HZ / 1000;

        t->wake_tick = timer_ticks() + (ticks ? ticks : 1);
        t->state = THREAD_SLEEPING;
    }
    spin_unlock(&sched_lock);
    sched_yield();
    woken = __atomic_load_n(&t->wake_pending, __ATOMIC_SEQ_CST);
    irq_restore(flags);
    return woken;
}

void sched_wake(struct thread *t)
{
    uint64_t flags;

    if (!t)
        return;
    flags = spin_lock_irqsave(&sched_lock);
    __atomic_store_n(&t->wake_pending, 1, __ATOMIC_SEQ_CST);
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING)
        t->state = THREAD_READY;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void (*thread_reap_hook)(struct thread *t);

// Frees dead threads that no CPU is still running on. Called with sched_lock held.
static void reap(void)
{
    for (struct thread **pp = &threads, *t; (t = *pp);) {
        if (t->state != THREAD_DEAD || t->on_cpu) {
            pp = &t->next;
            continue;
        }
        *pp = t->next;
        if (thread_reap_hook)
            thread_reap_hook(t);
        kfree(t->fpu_alloc);
        kfree(t->kstack);
        kfree(t);
    }
}

static struct thread *pick_next(struct cpu *c, struct thread *prev)
{
    struct thread *start = (!prev->idle && prev->next) ? prev->next : threads;
    struct thread *t = start;

    if (!t)
        return prev->state == THREAD_READY ? prev : c->idle;

    do {
        if (t->state == THREAD_READY && (!t->on_cpu || t == prev))
            return t;
        t = t->next ? t->next : threads;
    } while (t != start);

    return c->idle;
}

uint64_t sched_switch(struct interrupt_frame *frame)
{
    struct cpu *c = this_cpu();
    struct thread *prev = c->current, *next;

    if (prev->fpu)
        __asm__ volatile ("fxsave64 %0" : "=m"(*(uint8_t (*)[512])prev->fpu));
    spin_lock(&sched_lock);
    prev->frame = (uint64_t)frame;
    if (prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;
    if (prev->idle)
        prev->state = THREAD_READY;

    next = pick_next(c, prev);
    next->state = THREAD_RUNNING;
    next->on_cpu = 1;
    c->current = next;
    c->switch_done = next != prev ? &prev->on_cpu : NULL;
    reap();
    spin_unlock(&sched_lock);

    if (next->space != read_cr3())
        write_cr3(next->space);
    if (next->kstack_top) {
        c->tss.rsp[0] = next->kstack_top;
        c->kernel_rsp = next->kstack_top;
    }
    if (next->fpu)
        __asm__ volatile ("fxrstor64 %0" : : "m"(*(const uint8_t (*)[512])next->fpu));
    if (next->user)
        wrmsr(MSR_FS_BASE, next->fs_base);
    return next->frame;
}

uint64_t sched_tick(struct interrupt_frame *frame)
{
    uint64_t now = timer_ticks();
    struct cpu *c = this_cpu();

    if (c->current == c->idle)
        idle_ticks[c->id]++;
    else
        busy_ticks[c->id]++;
    c->current->ticks++;
    spin_lock(&sched_lock);
    for (struct thread *t = threads; t; t = t->next) {
        if (t->state == THREAD_SLEEPING && t->wake_tick <= now)
            t->state = THREAD_READY;
    }
    spin_unlock(&sched_lock);
    return sched_switch(frame);
}

void sched_list(void)
{
    static const char *const states[] = { "ready", "running", "sleeping", "blocked", "dead" };
    uint64_t flags = spin_lock_irqsave(&sched_lock);

    for (uint32_t i = 0; i < cpu_count; i++)
        kprintf("  %3s  %-8s  %-6s  %s\n", "-", cpus[i].current == cpus[i].idle ? "running" : "ready",
                "kernel", cpus[i].idle->name);
    for (struct thread *t = threads; t; t = t->next)
        kprintf("  %3lu  %-8s  %-6s  %s\n", t->id, states[t->state],
                t->user ? "user" : "kernel", t->name);
    spin_unlock_irqrestore(&sched_lock, flags);
}
