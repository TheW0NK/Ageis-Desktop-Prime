#include "sync.h"
#include "sched.h"

#define WAIT_FOREVER    UINT64_MAX

void wait_prepare(void)
{
    struct thread *t = sched_current();

    if (t)
        __atomic_store_n(&t->wake_pending, 0, __ATOMIC_SEQ_CST);
}

static void unlink_locked(struct wait_queue *q, struct wait_entry *e)
{
    for (struct wait_entry **pp = &q->head; *pp; pp = &(*pp)->next) {
        if (*pp == e) {
            *pp = e->next;
            return;
        }
    }
}

static bool sleep_on(struct wait_queue *q, uint64_t ms)
{
    struct wait_entry e = { sched_current(), q->head };
    bool woken;

    q->head = &e;
    spin_unlock(&q->lock);
    woken = sched_block_timeout(ms);
    spin_lock(&q->lock);
    unlink_locked(q, &e);
    spin_unlock(&q->lock);
    return woken;
}

void wait_queue_sleep_locked(struct wait_queue *q)
{
    sleep_on(q, WAIT_FOREVER);
}

bool wait_queue_sleep_locked_timeout(struct wait_queue *q, uint64_t ms)
{
    return sleep_on(q, ms);
}

void wait_queue_add(struct wait_queue *q, struct wait_entry *e)
{
    uint64_t flags = spin_lock_irqsave(&q->lock);

    e->next = q->head;
    q->head = e;
    spin_unlock_irqrestore(&q->lock, flags);
}

void wait_queue_remove(struct wait_queue *q, struct wait_entry *e)
{
    uint64_t flags = spin_lock_irqsave(&q->lock);

    unlink_locked(q, e);
    spin_unlock_irqrestore(&q->lock, flags);
}

// Wakes every waiter. Entries stay queued until their owners remove them.
void wake_up_locked(struct wait_queue *q)
{
    for (struct wait_entry *e = q->head; e; e = e->next)
        sched_wake(e->thread);
}

void wake_up(struct wait_queue *q)
{
    uint64_t flags = spin_lock_irqsave(&q->lock);

    wake_up_locked(q);
    spin_unlock_irqrestore(&q->lock, flags);
}

// Taking a mutex must not swallow a wake-up meant for a wait the caller is
// in the middle of preparing (poll takes mutexes while checking files), so
// any wake-up token seen here is handed back once the mutex is held.
void mutex_lock(struct mutex *m)
{
    struct thread *self = sched_current();
    uint32_t saved = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&m->lock);

        if (self)
            saved |= __atomic_exchange_n(&self->wake_pending, 0, __ATOMIC_SEQ_CST);
        if (!m->owner) {
            m->owner = self;
            if (saved)
                __atomic_store_n(&self->wake_pending, 1, __ATOMIC_SEQ_CST);
            spin_unlock_irqrestore(&m->lock, flags);
            return;
        }
        spin_lock(&m->waiters.lock);
        spin_unlock(&m->lock);
        wait_queue_sleep_locked(&m->waiters);
        irq_restore(flags);
    }
}

void mutex_unlock(struct mutex *m)
{
    uint64_t flags = spin_lock_irqsave(&m->lock);

    m->owner = NULL;
    spin_unlock_irqrestore(&m->lock, flags);
    wake_up(&m->waiters);
}
