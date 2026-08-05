// syscall.h - Professional Linux-compatible System Call definitions for EquantOS
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

// Standard Linux Syscall Numbers
#define SYS_READ      0
#define SYS_WRITE     1
#define SYS_MMAP      9
#define SYS_MUNMAP    11
#define SYS_BRK       12
#define SYS_GETPID    39
#define SYS_EXIT      60
#define SYS_YIELD     158

// Custom EquantOS Diagnostic Syscalls
#define SYS_SYSINFO   99

typedef struct {
    uint64_t total_ram;
    uint64_t free_ram;
    uint64_t used_ram;
    uint64_t pmm_total_pages;
    uint64_t pmm_used_pages;
    uint64_t kernel_heap_used;
} __attribute__((packed)) equant_sysinfo_t;

void init_syscalls(void);
void syscall_handler(void *regs);

#endif // SYSCALL_H