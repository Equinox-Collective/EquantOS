// src/kernel/fs/vfs.h - Virtual File System abstract layer for EquantOS
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02
#define FS_MOUNTPOINT  0x08
#define FS_SOCKET      0x10
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611

// Standard POSIX File Open & Access Flags
#ifndef O_RDONLY
#define O_RDONLY                 00
#define O_WRONLY                 01
#define O_RDWR                   02
#define O_CREAT                0100
#define O_EXCL                 0200
#define O_NOCTTY               0400
#define O_TRUNC               01000
#define O_APPEND              02000
#define O_NONBLOCK            04000
#define O_DIRECTORY         0200000
#define O_CLOEXEC          02000000
#endif

struct vfs_node;

struct fb_fix_screeninfo {
    char id[16];             // Identification string (e.g. "fb0")
    uint64_t smem_start;     // Physical address of framebuffer
    uint32_t smem_len;       // Total size in bytes
    uint32_t type;           // FB_TYPE_PACKED_PIXELS (0)
    uint32_t type_aux;
    uint32_t visual;         // FB_VISUAL_TRUECOLOR (2)
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;    // Pitch (bytes per scanline)
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

struct fb_bitfield {
    uint32_t offset;    // Beginning of bitfield
    uint32_t length;    // Length of bitfield
    uint32_t msb_right; // != 0 : Most significant bit is right
};

struct fb_var_screeninfo {
    uint32_t xres;           // Visible resolution width
    uint32_t yres;           // Visible resolution height
    uint32_t xres_virtual;   // Virtual resolution width
    uint32_t yres_virtual;   // Virtual resolution height
    uint32_t xoffset;        // Offset from virtual to visible
    uint32_t yoffset;
    uint32_t bits_per_pixel; // 32 bpp
    uint32_t grayscale;      // 0 = color, 1 = grayscale
    struct fb_bitfield red;  // Bitfield in fb mem for Red
    struct fb_bitfield green;// Bitfield for Green
    struct fb_bitfield blue; // Bitfield for Blue
    struct fb_bitfield transp;// Transparency / Alpha
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;         // Height in mm
    uint32_t width;          // Width in mm
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

typedef struct vfs_file_operations {
    int64_t (*read)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    int64_t (*write)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    void (*open)(struct vfs_node *node);
    void (*close)(struct vfs_node *node);
    struct vfs_node* (*readdir)(struct vfs_node *node, uint32_t index);
    struct vfs_node* (*finddir)(struct vfs_node *node, const char *name);
    struct vfs_node* (*create)(struct vfs_node *dir, const char *name, uint32_t flags);
    
    // Hardware & Device Methods
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
    struct vfs_node *ptr; // Used for mountpoints or private driver data
    
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