// vfs.h - Virtual File System abstract layer for EquantOS
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02
#define FS_MOUNTPOINT  0x08
#define FBIOGET_VSCREENINFO 0x4600

struct vfs_node;

struct fb_fix_screeninfo {
    uint64_t smem_start; // Physical address of framebuffer
    uint32_t smem_len;   // Total memory size in bytes
    uint32_t type;
    uint32_t visual;
    uint32_t line_length;// Pitch (bytes per scanline)
};

struct fb_var_screeninfo {
    uint32_t xres;       // Visible resolution width
    uint32_t yres;       // Visible resolution height
    uint32_t bits_per_pixel; // BPP (typically 32)
};

typedef struct vfs_file_operations {
    int64_t (*read)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    int64_t (*write)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    void (*open)(struct vfs_node *node);
    void (*close)(struct vfs_node *node);
    struct vfs_node* (*readdir)(struct vfs_node *node, uint32_t index);
    struct vfs_node* (*finddir)(struct vfs_node *node, const char *name);
    struct vfs_node* (*create)(struct vfs_node *dir, const char *name, uint32_t flags);
    
    // NEW METHODS FOR HARDWARE DEVICES & DRIVERS
    int (*ioctl)(struct vfs_node *node, uint64_t request, void *arg);
    int64_t (*mmap)(struct vfs_node *node, uint64_t addr, size_t length, int prot, int flags, int64_t offset);
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
vfs_node_t *vfs_create(vfs_node_t *dir, const char *name, uint32_t flags);
int64_t vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
int64_t vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
void vfs_close(vfs_node_t *node);
vfs_node_t *vfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name);

extern vfs_node_t *vfs_root;

#endif // VFS_H