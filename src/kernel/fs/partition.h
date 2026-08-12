// partition.h - Unified Partition Table Management (MBR & GPT abstraction)
#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include "../drivers/disk/block.h"

typedef struct {
    uint32_t start_lba;
    uint32_t sector_count;
    uint8_t  type;
    uint8_t  index;
} partition_info_t;

// Unified disk partition scanner interface
void disk_partition_scan_device(block_device_t dev);
void disk_partition_scan(uint8_t drive);
int disk_get_partition_count(void);
partition_info_t *disk_get_partition(int index);

#endif // PARTITION_H