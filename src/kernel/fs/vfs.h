// vfs.h - Virtual File System abstract layer for EquantOS
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02
#define FS_MOUNTPOINT  0x08

struct vfs_node;

typedef struct vfs_file_operations {
    int64_t (*read)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    int64_t (*write)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    void (*open)(struct vfs_node *node);
    void (*close)(struct vfs_node *node);
    struct vfs_node* (*readdir)(struct vfs_node *node, uint32_t index);
    struct vfs_node* (*finddir)(struct vfs_node *node, const char *name);
} vfs_file_operations_t;

typedef struct vfs_node {
    char name[128];
    uint32_t flags;
    uint32_t permissions;
    uint64_t length;
    uint64_t inode;
    vfs_file_operations_t *ops;
    struct vfs_node *ptr; // Used for mountpoints
    
    // Tree hierarchy links
    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next;
} vfs_node_t;

void vfs_init(void);
vfs_node_t *vfs_mount(const char *path, vfs_node_t *local_root);
vfs_node_t *vfs_open(const char *path, uint32_t flags);
int64_t vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
int64_t vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
void vfs_close(vfs_node_t *node);
vfs_node_t *vfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name);

extern vfs_node_t *vfs_root;

#endif // VFS_H