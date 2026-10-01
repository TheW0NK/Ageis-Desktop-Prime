#include "sync.h"
#include "sched.h"

void wait_queue_sleep_locked(struct wait_queue *q)
{
    struct thread *t = sched_current();

    t->state = THREAD_BLOCKED;
    t->wait_next = q->head;
    q->head = t;
    spin_unlock(&q->lock);
    sched_yield();
}

void wake_up_locked(struct wait_queue *q)
{
    struct thread *t = q->head;

    q->head = NULL;
    while (t) {
        struct thread *next = t->wait_next;

        t->wait_next = NULL;
        sched_wake(t);
        t = next;
    }
}

void wake_up(struct wait_queue *q)
{
    uint64_t flags = spin_lock_irqsave(&q->lock);

    wake_up_locked(q);
    spin_unlock_irqrestore(&q->lock, flags);
}

void mutex_lock(struct mutex *m)
{
    struct thread *self = sched_current();

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&m->lock);

        if (!m->owner) {
            m->owner = self;
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
