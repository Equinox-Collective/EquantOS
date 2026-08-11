// gpt.c - GUID Partition Table (GPT) parser implementation with deep logging
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
    char buf[32];
    serial_puts(COM1, "[DEBUG-GPT] ----------------------------------------\n");
    serial_puts(COM1, "[DEBUG-GPT] Starting GPT verification on drive ID: ");
    itoa(drive, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    uint8_t sector_buf[512];
    gpt_partition_count = 0;
    
    // 1. Read LBA 0 (Protective MBR)
    serial_puts(COM1, "[DEBUG-GPT] Reading LBA 0 (Protective MBR)...\n");
    read_sectors_ata_pio_drive(drive, (uintptr_t)sector_buf, 0, 1);
    protective_mbr_t *pmbr = (protective_mbr_t *)sector_buf;

    serial_puts(COM1, "[DEBUG-GPT] PMBR Boot Signature read: 0x");
    itoa_hex(pmbr->signature, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, " (Expected: 0xAA55)\n");

    serial_puts(COM1, "[DEBUG-GPT] PMBR Partition 0 OS Type read: 0x");
    itoa_hex(pmbr->partition_record.os_type, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, " (Expected for GPT: 0xEE)\n");

    if (pmbr->signature != 0xAA55 || pmbr->partition_record.os_type != 0xEE) {
        serial_puts(COM1, "[DEBUG-GPT] VERIFICATION FAILED: Not a valid Protective MBR.\n");
        return;
    }
    serial_puts(COM1, "[DEBUG-GPT] VERIFICATION PASSED: Protective MBR confirmed.\n");

    // 2. Read LBA 1 (GPT Header)
    serial_puts(COM1, "[DEBUG-GPT] Reading LBA 1 (Primary GPT Header)...\n");
    read_sectors_ata_pio_drive(drive, (uintptr_t)sector_buf, 1, 1);
    gpt_header_t *header = (gpt_header_t *)sector_buf;

    serial_puts(COM1, "[DEBUG-GPT] GPT Header Magic Signature read: 0x");
    itoa_hex((uint32_t)(header->signature >> 32), buf); // Print high bits
    serial_puts(COM1, buf);
    itoa_hex((uint32_t)(header->signature & 0xFFFFFFFF), buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, " (Expected: 'EFI PART' -> 0x5452415020494645)\n");

    if (header->signature != GPT_SIGNATURE) {
        serial_puts(COM1, "[DEBUG-GPT] VERIFICATION FAILED: 'EFI PART' signature missing in GPT header.\n");
        return;
    }
    serial_puts(COM1, "[DEBUG-GPT] VERIFICATION PASSED: Valid GPT Header signature found!\n");

    serial_puts(COM1, "[DEBUG-GPT]   -> Partition entries starting LBA: ");
    itoa((int64_t)header->partition_entries_lba, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n[DEBUG-GPT]   -> Total partition entry slots: ");
    itoa(header->num_partition_entries, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n[DEBUG-GPT]   -> Size of each partition entry: ");
    itoa(header->size_partition_entry, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, " bytes\n");

    uint64_t entries_lba = header->partition_entries_lba;
    uint32_t num_entries = header->num_partition_entries;
    uint32_t entry_size = header->size_partition_entry;

    uint32_t total_bytes = num_entries * entry_size;
    uint32_t sectors_to_read = (total_bytes + 511) / 512;

    serial_puts(COM1, "[DEBUG-GPT] Allocating buffer for partition entries (sectors to read: ");
    itoa(sectors_to_read, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, ")...\n");

    uint8_t *entries_buf = (uint8_t *)kmalloc(sectors_to_read * 512);
    if (!entries_buf) {
        serial_puts(COM1, "[DEBUG-GPT ERROR] Memory allocation failed for GPT partition entries!\n");
        return;
    }

    // Read partition entries array starting from entries_lba (usually LBA 2)
    serial_puts(COM1, "[DEBUG-GPT] Reading partition entries array from disk...\n");
    read_sectors_ata_pio_drive(drive, (uintptr_t)entries_buf, entries_lba, sectors_to_read);

    serial_puts(COM1, "[DEBUG-GPT] Scanning partition entry slots for active volumes...\n");
    for (uint32_t i = 0; i < num_entries && gpt_partition_count < MAX_GPT_PARTITIONS; i++) {
        gpt_partition_entry_t *entry = (gpt_partition_entry_t *)(entries_buf + (i * entry_size));

        // Check if partition type GUID is all zeros (unused slot)
        int is_empty = 1;
        for (int b = 0; b < 16; b++) {
            if (entry->partition_type_guid[b] != 0) {
                is_empty = 0;
                break;
            }
        }

        if (is_empty) continue;

        gpt_partitions[gpt_partition_count].index = gpt_partition_count;
        gpt_partitions[gpt_partition_count].type = 0x83; // Standard data type flag
        gpt_partitions[gpt_partition_count].start_lba = (uint32_t)entry->starting_lba;
        gpt_partitions[gpt_partition_count].sector_count = (uint32_t)(entry->ending_lba - entry->starting_lba + 1);

        serial_puts(COM1, "[DEBUG-GPT]   -> Found Active GPT Partition [Slot #");
        itoa(i, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, "] Index: ");
        itoa(gpt_partition_count, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, " | Start LBA: ");
        itoa((int64_t)entry->starting_lba, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, " | End LBA: ");
        itoa((int64_t)entry->ending_lba, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, " | Total Sectors: ");
        itoa((int64_t)gpt_partitions[gpt_partition_count].sector_count, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, "\n");

        gpt_partition_count++;
    }

    kfree(entries_buf);
    serial_puts(COM1, "[DEBUG-GPT] GPT scan completed successfully. Total valid partitions found: ");
    itoa(gpt_partition_count, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n[DEBUG-GPT] ----------------------------------------\n");
}

int gpt_get_partition_count(void) {
    return gpt_partition_count;
}

partition_info_t *gpt_get_partition(int index) {
    if (index < 0 || index >= gpt_partition_count) return NULL;
    return &gpt_partitions[index];
}