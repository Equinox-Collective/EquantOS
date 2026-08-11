// gpt.c - GUID Partition Table (GPT) parser implementation
#include "gpt.h"
#include "mbr.h"
#include "../drivers/disk/ata.h"
#include "../drivers/serial/serial.h"
#include "string.h"
#include "stdio.h"
#include "../core/mem/memory.h"

#define MAX_GPT_PARTITIONS 128
static partition_info_t gpt_partitions[MAX_GPT_PARTITIONS];
static int gpt_partition_count = 0;

void gpt_init(uint8_t drive) {
    serial_puts(COM1, "[GPT] Checking for GPT partition table...\n");

    uint8_t sector_buf[512];
    
    // 1. Read LBA 0 (Protective MBR)
    read_sectors_ata_pio_drive(drive, (uintptr_t)sector_buf, 0, 1);
    protective_mbr_t *pmbr = (protective_mbr_t *)sector_buf;

    if (pmbr->signature != 0xAA55 || pmbr->partition_record.os_type != 0xEE) {
        serial_puts(COM1, "[GPT] No protective MBR found. Drive uses legacy MBR or has no partition table.\n");
        return;
    }

    // 2. Read LBA 1 (GPT Header)
    read_sectors_ata_pio_drive(drive, (uintptr_t)sector_buf, 1, 1);
    gpt_header_t *header = (gpt_header_t *)sector_buf;

    if (header->signature != GPT_SIGNATURE) {
        serial_puts(COM1, "[GPT ERROR] Invalid GPT header signature ('EFI PART' not found).\n");
        return;
    }

    serial_puts(COM1, "[GPT] Valid GPT header found! Parsing partition entries...\n");
    gpt_partition_count = 0;

    uint64_t entries_lba = header->partition_entries_lba;
    uint32_t num_entries = header->num_partition_entries;
    uint32_t entry_size = header->size_partition_entry; // Usually 128 bytes

    uint32_t total_bytes = num_entries * entry_size;
    uint32_t sectors_to_read = (total_bytes + 511) / 512;

    uint8_t *entries_buf = (uint8_t *)kmalloc(sectors_to_read * 512);
    if (!entries_buf) {
        serial_puts(COM1, "[GPT ERROR] Out of memory for GPT partition entries!\n");
        return;
    }

    // Read partition entries array starting from entries_lba (usually LBA 2)
    read_sectors_ata_pio_drive(drive, (uintptr_t)entries_buf, entries_lba, sectors_to_read);

    for (uint32_t i = 0; i < num_entries && gpt_partition_count < MAX_GPT_PARTITIONS; i++) {
        gpt_partition_entry_t *entry = (gpt_partition_entry_t *)(entries_buf + (i * entry_size));

        // Check if partition type GUID is all zeros (unused entry slot)
        int is_empty = 1;
        for (int b = 0; b < 16; b++) {
            if (entry->partition_type_guid[b] != 0) {
                is_empty = 0;
                break;
            }
        }

        if (is_empty) continue;

        gpt_partitions[gpt_partition_count].index = gpt_partition_count;
        gpt_partitions[gpt_partition_count].type = 0x83; // Map to generic data partition flag
        gpt_partitions[gpt_partition_count].start_lba = (uint32_t)entry->starting_lba;
        gpt_partitions[gpt_partition_count].sector_count = (uint32_t)(entry->ending_lba - entry->starting_lba + 1);

        serial_puts(COM1, "[GPT]   -> Partition #");
        char buf[32];
        itoa(gpt_partition_count, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, " | Start LBA: ");
        itoa((int64_t)entry->starting_lba, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, " | Sectors: ");
        itoa((int64_t)gpt_partitions[gpt_partition_count].sector_count, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, "\n");

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