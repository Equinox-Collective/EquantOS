// syscall.c - System Call Dispatcher & Native x86_64 Syscall Setup
#include "syscall.h"
#include "task.h"
#include "sched.h"
#include "../core/mem/vmm.h"
#include "../core/mem/pmm.h"
#include "../core/gen/cpu.h"
#include "../drivers/serial/serial.h"
#include "../../equterm/term.h"
#include "string.h"
#include "../fs/vfs.h"

// MSR адреса x86_64
#define IA32_EFER        0xC0000080
#define IA32_STAR        0xC0000081
#define IA32_LSTAR       0xC0000082
#define IA32_FMASK       0xC0000084
#define IA32_FS_BASE_MSR 0xC0000100

// Номера Linux-сисколлов
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_FSTAT           5
#define SYS_MMAP            9
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_RT_SIGPROCMASK  14
#define SYS_IOCTL           16
#define SYS_WRITEV          20
#define SYS_GETPID          39
#define SYS_EXIT            60
#define SYS_SYSINFO         99
#define SYS_ARCH_PRCTL      158
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP      231

#define ARCH_SET_FS      0x1002
#define ARCH_GET_FS      0x1003

struct iovec {
    void *iov_base;
    size_t iov_len;
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
    if (!current_task) return -1;

    if (code == ARCH_SET_FS) {
        current_task->fs_base = addr;
        write_msr(IA32_FS_BASE_MSR, addr);
        return 0;
    } else if (code == ARCH_GET_FS) {
        if (!addr) return -1;
        *(uint64_t *)addr = current_task->fs_base;
        return 0;
    }
    return -1;
}

static int64_t sys_brk_handler(uint64_t new_brk) {
    if (!current_task || !current_task->process) return -1;
    
    if (new_brk == 0) {
        return current_task->process->brk;
    }

    uint64_t old_brk = current_task->process->brk;
    if (new_brk <= old_brk) {
        current_task->process->brk = new_brk;
        return new_brk;
    }

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    uint64_t start_page = (old_brk + 0xFFF) & ~0xFFFULL;
    uint64_t end_page = (new_brk + 0xFFF) & ~0xFFFULL;

    for (uint64_t addr = start_page; addr < end_page; addr += PAGE_SIZE) {
        void *phys = pmm_alloc();
        if (!phys) return -1;
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);
        vmm_map(pml4, addr, (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    current_task->process->brk = new_brk;
    return new_brk;
}

static int64_t sys_write_handler(int fd, const void *user_buf, size_t count) {
    if (fd == 1 || fd == 2) { // stdout / stderr
        const char *buf = (const char *)user_buf;
        for (size_t i = 0; i < count; i++) {
            char c = buf[i];
            char str[2] = {c, '\0'};
            serial_putchar(COM1, c);
            term_putchar_raw(c);
        }
        return count;
    }

    if (!current_task || !current_task->process) return -1;
    if (fd < 3 || fd >= MAX_OPEN_FILES) return -1;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -1;

    return vfs_write(node, 0, count, (uint8_t *)user_buf);
}

// Векторный вывод для printf() из Musl libc
static int64_t sys_writev_handler(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return -1;
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
    if (length == 0) return -1;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t aligned_len = page_count * PAGE_SIZE;

    uint64_t virt_addr = addr;
    if (virt_addr == 0) {
        virt_addr = mmap_virtual_base;
        mmap_virtual_base += aligned_len;
    }

    if (!current_task || !current_task->process) return -1;

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);

    for (size_t i = 0; i < page_count; i++) {
        void *phys = pmm_alloc();
        if (!phys) return -1;
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);
        vmm_map(pml4, virt_addr + (i * PAGE_SIZE), (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    return (int64_t)virt_addr;
}

static int64_t sys_munmap_handler(uint64_t addr, size_t length) {
    (void)addr; (void)length;
    return 0;
}

static int64_t sys_rt_sigprocmask_handler(int how, const void *set, void *oldset, size_t sigsetsize) {
    (void)how; (void)set; (void)oldset; (void)sigsetsize;
    return 0;
}

void syscall_handler(void *regs_ptr) {
    syscall_regs_t *regs = (syscall_regs_t *)regs_ptr;
    uint64_t syscall_no = regs->rax;
    int64_t ret = -1;

    switch (syscall_no) {
        case SYS_WRITE:
            ret = sys_write_handler((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_WRITEV:
            ret = sys_writev_handler((int)regs->rdi, (const struct iovec *)regs->rsi, (int)regs->rdx);
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
        case SYS_RT_SIGPROCMASK:
            ret = sys_rt_sigprocmask_handler((int)regs->rdi, (const void *)regs->rsi, (void *)regs->rdx, (size_t)regs->r10);
            break;
        case SYS_ARCH_PRCTL:
            ret = sys_arch_prctl_handler((int)regs->rdi, regs->rsi);
            break;
        case SYS_GETPID:
            ret = current_task ? current_task->id : 1;
            break;
        case SYS_SET_TID_ADDRESS:
            ret = current_task ? current_task->id : 1;
            break;
        case SYS_FSTAT:
            ret = 0; // Заглушка fstat
            break;
        case SYS_IOCTL:
            ret = -1; // ENOTTY (не является TTY-устройством)
            break;
        default:
            serial_puts(COM1, "[KERNEL] Unknown Syscall: ");
            char buf[32];
            itoa_hex(syscall_no, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, "\n");
            ret = -1;
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