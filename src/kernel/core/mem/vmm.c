// src/kernel/core/mem/vmm.c - Advanced VMM with Canonical Paging and Safe Process Cloning
#include "vmm.h"
#include "string.h"
#include "pmm.h"
#include "../panic.h"
#include "stdio.h"
#include "../gen/cpu.h"
#include "../../drivers/serial/serial.h"

// x86_64 Hardware Physical Address Mask (Bits 12..51)
// Strictly strips flags (0..11), OS bits (52..62) and NX bit (63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_FLAGS_MASK (~PTE_ADDR_MASK)
#define PTE_PAGE_SIZE  (1ULL << 7) // Huge page flag (2MB / 1GB)

static page_table_t *kernel_pml4;
uint64_t kernel_cr3;

static inline void invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static page_table_t *get_next_level(page_table_t *table, uint64_t index, bool allocate) {
    if (table[index] & PTE_PRESENT) {
        table[index] |= (PTE_PRESENT | PTE_USER | PTE_WRITABLE);
        uint64_t phys = table[index] & PTE_ADDR_MASK;
        return (page_table_t *)VIRT(phys);
    }

    if (!allocate) return NULL;

    void *next_level_phys = pmm_alloc();
    if (!next_level_phys) {
        PANIC("VMM: Out of physical memory for page tables!");
    }

    memset((void *)VIRT((uint64_t)next_level_phys), 0, PAGE_SIZE);
    table[index] = ((uint64_t)next_level_phys & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    return (page_table_t *)VIRT((uint64_t)next_level_phys);
}

void vmm_map(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    virt &= ~0xFFFULL;
    phys &= PTE_ADDR_MASK;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    page_table_t *pdpt = get_next_level(pml4, pml4_idx, true);
    page_table_t *pd   = get_next_level(pdpt, pdpt_idx, true);
    page_table_t *pt   = get_next_level(pd, pd_idx, true);

    pt[pt_idx] = phys | (flags & PTE_FLAGS_MASK) | PTE_PRESENT;
    invlpg(virt);
}

void vmm_unmap(page_table_t *pml4, uint64_t virt) {
    virt &= ~0xFFFULL;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return;
    page_table_t *pdpt = (page_table_t *)VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return;
    page_table_t *pd = (page_table_t *)VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    if (!(pd[pd_idx] & PTE_PRESENT)) return;
    page_table_t *pt = (page_table_t *)VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    pt[pt_idx] = 0;
    invlpg(virt);
}

void pat_init(void) {
    uint64_t pat = read_msr(0x277);
    pat &= ~(0xFFULL << 24);
    pat |= (0x01ULL << 24); // Set Write-Combining (WC)
    write_msr(0x277, pat);
}

void vmm_init(void) {
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    kernel_cr3 = cr3_val & PTE_ADDR_MASK;
    kernel_pml4 = (page_table_t *)VIRT(kernel_cr3);

    pat_init();
    serial_puts(COM1, "[VMM] Page Table Subsystem & PAT initialized.\n");
}

page_table_t *vmm_create_address_space(void) {
    void *phys = pmm_alloc();
    if (!phys) return NULL;

    page_table_t *new_pml4 = (page_table_t *)VIRT((uint64_t)phys);
    memset(new_pml4, 0, PAGE_SIZE);

    // Share Higher-Half Kernel mappings (PML4 entries 256..511)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }

    return new_pml4;
}

/* Deep Process Memory Cloning with Clean Address Sanitization */
page_table_t *vmm_clone_address_space(uint64_t parent_cr3_phys) {
    page_table_t *child = vmm_create_address_space();
    if (!child) return NULL;

    page_table_t *parent = (page_table_t *)VIRT(parent_cr3_phys & PTE_ADDR_MASK);

    // Clone user-space memory only (PML4 entries 0..255)
    for (int i = 0; i < 256; i++) {
        if (!(parent[i] & PTE_PRESENT)) continue;
        page_table_t *pdpt = (page_table_t *)VIRT(parent[i] & PTE_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            if (pdpt[j] & PTE_PAGE_SIZE) continue; // Skip huge pages

            page_table_t *pd = (page_table_t *)VIRT(pdpt[j] & PTE_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                if (pd[k] & PTE_PAGE_SIZE) continue; // Skip huge pages

                page_table_t *pt = (page_table_t *)VIRT(pd[k] & PTE_ADDR_MASK);

                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;

                    uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                    ((uint64_t)k << 21) | ((uint64_t)l << 12);

                    // Cleanly mask physical address and isolate flags
                    uint64_t parent_phys = pt[l] & PTE_ADDR_MASK;
                    uint64_t flags       = pt[l] & PTE_FLAGS_MASK;

                    void *child_phys = pmm_alloc();
                    if (!child_phys) {
                        vmm_destroy_address_space(PHYS(child));
                        return NULL;
                    }

                    // Safe copy using guaranteed canonical pointers
                    memcpy((void *)VIRT((uint64_t)child_phys), (void *)VIRT(parent_phys), PAGE_SIZE);

                    vmm_map(child, virt, (uint64_t)child_phys, flags);
                }
            }
        }
    }
    return child;
}

/* Page Fault Handler (#PF Interrupt Vector 14) */
void vmm_page_fault_handler(cpu_state_t *state) {
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    page_table_t *pml4 = (page_table_t *)VIRT(cr3_val & PTE_ADDR_MASK);

    uint64_t pml4_idx = (fault_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (fault_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (fault_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (fault_addr >> 12) & 0x1FF;

    if (pml4[pml4_idx] & PTE_PRESENT) {
        page_table_t *pdpt = (page_table_t *)VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (pdpt[pdpt_idx] & PTE_PRESENT) {
            page_table_t *pd = (page_table_t *)VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
            if (pd[pd_idx] & PTE_PRESENT) {
                page_table_t *pt = (page_table_t *)VIRT(pd[pd_idx] & PTE_ADDR_MASK);
                
                uint64_t pte = pt[pt_idx];
                bool is_write_fault = (state->error_code & 0x02) != 0;

                // Handle Copy-On-Write Fault
                if (is_write_fault && (pte & PTE_PRESENT) && (pte & PTE_COW)) {
                    uint64_t old_phys = pte & PTE_ADDR_MASK;

                    void *new_phys = pmm_alloc();
                    if (!new_phys) {
                        kernel_panic(state, __FILE__, __LINE__, "OOM during Copy-On-Write resolution!");
                    }

                    memcpy((void *)VIRT((uint64_t)new_phys), (void *)VIRT(old_phys), PAGE_SIZE);

                    uint64_t new_flags = (pte & PTE_FLAGS_MASK) & ~PTE_COW;
                    new_flags |= PTE_WRITABLE;

                    pt[pt_idx] = ((uint64_t)new_phys & PTE_ADDR_MASK) | new_flags;
                    invlpg(fault_addr);

                    return;
                }
            }
        }
    }

    serial_puts(COM1, "[VMM] Fatal Page Fault at virtual address: 0x");
    char buf[32];
    itoa_hex(fault_addr, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    kernel_panic(state, __FILE__, __LINE__, "Fatal Unhandled Page Fault (#PF)");
}

uint64_t vmm_get_phys(page_table_t *pml4, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return 0;
    page_table_t *pdpt = (page_table_t *)VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;
    page_table_t *pd = (page_table_t *)VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    if (!(pd[pd_idx] & PTE_PRESENT)) return 0;
    page_table_t *pt = (page_table_t *)VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    if (!(pt[pt_idx] & PTE_PRESENT)) return 0;

    return (pt[pt_idx] & PTE_ADDR_MASK) + (virt & 0xFFF);
}

void vmm_destroy_address_space(uint64_t cr3_phys) {
    page_table_t *pml4 = (page_table_t *)VIRT(cr3_phys & PTE_ADDR_MASK);

    for (int i = 0; i < 256; i++) {
        if (pml4[i] & PTE_PRESENT) {
            page_table_t *pdpt = (page_table_t *)VIRT(pml4[i] & PTE_ADDR_MASK);
            for (int j = 0; j < 512; j++) {
                if ((pdpt[j] & PTE_PRESENT) && !(pdpt[j] & PTE_PAGE_SIZE)) {
                    page_table_t *pd = (page_table_t *)VIRT(pdpt[j] & PTE_ADDR_MASK);
                    for (int k = 0; k < 512; k++) {
                        if ((pd[k] & PTE_PRESENT) && !(pd[k] & PTE_PAGE_SIZE)) {
                            page_table_t *pt = (page_table_t *)VIRT(pd[k] & PTE_ADDR_MASK);
                            for (int l = 0; l < 512; l++) {
                                if (pt[l] & PTE_PRESENT) {
                                    pmm_free((void *)(pt[l] & PTE_ADDR_MASK));
                                }
                            }
                            pmm_free((void *)(pd[k] & PTE_ADDR_MASK));
                        }
                    }
                    pmm_free((void *)(pdpt[j] & PTE_ADDR_MASK));
                }
            }
            pmm_free((void *)(pml4[i] & PTE_ADDR_MASK));
        }
    }
    // Clean up the PML4 root page itself
    pmm_free((void *)(cr3_phys & PTE_ADDR_MASK));
}