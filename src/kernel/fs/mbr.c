// mbr.c - Master Boot Record (MBR) parser implementation
#include "mbr.h"
#include "../drivers/disk/ata.h"
#include "../drivers/disk/nvme.h"
#include "../drivers/serial/serial.h"
#include "string.h"
#include "stdio.h"

#define MAX_PARTITIONS 4
static partition_info_t detected_partitions[MAX_PARTITIONS];
static int partition_count = 0;

void mbr_init_device(block_device_t dev) {
    uint8_t sector_buf[512];
    partition_count = 0;

    if (dev.read(0, 1, sector_buf) != 0) return;

    mbr_sector_t *mbr = (mbr_sector_t *)sector_buf;

    if (mbr->signature != MBR_SIGNATURE) {
        serial_puts(COM1, "[MBR] Invalid boot signature (expected 0xAA55). No valid MBR found.\n");
        return;
    }

    serial_puts(COM1, "[MBR] Valid MBR signature found. Parsing primary partitions...\n");

    for (int i = 0; i < 4; i++) {
        mbr_partition_t *part = &mbr->partitions[i];
        
        if (part->type == MBR_TYPE_EMPTY) {
            continue;
        }

        detected_partitions[partition_count].index = i;
        detected_partitions[partition_count].type = part->type;
        detected_partitions[partition_count].start_lba = part->lba_start;
        detected_partitions[partition_count].sector_count = part->sector_count;

        partition_count++;
    }
}

void mbr_init(void) {
    block_device_t nvme_dev = nvme_get_block_device();
    mbr_init_device(nvme_dev);
}

int mbr_get_partition_count(void) {
    return partition_count;
}

partition_info_t *mbr_get_partition(int index) {
    if (index < 0 || index >= partition_count) return NULL;
    return &detected_partitions[index];
}