// src/kernel/core/mem/vmm.c - Advanced VMM with Copy-On-Write and Guard Page Support
#include "vmm.h"
#include "string.h"
#include "pmm.h"
#include "../panic.h"
#include "stdio.h"
#include "../gen/cpu.h"
#include "../../drivers/serial/serial.h"

static page_table_t *kernel_pml4;
uint64_t kernel_cr3;

static void invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static page_table_t *get_next_level(page_table_t *table, uint64_t index, bool allocate) {
    if (table[index] & PTE_PRESENT) {
        table[index] |= (PTE_PRESENT | PTE_USER | PTE_WRITABLE);
        return (page_table_t *)VIRT(table[index] & ~0xFFFULL);
    }

    if (!allocate) return NULL;

    void *next_level_phys = pmm_alloc();
    if (!next_level_phys) {
        PANIC("VMM: Out of physical memory for page tables!");
    }

    memset((void *)VIRT(next_level_phys), 0, PAGE_SIZE);
    table[index] = (uint64_t)next_level_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    return (page_table_t *)VIRT(next_level_phys);
}

void vmm_map(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    virt &= ~0xFFFULL;
    phys &= ~0xFFFULL;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    page_table_t *pdpt = get_next_level(pml4, pml4_idx, true);
    page_table_t *pd   = get_next_level(pdpt, pdpt_idx, true);
    page_table_t *pt   = get_next_level(pd, pd_idx, true);

    pt[pt_idx] = phys | flags | PTE_PRESENT;
    invlpg(virt);
}

void vmm_unmap(page_table_t *pml4, uint64_t virt) {
    virt &= ~0xFFFULL;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return;
    page_table_t *pdpt = (page_table_t *)VIRT(pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return;
    page_table_t *pd = (page_table_t *)VIRT(pdpt[pdpt_idx] & ~0xFFFULL);

    if (!(pd[pd_idx] & PTE_PRESENT)) return;
    page_table_t *pt = (page_table_t *)VIRT(pd[pd_idx] & ~0xFFFULL);

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
    kernel_cr3 = cr3_val & ~0xFFFULL;
    kernel_pml4 = (page_table_t *)VIRT(kernel_cr3);

    pat_init();
    serial_puts(COM1, "[VMM] Page Table Subsystem & PAT initialized.\n");
}

page_table_t *vmm_create_address_space(void) {
    void *phys = pmm_alloc();
    if (!phys) return NULL;

    page_table_t *new_pml4 = (page_table_t *)VIRT(phys);
    memset(new_pml4, 0, PAGE_SIZE);

    // Copy Higher-Half Kernel mappings (PML4 entries 256..511)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }

    return new_pml4;
}

/* Deep Process Memory Cloning (Ensures child & parent have independent physical pages) */
page_table_t *vmm_clone_address_space(uint64_t parent_cr3_phys) {
    page_table_t *child = vmm_create_address_space();
    if (!child) return NULL;

    page_table_t *parent = (page_table_t *)VIRT(parent_cr3_phys);

    // Iterate through User-Space lower half (PML4 0..255)
    for (int i = 0; i < 256; i++) {
        if (!(parent[i] & PTE_PRESENT)) continue;
        page_table_t *pdpt = (page_table_t *)VIRT(parent[i] & ~0xFFFULL);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            page_table_t *pd = (page_table_t *)VIRT(pdpt[j] & ~0xFFFULL);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                page_table_t *pt = (page_table_t *)VIRT(pd[k] & ~0xFFFULL);

                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;

                    uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                    ((uint64_t)k << 21) | ((uint64_t)l << 12);

                    uint64_t parent_phys = pt[l] & ~0xFFFULL;
                    uint64_t flags       = pt[l] & 0xFFFULL;

                    // Allocate a separate, independent physical page for the child
                    void *child_phys = pmm_alloc();
                    if (!child_phys) {
                        vmm_destroy_address_space(PHYS(child));
                        return NULL;
                    }

                    // Copy exact 4KB memory contents from parent to child
                    memcpy((void *)VIRT((uint64_t)child_phys), (void *)VIRT(parent_phys), PAGE_SIZE);

                    // Map child to its own private physical page
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
    page_table_t *pml4 = (page_table_t *)VIRT(cr3_val & ~0xFFFULL);

    uint64_t pml4_idx = (fault_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (fault_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (fault_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (fault_addr >> 12) & 0x1FF;

    if (pml4[pml4_idx] & PTE_PRESENT) {
        page_table_t *pdpt = (page_table_t *)VIRT(pml4[pml4_idx] & ~0xFFFULL);
        if (pdpt[pdpt_idx] & PTE_PRESENT) {
            page_table_t *pd = (page_table_t *)VIRT(pdpt[pdpt_idx] & ~0xFFFULL);
            if (pd[pd_idx] & PTE_PRESENT) {
                page_table_t *pt = (page_table_t *)VIRT(pd[pd_idx] & ~0xFFFULL);
                
                uint64_t pte = pt[pt_idx];

                // Check Error Code: Caused by Write Access (Bit 1) on Present Page (Bit 0)
                bool is_write_fault = (state->error_code & 0x02) != 0;

                // Handle Copy-On-Write Fault
                if (is_write_fault && (pte & PTE_PRESENT) && (pte & PTE_COW)) {
                    uint64_t old_phys = pte & ~0xFFFULL;

                    void *new_phys = pmm_alloc(); // Allocate fresh page from Buddy Allocator
                    if (!new_phys) {
                        kernel_panic(state, __FILE__, __LINE__, "OOM during Copy-On-Write resolution!");
                    }

                    // Copy 4KB contents from shared page to new page
                    memcpy((void *)VIRT((uint64_t)new_phys), (void *)VIRT(old_phys), PAGE_SIZE);

                    // Update PTE: Remove COW flag, restore WRITABLE flag
                    uint64_t new_flags = (pte & 0xFFFULL) & ~PTE_COW;
                    new_flags |= PTE_WRITABLE;

                    pt[pt_idx] = (uint64_t)new_phys | new_flags;
                    invlpg(fault_addr);

                    return; // Resume instruction execution smoothly!
                }
            }
        }
    }

    // If fault is not resolved by COW, trigger Kernel Panic or Segfault
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
    page_table_t *pdpt = (page_table_t *)VIRT(pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;
    page_table_t *pd = (page_table_t *)VIRT(pdpt[pdpt_idx] & ~0xFFFULL);

    if (!(pd[pd_idx] & PTE_PRESENT)) return 0;
    page_table_t *pt = (page_table_t *)VIRT(pd[pd_idx] & ~0xFFFULL);

    if (!(pt[pt_idx] & PTE_PRESENT)) return 0;

    return (pt[pt_idx] & ~0xFFFULL) + (virt & 0xFFF);
}

void vmm_destroy_address_space(uint64_t cr3_phys) {
    page_table_t *pml4 = (page_table_t *)VIRT(cr3_phys);

    for (int i = 0; i < 256; i++) {
        if (pml4[i] & PTE_PRESENT) {
            page_table_t *pdpt = (page_table_t *)VIRT(pml4[i] & ~0xFFFULL);
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & PTE_PRESENT) {
                    page_table_t *pd = (page_table_t *)VIRT(pdpt[j] & ~0xFFFULL);
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & PTE_PRESENT) {
                            page_table_t *pt = (page_table_t *)VIRT(pd[k] & ~0xFFFULL);
                            for (int l = 0; l < 512; l++) {
                                if (pt[l] & PTE_PRESENT) {
                                    pmm_free((void *)(pt[l] & ~0xFFFULL));
                                }
                            }
                            pmm_free((void *)(pd[k] & ~0xFFFULL));
                        }
                    }
                    pmm_free((void *)(pdpt[j] & ~0xFFFULL));
                }
            }
            pmm_free((void *)(pml4[i] & ~0xFFFULL));
        }
    }
}