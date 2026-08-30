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

    // Проверка ELF Magic: 0x7F 'E' 'L' 'F'
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        printf("[FAIL 2] ELF Loader: Invalid ELF magic signature.\n");
        return false;
    }

    printf("DEBUG [2/7]: ELF Header verified. Entry point: %x, PH Off: %x, PH Num: %u\n", 
           ehdr->e_entry, ehdr->e_phoff, ehdr->e_phnum);

    // 1. Создаем новое виртуальное адресное пространство (PML4)
    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        printf("[FAIL 3] ELF Loader: Failed to create address space.\n");
        return false;
    }
    printf("DEBUG [3/7]: New PML4 created successfully at %x\n", (uint64_t)new_pml4);

    // 2. Загружаем сегменты PT_LOAD
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)elf_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
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

    // 3. Выделяем стек пользователя (8 МБ = 2048 страниц постранично)
    uint32_t stack_pages = 2048;
    uint64_t user_stack_top = 0x7FFFFFFF0000ULL;
    uint64_t user_stack_bottom = user_stack_top - ((uint64_t)stack_pages * PAGE_SIZE);

    void *top_stack_phys = NULL;

    for (uint32_t j = 0; j < stack_pages; j++) {
        void *phys = pmm_alloc();
        if (!phys) {
            printf("[FAIL 5] ELF Loader: Out of physical memory for user stack.\n");
            return false;
        }
        memset((void *)VIRT(phys), 0, PAGE_SIZE);
        vmm_map(new_pml4, user_stack_bottom + ((uint64_t)j * PAGE_SIZE), 
                (uint64_t)phys, 
                PTE_PRESENT | PTE_WRITABLE | PTE_USER);

        // Запоминаем физический адрес самой верхней страницы стека (где лежат argv/envp)
        if (j == stack_pages - 1) {
            top_stack_phys = phys;
        }
    }

    // =========================================================================
    //        ДИНАМИЧЕСКИЙ СТЕК SYSTEM V AMD64 ABI (ARGC, ARGV, ENVP, AUXV)
    // =========================================================================
    uint64_t stack_top = user_stack_top;
    uint8_t *topk = (uint8_t *)VIRT(top_stack_phys);
    uint64_t sp = stack_top;

    // Защита аргументов
    if (argc <= 0 || !argv) {
        argc = 1;
        argv = (char *[]){ "bash", NULL };
    }
    if (argc > 16) argc = 16;

    // 1. Копируем строки ARGV на стек
    uint64_t argv_u[17];
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        size_t len = strlen(s) + 1;
        sp -= len;
        memcpy(topk + (PAGE_SIZE - (stack_top - sp)), s, len);
        argv_u[i] = sp;
    }

    // 2. Копируем строки ENVP на стек
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

    // 3. 16 байт для AT_RANDOM
    sp -= 16;
    memset(topk + (PAGE_SIZE - (stack_top - sp)), 0x42, 16);
    uint64_t at_random = sp;

    // 4. Выравнивание 16 байт
    sp &= ~0xFULL;

    // 5. Формируем Auxiliary Vector (AUXV)
    uint64_t aux[32]; 
    int an = 0;
    uint64_t phdr_vaddr = phdr[0].p_vaddr + ehdr->e_phoff;
    aux[an++] = 3;  aux[an++] = phdr_vaddr;          // AT_PHDR
    aux[an++] = 4;  aux[an++] = ehdr->e_phentsize;   // AT_PHENT
    aux[an++] = 5;  aux[an++] = ehdr->e_phnum;       // AT_PHNUM
    aux[an++] = 6;  aux[an++] = PAGE_SIZE;           // AT_PAGESZ
    aux[an++] = 9;  aux[an++] = ehdr->e_entry;       // AT_ENTRY
    aux[an++] = 11; aux[an++] = 0;                   // AT_UID
    aux[an++] = 12; aux[an++] = 0;                   // AT_EUID
    aux[an++] = 13; aux[an++] = 0;                   // AT_GID
    aux[an++] = 14; aux[an++] = 0;                   // AT_EGID
    aux[an++] = 23; aux[an++] = 0;                   // AT_SECURE
    aux[an++] = 25; aux[an++] = at_random;           // AT_RANDOM
    aux[an++] = 0;  aux[an++] = 0;                   // AT_NULL

    // 6. Вычисляем количество слов и выравниваем глубину стека
    int total_words = 1 + (argc + 1) + (envc + 1) + an;
    if (total_words & 1) sp -= 8;

    sp -= (uint64_t)total_words * 8;

    // 7. Записываем массив указателей в стек
    uint64_t *w = (uint64_t *)(topk + (PAGE_SIZE - (stack_top - sp)));
    int idx = 0;
    w[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) w[idx++] = argv_u[i];
    w[idx++] = 0; // NULL terminator для argv
    for (int i = 0; i < envc; i++) w[idx++] = envp_u[i];
    w[idx++] = 0; // NULL terminator для envp
    for (int i = 0; i < an; i++)   w[idx++] = aux[i];

    uint64_t initial_user_rsp = sp;

    printf("DEBUG [6/7]: System V Stack initialized with %d args. User RSP: %x\n", argc, initial_user_rsp);

    // 4. Создаем структуры процесса и потока
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

    *--stack = 0x1B;                  // SS (User Data)
    *--stack = initial_user_rsp;      // RSP с нашими аргументами!
    *--stack = 0x202;                 // RFLAGS (IF включен)
    *--stack = 0x23;                  // CS (User Code)
    *--stack = ehdr->e_entry;         // RIP

    *--stack = 0; // Error code
    *--stack = 0; // Int no

    for (int k = 0; k < 15; k++) {
        *--stack = 0;
    }

    task->rsp = (uint64_t)stack;

    printf("DEBUG [7/7]: Task structure fully ready. Enqueueing to scheduler...\n");
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