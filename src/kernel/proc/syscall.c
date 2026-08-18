// syscall.c - System Call Dispatcher & Native x86_64 Syscall Setup
#include "syscall.h"
#include "task.h"
#include "sched.h"
#include "../core/mem/vmm.h"
#include "../core/mem/pmm.h"
#include "../core/mem/memory.h"
#include "../core/gen/cpu.h"
#include "../drivers/serial/serial.h"
#include "../../equterm/term.h"
#include "../misc/timer.h"
#include "string.h"
#include "../fs/vfs.h"
#include "../core/initcall.h"

#define IA32_EFER        0xC0000080
#define IA32_STAR        0xC0000081
#define IA32_LSTAR       0xC0000082
#define IA32_FMASK       0xC0000084
#define IA32_FS_BASE_MSR 0xC0000100

#define AT_FDCWD         -100

#define ARCH_SET_FS      0x1002
#define ARCH_GET_FS      0x1003

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

void linux_syscall_handler(void *regs_ptr) {
    syscall_handler(regs_ptr);
}

extern void syscall_entry_asm(void);

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} __attribute__((packed)) syscall_regs_t;

static uint64_t mmap_virtual_base = 0x700000000000ULL;

static void resolve_user_path(const char *input, char *output, size_t max_len) {
    if (!input || input[0] == '\0') {
        if (current_task && current_task->process && current_task->process->cwd[0] != '\0') {
            strncpy(output, current_task->process->cwd, max_len);
        } else {
            strcpy(output, "/");
        }
        return;
    }

    if (input[0] == '/') {
        strncpy(output, input, max_len);
    } else {
        if (current_task && current_task->process && current_task->process->cwd[0] != '\0') {
            strcpy(output, current_task->process->cwd);
        } else {
            strcpy(output, "/");
        }
        if (output[strlen(output) - 1] != '/') {
            strcat(output, "/");
        }
        strcat(output, input);
    }
}

static int alloc_fd(vfs_node_t *node) {
    if (!current_task || !current_task->process) return -EMFILE;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (current_task->process->files[i] == NULL) {
            current_task->process->files[i] = node;
            return i;
        }
    }
    return -EMFILE;
}

static int64_t sys_openat_handler(int dirfd, const char *pathname, int flags, int mode) {
    (void)dirfd; (void)flags; (void)mode;
    if (!pathname) return -EINVAL;

    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) return -ENOENT;

    return alloc_fd(node);
}

static int64_t sys_read_handler(int fd, void *buf, size_t count) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    return vfs_read(node, 0, count, (uint8_t *)buf);
}

static int64_t sys_close_handler(int fd) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 3 || fd >= MAX_OPEN_FILES) return -EBADF;

    if (current_task->process->files[fd]) {
        vfs_close(current_task->process->files[fd]);
        current_task->process->files[fd] = NULL;
    }
    return 0;
}

// Hash Table for Futex Wait Queues
#define FUTEX_HASH_SIZE 64
static task_t *futex_queues[FUTEX_HASH_SIZE] = {NULL};

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static inline uint32_t futex_hash(uint64_t uaddr) {
    return (uint32_t)((uaddr >> 3) % FUTEX_HASH_SIZE);
}

static int64_t sys_futex_handler(int *uaddr, int futex_op, int val, const struct linux_timespec *timeout, int *uaddr2, int val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    if (!uaddr) return -EINVAL;

    uint64_t vaddr = (uint64_t)uaddr;
    uint32_t hash = futex_hash(vaddr);
    int op = futex_op & 127; // Mask out FUTEX_PRIVATE_FLAG

    if (op == FUTEX_WAIT) {
        // Atomic check: if value changed in user space, don't sleep
        if (*uaddr != val) {
            return -EAGAIN;
        }

        // Put current task into Futex Wait Queue
        current_task->futex_addr = vaddr;
        current_task->futex_next = futex_queues[hash];
        futex_queues[hash] = current_task;

        // Block current task and switch context
        sched_block(current_task);
        sched_yield();

        return 0;
    } 
    else if (op == FUTEX_WAKE) {
        int woken = 0;
        task_t **curr = &futex_queues[hash];

        while (*curr && woken < val) {
            if ((*curr)->futex_addr == vaddr) {
                task_t *target = *curr;
                *curr = target->futex_next;
                target->futex_addr = 0;
                target->futex_next = NULL;

                sched_unblock(target); // Unblock waiting thread
                woken++;
            } else {
                curr = &(*curr)->futex_next;
            }
        }
        return woken; // Return count of woken threads
    }

    return -ENOSYS;
}

static int64_t sys_getcwd_handler(char *buf, size_t size) {
    if (!buf || size == 0) return -EINVAL;
    const char *cwd = (current_task && current_task->process && current_task->process->cwd[0] != '\0')
                      ? current_task->process->cwd : "/";
    size_t len = strlen(cwd) + 1;
    if (size < len) return -ERANGE;

    memcpy(buf, cwd, len);
    return (int64_t)buf;
}

static int64_t sys_chdir_handler(const char *path) {
    if (!path) return -EINVAL;

    char resolved[256];
    resolve_user_path(path, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node || !(node->flags & FS_DIRECTORY)) return -ENOENT;

    if (current_task && current_task->process) {
        strncpy(current_task->process->cwd, resolved, sizeof(current_task->process->cwd) - 1);
    }
    return 0;
}

static int64_t sys_getdents64_handler(int fd, void *dirp, size_t count) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *dir = current_task->process->files[fd];
    if (!dir || !(dir->flags & FS_DIRECTORY)) return -ENOTDIR;

    uint8_t *out_buf = (uint8_t *)dirp;
    size_t written = 0;
    uint32_t idx = 0;

    vfs_node_t *child = NULL;
    while ((child = vfs_readdir(dir, idx++)) != NULL) {
        size_t name_len = strlen(child->name);
        size_t rec_len = (sizeof(struct linux_dirent64) + name_len + 1 + 7) & ~7;

        if (written + rec_len > count) break;

        struct linux_dirent64 *d = (struct linux_dirent64 *)(out_buf + written);
        d->d_ino = child->inode ? child->inode : idx;
        d->d_off = idx * 32;
        d->d_reclen = (unsigned short)rec_len;
        d->d_type = (child->flags & FS_DIRECTORY) ? 4 : 8;
        strcpy(d->d_name, child->name);

        written += rec_len;
        kfree(child);
    }

    return (int64_t)written;
}

static int64_t sys_exit_handler(int code) {
    serial_puts(COM1, "[KERNEL] User process exited with code: ");
    char buf[32];
    itoa_hex(code, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    if (current_task) {
        current_task->state = TASK_STATE_ZOMBIE;
        current_task->running = false;
    }
    sched_yield();
    for(;;);
    return 0;
}

static int64_t sys_arch_prctl_handler(int code, uint64_t addr) {
    if (!current_task) return -EINVAL;

    if (code == ARCH_SET_FS) {
        current_task->fs_base = addr;
        write_msr(IA32_FS_BASE_MSR, addr);
        return 0;
    } else if (code == ARCH_GET_FS) {
        if (!addr) return -EINVAL;
        *(uint64_t *)addr = current_task->fs_base;
        return 0;
    }
    return -EINVAL;
}

// FIX: Linux sys_brk MUST return the current brk pointer on failure!
static int64_t sys_brk_handler(uint64_t new_brk) {
    if (!current_task || !current_task->process) return 0;
    
    uint64_t old_brk = current_task->process->brk;
    if (new_brk == 0 || new_brk == old_brk) {
        return old_brk;
    }

    if (new_brk < old_brk) {
        current_task->process->brk = new_brk;
        return new_brk;
    }

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    
    // Выравнивание границ страниц
    uint64_t start_page = old_brk & ~(PAGE_SIZE - 1);
    uint64_t end_page = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start_page; addr < end_page; addr += PAGE_SIZE) {
        // Проверяем, не замаплена ли страница уже
        if (vmm_get_phys(pml4, addr) != 0) {
            continue;
        }

        void *phys = pmm_alloc();
        if (!phys && phys != (void*)0) {
            return old_brk; // Возвращаем старый brk при ошибке PMM
        }
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);
        vmm_map(pml4, addr, (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    current_task->process->brk = new_brk;
    return new_brk;
}

static int64_t sys_write_handler(int fd, const void *user_buf, size_t count) {
    if (fd == 1 || fd == 2) {
        const char *buf = (const char *)user_buf;
        for (size_t i = 0; i < count; i++) {
            char c = buf[i];
            serial_putchar(COM1, c);
            term_putchar_raw(c);
        }
        return count;
    }

    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 3 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    return vfs_write(node, 0, count, (uint8_t *)user_buf);
}

static int64_t sys_writev_handler(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_base && iov[i].iov_len > 0) {
            int64_t ret = sys_write_handler(fd, iov[i].iov_base, iov[i].iov_len);
            if (ret < 0) return ret;
            total += ret;
        }
    }
    return total;
}

static int64_t sys_mmap_handler(uint64_t addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    (void)prot; (void)flags; (void)fd; (void)offset;
    if (length == 0) return -EINVAL;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t aligned_len = page_count * PAGE_SIZE;

    uint64_t virt_addr = addr;
    if (virt_addr == 0) {
        virt_addr = mmap_virtual_base;
        mmap_virtual_base += aligned_len;
    }

    if (!current_task || !current_task->process) return -EINVAL;

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);

    for (size_t i = 0; i < page_count; i++) {
        void *phys = pmm_alloc();
        if (!phys) return -ENOMEM;
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);
        vmm_map(pml4, virt_addr + (i * PAGE_SIZE), (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    return (int64_t)virt_addr;
}

static int64_t sys_munmap_handler(uint64_t addr, size_t length) {
    (void)addr; (void)length;
    return 0;
}

static int64_t sys_mprotect_handler(uint64_t addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    // Stub: Always accept memory protection changes
    return 0;
}

static int64_t sys_uname_handler(struct linux_utsname *buf) {
    if (!buf) return -EINVAL;

    memset(buf, 0, sizeof(struct linux_utsname));
    strcpy(buf->sysname, "EquantOS");
    strcpy(buf->nodename, "equant");
    strcpy(buf->release, "0.0.1-alpha");
    strcpy(buf->version, "EquantOS Kernel v0.0.1 (x86_64)");
    strcpy(buf->machine, "x86_64");
    strcpy(buf->domainname, "localdomain");

    return 0;
}

static int64_t sys_clock_gettime_handler(int clock_id, struct linux_timespec *tp) {
    (void)clock_id;
    if (!tp) return -EINVAL;

    // Convert PIT ticks (100 Hz = 10ms per tick) to seconds and nanoseconds
    uint64_t current_ticks = tick;
    tp->tv_sec = current_ticks / 100;
    tp->tv_nsec = (current_ticks % 100) * 10000000ULL;

    return 0;
}

static int64_t sys_nanosleep_handler(const struct linux_timespec *req, struct linux_timespec *rem) {
    (void)rem;
    if (!req) return -EINVAL;

    uint64_t target_tick = tick + (req->tv_sec * 100 + req->tv_nsec / 10000000ULL);
    
    if (target_tick > tick) {
        __asm__ volatile("sti"); // Включаем прерывания, чтобы таймер PIT мог тикать
        while (tick < target_tick) {
            __asm__ volatile("hlt"); // Спим до следующего прерывания таймера
        }
        __asm__ volatile("cli"); // Выключаем прерывания перед возвратом в код сисколла
    }
    return 0;
}

static int64_t sys_rt_sigaction_handler(int signum, const void *act, void *oldact, size_t sigsetsize) {
    (void)signum; (void)act; (void)oldact; (void)sigsetsize;
    // Stub signal handler registration
    return 0;
}

static int64_t sys_futex_handler(int *uaddr, int futex_op, int val, const struct linux_timespec *timeout, int *uaddr2, int val3) {
    (void)uaddr; (void)futex_op; (void)val; (void)timeout; (void)uaddr2; (void)val3;
    // Stub futex wake/wait implementation
    return 0;
}

void syscall_handler(void *regs_ptr) {
    syscall_regs_t *regs = (syscall_regs_t *)regs_ptr;
    uint64_t syscall_no = regs->rax;
    int64_t ret = -ENOSYS; // Default: Not Implemented

    switch (syscall_no) {
        case SYS_READ:
            ret = sys_read_handler((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_WRITE:
            ret = sys_write_handler((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_OPEN:
            ret = sys_openat_handler(AT_FDCWD, (const char *)regs->rdi, (int)regs->rsi, (int)regs->rdx);
            break;
        case SYS_CLOSE:
            ret = sys_close_handler((int)regs->rdi);
            break;
        case SYS_WRITEV:
            ret = sys_writev_handler((int)regs->rdi, (const struct iovec *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_GETCWD:
            ret = sys_getcwd_handler((char *)regs->rdi, (size_t)regs->rsi);
            break;
        case SYS_CHDIR:
            ret = sys_chdir_handler((const char *)regs->rdi);
            break;
        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            ret = sys_exit_handler((int)regs->rdi);
            break;
        case SYS_BRK:
            ret = sys_brk_handler(regs->rdi);
            break;
        case SYS_MMAP:
            ret = sys_mmap_handler(regs->rdi, (size_t)regs->rsi, (int)regs->rdx, (int)regs->r10, (int)regs->r8, (int64_t)regs->r9);
            break;
        case SYS_MUNMAP:
            ret = sys_munmap_handler(regs->rdi, (size_t)regs->rsi);
            break;
        case SYS_MPROTECT:
            ret = sys_mprotect_handler(regs->rdi, (size_t)regs->rsi, (int)regs->rdx);
            break;
        case SYS_RT_SIGACTION:
            ret = sys_rt_sigaction_handler((int)regs->rdi, (const void *)regs->rsi, (void *)regs->rdx, (size_t)regs->r10);
            break;
        case SYS_RT_SIGPROCMASK:
            ret = 0;
            break;
        case SYS_SCHED_YIELD:
            sched_yield();
            ret = 0;
            break;
        case SYS_NANOSLEEP:
            ret = sys_nanosleep_handler((const struct linux_timespec *)regs->rdi, (struct linux_timespec *)regs->rsi);
            break;
        case SYS_ARCH_PRCTL:
            ret = sys_arch_prctl_handler((int)regs->rdi, regs->rsi);
            break;
        case SYS_GETDENTS64:
            ret = sys_getdents64_handler((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_GETPID:
            ret = current_task ? current_task->id : 1;
            break;
        case SYS_GETPPID:
            ret = 1; // Return Init PID as Parent PID
            break;
        case SYS_GETUID:
        case SYS_GETGID:
            ret = 0; // Root user / group ID
            break;
        case SYS_UNAME:
            ret = sys_uname_handler((struct linux_utsname *)regs->rdi);
            break;
        case SYS_SET_TID_ADDRESS:
            ret = current_task ? current_task->id : 1;
            break;
        case SYS_CLOCK_GETTIME:
            ret = sys_clock_gettime_handler((int)regs->rdi, (struct linux_timespec *)regs->rsi);
            break;
        case SYS_FUTEX:
            ret = sys_futex_handler((int *)regs->rdi, (int)regs->rsi, (int)regs->rdx, (const struct linux_timespec *)regs->r10, (int *)regs->r8, (int)regs->r9);
            break;
        case SYS_OPENAT:
            ret = sys_openat_handler((int)regs->rdi, (const char *)regs->rsi, (int)regs->rdx, (int)regs->r10);
            break;
        case SYS_FSTAT:
            ret = 0;
            break;
        case SYS_IOCTL:
            ret = 0;
            break;
        default:
            serial_puts(COM1, "[KERNEL] Unknown Syscall: 0x");
            char buf[32];
            itoa_hex(syscall_no, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, "\n");
            ret = -ENOSYS;
            break;
    }

    regs->rax = (uint64_t)ret;
}

void init_syscalls(void) {
    uint64_t efer = read_msr(IA32_EFER);
    write_msr(IA32_EFER, efer | 1);

    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(IA32_STAR, star);

    write_msr(IA32_LSTAR, (uint64_t)syscall_entry_asm);
    write_msr(IA32_FMASK, 0x200);

    serial_puts(COM1, "[KERNEL] Native x86_64 Hardware 'syscall' MSRs Initialized.\n");
}

// // THIS SHOULD BELONG TO BOTTOM, DO NOT REWRITE IN ANY CASE // //

static int __init init_syscalls_initcall(void) {
    init_syscalls();
    return 0;
}
arch_initcall(init_syscalls_initcall);