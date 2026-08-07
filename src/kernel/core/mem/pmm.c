#include "pmm.h"
#include "../../../limine.h"
#include "string.h"
#include "../panic.h"

#include <stdint.h>

// --- МАКРОСЫ ДЛЯ РАБОТЫ С БИТАМИ ---
// 1 = страница занята, 0 = страница свободна
extern uint64_t hhdm_offset;
#define BITMAP_SET(bit) (bitmap[(bit) / 8] |= (1 << ((bit) % 8)))
#define BITMAP_CLEAR(bit) (bitmap[(bit) / 8] &= ~(1 << ((bit) % 8)))
#define BITMAP_TEST(bit) (bitmap[(bit) / 8] & (1 << ((bit) % 8)))
#define VIRT(addr) ((uint64_t)(addr) + hhdm_offset)
#define PAGE_SIZE 4096

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
uint8_t *bitmap;
uint64_t total_pages = 0;
uint64_t free_memory = 0;

// Скрываем last_page от других файлов с помощью static
static uint64_t last_page = 0;
uint64_t pmm_used_pages = 0;

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = { 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62 },
    .revision = 0,
    .response = NULL
};

// =========================================================================
//                        ИНИЦИАЛИЗАЦИЯ PMM
// =========================================================================

void pmm_init() {
  // Safety check: ensure Limine actually populated the memory map response
  if (memmap_request.response == NULL) {
      PANIC("PMM: Limine memory map request response is NULL!");
  }

  struct limine_memmap_response *map = memmap_request.response;
  uint64_t max_addr = 0;

  printf("PMM: Scanning memmap entries (count: %u)...\n", (unsigned int)map->entry_count);

  // 1. Find the highest physical address
  for (uint64_t i = 0; i < map->entry_count; i++) {
    if (map->entries[i]->type == LIMINE_MEMMAP_USABLE) {
      uint64_t end = map->entries[i]->base + map->entries[i]->length;
      printf("PMM: Usable region -> Base: %x, Length: %x, End: %x\n", 
             map->entries[i]->base, map->entries[i]->length, end);
      if (end > max_addr)
        max_addr = end;
    }
  }

  printf("PMM: Max physical address found: %x\n", max_addr);

  if (max_addr == 0) {
      PANIC("PMM: Max physical address is 0! No usable memory found.");
  }

  total_pages = max_addr / PAGE_SIZE;
  uint64_t bitmap_size = (total_pages + 7) / 8;
  uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
  
  // Safe calculation step-by-step
  uint64_t bitmap_size_aligned = bitmap_pages * PAGE_SIZE;

  printf("PMM: Total pages: %u, Bitmap size aligned: %u bytes\n", 
         (unsigned int)total_pages, (unsigned int)bitmap_size_aligned);

  // 2. Find a place to put the bitmap
  bitmap = NULL;
  for (uint64_t i = 0; i < map->entry_count; i++) {
    if (map->entries[i]->type == LIMINE_MEMMAP_USABLE &&
        map->entries[i]->length >= bitmap_size_aligned) {
      bitmap = (uint8_t *)VIRT(map->entries[i]->base);
      memset(bitmap, 0xFF, bitmap_size_aligned); // Mark everything as used initially

      map->entries[i]->base += bitmap_size_aligned;
      map->entries[i]->length -= bitmap_size_aligned;
      printf("PMM: Bitmap placed at virtual address: %x\n", (uint64_t)bitmap);
      break;
    }
  }

  // CRITICAL SAFETY CHECK: If bitmap is still NULL, we can't track physical memory!
  if (bitmap == NULL) {
      PANIC("PMM: Failed to find a continuous memory block for the allocation bitmap!");
  }

  // 3. Scan Limine memory map and mark usable pages as 0 (free)
  for (uint64_t i = 0; i < map->entry_count; i++) {
    if (map->entries[i]->type == LIMINE_MEMMAP_USABLE) {
      for (uint64_t addr = map->entries[i]->base;
           addr < map->entries[i]->base + map->entries[i]->length;
           addr += PAGE_SIZE) {
        uint64_t page = addr / PAGE_SIZE;
        if (page == 0)
          continue; // NEVER free page 0
        BITMAP_CLEAR(page);
        free_memory += PAGE_SIZE;
      }
    }
  }

  // Mark first megabyte as permanently used
  for (uint32_t i = 0; i < 256; i++) {
    if (i < total_pages) {
        BITMAP_SET(i);
    }
  }

  // Safe manual iteration without aggressive compiler loop-unrolling or 128-bit math
  pmm_used_pages = 0;
  uint64_t limit = total_pages;
  for (volatile uint64_t i = 0; i < limit; i++) {
    if (BITMAP_TEST(i)) {
      pmm_used_pages++;
    }
  }
  
  printf("PMM: Initialization complete. Free memory: %u MB\n", (unsigned int)(free_memory / (1024 * 1024)));
}

// =========================================================================
//                        ВЫДЕЛЕНИЕ ПАМЯТИ
// =========================================================================

void *pmm_alloc() {
  for (uint64_t i = last_page; i < total_pages; i++) {
    if (!BITMAP_TEST(i)) {
      BITMAP_SET(i);
      last_page = i;
      pmm_used_pages++;
      free_memory -= PAGE_SIZE;
      return (void *)(i * PAGE_SIZE);
    }
  }

  // Wrap-around: ищем с самого начала
  for (uint64_t i = 0; i < last_page; i++) {
    if (!BITMAP_TEST(i)) {
      BITMAP_SET(i);
      last_page = i;
      pmm_used_pages++;
      free_memory -= PAGE_SIZE;
      return (void *)(i * PAGE_SIZE);
    }
  }

  return NULL; // Реально Out of Memory
}

void* pmm_alloc_continuous(uint64_t count) {
    if (count == 0) return NULL;
    
    uint64_t found_pages = 0;
    uint64_t start_page = 0;

    for (uint64_t i = 1; i < total_pages; i++) { // Начинаем с 1, чтобы не отдать 0
        if (!BITMAP_TEST(i)) {
            if (found_pages == 0) start_page = i;
            found_pages++;

            if (found_pages == count) {
                for (uint64_t j = start_page; j < start_page + count; j++) {
                    BITMAP_SET(j);
                }
                free_memory -= (count * PAGE_SIZE);
                pmm_used_pages += count; // Обновляем счетчик!
                return (void*)(start_page * PAGE_SIZE);
            }
        } else {
            found_pages = 0;
        }
    }
    return NULL; 
}

// Добавим функцию освобождения памяти (пригодится на будущее)
void pmm_free(void *ptr) {
  uint64_t addr = (uintptr_t)ptr;
  uint64_t page = addr / PAGE_SIZE;

  // Защита от мусорных адресов: страницы, не покрытые PMM-битмапом
  // (например, MMIO-регионы вроде VESA LFB, замапленные через SYS_MAP_PHYS),
  // не должны редактировать битмап и счётчики. Раньше это вызывало
  // запись за пределы массива `bitmap` при выходе любого приложения,
  // которое мапило фреймбуфер.
  if (page >= total_pages)
    return;

  if (BITMAP_TEST(page)) {
    BITMAP_CLEAR(page);
    if (pmm_used_pages > 0)
      pmm_used_pages--;
    free_memory += PAGE_SIZE;
    // Сдвигаем last_page влево, чтобы следующий pmm_alloc мог сразу
    // переиспользовать только что освобождённую страницу.
    if (page < last_page)
      last_page = page;
  }
}

uint64_t pmm_get_used_memory() { return pmm_used_pages * 4096; }
uint64_t pmm_get_total_memory() { return total_pages * 4096; }