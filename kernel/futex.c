#include "futex.h"
#include "mem.h"
#include "process.h"
#include "sched.h"
#include "spinlock.h"

// Waiters are keyed by the physical address of the futex word, so a futex
// in shared memory works across processes.

#define BUCKETS     64

struct waiter {
    struct thread *thread;
    uint64_t key;
    bool woken;
    struct waiter *next;
};

static struct {
    spinlock_t lock;
    struct waiter *head;
} buckets[BUCKETS];

static uint64_t key_for(struct process *p, uint64_t uaddr)
{
    return (uaddr & 3) ? 0 : paging_translate_in(p->space, uaddr);
}

int futex_wait(struct process *p, uint64_t uaddr, uint32_t val, uint64_t timeout_ms)
{
    struct waiter w = { sched_current(), 0, false, NULL };
    uint64_t key, flags;
    uint32_t cur;
    unsigned b;
    bool woken;

    // Fault the page in, then compare through the physical address.
    if (copy_from_user(&cur, uaddr, sizeof(cur)))
        return -EFAULT;
    if (!(key = key_for(p, uaddr)))
        return -EFAULT;
    b = (key >> 2) % BUCKETS;
    w.key = key;

    wait_prepare();
    flags = spin_lock_irqsave(&buckets[b].lock);
    if (*(volatile uint32_t *)key != val) {
        spin_unlock_irqrestore(&buckets[b].lock, flags);
        return -EAGAIN;
    }
    w.next = buckets[b].head;
    buckets[b].head = &w;
    spin_unlock_irqrestore(&buckets[b].lock, flags);

    if (!signal_pending())
        sched_block_timeout(timeout_ms);

    flags = spin_lock_irqsave(&buckets[b].lock);
    woken = w.woken;
    for (struct waiter **pp = &buckets[b].head; *pp; pp = &(*pp)->next) {
        if (*pp == &w) {
            *pp = w.next;
            break;
        }
    }
    spin_unlock_irqrestore(&buckets[b].lock, flags);
    if (woken)
        return 0;
    return signal_pending() ? -EINTR : -ETIMEDOUT;
}

int futex_wake(struct process *p, uint64_t uaddr, int count)
{
    uint64_t key = key_for(p, uaddr), flags;
    unsigned b;
    int n = 0;

    if (!key)
        return 0;
    b = (key >> 2) % BUCKETS;
    flags = spin_lock_irqsave(&buckets[b].lock);
    for (struct waiter **pp = &buckets[b].head, *w; (w = *pp) && n < count;) {
        if (w->key != key) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        w->woken = true;
        sched_wake(w->thread);
        n++;
    }
    spin_unlock_irqrestore(&buckets[b].lock, flags);
    return n;
}
