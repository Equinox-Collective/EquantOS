// src/kernel/proc/loader.c - ELF Loader with Stdio Initialization
#include "loader.h"
#include "task.h"
#include "../core/mem/vmm.h"
#include "../core/mem/pmm.h"
#include "../core/mem/memory.h"
#include "sched.h"
#include "string.h"
#include "stdio.h"
#include "../fs/vfs.h"

task_t *last_spawned_task = NULL;

extern uint64_t hhdm_offset;
#define VIRT(addr) ((uint64_t)(addr) + hhdm_offset)
#define PHYS(addr) ((uint64_t)(addr) - hhdm_offset)

// Static dummy VFS nodes for stdin, stdout, stderr
static vfs_node_t tty_stdin_node  = { .name = "stdin",  .flags = FS_FILE, .length = 0 };
static vfs_node_t tty_stdout_node = { .name = "stdout", .flags = FS_FILE, .length = 0 };
static vfs_node_t tty_stderr_node = { .name = "stderr", .flags = FS_FILE, .length = 0 };

extern vfs_file_operations_t g_tty_fops;

static vfs_node_t dev_tty_master_node = {
    .name = "tty",
    .flags = FS_FILE,
    .permissions = 0666,
    .length = 0,
    .inode = 1,
    .ops = &g_tty_fops,
    .ptr = NULL,
    .parent = NULL,
    .children = NULL,
    .next = NULL
};

bool elf_load_args(void *elf_data, uint64_t size, int argc, char **argv) {
    if (!elf_data || size < sizeof(Elf64_Ehdr)) return false;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return false;
    }

    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) return false;

    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)elf_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
            uint64_t p_vaddr = phdr[i].p_vaddr;
            uint64_t p_filesz = phdr[i].p_filesz;
            uint64_t p_memsz = phdr[i].p_memsz;
            uint64_t p_offset = phdr[i].p_offset;

            uint64_t vaddr_aligned = p_vaddr & ~0xFFFULL;
            uint64_t vaddr_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
            uint64_t total_size = vaddr_end - vaddr_aligned;
            uint32_t page_count = total_size / PAGE_SIZE;

            void *phys_pages = pmm_alloc_continuous(page_count);
            if (!phys_pages) return false;

            for (uint32_t j = 0; j < page_count; j++) {
                uint64_t virt_page = vaddr_aligned + (j * PAGE_SIZE);
                uint64_t phys_page = (uint64_t)phys_pages + (j * PAGE_SIZE);
                vmm_map(new_pml4, virt_page, phys_page, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            }

            memset((void *)VIRT((uint64_t)phys_pages + (p_vaddr - vaddr_aligned)), 0, p_memsz);
            memcpy((void *)VIRT((uint64_t)phys_pages + (p_vaddr - vaddr_aligned)), 
                   (uint8_t *)elf_data + p_offset, p_filesz);
        }
    }

    // Allocate 8 MB stack
    uint32_t stack_pages = 2048;
    uint64_t user_stack_top = 0x7FFFFFFF0000ULL;
    uint64_t user_stack_bottom = user_stack_top - ((uint64_t)stack_pages * PAGE_SIZE);
    void *top_stack_phys = NULL;

    for (uint32_t j = 0; j < stack_pages; j++) {
        void *phys = pmm_alloc();
        if (!phys) return false;
        memset((void *)VIRT(phys), 0, PAGE_SIZE);
        vmm_map(new_pml4, user_stack_bottom + ((uint64_t)j * PAGE_SIZE), 
                (uint64_t)phys, 
                PTE_PRESENT | PTE_WRITABLE | PTE_USER);

        if (j == stack_pages - 1) top_stack_phys = phys;
    }

    // System V AMD64 ABI Stack Setup
    uint64_t stack_top = user_stack_top;
    uint8_t *topk = (uint8_t *)VIRT(top_stack_phys);
    uint64_t sp = stack_top;

    if (argc <= 0 || !argv) {
        argc = 1;
        argv = (char *[]){ "bash", NULL };
    }
    if (argc > 16) argc = 16;

    uint64_t argv_u[17];
    for (int i = 0; i < argc; i++) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        size_t len = strlen(s) + 1;
        sp -= len;
        memcpy(topk + (PAGE_SIZE - (stack_top - sp)), s, len);
        argv_u[i] = sp;
    }

    const char *default_env[] = {
        "PATH=/bin/:/usr/bin/:/sbin/:/usr/sbin/:/sys/bin/",
        "USER=root",
        "HOME=/",
        "TERM=linux",
        "SHELL=/bin/bash",
        NULL
    };
    int envc = 0;
    while (default_env[envc]) envc++;

    uint64_t envp_u[16];
    for (int i = 0; i < envc; i++) {
        size_t len = strlen(default_env[i]) + 1;
        sp -= len;
        memcpy(topk + (PAGE_SIZE - (stack_top - sp)), default_env[i], len);
        envp_u[i] = sp;
    }

    sp -= 16;
    memset(topk + (PAGE_SIZE - (stack_top - sp)), 0x42, 16);
    uint64_t at_random = sp;

    sp &= ~0xFULL;

    uint64_t aux[32]; 
    int an = 0;
    uint64_t phdr_vaddr = phdr[0].p_vaddr + ehdr->e_phoff;
    aux[an++] = 3;  aux[an++] = phdr_vaddr;
    aux[an++] = 4;  aux[an++] = ehdr->e_phentsize;
    aux[an++] = 5;  aux[an++] = ehdr->e_phnum;
    aux[an++] = 6;  aux[an++] = PAGE_SIZE;
    aux[an++] = 9;  aux[an++] = ehdr->e_entry;
    aux[an++] = 11; aux[an++] = 0;
    aux[an++] = 12; aux[an++] = 0;
    aux[an++] = 13; aux[an++] = 0;
    aux[an++] = 14; aux[an++] = 0;
    aux[an++] = 23; aux[an++] = 0;
    aux[an++] = 25; aux[an++] = at_random;
    aux[an++] = 0;  aux[an++] = 0;

    int total_words = 1 + (argc + 1) + (envc + 1) + an;
    if (total_words & 1) sp -= 8;
    sp -= (uint64_t)total_words * 8;

    uint64_t *w = (uint64_t *)(topk + (PAGE_SIZE - (stack_top - sp)));
    int idx = 0;
    w[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) w[idx++] = argv_u[i];
    w[idx++] = 0;
    for (int i = 0; i < envc; i++) w[idx++] = envp_u[i];
    w[idx++] = 0;
    for (int i = 0; i < an; i++)   w[idx++] = aux[i];

    uint64_t initial_user_rsp = sp;

    // Process Structure Initialization with Standard File Descriptors
    process_t *proc = (process_t *)kmalloc(sizeof(process_t));
    if (!proc) return false;
    memset(proc, 0, sizeof(process_t));

    proc->pid = next_pid++; // Auto-increment PID!
    proc->pgid = proc->pid;
    proc->cr3 = PHYS(new_pml4);
    proc->brk = 0x600000;
    strcpy(proc->cwd, "/");

    // Initialize FDs 0, 1, 2
    proc->files[0] = &dev_tty_master_node; // STDIN  -> /dev/tty
proc->files[1] = &dev_tty_master_node; // STDOUT -> /dev/tty
proc->files[2] = &dev_tty_master_node; // STDERR -> /dev/tty

    task_t *task = (task_t *)kmalloc(sizeof(task_t));
    if (!task) return false;
    memset(task, 0, sizeof(task_t));

    task_init_fpu(task);
    
    task->id = proc->pid;
    task->priority = PRIO_INTERACTIVE;
    task->time_slice = 10;
    task->state = TASK_STATE_RUNNABLE;
    task->running = true;
    task->process = proc;
    task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    uint64_t *stack = (uint64_t *)task->kstack_at_bottom;

    *--stack = 0x1B;                  // SS (User Data)
    *--stack = initial_user_rsp;      // User Stack Pointer
    *--stack = 0x202;                 // RFLAGS (IF=1)
    *--stack = 0x23;                  // CS (User Code)
    *--stack = ehdr->e_entry;         // RIP

    *--stack = 0; // Error code
    *--stack = 0; // Int no

    for (int k = 0; k < 15; k++) {
        *--stack = 0;
    }

    task->rsp = (uint64_t)stack;

    extern task_t *task_list;
    if (task_list) {
        task->next = task_list->next;
        task->prev = task_list;
        task_list->next->prev = task;
        task_list->next = task;
    } else {
        task->next = task;
        task->prev = task;
        task_list = task;
    }

    last_spawned_task = task;
    sched_enqueue(task);
    return true;
}

// Замена текущего процесса на новый ELF без создания новых задач
bool elf_execve_replace(void *elf_data, uint64_t size, int argc, char **argv, uint64_t *out_entry, uint64_t *out_rsp, uint64_t *out_cr3) {
    if (!elf_data || size < sizeof(Elf64_Ehdr)) return false;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return false;
    }

    // 1. Создаем новое адресное пространство
    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) return false;

    // 2. Загружаем PT_LOAD сегменты
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)elf_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
            uint64_t p_vaddr = phdr[i].p_vaddr;
            uint64_t p_filesz = phdr[i].p_filesz;
            uint64_t p_memsz = phdr[i].p_memsz;
            uint64_t p_offset = phdr[i].p_offset;

            uint64_t vaddr_aligned = p_vaddr & ~0xFFFULL;
            uint64_t vaddr_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
            uint64_t total_size = vaddr_end - vaddr_aligned;
            uint32_t page_count = total_size / PAGE_SIZE;

            void *phys_pages = pmm_alloc_continuous(page_count);
            if (!phys_pages) return false;

            for (uint32_t j = 0; j < page_count; j++) {
                uint64_t virt_page = vaddr_aligned + (j * PAGE_SIZE);
                uint64_t phys_page = (uint64_t)phys_pages + (j * PAGE_SIZE);
                vmm_map(new_pml4, virt_page, phys_page, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            }

            memset((void *)VIRT((uint64_t)phys_pages + (p_vaddr - vaddr_aligned)), 0, p_memsz);
            memcpy((void *)VIRT((uint64_t)phys_pages + (p_vaddr - vaddr_aligned)), 
                   (uint8_t *)elf_data + p_offset, p_filesz);
        }
    }

    // 3. Выделяем стек
    uint32_t stack_pages = 2048;
    uint64_t user_stack_top = 0x7FFFFFFF0000ULL;
    uint64_t user_stack_bottom = user_stack_top - ((uint64_t)stack_pages * PAGE_SIZE);
    void *top_stack_phys = NULL;

    for (uint32_t j = 0; j < stack_pages; j++) {
        void *phys = pmm_alloc();
        if (!phys) return false;
        memset((void *)VIRT(phys), 0, PAGE_SIZE);
        vmm_map(new_pml4, user_stack_bottom + ((uint64_t)j * PAGE_SIZE), 
                (uint64_t)phys, 
                PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        if (j == stack_pages - 1) top_stack_phys = phys;
    }

    // 4. Формируем System V ABI стек
    uint64_t stack_top = user_stack_top;
    uint8_t *topk = (uint8_t *)VIRT(top_stack_phys);
    uint64_t sp = stack_top;

    if (argc <= 0 || !argv) {
        argc = 1;
        argv = (char *[]){ "busybox", NULL };
    }
    if (argc > 16) argc = 16;

    uint64_t argv_u[17];
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        size_t len = strlen(s) + 1;
        sp -= len;
        memcpy(topk + (PAGE_SIZE - (stack_top - sp)), s, len);
        argv_u[i] = sp;
    }

    const char *default_env[] = {
        "PATH=/bin/:/usr/bin/:/sbin/:/usr/sbin/:/sys/bin/",
        "USER=root",
        "HOME=/",
        "TERM=linux",
        "SHELL=/bin/bash",
        NULL
    };
    int envc = 0;
    while (default_env[envc]) envc++;

    uint64_t envp_u[16];
    for (int i = 0; i < envc; i++) {
        size_t len = strlen(default_env[i]) + 1;
        sp -= len;
        memcpy(topk + (PAGE_SIZE - (stack_top - sp)), default_env[i], len);
        envp_u[i] = sp;
    }

    sp -= 16;
    memset(topk + (PAGE_SIZE - (stack_top - sp)), 0x42, 16);
    uint64_t at_random = sp;

    sp &= ~0xFULL;

    uint64_t aux[32]; 
    int an = 0;
    uint64_t phdr_vaddr = phdr[0].p_vaddr + ehdr->e_phoff;
    aux[an++] = 3;  aux[an++] = phdr_vaddr;
    aux[an++] = 4;  aux[an++] = ehdr->e_phentsize;
    aux[an++] = 5;  aux[an++] = ehdr->e_phnum;
    aux[an++] = 6;  aux[an++] = PAGE_SIZE;
    aux[an++] = 9;  aux[an++] = ehdr->e_entry;
    aux[an++] = 11; aux[an++] = 0;
    aux[an++] = 12; aux[an++] = 0;
    aux[an++] = 13; aux[an++] = 0;
    aux[an++] = 14; aux[an++] = 0;
    aux[an++] = 23; aux[an++] = 0;
    aux[an++] = 25; aux[an++] = at_random;
    aux[an++] = 0;  aux[an++] = 0;

    int total_words = 1 + (argc + 1) + (envc + 1) + an;
    if (total_words & 1) sp -= 8;
    sp -= (uint64_t)total_words * 8;

    uint64_t *w = (uint64_t *)(topk + (PAGE_SIZE - (stack_top - sp)));
    int idx = 0;
    w[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) w[idx++] = argv_u[i];
    w[idx++] = 0;
    for (int i = 0; i < envc; i++) w[idx++] = envp_u[i];
    w[idx++] = 0;
    for (int i = 0; i < an; i++)   w[idx++] = aux[i];

    *out_entry = ehdr->e_entry;
    *out_rsp = sp;
    *out_cr3 = PHYS(new_pml4);
    return true;
}

bool elf_load(void *elf_data, uint64_t size) {
    char *default_argv[] = { "sh", NULL };
    return elf_load_args(elf_data, size, 1, default_argv);
}