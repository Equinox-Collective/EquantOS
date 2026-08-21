// syscall.h - Linux-compatible System Call definitions for EquantOS
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

// Linux Error Codes (negative return values in RAX)
#define ENOENT              2
#define EBADF               9
#define EAGAIN              11
#define ENOMEM              12
#define ENOTDIR             20
#define EINVAL              22
#define EMFILE              24
#define ERANGE              34
#define ENOSYS              38

// Standard Linux Syscall Numbers
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_FSTAT           5
#define SYS_MMAP            9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_RT_SIGACTION    13
#define SYS_RT_SIGPROCMASK  14
#define SYS_IOCTL           16
#define SYS_WRITEV          20
#define SYS_SCHED_YIELD     24
#define SYS_NANOSLEEP       35
#define SYS_GETPID          39
#define SYS_EXIT            60
#define SYS_UNAME           63
#define SYS_GETCWD          79
#define SYS_CHDIR           80
#define SYS_SYSINFO         99
#define SYS_GETUID          102
#define SYS_GETGID          104
#define SYS_GETEUID         107
#define SYS_GETEGID         108
#define SYS_GETPPID         110
#define SYS_ARCH_PRCTL      158
#define SYS_TKILL           200
#define SYS_FUTEX           202
#define SYS_GETDENTS64      217
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME   228
#define SYS_EXIT_GROUP      231
#define SYS_TGKILL          234
#define SYS_OPENAT          257

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