// src/kernel/core/mem/pmm.h
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_SIZE 4096ULL
#define PMM_MAX_ORDER 11 // Orders 0..10 (Up to 4MB continuous blocks)

typedef struct pmm_page {
    uint32_t flags;
    uint8_t order;
    bool is_free;
    struct pmm_page *next;
    struct pmm_page *prev;
} pmm_page_t;

typedef struct {
    pmm_page_t *freelist;
    size_t free_count;
} pmm_free_area_t;

void pmm_init(void);
void *pmm_alloc_pages(size_t order);
void *pmm_alloc(void); // Order 0 helper
void pmm_free_pages(void *ptr, size_t order);
void pmm_free(void *ptr); // Order 0 helper

uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_total_memory(void);

#endif // PMM_H