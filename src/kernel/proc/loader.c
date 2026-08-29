// src/kernel/proc/loader.c
#include "loader.h"
#include "task.h"
#include "../core/mem/vmm.h"
#include "../core/mem/pmm.h"
#include "../core/mem/memory.h"
#include "sched.h"
#include "string.h"
#include "stdio.h"

task_t *last_spawned_task = NULL;

extern uint64_t hhdm_offset;
#define VIRT(addr) ((uint64_t)(addr) + hhdm_offset)
#define PHYS(addr) ((uint64_t)(addr) - hhdm_offset)

bool elf_load_args(void *elf_data, uint64_t size, int argc, char **argv) {
    printf("DEBUG [1/7]: elf_load entered. Data: %x, Size: %u bytes\n", (uint64_t)elf_data, (unsigned int)size);

    if (!elf_data || size < sizeof(Elf64_Ehdr)) {
        printf("[FAIL 1] ELF Loader: Invalid ELF data pointer or size.\n");
        return false;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;

    // Verify ELF Magic bytes: 0x7F 'E' 'L' 'F'
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        printf("[FAIL 2] ELF Loader: Invalid ELF magic signature.\n");
        return false;
    }

    printf("DEBUG [2/7]: ELF Header verified. Entry point: %x, PH Off: %x, PH Num: %u\n", 
           ehdr->e_entry, ehdr->e_phoff, ehdr->e_phnum);

    // 1. Create a new virtual address space for the process
    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        printf("[FAIL 3] ELF Loader: Failed to create address space.\n");
        return false;
    }
    printf("DEBUG [3/7]: New PML4 created successfully at %x\n", (uint64_t)new_pml4);

    // 2. Iterate through Program Headers and load PT_LOAD segments
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)elf_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD segment
            uint64_t p_vaddr = phdr[i].p_vaddr;
            uint64_t p_filesz = phdr[i].p_filesz;
            uint64_t p_memsz = phdr[i].p_memsz;
            uint64_t p_offset = phdr[i].p_offset;

            printf("DEBUG [4/7]: Loading segment %u -> VMA: %x, FileSz: %u, MemSz: %u\n", 
                   i, p_vaddr, (unsigned int)p_filesz, (unsigned int)p_memsz);

            uint64_t vaddr_aligned = p_vaddr & ~0xFFFULL;
            uint64_t vaddr_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
            uint64_t total_size = vaddr_end - vaddr_aligned;
            uint32_t page_count = total_size / PAGE_SIZE;

            void *phys_pages = pmm_alloc_continuous(page_count);
            if (!phys_pages) {
                printf("[FAIL 4] ELF Loader: Out of physical memory for segment %u.\n", i);
                return false;
            }

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
    printf("DEBUG [5/7]: All segments mapped and copied.\n");

    // 3. Allocate a user stack for the task (16 KB)
    uint32_t stack_pages = 4;
    void *stack_phys = pmm_alloc_continuous(stack_pages);
    if (!stack_phys) {
        printf("[FAIL 5] ELF Loader: Failed to allocate physical memory for task stack.\n");
        return false;
    }

    uint64_t user_stack_top = 0x7FFFFFFF0000ULL;
    uint64_t user_stack_bottom = user_stack_top - (stack_pages * PAGE_SIZE);

    for (uint32_t j = 0; j < stack_pages; j++) {
        vmm_map(new_pml4, user_stack_bottom + (j * PAGE_SIZE), 
                (uint64_t)stack_phys + (j * PAGE_SIZE), 
                PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    // =========================================================================
    //        ДИНАМИЧЕСКИЙ СТЕК SYSTEM V AMD64 ABI (ARGC, ARGV, ENVP, AUXV)
    // =========================================================================
    uint8_t *k_stack_top = (uint8_t *)VIRT((uint64_t)stack_phys + (stack_pages * PAGE_SIZE));
    uint8_t *k_ptr = k_stack_top;

    // A. 16 случайных байт для AT_RANDOM
    k_ptr -= 16;
    memset(k_ptr, 0x42, 16);
    uint64_t user_rand_vaddr = user_stack_top - (uint64_t)(k_stack_top - k_ptr);

    // B. Базовые переменные окружения (ENVP)
    const char *default_env[] = {
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin:/sys/bin",
        "USER=root",
        "HOME=/",
        "TERM=linux",
        "SHELL=/bin/sh",
        NULL
    };
    int envc = 0;
    while (default_env[envc]) envc++;

    uint64_t env_vaddrs[8];
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(default_env[i]) + 1;
        k_ptr -= len;
        memcpy(k_ptr, default_env[i], len);
        env_vaddrs[i] = user_stack_top - (uint64_t)(k_stack_top - k_ptr);
    }

    // C. Копируем строки аргументов (ARGV)
    if (argc <= 0 || !argv) {
        argc = 1;
        argv = (char *[]){ "sh", NULL };
    }
    if (argc > 16) argc = 16;

    uint64_t arg_vaddrs[16];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        k_ptr -= len;
        memcpy(k_ptr, argv[i], len);
        arg_vaddrs[i] = user_stack_top - (uint64_t)(k_stack_top - k_ptr);
    }

    // D. Выравниваем стек по 16 байт
    uint64_t *k_sp = (uint64_t *)((uintptr_t)k_ptr & ~0xFULL);

    // Выравнивание глубины стека: проверяем четность элементов для System V ABI (RSP % 16 == 0)
    size_t total_words = 26 + (envc + 1) + (argc + 1) + 1;
    if ((total_words % 2) != 0) {
        *--k_sp = 0; // Padding word
    }

    // E. Auxiliary Vector (AUXV)
    *--k_sp = 0;                   *--k_sp = 0;  // AT_NULL (0)
    *--k_sp = user_rand_vaddr;     *--k_sp = 25; // AT_RANDOM (25)
    *--k_sp = 0;                   *--k_sp = 23; // AT_SECURE (23)
    *--k_sp = 0;                   *--k_sp = 14; // AT_EGID (14)
    *--k_sp = 0;                   *--k_sp = 13; // AT_GID (13)
    *--k_sp = 0;                   *--k_sp = 12; // AT_EUID (12)
    *--k_sp = 0;                   *--k_sp = 11; // AT_UID (11)
    *--k_sp = ehdr->e_entry;       *--k_sp = 9;  // AT_ENTRY (9)
    *--k_sp = PAGE_SIZE;           *--k_sp = 6;  // AT_PAGESZ (6)
    *--k_sp = ehdr->e_phnum;       *--k_sp = 5;  // AT_PHNUM (5)
    *--k_sp = ehdr->e_phentsize;   *--k_sp = 4;  // AT_PHENT (4)
    uint64_t phdr_vaddr = phdr[0].p_vaddr + ehdr->e_phoff;
    *--k_sp = phdr_vaddr;          *--k_sp = 3;  // AT_PHDR (3)

    // F. Массив указателей envp[]
    *--k_sp = 0; // NULL terminator
    for (int i = envc - 1; i >= 0; i--) {
        *--k_sp = env_vaddrs[i];
    }

    // G. Массив указателей argv[]
    *--k_sp = 0; // NULL terminator
    for (int i = argc - 1; i >= 0; i--) {
        *--k_sp = arg_vaddrs[i];
    }

    // H. Число аргументов argc
    *--k_sp = (uint64_t)argc;

    uint64_t initial_user_rsp = user_stack_top - (uint64_t)(k_stack_top - (uint8_t *)k_sp);

    printf("DEBUG [6/7]: System V Stack initialized with %d args. User RSP: %x\n", argc, initial_user_rsp);

    // 4. Create process and task structures
    process_t *proc = (process_t *)kmalloc(sizeof(process_t));
    if (!proc) return false;
    memset(proc, 0, sizeof(process_t));
    proc->pid = 2;
    proc->cr3 = PHYS(new_pml4);
    proc->brk = 0x600000;

    task_t *task = (task_t *)kmalloc(sizeof(task_t));
    if (!task) return false;
    memset(task, 0, sizeof(task_t));

    task_init_fpu(task);
    
    task->id = proc->pid;
    task->state = TASK_STATE_RUNNABLE;
    task->running = true;
    task->process = proc;
    task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    uint64_t *stack = (uint64_t *)task->kstack_at_bottom;

    *--stack = 0x1B;                  // SS
    *--stack = initial_user_rsp;      // RSP с нашими аргументами!
    *--stack = 0x202;                 // RFLAGS
    *--stack = 0x23;                  // CS
    *--stack = ehdr->e_entry;         // RIP

    *--stack = 0;
    *--stack = 0;

    for (int k = 0; k < 15; k++) {
        *--stack = 0;
    }

    task->rsp = (uint64_t)stack;

    printf("DEBUG [7/7]: Task structure fully ready. Enqueueing to scheduler...\n");
    last_spawned_task = task;
    sched_enqueue(task);
    return true;
}

bool elf_load(void *elf_data, uint64_t size) {
    char *default_argv[] = { "sh", NULL };
    return elf_load_args(elf_data, size, 1, default_argv);
}