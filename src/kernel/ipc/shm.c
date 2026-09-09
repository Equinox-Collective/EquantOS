// src/kernel/ipc/shm.c - Native System V Shared Memory Implementation
#include "shm.h"
#include "../core/mem/memory.h"
#include "../core/mem/pmm.h"
#include "../core/mem/vmm.h"
#include "../proc/task.h"
#include "../proc/syscall.h"
#include "string.h"

#define SHM_VIRT_BASE 0x600000000000ULL

typedef struct {
    bool active;
    bool marked_destroy;
    uint32_t key;
    int shmid;
    size_t size;
    size_t page_count;
    void **phys_pages;
    uint32_t nattch;
    uint32_t mode;
} shm_segment_t;

static shm_segment_t shm_segments[SHM_MAX_SEGMENTS];
static int next_shmid = 1;
static uint64_t shm_next_virt = SHM_VIRT_BASE;

void shm_init(void) {
    memset(shm_segments, 0, sizeof(shm_segments));
    next_shmid = 1;
    shm_next_virt = SHM_VIRT_BASE;
}

int64_t sys_shmget_handler(uint64_t key, size_t size, int shmflg) {
    if (size == 0 || size > SHM_MAX_SIZE) return -EINVAL;

    // 1. Search for existing segment if key != IPC_PRIVATE
    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
            if (shm_segments[i].active && shm_segments[i].key == (uint32_t)key) {
                if (shmflg & IPC_EXCL) return -EEXIST;
                return shm_segments[i].shmid;
            }
        }
        if (!(shmflg & IPC_CREAT)) return -ENOENT;
    }

    // 2. Find empty slot
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!shm_segments[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return -ENOSPC;

    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void **phys_list = (void **)kmalloc(pages * sizeof(void *));
    if (!phys_list) return -ENOMEM;

    for (size_t i = 0; i < pages; i++) {
        phys_list[i] = pmm_alloc();
        if (!phys_list[i]) {
            for (size_t j = 0; j < i; j++) pmm_free(phys_list[j]);
            kfree(phys_list);
            return -ENOMEM;
        }
        memset((void *)VIRT((uint64_t)phys_list[i]), 0, PAGE_SIZE);
    }

    shm_segment_t *seg = &shm_segments[slot];
    seg->active = true;
    seg->marked_destroy = false;
    seg->key = (uint32_t)key;
    seg->shmid = next_shmid++;
    seg->size = size;
    seg->page_count = pages;
    seg->phys_pages = phys_list;
    seg->nattch = 0;
    seg->mode = (uint32_t)(shmflg & 0777);

    return (int64_t)seg->shmid;
}

int64_t sys_shmat_handler(int shmid, uint64_t shmaddr, int shmflg) {
    if (!current_task || !current_task->process) return -EINVAL;

    shm_segment_t *seg = NULL;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].active && shm_segments[i].shmid == shmid) {
            seg = &shm_segments[i];
            break;
        }
    }
    if (!seg) return -EINVAL;

    uint64_t vaddr = shmaddr;
    if (vaddr == 0) {
        vaddr = shm_next_virt;
        shm_next_virt += (seg->page_count * PAGE_SIZE);
    }

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    uint64_t pte_flags = PTE_PRESENT | PTE_USER;
    if (!(shmflg & SHM_RDONLY)) pte_flags |= PTE_WRITABLE;

    for (size_t i = 0; i < seg->page_count; i++) {
        vmm_map(pml4, vaddr + (i * PAGE_SIZE), (uint64_t)seg->phys_pages[i], pte_flags);
    }

    seg->nattch++;
    return (int64_t)vaddr;
}


int64_t sys_shmdt_handler(uint64_t shmaddr) {
    if (!current_task || !current_task->process) return -EINVAL;
    if (shmaddr == 0 || (shmaddr & (PAGE_SIZE - 1)) != 0) return -EINVAL;

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    uint64_t first_phys = vmm_get_phys(pml4, shmaddr) & ~0xFFFULL;

    shm_segment_t *seg = NULL;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        // ИСПРАВЛЕНО: проверяем shm_segments[i], а не seg!
        if (shm_segments[i].active && shm_segments[i].page_count > 0 &&
            (uint64_t)shm_segments[i].phys_pages[0] == first_phys) {
            seg = &shm_segments[i];
            break;
        }
    }

    if (seg) {
        for (size_t i = 0; i < seg->page_count; i++) {
            vmm_unmap(pml4, shmaddr + (i * PAGE_SIZE));
        }
        if (seg->nattch > 0) seg->nattch--;
        if (seg->marked_destroy && seg->nattch == 0) {
            for (size_t i = 0; i < seg->page_count; i++) {
                pmm_free(seg->phys_pages[i]);
            }
            kfree(seg->phys_pages);
            seg->active = false;
        }
        return 0;
    }

    return -EINVAL;
}

int64_t sys_shmctl_handler(int shmid, int cmd, void *buf) {
    shm_segment_t *seg = NULL;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].active && shm_segments[i].shmid == shmid) {
            seg = &shm_segments[i];
            break;
        }
    }
    if (!seg) return -EINVAL;

    if (cmd == IPC_RMID) {
        if (seg->nattch == 0) {
            for (size_t i = 0; i < seg->page_count; i++) {
                pmm_free(seg->phys_pages[i]);
            }
            kfree(seg->phys_pages);
            seg->active = false;
        } else {
            seg->marked_destroy = true;
        }
        return 0;
    }

    if (cmd == IPC_STAT && buf) {
        struct shmid_ds *ds = (struct shmid_ds *)buf;
        memset(ds, 0, sizeof(struct shmid_ds));
        ds->shm_perm_key = seg->key;
        ds->shm_segsz = seg->size;
        ds->shm_nattch = seg->nattch;
        return 0;
    }

    return -EINVAL;
}