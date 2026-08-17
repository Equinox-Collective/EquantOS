// src/kernel/core/mem/memory.h - High-Performance Kernel Slab/Buddy Allocator
#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define HEAP_ALIGNMENT 16

extern size_t used_memory;

void init_heap(uint64_t start_addr, size_t size);

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t new_size);
void kfree(void *ptr);

void kheap_dump(void);

#endif // MEMORY_H