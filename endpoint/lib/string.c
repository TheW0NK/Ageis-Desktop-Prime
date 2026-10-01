#include "aegis.h"

void *memset(void *dst, int c, size_t n)
{
    void *ret = dst;

    __asm__ volatile ("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    void *ret = dst;

    __asm__ volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;

    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] - y[i];
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    return memcpy(dst, src, strlen(src) + 1);
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t len = strnlen(src, n);

    memcpy(dst, src, len);
    memset(dst + len, 0, n - len);
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);

    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) {
        if (*s == (char)c)
            return (char *)s;
    }
    return c ? NULL : (char *)s;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    for (; *s; s++) {
        if (*s == (char)c)
            last = s;
    }
    return c ? (char *)last : (char *)s;
}

char *strstr(const char *h, const char *n)
{
    size_t len = strlen(n);

    for (; *h; h++) {
        if (!strncmp(h, n, len))
            return (char *)h;
    }
    return len ? NULL : (char *)h;
}

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *d = malloc(len);

    return d ? memcpy(d, s, len) : NULL;
}

const char *strerror(int err)
{
    switch (err) {
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EPIPE: return "Broken pipe";
    case ENOTSOCK: return "Not a socket";
    case EDESTADDRREQ: return "Destination address required";
    case EMSGSIZE: return "Message too long";
    case EPROTOTYPE: return "Wrong protocol type for socket";
    case ENOPROTOOPT: return "Protocol not available";
    case EPROTONOSUPPORT: return "Protocol not supported";
    case EAFNOSUPPORT: return "Address family not supported";
    case EADDRINUSE: return "Address already in use";
    case EADDRNOTAVAIL: return "Address not available";
    case ENETDOWN: return "Network is down";
    case ENETUNREACH: return "Network is unreachable";
    case ECONNABORTED: return "Connection aborted";
    case ECONNRESET: return "Connection reset by peer";
    case ENOBUFS: return "No buffer space available";
    case EISCONN: return "Already connected";
    case ENOTCONN: return "Not connected";
    case ETIMEDOUT: return "Timed out";
    case ECONNREFUSED: return "Connection refused";
    case EHOSTUNREACH: return "Host is unreachable";
    case EALREADY: return "Operation already in progress";
    case EINPROGRESS: return "Operation in progress";
    case EDEADLK: return "Resource deadlock avoided";
    case ESRCH: return "No such process";
    case EINTR: return "Interrupted";
    case EIO: return "Input/output error";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Not an executable";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
    case ENOMEM: return "Out of memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EBUSY: return "Resource busy";
    case EEXIST: return "File exists";
    case EXDEV: return "Cross-device link";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case ENOTTY: return "Not a terminal";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EROFS: return "Read-only file system";
    case EMLINK: return "Too many links";
    case ERANGE: return "Result out of range";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Not implemented";
    case ENOTEMPTY: return "Directory not empty";
    case ELOOP: return "Too many symbolic links";
    case ENOTSUP: return "Not supported";
    default: return "Unknown error";
    }
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    }
    return NULL;
}
