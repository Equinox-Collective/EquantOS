// src/kernel/core/mem/vmm.h - Virtual Memory Manager with Mach/BSD COW
#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>
#include "../panic.h"

extern uint64_t hhdm_offset;

#define VIRT(addr) ((uint64_t)(addr) + (uint64_t)hhdm_offset)
#define PHYS(addr) ((uint64_t)(addr) - (uint64_t)hhdm_offset)

#define PAGE_SIZE 4096ULL

// Page Table Entry (PTE) Flags
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PWT      (1ULL << 3)
#define PTE_PCD      (1ULL << 4)
#define PTE_COW      (1ULL << 9)  // Custom OS Bit for Copy-On-Write (Mach/BSD style)

typedef uint64_t page_table_t;

void vmm_init(void);
void pat_init(void);

void vmm_map(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(page_table_t *pml4, uint64_t virt);
page_table_t *vmm_create_address_space(void);

/* Mach/FreeBSD Copy-On-Write process address space cloning */
page_table_t *vmm_clone_address_space(uint64_t parent_cr3_phys);
uint64_t vmm_get_phys(page_table_t *pml4, uint64_t virt);
void vmm_destroy_address_space(uint64_t cr3_phys);

/* Page Fault Interrupt Handler (#PF Vector 14) */
void vmm_page_fault_handler(cpu_state_t *state);

#endif // VMM_H