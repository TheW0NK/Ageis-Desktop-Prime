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
#define SYS_COUNT       50

#define REBOOT_RESTART  1
#define REBOOT_POWEROFF 2

#define IOCTL_CONSOLE_RAW       1   // arg: 1 = raw (no echo, byte at a time), 0 = line mode
#define IOCTL_CONSOLE_FOREGROUND 2  // arg: pid that receives Ctrl+C, 0 = none
#define IOCTL_CONSOLE_SIZE      3   // returns (rows << 16) | columns
#define IOCTL_DISPLAY_MODE      4   // arg: (width << 16) | height; root only

#endif
