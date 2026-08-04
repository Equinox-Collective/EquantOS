#include "loader.h"
#include "task.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../mm/memory.h"
#include "../core/panic.h"
#include "string.h"
#include "stdio.h"

extern uint64_t hhdm_offset;
#define VIRT(addr) ((uint64_t)(addr) + hhdm_offset)
#define PHYS(addr) ((uint64_t)(addr) - hhdm_offset)

bool elf_load(void *elf_data, uint64_t size) {
    if (!elf_data || size < sizeof(Elf64_Ehdr)) {
        printf("ELF Loader: Invalid ELF data pointer or size (size: %u bytes).\n", (unsigned int)size);
        return false;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;

    // DEBUG: Print actual bytes found at the start of the module
    printf("ELF Debug: Size = %u bytes\n", (unsigned int)size);
    printf("ELF Debug: Magic bytes found = %x %x %x %x (Expected: 7f 45 4c 46)\n", 
           ehdr->e_ident[0], ehdr->e_ident[1], ehdr->e_ident[2], ehdr->e_ident[3]);

    // Verify ELF Magic bytes: 0x7F 'E' 'L' 'F'
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        printf("ELF Loader: Invalid ELF magic signature.\n");
        return false;
    }

    printf("ELF Loader: Valid ELF64 binary found. Entry point: %x\n", ehdr->e_entry);

    // 1. Create a new virtual address space for the process
    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        printf("ELF Loader: Failed to create address space.\n");
        return false;
    }
    

    // 2. Iterate through Program Headers and load PT_LOAD segments
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t * )elf_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD segment
            uint64_t p_vaddr = phdr[i].p_vaddr;
            uint64_t p_filesz = phdr[i].p_filesz;
            uint64_t p_memsz = phdr[i].p_memsz;
            uint64_t p_offset = phdr[i].p_offset;

            // Align virtual address to page boundary
            uint64_t vaddr_aligned = p_vaddr & ~0xFFFULL;
            uint64_t vaddr_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
            uint64_t total_size = vaddr_end - vaddr_aligned;
            uint32_t page_count = total_size / PAGE_SIZE;

            // Allocate physical pages for the segment
            void *phys_pages = pmm_alloc_continuous(page_count);
            if (!phys_pages) {
                printf("ELF Loader: Out of physical memory for segment loading.\n");
                return false;
            }

            // Map each page into the new address space
            for (uint32_t j = 0; j < page_count; j++) {
                uint64_t virt_page = vaddr_aligned + (j * PAGE_SIZE);
                uint64_t phys_page = (uint64_t)phys_pages + (j * PAGE_SIZE);
                
                // Map with Present, Writable, and User flags
                vmm_map(new_pml4, virt_page, phys_page, PTE_PRESENT | PTE_WRITABLE);
            }

            // Copy segment data from file buffer into newly allocated virtual space
            // Since new_pml4 maps lower half, we can copy using HHDM or direct mapping
            memset((void *)VIRT((uint64_t)phys_pages + (p_vaddr - vaddr_aligned)), 0, p_memsz);
            memcpy((void *)VIRT((uint64_t)phys_pages + (p_vaddr - vaddr_aligned)), 
                   (uint8_t *)elf_data + p_offset, p_filesz);

            printf("ELF Loader: Loaded segment to VMA %x (size: %u bytes)\n", p_vaddr, (unsigned int)p_memsz);
        }
    }

    // 3. Allocate a user stack for the task (e.g., 16 KB)
    uint32_t stack_pages = 4;
    void *stack_phys = pmm_alloc_continuous(stack_pages);
    uint64_t user_stack_top = 0x7FFFFFFF0000ULL; // Standard high user stack address
    uint64_t user_stack_bottom = user_stack_top - (stack_pages * PAGE_SIZE);

    for (uint32_t j = 0; j < stack_pages; j++) {
        vmm_map(new_pml4, user_stack_bottom + (j * PAGE_SIZE), 
                (uint64_t)stack_phys + (j * PAGE_SIZE), 
                PTE_PRESENT | PTE_WRITABLE);
    }

    // 4. Create process and task structures
    process_t *proc = (process_t *)kmalloc(sizeof(process_t));
    memset(proc, 0, sizeof(process_t));
    proc->pid = 2; // PID 1 is usually reserved for init
    proc->cr3 = PHYS(new_pml4);
    proc->brk = 0x600000; // Initial program break

    task_t *task = (task_t *)kmalloc(sizeof(task_t));
    memset(task, 0, sizeof(task_t));

    task_init_fpu(task);

    uint16_t *fpu_cw = (uint16_t *)task_fpu_area(task);
    fpu_cw[0] = 0x037F; // FPU Control Word default
    fpu_cw[2] = 0xFFFF; // FPU Tag Word
    uint32_t *fpu_mxcsr = (uint32_t *)((uint8_t *)task_fpu_area(task) + 24);
    *fpu_mxcsr = 0x1F80; // MXCSR default
    
    task->id = proc->pid;
    task->state = TASK_STATE_RUNNABLE;
    task->running = true;
    task->process = proc;
    task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    // Setup initial stack frame for task execution
    uint64_t *stack = (uint64_t *)task->kstack_at_bottom;

    // Stack frame for IRETQ (User mode or Kernel mode execution)
    // For now, running at kernel privilege level (CS = 0x08) using user cr3:
    *--stack = 0x10;                  // SS
    *--stack = user_stack_top;        // RSP
    *--stack = 0x202;                 // RFLAGS (Interrupts enabled)
    *--stack = 0x08;                  // CS (Kernel code segment for now until Ring 3 SDK step)
    *--stack = ehdr->e_entry;         // RIP (ELF entry point)

    // Dummy error code and interrupt number
    *--stack = 0;
    *--stack = 0;

    // SAVE_REGS general purpose registers (15 registers)
    for (int i = 0; i < 15; i++) {
        *--stack = 0;
    }

    task->rsp = (uint64_t)stack;

    // Enqueue task to scheduler
    sched_enqueue(task);
    printf("ELF Loader: Task spawned successfully for entry point %x!\n", ehdr->e_entry);

    return true;
}