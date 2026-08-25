// src/kernel/fs/partition.c - Rock-solid Partition & Unallocated Space Detection Engine
#include "partition.h"
#include "gpt.h"
#include "mbr.h"
#include "../drivers/disk/nvme.h"
#include "../drivers/serial/serial.h"
#include "stdio.h"
#include "string.h"

#define MAX_TOTAL_PARTITIONS 128
static partition_info_t unified_partitions[MAX_TOTAL_PARTITIONS];
static int unified_partition_count = 0;

static partition_kind_t classify_mbr_type(uint8_t type) {
    switch (type) {
        case 0xEF: return PART_TYPE_ESP;
        case 0x07:
        case 0x0B:
        case 0x0C: return PART_TYPE_WINDOWS;
        case 0x83: return PART_TYPE_LINUX;
        case 0x82: return PART_TYPE_SWAP;
        default:   return PART_TYPE_UNKNOWN;
    }
}

void disk_partition_scan_device(block_device_t dev) {
    serial_puts(COM1, "[PARTITION] Scanning physical storage topology...\n");
    
    unified_partition_count = 0;

    // 1. Scan GPT (GUID Partition Table)
    gpt_init_device(dev);
    int gpt_count = gpt_get_partition_count();

    if (gpt_count > 0) {
        for (int i = 0; i < gpt_count && unified_partition_count < MAX_TOTAL_PARTITIONS; i++) {
            partition_info_t *p = gpt_get_partition(i);
            if (p) {
                unified_partitions[unified_partition_count] = *p;
                unified_partitions[unified_partition_count].index = (uint8_t)unified_partition_count;
                unified_partitions[unified_partition_count].is_free_space = false;
                
                // Categorize GPT Partition
                if (p->type == 0xEF) {
                    unified_partitions[unified_partition_count].kind = PART_TYPE_ESP;
                    strcpy(unified_partitions[unified_partition_count].description, "EFI System Partition (ESP)");
                } else if (p->type == 0x83) {
                    unified_partitions[unified_partition_count].kind = PART_TYPE_LINUX;
                    strcpy(unified_partitions[unified_partition_count].description, "Linux Filesystem Data");
                } else {
                    unified_partitions[unified_partition_count].kind = PART_TYPE_UNKNOWN;
                    strcpy(unified_partitions[unified_partition_count].description, "Existing Data / OS Partition");
                }

                unified_partition_count++;
            }
        }
        return;
    }

    // 2. Fallback to MBR
    mbr_init();
    int mbr_count = mbr_get_partition_count();

    for (int i = 0; i < mbr_count && unified_partition_count < MAX_TOTAL_PARTITIONS; i++) {
        partition_info_t *p = mbr_get_partition(i);
        if (p) {
            unified_partitions[unified_partition_count] = *p;
            unified_partitions[unified_partition_count].index = (uint8_t)unified_partition_count;
            unified_partitions[unified_partition_count].is_free_space = false;
            unified_partitions[unified_partition_count].kind = classify_mbr_type(p->type);

            switch (unified_partitions[unified_partition_count].kind) {
                case PART_TYPE_ESP:
                    strcpy(unified_partitions[unified_partition_count].description, "EFI System Partition (ESP)");
                    break;
                case PART_TYPE_WINDOWS:
                    strcpy(unified_partitions[unified_partition_count].description, "Windows / NTFS / FAT Partition");
                    break;
                case PART_TYPE_LINUX:
                    strcpy(unified_partitions[unified_partition_count].description, "Linux / Native Partition");
                    break;
                default:
                    strcpy(unified_partitions[unified_partition_count].description, "Existing Partition (Unknown FS)");
                    break;
            }

            unified_partition_count++;
        }
    }
}

void disk_partition_scan(uint8_t drive) {
    (void)drive;
    block_device_t nvme_dev = nvme_get_block_device();
    disk_partition_scan_device(nvme_dev);
}

int disk_get_partition_count(void) {
    return unified_partition_count;
}

partition_info_t *disk_get_partition(int index) {
    if (index < 0 || index >= unified_partition_count) return NULL;
    return &unified_partitions[index];
}

/* Scans for true unallocated gaps between partitions.
   Protects ALL existing partitions (known and unknown) from being overwritten! */
int disk_get_free_space_gaps(partition_info_t *out_gaps, int max_gaps, uint64_t total_disk_sectors) {
    if (!out_gaps || max_gaps <= 0) return 0;

    int gap_count = 0;
    uint32_t current_lba = 2048; // Standard 1MB alignment offset

    // Sort partitions by starting LBA
    partition_info_t sorted[MAX_TOTAL_PARTITIONS];
    int count = unified_partition_count;
    for (int i = 0; i < count; i++) sorted[i] = unified_partitions[i];

    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (sorted[j].start_lba > sorted[j + 1].start_lba) {
                partition_info_t tmp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = tmp;
            }
        }
    }

    for (int i = 0; i < count && gap_count < max_gaps; i++) {
        if (sorted[i].start_lba > current_lba + 2048) { // Minimum 1MB gap
            out_gaps[gap_count].index = 0xFF;
            out_gaps[gap_count].start_lba = current_lba;
            out_gaps[gap_count].sector_count = sorted[i].start_lba - current_lba;
            out_gaps[gap_count].type = 0x00;
            out_gaps[gap_count].kind = PART_TYPE_UNALLOCATED;
            out_gaps[gap_count].is_free_space = true;
            strcpy(out_gaps[gap_count].description, "Unallocated Free Disk Space");
            gap_count++;
        }
        current_lba = sorted[i].start_lba + sorted[i].sector_count;
    }

    // Check tail space up to total_disk_sectors
    if (total_disk_sectors > current_lba + 2048 && gap_count < max_gaps) {
        out_gaps[gap_count].index = 0xFF;
        out_gaps[gap_count].start_lba = current_lba;
        out_gaps[gap_count].sector_count = (uint32_t)(total_disk_sectors - current_lba);
        out_gaps[gap_count].type = 0x00;
        out_gaps[gap_count].kind = PART_TYPE_UNALLOCATED;
        out_gaps[gap_count].is_free_space = true;
        strcpy(out_gaps[gap_count].description, "Unallocated Free Disk Space (Disk Tail)");
        gap_count++;
    }

    return gap_count;
}