// src/kernel/core/mem/vmm.h
#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>
#include "../panic.h"

extern uint64_t hhdm_offset;

#define VIRT(addr) ((uint64_t)(addr) + (uint64_t)hhdm_offset)
#define PHYS(addr) ((uint64_t)(addr) - (uint64_t)hhdm_offset)

#define PAGE_SIZE     4096ULL
#define PAGE_SIZE_2MB 0x200000ULL

// Page Table Entry (PTE) Flags
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_HUGE      (1ULL << 7) // Page Size flag for PDE/PDPE (2MB/1GB)
#define PTE_COW       (1ULL << 9)
#define PTE_PAT_4KB   (1ULL << 7)  // PAT bit for 4KB PTE
#define PTE_PAT_2MB   (1ULL << 12) // PAT bit for 2MB PDE

// Write-Combining combination using PAT index 3 (PA3 configured as WC in pat_init)
#define PTE_WC        (PTE_PCD | PTE_PWT)

typedef uint64_t page_table_t;

void vmm_init(void);
void pat_init(void);

void vmm_map(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_2mb(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(page_table_t *pml4, uint64_t virt);
page_table_t *vmm_create_address_space(void);
page_table_t *vmm_clone_address_space(uint64_t parent_cr3_phys);
uint64_t vmm_get_phys(page_table_t *pml4, uint64_t virt);
void vmm_destroy_address_space(uint64_t cr3_phys);
void vmm_page_fault_handler(cpu_state_t *state);

#endif // VMM_H