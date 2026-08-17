// src/kernel/core/mem/slab.h - Bonwick Slab Allocator & UMA Interface
#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include <stddef.h>

typedef struct kmem_cache kmem_cache_t;

/**
 * @brief Create a specialized object cache (Solaris/FreeBSD style)
 * @param name Cache debugging identifier
 * @param object_size Size of individual elements
 * @param align Strict memory alignment (e.g., 16 or 64 bytes for SIMD)
 */
kmem_cache_t *kmem_cache_create(const char *name, size_t object_size, size_t align);

void *kmem_cache_alloc(kmem_cache_t *cache);
void kmem_cache_free(kmem_cache_t *cache, void *obj);

void slab_init(void);

#endif // SLAB_H