// mbr.c - Master Boot Record (MBR) and Partition Table parser implementation
#include "mbr.h"
#include "../drivers/disk/ata.h"
#include "serial.h"
#include "string.h"
#include "stdio.h"

#define MAX_PARTITIONS 4
static partition_info_t detected_partitions[MAX_PARTITIONS];
static int partition_count = 0;

void mbr_init(void) {
    serial_puts(COM1, "[MBR] Reading primary sector (LBA 0) for partition table...\n");
    
    // Allocate 512-byte buffer on stack or via kmalloc
    uint8_t sector_buf[512];
    read_sectors_ata_pio((uintptr_t)sector_buf, 0, 1);

    mbr_sector_t *mbr = (mbr_sector_t *)sector_buf;

    // Verify boot signature 0xAA55
    if (mbr->signature != MBR_SIGNATURE) {
        serial_puts(COM1, "[MBR] Invalid boot signature (expected 0xAA55). No valid MBR found.\n");
        return;
    }

    serial_puts(COM1, "[MBR] Valid MBR signature found. Parsing primary partitions...\n");
    partition_count = 0;

    for (int i = 0; i < 4; i++) {
        mbr_partition_t *part = &mbr->partitions[i];
        
        if (part->type == MBR_TYPE_EMPTY) {
            continue;
        }

        detected_partitions[partition_count].index = i;
        detected_partitions[partition_count].type = part->type;
        detected_partitions[partition_count].start_lba = part->lba_start;
        detected_partitions[partition_count].sector_count = part->sector_count;

        // Log partition details to serial port
        serial_puts(COM1, "[MBR]   -> Partition ");
        char idx_str[4];
        itoa(i, 10, idx_str);
        serial_puts(COM1, idx_str);
        serial_puts(COM1, " | Type: 0x");
        char type_str[8];
        itoa_hex(part->type, type_str);
        serial_puts(COM1, type_str);
        serial_puts(COM1, " | Start LBA: ");
        char lba_str[16];
        itoa(part->lba_start, 10, lba_str);
        serial_puts(COM1, lba_str);
        serial_puts(COM1, " | Sectors: ");
        char sec_str[16];
        itoa(part->sector_count, 10, sec_str);
        serial_puts(COM1, sec_str);
        serial_puts(COM1, "\n");

        partition_count++;
    }

    if (partition_count == 0) {
        serial_puts(COM1, "[MBR] No active partitions found on the disk.\n");
    }
}

int mbr_get_partition_count(void) {
    return partition_count;
}

partition_info_t *mbr_get_partition(int index) {
    if (index < 0 || index >= partition_count) return NULL;
    return &detected_partitions[index];
}