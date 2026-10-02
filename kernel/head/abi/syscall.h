#ifndef AEGIS_ABI_SYSCALL_H
#define AEGIS_ABI_SYSCALL_H

// System call numbers. Arguments go in rdi, rsi, rdx, r10, r8, r9; the result
// is returned in rax, with errors as negative errno values.
#define SYS_EXIT        0
#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_LSEEK       5
#define SYS_STAT        6
#define SYS_FSTAT       7
#define SYS_LSTAT       8
#define SYS_GETDENTS    9
#define SYS_MKDIR       10
#define SYS_RMDIR       11
#define SYS_UNLINK      12
#define SYS_RENAME      13
#define SYS_CHDIR       14
#define SYS_GETCWD      15
#define SYS_SPAWN       16
#define SYS_WAIT        17
#define SYS_GETPID      18
#define SYS_GETPPID     19
#define SYS_SLEEP       20
#define SYS_YIELD       21
#define SYS_UPTIME      22
#define SYS_TIME        23
#define SYS_BRK         24
#define SYS_DUP         25
#define SYS_DUP2        26
#define SYS_CHMOD       27
#define SYS_CHOWN       28
#define SYS_TRUNCATE    29
#define SYS_FTRUNCATE   30
#define SYS_SYMLINK     31
#define SYS_READLINK    32
#define SYS_LINK        33
#define SYS_SYNC        34
#define SYS_FSYNC       35
#define SYS_GETUID      36
#define SYS_GETEUID     37
#define SYS_GETGID      38
#define SYS_GETEGID     39
#define SYS_SETEUID     40
#define SYS_LOGIN       41
#define SYS_SUDO        42
#define SYS_REBOOT      43
#define SYS_UNAME       44
#define SYS_KILL        45
#define SYS_IOCTL       46
#define SYS_ACCESS      47
#define SYS_UTIME       48
#define SYS_STATFS      49
#define SYS_MMAP        50      // addr, len, prot, flags, fd, offset
#define SYS_MUNMAP      51
#define SYS_MPROTECT    52
#define SYS_SHM_CREATE  53      // size, flags (O_CLOEXEC) -> fd
#define SYS_PIPE        54      // int fds[2], flags
#define SYS_POLL        55      // struct pollfd *, n, timeout_ms (-1 forever)
#define SYS_THREAD_CREATE 56    // entry, stack top, arg, tls, clear_tid address -> tid
#define SYS_THREAD_EXIT 57
#define SYS_GETTID      58
#define SYS_FUTEX_WAIT  59      // address, expected value, timeout_ms (-1 forever)
#define SYS_FUTEX_WAKE  60      // address, count
#define SYS_SET_TLS     61      // %fs base
#define SYS_SIGACTION   62      // sig, const struct aegis_sigaction *, old
#define SYS_SIGPROCMASK 63      // how, const uint64_t *set, uint64_t *old
#define SYS_SIGRETURN   64
#define SYS_SOCKET      65      // domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol
#define SYS_SOCKETPAIR  66      // domain, type, protocol, int fds[2]
#define SYS_BIND        67
#define SYS_LISTEN      68
#define SYS_ACCEPT      69      // fd, addr, uint32_t *len, flags
#define SYS_CONNECT     70
#define SYS_SENDMSG     71      // fd, struct aegis_msghdr *, flags
#define SYS_RECVMSG     72
#define SYS_SHUTDOWN    73
#define SYS_GETSOCKOPT  74      // fd, level, option, value, uint32_t *len
#define SYS_SETSOCKOPT  75
#define SYS_GETSOCKNAME 76      // fd, addr, uint32_t *len
#define SYS_GETPEERNAME 77
#define SYS_PROCINFO    78      // struct aegis_procinfo *, max -> count
#define SYS_SYSINFO     79      // struct aegis_sysinfo *
#define SYS_FCNTL       80      // fd, cmd, arg
#define SYS_SIGSUSPEND  81      // const uint64_t *mask
#define SYS_NETCONFIG   82      // op, interface index, struct aegis_netif *
#define SYS_OPENPTY     83      // int fds[2] (master, slave), flags (O_CLOEXEC)
#define SYS_BECOME      84      // uid: root only; become that account (uid, gid, groups)
#define SYS_COUNT       85

#define WNOHANG         1       // SYS_WAIT options (third argument)

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define FD_CLOEXEC      1

#define REBOOT_RESTART  1
#define REBOOT_POWEROFF 2

#define IOCTL_CONSOLE_RAW       1   // arg: 1 = raw (no echo, byte at a time), 0 = line mode
#define IOCTL_CONSOLE_FOREGROUND 2  // arg: pid that receives Ctrl+C, 0 = none
#define IOCTL_CONSOLE_SIZE      3   // returns (rows << 16) | columns
#define IOCTL_PTY_SET_SIZE      9   // pty master: arg (rows << 16) | columns; sends SIGWINCH
#define IOCTL_SHM_SIZE          8   // shared memory object: returns its size
#define IOCTL_DISPLAY_MODE      4   // arg: (width << 16) | height; root only

#endif
