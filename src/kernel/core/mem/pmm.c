// src/kernel/core/mem/pmm.c - Production Buddy Allocator (Linux/BSD style)
#include "pmm.h"
#include "vmm.h"
#include "../panic.h"
#include "../../../limine.h"
#include "string.h"
#include "stdio.h"

extern uint64_t hhdm_offset;

static pmm_free_area_t free_areas[PMM_MAX_ORDER];
static pmm_page_t *page_array = NULL;
static uint64_t total_phys_pages = 0;
static uint64_t free_phys_pages = 0;

uint64_t total_pages = 0;
uint64_t free_memory = 0;


__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = { 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62 },
    .revision = 0,
    .response = NULL
};

static inline uint64_t page_to_pfn(pmm_page_t *page) {
    return (uint64_t)(page - page_array);
}

static inline pmm_page_t *pfn_to_page(uint64_t pfn) {
    return &page_array[pfn];
}

static inline void list_add(pmm_page_t **head, pmm_page_t *page) {
    page->next = *head;
    page->prev = NULL;
    if (*head) {
        (*head)->prev = page;
    }
    *head = page;
}

static inline void list_remove(pmm_page_t **head, pmm_page_t *page) {
    if (page->prev) {
        page->prev->next = page->next;
    } else {
        *head = page->next;
    }
    if (page->next) {
        page->next->prev = page->prev;
    }
    page->next = NULL;
    page->prev = NULL;
}

void pmm_init(void) {
    if (memmap_request.response == NULL) {
        PANIC("PMM: Limine memory map request response is NULL!");
    }

    struct limine_memmap_response *map = memmap_request.response;
    uint64_t max_addr = 0;

    for (uint64_t i = 0; i < map->entry_count; i++) {
        if (map->entries[i]->type == LIMINE_MEMMAP_USABLE) {
            uint64_t end = map->entries[i]->base + map->entries[i]->length;
            if (end > max_addr) max_addr = end;
        }
    }

    total_phys_pages = max_addr / PAGE_SIZE;
    size_t page_array_size = total_phys_pages * sizeof(pmm_page_t);
    size_t page_array_pages = (page_array_size + PAGE_SIZE - 1) / PAGE_SIZE;

    // Reserve physical region for the Page Descriptor Metadata
    uint64_t page_array_phys = 0;
    for (uint64_t i = 0; i < map->entry_count; i++) {
        if (map->entries[i]->type == LIMINE_MEMMAP_USABLE &&
            map->entries[i]->length >= page_array_pages * PAGE_SIZE) {
            page_array_phys = map->entries[i]->base;
            map->entries[i]->base += page_array_pages * PAGE_SIZE;
            map->entries[i]->length -= page_array_pages * PAGE_SIZE;
            break;
        }
    }

    if (!page_array_phys) {
        PANIC("PMM: Failed to allocate Buddy Allocator page array!");
    }

    page_array = (pmm_page_t *)VIRT(page_array_phys);
    memset(page_array, 0, page_array_pages * PAGE_SIZE);

    for (size_t i = 0; i < PMM_MAX_ORDER; i++) {
        free_areas[i].freelist = NULL;
        free_areas[i].free_count = 0;
    }

    // Populate Buddy Free Lists with Usable Physical Regions
    for (uint64_t i = 0; i < map->entry_count; i++) {
        if (map->entries[i]->type == LIMINE_MEMMAP_USABLE) {
            uint64_t base_pfn = map->entries[i]->base / PAGE_SIZE;
            uint64_t count = map->entries[i]->length / PAGE_SIZE;

            for (uint64_t p = 0; p < count; p++) {
                uint64_t pfn = base_pfn + p;
                if (pfn == 0) continue; // Safety guard for Zero Page
                pmm_free_pages((void *)(pfn * PAGE_SIZE), 0);
            }
        }
    }

    printf("PMM: Buddy Allocator initialized. Free RAM: %u MB\n", 
           (unsigned int)(free_phys_pages * PAGE_SIZE / (1024 * 1024)));
}

void *pmm_alloc_pages(size_t order) {
    if (order >= PMM_MAX_ORDER) return NULL;

    for (size_t current_order = order; current_order < PMM_MAX_ORDER; current_order++) {
        if (free_areas[current_order].freelist != NULL) {
            pmm_page_t *page = free_areas[current_order].freelist;
            list_remove(&free_areas[current_order].freelist, page);
            free_areas[current_order].free_count--;

            // Split larger blocks down to requested order
            while (current_order > order) {
                current_order--;
                uint64_t buddy_pfn = page_to_pfn(page) ^ (1ULL << current_order);
                pmm_page_t *buddy = pfn_to_page(buddy_pfn);

                buddy->order = current_order;
                buddy->is_free = true;
                list_add(&free_areas[current_order].freelist, buddy);
                free_areas[current_order].free_count++;
            }

            page->order = order;
            page->is_free = false;
            free_phys_pages -= (1ULL << order);
            return (void *)(page_to_pfn(page) * PAGE_SIZE);
        }
    }

    return NULL; // Out of Physical Memory
}

void pmm_free_pages(void *ptr, size_t order) {
    if (!ptr || order >= PMM_MAX_ORDER) return;

    uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;
    if (pfn >= total_phys_pages) return;

    pmm_page_t *page = pfn_to_page(pfn);
    page->order = order;

    // Coalesce (Merge) free buddies upwards
    while (order < PMM_MAX_ORDER - 1) {
        uint64_t buddy_pfn = pfn ^ (1ULL << order);
        if (buddy_pfn >= total_phys_pages) break;

        pmm_page_t *buddy = pfn_to_page(buddy_pfn);
        if (!buddy->is_free || buddy->order != order) {
            break; // Buddy cannot be merged
        }

        // Remove buddy from its current free list and merge
        list_remove(&free_areas[order].freelist, buddy);
        free_areas[order].free_count--;
        buddy->is_free = false;

        pfn &= ~(1ULL << order); // Combined block start
        page = pfn_to_page(pfn);
        order++;
    }

    page->order = order;
    page->is_free = true;
    list_add(&free_areas[order].freelist, page);
    free_areas[order].free_count++;
    free_phys_pages += (1ULL << order);
}

void *pmm_alloc(void) {
    return pmm_alloc_pages(0);
}

void pmm_free(void *ptr) {
    pmm_free_pages(ptr, 0);
}

void *pmm_alloc_continuous(uint64_t count) {
    size_t order = 0;
    while ((1ULL << order) < count) {
        order++;
    }
    return pmm_alloc_pages(order);
}

uint64_t pmm_get_used_memory(void) {
    return (total_phys_pages - free_phys_pages) * PAGE_SIZE;
}

uint64_t pmm_get_total_memory(void) {
    return total_phys_pages * PAGE_SIZE;
}