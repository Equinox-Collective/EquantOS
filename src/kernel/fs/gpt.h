// gpt.h - GUID Partition Table (GPT) parser definitions for EquantOS
#ifndef GPT_H
#define GPT_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "partition.h"

#define GPT_SIGNATURE 0x5452415020494645ULL // "EFI PART"

typedef struct {
    uint8_t  boot_indicator;
    uint8_t  chs_start[3];
    uint8_t  os_type;       // Must be 0xEE for Protective MBR
    uint8_t  chs_end[3];
    uint32_t starting_lba;  // Usually 1
    uint32_t total_sectors;
} __attribute__((packed)) protective_mbr_record_t;

typedef struct {
    uint8_t  boot_code[446];
    protective_mbr_record_t partition_record;
    uint16_t signature;     // 0xAA55
} __attribute__((packed)) protective_mbr_t;

typedef struct {
    uint64_t signature;     // "EFI PART" (0x5452415020494645)
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entries_lba; // Usually 2
    uint32_t num_partition_entries; // Usually 128
    uint32_t size_partition_entry;  // Usually 128 bytes
    uint32_t partition_array_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t  partition_type_guid[16];
    uint8_t  unique_partition_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t partition_name[36]; // UTF-16LE
} __attribute__((packed)) gpt_partition_entry_t;

void gpt_init(uint8_t drive);
int gpt_get_partition_count(void);
partition_info_t *gpt_get_partition(int index);

#endif // GPT_H