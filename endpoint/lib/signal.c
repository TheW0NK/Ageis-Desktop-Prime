#include "aegis.h"

void __signal_restorer(void) __attribute__((visibility("hidden")));

int sigaction(int sig, const struct aegis_sigaction *act, struct aegis_sigaction *old)
{
    struct aegis_sigaction k;

    if (act) {
        k = *act;
        k.restorer = (uint64_t)__signal_restorer;
        act = &k;
    }
    return __check(syscall3(SYS_SIGACTION, sig, act, old));
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct aegis_sigaction act = { (uint64_t)handler, SA_RESTART, 0, 0 }, old;

    if (sigaction(sig, &act, &old) < 0)
        return SIG_ERR;
    return (sighandler_t)old.handler;
}

int sigprocmask(int how, const uint64_t *set, uint64_t *old)
{
    return __check(syscall3(SYS_SIGPROCMASK, how, set, old));
}

int sigsuspend(const uint64_t *mask)
{
    return __check(syscall1(SYS_SIGSUSPEND, mask));
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

void abort(void)
{
    uint64_t unblock = SIGBIT(SIGABRT);

    sigprocmask(SIG_UNBLOCK, &unblock, NULL);
    raise(SIGABRT);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    exit(127);
}
