// ramfs.h - In-memory RAM File System for EquantOS
#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

typedef struct ramfs_file_data {
    uint8_t *buffer;
    size_t capacity;
} ramfs_file_data_t;

vfs_node_t *ramfs_create_root(void);
vfs_node_t *ramfs_create_file(vfs_node_t *parent, const char *name, void *data, size_t size);
vfs_node_t *ramfs_create_directory(vfs_node_t *parent, const char *name);

#endif // RAMFS_H