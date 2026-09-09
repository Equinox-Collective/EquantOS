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
#include "../drivers/tty/tty.h"
#include "../drivers/input/evdev.h"
#include "../core/uevent.h"

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

    // Handle both GET and PUT video mode queries
    if (req == FBIOGET_VSCREENINFO || req == FBIOPUT_VSCREENINFO) {
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        if (req == FBIOGET_VSCREENINFO) {
            memset(var, 0, sizeof(struct fb_var_screeninfo));
            var->xres = (uint32_t)kernel_fb->width;
            var->yres = (uint32_t)kernel_fb->height;
            var->xres_virtual = (uint32_t)kernel_fb->width;
            var->yres_virtual = (uint32_t)kernel_fb->height;
            var->xoffset = 0;
            var->yoffset = 0;
            var->bits_per_pixel = (uint32_t)kernel_fb->bpp;
            
            // Standard 32-bit ARGB TrueColor masks (Crucial for Kdrive visual initialization!)
            var->red.offset = 16;
            var->red.length = 8;
            var->green.offset = 8;
            var->green.length = 8;
            var->blue.offset = 0;
            var->blue.length = 8;
            var->transp.offset = 24;
            var->transp.length = 8;
        }
        return 0; // Mode confirmed successfully!
    }

    if (req == FBIOGET_FSCREENINFO) {
        struct fb_fix_screeninfo *fix = (struct fb_fix_screeninfo *)arg;
        memset(fix, 0, sizeof(struct fb_fix_screeninfo));
        
        strncpy(fix->id, "fb0", sizeof(fix->id) - 1);
        fix->smem_start = (uint64_t)kernel_fb->address - hhdm_offset;
        fix->smem_len   = (uint32_t)(kernel_fb->pitch * kernel_fb->height);
        fix->type       = 0; // FB_TYPE_PACKED_PIXELS
        fix->visual     = 2; // FB_VISUAL_TRUECOLOR
        fix->line_length = (uint32_t)kernel_fb->pitch;
        return 0;
    }

    // Panning & Screen Blanking ioctls
    if (req == FBIOPAN_DISPLAY || req == FBIOBLANK) {
        return 0;
    }

    return -ENOTTY;
}

static int64_t dev_fb_mmap(vfs_node_t *node, uint64_t addr, size_t length, int prot, int flags, int64_t offset) {
    (void)node; (void)prot; (void)flags;
    if (!kernel_fb) return -ENODEV;
    if (!current_task || !current_task->process) return -EINVAL;

    // Physical base address of the video framebuffer
    uint64_t fb_phys = (uint64_t)kernel_fb->address - hhdm_offset;
    uint64_t total_size = (uint64_t)kernel_fb->pitch * kernel_fb->height;

    // If application requests full buffer or more, map the whole screen
    if (length == 0 || length > total_size) {
        length = total_size;
    }

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);

    // Map physical video memory directly into user address space
    for (size_t i = 0; i < page_count; i++) {
        uint64_t p_addr = fb_phys + (uint64_t)offset + (i * PAGE_SIZE);
        vmm_map(pml4, addr + (i * PAGE_SIZE), p_addr,
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

// Handler for /dev/tty read/write
static int64_t dev_tty_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (size == 0 || !buffer) return 0;
    
    // Check if non-blocking mode is set on this descriptor
    bool nonblock = false;
    if (current_task && current_task->process) {
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            if (current_task->process->files[i] == node) {
                if (current_task->process->file_flags[i] & O_NONBLOCK) {
                    nonblock = true;
                }
                break;
            }
        }
    }

    if (nonblock) {
        int c = tty_getchar_nonblock();
        if (c == -1) {
            return -EAGAIN; // Non-blocking: buffer empty, return immediately!
        }
        buffer[0] = (uint8_t)c;
        return 1;
    }

    // Standard blocking read for Bash
    char c = tty_getchar();
    if (c == 0x04) return 0; // EOF
    buffer[0] = (uint8_t)c;
    return 1;
}

static vfs_file_operations_t tty_device_fops = {
    .read = dev_tty_read,
    .write = dev_tty_write,
    .ioctl = NULL
};

static int64_t devfs_tty_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;

    bool nonblock = false;
    if (current_task && current_task->process) {
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            if (current_task->process->files[i] == node) {
                if (current_task->process->file_flags[i] & O_NONBLOCK) {
                    nonblock = true;
                }
                break;
            }
        }
    }

    if (nonblock) {
        size_t read_bytes = 0;
        while (read_bytes < size) {
            int c = tty_getchar_nonblock();
            if (c == -1) {
                if (read_bytes == 0) return -EAGAIN;
                break;
            }
            buffer[read_bytes++] = (uint8_t)c;
        }
        return (int64_t)read_bytes;
    }

    // Normal blocking read for Bash
    size_t bytes_read = 0;
    while (bytes_read < size) {
        char c = tty_getchar();
        if (c == 0x04) break;
        buffer[bytes_read++] = (uint8_t)c;
        if (c == '\n') break;
    }
    return (int64_t)bytes_read;
}

// Canonical TTY write implementation (Outputs to Display and Serial)
static int64_t devfs_tty_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;

    for (size_t i = 0; i < size; i++) {
        char c = (char)buffer[i];
        serial_putchar(COM1, c);
        term_putchar_raw(c);
    }
    return (int64_t)size;
}

vfs_file_operations_t g_tty_fops = {
    .read = devfs_tty_read,
    .write = devfs_tty_write,
    .open = NULL,
    .close = NULL,
    .readdir = NULL,
    .finddir = NULL,
    .create = NULL,
    .ioctl = NULL,
    .mmap = NULL
};

void devfs_init(void) {
    devfs_root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(devfs_root->name, "dev");
    devfs_root->flags = FS_DIRECTORY;
    devfs_root->ops = &devfs_root_fops;

    // 1. Initialize Subsystems
    evdev_init();
    uevent_init();

    // 2. Standard TTY and NULL devices
    devfs_register_device("null", &null_fops, NULL, 0666);
    devfs_register_device("tty",  &tty_device_fops, NULL, 0666);
    devfs_register_device("tty0", &tty_device_fops, NULL, 0666);
    devfs_register_device("tty1", &tty_device_fops, NULL, 0666);

    // 3. Register Framebuffer
    devfs_register_device("fb0", &fb_fops, NULL, 0666);

    // 4. Register Legacy /dev/mouse and /dev/psaux (PS/2 streams)
    devfs_register_device("mouse", &g_mousedev_fops, NULL, 0666);
    devfs_register_device("psaux", &g_mousedev_fops, NULL, 0666);

    // 5. Create /dev/input directory and nodes
    vfs_node_t *input_dir = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(input_dir->name, "input");
    input_dir->flags = FS_DIRECTORY;
    input_dir->ops = &devfs_root_fops;
    input_dir->parent = devfs_root;

    // Link /dev/input to devfs root
    input_dir->next = devfs_root->children;
    devfs_root->children = input_dir;

    // Register /dev/input/event0 (Keyboard), /dev/input/event1 (Mouse), /dev/input/mice
    vfs_node_t *ev0 = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(ev0->name, "event0");
    ev0->flags = FS_FILE;
    ev0->ops = &g_evdev_kbd_fops;
    ev0->parent = input_dir;

    vfs_node_t *ev1 = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(ev1->name, "event1");
    ev1->flags = FS_FILE;
    ev1->ops = &g_evdev_mouse_fops;
    ev1->parent = input_dir;

    vfs_node_t *mice = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(mice->name, "mice");
    mice->flags = FS_FILE;
    mice->ops = &g_mousedev_fops;
    mice->parent = input_dir;

    mice->next = NULL;
    ev1->next = mice;
    ev0->next = ev1;
    input_dir->children = ev0;

    // 6. Broadcast uevent announcements to userspace
    uevent_broadcast(KOBJ_ADD, "input", "input/event0", 13, 64);
    uevent_broadcast(KOBJ_ADD, "input", "input/event1", 13, 65);
    uevent_broadcast(KOBJ_ADD, "input", "input/mice",   13, 63);

    // 7. Temporary mount directories
    if (vfs_root) {
        vfs_node_t *tmp_dir = vfs_finddir(vfs_root, "tmp");
        if (!tmp_dir) {
            tmp_dir = ramfs_create_directory(vfs_root, "tmp");
        }
        if (tmp_dir && !vfs_finddir(tmp_dir, ".X11-unix")) {
            ramfs_create_directory(tmp_dir, ".X11-unix");
        }
    }

    serial_puts(COM1, "[DEVFS] Nodes /dev/input/event0, event1, mice and /dev/mouse active.\n");
}

static int __init devfs_initcall(void) {
    devfs_init();
    return 0;
}
fs_initcall(devfs_initcall);