import struct
import os

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

def create_gpt_ext2_disk(filename="disk_gpt_ext2.img", size_mb=64):
    if os.path.exists(filename):
        print(f"[SKIP] Disk image '{filename}' already exists. Preserving data.")
        return

    print(f"[CREATE] Generating valid {size_mb}MB GPT Dual-Partition disk: '{filename}'...")
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)
    total_sectors = img_size // 512

    # 1. Protective MBR
    struct.pack_into('<H', disk, 510, 0xAA55)
    struct.pack_into('<B', disk, 446 + 4, 0xEE)
    struct.pack_into('<I', disk, 446 + 8, 1)
    struct.pack_into('<I', disk, 446 + 12, total_sectors - 1)

    # 2. GPT Header (LBA 1)
    struct.pack_into('<8sIIIIQQQQ16sQIII', disk, 512,
        b'EFI PART', 0x00010000, 92, 0, 0,
        1, total_sectors - 1, 34, total_sectors - 34,
        b'\x01' * 16, 2, 128, 128, 0
    )

    # 3. Partition 0: ESP (FAT32, 16MB = 32768 sectors)
    esp_guid = bytes([
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    ])
    esp_start = 2048
    esp_sectors = 32768
    esp_end = esp_start + esp_sectors - 1
    struct.pack_into('<16s16sQQQ', disk, 1024, esp_guid, b'\x01' * 16, esp_start, esp_end, 0)

    # Format ESP with FAT32 BPB
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
    struct.pack_into('<I', disk, esp_offset + 36, 256)
    struct.pack_into('<I', disk, esp_offset + 44, 2)
    struct.pack_into('<H', disk, esp_offset + 48, 1)
    struct.pack_into('<H', disk, esp_offset + 50, 6)
    disk[esp_offset + 66] = 0x29
    struct.pack_into('<I', disk, esp_offset + 67, 0x87654321)
    disk[esp_offset + 71:esp_offset + 82] = b'EFI SYSTEM '
    disk[esp_offset + 82:esp_offset + 90] = b'FAT32   '
    struct.pack_into('<H', disk, esp_offset + 510, 0xAA55)

    # 4. Partition 1: Root (EXT2, 32MB = 65536 sectors)
    linux_guid = bytes([
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    ])
    root_start = esp_end + 1
    root_sectors = 65536
    root_end = root_start + root_sectors - 1
    struct.pack_into('<16s16sQQQ', disk, 1024 + 128, linux_guid, b'\x02' * 16, root_start, root_end, 0)

    # EXT2 Superblock on Partition 1
    part_offset = root_start * 512
    sb_offset = part_offset + 1024
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
    print(f"[SUCCESS] Created GPT Disk with ESP (16M) + EXT2 (32M) + Unallocated (16M): '{filename}'")

if __name__ == "__main__":
    create_mbr_fat32_disk()
    create_gpt_ext2_disk()
