#include "aegis.h"

// Threads, futex-based mutexes and condition variables, and per-thread
// errno. Each thread's control block is reached through %fs:0.

#define STACK_SIZE  (1024 * 1024)
#define GUARD_SIZE  4096

struct thread {
    struct thread *self;
    int errno_value;
    volatile uint32_t alive;        // zeroed (and futex-woken) by the kernel at exit
    int tid;
    void *(*fn)(void *);
    void *arg;
    void *result;
    void *map;
    size_t map_len;
};

static struct thread main_thread;

static struct thread *current(void)
{
    struct thread *t;

    __asm__ volatile ("mov %%fs:0, %0" : "=r"(t));
    return t;
}

int *__errno_location(void)
{
    return &current()->errno_value;
}

void thread_init_main(void)
{
    main_thread.self = &main_thread;
    main_thread.alive = 1;
    syscall1(SYS_SET_TLS, &main_thread);
    main_thread.tid = syscall0(SYS_GETTID);
}

thread_t thread_self(void)
{
    return current();
}

int gettid(void)
{
    return current()->tid;
}

int futex_wait(volatile uint32_t *addr, uint32_t val, int64_t timeout_ms)
{
    long r = syscall3(SYS_FUTEX_WAIT, addr, val, timeout_ms);

    if (r < 0) {
        errno = -r;
        return -1;
    }
    return 0;
}

int futex_wake(volatile uint32_t *addr, int count)
{
    return syscall2(SYS_FUTEX_WAKE, addr, count);
}

static void thread_start(struct thread *t)
{
    t->tid = syscall0(SYS_GETTID);
    thread_exit(t->fn(t->arg));
}

int thread_create(thread_t *out, void *(*fn)(void *), void *arg)
{
    size_t len = STACK_SIZE + GUARD_SIZE;
    uint8_t *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct thread *t;
    long tid;

    if (map == MAP_FAILED)
        return -1;
    mprotect(map, GUARD_SIZE, PROT_NONE);
    t = (struct thread *)(((uintptr_t)(map + len) - sizeof(*t)) & ~(uintptr_t)63);
    memset(t, 0, sizeof(*t));
    t->self = t;
    t->alive = 1;
    t->fn = fn;
    t->arg = arg;
    t->map = map;
    t->map_len = len;
    tid = syscall5(SYS_THREAD_CREATE, thread_start, ((uintptr_t)t) & ~(uintptr_t)15, t, t, &t->alive);
    if (tid < 0) {
        munmap(map, len);
        errno = -tid;
        return -1;
    }
    *out = t;
    return 0;
}

void thread_exit(void *result)
{
    current()->result = result;
    syscall1(SYS_THREAD_EXIT, 0);
    __builtin_unreachable();
}

int thread_join(thread_t t, void **result)
{
    uint32_t v;

    if (t == current() || t == &main_thread) {
        errno = EDEADLK;
        return -1;
    }
    while ((v = t->alive) != 0)
        futex_wait(&t->alive, v, -1);
    if (result)
        *result = t->result;
    munmap(t->map, t->map_len);
    return 0;
}

// Mutex states: 0 unlocked, 1 locked, 2 locked with waiters.
bool mutex_trylock(mutex_t *m)
{
    return __sync_bool_compare_and_swap(&m->state, 0, 1);
}

void mutex_lock(mutex_t *m)
{
    uint32_t c = __sync_val_compare_and_swap(&m->state, 0, 1);

    if (c == 0)
        return;
    if (c != 2)
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        futex_wait(&m->state, 2, -1);
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    }
}

void mutex_unlock(mutex_t *m)
{
    if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
        futex_wake(&m->state, 1);
    }
}

bool cond_timedwait(cond_t *c, mutex_t *m, uint64_t timeout_ms)
{
    uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    int r;

    mutex_unlock(m);
    r = futex_wait(&c->seq, seq, (int64_t)timeout_ms);
    mutex_lock(m);
    return !(r < 0 && errno == ETIMEDOUT);
}

void cond_wait(cond_t *c, mutex_t *m)
{
    cond_timedwait(c, m, (uint64_t)-1);
}

void cond_signal(cond_t *c)
{
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    futex_wake(&c->seq, 1);
}

void cond_broadcast(cond_t *c)
{
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    futex_wake(&c->seq, 0x7FFFFFFF);
}
