#ifndef AEGIS_SCHED_H
#define AEGIS_SCHED_H

#include "kernel.h"
#include "cpu.h"
#include "percpu.h"

#define THREAD_NAME_MAX     32
#define KERNEL_STACK_SIZE   (32 * 1024)

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

struct thread {
    uint64_t id;
    char name[THREAD_NAME_MAX];
    enum thread_state state;
    volatile uint32_t wake_pending;     // set by sched_wake; see wait_prepare()
    uint64_t frame;
    uint8_t *kstack;
    uint64_t kstack_top;
    uint64_t wake_tick;
    bool user;
    bool idle;
    volatile uint8_t on_cpu;
    uint64_t space;
    int exit_code;
    struct process *process;
    struct thread *next;
    struct thread *proc_next;           // the process's thread list
    uint8_t *fpu;                       // FXSAVE area of a user thread (16-byte aligned)
    void *fpu_alloc;
    uint64_t fs_base;
    uint64_t ticks;                     // timer ticks spent running
    uint64_t sig_mask;                  // blocked signals
    uint64_t sig_pending;               // signals sent to this thread alone
    uint64_t clear_tid;                 // user address zeroed and futex-woken at exit
};

typedef void (*thread_fn)(void *arg);

struct process;

void sched_init(void);
void sched_init_cpu(struct cpu *c);
struct thread *thread_create(const char *name, thread_fn fn, void *arg);
struct thread *thread_create_user_at(const char *name, uint64_t space, uint64_t rip, uint64_t rsp,
                                     struct process *process);
void thread_start_ready(struct thread *t);
void thread_free_unstarted(struct thread *t);
NORETURN void thread_exit(int code);
struct thread *sched_current(void);
void sched_yield(void);
void sched_sleep(uint64_t ms);
void sched_block(void);
// Blocks until sched_wake() or until ms milliseconds pass (UINT64_MAX: no
// limit). Returns at once if a wake-up arrived since wait_prepare(). Returns
// false on timeout.
bool sched_block_timeout(uint64_t ms);
void sched_wake(struct thread *t);
uint64_t sched_idle_ticks(uint32_t cpu);
uint64_t sched_busy_ticks(uint32_t cpu);
void fpu_init(void);
int thread_fpu_alloc(struct thread *t);
uint64_t sched_tick(struct interrupt_frame *frame);
uint64_t sched_switch(struct interrupt_frame *frame);
void sched_list(void);

#endif
