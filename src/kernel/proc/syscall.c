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
#define SYS_READ         0
#define SYS_WRITE        1
#define SYS_OPEN         2
#define SYS_CLOSE        3
#define SYS_MMAP         9
#define SYS_MUNMAP       11
#define SYS_BRK          12
#define SYS_GETPID       39
#define SYS_EXIT         60
#define SYS_SYSINFO      99
#define SYS_ARCH_PRCTL   158

#define ARCH_SET_FS      0x1002
#define ARCH_GET_FS      0x1003

extern void syscall_entry_asm(void);

// Функция записи в MSR
static inline void write_msr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Внешние метрики памяти
extern uint64_t pmm_used_pages;
extern size_t used_memory;
extern uint64_t free_memory;
extern uint64_t total_pages;

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} __attribute__((packed)) syscall_regs_t;

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
            serial_puts(COM1, str);
            term_print(str);
        }
        return count;
    }

    if (!current_task || !current_task->process) return -1;
    if (fd < 3 || fd >= MAX_OPEN_FILES) return -1;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -1;

    return vfs_write(node, 0, count, (uint8_t *)user_buf);
}

void syscall_handler(void *regs_ptr) {
    syscall_regs_t *regs = (syscall_regs_t *)regs_ptr;
    uint64_t syscall_no = regs->rax;
    int64_t ret = -1;

    // В x86_64 System V ABI аргументы сисколла: RDI, RSI, RDX, R10, R8, R9
    switch (syscall_no) {
        case SYS_WRITE:
            ret = sys_write_handler((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_EXIT:
            ret = sys_exit_handler((int)regs->rdi);
            break;
        case SYS_BRK:
            ret = sys_brk_handler(regs->rdi);
            break;
        case SYS_ARCH_PRCTL:
            ret = sys_arch_prctl_handler((int)regs->rdi, regs->rsi);
            break;
        case SYS_GETPID:
            ret = current_task ? current_task->id : 0;
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
    // 1. Включаем SCE (System Call Extension) в EFER
    uint64_t efer = read_msr(IA32_EFER);
    write_msr(IA32_EFER, efer | 1);

    // 2. Настраиваем сегменты GDT в STAR MSR:
    // Kernel CS/SS = 0x08, User CS/SS = 0x10 (вычисляет 0x23 для Code и 0x1B для Data)
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(IA32_STAR, star);

    // 3. Устанавливаем адрес точки входа в LSTAR
    write_msr(IA32_LSTAR, (uint64_t)syscall_entry_asm);

    // 4. Маскируем флаги при сисколле (отключаем прерывания IF во время входа)
    write_msr(IA32_FMASK, 0x200);

    serial_puts(COM1, "[KERNEL] Native x86_64 Hardware 'syscall' MSRs Initialized.\n");
}