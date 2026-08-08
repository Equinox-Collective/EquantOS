// fat32.h - FAT32 File System Driver Definitions for EquantOS
#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LONG_NAME (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID)

// BIOS Parameter Block (BPB) for FAT32
typedef struct {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    // 0 for FAT32
    uint16_t total_sectors_16;    // 0 for FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;         // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    
    // FAT32 Extended BPB Fields
    uint32_t table_size_32;       // Sectors per FAT
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;        // Cluster number of root directory (usually 2)
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_num;
    uint8_t  reserved1;
    uint8_t  boot_signature;      // 0x28 or 0x29
    uint32_t volume_id;
    char     volume_label[11];
    char     file_system_type[8]; // "FAT32   "
} __attribute__((packed)) fat32_bpb_t;

// Standard 32-byte Directory Entry
typedef struct {
    char     name[8];
    char     ext[3];
    uint8_t  attribute;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high; // High 16 bits of first cluster
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;  // Low 16 bits of first cluster
    uint32_t file_size;
} __attribute__((packed)) fat32_dir_entry_t;

// Public FAT32 driver interface
void fat32_init(void);
vfs_node_t *fat32_mount_partition(uint32_t partition_lba);

#endif // FAT32_H