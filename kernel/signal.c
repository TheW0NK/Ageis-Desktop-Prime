#include "process.h"
#include "mem.h"
#include "string.h"
#include "syscall.h"

extern struct wait_queue process_tree;
extern struct process *process_all;

#define UNBLOCKABLE     (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))
#define USER_RFLAGS     0xED5ULL        // CF PF AF ZF SF DF OF

static bool default_ignore(int sig)
{
    return sig == SIGCHLD || sig == SIGWINCH || sig == SIGCONT;
}

static bool default_stop(int sig)
{
    return sig == SIGSTOP || sig == SIGTSTP;
}

static bool ignored(struct process *p, int sig)
{
    uint64_t h = p->sigactions[sig - 1].handler;

    if (sig == SIGKILL || sig == SIGSTOP)
        return false;
    return h == SIG_IGN || (h == SIG_DFL && default_ignore(sig));
}

static struct process *find(int pid)
{
    for (struct process *p = process_all; p; p = p->next_all) {
        if (p->pid == pid && !p->zombie)
            return p;
    }
    return NULL;
}

int signal_send(int pid, int sig, const struct cred *by)
{
    uint64_t flags;
    struct process *p;
    int ret = 0;

    if (sig < 0 || sig > NSIG)
        return -EINVAL;
    flags = spin_lock_irqsave(&process_tree.lock);
    if (!(p = find(pid))) {
        ret = -ESRCH;
    } else if (by && by->euid != 0 && by->uid != p->cred.uid && by->euid != p->cred.uid) {
        ret = -EPERM;
    } else if (p->pid == 1 && by && sig) {
        ret = -EPERM;               // init cannot be signalled from user space
    } else if (sig && !p->exiting) {
        if (sig == SIGCONT || sig == SIGKILL) {
            p->stopped = false;
            p->sig_pending &= ~(SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP));
            wake_up_locked(&p->stop_wq);
        }
        if (default_stop(sig))
            p->sig_pending &= ~SIGBIT(SIGCONT);
        if (!ignored(p, sig) || sig == SIGCONT) {
            p->sig_pending |= SIGBIT(sig);
            for (struct thread *t = p->threads; t; t = t->proc_next)
                sched_wake(t);
        }
    }
    spin_unlock_irqrestore(&process_tree.lock, flags);
    return ret;
}

void signal_thread(struct thread *t, int sig)
{
    __atomic_fetch_or(&t->sig_pending, SIGBIT(sig), __ATOMIC_SEQ_CST);
    sched_wake(t);
}

static uint64_t deliverable(struct process *p, struct thread *t)
{
    return (p->sig_pending | t->sig_pending) & (~t->sig_mask | UNBLOCKABLE);
}

bool signal_pending(void)
{
    struct process *p = process_current();
    struct thread *t = sched_current();
    uint64_t set;

    if (!p)
        return false;
    if (p->exiting)
        return true;
    set = deliverable(p, t);
    for (int sig = 1; set; sig++, set >>= 1) {
        if ((set & 1) && !ignored(p, sig))
            return true;
    }
    return false;
}

// Takes the next signal to act on, or 0. Ignored ones are discarded.
static int dequeue(struct process *p, struct thread *t)
{
    uint64_t flags = spin_lock_irqsave(&process_tree.lock);
    uint64_t set = deliverable(p, t);
    int sig = 0;

    for (int s = 1; s <= NSIG && !sig; s++) {
        uint64_t bit = SIGBIT(s);

        if (!(set & bit))
            continue;
        if (t->sig_pending & bit)
            __atomic_fetch_and(&t->sig_pending, ~bit, __ATOMIC_SEQ_CST);
        else
            p->sig_pending &= ~bit;
        if (!ignored(p, s))
            sig = s;
    }
    spin_unlock_irqrestore(&process_tree.lock, flags);
    return sig;
}

static void wait_while_stopped(struct process *p)
{
    uint64_t flags = spin_lock_irqsave(&process_tree.lock);

    for (;;) {
        wait_prepare();
        if (!p->stopped || p->exiting || (p->sig_pending & SIGBIT(SIGKILL)))
            break;
        spin_lock(&p->stop_wq.lock);
        spin_unlock(&process_tree.lock);
        wait_queue_sleep_locked(&p->stop_wq);
        spin_lock(&process_tree.lock);
    }
    spin_unlock_irqrestore(&process_tree.lock, flags);
}

// Builds a signal frame on the user stack and points the thread at the
// handler. Returns false if the stack is unusable.
static bool enter_handler(struct interrupt_frame *f, struct process *p, struct thread *t, int sig)
{
    struct aegis_sigaction *act = &p->sigactions[sig - 1];
    struct aegis_ucontext *uc = kmalloc(sizeof(*uc) + 16);
    struct aegis_ucontext *ucp;
    struct aegis_siginfo info = { .signo = sig };
    uint64_t sp = f->rsp - 128, uc_addr, info_addr, frame;
    bool ok;

    if (!uc)
        return false;
    ucp = (struct aegis_ucontext *)ALIGN_UP((uint64_t)uc, 16);
    memset(ucp, 0, sizeof(*ucp));
    memcpy(ucp->gregs, f, 15 * sizeof(uint64_t));
    ucp->gregs[15] = f->rip;
    ucp->gregs[16] = f->rflags;
    ucp->gregs[17] = f->rsp;
    ucp->mask = t->sig_mask;
    __asm__ volatile ("fxsave64 %0" : "=m"(*(uint8_t (*)[512])ucp->fpu));

    uc_addr = ALIGN_DOWN(sp - sizeof(*ucp), 16);
    info_addr = ALIGN_DOWN(uc_addr - sizeof(info), 8);
    // [restorer][uc pointer] with rsp % 16 == 8 at handler entry.
    frame = ALIGN_DOWN(info_addr - 16, 16) - 8;
    ok = copy_to_user(uc_addr, ucp, sizeof(*ucp)) == 0
      && copy_to_user(info_addr, &info, sizeof(info)) == 0
      && copy_to_user(frame, &act->restorer, 8) == 0
      && copy_to_user(frame + 8, &uc_addr, 8) == 0;
    kfree(uc);
    if (!ok)
        return false;

    f->rip = act->handler;
    f->rsp = frame;
    f->rdi = sig;
    f->rsi = info_addr;
    f->rdx = uc_addr;
    f->rax = 0;
    f->rflags &= ~((1ULL << 10) | (1ULL << 8));     // clear DF and TF
    t->sig_mask |= act->mask | ((act->flags & SA_NODEFER) ? 0 : SIGBIT(sig));
    t->sig_mask &= ~UNBLOCKABLE;
    return true;
}

static void deliver(struct interrupt_frame *f)
{
    struct process *p = process_current();
    struct thread *t = sched_current();
    int sig;

    if (!p)
        return;
    for (;;) {
        if (p->exiting)
            process_thread_exit(p->exit_status);
        if (p->stopped)
            wait_while_stopped(p);
        if (p->exiting)
            continue;
        if (!(sig = dequeue(p, t)))
            return;

        uint64_t h = p->sigactions[sig - 1].handler;

        if (sig == SIGKILL || h == SIG_DFL) {
            if (default_stop(sig)) {
                p->stopped = true;
                continue;
            }
            if (sig == SIGCONT)
                continue;
            process_exit(sig);
        }
        if (!enter_handler(f, p, t, sig)) {
            kprintf("%s (pid %d): cannot deliver signal %d; stack unusable\n", p->name, p->pid, sig);
            process_exit(SIGSEGV);
        }
        return;
    }
}

void signal_deliver(struct interrupt_frame *frame)
{
    deliver(frame);
}

int signal_return(struct interrupt_frame *f)
{
    struct thread *t = sched_current();
    struct aegis_ucontext *uc = kmalloc(sizeof(*uc) + 16), *ucp;
    uint64_t uc_addr;
    uint32_t *mxcsr;

    if (!uc)
        return -ENOMEM;
    ucp = (struct aegis_ucontext *)ALIGN_UP((uint64_t)uc, 16);
    if (copy_from_user(&uc_addr, f->rsp, 8) || copy_from_user(ucp, uc_addr, sizeof(*ucp))
        || !user_range_ok(ucp->gregs[15], 1)) {
        kfree(uc);
        process_exit(SIGSEGV);
    }
    memcpy(f, ucp->gregs, 15 * sizeof(uint64_t));
    f->rip = ucp->gregs[15];
    f->rflags = (ucp->gregs[16] & USER_RFLAGS) | 0x202;
    f->rsp = ucp->gregs[17];
    t->sig_mask = ucp->mask & ~UNBLOCKABLE;
    mxcsr = (uint32_t *)(ucp->fpu + 24);
    *mxcsr &= 0xFFFF;
    __asm__ volatile ("fxrstor64 %0" : : "m"(*(const uint8_t (*)[512])ucp->fpu));
    kfree(uc);
    return 0;
}
