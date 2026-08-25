// partition.h - Advanced Hardware Partition & Free Space Scanner
#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include <stdbool.h>
#include "../drivers/disk/block.h"

#define MAX_TOTAL_PARTITIONS 32

typedef enum {
    PARTITION_TYPE_UNKNOWN = 0,
    PARTITION_TYPE_FAT32,
    PARTITION_TYPE_EXT2,
    PARTITION_TYPE_ESP,      // EFI System Partition (Protect against accidental format)
    PARTITION_TYPE_UNALLOCATED
} partition_kind_t;

typedef struct {
    uint8_t index;
    uint32_t start_lba;
    uint32_t sector_count;
    uint8_t raw_type;
    uint8_t type;
    partition_kind_t kind;
    char fs_name[32];
    bool is_esp;             // Critical Hardware Protection Guard
} partition_info_t;

typedef struct {
    char name[32];
    block_device_t bdev;
    uint64_t total_sectors;
    uint32_t sector_size;
    partition_info_t partitions[MAX_TOTAL_PARTITIONS];
    int partition_count;
} disk_device_info_t;

void disk_partition_scan_device(block_device_t dev);
int disk_get_partition_count(void);
partition_info_t *disk_get_partition(int index);
void disk_partition_scan(uint8_t drive);
void disk_find_unallocated_space(disk_device_info_t *disk);

#endif // PARTITION_H