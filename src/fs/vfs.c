// vfs.c - Virtual File System core implementation
#include "vfs.h"
#include "../mm/memory.h"
#include "string.h"
#include "stdio.h"

vfs_node_t *vfs_root = NULL;

void vfs_init(void) {
    vfs_root = NULL;
    printf("[VFS] Virtual File System subsystem initialized.\n");
}

vfs_node_t *vfs_mount(const char *path, vfs_node_t *local_root) {
    (void)path;
    if (!vfs_root) {
        vfs_root = local_root;
        return vfs_root;
    }
    // Simple VFS mount logic can be expanded for multi-fs trees
    return NULL;
}

// Helper to traverse path components
static vfs_node_t *vfs_resolve_path(const char *path) {
    if (!path || path[0] != '/') return NULL;
    
    vfs_node_t *current = vfs_root;
    if (!current) return NULL;

    if (path[1] == '\0' || (path[1] == '/' && path[2] == '\0')) {
        return vfs_root; // Root directory
    }

    // Skip leading slash and tokenize path
    char buffer[256];
    size_t len = strlen(path);
    if (len >= sizeof(buffer)) return NULL;
    strcpy(buffer, path + 1);

    char *token = buffer;
    char *rest = buffer;

    while (rest != NULL) {
        token = strsep(&rest, "/");
        if (!token || token[0] == '\0') continue;

        vfs_node_t *next = vfs_finddir(current, token);
        if (!next) return NULL; // Not found
        current = next;
    }

    return current;
}

vfs_node_t *vfs_open(const char *path, uint32_t flags) {
    (void)flags;
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return NULL;

    if (node->ops && node->ops->open) {
        node->ops->open(node);
    }
    return node;
}

int64_t vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, offset, size, buffer);
}

int64_t vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, offset, size, buffer);
}

void vfs_close(vfs_node_t *node) {
    if (node && node->ops && node->ops->close) {
        node->ops->close(node);
    }
}

vfs_node_t *vfs_readdir(vfs_node_t *node, uint32_t index) {
    if (!node || !(node->flags & FS_DIRECTORY) || !node->ops || !node->ops->readdir) {
        return NULL;
    }
    return node->ops->readdir(node, index);
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name) {
    if (!node || !(node->flags & FS_DIRECTORY) || !node->ops || !node->ops->finddir) {
        return NULL;
    }
    return node->ops->finddir(node, name);
}