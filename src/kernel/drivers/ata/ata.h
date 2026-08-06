#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// Чтение одного сектора (512 байт) с диска
// src/drivers/disk/ata.h
void read_sectors_ata_pio(uintptr_t target_address, uint64_t LBA,
                          uint32_t sector_count);
void write_sectors_ata_pio(uintptr_t source_address, uint64_t LBA,
                           uint32_t sector_count);
void ata_identify();

#endif