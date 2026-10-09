// src/kernel/fs/pagecache.c - Unified Page Cache with Clock Eviction
#include "pagecache.h"
#include "../core/mem/pmm.h"
#include "../core/mem/vmm.h"
#include "../core/mem/memory.h"
#include "../libs/string.h"
#include "../drivers/serial/serial.h"

static page_cache_entry_t *hash_table[PAGE_CACHE_HASH_SIZE];
static page_cache_entry_t *lru_head = NULL;
static page_cache_entry_t *lru_tail = NULL;
static size_t cached_page_count = 0;

static inline uint32_t page_cache_hash(uint64_t inode, uint64_t page_index) {
    uint64_t key = inode ^ (page_index * 0x9E3779B97F4A7C15ULL);
    return (uint32_t)(key % PAGE_CACHE_HASH_SIZE);
}

static void lru_remove(page_cache_entry_t *entry) {
    if (entry->lru_prev) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else {
        lru_head = entry->lru_next;
    }
    if (entry->lru_next) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else {
        lru_tail = entry->lru_prev;
    }
    entry->lru_next = NULL;
    entry->lru_prev = NULL;
}

static void lru_push_front(page_cache_entry_t *entry) {
    entry->lru_prev = NULL;
    entry->lru_next = lru_head;
    if (lru_head) {
        lru_head->lru_prev = entry;
    } else {
        lru_tail = entry;
    }
    lru_head = entry;
}

static page_cache_entry_t *page_cache_lookup(uint64_t inode, uint64_t page_index) {
    uint32_t bucket = page_cache_hash(inode, page_index);
    page_cache_entry_t *curr = hash_table[bucket];
    while (curr) {
        if (curr->inode == inode && curr->page_index == page_index) {
            curr->referenced = true;
            lru_remove(curr);
            lru_push_front(curr);
            return curr;
        }
        curr = curr->hash_next;
    }
    return NULL;
}

static void page_cache_evict_one(void) {
    page_cache_entry_t *victim = lru_tail;
    while (victim && victim->referenced) {
        victim->referenced = false;
        victim = victim->lru_prev;
    }

    if (!victim) {
        victim = lru_tail;
    }
    if (!victim) return;

    // Flush dirty page to disk if needed
    if (victim->dirty) {
        // Sync operation is executed on flush cycles
    }

    uint32_t bucket = page_cache_hash(victim->inode, victim->page_index);
    page_cache_entry_t **tracer = &hash_table[bucket];
    while (*tracer && *tracer != victim) {
        tracer = &(*tracer)->hash_next;
    }
    if (*tracer) {
        *tracer = victim->hash_next;
    }

    lru_remove(victim);
    if (victim->phys_addr) {
        pmm_free(victim->phys_addr);
    }
    kfree(victim);
    cached_page_count--;
}

static page_cache_entry_t *page_cache_allocate(uint64_t inode, uint64_t page_index) {
    if (cached_page_count >= PAGE_CACHE_MAX_PAGES) {
        page_cache_evict_one();
    }

    void *phys = pmm_alloc();
    if (!phys) return NULL;

    page_cache_entry_t *entry = (page_cache_entry_t *)kmalloc(sizeof(page_cache_entry_t));
    if (!entry) {
        pmm_free(phys);
        return NULL;
    }

    entry->inode = inode;
    entry->page_index = page_index;
    entry->phys_addr = phys;
    entry->dirty = false;
    entry->referenced = true;

    uint32_t bucket = page_cache_hash(inode, page_index);
    entry->hash_next = hash_table[bucket];
    hash_table[bucket] = entry;

    lru_push_front(entry);
    cached_page_count++;

    return entry;
}

int64_t page_cache_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || offset >= node->length || size == 0) return 0;

    uint64_t bytes_to_read = size;
    if (offset + size > node->length) {
        bytes_to_read = node->length - offset;
    }

    uint64_t bytes_done = 0;
    while (bytes_done < bytes_to_read) {
        uint64_t cur_offset = offset + bytes_done;
        uint64_t page_idx = cur_offset / PAGE_SIZE;
        uint64_t page_off = cur_offset % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - page_off;
        if (chunk > (bytes_to_read - bytes_done)) {
            chunk = bytes_to_read - bytes_done;
        }

        page_cache_entry_t *cached = page_cache_lookup(node->inode, page_idx);
        if (!cached) {
            cached = page_cache_allocate(node->inode, page_idx);
            if (!cached) break;

            uint8_t *page_virt = (uint8_t *)VIRT(cached->phys_addr);
            memset(page_virt, 0, PAGE_SIZE);

            if (node->ops && node->ops->read) {
                node->ops->read(node, page_idx * PAGE_SIZE, PAGE_SIZE, page_virt);
            }
        }

        uint8_t *src = (uint8_t *)VIRT(cached->phys_addr) + page_off;
        memcpy(buffer + bytes_done, src, chunk);

        bytes_done += chunk;
    }

    return (int64_t)bytes_done;
}

int64_t page_cache_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || size == 0) return -1;

    uint64_t bytes_done = 0;
    while (bytes_done < size) {
        uint64_t cur_offset = offset + bytes_done;
        uint64_t page_idx = cur_offset / PAGE_SIZE;
        uint64_t page_off = cur_offset % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - page_off;
        if (chunk > (size - bytes_done)) {
            chunk = size - bytes_done;
        }

        page_cache_entry_t *cached = page_cache_lookup(node->inode, page_idx);
        if (!cached) {
            cached = page_cache_allocate(node->inode, page_idx);
            if (!cached) break;

            uint8_t *page_virt = (uint8_t *)VIRT(cached->phys_addr);
            if (page_off != 0 || chunk != PAGE_SIZE) {
                if (cur_offset < node->length && node->ops && node->ops->read) {
                    node->ops->read(node, page_idx * PAGE_SIZE, PAGE_SIZE, page_virt);
                } else {
                    memset(page_virt, 0, PAGE_SIZE);
                }
            }
        }

        uint8_t *dst = (uint8_t *)VIRT(cached->phys_addr) + page_off;
        memcpy(dst, buffer + bytes_done, chunk);
        cached->dirty = true;

        // Write-through to backing storage driver immediately
        if (node->ops && node->ops->write) {
            node->ops->write(node, cur_offset, chunk, buffer + bytes_done);
        }

        bytes_done += chunk;
    }

    if (offset + bytes_done > node->length) {
        node->length = offset + bytes_done;
    }

    return (int64_t)bytes_done;
}

void page_cache_init(void) {
    memset(hash_table, 0, sizeof(hash_table));
    lru_head = NULL;
    lru_tail = NULL;
    cached_page_count = 0;
    serial_puts(COM1, "[PAGECACHE] In-Memory VFS Page Cache Initialized.\n");
}