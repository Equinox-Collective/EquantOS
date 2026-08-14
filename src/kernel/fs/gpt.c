// gpt.c - GUID Partition Table (GPT) parser implementation with block device abstraction
#include "gpt.h"
#include "mbr.h"
#include "../drivers/disk/block.h"
#include "../drivers/serial/serial.h"
#include "string.h"
#include "stdio.h"
#include "../core/mem/memory.h"

#define MAX_GPT_PARTITIONS 128
static partition_info_t gpt_partitions[MAX_GPT_PARTITIONS];
static int gpt_partition_count = 0;

void gpt_init_device(block_device_t dev) {
    uint8_t sector_buf[512];
    gpt_partition_count = 0;
    
    // 1. Read LBA 0 (Protective MBR)
    if (dev.read(0, 1, sector_buf) != 0) return;
    protective_mbr_t *pmbr = (protective_mbr_t *)sector_buf;

    if (pmbr->signature != 0xAA55 || pmbr->partition_record.os_type != 0xEE) {
        return;
    }

    // 2. Read LBA 1 (Primary GPT Header)
    if (dev.read(1, 1, sector_buf) != 0) return;
    gpt_header_t *header = (gpt_header_t *)sector_buf;

    if (header->signature != GPT_SIGNATURE) {
        return;
    }

    uint64_t entries_lba = header->partition_entries_lba;
    uint32_t num_entries = header->num_partition_entries;
    uint32_t entry_size = header->size_partition_entry;

    uint32_t total_bytes = num_entries * entry_size;
    uint32_t sectors_to_read = (total_bytes + 511) / 512;

    uint8_t *entries_buf = (uint8_t *)kmalloc(sectors_to_read * 512);
    if (!entries_buf) return;

    if (dev.read(entries_lba, sectors_to_read, entries_buf) != 0) {
        kfree(entries_buf);
        return;
    }

    for (uint32_t i = 0; i < num_entries && gpt_partition_count < MAX_GPT_PARTITIONS; i++) {
        gpt_partition_entry_t *entry = (gpt_partition_entry_t *)(entries_buf + (i * entry_size));

        int is_empty = 1;
        for (int b = 0; b < 16; b++) {
            if (entry->partition_type_guid[b] != 0) {
                is_empty = 0;
                break;
            }
        }

        if (is_empty) continue;

        gpt_partitions[gpt_partition_count].index = gpt_partition_count;
        gpt_partitions[gpt_partition_count].type = 0x0C; // General partition candidate
        gpt_partitions[gpt_partition_count].start_lba = (uint32_t)entry->starting_lba;
        gpt_partitions[gpt_partition_count].sector_count = (uint32_t)(entry->ending_lba - entry->starting_lba + 1);

        gpt_partition_count++;
    }

    kfree(entries_buf);
}

int gpt_get_partition_count(void) {
    return gpt_partition_count;
}

partition_info_t *gpt_get_partition(int index) {
    if (index < 0 || index >= gpt_partition_count) return NULL;
    return &gpt_partitions[index];
}