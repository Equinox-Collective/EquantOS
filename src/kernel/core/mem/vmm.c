#include "vmm.h"
#include "string.h"
#include "pmm.h"
#include "../core/panic.h"
#include "stdio.h"
#include "cpu.h"

static page_table_t *kernel_pml4;
uint64_t kernel_cr3;

extern void term_print(const char *str);

static void vmm_panic(const char *msg) {
    PANIC(msg);
}

static page_table_t *get_next_level(page_table_t *table, uint64_t index,
                                    bool allocate) {
  if (table[index] & PTE_PRESENT) {
    table[index] |= (PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    return (page_table_t *)VIRT(table[index] & ~0xFFFULL);
  }

  if (!allocate)
    return NULL;

  void *next_level_phys = pmm_alloc();
  if (!next_level_phys)
    vmm_panic("VMM: Out of physical memory for page tables!");

  memset((void *)VIRT(next_level_phys), 0, PAGE_SIZE);
  table[index] =
      (uint64_t)next_level_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

  return (page_table_t *)VIRT(next_level_phys);
}

void vmm_map(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
  virt &= ~0xFFFULL;
  phys &= ~0xFFFULL;

  uint64_t pml4_idx = (virt >> 39) & 0x1FF;
  uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
  uint64_t pd_idx = (virt >> 21) & 0x1FF;
  uint64_t pt_idx = (virt >> 12) & 0x1FF;

  page_table_t *pdpt = get_next_level(pml4, pml4_idx, true);
  page_table_t *pd = get_next_level(pdpt, pdpt_idx, true);
  page_table_t *pt = get_next_level(pd, pd_idx, true);
  
  // Разрешаем перезапись флагов кэширования для существующих страниц видеопамяти
  pt[pt_idx] = phys | flags | PTE_PRESENT;

  __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void pat_init(void) {
  // Читаем текущий регистр IA32_PAT MSR (0x277)
  uint64_t pat = read_msr(0x277);
  
  // Очищаем запись PA3 (биты 24-31) и выставляем тип 0x01 (Write-Combining)
  pat &= ~(0xFFULL << 24);
  pat |= (0x01ULL << 24);
  
  write_msr(0x277, pat);
}

extern uintptr_t fb_base_addr;
extern uint32_t screen_width;
extern uint32_t screen_height;
extern uint32_t screen_pitch;
extern uint64_t hhdm_offset;

void vmm_remap_fb_wc(void) {
    // Stubbed out for now: VESA driver and global screen parameters 
    // will be integrated later when we port the graphics subsystem.
}

void *vmm_alloc_large_buffer(uint64_t size) {
  uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  void *phys = pmm_alloc_continuous(pages);
  if (!phys) return NULL;

  // Резервируем диапазон адресов под крупные буферы ядра
  static uint64_t large_vaddr = 0xFFFFE00000000000;
  uint64_t virt = large_vaddr;
  large_vaddr += (pages * PAGE_SIZE);

  for (uint32_t i = 0; i < pages; i++) {
    vmm_map(kernel_pml4, virt + (i * PAGE_SIZE), (uint64_t)phys + (i * PAGE_SIZE),
            PTE_PRESENT | PTE_WRITABLE);
  }

  memset((void *)virt, 0, size);
  return (void *)virt;
}

void vmm_init() {
  uint64_t cr3_val;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
  kernel_cr3 = cr3_val & ~0xFFFULL;
  kernel_pml4 = (page_table_t *)VIRT(kernel_cr3);

  // Инициализируем аппаратную поддержку PAT
  pat_init();

  // Применяем WC к видеопамяти ядра
  // vmm_remap_fb_wc();
}

page_table_t *vmm_create_address_space() {
  void *phys = pmm_alloc();
  if (!phys)
    return NULL;

  page_table_t *new_pml4 = (page_table_t *)VIRT(phys);
  memset(new_pml4, 0, PAGE_SIZE);

  for (int i = 256; i < 512; i++) {
    new_pml4[i] = kernel_pml4[i];
  }

  return new_pml4;
}

page_table_t *vmm_clone_address_space(uint64_t parent_cr3_phys) {
  page_table_t *child = vmm_create_address_space();
  if (!child)
    return NULL;

  page_table_t *parent = (page_table_t *)VIRT(parent_cr3_phys);

  /* Обходим только пользовательскую половину (PML4 0..255). Старшая
   * половина (ядро) уже скопирована по ссылке в vmm_create_address_space. */
  for (int i = 0; i < 256; i++) {
    if (!(parent[i] & PTE_PRESENT))
      continue;
    page_table_t *pdpt = (page_table_t *)VIRT(parent[i] & ~0xFFFULL);
    for (int j = 0; j < 512; j++) {
      if (!(pdpt[j] & PTE_PRESENT))
        continue;
      page_table_t *pd = (page_table_t *)VIRT(pdpt[j] & ~0xFFFULL);
      for (int k = 0; k < 512; k++) {
        if (!(pd[k] & PTE_PRESENT))
          continue;
        page_table_t *pt = (page_table_t *)VIRT(pd[k] & ~0xFFFULL);
        for (int l = 0; l < 512; l++) {
          if (!(pt[l] & PTE_PRESENT))
            continue;

          uint64_t flags = pt[l] & 0xFFFULL;          /* сохраняем атрибуты PTE */
          uint64_t src_phys = pt[l] & ~0xFFFULL;
          uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                          ((uint64_t)k << 21) | ((uint64_t)l << 12);

          void *dst_phys = pmm_alloc();
          if (!dst_phys) {
            /* OOM посреди клонирования — откатываем уже выделенное. */
            vmm_destroy_address_space(PHYS(child));
            return NULL;
          }
          memcpy((void *)VIRT((uint64_t)dst_phys), (void *)VIRT(src_phys),
                 PAGE_SIZE);
          vmm_map(child, virt, (uint64_t)dst_phys, flags);
        }
      }
    }
  }
  return child;
}

uint64_t vmm_get_phys(page_table_t *pml4, uint64_t virt) {
  uint64_t pml4_idx = (virt >> 39) & 0x1FF;
  uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
  uint64_t pd_idx = (virt >> 21) & 0x1FF;
  uint64_t pt_idx = (virt >> 12) & 0x1FF;

  if (!(pml4[pml4_idx] & PTE_PRESENT))
    return 0;
  page_table_t *pdpt = (page_table_t *)VIRT(pml4[pml4_idx] & ~0xFFFULL);

  if (!(pdpt[pdpt_idx] & PTE_PRESENT))
    return 0;

  // Проверяем бит 7 (PS - Page Size) на уровне PDPT (Огромная страница 1 ГБ)
  if (pdpt[pdpt_idx] & (1ULL << 7)) {
    return (pdpt[pdpt_idx] & ~0x3FFFFFFFULL) + (virt & 0x3FFFFFFFULL);
  }

  page_table_t *pd = (page_table_t *)VIRT(pdpt[pdpt_idx] & ~0xFFFULL);

  if (!(pd[pd_idx] & PTE_PRESENT))
    return 0;

  // Проверяем бит 7 (PS - Page Size) на уровне PD (Огромная страница 2 МБ)
  if (pd[pd_idx] & (1ULL << 7)) {
    return (pd[pd_idx] & ~0x1FFFFFULL) + (virt & 0x1FFFFFULL);
  }

  page_table_t *pt = (page_table_t *)VIRT(pd[pd_idx] & ~0xFFFULL);

  if (!(pt[pt_idx] & PTE_PRESENT))
    return 0;

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

