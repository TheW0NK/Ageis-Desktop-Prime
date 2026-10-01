#ifndef AEGIS_SYNC_H
#define AEGIS_SYNC_H

#include "spinlock.h"

struct thread;

struct wait_queue {
    spinlock_t lock;
    struct thread *head;
};

#define WAIT_QUEUE_INIT { SPINLOCK_INIT, NULL }

struct mutex {
    spinlock_t lock;
    struct thread *owner;
    struct wait_queue waiters;
};

#define MUTEX_INIT { SPINLOCK_INIT, NULL, WAIT_QUEUE_INIT }

// Sleeps until cond becomes true. cond is evaluated with q->lock held.
#define wait_event(q, cond)                                     \
    do {                                                        \
        uint64_t __f = spin_lock_irqsave(&(q)->lock);           \
        while (!(cond)) {                                       \
            wait_queue_sleep_locked(q);                         \
            spin_lock(&(q)->lock);                              \
        }                                                       \
        spin_unlock_irqrestore(&(q)->lock, __f);                \
    } while (0)

void wait_queue_sleep_locked(struct wait_queue *q);
void wake_up(struct wait_queue *q);
void wake_up_locked(struct wait_queue *q);

void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

#endif
