// src/kernel/fs/iso9660.h - ISO9660 Optical Disc Filesystem Driver
#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vfs.h"
#include "../drivers/disk/block.h"

#define ISO_SECTOR_SIZE 2048
#define ISO_PVD_LBA     16

// ISO9660 Directory Record Structure (34+ bytes)
typedef struct {
    uint8_t  length;
    uint8_t  ext_attr_length;
    uint32_t extent_lba_lsb;
    uint32_t extent_lba_msb;
    uint32_t data_length_lsb;
    uint32_t data_length_msb;
    uint8_t  recording_date[7];
    uint8_t  flags;             // Bit 1 (0x02) = Directory
    uint8_t  file_unit_size;
    uint8_t  interleave_gap_size;
    uint16_t vol_seq_num_lsb;
    uint16_t vol_seq_num_msb;
    uint8_t  name_len;
    char     file_identifier[];
} __attribute__((packed)) iso9660_dir_record_t;

// ISO9660 Primary Volume Descriptor (PVD) Structure (2048 bytes)
typedef struct {
    uint8_t  type;              // Must be 0x01
    char     id[5];             // Must be "CD001"
    uint8_t  version;           // Must be 0x01
    uint8_t  unused1;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused2[8];
    uint32_t volume_space_size_lsb;
    uint32_t volume_space_size_msb;
    uint8_t  unused3[32];
    uint16_t volume_set_size_lsb;
    uint16_t volume_set_size_msb;
    uint16_t volume_sequence_number_lsb;
    uint16_t volume_sequence_number_msb;
    uint16_t logical_block_size_lsb;
    uint16_t logical_block_size_msb;
    uint32_t path_table_size_lsb;
    uint32_t path_table_size_msb;
    uint32_t type_l_path_table;
    uint32_t opt_type_l_path_table;
    uint32_t type_m_path_table;
    uint32_t opt_type_m_path_table;
    iso9660_dir_record_t root_directory_record;
} __attribute__((packed)) iso9660_pvd_t;

void iso9660_init(void);
vfs_node_t *iso9660_mount(block_device_t dev);

#endif // ISO9660_H