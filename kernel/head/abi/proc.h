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

#endif
