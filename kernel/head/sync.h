#ifndef AEGIS_SYNC_H
#define AEGIS_SYNC_H

#include "spinlock.h"

struct thread;

// A thread waits on a queue through an entry, so it can wait on several
// queues at once (poll).
struct wait_entry {
    struct thread *thread;
    struct wait_entry *next;
};

struct wait_queue {
    spinlock_t lock;
    struct wait_entry *head;
};

#define WAIT_QUEUE_INIT { SPINLOCK_INIT, NULL }

struct mutex {
    spinlock_t lock;
    struct thread *owner;
    struct wait_queue waiters;
};

#define MUTEX_INIT { SPINLOCK_INIT, NULL, WAIT_QUEUE_INIT }

// Wait protocol: call wait_prepare() before testing the condition, then
// wait_queue_sleep_locked() if it is false. A wake-up that lands between the
// two makes the sleep return at once instead of being lost.
void wait_prepare(void);

// Sleeps until cond becomes true. cond is evaluated with q->lock held.
#define wait_event(q, cond)                                     \
    do {                                                        \
        uint64_t __f = spin_lock_irqsave(&(q)->lock);           \
        for (;;) {                                              \
            wait_prepare();                                     \
            if (cond)                                           \
                break;                                          \
            wait_queue_sleep_locked(q);                         \
            spin_lock(&(q)->lock);                              \
        }                                                       \
        spin_unlock_irqrestore(&(q)->lock, __f);                \
    } while (0)

// Called with q->lock held; returns with it released after a wake-up.
void wait_queue_sleep_locked(struct wait_queue *q);
// Like wait_queue_sleep_locked, giving up after ms milliseconds. Returns
// false on timeout.
bool wait_queue_sleep_locked_timeout(struct wait_queue *q, uint64_t ms);
void wait_queue_add(struct wait_queue *q, struct wait_entry *e);
void wait_queue_remove(struct wait_queue *q, struct wait_entry *e);
void wake_up(struct wait_queue *q);
void wake_up_locked(struct wait_queue *q);

void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

#endif
