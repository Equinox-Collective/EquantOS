// syscall.c - System Call Dispatcher and Handlers
#include "syscall.h"
#include "task.h"
#include "sched.h"
#include "vmm.h"
#include "pmm.h"
#include "memory.h"
#include "serial.h"
#include "term.h"
#include "panic.h"
#include "string.h"

// External kernel memory metrics
extern uint64_t pmm_used_pages;
extern size_t used_memory;
extern uint64_t free_memory;
extern uint64_t total_pages;

// Register state pushed by syscall_interrupt_asm in interrupt.asm
typedef struct {
    uint64_t rax;
    uint64_t r9, r8;
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r10, r11, r12, r13, r14, r15;
} __attribute__((packed)) syscall_regs_t;

void linux_syscall_handler(void *regs_ptr) {
    syscall_handler(regs_ptr);
}

static int64_t sys_write_handler(int fd, const char *buf, size_t count) {
    if (fd == 1 || fd == 2) { // stdout / stderr
        // Print to serial and terminal safely
        for (size_t i = 0; i < count; i++) {
            char c = buf[i];
            char str[2] = {c, '\0'};
            serial_puts(COM1, str);
            term_print(str);
        }
        return count;
    }
    return -1;
}

static int64_t sys_exit_handler(int code) {
    serial_puts(COM1, "[KERNEL] User task exited with code ");
    char buf[32];
    itoa_hex(code, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    current_task->state = TASK_STATE_ZOMBIE;
    current_task->running = false;
    sched_yield();
    for(;;);
    return 0;
}

static int64_t sys_getpid_handler(void) {
    return current_task ? current_task->id : 0;
}

static int64_t sys_yield_handler(void) {
    sched_yield();
    return 0;
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

    uint64_t page_table = current_task->process->cr3;
    page_table_t *pml4 = (page_table_t *)VIRT(page_table);

    uint64_t start_page = (old_brk + 0xFFF) & ~0xFFFULL;
    uint64_t end_page = (new_brk + 0xFFF) & ~0xFFFULL;

    for (uint64_t addr = start_page; addr < end_page; addr += PAGE_SIZE) {
        void *phys = pmm_alloc();
        if (!phys) return -1; // Out of memory
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);
        vmm_map(pml4, addr, (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    current_task->process->brk = new_brk;
    return new_brk;
}

static int64_t sys_sysinfo_handler(equant_sysinfo_t *user_info) {
    if (!user_info) return -1;

    equant_sysinfo_t info;
    info.total_ram = pmm_get_total_memory();
    info.free_ram = free_memory;
    info.used_ram = pmm_get_used_memory();
    info.pmm_total_pages = total_pages;
    info.pmm_used_pages = pmm_used_pages;
    info.kernel_heap_used = used_memory;

    // Copy to user space safely
    memcpy(user_info, &info, sizeof(equant_sysinfo_t));
    return 0;
}

void syscall_handler(void *regs_ptr) {
    syscall_regs_t *regs = (syscall_regs_t *)regs_ptr;
    uint64_t syscall_no = regs->rax;

    int64_t ret = -1;

    switch (syscall_no) {
        case SYS_WRITE:
            ret = sys_write_handler((int)regs->rdi, (const char *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_EXIT:
            ret = sys_exit_handler((int)regs->rdi);
            break;
        case SYS_GETPID:
            ret = sys_getpid_handler();
            break;
        case SYS_YIELD:
            ret = sys_yield_handler();
            break;
        case SYS_BRK:
            ret = sys_brk_handler(regs->rdi);
            break;
        case SYS_SYSINFO:
            ret = sys_sysinfo_handler((equant_sysinfo_t *)regs->rdi);
            break;
        default:
            serial_puts(COM1, "[KERNEL WARNING] Unknown syscall number: ");
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
    serial_puts(COM1, "[KERNEL] System Call Subsystem Initialized (int 0x80 ABI).\n");
}