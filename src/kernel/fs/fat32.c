// src/kernel/fs/fat32.c - Production FAT32 with Full VFAT Long File Name (LFN) Engine
#include "fat32.h"
#include "mbr.h"
#include "gpt.h"
#include "../drivers/disk/block.h"
#include "../drivers/disk/nvme.h"
#include "ramfs.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "../drivers/serial/serial.h"
#include "../core/initcall.h"
#include "ext2.h"

typedef struct {
    uint32_t lead_sig;      // 0x41615252 ("RRaA")
    uint8_t  reserved1[480];
    uint32_t struct_sig;    // 0x61417272 ("rrAa")
    uint32_t free_count;    // 0xFFFFFFFF
    uint32_t next_free;     // 0xFFFFFFFF (or 3)
    uint8_t  reserved2[12];
    uint32_t trail_sig;     // 0xAA550000
} __attribute__((packed)) fat32_fsinfo_t;

typedef struct {
    block_device_t dev;
    uint32_t partition_lba;
    uint32_t partition_sectors;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t first_fat_sector;
    uint32_t first_data_sector;
    uint32_t total_clusters;
} fat32_volume_t;

static fat32_volume_t current_vol;

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

// Calculate standard 8.3 checksum for LFN entries
static uint8_t fat32_calc_lfn_checksum(const uint8_t *short_name_11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name_11[i]);
    }
    return sum;
}

// Generate short 8.3 name alias from long filename
static void fat32_gen_short_alias(const char *long_name, char short_name[8], char short_ext[3]) {
    memset(short_name, ' ', 8);
    memset(short_ext, ' ', 3);

    const char *last_dot = strrchr(long_name, '.');
    size_t name_part_len = last_dot ? (size_t)(last_dot - long_name) : strlen(long_name);

    if (name_part_len == 0 && last_dot) {
        // Dotfile (e.g. ".bashrc")
        name_part_len = strlen(long_name);
        last_dot = NULL;
    }

    size_t k = 0;
    for (size_t i = 0; i < name_part_len && k < 6; i++) {
        char c = long_name[i];
        if (c == '.' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        short_name[k++] = c;
    }

    short_name[k++] = '~';
    short_name[k++] = '1';

    if (last_dot) {
        size_t elen = strlen(last_dot + 1);
        if (elen > 3) elen = 3;
        for (size_t i = 0; i < elen; i++) {
            char c = last_dot[1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            short_ext[i] = c;
        }
    }
}

static uint32_t fat32_cluster_to_lba(uint32_t cluster) {
    return current_vol.partition_lba + current_vol.first_data_sector + (cluster - 2) * current_vol.sectors_per_cluster;
}

static uint32_t fat32_get_next_cluster(uint32_t cluster) {
    if (cluster < 2 || cluster >= current_vol.total_clusters + 2) {
        return 0x0FFFFFF8;
    }

    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = current_vol.first_fat_sector + (fat_offset / current_vol.bytes_per_sector);
    uint32_t ent_offset = fat_offset % current_vol.bytes_per_sector;

    uint8_t sector_buf[512];
    current_vol.dev.read(current_vol.partition_lba + fat_sector, 1, sector_buf);

    return *(uint32_t *)&sector_buf[ent_offset] & 0x0FFFFFFF;
}

static void fat32_set_next_cluster(uint32_t cluster, uint32_t next_cluster) {
    if (cluster < 2 || cluster >= current_vol.total_clusters + 2) {
        return;
    }

    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = current_vol.first_fat_sector + (fat_offset / current_vol.bytes_per_sector);
    uint32_t ent_offset = fat_offset % current_vol.bytes_per_sector;

    uint8_t sector_buf[512];
    for (uint32_t f = 0; f < current_vol.num_fats; f++) {
        uint32_t current_fat_lba = current_vol.partition_lba + fat_sector + (f * current_vol.fat_size_sectors);
        current_vol.dev.read(current_fat_lba, 1, sector_buf);
        
        uint32_t *ent = (uint32_t *)&sector_buf[ent_offset];
        *ent = (*ent & 0xF0000000) | (next_cluster & 0x0FFFFFFF);
        
        current_vol.dev.write(current_fat_lba, 1, sector_buf);
    }
}

static uint32_t fat32_alloc_cluster(void) {
    uint8_t sector_buf[512];

    for (uint32_t cluster = 2; cluster < current_vol.total_clusters + 2; cluster++) {
        uint32_t fat_offset = cluster * 4;
        uint32_t fat_sector = current_vol.first_fat_sector + (fat_offset / current_vol.bytes_per_sector);
        uint32_t ent_offset = fat_offset % current_vol.bytes_per_sector;

        current_vol.dev.read(current_vol.partition_lba + fat_sector, 1, sector_buf);
        uint32_t entry = (*(uint32_t *)&sector_buf[ent_offset]) & 0x0FFFFFFF;

        if (entry == 0x00000000) {
            fat32_set_next_cluster(cluster, 0x0FFFFFF8);
            return cluster;
        }
    }
    return 0;
}

static void fat32_format_short_name(fat32_dir_entry_t *entry, char *dest) {
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

static void fat32_update_dir_entry(uint32_t parent_cluster, const char *filename, uint32_t first_cluster, uint32_t file_size) {
    uint32_t cluster = parent_cluster;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return;

    char lfn_acc[260] = {0};

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        current_vol.dev.read(lba, current_vol.sectors_per_cluster, cluster_buf);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;

            if (entries[i].attribute == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&entries[i];
                uint8_t seq = lfn->order & 0x1F;
                if (seq > 0 && seq <= 20) {
                    size_t char_idx = (seq - 1) * 13;
                    for (int c = 0; c < 5; c++) {
                        if (lfn->name1[c] == 0 || lfn->name1[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name1[c] & 0xFF);
                    }
                    for (int c = 0; c < 6; c++) {
                        if (lfn->name2[c] == 0 || lfn->name2[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name2[c] & 0xFF);
                    }
                    for (int c = 0; c < 2; c++) {
                        if (lfn->name3[c] == 0 || lfn->name3[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name3[c] & 0xFF);
                    }
                }
                continue;
            }

            // Short entry reached
            char entry_name[260];
            if (lfn_acc[0] != '\0') {
                strcpy(entry_name, lfn_acc);
            } else {
                fat32_format_short_name(&entries[i], entry_name);
            }

            if (strcmp(entry_name, filename) == 0) {
                entries[i].first_cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                entries[i].first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
                entries[i].file_size = file_size;
                current_vol.dev.write(lba, current_vol.sectors_per_cluster, cluster_buf);
                kfree(cluster_buf);
                return;
            }

            memset(lfn_acc, 0, sizeof(lfn_acc));
        }
        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
    }
    kfree(cluster_buf);
}

static int64_t fat32_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    if (!node || offset >= node->length) return 0;

    uint32_t cluster = (uint32_t)(uintptr_t)node->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;

    while (offset >= cluster_size && cluster < 0x0FFFFFF8) {
        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
        offset -= cluster_size;
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
        current_vol.dev.read(lba, current_vol.sectors_per_cluster, cluster_buf);

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
    return (int64_t)bytes_read;
}

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
        current_vol.dev.read(lba, current_vol.sectors_per_cluster, cluster_buf);

        uint64_t chunk_off = (offset + bytes_written) % cluster_size;
        uint64_t chunk_size = cluster_size - chunk_off;
        if (chunk_size > size - bytes_written) {
            chunk_size = size - bytes_written;
        }

        memcpy(cluster_buf + chunk_off, buffer + bytes_written, chunk_size);
        current_vol.dev.write(lba, current_vol.sectors_per_cluster, cluster_buf);

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
        fat32_update_dir_entry(parent_cluster, node->name, cluster, (uint32_t)node->length);
    }

    return (int64_t)bytes_written;
}

static vfs_node_t *fat32_readdir(vfs_node_t *node, uint32_t index) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;

    uint32_t cluster = (uint32_t)(uintptr_t)node->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    uint32_t current_index = 0;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return NULL;

    char lfn_acc[260] = {0};

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        current_vol.dev.read(lba, current_vol.sectors_per_cluster, cluster_buf);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(cluster_buf);
                return NULL;
            }
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;

            if (entries[i].attribute == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&entries[i];
                uint8_t seq = lfn->order & 0x1F;
                if (seq > 0 && seq <= 20) {
                    size_t char_idx = (seq - 1) * 13;
                    for (int c = 0; c < 5; c++) {
                        if (lfn->name1[c] == 0 || lfn->name1[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name1[c] & 0xFF);
                    }
                    for (int c = 0; c < 6; c++) {
                        if (lfn->name2[c] == 0 || lfn->name2[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name2[c] & 0xFF);
                    }
                    for (int c = 0; c < 2; c++) {
                        if (lfn->name3[c] == 0 || lfn->name3[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name3[c] & 0xFF);
                    }
                }
                continue;
            }

            char entry_name[260];
            if (lfn_acc[0] != '\0') {
                strcpy(entry_name, lfn_acc);
            } else {
                fat32_format_short_name(&entries[i], entry_name);
            }

            if (current_index == index) {
                vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                strcpy(vnode->name, entry_name);
                vnode->length = entries[i].file_size;
                vnode->inode = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                vnode->ptr = (vfs_node_t *)(uintptr_t)vnode->inode;
                vnode->parent = node;
                vnode->flags = (entries[i].attribute & FAT32_ATTR_DIRECTORY) ? FS_DIRECTORY : FS_FILE;
                vnode->ops = &fat32_fops;

                kfree(cluster_buf);
                return vnode;
            }
            current_index++;
            memset(lfn_acc, 0, sizeof(lfn_acc));
        }
        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
    }

    kfree(cluster_buf);
    return NULL;
}

static vfs_node_t *fat32_finddir(vfs_node_t *node, const char *name) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;

    uint32_t cluster = (uint32_t)(uintptr_t)node->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return NULL;

    char lfn_acc[260] = {0};

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        current_vol.dev.read(lba, current_vol.sectors_per_cluster, cluster_buf);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(cluster_buf);
                return NULL;
            }
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;

            if (entries[i].attribute == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&entries[i];
                uint8_t seq = lfn->order & 0x1F;
                if (seq > 0 && seq <= 20) {
                    size_t char_idx = (seq - 1) * 13;
                    for (int c = 0; c < 5; c++) {
                        if (lfn->name1[c] == 0 || lfn->name1[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name1[c] & 0xFF);
                    }
                    for (int c = 0; c < 6; c++) {
                        if (lfn->name2[c] == 0 || lfn->name2[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name2[c] & 0xFF);
                    }
                    for (int c = 0; c < 2; c++) {
                        if (lfn->name3[c] == 0 || lfn->name3[c] == 0xFFFF) break;
                        lfn_acc[char_idx++] = (char)(lfn->name3[c] & 0xFF);
                    }
                }
                continue;
            }

            char entry_name[260];
            if (lfn_acc[0] != '\0') {
                strcpy(entry_name, lfn_acc);
            } else {
                fat32_format_short_name(&entries[i], entry_name);
            }

            if (strcmp(entry_name, name) == 0) {
                vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                strcpy(vnode->name, entry_name);
                vnode->length = entries[i].file_size;
                vnode->inode = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                vnode->ptr = (vfs_node_t *)(uintptr_t)vnode->inode;
                vnode->parent = node;
                vnode->flags = (entries[i].attribute & FAT32_ATTR_DIRECTORY) ? FS_DIRECTORY : FS_FILE;
                vnode->ops = &fat32_fops;

                kfree(cluster_buf);
                return vnode;
            }
            memset(lfn_acc, 0, sizeof(lfn_acc));
        }
        cluster = fat32_get_next_cluster(cluster) & 0x0FFFFFFF;
    }

    kfree(cluster_buf);
    return NULL;
}

static vfs_node_t *fat32_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    uint32_t parent_cluster = (uint32_t)(uintptr_t)dir->ptr;
    uint32_t cluster_size = current_vol.sectors_per_cluster * current_vol.bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return NULL;

    size_t name_len = strlen(name);
    uint32_t num_lfn = (uint32_t)((name_len + 12) / 13);
    uint32_t slots_needed = num_lfn + 1; // LFN entries + 1 Short 8.3 entry

    char short_name[8], short_ext[3];
    fat32_gen_short_alias(name, short_name, short_ext);

    uint8_t short_11[11];
    memcpy(short_11, short_name, 8);
    memcpy(short_11 + 8, short_ext, 3);
    uint8_t checksum = fat32_calc_lfn_checksum(short_11);

    uint32_t curr_cluster = parent_cluster;
    uint32_t target_lba = 0;
    uint32_t start_slot_idx = 0;
    bool found_slots = false;

    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(curr_cluster);
        current_vol.dev.read(lba, current_vol.sectors_per_cluster, cluster_buf);
        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;

        for (uint32_t i = 0; i <= entries_per_cluster - slots_needed; i++) {
            bool consecutive = true;
            for (uint32_t j = 0; j < slots_needed; j++) {
                if (entries[i + j].name[0] != 0x00 && (uint8_t)entries[i + j].name[0] != 0xE5) {
                    consecutive = false;
                    i += j;
                    break;
                }
            }
            if (consecutive) {
                target_lba = lba;
                start_slot_idx = i;
                found_slots = true;
                break;
            }
        }
        if (found_slots) break;

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
            current_vol.dev.write(lba, current_vol.sectors_per_cluster, cluster_buf);
            target_lba = lba;
            start_slot_idx = 0;
            found_slots = true;
            break;
        }
        curr_cluster = next;
    }

    if (!found_slots) {
        kfree(cluster_buf);
        return NULL;
    }

    bool is_dir = (flags & FS_DIRECTORY) != 0;
    uint32_t initial_cluster = 0;

    if (is_dir) {
        initial_cluster = fat32_alloc_cluster();
        if (initial_cluster == 0) {
            kfree(cluster_buf);
            return NULL;
        }
        fat32_set_next_cluster(initial_cluster, 0x0FFFFFF8);

        uint8_t *dir_data = (uint8_t *)kzalloc(cluster_size);
        if (dir_data) {
            current_vol.dev.write(fat32_cluster_to_lba(initial_cluster), current_vol.sectors_per_cluster, dir_data);
            kfree(dir_data);
        }
    }

    // 1. Write VFAT LFN Entries in reverse order
    for (uint32_t seq = num_lfn; seq >= 1; seq--) {
        uint32_t lfn_idx = start_slot_idx + (num_lfn - seq);
        fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&((fat32_dir_entry_t *)cluster_buf)[lfn_idx];
        memset(lfn, 0xFF, sizeof(fat32_lfn_entry_t));

        lfn->order = (uint8_t)seq;
        if (seq == num_lfn) {
            lfn->order |= LFN_LAST_ENTRY_FLAG; // 0x40
        }
        lfn->attribute = FAT32_ATTR_LONG_NAME; // 0x0F
        lfn->type = 0x00;
        lfn->checksum = checksum;
        lfn->first_cluster = 0x0000;

        size_t str_pos = (seq - 1) * 13;

        // name1 (5 chars)
        for (int c = 0; c < 5; c++) {
            if (str_pos < name_len) lfn->name1[c] = (uint16_t)(uint8_t)name[str_pos++];
            else if (str_pos == name_len) { lfn->name1[c] = 0x0000; str_pos++; }
            else lfn->name1[c] = 0xFFFF;
        }
        // name2 (6 chars)
        for (int c = 0; c < 6; c++) {
            if (str_pos < name_len) lfn->name2[c] = (uint16_t)(uint8_t)name[str_pos++];
            else if (str_pos == name_len) { lfn->name2[c] = 0x0000; str_pos++; }
            else lfn->name2[c] = 0xFFFF;
        }
        // name3 (2 chars)
        for (int c = 0; c < 2; c++) {
            if (str_pos < name_len) lfn->name3[c] = (uint16_t)(uint8_t)name[str_pos++];
            else if (str_pos == name_len) { lfn->name3[c] = 0x0000; str_pos++; }
            else lfn->name3[c] = 0xFFFF;
        }
    }

    // 2. Write 8.3 Short Entry immediately after LFN chain
    uint32_t short_idx = start_slot_idx + num_lfn;
    fat32_dir_entry_t *target_entry = &((fat32_dir_entry_t *)cluster_buf)[short_idx];
    memset(target_entry, 0, sizeof(fat32_dir_entry_t));
    memcpy(target_entry->name, short_name, 8);
    memcpy(target_entry->ext, short_ext, 3);
    target_entry->attribute = is_dir ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE;
    target_entry->first_cluster_high = (uint16_t)((initial_cluster >> 16) & 0xFFFF);
    target_entry->first_cluster_low = (uint16_t)(initial_cluster & 0xFFFF);
    target_entry->file_size = 0;

    current_vol.dev.write(target_lba, current_vol.sectors_per_cluster, cluster_buf);
    kfree(cluster_buf);

    vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strncpy(vnode->name, name, sizeof(vnode->name) - 1);
    vnode->flags = is_dir ? FS_DIRECTORY : FS_FILE;
    vnode->length = 0;
    vnode->inode = initial_cluster;
    vnode->ptr = (vfs_node_t *)(uintptr_t)initial_cluster;
    vnode->parent = dir;
    vnode->ops = &fat32_fops;

    return vnode;
}

vfs_node_t *fat32_mount_partition(block_device_t dev, uint32_t partition_lba, uint32_t partition_sectors) {
    uint8_t sector_buf[512];
    if (dev.read(partition_lba, 1, sector_buf) != 0) return NULL;

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    if (bpb->bytes_per_sector != 512 && bpb->bytes_per_sector != 1024 &&
        bpb->bytes_per_sector != 2048 && bpb->bytes_per_sector != 4096) {
        return NULL;
    }

    if (bpb->sectors_per_cluster == 0 || (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) != 0) {
        return NULL;
    }

    if (bpb->reserved_sectors == 0 || bpb->num_fats == 0) {
        return NULL;
    }

    uint32_t total_sectors = bpb->total_sectors_16 != 0 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    if (total_sectors == 0) {
        total_sectors = partition_sectors;
    }

    uint32_t fat_size = bpb->fat_size_16 != 0 ? bpb->fat_size_16 : bpb->table_size_32;
    if (fat_size == 0 || bpb->root_cluster < 2) return NULL;

    uint32_t root_dir_sectors = ((bpb->root_entry_count * 32) + (bpb->bytes_per_sector - 1)) / bpb->bytes_per_sector;
    uint32_t data_sectors = total_sectors - (bpb->reserved_sectors + (bpb->num_fats * fat_size) + root_dir_sectors);
    uint32_t total_clusters = data_sectors / bpb->sectors_per_cluster;

    current_vol.dev = dev;
    current_vol.partition_lba = partition_lba;
    current_vol.partition_sectors = partition_sectors;
    current_vol.bytes_per_sector = bpb->bytes_per_sector;
    current_vol.sectors_per_cluster = bpb->sectors_per_cluster;
    current_vol.reserved_sectors = bpb->reserved_sectors;
    current_vol.num_fats = bpb->num_fats;
    current_vol.fat_size_sectors = fat_size;
    current_vol.root_cluster = bpb->root_cluster;

    current_vol.first_fat_sector = current_vol.reserved_sectors;
    current_vol.first_data_sector = current_vol.reserved_sectors + (current_vol.num_fats * current_vol.fat_size_sectors);
    current_vol.total_clusters = total_clusters;

    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->length = 0;
    root->ptr = (vfs_node_t *)(uintptr_t)current_vol.root_cluster;
    root->ops = &fat32_fops;

    return root;
}

int mkfs_fat32(block_device_t dev, uint32_t start_lba, uint32_t sector_count, const char *label) {
    if (!dev.write || sector_count < 65536) return -1;

    uint32_t bytes_per_sector = 512;
    uint32_t sectors_per_cluster = 1;
    uint16_t reserved_sectors = 32;
    uint8_t num_fats = 2;
    uint32_t root_cluster = 2;

    uint32_t total_clusters = (sector_count - reserved_sectors) / sectors_per_cluster;
    uint32_t fat_size_sectors = ((total_clusters * 4) + (bytes_per_sector - 1)) / bytes_per_sector;

    uint8_t *sec_buf = (uint8_t *)kzalloc(512);
    if (!sec_buf) return -1;

    // 1. Write Volume Boot Record (VBR / Sector 0)
    fat32_bpb_t *bpb = (fat32_bpb_t *)sec_buf;
    bpb->jmp[0] = 0xEB; bpb->jmp[1] = 0x58; bpb->jmp[2] = 0x90;
    memcpy(bpb->oem, "MSWIN4.1", 8);
    bpb->bytes_per_sector = (uint16_t)bytes_per_sector;
    bpb->sectors_per_cluster = (uint8_t)sectors_per_cluster;
    bpb->reserved_sectors = reserved_sectors;
    bpb->num_fats = num_fats;
    bpb->root_entry_count = 0;
    bpb->total_sectors_16 = 0;
    bpb->media_type = 0xF8;
    bpb->fat_size_16 = 0;
    bpb->sectors_per_track = 32;
    bpb->num_heads = 64;
    bpb->hidden_sectors = start_lba;
    bpb->total_sectors_32 = sector_count;
    bpb->table_size_32 = fat_size_sectors;
    bpb->ext_flags = 0;
    bpb->fs_version = 0;
    bpb->root_cluster = root_cluster;
    bpb->fs_info = 1;               // FSInfo at Sector 1
    bpb->backup_boot_sector = 6;    // Backup VBR at Sector 6
    bpb->drive_num = 0x80;
    bpb->boot_signature = 0x29;
    bpb->volume_id = 0x12345678;
    memcpy(bpb->volume_label, label ? label : "EFI SYSTEM ", 11);
    memcpy(bpb->file_system_type, "FAT32   ", 8);
    sec_buf[510] = 0x55;
    sec_buf[511] = 0xAA;

    dev.write(start_lba, 1, sec_buf);
    dev.write(start_lba + 6, 1, sec_buf); // Backup VBR

    // 2. Write Mandatory UEFI FSInfo Sector (Sector 1 & Backup Sector 7)
    memset(sec_buf, 0, 512);
    fat32_fsinfo_t *fsinfo = (fat32_fsinfo_t *)sec_buf;
    fsinfo->lead_sig = 0x41615252;   // "RRaA"
    fsinfo->struct_sig = 0x61417272; // "rrAa"
    fsinfo->free_count = 0xFFFFFFFF;
    fsinfo->next_free = 3;
    fsinfo->trail_sig = 0xAA550000;

    dev.write(start_lba + 1, 1, sec_buf);
    dev.write(start_lba + 7, 1, sec_buf); // Backup FSInfo

    // 3. Initialize FAT Tables (Cluster 0, 1, 2)
    memset(sec_buf, 0, 512);
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_start = start_lba + reserved_sectors + (f * fat_size_sectors);
        sec_buf[0] = 0xF8; sec_buf[1] = 0xFF; sec_buf[2] = 0xFF; sec_buf[3] = 0x0F;
        sec_buf[4] = 0xFF; sec_buf[5] = 0xFF; sec_buf[6] = 0xFF; sec_buf[7] = 0x0F;
        sec_buf[8] = 0xF8; sec_buf[9] = 0xFF; sec_buf[10] = 0xFF; sec_buf[11] = 0x0F;
        dev.write(fat_start, 1, sec_buf);

        memset(sec_buf, 0, 512);
        for (uint32_t s = 1; s < fat_size_sectors; s++) {
            dev.write(fat_start + s, 1, sec_buf);
        }
    }

    // 4. Clear Root Directory Cluster
    uint32_t first_data_sec = start_lba + reserved_sectors + (num_fats * fat_size_sectors);
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        dev.write(first_data_sec + s, 1, sec_buf);
    }

    kfree(sec_buf);
    return 0;
}

void fat32_init(void) {
    if (!vfs_root) return;

    serial_puts(COM1, "[FAT32] Initializing NVMe Hardware Controller...\n");
    if (nvme_init() != NVME_SUCCESS) {
        serial_puts(COM1, "[FAT32 WARNING] NVMe initialization failed or drive not found.\n");
        return;
    }

    block_device_t nvme_dev = nvme_get_block_device();
    disk_partition_scan_device(nvme_dev);

    vfs_node_t *drives_dir = vfs_finddir(vfs_root, "drives");
    if (!drives_dir) {
        drives_dir = ramfs_create_directory(vfs_root, "drives");
    }

    int p_count = disk_get_partition_count();
    for (int i = 0; i < p_count; i++) {
        partition_info_t *part = disk_get_partition(i);
        if (!part) continue;

        vfs_node_t *fat_root = fat32_mount_partition(nvme_dev, part->start_lba, part->sector_count);
        if (fat_root && drives_dir) {
            vfs_node_t *disk_dir = ramfs_create_directory(drives_dir, "fat32_nvme");
            if (disk_dir) {
                disk_dir->flags |= FS_MOUNTPOINT;
                disk_dir->ptr = (vfs_node_t *)fat_root;
                serial_puts(COM1, "[STORAGE] Mounted FAT32 partition at '/drives/fat32_nvme'\n");
            }
            continue;
        }

        vfs_node_t *ext2_root = ext2_mount_partition(nvme_dev, part->start_lba);
        if (ext2_root && drives_dir) {
            vfs_node_t *ext2_dir = ramfs_create_directory(drives_dir, "ext2_nvme");
            if (ext2_dir) {
                ext2_dir->flags |= FS_MOUNTPOINT;
                ext2_dir->ptr = (vfs_node_t *)ext2_root;
                serial_puts(COM1, "[STORAGE] Mounted EXT2 partition at '/drives/ext2_nvme'\n");
            }
            continue;
        }
    }
}

static int __init fat32_fs_initcall(void) {
    fat32_init();
    return 0;
}
fs_initcall(fat32_fs_initcall);