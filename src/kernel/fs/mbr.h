// mbr.h - Master Boot Record (MBR) and Partition Table parser definitions
#ifndef MBR_H
#define MBR_H

#include <stdint.h>

#define MBR_SIGNATURE 0xAA55

// Common partition type identifiers
#define MBR_TYPE_EMPTY     0x00
#define MBR_TYPE_FAT16     0x06
#define MBR_TYPE_FAT32_1   0x0B
#define MBR_TYPE_FAT32_2   0x0C
#define MBR_TYPE_EXT2      0x83

// 16-byte Primary Partition Record
typedef struct {
    uint8_t  attributes;         // Bootable flag (0x80 = active/bootable, 0x00 = non-bootable)
    uint8_t  chs_start[3];       // CHS start address (legacy, ignored in LBA mode)
    uint8_t  type;               // Partition type identifier code
    uint8_t  chs_end[3];         // CHS end address (legacy, ignored in LBA mode)
    uint32_t lba_start;          // Starting LBA sector of the partition (little-endian)
    uint32_t sector_count;       // Total number of sectors in the partition (little-endian)
} __attribute__((packed)) mbr_partition_t;

// Full 512-byte MBR sector layout
typedef struct {
    uint8_t         bootstrap_code[446]; // Bootloader bootstrap machine code
    mbr_partition_t partitions[4];       // Array of 4 primary partition entries
    uint16_t        signature;           // Boot sector signature: must be 0xAA55
} __attribute__((packed)) mbr_sector_t;

// High-level abstracted partition info structure used by VFS/Drivers
typedef struct {
    uint32_t start_lba;
    uint32_t sector_count;
    uint8_t  type;
    uint8_t  index;              // Partition slot index (0 to 3)
} partition_info_t;


void mbr_init(void);
int mbr_get_partition_count(void);
partition_info_t *mbr_get_partition(int index);

#endif // MBR_H