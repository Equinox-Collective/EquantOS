// src/kernel/core/mem/memory.c - Solaris Bonwick Slab / Buddy Hybrid Allocator
#include "memory.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "stdio.h"

size_t used_memory = 0;

// Slab Bucket Sizes: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes
#define NUM_SLAB_BUCKETS 8
static const size_t bucket_sizes[NUM_SLAB_BUCKETS] = {16, 32, 64, 128, 256, 512, 1024, 2048};

typedef struct slab_object {
    struct slab_object *next;
} slab_object_t;

typedef struct slab_page {
    size_t object_size;
    size_t free_count;
    slab_object_t *free_list;
    struct slab_page *next;
} slab_page_t;

typedef struct {
    size_t object_size;
    slab_page_t *slabs;
} kmem_bucket_t;

static kmem_bucket_t buckets[NUM_SLAB_BUCKETS];

// Large Allocation Header for allocations > 2048 bytes
typedef struct large_alloc_header {
    uint32_t magic;
    size_t page_count;
    size_t size;
} large_alloc_header_t;

#define LARGE_ALLOC_MAGIC 0x4C415247 // "LARG"

void init_heap(uint64_t start_addr, size_t size) {
    (void)start_addr;
    (void)size;

    for (int i = 0; i < NUM_SLAB_BUCKETS; i++) {
        buckets[i].object_size = bucket_sizes[i];
        buckets[i].slabs = NULL;
    }
    used_memory = 0;
    printf("[MEMORY] Solaris-style Slab & Buddy Kernel Heap Initialized.\n");
}

static slab_page_t *create_slab_page(size_t object_size) {
    void *phys = pmm_alloc(); // Allocate 1 physical page (4KB) from Buddy Allocator
    if (!phys) return NULL;

    slab_page_t *slab = (slab_page_t *)VIRT(phys);
    slab->object_size = object_size;
    slab->next = NULL;

    uint8_t *payload_start = (uint8_t *)slab + sizeof(slab_page_t);
    // Align payload to 16 bytes boundary
    payload_start = (uint8_t *)(((uintptr_t)payload_start + 15) & ~15ULL);

    size_t available_space = PAGE_SIZE - (size_t)(payload_start - (uint8_t *)slab);
    size_t capacity = available_space / object_size;

    slab->free_count = capacity;
    slab->free_list = (slab_object_t *)payload_start;

    slab_object_t *curr = slab->free_list;
    for (size_t i = 0; i < capacity - 1; i++) {
        uint8_t *next_obj = (uint8_t *)curr + object_size;
        curr->next = (slab_object_t *)next_obj;
        curr = curr->next;
    }
    curr->next = NULL;

    return slab;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    // 1. Small Allocations (<= 2048 bytes): Handle via Slab Object Cache (O(1))
    if (size <= 2048) {
        int bucket_idx = -1;
        for (int i = 0; i < NUM_SLAB_BUCKETS; i++) {
            if (bucket_sizes[i] >= size) {
                bucket_idx = i;
                break;
            }
        }

        if (bucket_idx != -1) {
            kmem_bucket_t *bucket = &buckets[bucket_idx];
            slab_page_t *slab = bucket->slabs;

            // Find a slab page with free objects
            while (slab && slab->free_count == 0) {
                slab = slab->next;
            }

            if (!slab) {
                slab = create_slab_page(bucket->object_size);
                if (!slab) return NULL;
                slab->next = bucket->slabs;
                bucket->slabs = slab;
            }

            // Pop object from slab free list (O(1) Constant Time)
            slab_object_t *obj = slab->free_list;
            slab->free_list = obj->next;
            slab->free_count--;

            used_memory += bucket->object_size;
            return (void *)obj;
        }
    }

    // 2. Large Allocations (> 2048 bytes): Delegate directly to Buddy Allocator
    size_t total_needed = size + sizeof(large_alloc_header_t);
    size_t pages_needed = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE;

    void *phys = pmm_alloc_continuous(pages_needed);
    if (!phys) return NULL;

    large_alloc_header_t *header = (large_alloc_header_t *)VIRT(phys);
    header->magic = LARGE_ALLOC_MAGIC;
    header->page_count = pages_needed;
    header->size = size;

    used_memory += (pages_needed * PAGE_SIZE);
    return (void *)((uint8_t *)header + sizeof(large_alloc_header_t));
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;

    uint64_t addr = (uint64_t)ptr;
    uint64_t page_base = addr & ~0xFFFULL;

    large_alloc_header_t *header = (large_alloc_header_t *)page_base;
    if ((uint8_t *)ptr == (uint8_t *)header + sizeof(large_alloc_header_t) &&
        header->magic == LARGE_ALLOC_MAGIC) {
        
        size_t pages = header->page_count;
        used_memory -= (pages * PAGE_SIZE);

        size_t order = 0;
        while ((1ULL << order) < pages) {
            order++;
        }

        pmm_free_pages((void *)PHYS(page_base), order); // Освобождаем правильный Buddy-порядок!
        return;
    }

    // Обработка Slab...
    slab_page_t *slab = (slab_page_t *)page_base;
    slab_object_t *obj = (slab_object_t *)ptr;

    obj->next = slab->free_list;
    slab->free_list = obj;
    slab->free_count++;

    if (used_memory >= slab->object_size) {
        used_memory -= slab->object_size;
    }
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    // Determine copy size
    uint64_t page_base = (uint64_t)ptr & ~0xFFFULL;
    large_alloc_header_t *header = (large_alloc_header_t *)page_base;

    size_t old_size = 0;
    if ((uint8_t *)ptr == (uint8_t *)header + sizeof(large_alloc_header_t) &&
        header->magic == LARGE_ALLOC_MAGIC) {
        old_size = header->size;
    } else {
        slab_page_t *slab = (slab_page_t *)page_base;
        old_size = slab->object_size;
    }

    size_t copy_size = (new_size < old_size) ? new_size : old_size;
    memcpy(new_ptr, ptr, copy_size);
    kfree(ptr);

    return new_ptr;
}

void kheap_dump(void) {
    printf("--- KERNEL SLAB HEAP DUMP ---\n");
    for (int i = 0; i < NUM_SLAB_BUCKETS; i++) {
        size_t total_slabs = 0;
        slab_page_t *s = buckets[i].slabs;
        while (s) {
            total_slabs++;
            s = s->next;
        }
        printf("Bucket [%u bytes]: %u slab pages active\n", 
               (unsigned int)bucket_sizes[i], (unsigned int)total_slabs);
    }
    printf("Total Heap Allocated: %u KB\n", (unsigned int)(used_memory / 1024));
    printf("-----------------------------\n");
}