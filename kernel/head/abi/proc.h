#ifndef AEGIS_ABI_PROC_H
#define AEGIS_ABI_PROC_H

#include <stdint.h>

#define PROC_RUNNING    0
#define PROC_SLEEPING   1
#define PROC_STOPPED    2
#define PROC_ZOMBIE     3

struct aegis_procinfo {
    int32_t pid;
    int32_t ppid;
    uint32_t uid, euid;
    uint32_t threads;
    uint32_t state;                 // PROC_*
    uint64_t cpu_ms;                // CPU time used
    uint64_t memory;                // resident bytes
    uint64_t virtual_memory;        // mapped bytes
    uint64_t start_ms;              // uptime when it started
    char name[32];
};

struct aegis_sysinfo {
    uint64_t uptime_ms;
    uint64_t memory_total;
    uint64_t memory_free;
    uint32_t cpus;
    uint32_t processes;
    uint32_t threads;
    uint32_t reserved;
    uint64_t cpu_busy_ms[64];
    uint64_t cpu_idle_ms[64];
};

// inspect(pid, what, buffer, size): a look inside a process, for debuggers.
// Allowed for the process's owner and root.
#define INSPECT_THREADS 0       // struct aegis_threadinfo[]; returns the count
#define INSPECT_MAPS    1       // struct aegis_mapinfo[]; returns the count
#define INSPECT_FILES   2       // struct aegis_fileinfo[]; returns the count
// INSPECT_MEMORY | (address << 8) would not fit: memory reads take the
// address as the first 8 bytes of the buffer and return the bytes read.
#define INSPECT_MEMORY  3

struct aegis_threadinfo {
    uint64_t tid;
    uint32_t state;                 // 0 running, 1 ready, 2 waiting, 3 stopped, 4 ended
    uint32_t on_cpu;
    uint64_t cpu_ms;
    // User registers when the thread last entered the kernel.
    uint64_t rip, rsp, rbp, rflags, fs_base;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15;
    char name[32];
};

#define MAP_KIND_ANON   0
#define MAP_KIND_FILE   1
#define MAP_KIND_SHARED 2
#define MAP_KIND_DEVICE 3

struct aegis_mapinfo {
    uint64_t start, end;
    uint32_t prot;                  // PROT_*
    uint32_t kind;                  // MAP_KIND_*
    uint64_t offset;
    uint64_t ino;                   // file mappings
};

#define FILE_KIND_FILE      0
#define FILE_KIND_DIR       1
#define FILE_KIND_DEVICE    2
#define FILE_KIND_OTHER     3       // pipes, sockets, terminals

struct aegis_fileinfo {
    int32_t fd;
    uint32_t flags;                 // O_*
    uint64_t offset;
    uint32_t kind;
    uint32_t mode;
    uint64_t ino, size;
};

#endif
