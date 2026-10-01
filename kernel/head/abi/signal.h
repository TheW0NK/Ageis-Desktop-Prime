#ifndef AEGIS_ABI_SIGNAL_H
#define AEGIS_ABI_SIGNAL_H

#include <stdint.h>

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGWINCH    28
#define NSIG        64

#define SIG_DFL     ((uint64_t)0)
#define SIG_IGN     ((uint64_t)1)

#define SA_RESTART  0x10000000      // accepted for compatibility; syscalls return EINTR
#define SA_NODEFER  0x40000000

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGBIT(s)   (1ULL << ((s) - 1))

struct aegis_sigaction {
    uint64_t handler;               // SIG_DFL, SIG_IGN, or void (*)(int, siginfo *, ucontext *)
    uint64_t flags;
    uint64_t mask;                  // also blocked while the handler runs
    uint64_t restorer;              // returns to the kernel via SYS_SIGRETURN
};

struct aegis_siginfo {
    int32_t signo;
    int32_t code;
    int32_t pid;                    // sender, 0 for the kernel
    uint32_t uid;
    uint64_t addr;                  // faulting address for SIGSEGV/SIGBUS
};

// Saved user state; gregs follow the order r15 ... rax, rip, rflags, rsp.
struct aegis_ucontext {
    uint64_t gregs[18];
    uint64_t mask;
    uint64_t reserved;
    uint8_t fpu[512] __attribute__((aligned(16)));
};

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) != 0)
#define WTERMSIG(s)     ((s) & 0x7F)

#endif
