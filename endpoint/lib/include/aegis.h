#ifndef AEGIS_LIBC_H
#define AEGIS_LIBC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/errno.h"
#include "abi/fs.h"
#include "abi/mman.h"
#include "abi/poll.h"
#include "abi/proc.h"
#include "abi/signal.h"
#include "abi/socket.h"
#include "abi/syscall.h"

typedef int64_t ssize_t;

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

// errno is per thread.
int *__errno_location(void);
#define errno (*__errno_location())
extern char **environ;

long syscall6(long n, long a, long b, long c, long d, long e, long f);
// Turns a raw syscall result into -1 and errno on failure.
long __check(long r);
#define syscall0(n)             syscall6(n, 0, 0, 0, 0, 0, 0)
#define syscall1(n, a)          syscall6(n, (long)(a), 0, 0, 0, 0, 0)
#define syscall2(n, a, b)       syscall6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define syscall3(n, a, b, c)    syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define syscall4(n, a, b, c, d) syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define syscall5(n, a, b, c, d, e) syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

// System calls. On failure they return -1 and set errno.
__attribute__((noreturn)) void exit(int status);
ssize_t read(int fd, void *buf, size_t size);
ssize_t write(int fd, const void *buf, size_t size);
int open(const char *path, int flags, ...);
int close(int fd);
int64_t lseek(int fd, int64_t off, int whence);
int stat(const char *path, struct aegis_stat *st);
int fstat(int fd, struct aegis_stat *st);
int lstat(const char *path, struct aegis_stat *st);
ssize_t getdents(int fd, void *buf, size_t size);
int mkdir(const char *path, uint32_t mode);
int rmdir(const char *path);
int unlink(const char *path);
int rename(const char *from, const char *to);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int spawn(const char *path, char *const argv[], char *const envp[]);
int waitpid(int pid, int *status, int options);
int getpid(void);
int getppid(void);
int msleep(uint64_t ms);
int yield(void);
uint64_t uptime_ms(void);
int64_t time(int64_t *out);
void *sbrk(intptr_t increment);
int dup(int fd);
int dup2(int old, int new);
int chmod(const char *path, uint32_t mode);
int chown(const char *path, uint32_t uid, uint32_t gid);
int truncate(const char *path, uint64_t size);
int ftruncate(int fd, uint64_t size);
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t size);
int link(const char *existing, const char *path);
int sync(void);
uint32_t getuid(void);
uint32_t geteuid(void);
uint32_t getgid(void);
uint32_t getegid(void);
int seteuid(uint32_t uid);
int login(const char *user, const char *password);
int sudo(const char *password);
int reboot(int cmd);
int uname(struct aegis_utsname *u);
int kill(int pid, int sig);
long ioctl(int fd, unsigned long cmd, unsigned long arg);
int access(const char *path, int mode);
int utime(const char *path, int64_t atime, int64_t mtime);
int statfs(const char *path, struct aegis_statfs *st);

// Memory, pipes, polling and descriptors.
void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t offset);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int shm_create(size_t size, int flags);     // shared memory object; mmap it with MAP_SHARED
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int poll(struct pollfd *fds, size_t n, int timeout_ms);
int fcntl(int fd, int cmd, long arg);
int procinfo(struct aegis_procinfo *buf, int max);
int sysinfo(struct aegis_sysinfo *info);

// Signals.
typedef void (*sighandler_t)(int);
#undef SIG_DFL
#undef SIG_IGN
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct aegis_sigaction *act, struct aegis_sigaction *old);
int sigprocmask(int how, const uint64_t *set, uint64_t *old);
int sigsuspend(const uint64_t *mask);
int raise(int sig);
__attribute__((noreturn)) void abort(void);

// Threads. Mutexes and condition variables need no initialisation beyond
// being zeroed.
typedef struct { volatile uint32_t state; } mutex_t;
typedef struct { volatile uint32_t seq; } cond_t;
typedef struct thread *thread_t;
int thread_create(thread_t *out, void *(*fn)(void *), void *arg);
int thread_join(thread_t t, void **result);
__attribute__((noreturn)) void thread_exit(void *result);
thread_t thread_self(void);
int gettid(void);
void mutex_lock(mutex_t *m);
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);
void cond_wait(cond_t *c, mutex_t *m);
bool cond_timedwait(cond_t *c, mutex_t *m, uint64_t timeout_ms);     // false on timeout
void cond_signal(cond_t *c);
void cond_broadcast(cond_t *c);
int futex_wait(volatile uint32_t *addr, uint32_t val, int64_t timeout_ms);
int futex_wake(volatile uint32_t *addr, int count);

// Sockets.
int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int fds[2]);
int bind(int fd, const void *addr, uint32_t len);
int listen(int fd, int backlog);
int accept(int fd, void *addr, uint32_t *len);
int accept4(int fd, void *addr, uint32_t *len, int flags);
int connect(int fd, const void *addr, uint32_t len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const void *addr, uint32_t alen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, void *addr, uint32_t *alen);
ssize_t sendmsg(int fd, const struct aegis_msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct aegis_msghdr *msg, int flags);
int shutdown(int fd, int how);
int getsockopt(int fd, int level, int opt, void *val, uint32_t *len);
int setsockopt(int fd, int level, int opt, const void *val, uint32_t len);
int getsockname(int fd, void *addr, uint32_t *len);
int getpeername(int fd, void *addr, uint32_t *len);
// AF_UNIX helpers: name is a path, or "@name" for an abstract socket.
int unix_listen(const char *name, int type);
int unix_connect(const char *name, int type);
// Sends or receives data together with file descriptors.
ssize_t send_fds(int fd, const void *buf, size_t len, const int *fds, int nfds);
ssize_t recv_fds(int fd, void *buf, size_t len, int *fds, int *nfds);

// string.h
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *s);
size_t strlcpy(char *dst, const char *src, size_t size);
const char *strerror(int err);

// stdlib.h
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int atoi(const char *s);
char *getenv(const char *name);
int setenv(const char *name, const char *value);
int isspace(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

// stdio.h
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vdprintf(int fd, const char *fmt, va_list ap);
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int puts(const char *s);
int putchar(int c);
int fputs_fd(const char *s, int fd);
void perror(const char *msg);
ssize_t read_line(int fd, char *buf, size_t size);

// Directory reading.
struct dir_stream {
    int fd;
    size_t pos, len;
    char buf[4096];
};
struct dir_stream *opendir(const char *path);
struct aegis_dirent *readdir(struct dir_stream *d);
void closedir(struct dir_stream *d);

#endif
