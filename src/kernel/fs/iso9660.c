// src/kernel/fs/iso9660.c - Production ISO9660 Filesystem Implementation
#include "iso9660.h"
#include "../core/mem/memory.h"
#include "../libs/string.h"
#include "../libs/stdio.h"
#include "../drivers/serial/serial.h"
#include "../core/initcall.h"
#include "ramfs.h"

typedef struct {
    block_device_t dev;
    uint32_t root_extent_lba;
    uint32_t root_data_length;
} iso9660_vol_t;

static iso9660_vol_t iso_vol;

// Helper: Read a 2048-byte ISO sector translating to 512-byte device LBAs if necessary
static int iso9660_read_sector(block_device_t dev, uint32_t iso_sector, void *buf) {
    if (dev.sector_size == 2048) {
        return dev.read(iso_sector, 1, buf);
    } else if (dev.sector_size == 512) {
        return dev.read(iso_sector * 4, 4, buf);
    }
    return -1;
}

// Clean ISO filename by stripping version tag ';1' and converting to lowercase
static void iso9660_clean_name(const char *raw_name, uint8_t len, char *dest, size_t max_dest) {
    if (len == 1 && raw_name[0] == 0x00) {
        strcpy(dest, ".");
        return;
    }
    if (len == 1 && raw_name[0] == 0x01) {
        strcpy(dest, "..");
        return;
    }

    size_t out = 0;
    for (uint8_t i = 0; i < len && out < max_dest - 1; i++) {
        char c = raw_name[i];
        if (c == ';') break; // Strip ';1' version tag
        if (c >= 'A' && c <= 'Z') c += 32; // Convert to lowercase
        dest[out++] = c;
    }
    dest[out] = '\0';
}

static int64_t iso9660_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || offset >= node->length) return 0;

    uint32_t extent_lba = (uint32_t)(uintptr_t)node->ptr;
    uint64_t bytes_to_read = size;
    if (offset + size > node->length) {
        bytes_to_read = node->length - offset;
    }

    uint8_t *sector_buf = (uint8_t *)kmalloc(ISO_SECTOR_SIZE);
    if (!sector_buf) return -1;

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

    kfree(sector_buf);
    return (int64_t)bytes_read;
}

static vfs_node_t *iso9660_readdir(vfs_node_t *node, uint32_t index) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;

    uint32_t extent_lba = (uint32_t)(uintptr_t)node->ptr;
    uint32_t dir_len = (uint32_t)node->length;

    uint8_t *dir_buf = (uint8_t *)kmalloc(dir_len > ISO_SECTOR_SIZE ? dir_len : ISO_SECTOR_SIZE);
    if (!dir_buf) return NULL;

    uint32_t sectors = (dir_len + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    for (uint32_t s = 0; s < sectors; s++) {
        iso9660_read_sector(iso_vol.dev, extent_lba + s, dir_buf + (s * ISO_SECTOR_SIZE));
    }

    uint32_t pos = 0;
    uint32_t current_idx = 0;

    while (pos < dir_len) {
        iso9660_dir_record_t *rec = (iso9660_dir_record_t *)(dir_buf + pos);
        if (rec->length == 0) {
            pos = (pos + ISO_SECTOR_SIZE) & ~(ISO_SECTOR_SIZE - 1);
            continue;
        }

        char clean_name[128];
        iso9660_clean_name(rec->file_identifier, rec->name_len, clean_name, sizeof(clean_name));

        if (strcmp(clean_name, ".") != 0 && strcmp(clean_name, "..") != 0) {
            if (current_idx == index) {
                vfs_node_t *child = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                strcpy(child->name, clean_name);
                child->length = rec->data_length_lsb;
                child->inode = rec->extent_lba_lsb;
                child->ptr = (vfs_node_t *)(uintptr_t)rec->extent_lba_lsb;
                child->flags = (rec->flags & 0x02) ? FS_DIRECTORY : FS_FILE;

                static vfs_file_operations_t iso_fops;
                iso_fops.read = iso9660_read;
                iso_fops.readdir = iso9660_readdir;
                iso_fops.finddir = NULL;
                child->ops = &iso_fops;

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
    .finddir = NULL,
    .create = NULL
};

vfs_node_t *iso9660_mount(block_device_t dev) {
    uint8_t *sector_buf = (uint8_t *)kmalloc(ISO_SECTOR_SIZE);
    if (!sector_buf) return NULL;

    if (iso9660_read_sector(dev, ISO_PVD_LBA, sector_buf) != 0) {
        kfree(sector_buf);
        return NULL;
    }

    iso9660_pvd_t *pvd = (iso9660_pvd_t *)sector_buf;
    if (pvd->type != 0x01 || memcmp(pvd->id, "CD001", 5) != 0) {
        kfree(sector_buf);
        return NULL; // Not a valid ISO9660 Primary Volume Descriptor
    }

    iso_vol.dev = dev;
    iso_vol.root_extent_lba = pvd->root_directory_record.extent_lba_lsb;
    iso_vol.root_data_length = pvd->root_directory_record.data_length_lsb;

    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->length = iso_vol.root_data_length;
    root->ptr = (vfs_node_t *)(uintptr_t)iso_vol.root_extent_lba;
    root->ops = &iso_fops;

    kfree(sector_buf);
    serial_puts(COM1, "[ISO9660] Volume successfully mounted.\n");
    return root;
}

void iso9660_init(void) {
    serial_puts(COM1, "[ISO9660] Initializing ISO9660 Filesystem Engine...\n");
}

static int __init iso9660_fs_initcall(void) {
    iso9660_init();
    return 0;
}
fs_initcall(iso9660_fs_initcall);