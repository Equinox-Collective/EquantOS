// ramfs.c - In-memory RAM File System implementation
#include "ramfs.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "stdio.h"

static int64_t ramfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    ramfs_file_data_t *fdata = (ramfs_file_data_t *)node->ptr;
    if (!fdata || offset >= node->length) return 0;

    uint64_t bytes_to_read = size;
    if (offset + size > node->length) {
        bytes_to_read = node->length - offset;
    }

    memcpy(buffer, fdata->buffer + offset, bytes_to_read);
    return bytes_to_read;
}

static int64_t ramfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    ramfs_file_data_t *fdata = (ramfs_file_data_t *)node->ptr;
    if (!fdata) return -1;

    uint64_t required_size = offset + size;
    if (required_size > fdata->capacity) {
        size_t new_cap = required_size + 1024; // Buffer growth padding
        uint8_t *new_buf = (uint8_t *)krealloc(fdata->buffer, new_cap);
        if (!new_buf) return -1;
        fdata->buffer = new_buf;
        fdata->capacity = new_cap;
    }

    memcpy(fdata->buffer + offset, buffer, size);
    if (required_size > node->length) {
        node->length = required_size;
    }
    return size;
}

static vfs_node_t *ramfs_readdir(vfs_node_t *node, uint32_t index) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;
    vfs_node_t *child = node->children;
    uint32_t i = 0;
    while (child) {
        if (i == index) return child;
        child = child->next;
        i++;
    }
    return NULL;
}

static vfs_node_t *ramfs_finddir(vfs_node_t *node, const char *name) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;
    vfs_node_t *child = node->children;
    while (child) {
        if (strcmp(child->name, name) == 0) return child;
        child = child->next;
    }
    return NULL;
}

static vfs_file_operations_t ramfs_fops = {
    .read = ramfs_read,
    .write = ramfs_write,
    .open = NULL,
    .close = NULL,
    .readdir = ramfs_readdir,
    .finddir = ramfs_finddir
};

vfs_node_t *ramfs_create_root(void) {
    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->permissions = 0755;
    root->ops = &ramfs_fops;
    root->parent = root;
    return root;
}

vfs_node_t *ramfs_create_directory(vfs_node_t *parent, const char *name) {
    vfs_node_t *dir = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!dir) return NULL; // Safety guard against OOM
    
    strcpy(dir->name, name);
    dir->flags = FS_DIRECTORY;
    dir->permissions = 0755;
    dir->ops = &ramfs_fops;
    dir->parent = parent;

    if (!parent) {
        parent = dir; // Root self-parenting fallback
    }

    // Append to parent children list
    if (!parent->children) {
        parent->children = dir;
    } else {
        vfs_node_t *curr = parent->children;
        while (curr->next) curr = curr->next;
        curr->next = dir;
    }
    return dir;
}

vfs_node_t *ramfs_create_file(vfs_node_t *parent, const char *name, void *data, size_t size) {
    vfs_node_t *file = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!file) return NULL; // Safety guard against OOM

    strcpy(file->name, name);
    file->flags = FS_FILE;
    file->permissions = 0644;
    file->length = size;
    file->ops = &ramfs_fops;
    file->parent = parent;

    ramfs_file_data_t *fdata = (ramfs_file_data_t *)kzalloc(sizeof(ramfs_file_data_t));
    if (!fdata) {
        kfree(file);
        return NULL;
    }

    if (size > 0 && data) {
        fdata->buffer = (uint8_t *)kmalloc(size);
        if (fdata->buffer) {
            memcpy(fdata->buffer, data, size);
            fdata->capacity = size;
        } else {
            kfree(fdata);
            kfree(file);
            return NULL;
        }
    } else {
        fdata->buffer = NULL;
        fdata->capacity = 0;
    }
    file->ptr = (vfs_node_t *)fdata;

    if (parent) {
        if (!parent->children) {
            parent->children = file;
        } else {
            vfs_node_t *curr = parent->children;
            while (curr->next) curr = curr->next;
            curr->next = file;
        }
    }
    return file;
}