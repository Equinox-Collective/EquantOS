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
#include "../../limine.h"
#include "../core/mem/vmm.h"
#include "../core/mem/pmm.h"
#include "../proc/task.h"
#include "../proc/syscall.h"

static vfs_node_t *devfs_root = NULL;
extern struct limine_framebuffer *kernel_fb;
extern uint64_t hhdm_offset;

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

static int dev_fb_ioctl(vfs_node_t *node, uint64_t req, void *arg) {
    (void)node;
    if (!kernel_fb || !arg) return -EINVAL;

    if (req == FBIOGET_VSCREENINFO) {
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        var->xres = kernel_fb->width;
        var->yres = kernel_fb->height;
        var->bits_per_pixel = kernel_fb->bpp;
        return 0;
    }
    return -ENOTTY;
}

static int64_t dev_fb_mmap(vfs_node_t *node, uint64_t addr, size_t length, int prot, int flags, int64_t offset) {
    (void)node; (void)prot; (void)flags; (void)offset;
    if (!kernel_fb) return -ENODEV;
    if (!current_task || !current_task->process) return -EINVAL;

    // Convert Limine HHDM virtual framebuffer address to raw physical address
    uint64_t fb_phys = (uint64_t)kernel_fb->address - hhdm_offset;
    uint32_t total_size = kernel_fb->pitch * kernel_fb->height;
    if (length > total_size) length = total_size;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);

    // Map physical video memory directly into user virtual space (Write-Combining / Uncached)
    for (size_t i = 0; i < page_count; i++) {
        vmm_map(pml4, addr + (i * PAGE_SIZE), fb_phys + (i * PAGE_SIZE),
                PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PCD | PTE_PWT);
    }

    return (int64_t)addr;
}

static vfs_file_operations_t fb_fops = {
    .read = NULL,
    .write = NULL,
    .ioctl = dev_fb_ioctl,
    .mmap = dev_fb_mmap
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
    devfs_register_device("fb0", &fb_fops, NULL, 0);

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