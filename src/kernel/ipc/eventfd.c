// src/kernel/ipc/eventfd.c - eventfd(2): a 64-bit counter behind a file descriptor
#include "eventfd.h"
#include "../core/mem/memory.h"
#include "../misc/timer.h"
#include "../proc/task.h"
#include "../proc/sched.h"
#include "string.h"

#define EFD_EAGAIN 11
#define EFD_EINVAL 22

typedef struct {
    uint64_t count;
    bool semaphore;
} eventfd_t;

static void eventfd_close_op(vfs_node_t *node) {
    if (node && node->ptr) {
        kfree(node->ptr);
        node->ptr = NULL;
    }
}

static int64_t eventfd_read_op(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    return eventfd_read(node, buffer, size, false);
}

static int64_t eventfd_write_op(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    return eventfd_write(node, buffer, size, false);
}

vfs_file_operations_t eventfd_vfs_ops = {
    .read    = eventfd_read_op,
    .write   = eventfd_write_op,
    .open    = NULL,
    .close   = eventfd_close_op,
    .readdir = NULL,
    .finddir = NULL,
    .create  = NULL,
    .ioctl   = NULL,
    .mmap    = NULL
};

vfs_node_t *eventfd_create(uint64_t initval, int flags) {
    eventfd_t *efd = (eventfd_t *)kzalloc(sizeof(eventfd_t));
    vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!efd || !node) {
        if (efd) kfree(efd);
        if (node) kfree(node);
        return NULL;
    }
    efd->count = initval;
    efd->semaphore = (flags & EFD_SEMAPHORE) != 0;

    strcpy(node->name, "anon_inode:[eventfd]");
    node->flags = FS_FILE;
    node->ops = &eventfd_vfs_ops;
    node->ptr = (struct vfs_node *)efd;
    return node;
}

int64_t eventfd_read(vfs_node_t *node, void *buf, uint64_t count, bool nonblock) {
    eventfd_t *efd = (eventfd_t *)node->ptr;
    if (!efd || count < sizeof(uint64_t)) return -EFD_EINVAL;

    while (efd->count == 0) {
        if (nonblock) return -EFD_EAGAIN;
        io_wait_prepare();
        sched_make_sleep(current_task, tick + 2);
        sched_yield();
        io_wait_done();
    }

    uint64_t value;
    if (efd->semaphore) {
        value = 1;
        efd->count--;
    } else {
        value = efd->count;
        efd->count = 0;
    }
    memcpy(buf, &value, sizeof(value));
    return sizeof(value);
}

int64_t eventfd_write(vfs_node_t *node, const void *buf, uint64_t count, bool nonblock) {
    eventfd_t *efd = (eventfd_t *)node->ptr;
    if (!efd || count < sizeof(uint64_t)) return -EFD_EINVAL;

    uint64_t value;
    memcpy(&value, buf, sizeof(value));
    if (value == UINT64_MAX) return -EFD_EINVAL;

    while (UINT64_MAX - 1 - efd->count < value) {
        if (nonblock) return -EFD_EAGAIN;
        sched_make_sleep(current_task, tick + 1);
        sched_yield();
    }
    efd->count += value;
    if (value) io_wake_all();
    return sizeof(value);
}

bool eventfd_readable(vfs_node_t *node) {
    eventfd_t *efd = (eventfd_t *)node->ptr;
    return efd && efd->count > 0;
}
