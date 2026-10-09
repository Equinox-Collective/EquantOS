// src/kernel/fs/pagecache.h - High-Performance In-Memory VFS Page Cache
#ifndef PAGECACHE_H
#define PAGECACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vfs.h"

#define PAGE_CACHE_HASH_SIZE 1024
#define PAGE_CACHE_MAX_PAGES 2048 // 8MB of RAM dedicated for cache pool

typedef struct page_cache_entry {
    uint64_t inode;
    uint64_t page_index;
    void *phys_addr;
    bool dirty;
    bool referenced;
    struct page_cache_entry *hash_next;
    struct page_cache_entry *lru_next;
    struct page_cache_entry *lru_prev;
} page_cache_entry_t;

void page_cache_init(void);
int64_t page_cache_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
int64_t page_cache_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
void page_cache_sync_node(vfs_node_t *node);
void page_cache_sync_all(void);

#endif // PAGECACHE_H