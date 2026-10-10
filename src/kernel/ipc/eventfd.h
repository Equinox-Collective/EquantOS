// src/kernel/ipc/eventfd.h - eventfd(2) counters (used by libcurl's multi wakeup)
#ifndef EVENTFD_H
#define EVENTFD_H

#include <stdint.h>
#include <stdbool.h>
#include "../fs/vfs.h"

#define EFD_SEMAPHORE 1

extern vfs_file_operations_t eventfd_vfs_ops;

vfs_node_t *eventfd_create(uint64_t initval, int flags);
int64_t eventfd_read(vfs_node_t *node, void *buf, uint64_t count, bool nonblock);
int64_t eventfd_write(vfs_node_t *node, const void *buf, uint64_t count, bool nonblock);
bool eventfd_readable(vfs_node_t *node);

#endif // EVENTFD_H
