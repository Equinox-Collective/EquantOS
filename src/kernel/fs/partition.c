// partition.c - Unified Partition Table Manager implementation with exhaustive debugging
#include "partition.h"
#include "gpt.h"
#include "mbr.h"
#include "../drivers/disk/nvme.h"
#include "../drivers/serial/serial.h"
#include "stdio.h"

#define MAX_TOTAL_PARTITIONS 128
static partition_info_t unified_partitions[MAX_TOTAL_PARTITIONS];
static int unified_partition_count = 0;

void disk_partition_scan_device(block_device_t dev) {
    char buf[32];
    serial_puts(COM1, "[DEBUG-PARTITION] ========================================\n");
    serial_puts(COM1, "[DEBUG-PARTITION] BEGIN UNIFIED PARTITION SCAN FOR DEVICE\n");
    
    unified_partition_count = 0;

    // 1. Try GPT (GUID Partition Table) first via Block Device Interface
    serial_puts(COM1, "[DEBUG-PARTITION] Step 1: Initializing GPT parser...\n");
    gpt_init_device(dev);
    int gpt_count = gpt_get_partition_count();
    
    serial_puts(COM1, "[DEBUG-PARTITION] GPT parser reported partition count: ");
    itoa(gpt_count, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    if (gpt_count > 0) {
        serial_puts(COM1, "[DEBUG-PARTITION] STATUS: Adopting GPT partition table layout.\n");
        for (int i = 0; i < gpt_count && unified_partition_count < MAX_TOTAL_PARTITIONS; i++) {
            partition_info_t *p = gpt_get_partition(i);
            if (p) {
                unified_partitions[unified_partition_count] = *p;
                unified_partitions[unified_partition_count].index = unified_partition_count;
                
                serial_puts(COM1, "[DEBUG-PARTITION]   -> [GPT] Adopted Part #");
                itoa(unified_partition_count, 10, buf);
                serial_puts(COM1, buf);
                serial_puts(COM1, " | Start LBA: ");
                itoa((int64_t)p->start_lba, 10, buf);
                serial_puts(COM1, buf);
                serial_puts(COM1, " | Sectors: ");
                itoa((int64_t)p->sector_count, 10, buf);
                serial_puts(COM1, buf);
                serial_puts(COM1, "\n");

                unified_partition_count++;
            }
        }
        serial_puts(COM1, "[DEBUG-PARTITION] SCAN COMPLETE (Source: GPT). Total partitions: ");
        itoa(unified_partition_count, 10, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, "\n[DEBUG-PARTITION] ========================================\n");
        return;
    }

    // 2. Fallback to legacy MBR (Master Boot Record)
    serial_puts(COM1, "[DEBUG-PARTITION] STATUS: GPT yielded 0 partitions. Falling back to Legacy MBR scan...\n");
    mbr_init();
    int mbr_count = mbr_get_partition_count();
    
    serial_puts(COM1, "[DEBUG-PARTITION] MBR parser reported partition count: ");
    itoa(mbr_count, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    for (int i = 0; i < mbr_count && unified_partition_count < MAX_TOTAL_PARTITIONS; i++) {
        partition_info_t *p = mbr_get_partition(i);
        if (p) {
            unified_partitions[unified_partition_count] = *p;
            unified_partitions[unified_partition_count].index = unified_partition_count;
            
            serial_puts(COM1, "[DEBUG-PARTITION]   -> [MBR] Adopted Part #");
            itoa(unified_partition_count, 10, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, " | Type: 0x");
            itoa_hex(p->type, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, " | Start LBA: ");
            itoa((int64_t)p->start_lba, 10, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, " | Sectors: ");
            itoa((int64_t)p->sector_count, 10, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, "\n");

            unified_partition_count++;
        }
    }

    serial_puts(COM1, "[DEBUG-PARTITION] SCAN COMPLETE (Source: MBR). Total partitions: ");
    itoa(unified_partition_count, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n[DEBUG-PARTITION] ========================================\n");
}

void disk_partition_scan(uint8_t drive) {
    (void)drive;
    // Redirect legacy call to NVMe block device
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