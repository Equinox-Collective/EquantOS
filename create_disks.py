import struct
import os
import zlib
import uuid

def create_mbr_fat32_disk(filename="disk_mbr_fat32.img", size_mb=64):
    if os.path.exists(filename):
        print(f"[SKIP] Disk image '{filename}' already exists. Preserving data.")
        return

    print(f"[CREATE] Generating new {size_mb}MB FAT32 disk image: '{filename}'...")
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)

    struct.pack_into('<H', disk, 510, 0xAA55)
    part_start_lba = 2048
    part_sectors = (img_size // 512) - part_start_lba
    
    struct.pack_into('<B', disk, 446 + 0, 0x80)
    struct.pack_into('<B', disk, 446 + 4, 0x0C)
    struct.pack_into('<I', disk, 446 + 8, part_start_lba)
    struct.pack_into('<I', disk, 446 + 12, part_sectors)

    fat_offset = part_start_lba * 512
    disk[fat_offset:fat_offset + 3] = b'\xEB\x58\x90'
    disk[fat_offset + 3:fat_offset + 11] = b'MSWIN4.1'
    struct.pack_into('<H', disk, fat_offset + 11, 512)
    disk[fat_offset + 13] = 8
    struct.pack_into('<H', disk, fat_offset + 14, 32)
    disk[fat_offset + 16] = 2
    disk[fat_offset + 21] = 0xF8
    struct.pack_into('<I', disk, fat_offset + 28, part_start_lba)
    struct.pack_into('<I', disk, fat_offset + 32, part_sectors)
    struct.pack_into('<I', disk, fat_offset + 36, 512)
    struct.pack_into('<I', disk, fat_offset + 44, 2)
    struct.pack_into('<H', disk, fat_offset + 48, 1)
    struct.pack_into('<H', disk, fat_offset + 50, 6)
    disk[fat_offset + 66] = 0x29
    struct.pack_into('<I', disk, fat_offset + 67, 0x12345678)
    disk[fat_offset + 71:fat_offset + 82] = b'EQUANT FAT32'
    disk[fat_offset + 82:fat_offset + 90] = b'FAT32   '
    struct.pack_into('<H', disk, fat_offset + 510, 0xAA55)

    with open(filename, "wb") as f:
        f.write(disk)
    print(f"[SUCCESS] Created MBR + FAT32 Disk Image: '{filename}'")

def create_gpt_ext2_disk(filename="disk_gpt_ext2.img", size_mb=128):
    if os.path.exists(filename):
        print(f"[SKIP] Disk image '{filename}' already exists. Preserving data.")
        return

    print(f"[CREATE] Generating spec-compliant {size_mb}MB GPT Dual-Boot disk: '{filename}'...")
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)
    total_sectors = img_size // 512

    # 1. Protective MBR (LBA 0)
    struct.pack_into('<H', disk, 510, 0xAA55)
    struct.pack_into('<B', disk, 446 + 4, 0xEE)
    struct.pack_into('<I', disk, 446 + 8, 1)
    struct.pack_into('<I', disk, 446 + 12, total_sectors - 1)

    # 2. Partition Entries Array (LBA 2..33, 128 entries * 128 bytes)
    entries_offset = 2 * 512
    entries_size = 128 * 128

    # Entry 0: ESP (FAT32, 40MB = 81920 sectors, >65525 clusters for FAT32 compliance!)
    esp_guid = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
    esp_unique = uuid.UUID('11111111-1111-1111-1111-111111111111').bytes_le
    esp_start = 2048
    esp_sectors = 81920
    esp_end = esp_start + esp_sectors - 1
    esp_name = "EFI System Partition".encode('utf-16le')
    struct.pack_into('<16s16sQQQ72s', disk, entries_offset,
                     esp_guid, esp_unique, esp_start, esp_end, 0, esp_name)

    # Entry 1: Root (EXT2, 64MB = 131072 sectors)
    root_guid = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le
    root_unique = uuid.UUID('22222222-2222-2222-2222-222222222222').bytes_le
    root_start = esp_end + 1
    root_sectors = 131072
    root_end = root_start + root_sectors - 1
    root_name = "EquantOS Root".encode('utf-16le')
    struct.pack_into('<16s16sQQQ72s', disk, entries_offset + 128,
                     root_guid, root_unique, root_start, root_end, 0, root_name)

    # Calculate CRC32 of partition array
    part_array_crc = zlib.crc32(bytes(disk[entries_offset : entries_offset + entries_size])) & 0xFFFFFFFF
    disk_guid = uuid.UUID('33333333-3333-3333-3333-333333333333').bytes_le

    # 3. Primary GPT Header (LBA 1)
    struct.pack_into('<8sIIIIQQQQ16sQIII', disk, 512,
        b'EFI PART', 0x00010000, 92, 0, 0,
        1, total_sectors - 1, 34, total_sectors - 34,
        disk_guid, 2, 128, 128, part_array_crc
    )
    hdr_crc = zlib.crc32(bytes(disk[512 : 512 + 92])) & 0xFFFFFFFF
    struct.pack_into('<I', disk, 512 + 16, hdr_crc)

    # 4. Backup Partition Array (LBA total_sectors - 33)
    backup_entries_offset = (total_sectors - 33) * 512
    disk[backup_entries_offset : backup_entries_offset + entries_size] = disk[entries_offset : entries_offset + entries_size]

    # 5. Backup GPT Header (LBA total_sectors - 1)
    backup_hdr_offset = (total_sectors - 1) * 512
    struct.pack_into('<8sIIIIQQQQ16sQIII', disk, backup_hdr_offset,
        b'EFI PART', 0x00010000, 92, 0, 0,
        total_sectors - 1, 1, 34, total_sectors - 34,
        disk_guid, total_sectors - 33, 128, 128, part_array_crc
    )
    bhdr_crc = zlib.crc32(bytes(disk[backup_hdr_offset : backup_hdr_offset + 92])) & 0xFFFFFFFF
    struct.pack_into('<I', disk, backup_hdr_offset + 16, bhdr_crc)

    # 6. Format ESP (FAT32 BPB + FSInfo) at LBA 2048 (40MB, 80608 data clusters)
    esp_offset = esp_start * 512
    disk[esp_offset:esp_offset + 3] = b'\xEB\x58\x90'
    disk[esp_offset + 3:esp_offset + 11] = b'MSWIN4.1'
    struct.pack_into('<H', disk, esp_offset + 11, 512)
    disk[esp_offset + 13] = 1
    struct.pack_into('<H', disk, esp_offset + 14, 32)
    disk[esp_offset + 16] = 2
    disk[esp_offset + 21] = 0xF8
    struct.pack_into('<I', disk, esp_offset + 28, esp_start)
    struct.pack_into('<I', disk, esp_offset + 32, esp_sectors)
    struct.pack_into('<I', disk, esp_offset + 36, 640)
    struct.pack_into('<I', disk, esp_offset + 44, 2)
    struct.pack_into('<H', disk, esp_offset + 48, 1)
    struct.pack_into('<H', disk, esp_offset + 50, 6)
    disk[esp_offset + 66] = 0x29
    struct.pack_into('<I', disk, esp_offset + 67, 0x87654321)
    disk[esp_offset + 71:esp_offset + 82] = b'EFI SYSTEM '
    disk[esp_offset + 82:esp_offset + 90] = b'FAT32   '
    struct.pack_into('<H', disk, esp_offset + 510, 0xAA55)
    disk[esp_offset + (6 * 512) : esp_offset + (7 * 512)] = disk[esp_offset : esp_offset + 512]

    # FSInfo Sector 1 & 7
    struct.pack_into('<I', disk, esp_offset + 512, 0x41615252)
    struct.pack_into('<I', disk, esp_offset + 512 + 484, 0x61417272)
    struct.pack_into('<I', disk, esp_offset + 512 + 488, 80000)
    struct.pack_into('<I', disk, esp_offset + 512 + 492, 3)
    struct.pack_into('<I', disk, esp_offset + 512 + 508, 0xAA550000)
    disk[esp_offset + (7 * 512) : esp_offset + (8 * 512)] = disk[esp_offset + 512 : esp_offset + 1024]

    # FAT1 & FAT2 Root Clusters
    fat1_off = esp_offset + (32 * 512)
    fat2_off = fat1_off + (640 * 512)
    fat_init = b'\xF8\xFF\xFF\x0F\xFF\xFF\xFF\x0F\xFF\xFF\xFF\x0F'
    disk[fat1_off : fat1_off + 12] = fat_init
    disk[fat2_off : fat2_off + 12] = fat_init

    # 7. Format Root (EXT2 Superblock) at LBA 83968
    root_offset = root_start * 512
    sb_offset = root_offset + 1024
    total_blocks = (root_sectors * 512) // 1024
    struct.pack_into('<I', disk, sb_offset + 0, 1024)
    struct.pack_into('<I', disk, sb_offset + 4, total_blocks)
    struct.pack_into('<I', disk, sb_offset + 12, total_blocks - 134)
    struct.pack_into('<I', disk, sb_offset + 16, 1024 - 11)
    struct.pack_into('<I', disk, sb_offset + 20, 1)
    struct.pack_into('<I', disk, sb_offset + 24, 0)
    struct.pack_into('<I', disk, sb_offset + 32, 8192)
    struct.pack_into('<I', disk, sb_offset + 40, 1024)
    struct.pack_into('<H', disk, sb_offset + 56, 0xEF53)
    struct.pack_into('<H', disk, sb_offset + 58, 1)
    struct.pack_into('<I', disk, sb_offset + 84, 11)
    struct.pack_into('<H', disk, sb_offset + 88, 128)
    disk[sb_offset + 120:sb_offset + 131] = b'EQUANT_EXT2'

    with open(filename, "wb") as f:
        f.write(disk)
    print(f"[SUCCESS] Spec-compliant GPT disk created: ESP (40M) + EXT2 (64M) + Free Space (24M)!")

if __name__ == "__main__":
    create_mbr_fat32_disk()
    create_gpt_ext2_disk()
