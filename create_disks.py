import struct
import os

def create_mbr_fat32_disk(filename="disk_mbr_fat32.img", size_mb=64):
    """Generates a 64MB disk image formatted with MBR partition table and a FAT32 filesystem."""
    if os.path.exists(filename):
        print(f"[SKIP] Disk image '{filename}' already exists. Preserving data.")
        return

    print(f"[CREATE] Generating new {size_mb}MB FAT32 disk image: '{filename}'...")
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)

    # 1. LBA 0: Master Boot Record (MBR)
    struct.pack_into('<H', disk, 510, 0xAA55)
    
    part_start_lba = 2048  # 1MB offset alignment
    part_sectors = (img_size // 512) - part_start_lba
    
    struct.pack_into('<B', disk, 446 + 0, 0x80)            # Active flag
    struct.pack_into('<B', disk, 446 + 4, 0x0C)            # Type: 0x0C (FAT32 LBA)
    struct.pack_into('<I', disk, 446 + 8, part_start_lba)   # Starting LBA
    struct.pack_into('<I', disk, 446 + 12, part_sectors)   # Sector count

    # 2. LBA 2048: FAT32 Volume Boot Record (VBR / BPB)
    fat_offset = part_start_lba * 512
    disk[fat_offset:fat_offset + 3] = b'\xEB\x58\x90'
    disk[fat_offset + 3:fat_offset + 11] = b'MSWIN4.1'
    
    bytes_per_sector = 512
    sectors_per_cluster = 8  # 4KB cluster size
    reserved_sectors = 32
    num_fats = 2
    fat_size_sectors = 512
    root_cluster = 2
    
    struct.pack_into('<H', disk, fat_offset + 11, bytes_per_sector)
    disk[fat_offset + 13] = sectors_per_cluster
    struct.pack_into('<H', disk, fat_offset + 14, reserved_sectors)
    disk[fat_offset + 16] = num_fats
    disk[fat_offset + 21] = 0xF8
    struct.pack_into('<I', disk, fat_offset + 28, part_start_lba)
    struct.pack_into('<I', disk, fat_offset + 32, part_sectors)
    
    struct.pack_into('<I', disk, fat_offset + 36, fat_size_sectors)
    struct.pack_into('<I', disk, fat_offset + 44, root_cluster)
    struct.pack_into('<H', disk, fat_offset + 48, 1)
    struct.pack_into('<H', disk, fat_offset + 50, 6)
    disk[fat_offset + 66] = 0x29
    struct.pack_into('<I', disk, fat_offset + 67, 0x12345678)
    disk[fat_offset + 71:fat_offset + 82] = b'EQUANT FAT32'
    disk[fat_offset + 82:fat_offset + 90] = b'FAT32   '
    
    struct.pack_into('<H', disk, fat_offset + 510, 0xAA55)

    with open(filename, "wb") as f:
        f.write(disk)
    print(f"[SUCCESS] Created MBR + FAT32 Disk Image: '{filename}' ({size_mb} MB)")


def create_gpt_ext2_disk(filename="disk_gpt_ext2.img", size_mb=64):
    """Generates a 64MB disk image formatted with GPT partition table and a standard EXT2 filesystem."""
    if os.path.exists(filename):
        print(f"[SKIP] Disk image '{filename}' already exists. Preserving data.")
        return

    print(f"[CREATE] Generating valid {size_mb}MB EXT2 disk image: '{filename}'...")
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)
    total_sectors = img_size // 512

    # 1. LBA 0: Protective MBR
    struct.pack_into('<H', disk, 510, 0xAA55)
    struct.pack_into('<B', disk, 446 + 4, 0xEE)
    struct.pack_into('<I', disk, 446 + 8, 1)
    struct.pack_into('<I', disk, 446 + 12, total_sectors - 1)

    # 2. LBA 1: GPT Header
    gpt_sig = b'EFI PART'
    struct.pack_into('<8sIIIIQQQQ16sQIII', disk, 512,
        gpt_sig, 0x00010000, 92, 0, 0,
        1, total_sectors - 1, 34, total_sectors - 34,
        b'\x01' * 16, 2, 128, 128, 0
    )

    # 3. LBA 2: GPT Partition Entry #0
    linux_guid = bytes([
        0xA0, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    ])
    part_guid = b'\x02' * 16
    part_start_lba = 2048
    part_end_lba = total_sectors - 34

    struct.pack_into('<16s16sQQQ', disk, 1024,
        linux_guid, part_guid, part_start_lba, part_end_lba, 0
    )

    # 4. EXT2 Superblock (Block Size 1KB)
    part_offset = part_start_lba * 512
    sb_offset = part_offset + 1024
    block_size = 1024
    
    inodes_count = 1024
    total_blocks = (img_size - part_offset) // block_size
    inode_table_blocks = (inodes_count * 128) // block_size  # 128 blocks (5..132)
    root_dir_blk = 5 + inode_table_blocks                    # Block 133
    system_blocks = root_dir_blk + 1                         # 134 blocks (0..133)

    struct.pack_into('<I', disk, sb_offset + 0, inodes_count)               # s_inodes_count
    struct.pack_into('<I', disk, sb_offset + 4, total_blocks)               # s_blocks_count
    struct.pack_into('<I', disk, sb_offset + 12, total_blocks - system_blocks) # s_free_blocks_count
    struct.pack_into('<I', disk, sb_offset + 16, inodes_count - 11)         # s_free_inodes_count
    struct.pack_into('<I', disk, sb_offset + 20, 1)                         # s_first_data_block
    struct.pack_into('<I', disk, sb_offset + 24, 0)                         # s_log_block_size (0 = 1KB)
    struct.pack_into('<I', disk, sb_offset + 32, 8192)                      # s_blocks_per_group
    struct.pack_into('<I', disk, sb_offset + 40, inodes_count)              # s_inodes_per_group
    struct.pack_into('<H', disk, sb_offset + 56, 0xEF53)                    # Magic
    struct.pack_into('<H', disk, sb_offset + 58, 1)                         # Valid state
    struct.pack_into('<I', disk, sb_offset + 84, 11)                        # s_first_ino (11)
    struct.pack_into('<H', disk, sb_offset + 88, 128)                       # s_inode_size
    disk[sb_offset + 120:sb_offset + 131] = b'EQUANT_EXT2'

    # 5. Block Group Descriptor (Block 2)
    bgd_offset = part_offset + 2048
    struct.pack_into('<I', disk, bgd_offset + 0, 3)                         # bg_block_bitmap = Block 3
    struct.pack_into('<I', disk, bgd_offset + 4, 4)                         # bg_inode_bitmap = Block 4
    struct.pack_into('<I', disk, bgd_offset + 8, 5)                         # bg_inode_table  = Block 5
    struct.pack_into('<H', disk, bgd_offset + 12, total_blocks - system_blocks) # bg_free_blocks_count
    struct.pack_into('<H', disk, bgd_offset + 14, inodes_count - 11)        # bg_free_inodes_count
    struct.pack_into('<H', disk, bgd_offset + 16, 1)                        # bg_used_dirs_count

    # 6. Block Bitmap (Block 3) - Помечаем блоки 0..133 как занятые
    block_bitmap_offset = part_offset + (3 * block_size)
    for b in range(system_blocks):
        disk[block_bitmap_offset + (b // 8)] |= (1 << (b % 8))

    # 7. Inode Bitmap (Block 4) - Помечаем иноды 1..10 как занятые (резерв)
    inode_bitmap_offset = part_offset + (4 * block_size)
    for ino in range(10):
        disk[inode_bitmap_offset + (ino // 8)] |= (1 << (ino % 8))

    # 8. Root Inode (#2) в Таблице Инодов (Блок 5, смещение 128)
    inode_table_offset = part_offset + (5 * block_size)
    root_inode_offset = inode_table_offset + (1 * 128)
    
    struct.pack_into('<H', disk, root_inode_offset + 0, 0x41ED) # Mode: Directory 0755
    struct.pack_into('<I', disk, root_inode_offset + 4, block_size) # Size: 1024
    struct.pack_into('<H', disk, root_inode_offset + 26, 2)     # Links count: 2 (. and ..)
    struct.pack_into('<I', disk, root_inode_offset + 28, 2)     # Sectors: 2
    struct.pack_into('<I', disk, root_inode_offset + 40, root_dir_blk) # i_block[0] = Block 133!
# 9. Root Directory Data (Блок 133)
    root_dir_offset = part_offset + (root_dir_blk * block_size)
    
    # Entry '.' (Смещение 0..11)
    struct.pack_into('<I', disk, root_dir_offset + 0, 2)        # Inode 2
    struct.pack_into('<H', disk, root_dir_offset + 4, 12)       # Rec len: 12
    disk[root_dir_offset + 6] = 1                               # Name len: 1
    disk[root_dir_offset + 7] = 2                               # File type: Dir
    disk[root_dir_offset + 8:root_dir_offset + 9] = b'.'
    
    # Entry '..' (Смещение 12..1023)
    struct.pack_into('<I', disk, root_dir_offset + 12, 2)       # Inode 2 (offset +0)
    struct.pack_into('<H', disk, root_dir_offset + 14, 1012)    # Rec len: 1012 (offset +2..+3)
    disk[root_dir_offset + 18] = 2                              # Name len: 2 (offset +6 -> 12+6 = 18)
    disk[root_dir_offset + 19] = 2                              # File type: Dir (offset +7 -> 12+7 = 19)
    disk[root_dir_offset + 20:root_dir_offset + 22] = b'..'     # Name: '..' (offset +8 -> 12+8 = 20..21)

    with open(filename, "wb") as f:
        f.write(disk)
    print(f"[SUCCESS] Created valid GPT + EXT2 Disk Image: '{filename}' ({size_mb} MB)")


if __name__ == "__main__":
    create_mbr_fat32_disk()
    create_gpt_ext2_disk()
