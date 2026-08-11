// fat32.c - FAT32 File System Driver Implementation
#include "fat32.h"
#include "mbr.h"
#include "../drivers/disk/ata.h"
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

// Forward declarations
static int64_t fat32_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static int64_t fat32_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static vfs_node_t *fat32_readdir(vfs_node_t *node, uint32_t index);
static vfs_node_t *fat32_finddir(vfs_node_t *node, const char *name);
static vfs_node_t *fat32_create(vfs_node_t *dir, const char *name, uint32_t flags);

static vfs_file_operations_t fat32_fops = {
    .read = fat32_read,
    .write = fat32_write,
    .open = NULL,
    .close = NULL,
    .readdir = fat32_readdir,
    .finddir = fat32_finddir,
    .create = fat32_create
};

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
    return next_cluster;
}

// Set next cluster entry in FAT table
static void fat32_set_next_cluster(uint32_t cluster, uint32_t next_cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = current_vol.first_fat_sector + (fat_offset / current_vol.bytes_per_sector);
    uint32_t ent_offset = fat_offset % current_vol.bytes_per_sector;

    uint8_t sector_buf[512];
    for (uint32_t f = 0; f < current_vol.num_fats; f++) {
        uint32_t current_fat_lba = current_vol.partition_lba + fat_sector + (f * current_vol.fat_size_sectors);
        read_sectors_ata_pio((uintptr_t)sector_buf, current_fat_lba, 1);
        
        uint32_t *ent = (uint32_t *)&sector_buf[ent_offset];
        *ent = (*ent & 0xF0000000) | (next_cluster & 0x0FFFFFFF);
        
        write_sectors_ata_pio((uintptr_t)sector_buf, current_fat_lba, 1);
    }
}

// Allocate a new free cluster
static uint32_t fat32_alloc_cluster(void) {
    uint32_t total_fat_entries = (current_vol.fat_size_sectors * current_vol.bytes_per_sector) / 4;
    uint8_t sector_buf[512];

    for (uint32_t cluster = 2; cluster < total_fat_entries; cluster++) {
        uint32_t fat_offset = cluster * 4;
        uint32_t fat_sector = current_vol.first_fat_sector + (fat_offset / current_vol.bytes_per_sector);
        uint32_t ent_offset = fat_offset % current_vol.bytes_per_sector;

        read_sectors_ata_pio((uintptr_t)sector_buf, current_vol.partition_lba + fat_sector, 1);
        uint32_t entry = (*(uint32_t *)&sector_buf[ent_offset]) & 0x0FFFFFFF;

        if (entry == 0x00000000) {
            fat32_set_next_cluster(cluster, 0x0FFFFFF8);
            return cluster;
        }
    }
    return 0; // Disk full
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

// Case-insensitive 8.3 filename matcher
static int fat32_name_match(const char *fat_name, const char *target) {
    char upper_target[13];
    int i = 0;
    while (target[i] && i < 12) {
        char c = target[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        upper_target[i] = c;
        i++;
    }
    upper_target[i] = '\0';

    return strcmp(fat_name, upper_target) == 0;
}

// Helper to update directory entry size and cluster on disk
static void fat32_update_dir_entry(uint32_t parent_cluster, const char *filename, uint32_t first_cluster, uint32_t file_size) {
    uint32_t cluster = parent_cluster;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        read_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5 || (entries[i].attribute & FAT32_ATTR_LONG_NAME) == FAT32_ATTR_LONG_NAME) continue;

            char fmt[13];
            fat32_format_filename(&entries[i], fmt);
            if (fat32_name_match(fmt, filename)) {
                entries[i].first_cluster_high = (first_cluster >> 16) & 0xFFFF;
                entries[i].first_cluster_low = first_cluster & 0xFFFF;
                entries[i].file_size = file_size;
                write_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);
                kfree(cluster_buf);
                return;
            }
        }
        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
    }
    kfree(cluster_buf);
}

// VFS Read Operation for FAT32 files
static int64_t fat32_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || offset >= node->length) return 0;

    uint32_t cluster = (uint32_t)(uintptr_t)node->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;

    uint64_t bytes_skipped = 0;
    while (offset >= cluster_size && cluster < 0x0FFFFFF8) {
        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
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

        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
    }

    kfree(cluster_buf);
    return bytes_read;
}

// VFS Write Operation for FAT32 files
static int64_t fat32_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node) return -1;

    uint32_t cluster = (uint32_t)(uintptr_t)node->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;

    if (cluster == 0) {
        cluster = fat32_alloc_cluster();
        if (cluster == 0) return -1;
        fat32_set_next_cluster(cluster, 0x0FFFFFF8);
        node->ptr = (vfs_node_t *)(uintptr_t)cluster;
        node->inode = cluster;
    }

    uint64_t bytes_written = 0;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return -1;

    uint64_t curr_off = 0;
    uint32_t current_cluster = cluster;

    while (curr_off + cluster_size <= offset) {
        uint32_t next = fat32_get_next_cluster(current_cluster) & 0x0FFFFFFF;
        if (next >= 0x0FFFFFF8) {
            uint32_t new_c = fat32_alloc_cluster();
            if (new_c == 0) {
                kfree(cluster_buf);
                return -1;
            }
            fat32_set_next_cluster(current_cluster, new_c);
            fat32_set_next_cluster(new_c, 0x0FFFFFF8);
            next = new_c;
        }
        current_cluster = next;
        curr_off += cluster_size;
    }

    while (bytes_written < size) {
        uint32_t lba = fat32_cluster_to_lba(current_cluster);
        read_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);

        uint64_t chunk_off = (offset + bytes_written) % cluster_size;
        uint64_t chunk_size = cluster_size - chunk_off;
        if (chunk_size > size - bytes_written) {
            chunk_size = size - bytes_written;
        }

        memcpy(cluster_buf + chunk_off, buffer + bytes_written, chunk_size);
        write_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);

        bytes_written += chunk_size;

        if (bytes_written < size) {
            uint32_t next = fat32_get_next_cluster(current_cluster) & 0x0FFFFFFF;
            if (next >= 0x0FFFFFF8) {
                uint32_t new_c = fat32_alloc_cluster();
                if (new_c == 0) break;
                fat32_set_next_cluster(current_cluster, new_c);
                fat32_set_next_cluster(new_c, 0x0FFFFFF8);
                next = new_c;
            }
            current_cluster = next;
        }
    }

    kfree(cluster_buf);

    if (offset + bytes_written > node->length) {
        node->length = offset + bytes_written;
    }

    if (node->parent) {
        uint32_t parent_cluster = (uint32_t)(uintptr_t)node->parent->ptr;
        fat32_update_dir_entry(parent_cluster, node->name, cluster, node->length);
    }

    return bytes_written;
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
                return NULL;
            }
            if ((uint8_t)entries[i].name[0] == 0xE5 || (entries[i].attribute & FAT32_ATTR_LONG_NAME) == FAT32_ATTR_LONG_NAME) {
                continue;
            }

            if (current_index == index) {
                vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                fat32_format_filename(&entries[i], vnode->name);
                vnode->length = entries[i].file_size;
                vnode->inode = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                vnode->ptr = (vfs_node_t *)(uintptr_t)vnode->inode;
                vnode->parent = node;

                if (entries[i].attribute & FAT32_ATTR_DIRECTORY) {
                    vnode->flags = FS_DIRECTORY;
                } else {
                    vnode->flags = FS_FILE;
                }

                vnode->ops = &fat32_fops;

                kfree(cluster_buf);
                return vnode;
            }
            current_index++;
        }
        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
    }

    kfree(cluster_buf);
    return NULL;
}

// VFS Find Directory Operation
static vfs_node_t *fat32_finddir(vfs_node_t *node, const char *name) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;

    uint32_t cluster = (uint32_t)(uintptr_t)node->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return NULL;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        read_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(cluster_buf);
                return NULL;
            }
            if ((uint8_t)entries[i].name[0] == 0xE5 || (entries[i].attribute & FAT32_ATTR_LONG_NAME) == FAT32_ATTR_LONG_NAME) {
                continue;
            }

            char formatted_name[13];
            fat32_format_filename(&entries[i], formatted_name);

            if (fat32_name_match(formatted_name, name)) {
                vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                strcpy(vnode->name, formatted_name);
                vnode->length = entries[i].file_size;
                vnode->inode = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                vnode->ptr = (vfs_node_t *)(uintptr_t)vnode->inode;
                vnode->parent = node;

                if (entries[i].attribute & FAT32_ATTR_DIRECTORY) {
                    vnode->flags = FS_DIRECTORY;
                } else {
                    vnode->flags = FS_FILE;
                }

                vnode->ops = &fat32_fops;

                kfree(cluster_buf);
                return vnode;
            }
        }
        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
    }

    kfree(cluster_buf);
    return NULL;
}

// VFS Create Operation
static vfs_node_t *fat32_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    (void)flags;
    uint32_t parent_cluster = (uint32_t)(uintptr_t)dir->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return NULL;

    char short_name[8], short_ext[3];
    memset(short_name, ' ', 8);
    memset(short_ext, ' ', 3);

    const char *dot = strchr(name, '.');
    size_t name_len = dot ? (size_t)(dot - name) : strlen(name);
    if (name_len > 8) name_len = 8;

    for (size_t i = 0; i < name_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        short_name[i] = c;
    }
    if (dot) {
        size_t ext_len = strlen(dot + 1);
        if (ext_len > 3) ext_len = 3;
        for (size_t i = 0; i < ext_len; i++) {
            char c = dot[1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            short_ext[i] = c;
        }
    }

    uint32_t curr_cluster = parent_cluster;
    fat32_dir_entry_t *target_entry = NULL;
    uint32_t target_lba = 0;

    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(curr_cluster);
        read_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);
        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
                target_entry = &entries[i];
                target_lba = lba;
                break;
            }
        }
        if (target_entry) break;

        uint32_t next = fat32_get_next_cluster(curr_cluster) & 0x0FFFFFFF;
        if (next >= 0x0FFFFFF8) {
            uint32_t new_dir_cluster = fat32_alloc_cluster();
            if (new_dir_cluster == 0) {
                kfree(cluster_buf);
                return NULL;
            }
            fat32_set_next_cluster(curr_cluster, new_dir_cluster);
            fat32_set_next_cluster(new_dir_cluster, 0x0FFFFFF8);
            curr_cluster = new_dir_cluster;

            memset(cluster_buf, 0, cluster_size);
            lba = fat32_cluster_to_lba(new_dir_cluster);
            write_sectors_ata_pio((uintptr_t)cluster_buf, lba, current_vol.sectors_per_cluster);
            target_entry = &((fat32_dir_entry_t *)cluster_buf)[0];
            target_lba = lba;
            break;
        }
        curr_cluster = next;
    }

    if (!target_entry) {
        kfree(cluster_buf);
        return NULL;
    }

    memset(target_entry, 0, sizeof(fat32_dir_entry_t));
    memcpy(target_entry->name, short_name, 8);
    memcpy(target_entry->ext, short_ext, 3);
    target_entry->attribute = FAT32_ATTR_ARCHIVE;
    target_entry->first_cluster_high = 0;
    target_entry->first_cluster_low = 0;
    target_entry->file_size = 0;

    write_sectors_ata_pio((uintptr_t)cluster_buf, target_lba, current_vol.sectors_per_cluster);
    kfree(cluster_buf);

    vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strncpy(vnode->name, name, sizeof(vnode->name) - 1);
    vnode->flags = FS_FILE;
    vnode->length = 0;
    vnode->inode = 0;
    vnode->ptr = (vfs_node_t *)0;
    vnode->parent = dir;
    vnode->ops = &fat32_fops;

    return vnode;
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

    // Check FAT[1] dirty / clean shutdown bit (bit 31)
    uint32_t fat1_entry = fat32_get_next_cluster(1);
    if (!(fat1_entry & 0x80000000)) {
        serial_puts(COM1, "[FAT32 WARNING] Volume was NOT cleanly unmounted (DIRTY/IN-USE by another OS)!\n");
    } else {
        serial_puts(COM1, "[FAT32] Volume clean status check PASSED.\n");
    }

    // Mark FAT32 active / dirty on mount (clear bit 31 of FAT[1])
    uint32_t dirty_fat1 = fat1_entry & ~0x80000000;
    fat32_set_next_cluster(1, dirty_fat1);

    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->length = 0;
    root->ptr = (vfs_node_t *)(uintptr_t)current_vol.root_cluster;
    root->ops = &fat32_fops;

    return root;
}

void fat32_init(void) {
    serial_puts(COM1, "[DEBUG-FAT32] Initializing FAT32 subsystem...\n");
    
    if (!vfs_root) {
        serial_puts(COM1, "[DEBUG-FAT32 ERROR] VFS Root is NULL! Cannot mount FAT32.\n");
        return;
    }

    int p_count = disk_get_partition_count(); // Using unified partition manager!
    char buf[32];
    serial_puts(COM1, "[DEBUG-FAT32] Total unified partitions available for FAT32 check: ");
    itoa(p_count, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    for (int i = 0; i < p_count; i++) {
        partition_info_t *part = disk_get_partition(i);
        if (part) {
            serial_puts(COM1, "[DEBUG-FAT32] Inspecting Partition #");
            itoa(i, 10, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, " | Start LBA: ");
            itoa((int64_t)part->start_lba, 10, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, " | Type: 0x");
            itoa_hex(part->type, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, "\n");

            // Check if partition is FAT32 (Type 0x0B, 0x0C, or EFI System Partition on GPT: C12A7328-F81F-11D2-BA4B-00A0C93EC93B)
            // For universal GPT support, EFI System Partition (ESP) is also FAT32!
            if (part->type == 0x0B || part->type == 0x0C || part->type == 0x83 /* or check GPT ESP guid type */) {
                serial_puts(COM1, "[DEBUG-FAT32] CANDIDATE MATCHED! Attempting to mount FAT32 at LBA: ");
                itoa((int64_t)part->start_lba, 10, buf);
                serial_puts(COM1, buf);
                serial_puts(COM1, "\n");
                
                vfs_node_t *fat_root = fat32_mount_partition(part->start_lba);
                if (fat_root) {
                    vfs_node_t *disk_dir = ramfs_create_directory(vfs_root, "disk");
                    if (disk_dir) {
                        disk_dir->flags |= FS_MOUNTPOINT;
                        disk_dir->ptr = (vfs_node_t *)fat_root;
                        serial_puts(COM1, "[DEBUG-FAT32] SUCCESS: FAT32 successfully mounted at /disk!\n");
                        return; // Successfully mounted first valid boot/fat32 partition
                    }
                }
            }
        }
    }
    serial_puts(COM1, "[DEBUG-FAT32 WARNING] No valid FAT32 / ESP partition found to mount.\n");
}