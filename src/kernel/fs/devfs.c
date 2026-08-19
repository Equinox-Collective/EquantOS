// src/kernel/fs/devfs.c - DevFS Dynamic Device Node Registry Implementation
#include "devfs.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "vfs.h"
#include "ramfs.h"
#include "../drivers/serial/serial.h"
#include "../drivers/input.h"
#include "../../equterm/term.h"
#include "../core/initcall.h"

static vfs_node_t *devfs_root = NULL;

// Handlers for /dev/null
static int64_t dev_null_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0; // Always EOF
}

static int64_t dev_null_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset; (void)buffer;
    return size; // Consume bytes silently
}

static vfs_file_operations_t null_fops = {
    .read = dev_null_read,
    .write = dev_null_write
};

// Handlers for /dev/input0 (Unified Input Subsystem Event Stream)
static int64_t dev_input_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    if (size < sizeof(input_event_t)) return -1;

    input_event_t *ev = (input_event_t *)buffer;
    if (input_pop_event(ev)) {
        return sizeof(input_event_t);
    }
    return 0; // No events pending
}

static vfs_file_operations_t input_fops = {
    .read = dev_input_read,
    .write = NULL
};

// Handlers for /dev/tty0 (Terminal Display Output)
static int64_t dev_tty_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    for (uint64_t i = 0; i < size; i++) {
        term_putchar((char)buffer[i]);
    }
    return size;
}

static vfs_file_operations_t tty_fops = {
    .read = NULL,
    .write = dev_tty_write
};

vfs_node_t *devfs_get_root(void) {
    return devfs_root;
}

vfs_node_t *devfs_register_device(const char *name, vfs_file_operations_t *fops, void *ptr, uint32_t flags) {
    if (!devfs_root) return NULL;

    vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->flags = flags | FS_FILE;
    node->ops = fops;
    node->ptr = (vfs_node_t *)ptr;
    node->parent = devfs_root;

    // Append to DevFS children list
    if (!devfs_root->children) {
        devfs_root->children = node;
    } else {
        vfs_node_t *curr = devfs_root->children;
        while (curr->next) curr = curr->next;
        curr->next = node;
    }

    return node;
}

static vfs_node_t *devfs_readdir(vfs_node_t *node, uint32_t index) {
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

static vfs_node_t *devfs_finddir(vfs_node_t *node, const char *name) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;
    vfs_node_t *child = node->children;
    while (child) {
        if (strcmp(child->name, name) == 0) return child;
        child = child->next;
    }
    return NULL;
}

static vfs_file_operations_t devfs_root_fops = {
    .readdir = devfs_readdir,
    .finddir = devfs_finddir
};

void devfs_init(void) {
    devfs_root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(devfs_root->name, "dev");
    devfs_root->flags = FS_DIRECTORY;
    devfs_root->ops = &devfs_root_fops;

    // Register Default Core Devices
    devfs_register_device("null", &null_fops, NULL, 0);
    devfs_register_device("input0", &input_fops, NULL, 0);
    devfs_register_device("tty0", &tty_fops, NULL, 0);

    // Mount /dev onto VFS root
    if (vfs_root) {
        vfs_node_t *dev_dir = ramfs_create_directory(vfs_root, "dev");
        if (dev_dir) {
            dev_dir->flags |= FS_MOUNTPOINT;
            dev_dir->ptr = (struct vfs_node *)devfs_root;
        }
    }

    serial_puts(COM1, "[DEVFS] Dynamic DevFS mounted at '/dev' with /dev/null, /dev/input0, /dev/tty0.\n");
}

static int __init devfs_initcall(void) {
    devfs_init();
    return 0;
}
fs_initcall(devfs_initcall);