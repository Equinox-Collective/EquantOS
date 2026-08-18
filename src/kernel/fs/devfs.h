// src/kernel/fs/devfs.h - Dynamic Device File System (DevFS) for EquantOS
#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

void devfs_init(void);
vfs_node_t *devfs_get_root(void);
vfs_node_t *devfs_register_device(const char *name, vfs_file_operations_t *fops, void *ptr, uint32_t flags);

#endif // DEVFS_H