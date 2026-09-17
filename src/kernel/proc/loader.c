// src/kernel/proc/loader.c - ELF Loader with Stdio Initialization & Dynamic Args
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

static void push_to_user_stack(page_table_t *pml4, uint64_t *sp, const void *data, size_t len) {
    *sp -= len;
    uint64_t curr = *sp;
    const uint8_t *src = (const uint8_t *)data;

    while (len > 0) {
        uint64_t page_vaddr = curr & ~0xFFFULL;
        uint64_t offset_in_page = curr & 0xFFFULL;
        uint64_t phys_page = vmm_get_phys(pml4, page_vaddr) & ~0xFFFULL;
        if (!phys_page) return;

        uint64_t chunk = PAGE_SIZE - offset_in_page;
        if (chunk > len) chunk = len;

        memcpy((void *)VIRT(phys_page + offset_in_page), src, chunk);

        curr += chunk;
        src += chunk;
        len -= chunk;
    }
}

bool elf_load_args(void *elf_data, uint64_t size, int argc, char **argv) {
    if (!elf_data || size < sizeof(Elf64_Ehdr)) return false;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return false;
    }

    uint64_t load_base = (ehdr->e_type == 3) ? 0x400000ULL : 0ULL;

    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) return false;

    uint64_t max_vaddr_end = 0;
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)elf_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
            uint64_t p_vaddr = phdr[i].p_vaddr + load_base;
            uint64_t p_filesz = phdr[i].p_filesz;
            uint64_t p_memsz = phdr[i].p_memsz;
            uint64_t p_offset = phdr[i].p_offset;

            uint64_t vaddr_aligned = p_vaddr & ~0xFFFULL;
            uint64_t vaddr_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
            if (vaddr_end > max_vaddr_end) max_vaddr_end = vaddr_end;

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

    uint32_t stack_pages = 2048;
    uint64_t user_stack_top = 0x7FFFF0000000ULL;
    uint64_t user_stack_bottom = user_stack_top - ((uint64_t)stack_pages * PAGE_SIZE);

    for (uint32_t j = 0; j < stack_pages; j++) {
        void *phys = pmm_alloc();
        if (!phys) return false;
        memset((void *)VIRT(phys), 0, PAGE_SIZE);
        vmm_map(new_pml4, user_stack_bottom + ((uint64_t)j * PAGE_SIZE), 
                (uint64_t)phys, 
                PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    uint64_t sp = user_stack_top;

    if (argc <= 0 || !argv) {
        argc = 1;
        argv = (char *[]){ "bash", NULL };
    }
    if (argc > 16) argc = 16;

    uint64_t argv_u[17];
    for (int i = 0; i < argc; i++) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        size_t len = strlen(s) + 1;
        push_to_user_stack(new_pml4, &sp, s, len);
        argv_u[i] = sp;
    }
    argv_u[argc] = 0;

    const char *default_env[] = {
        "PATH=/bin:/usr/bin:/sys/bin:/sbin:/usr/sbin:/drives/ext2_nvme/bin",
        "USER=root",
        "HOME=/root",
        "TERM=linux",
        "SHELL=/bin/bash",
        "DISPLAY=:0",
        NULL
    };
    int envc = 0;
    while (default_env[envc]) envc++;

    uint64_t envp_u[16];
    for (int i = 0; i < envc; i++) {
        size_t len = strlen(default_env[i]) + 1;
        push_to_user_stack(new_pml4, &sp, default_env[i], len);
        envp_u[i] = sp;
    }
    envp_u[envc] = 0;

    uint8_t rand_bytes[16];
    memset(rand_bytes, 0x42, 16);
    push_to_user_stack(new_pml4, &sp, rand_bytes, 16);
    uint64_t at_random = sp;

    sp &= ~0xFULL;

    uint64_t phdr_vaddr = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 6) {
            phdr_vaddr = phdr[i].p_vaddr + load_base;
            break;
        }
    }
    if (!phdr_vaddr) {
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == 1) {
                if (phdr[i].p_offset <= ehdr->e_phoff &&
                    ehdr->e_phoff < phdr[i].p_offset + phdr[i].p_filesz) {
                    phdr_vaddr = phdr[i].p_vaddr + load_base + (ehdr->e_phoff - phdr[i].p_offset);
                    break;
                }
            }
        }
    }
    if (!phdr_vaddr) {
        phdr_vaddr = load_base + ehdr->e_phoff;
    }
    uint64_t entry_point = ehdr->e_entry + load_base;

    uint64_t aux[32]; 
    int an = 0;
    aux[an++] = 3;  aux[an++] = phdr_vaddr;
    aux[an++] = 4;  aux[an++] = ehdr->e_phentsize;
    aux[an++] = 5;  aux[an++] = ehdr->e_phnum;
    aux[an++] = 6;  aux[an++] = PAGE_SIZE;
    aux[an++] = 7;  aux[an++] = (ehdr->e_type == 3) ? load_base : 0;
    aux[an++] = 8;  aux[an++] = 0;
    aux[an++] = 9;  aux[an++] = entry_point;
    aux[an++] = 11; aux[an++] = 0;
    aux[an++] = 12; aux[an++] = 0;
    aux[an++] = 13; aux[an++] = 0;
    aux[an++] = 14; aux[an++] = 0;
    aux[an++] = 23; aux[an++] = 0;
    aux[an++] = 25; aux[an++] = at_random;
    aux[an++] = 0;  aux[an++] = 0;

    int total_words = 1 + (argc + 1) + (envc + 1) + an;
    if (total_words & 1) sp -= 8;

    uint64_t vector_table[128];
    int idx = 0;
    vector_table[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) vector_table[idx++] = argv_u[i];
    vector_table[idx++] = 0;
    for (int i = 0; i < envc; i++) vector_table[idx++] = envp_u[i];
    vector_table[idx++] = 0;
    for (int i = 0; i < an; i++)   vector_table[idx++] = aux[i];

    push_to_user_stack(new_pml4, &sp, vector_table, total_words * sizeof(uint64_t));

    uint64_t initial_user_rsp = sp;

    process_t *proc = (process_t *)kmalloc(sizeof(process_t));
    if (!proc) return false;
    memset(proc, 0, sizeof(process_t));

    proc->pid = next_pid++;
    proc->pgid = proc->pid;
    proc->cr3 = PHYS(new_pml4);
    proc->brk = (max_vaddr_end + PAGE_SIZE - 1) & ~0xFFFULL;
    strcpy(proc->cwd, "/");

    proc->files[0] = &dev_tty_master_node;
    proc->files[1] = &dev_tty_master_node;
    proc->files[2] = &dev_tty_master_node;

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

    *--stack = 0x1B;
    *--stack = initial_user_rsp;
    *--stack = 0x202;
    *--stack = 0x23;
    *--stack = entry_point;

    *--stack = 0;
    *--stack = 0;

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

bool elf_execve_replace(void *elf_data, uint64_t size, int argc, char **argv, uint64_t *out_entry, uint64_t *out_rsp, uint64_t *out_cr3) {
    if (!elf_data || size < sizeof(Elf64_Ehdr)) return false;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return false;
    }

    uint64_t load_base = (ehdr->e_type == 3) ? 0x400000ULL : 0ULL;

    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) return false;

    uint64_t max_vaddr_end = 0;
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)elf_data + ehdr->e_phoff);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != 1) continue;

        uint64_t seg_vaddr  = phdr[i].p_vaddr + load_base;
        uint64_t seg_filesz = phdr[i].p_filesz;
        uint64_t seg_memsz  = phdr[i].p_memsz;
        uint64_t seg_offset = phdr[i].p_offset;

        uint64_t vaddr_start = seg_vaddr;
        uint64_t vaddr_end   = seg_vaddr + seg_memsz;
        if (vaddr_end > max_vaddr_end) max_vaddr_end = vaddr_end;

        uint64_t page_start = vaddr_start & ~0xFFFULL;
        uint64_t page_end   = (vaddr_end + 0xFFF) & ~0xFFFULL;

        for (uint64_t vpage = page_start; vpage < page_end; vpage += PAGE_SIZE) {
            uint64_t existing_phys = vmm_get_phys(new_pml4, vpage);
            if (!existing_phys) {
                void *new_phys = pmm_alloc();
                if (!new_phys) {
                    vmm_destroy_address_space(PHYS(new_pml4));
                    return false;
                }
                memset((void *)VIRT((uint64_t)new_phys), 0, PAGE_SIZE);
                vmm_map(new_pml4, vpage, (uint64_t)new_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            }
        }

        uint64_t bytes_copied = 0;
        while (bytes_copied < seg_filesz) {
            uint64_t curr_vaddr = seg_vaddr + bytes_copied;
            uint64_t page_off   = curr_vaddr & 0xFFFULL;
            uint64_t chunk      = PAGE_SIZE - page_off;
            if (chunk > (seg_filesz - bytes_copied)) {
                chunk = seg_filesz - bytes_copied;
            }

            uint64_t phys = vmm_get_phys(new_pml4, curr_vaddr);
            uint64_t phys_page = phys & ~0xFFFULL;

            memcpy((void *)VIRT(phys_page + page_off),
                   (uint8_t *)elf_data + seg_offset + bytes_copied,
                   chunk);

            bytes_copied += chunk;
        }
    }

    uint32_t stack_pages = 2048;
    uint64_t user_stack_top = 0x7FFFF0000000ULL;
    uint64_t user_stack_bottom = user_stack_top - ((uint64_t)stack_pages * PAGE_SIZE);

    for (uint32_t j = 0; j < stack_pages; j++) {
        void *phys = pmm_alloc();
        if (!phys) {
            vmm_destroy_address_space(PHYS(new_pml4));
            return false;
        }
        memset((void *)VIRT(phys), 0, PAGE_SIZE);
        vmm_map(new_pml4, user_stack_bottom + ((uint64_t)j * PAGE_SIZE), 
                (uint64_t)phys, 
                PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    uint64_t sp = user_stack_top;

    if (argc <= 0 || !argv) {
        argc = 1;
        argv = (char *[]){ "app", NULL };
    }
    if (argc > 16) argc = 16;

    uint64_t argv_u[17];
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        size_t len = strlen(s) + 1;
        push_to_user_stack(new_pml4, &sp, s, len);
        argv_u[i] = sp;
    }
    argv_u[argc] = 0;

    const char *default_env[] = {
        "PATH=/bin:/usr/bin:/sys/bin:/",
        "USER=root",
        "HOME=/",
        "TERM=linux",
        "SHELL=/bin/bash",
        "DISPLAY=:0",
        NULL
    };
    int envc = 0;
    while (default_env[envc]) envc++;

    uint64_t envp_u[16];
    for (int i = 0; i < envc; i++) {
        size_t len = strlen(default_env[i]) + 1;
        push_to_user_stack(new_pml4, &sp, default_env[i], len);
        envp_u[i] = sp;
    }
    envp_u[envc] = 0;

    uint8_t rand_bytes[16];
    memset(rand_bytes, 0x42, 16);
    push_to_user_stack(new_pml4, &sp, rand_bytes, 16);
    uint64_t at_random = sp;

    sp &= ~0xFULL;

    uint64_t phdr_vaddr = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 6) {
            phdr_vaddr = phdr[i].p_vaddr + load_base;
            break;
        }
    }
    if (!phdr_vaddr) {
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == 1) {
                if (phdr[i].p_offset <= ehdr->e_phoff &&
                    ehdr->e_phoff < phdr[i].p_offset + phdr[i].p_filesz) {
                    phdr_vaddr = phdr[i].p_vaddr + load_base + (ehdr->e_phoff - phdr[i].p_offset);
                    break;
                }
            }
        }
    }
    if (!phdr_vaddr) {
        phdr_vaddr = load_base + ehdr->e_phoff;
    }

    uint64_t entry_point = ehdr->e_entry + load_base;

    uint64_t aux[32]; 
    int an = 0;
    aux[an++] = 3;  aux[an++] = phdr_vaddr;
    aux[an++] = 4;  aux[an++] = ehdr->e_phentsize;
    aux[an++] = 5;  aux[an++] = ehdr->e_phnum;
    aux[an++] = 6;  aux[an++] = PAGE_SIZE;
    aux[an++] = 7;  aux[an++] = (ehdr->e_type == 3) ? load_base : 0;
    aux[an++] = 8;  aux[an++] = 0;
    aux[an++] = 9;  aux[an++] = entry_point;
    aux[an++] = 11; aux[an++] = 0;
    aux[an++] = 12; aux[an++] = 0;
    aux[an++] = 13; aux[an++] = 0;
    aux[an++] = 14; aux[an++] = 0;
    aux[an++] = 23; aux[an++] = 0;
    aux[an++] = 25; aux[an++] = at_random;
    aux[an++] = 0;  aux[an++] = 0;

    int total_words = 1 + (argc + 1) + (envc + 1) + an;
    if (total_words % 2 != 0) {
        sp -= 8;
    }

    uint64_t vector_table[128];
    int idx = 0;
    vector_table[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) vector_table[idx++] = argv_u[i];
    vector_table[idx++] = 0;
    for (int i = 0; i < envc; i++) vector_table[idx++] = envp_u[i];
    vector_table[idx++] = 0;
    for (int i = 0; i < an; i++)   vector_table[idx++] = aux[i];

    push_to_user_stack(new_pml4, &sp, vector_table, total_words * sizeof(uint64_t));

    if (current_task && current_task->process) {
        current_task->process->brk = (max_vaddr_end + PAGE_SIZE - 1) & ~0xFFFULL;
    }

    *out_entry = entry_point;
    *out_rsp = sp;
    *out_cr3 = PHYS(new_pml4);
    return true;
}

bool elf_load(void *elf_data, uint64_t size) {
    char *default_argv[] = { "sh", NULL };
    return elf_load_args(elf_data, size, 1, default_argv);
}