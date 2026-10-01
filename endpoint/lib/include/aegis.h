#ifndef AEGIS_LIBC_H
#define AEGIS_LIBC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/errno.h"
#include "abi/fs.h"
#include "abi/syscall.h"

typedef int64_t ssize_t;

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

extern int errno;
extern char **environ;

long syscall6(long n, long a, long b, long c, long d, long e, long f);
#define syscall0(n)             syscall6(n, 0, 0, 0, 0, 0, 0)
#define syscall1(n, a)          syscall6(n, (long)(a), 0, 0, 0, 0, 0)
#define syscall2(n, a, b)       syscall6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define syscall3(n, a, b, c)    syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

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
int waitpid(int pid, int *status);
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
int kill(int pid);
long ioctl(int fd, unsigned long cmd, unsigned long arg);
int access(const char *path, int mode);
int utime(const char *path, int64_t atime, int64_t mtime);
int statfs(const char *path, struct aegis_statfs *st);

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
