#include "iso9660.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "stdio.h"
#include "../drivers/serial/serial.h"
#include "../core/initcall.h"
#include "../core/gen/io.h"
#include "ramfs.h"

typedef struct {
    block_device_t dev;
    uint32_t root_extent_lba;
    uint32_t root_data_length;
} iso9660_vol_t;

static iso9660_vol_t iso_vol;

static uint16_t g_atapi_io = 0;
static uint16_t g_atapi_ctrl = 0;
static uint8_t  g_atapi_drive = 0;

static int atapi_wait_ready(uint16_t io) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t st = inb(io + 7);
        if (st & 0x01) return -1;
        if (!(st & 0x80) && (st & 0x08)) return 0;
    }
    return -1;
}

static int atapi_read_blocks(uint64_t lba, uint32_t count, void *buf) {
    if (!g_atapi_io || count == 0 || !buf) return -1;

    uint16_t *dest = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint32_t cur_lba = (uint32_t)(lba + s);

        outb(g_atapi_io + 6, g_atapi_drive);
        for (int i = 0; i < 100000; i++) {
            if (!(inb(g_atapi_io + 7) & 0x80)) break;
        }

        outb(g_atapi_io + 1, 0x00);
        outb(g_atapi_io + 4, (uint8_t)(ISO_SECTOR_SIZE & 0xFF));
        outb(g_atapi_io + 5, (uint8_t)(ISO_SECTOR_SIZE >> 8));
        outb(g_atapi_io + 7, 0xA0);

        if (atapi_wait_ready(g_atapi_io) != 0) return -1;

        uint8_t pkt[12] = {
            0x28, 0,
            (uint8_t)((cur_lba >> 24) & 0xFF),
            (uint8_t)((cur_lba >> 16) & 0xFF),
            (uint8_t)((cur_lba >> 8) & 0xFF),
            (uint8_t)(cur_lba & 0xFF),
            0,
            0, 1,
            0, 0, 0
        };

        uint16_t *pkt16 = (uint16_t *)pkt;
        for (int i = 0; i < 6; i++) {
            outw(g_atapi_io, pkt16[i]);
        }

        if (atapi_wait_ready(g_atapi_io) != 0) return -1;

        for (int i = 0; i < 1024; i++) {
            dest[i] = inw(g_atapi_io);
        }
        dest += 1024;
    }
    return 0;
}

static int iso9660_read_sector(block_device_t dev, uint32_t iso_sector, void *buf) {
    if (dev.sector_size == 2048) {
        return dev.read(iso_sector, 1, buf);
    } else if (dev.sector_size == 512) {
        return dev.read((uint64_t)iso_sector * 4, 4, buf);
    }
    return -1;
}

static bool iso9660_get_rock_ridge_name(iso9660_dir_record_t *rec, char *dest, size_t max_dest) {
    size_t sua_offset = 33 + rec->name_len;
    if ((sua_offset % 2) != 0) sua_offset++;

    uint8_t *entry = (uint8_t *)rec;
    while (sua_offset + 4 <= rec->length) {
        uint8_t *susp = entry + sua_offset;
        uint8_t sig0 = susp[0];
        uint8_t sig1 = susp[1];
        uint8_t len  = susp[2];

        if (len < 4 || sua_offset + len > rec->length) break;

        if (sig0 == 'N' && sig1 == 'M' && len >= 5) {
            uint8_t name_len = len - 5;
            if (name_len >= max_dest) name_len = max_dest - 1;
            memcpy(dest, susp + 5, name_len);
            dest[name_len] = '\0';
            return true;
        }

        sua_offset += len;
    }
    return false;
}

static void iso9660_clean_name(iso9660_dir_record_t *rec, char *dest, size_t max_dest) {
    if (rec->name_len == 1 && rec->file_identifier[0] == 0x00) {
        strcpy(dest, ".");
        return;
    }
    if (rec->name_len == 1 && rec->file_identifier[0] == 0x01) {
        strcpy(dest, "..");
        return;
    }

    if (iso9660_get_rock_ridge_name(rec, dest, max_dest)) {
        return;
    }

    const char *raw_name = rec->file_identifier;
    uint8_t len = rec->name_len;
    size_t out = 0;
    for (uint8_t i = 0; i < len && out < max_dest - 1; i++) {
        char c = raw_name[i];
        if (c == ';') break;
        if (c >= 'A' && c <= 'Z') c += 32;
        dest[out++] = c;
    }
    if (out > 1 && dest[out - 1] == '.' && dest[out - 2] != '.') {
        out--;
    }
    dest[out] = '\0';
}

static int64_t iso9660_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || !buffer || offset >= node->length) return 0;

    uint32_t extent_lba = (uint32_t)(uintptr_t)node->ptr;
    uint64_t bytes_to_read = size;
    if (offset + size > node->length) {
        bytes_to_read = node->length - offset;
    }

    uint8_t sector_buf[ISO_SECTOR_SIZE];
    uint64_t bytes_read = 0;

    while (bytes_read < bytes_to_read) {
        uint64_t current_offset = offset + bytes_read;
        uint32_t iso_sec = extent_lba + (uint32_t)(current_offset / ISO_SECTOR_SIZE);
        uint32_t sec_offset = (uint32_t)(current_offset % ISO_SECTOR_SIZE);

        if (iso9660_read_sector(iso_vol.dev, iso_sec, sector_buf) != 0) {
            break;
        }

        uint64_t chunk = ISO_SECTOR_SIZE - sec_offset;
        if (chunk > bytes_to_read - bytes_read) {
            chunk = bytes_to_read - bytes_read;
        }

        memcpy(buffer + bytes_read, sector_buf + sec_offset, chunk);
        bytes_read += chunk;
    }

    return (int64_t)bytes_read;
}

static vfs_file_operations_t iso_fops;

static vfs_node_t *iso9660_finddir(vfs_node_t *node, const char *name) {
    if (!node || !(node->flags & FS_DIRECTORY) || !name) return NULL;

    uint32_t extent_lba = (uint32_t)(uintptr_t)node->ptr;
    uint32_t dir_len = (uint32_t)node->length;
    if (dir_len == 0) return NULL;

    uint32_t sectors = (dir_len + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    size_t alloc_size = (size_t)sectors * ISO_SECTOR_SIZE;
    uint8_t *dir_buf = (uint8_t *)kmalloc(alloc_size);
    if (!dir_buf) return NULL;

    for (uint32_t s = 0; s < sectors; s++) {
        if (iso9660_read_sector(iso_vol.dev, extent_lba + s, dir_buf + (s * ISO_SECTOR_SIZE)) != 0) {
            kfree(dir_buf);
            return NULL;
        }
    }

    uint32_t pos = 0;
    while (pos < dir_len) {
        iso9660_dir_record_t *rec = (iso9660_dir_record_t *)(dir_buf + pos);
        if (rec->length == 0) {
            pos = (pos + ISO_SECTOR_SIZE) & ~(ISO_SECTOR_SIZE - 1);
            continue;
        }
        if (pos + rec->length > dir_len) break;

        char clean_name[128];
        iso9660_clean_name(rec, clean_name, sizeof(clean_name));

        if (strcmp(clean_name, name) == 0) {
            vfs_node_t *child = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
            if (!child) {
                kfree(dir_buf);
                return NULL;
            }
            strncpy(child->name, clean_name, sizeof(child->name) - 1);
            child->length = rec->data_length_lsb;
            child->inode = rec->extent_lba_lsb;
            child->ptr = (vfs_node_t *)(uintptr_t)rec->extent_lba_lsb;
            child->flags = (rec->flags & 0x02) ? FS_DIRECTORY : FS_FILE;
            child->permissions = (rec->flags & 0x02) ? 0755 : 0644;
            child->ops = &iso_fops;
            child->parent = node;
            kfree(dir_buf);
            return child;
        }
        pos += rec->length;
    }

    kfree(dir_buf);
    return NULL;
}

static vfs_node_t *iso9660_readdir(vfs_node_t *node, uint32_t index) {
    if (!node || !(node->flags & FS_DIRECTORY)) return NULL;

    uint32_t extent_lba = (uint32_t)(uintptr_t)node->ptr;
    uint32_t dir_len = (uint32_t)node->length;
    if (dir_len == 0) return NULL;

    uint32_t sectors = (dir_len + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    size_t alloc_size = (size_t)sectors * ISO_SECTOR_SIZE;
    uint8_t *dir_buf = (uint8_t *)kmalloc(alloc_size);
    if (!dir_buf) return NULL;

    for (uint32_t s = 0; s < sectors; s++) {
        if (iso9660_read_sector(iso_vol.dev, extent_lba + s, dir_buf + (s * ISO_SECTOR_SIZE)) != 0) {
            kfree(dir_buf);
            return NULL;
        }
    }

    uint32_t pos = 0;
    uint32_t current_idx = 0;

    while (pos < dir_len) {
        iso9660_dir_record_t *rec = (iso9660_dir_record_t *)(dir_buf + pos);
        if (rec->length == 0) {
            pos = (pos + ISO_SECTOR_SIZE) & ~(ISO_SECTOR_SIZE - 1);
            continue;
        }
        if (pos + rec->length > dir_len) break;

        char clean_name[128];
        iso9660_clean_name(rec, clean_name, sizeof(clean_name));

        if (strcmp(clean_name, ".") != 0 && strcmp(clean_name, "..") != 0) {
            if (current_idx == index) {
                vfs_node_t *child = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                if (!child) {
                    kfree(dir_buf);
                    return NULL;
                }
                strncpy(child->name, clean_name, sizeof(child->name) - 1);
                child->length = rec->data_length_lsb;
                child->inode = rec->extent_lba_lsb;
                child->ptr = (vfs_node_t *)(uintptr_t)rec->extent_lba_lsb;
                child->flags = (rec->flags & 0x02) ? FS_DIRECTORY : FS_FILE;
                child->permissions = (rec->flags & 0x02) ? 0755 : 0644;
                child->ops = &iso_fops;
                child->parent = node;
                kfree(dir_buf);
                return child;
            }
            current_idx++;
        }
        pos += rec->length;
    }

    kfree(dir_buf);
    return NULL;
}

static vfs_file_operations_t iso_fops = {
    .read = iso9660_read,
    .write = NULL,
    .open = NULL,
    .close = NULL,
    .readdir = iso9660_readdir,
    .finddir = iso9660_finddir,
    .create = NULL,
    .ioctl = NULL,
    .mmap = NULL
};

vfs_node_t *iso9660_mount(block_device_t dev) {
    uint8_t sector_buf[ISO_SECTOR_SIZE];

    if (iso9660_read_sector(dev, ISO_PVD_LBA, sector_buf) != 0) {
        return NULL;
    }

    iso9660_pvd_t *pvd = (iso9660_pvd_t *)sector_buf;
    if (pvd->type != 0x01 || memcmp(pvd->id, "CD001", 5) != 0) {
        return NULL;
    }

    iso_vol.dev = dev;
    iso_vol.root_extent_lba = pvd->root_directory_record.extent_lba_lsb;
    iso_vol.root_data_length = pvd->root_directory_record.data_length_lsb;

    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!root) return NULL;

    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->permissions = 0755;
    root->length = iso_vol.root_data_length;
    root->ptr = (vfs_node_t *)(uintptr_t)iso_vol.root_extent_lba;
    root->ops = &iso_fops;

    serial_puts(COM1, "[ISO9660] Volume successfully mounted.\n");
    return root;
}

vfs_node_t *iso9660_mount_boot_drive(void) {
    static const struct { uint16_t io; uint16_t ctrl; uint8_t drive; } ports[4] = {
        { 0x170, 0x376, 0xA0 },
        { 0x170, 0x376, 0xB0 },
        { 0x1F0, 0x3F6, 0xB0 },
        { 0x1F0, 0x3F6, 0xA0 }
    };

    for (int i = 0; i < 4; i++) {
        uint16_t io = ports[i].io;
        uint8_t drive = ports[i].drive;

        outb(io + 6, drive);
        for (volatile int d = 0; d < 1000; d++);
        if (inb(io + 7) == 0xFF) continue;

        outb(io + 7, 0xEC);
        for (volatile int d = 0; d < 1000; d++);

        uint8_t cl = inb(io + 4);
        uint8_t ch = inb(io + 5);

        if ((cl == 0x14 && ch == 0xEB) || (cl == 0x69 && ch == 0x96)) {
            g_atapi_io = io;
            g_atapi_ctrl = ports[i].ctrl;
            g_atapi_drive = drive;

            block_device_t dev;
            memset(&dev, 0, sizeof(block_device_t));
            dev.sector_size = 2048;
            dev.read = atapi_read_blocks;

            vfs_node_t *root = iso9660_mount(dev);
            if (root) return root;
        }
    }
    return NULL;
}

void iso9660_init(void) {
    serial_puts(COM1, "[ISO9660] Initializing ISO9660 Filesystem Engine...\n");
}

static int __init iso9660_fs_initcall(void) {
    iso9660_init();
    return 0;
}
fs_initcall(iso9660_fs_initcall);