// mbr.h - Master Boot Record (MBR) and Partition Table parser definitions
#ifndef MBR_H
#define MBR_H

#include <stdint.h>
#include "partition.h"
#include "../drivers/disk/block.h"

#define MBR_SIGNATURE 0xAA55

#define MBR_TYPE_EMPTY     0x00
#define MBR_TYPE_FAT16     0x06
#define MBR_TYPE_FAT32_1   0x0B
#define MBR_TYPE_FAT32_2   0x0C
#define MBR_TYPE_EXT2      0x83

typedef struct {
    uint8_t  attributes;         
    uint8_t  chs_start[3];       
    uint8_t  type;               
    uint8_t  chs_end[3];         
    uint32_t lba_start;          
    uint32_t sector_count;       
} __attribute__((packed)) mbr_partition_t;

typedef struct {
    uint8_t         bootstrap_code[446]; 
    mbr_partition_t partitions[4];       
    uint16_t        signature;           
} __attribute__((packed)) mbr_sector_t;

void mbr_init_device(block_device_t dev);
void mbr_init(void);
int mbr_get_partition_count(void);
partition_info_t *mbr_get_partition(int index);

#endif // MBR_H