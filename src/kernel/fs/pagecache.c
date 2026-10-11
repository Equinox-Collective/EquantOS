// src/kernel/fs/pagecache.c - Unified Page Cache with Clock Eviction
#include "pagecache.h"
#include "../core/mem/pmm.h"
#include "../core/mem/vmm.h"
#include "../core/mem/memory.h"
#include "../core/initcall.h"
#include "../libs/string.h"
#include "../../libs/stdio.h"
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

// --- Sequential read-ahead -----------------------------------------------
// Tiny MRU table of (inode -> next expected offset). A read is sequential
// when it continues the recorded stream; a first-seen inode starts a new
// stream (executables/fonts are read front-to-back, so this warms them).
typedef struct {
    bool valid;
    uint64_t inode;
    uint64_t next_off;
} seq_track_t;

static seq_track_t seq_table[PAGE_CACHE_SEQ_TRACK];

static seq_track_t *seq_find(uint64_t inode) {
    for (int i = 0; i < PAGE_CACHE_SEQ_TRACK; i++) {
        if (seq_table[i].valid && seq_table[i].inode == inode) return &seq_table[i];
    }
    return NULL;
}

static bool page_cache_seq_observe(uint64_t inode, uint64_t offset) {
    seq_track_t *t = seq_find(inode);
    if (!t) {
        // Evict slot 0 (simple round-robin victim) for the new stream.
        static int victim = 0;
        t = &seq_table[victim];
        victim = (victim + 1) % PAGE_CACHE_SEQ_TRACK;
        t->valid = true;
        t->inode = inode;
        t->next_off = offset;
        return true;
    }
    bool sequential = (offset == t->next_off);
    // Move-to-front: keep hot streams resident.
    if (t != &seq_table[0]) {
        seq_track_t tmp = *t;
        memmove(&seq_table[1], &seq_table[0], (size_t)(t - &seq_table[0]) * sizeof(seq_track_t));
        seq_table[0] = tmp;
        t = &seq_table[0];
    }
    t->next_off = offset;
    return sequential;
}

static void page_cache_seq_anchor(uint64_t inode, uint64_t next_off) {
    seq_track_t *t = seq_find(inode);
    if (t) t->next_off = next_off;
}

// Pull up to `count` pages starting at `first_idx` into the cache.
// Skips resident pages and stops at EOF. Never touches user buffers.
static void page_cache_prefetch(vfs_node_t *node, uint64_t first_idx, uint64_t count) {
    if (!node || !node->ops || !node->ops->read) return;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t page_idx = first_idx + i;
        if (page_idx * PAGE_SIZE >= node->length) break;
        if (page_cache_lookup(node->inode, page_idx)) continue;
        page_cache_entry_t *entry = page_cache_allocate(node->inode, page_idx);
        if (!entry) break;
        uint8_t *page_virt = (uint8_t *)VIRT(entry->phys_addr);
        memset(page_virt, 0, PAGE_SIZE);
        node->ops->read(node, page_idx * PAGE_SIZE, PAGE_SIZE, page_virt);
    }
}

int64_t page_cache_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || offset >= node->length || size == 0) return 0;

    uint64_t bytes_to_read = size;
    if (offset + size > node->length) {
        bytes_to_read = node->length - offset;
    }

    // Sequential-stream detector: read-ahead only fires when this read
    // continues the previous one (or starts a new stream), so random
    // access doesn't pollute the cache with useless prefetches.
    bool sequential = page_cache_seq_observe(node->inode, offset);

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

            // Synchronous read-ahead: pull the next pages now so the
            // following sequential reads hit the cache. Bounded by EOF.
            if (sequential && PAGE_CACHE_READ_AHEAD_PAGES > 0) {
                page_cache_prefetch(node, page_idx + 1, PAGE_CACHE_READ_AHEAD_PAGES);
            }
        }

        uint8_t *src = (uint8_t *)VIRT(cached->phys_addr) + page_off;
        memcpy(buffer + bytes_done, src, chunk);

        bytes_done += chunk;
    }

    // Re-anchor the stream to the end of this read so a continuing
    // stream keeps prefetching across both hits and misses.
    page_cache_seq_anchor(node->inode, offset + bytes_done);

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

// Write-through cache: every page_cache_write already reaches the backing
// driver, so sync only clears dirty flags (a future write-back pass then
// has nothing to do). Kept as real functions: the header declares them.
void page_cache_sync_node(vfs_node_t *node) {
    if (!node) return;
    for (uint32_t b = 0; b < PAGE_CACHE_HASH_SIZE; b++) {
        for (page_cache_entry_t *e = hash_table[b]; e; e = e->hash_next) {
            if (e->inode == node->inode) e->dirty = false;
        }
    }
}

void page_cache_sync_all(void) {
    for (uint32_t b = 0; b < PAGE_CACHE_HASH_SIZE; b++) {
        for (page_cache_entry_t *e = hash_table[b]; e; e = e->hash_next) {
            e->dirty = false;
        }
    }
}

// Boot prewarm: read every file listed in CACHE/PRELOAD.TXT (generated on
// the host, seeded into the disk images + ISO). Files flagged FS_CACHEABLE
// land in the page cache; anything missing is skipped silently.
#define PREWARM_LIST_MAX (8u * 1024u)
#define PREWARM_CHUNK    (32u * 1024u)
#define PREWARM_FILE_MAX (64u * 1024u * 1024u)

static void page_cache_prewarm_file(const char *path, uint64_t *files, uint64_t *bytes) {
    vfs_node_t *node = vfs_open(path, 0);
    if (!node || !(node->flags & FS_FILE) || node->length == 0 ||
        node->length > PREWARM_FILE_MAX) {
        return;
    }
    uint8_t *chunk = (uint8_t *)kmalloc(PREWARM_CHUNK);
    if (!chunk) return;
    uint64_t off = 0;
    while (off < node->length) {
        uint64_t want = node->length - off;
        if (want > PREWARM_CHUNK) want = PREWARM_CHUNK;
        int64_t r = vfs_read(node, off, want, chunk);
        if (r <= 0) break;
        off += (uint64_t)r;
    }
    if (off == node->length) {
        (*files)++;
        (*bytes) += off;
    }
    kfree(chunk);
}

void page_cache_prewarm(void) {
    static const char *lists[] = {
        "/drives/fat32_nvme/CACHE/PRELOAD.TXT",
        "/drives/fat32_ata/CACHE/PRELOAD.TXT",
        "/cdrom/CACHE/PRELOAD.TXT",
        "/drives/ext2_nvme/cache/PRELOAD.TXT",
        "/drives/ext2_ata/cache/PRELOAD.TXT",
        NULL,
    };
    const char *used = NULL;
    uint8_t *list = NULL;
    uint64_t list_len = 0;
    for (int i = 0; lists[i]; i++) {
        vfs_node_t *node = vfs_open(lists[i], 0);
        if (!node || !(node->flags & FS_FILE) || node->length == 0 ||
            node->length > PREWARM_LIST_MAX) {
            continue;
        }
        list = (uint8_t *)kmalloc((size_t)node->length + 1);
        if (!list) return;
        int64_t r = vfs_read(node, 0, node->length, list);
        if (r != (int64_t)node->length) {
            kfree(list);
            list = NULL;
            continue;
        }
        list[node->length] = '\0';
        list_len = node->length;
        used = lists[i];
        break;
    }
    if (!list) {
        serial_puts(COM1, "[PAGECACHE] no preload list found, skipping prewarm.\n");
        return;
    }

    uint64_t files = 0, bytes = 0;
    char line[256];
    size_t li = 0;
    for (uint64_t p = 0; p <= list_len; p++) {
        char c = (p < list_len) ? (char)list[p] : '\n';
        if (c == '\r') continue;
        if (c == '\n') {
            line[li] = '\0';
            // Trim leading whitespace.
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s != '\0' && *s != '#') {
                // Trim trailing whitespace.
                size_t e = strlen(s);
                while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) s[--e] = '\0';
                if (s[0] != '\0') page_cache_prewarm_file(s, &files, &bytes);
            }
            li = 0;
        } else if (li < sizeof(line) - 1) {
            line[li++] = c;
        }
    }
    kfree(list);

    char msg[128];
    snprintf(msg, sizeof(msg), "[PAGECACHE] prewarm: %llu files, %llu bytes from %s\n",
             (unsigned long long)files, (unsigned long long)bytes, used ? used : "?");
    serial_puts(COM1, msg);
}

static int __init pagecache_prewarm_initcall(void) {
    page_cache_prewarm();
    return 0;
}
late_initcall(pagecache_prewarm_initcall);