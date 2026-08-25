// partition.c - Non-Destructive Partition & Unallocated Space Analyzer
#include "partition.h"
#include "gpt.h"
#include "mbr.h"
#include "../drivers/serial/serial.h"
#include "../libs/string.h"
#include "../libs/stdio.h"

static partition_info_t unified_partitions[MAX_TOTAL_PARTITIONS];
static int unified_partition_count = 0;

void disk_partition_scan_device(block_device_t dev) {
    unified_partition_count = 0;
    memset(unified_partitions, 0, sizeof(unified_partitions));

    // 1. Try GPT
    gpt_init_device(dev);
    int gpt_count = gpt_get_partition_count();

    if (gpt_count > 0) {
        for (int i = 0; i < gpt_count && unified_partition_count < MAX_TOTAL_PARTITIONS; i++) {
            partition_info_t *p = gpt_get_partition(i);
            if (p) {
                unified_partitions[unified_partition_count] = *p;
                unified_partitions[unified_partition_count].index = (uint8_t)unified_partition_count;

                // Protect EFI System Partition
                if (p->raw_type == 0xEF || p->kind == PARTITION_TYPE_ESP) {
                    unified_partitions[unified_partition_count].is_esp = true;
                    strcpy(unified_partitions[unified_partition_count].fs_name, "EFI System (ESP)");
                } else if (p->kind == PARTITION_TYPE_FAT32) {
                    strcpy(unified_partitions[unified_partition_count].fs_name, "FAT32");
                } else if (p->kind == PARTITION_TYPE_EXT2) {
                    strcpy(unified_partitions[unified_partition_count].fs_name, "EXT2");
                } else {
                    strcpy(unified_partitions[unified_partition_count].fs_name, "Unknown/Raw");
                }

                unified_partition_count++;
            }
        }
        return;
    }

    // 2. Fallback MBR
    mbr_init_device(dev);
    int mbr_count = mbr_get_partition_count();

    for (int i = 0; i < mbr_count && unified_partition_count < MAX_TOTAL_PARTITIONS; i++) {
        partition_info_t *p = mbr_get_partition(i);
        if (p) {
            unified_partitions[unified_partition_count] = *p;
            unified_partitions[unified_partition_count].index = (uint8_t)unified_partition_count;

            if (p->raw_type == 0xEF || p->raw_type == 0xEE) {
                unified_partitions[unified_partition_count].is_esp = true;
                strcpy(unified_partitions[unified_partition_count].fs_name, "ESP/Protective");
            } else if (p->raw_type == 0x0B || p->raw_type == 0x0C) {
                strcpy(unified_partitions[unified_partition_count].fs_name, "FAT32");
            } else if (p->raw_type == 0x83) {
                strcpy(unified_partitions[unified_partition_count].fs_name, "EXT2/Linux");
            } else {
                strcpy(unified_partitions[unified_partition_count].fs_name, "Unknown/Raw");
            }

            unified_partition_count++;
        }
    }
}

int disk_get_partition_count(void) {
    return unified_partition_count;
}

partition_info_t *disk_get_partition(int index) {
    if (index < 0 || index >= unified_partition_count) return NULL;
    return &unified_partitions[index];
}

void disk_find_unallocated_space(disk_device_info_t *disk) {
    if (!disk || disk->partition_count == 0) return;

    uint32_t last_lba = 2048; // Standard 1MB Offset

    for (int i = 0; i < disk->partition_count; i++) {
        partition_info_t *p = &disk->partitions[i];
        if (p->start_lba > last_lba + 2048) {
            // Gap detected!
            serial_puts(COM1, "[PARTITION] Unallocated space found between LBA ");
            char b[32];
            itoa(last_lba, 10, b);
            serial_puts(COM1, b);
            serial_puts(COM1, " and ");
            itoa(p->start_lba, 10, b);
            serial_puts(COM1, b);
            serial_puts(COM1, "\n");
        }
        last_lba = p->start_lba + p->sector_count;
    }
}