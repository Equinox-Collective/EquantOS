// fat32.c - FAT32 File System Driver Implementation
#include "fat32.h"
#include "mbr.h"
#include "../drivers/ata/ata.h"
#include "ramfs.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "../drivers/serial/serial.h"

// Internal FAT32 volume context structure
typedef struct {
    uint32_t partition_lba;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t first_fat_sector;
    uint32_t first_data_sector;
} fat32_volume_t;

static fat32_volume_t current_vol;

// Convert cluster number to absolute LBA sector
static uint32_t fat32_cluster_to_lba(uint32_t cluster) {
    return current_vol.partition_lba + current_vol.first_data_sector + (cluster - 2) * current_vol.sectors_per_cluster;
}

// Read next cluster from FAT table
static uint32_t fat32_get_next_cluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = current_vol.first_fat_sector + (fat_offset / current_vol.bytes_per_sector);
    uint32_t ent_offset = fat_offset % current_vol.bytes_per_sector;

    uint8_t sector_buf[512];
    read_sectors_ata_pio((uintptr_t)sector_buf, current_vol.partition_lba + fat_sector, 1);

    uint32_t next_cluster = *(uint32_t *)&sector_buf[ent_offset];
    return next_cluster & 0x0FFFFFFF; // Mask top 4 bits
}

// VFS Read Operation for FAT32 files
static int64_t fat32_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || offset >= node->length) return 0;

    uint32_t cluster = (uint32_t)(uintptr_t)node->ptr; // Stored cluster in node->ptr
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;

    // Skip clusters up to offset
    uint64_t bytes_skipped = 0;
    while (offset >= cluster_size && cluster < 0x0FFFFFF8) {
        cluster = fat32_get_next_cluster(cluster);
        offset -= cluster_size;
        bytes_skipped += cluster_size;
    }

    uint64_t bytes_to_read = size;
    if (offset + size > node->length) {
        bytes_to_read = node->length - offset;
    }

    uint64_t bytes_read = 0;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return -1;

    while (bytes_read < bytes_to_read && cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        read_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);

        uint64_t chunk_offset = (bytes_read == 0) ? offset : 0;
        uint64_t chunk_size = cluster_size - chunk_offset;
        if (chunk_size > bytes_to_read - bytes_read) {
            chunk_size = bytes_to_read - bytes_read;
        }

        memcpy(buffer + bytes_read, cluster_buf + chunk_offset, chunk_size);
        bytes_read += chunk_size;

        cluster = fat32_get_next_cluster(cluster);
    }

    kfree(cluster_buf);
    return bytes_read;
}

// Helper to convert 8.3 filename to standard string
static void fat32_format_filename(fat32_dir_entry_t *entry, char *dest) {
    int k = 0;
    for (int i = 0; i < 8; i++) {
        if (entry->name[i] == ' ') break;
        dest[k++] = entry->name[i];
    }
    if (entry->ext[0] != ' ') {
        dest[k++] = '.';
        for (int i = 0; i < 3; i++) {
            if (entry->ext[i] == ' ') break;
            dest[k++] = entry->ext[i];
        }
    }
    dest[k] = '\0';
}

// VFS Readdir Operation
static vfs_node_t *fat32_readdir(vfs_node_t *node, uint32_t index) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;

    uint32_t cluster = (uint32_t)(uintptr_t)node->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    uint32_t current_index = 0;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return NULL;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        read_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(cluster_buf);
                return NULL; // End of directory
            }
            if (entries[i].name[0] == 0xE5 || (entries[i].attribute & FAT32_ATTR_LONG_NAME) == FAT32_ATTR_LONG_NAME) {
                continue; // Deleted or LFN entry
            }

            if (current_index == index) {
                vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                fat32_format_filename(&entries[i], vnode->name);
                vnode->length = entries[i].file_size;
                vnode->inode = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                vnode->ptr = (vfs_node_t *)(uintptr_t)vnode->inode;

                if (entries[i].attribute & FAT32_ATTR_DIRECTORY) {
                    vnode->flags = FS_DIRECTORY;
                } else {
                    vnode->flags = FS_FILE;
                }

                // Setup operations table
                static vfs_file_operations_t fat32_fops = {
                    .read = fat32_read,
                    .write = NULL,
                    .open = NULL,
                    .close = NULL,
                    .readdir = fat32_readdir,
                    .finddir = NULL
                };
                vnode->ops = &fat32_fops;

                kfree(cluster_buf);
                return vnode;
            }
            current_index++;
        }
        cluster = fat32_get_next_cluster(cluster);
    }

    kfree(cluster_buf);
    return NULL;
}

vfs_node_t *fat32_mount_partition(uint32_t partition_lba) {
    uint8_t sector_buf[512];
    read_sectors_ata_pio((uintptr_t)sector_buf, partition_lba, 1);

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    current_vol.partition_lba = partition_lba;
    current_vol.bytes_per_sector = bpb->bytes_per_sector;
    current_vol.sectors_per_cluster = bpb->sectors_per_cluster;
    current_vol.reserved_sectors = bpb->reserved_sectors;
    current_vol.num_fats = bpb->num_fats;
    current_vol.fat_size_sectors = bpb->table_size_32;
    current_vol.root_cluster = bpb->root_cluster;

    current_vol.first_fat_sector = current_vol.reserved_sectors;
    current_vol.first_data_sector = current_vol.reserved_sectors + (current_vol.num_fats * current_vol.fat_size_sectors);

    serial_puts(COM1, "[FAT32] Volume mounted successfully!\n");

    // Create Root VFS Node for FAT32
    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->length = 0;
    root->ptr = (vfs_node_t *)(uintptr_t)current_vol.root_cluster;

    static vfs_file_operations_t fat32_root_fops = {
        .read = fat32_read,
        .write = NULL,
        .open = NULL,
        .close = NULL,
        .readdir = fat32_readdir,
        .finddir = NULL
    };
    root->ops = &fat32_root_fops;

    return root;
}

void fat32_init(void) {
    serial_puts(COM1, "[FAT32] Initializing FAT32 file system driver...\n");
    
    if (!vfs_root) {
        serial_puts(COM1, "[FAT32 ERROR] VFS Root is NULL! Cannot mount FAT32.\n");
        return;
    }

    int p_count = mbr_get_partition_count();
    if (p_count > 0) {
        partition_info_t *part = mbr_get_partition(0);
        if (part && (part->type == 0x0B || part->type == 0x0C)) {
            serial_puts(COM1, "[FAT32] Found FAT32 partition in MBR. Mounting at /disk...\n");
            
            vfs_node_t *fat_root = fat32_mount_partition(part->start_lba);
            if (!fat_root) {
                serial_puts(COM1, "[FAT32 ERROR] Failed to mount FAT32 partition!\n");
                return;
            }
            
            // DEBUG LOG: Print fat_root address
            char dbg[32];
            serial_puts(COM1, "[FAT32 DEBUG] fat_root ptr = 0x");
            itoa_hex((uint64_t)fat_root, dbg);
            serial_puts(COM1, dbg);
            serial_puts(COM1, "\n");

            // Create /disk directory in RAMFS root safely
            vfs_node_t *disk_dir = ramfs_create_directory(vfs_root, "disk");
            if (!disk_dir) {
                serial_puts(COM1, "[FAT32 ERROR] Failed to allocate /disk directory node in RAMFS!\n");
                return;
            }

            // DEBUG LOG: Print disk_dir address
            serial_puts(COM1, "[FAT32 DEBUG] disk_dir ptr = 0x");
            itoa_hex((uint64_t)disk_dir, dbg);
            serial_puts(COM1, dbg);
            serial_puts(COM1, "\n");

            disk_dir->flags |= FS_MOUNTPOINT;
            
            // SAFE ASSIGNMENT WITH EXPLICIT CAST
            disk_dir->ptr = (vfs_node_t *)fat_root;
            
            serial_puts(COM1, "[FAT32] FAT32 successfully mounted at /disk!\n");
        } else {
            serial_puts(COM1, "[FAT32] No FAT32 partition found in MBR.\n");
        }
    } else {
        serial_puts(COM1, "[FAT32] No MBR partitions detected.\n");
    }
}