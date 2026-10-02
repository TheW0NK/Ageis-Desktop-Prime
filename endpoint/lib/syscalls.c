#include "aegis.h"

char **environ;

long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;

    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

long __check(long r);
#define check __check

long __check(long r)
{
    if (r < 0 && r > -4096) {
        errno = -r;
        return -1;
    }
    return r;
}

void malloc_init(void);
void thread_init_main(void);

void __libc_init(char **envp)
{
    environ = envp;
    thread_init_main();
    malloc_init();
}

void exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

ssize_t read(int fd, void *buf, size_t size) { return check(syscall3(SYS_READ, fd, buf, size)); }
ssize_t write(int fd, const void *buf, size_t size) { return check(syscall3(SYS_WRITE, fd, buf, size)); }

int open(const char *path, int flags, ...)
{
    va_list ap;
    uint32_t mode = 0;

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, uint32_t);
        va_end(ap);
    }
    return check(syscall3(SYS_OPEN, path, flags, mode));
}

int close(int fd) { return check(syscall1(SYS_CLOSE, fd)); }
int64_t lseek(int fd, int64_t off, int whence) { return check(syscall3(SYS_LSEEK, fd, off, whence)); }
int stat(const char *p, struct aegis_stat *st) { return check(syscall2(SYS_STAT, p, st)); }
int fstat(int fd, struct aegis_stat *st) { return check(syscall2(SYS_FSTAT, fd, st)); }
int lstat(const char *p, struct aegis_stat *st) { return check(syscall2(SYS_LSTAT, p, st)); }
ssize_t getdents(int fd, void *buf, size_t size) { return check(syscall3(SYS_GETDENTS, fd, buf, size)); }
int mkdir(const char *p, uint32_t mode) { return check(syscall2(SYS_MKDIR, p, mode)); }
int rmdir(const char *p) { return check(syscall1(SYS_RMDIR, p)); }
int unlink(const char *p) { return check(syscall1(SYS_UNLINK, p)); }
int rename(const char *a, const char *b) { return check(syscall2(SYS_RENAME, a, b)); }
int chdir(const char *p) { return check(syscall1(SYS_CHDIR, p)); }

char *getcwd(char *buf, size_t size)
{
    return check(syscall2(SYS_GETCWD, buf, size)) < 0 ? NULL : buf;
}

int spawn(const char *path, char *const argv[], char *const envp[])
{
    return check(syscall3(SYS_SPAWN, path, argv, envp));
}

int waitpid(int pid, int *status, int options) { return check(syscall3(SYS_WAIT, pid, status, options)); }
int getpid(void) { return syscall0(SYS_GETPID); }
int getppid(void) { return syscall0(SYS_GETPPID); }
int msleep(uint64_t ms) { return check(syscall1(SYS_SLEEP, ms)); }
int yield(void) { return syscall0(SYS_YIELD); }
uint64_t uptime_ms(void) { return syscall0(SYS_UPTIME); }

int64_t time(int64_t *out)
{
    int64_t t = syscall0(SYS_TIME);

    if (out)
        *out = t;
    return t;
}

void *sbrk(intptr_t increment)
{
    uintptr_t cur = syscall1(SYS_BRK, 0);

    if (increment == 0)
        return (void *)cur;
    if ((uintptr_t)syscall1(SYS_BRK, cur + increment) != cur + increment) {
        errno = ENOMEM;
        return (void *)-1;
    }
    return (void *)cur;
}

int dup(int fd) { return check(syscall1(SYS_DUP, fd)); }
int dup2(int a, int b) { return check(syscall2(SYS_DUP2, a, b)); }
int chmod(const char *p, uint32_t mode) { return check(syscall2(SYS_CHMOD, p, mode)); }
int chown(const char *p, uint32_t u, uint32_t g) { return check(syscall3(SYS_CHOWN, p, u, g)); }
int truncate(const char *p, uint64_t s) { return check(syscall2(SYS_TRUNCATE, p, s)); }
int ftruncate(int fd, uint64_t s) { return check(syscall2(SYS_FTRUNCATE, fd, s)); }
int symlink(const char *t, const char *p) { return check(syscall2(SYS_SYMLINK, t, p)); }
ssize_t readlink(const char *p, char *b, size_t s) { return check(syscall3(SYS_READLINK, p, b, s)); }
int link(const char *a, const char *b) { return check(syscall2(SYS_LINK, a, b)); }
int sync(void) { return check(syscall0(SYS_SYNC)); }
uint32_t getuid(void) { return syscall0(SYS_GETUID); }
uint32_t geteuid(void) { return syscall0(SYS_GETEUID); }
uint32_t getgid(void) { return syscall0(SYS_GETGID); }
uint32_t getegid(void) { return syscall0(SYS_GETEGID); }
int seteuid(uint32_t uid) { return check(syscall1(SYS_SETEUID, uid)); }
int login(const char *u, const char *p) { return check(syscall2(SYS_LOGIN, u, p)); }
int sudo(const char *p) { return check(syscall1(SYS_SUDO, p)); }
int become(uint32_t uid) { return check(syscall1(SYS_BECOME, uid)); }
int inspect(int pid, int what, void *buf, size_t size) { return check(syscall4(SYS_INSPECT, pid, what, buf, size)); }
int reboot(int cmd) { return check(syscall1(SYS_REBOOT, cmd)); }
int uname(struct aegis_utsname *u) { return check(syscall1(SYS_UNAME, u)); }
int kill(int pid, int sig) { return check(syscall2(SYS_KILL, pid, sig)); }
long ioctl(int fd, unsigned long cmd, unsigned long arg) { return check(syscall3(SYS_IOCTL, fd, cmd, arg)); }
int access(const char *p, int mode) { return check(syscall2(SYS_ACCESS, p, mode)); }
int utime(const char *p, int64_t a, int64_t m) { return check(syscall3(SYS_UTIME, p, a, m)); }
int statfs(const char *p, struct aegis_statfs *st) { return check(syscall2(SYS_STATFS, p, st)); }

void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off)
{
    long r = syscall6(SYS_MMAP, (long)addr, len, prot, flags, fd, off);

    if (r < 0 && r > -4096) {
        errno = -r;
        return MAP_FAILED;
    }
    return (void *)r;
}

int munmap(void *a, size_t l) { return check(syscall2(SYS_MUNMAP, a, l)); }
int mprotect(void *a, size_t l, int p) { return check(syscall3(SYS_MPROTECT, a, l, p)); }
int shm_create(size_t size, int flags) { return check(syscall2(SYS_SHM_CREATE, size, flags)); }
int pipe(int fds[2]) { return check(syscall2(SYS_PIPE, fds, 0)); }
int pipe2(int fds[2], int flags) { return check(syscall2(SYS_PIPE, fds, flags)); }
int openpty(int fds[2], int flags) { return check(syscall2(SYS_OPENPTY, fds, flags)); }
int poll(struct pollfd *fds, size_t n, int t) { return check(syscall3(SYS_POLL, fds, n, (long)t)); }
int fcntl(int fd, int cmd, long arg) { return check(syscall3(SYS_FCNTL, fd, cmd, arg)); }
int procinfo(struct aegis_procinfo *b, int max) { return check(syscall2(SYS_PROCINFO, b, max)); }
int sysinfo(struct aegis_sysinfo *i) { return check(syscall1(SYS_SYSINFO, i)); }
