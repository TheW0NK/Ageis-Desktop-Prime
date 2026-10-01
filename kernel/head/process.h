#ifndef AEGIS_PROCESS_H
#define AEGIS_PROCESS_H

#include "kernel.h"
#include "sched.h"
#include "vfs.h"
#include "vm.h"
#include "abi/proc.h"
#include "abi/signal.h"

#define MAX_FDS             256
#define PROC_NAME_MAX       32
#define USER_STACK_TOP      (USER_REGION_END - PAGE_SIZE)
#define USER_STACK_SIZE     (8ULL << 20)
#define USER_ARG_MAX        (64 * 1024)

struct process {
    int pid;
    char name[PROC_NAME_MAX];
    struct process *parent;
    struct process *children;
    struct process *sibling;
    struct process *next_all;
    struct thread *threads;         // linked through thread->proc_next
    uint32_t nthreads;
    struct mm *mm;
    uint64_t space;                 // mm->space
    uint64_t brk_start, brk;
    struct file *fds[MAX_FDS];
    bool cloexec[MAX_FDS];
    struct vnode *cwd;
    struct cred cred;
    bool zombie;
    volatile bool exiting;          // every thread leaves at its next chance
    volatile bool stopped;          // SIGSTOP until SIGCONT
    int exit_status;                // wait() status: (code << 8) or signal number
    struct aegis_sigaction sigactions[NSIG];
    volatile uint64_t sig_pending;
    uint64_t dead_ticks;            // CPU time of threads that have exited
    uint64_t start_ms;
    struct wait_queue stop_wq;
};

struct process *process_current(void);
int process_spawn(const char *path, char *const argv[], char *const envp[],
                  struct process *parent, int *pid);
int process_wait(int pid, int *status, bool nohang);
// Ends the whole process; status is a wait() status.
NORETURN void process_exit(int status);
// Ends the calling thread; the last thread out ends the process.
NORETURN void process_thread_exit(int status);
int process_thread_create(struct process *p, uint64_t entry, uint64_t stack, uint64_t arg,
                          uint64_t tls, uint64_t clear_tid);
int process_count(void);
void process_list(void);
int process_info(struct aegis_procinfo *out, int max);
uint32_t process_thread_total(void);

// Signals. `by` is the sender's credentials, or NULL for the kernel.
int signal_send(int pid, int sig, const struct cred *by);
void signal_thread(struct thread *t, int sig);
// True if the current thread should abandon a blocking wait.
bool signal_pending(void);
// Runs on every return to user mode: stops, exits or enters a handler.
void signal_deliver(struct interrupt_frame *frame);
// SYS_SIGRETURN: restores the state saved when the handler was entered.
int signal_return(struct interrupt_frame *frame);

int fd_alloc(struct process *p, struct file *f, int min);
struct file *fd_get(struct process *p, int fd);

#endif
