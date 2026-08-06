#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>

extern uint64_t hhdm_offset; // Оставляем просто uint64_t

// Форсируем 64-битную арифметику при сложении
#define VIRT(addr) ((uint64_t)(addr) + (uint64_t)hhdm_offset)
#define PHYS(addr) ((uint64_t)(addr) - (uint64_t)hhdm_offset)

#define PAGE_SIZE 4096

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PWT      (1ULL << 3)  // Page-level Write-Through
#define PTE_PCD      (1ULL << 4)

typedef uint64_t page_table_t;

void vmm_init(void);
void pat_init(void);
void vmm_remap_fb_wc(void);
void *vmm_alloc_large_buffer(uint64_t size);

void vmm_map(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
page_table_t *vmm_create_address_space(void);

/* Этап 1: глубокая копия пользовательской половины (PML4[0..255]) адресного
 * пространства parent_cr3_phys. Старшая половина (ядро/HHDM, индексы 256..511)
 * разделяется как и в vmm_create_address_space. Каждая present USER-страница
 * physически копируется (eager copy, без COW). Возвращает VIRT-указатель на
 * новый PML4 или NULL при нехватке памяти. */
page_table_t *vmm_clone_address_space(uint64_t parent_cr3_phys);
uint64_t vmm_get_phys(page_table_t *pml4, uint64_t virt);
void vmm_destroy_address_space(uint64_t cr3_phys);

#endif