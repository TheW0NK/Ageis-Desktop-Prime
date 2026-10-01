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
    struct thread *wait_next;
};

typedef void (*thread_fn)(void *arg);

struct process;

void sched_init(void);
void sched_init_cpu(struct cpu *c);
struct thread *thread_create(const char *name, thread_fn fn, void *arg);
struct thread *thread_create_user_at(const char *name, uint64_t space, uint64_t rip, uint64_t rsp,
                                     struct process *process);
void thread_start_ready(struct thread *t);
NORETURN void thread_exit(int code);
struct thread *sched_current(void);
void sched_yield(void);
void sched_sleep(uint64_t ms);
void sched_block(void);
void sched_wake(struct thread *t);
uint64_t sched_tick(struct interrupt_frame *frame);
uint64_t sched_switch(struct interrupt_frame *frame);
void sched_list(void);

#endif
