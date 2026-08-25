// src/kernel/fs/partition.h - Production Partition & Unallocated Space Analyzer
#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include <stdbool.h>
#include "../drivers/disk/block.h"

typedef enum {
    PART_TYPE_UNKNOWN = 0,
    PART_TYPE_ESP,              // EFI System Partition
    PART_TYPE_WINDOWS,          // NTFS / FAT32 (Microsoft Basic Data)
    PART_TYPE_LINUX,            // EXT4 / Btrfs / XFS
    PART_TYPE_EQUANTOS,         // EquantOS Target (FAT32 / EXT2)
    PART_TYPE_SWAP,             // Linux Swap
    PART_TYPE_UNALLOCATED       // True Free Space Gap
} partition_kind_t;

typedef struct {
    uint8_t  index;
    uint32_t start_lba;
    uint32_t sector_count;
    uint8_t  type_code;        // MBR type byte
    partition_kind_t kind;
    bool     is_free_space;    // True if this is an unallocated gap
    char     description[64];  // Human readable description
} partition_info_t;

// Scanning API
void disk_partition_scan_device(block_device_t dev);
void disk_partition_scan(uint8_t drive);
int disk_get_partition_count(void);
partition_info_t *disk_get_partition(int index);

// Unallocated space scan API
int disk_get_free_space_gaps(partition_info_t *out_gaps, int max_gaps, uint64_t total_disk_sectors);

#endif // PARTITION_H