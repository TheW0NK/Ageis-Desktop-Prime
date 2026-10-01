#ifndef AEGIS_PROCESS_H
#define AEGIS_PROCESS_H

#include "kernel.h"
#include "sched.h"
#include "vfs.h"

#define MAX_FDS             64
#define PROC_NAME_MAX       32
#define USER_STACK_TOP      (USER_REGION_END - PAGE_SIZE)
#define USER_STACK_SIZE     (256 * 1024)
#define USER_ARG_MAX        (64 * 1024)

struct process {
    int pid;
    char name[PROC_NAME_MAX];
    struct process *parent;
    struct process *children;
    struct process *sibling;
    struct process *next_all;
    struct thread *thread;
    uint64_t space;
    uint64_t brk_start, brk;
    struct file *fds[MAX_FDS];
    bool cloexec[MAX_FDS];
    struct vnode *cwd;
    struct cred cred;
    bool zombie;
    volatile bool killed;
    int exit_status;
};

struct process *process_current(void);
int process_spawn(const char *path, char *const argv[], char *const envp[],
                  struct process *parent, int *pid);
int process_wait(int pid, int *status);
NORETURN void process_exit(int status);
int process_kill(int pid, const struct cred *by);
void process_check_killed(void);
int process_count(void);
void process_list(void);

int fd_alloc(struct process *p, struct file *f, int min);
struct file *fd_get(struct process *p, int fd);

#endif
