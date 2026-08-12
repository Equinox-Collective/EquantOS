import struct
import os

def create_mbr_fat32_disk(filename="disk_mbr_fat32.img", size_mb=32):
    """Generates a 32MB disk image formatted with MBR partition table and a FAT32 filesystem."""
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)

    # --------------------------------------------------------------------------
    # 1. LBA 0: Master Boot Record (MBR)
    # --------------------------------------------------------------------------
    # MBR Boot Signature
    struct.pack_into('<H', disk, 510, 0xAA55)
    
    # Primary Partition Entry #1 (Offset 446)
    part_start_lba = 2048  # 1MB offset alignment
    part_sectors = (img_size // 512) - part_start_lba
    
    struct.pack_into('<B', disk, 446 + 0, 0x80)            # Boot indicator (Active)
    struct.pack_into('<B', disk, 446 + 4, 0x0C)            # Partition Type: 0x0C (FAT32 LBA)
    struct.pack_into('<I', disk, 446 + 8, part_start_lba)   # Starting LBA
    struct.pack_into('<I', disk, 446 + 12, part_sectors)   # Total Sector Count

    # --------------------------------------------------------------------------
    # 2. LBA 2048: FAT32 Volume Boot Record (VBR / BPB)
    # --------------------------------------------------------------------------
    fat_offset = part_start_lba * 512
    
    # Jump instruction & OEM Name
    disk[fat_offset:fat_offset + 3] = b'\xEB\x58\x90'
    disk[fat_offset + 3:fat_offset + 11] = b'MSWIN4.1'
    
    # BIOS Parameter Block (BPB)
    bytes_per_sector = 512
    sectors_per_cluster = 8  # 4KB cluster size
    reserved_sectors = 32
    num_fats = 2
    fat_size_sectors = 256
    root_cluster = 2
    
    struct.pack_into('<H', disk, fat_offset + 11, bytes_per_sector)
    disk[fat_offset + 13] = sectors_per_cluster
    struct.pack_into('<H', disk, fat_offset + 14, reserved_sectors)
    disk[fat_offset + 16] = num_fats
    disk[fat_offset + 21] = 0xF8                            # Media Descriptor (Fixed Hard Disk)
    struct.pack_into('<I', disk, fat_offset + 28, part_start_lba) # Hidden Sectors
    struct.pack_into('<I', disk, fat_offset + 32, part_sectors)  # Total Sectors 32
    
    # FAT32 Extended BPB
    struct.pack_into('<I', disk, fat_offset + 36, fat_size_sectors) # Sectors Per FAT32
    struct.pack_into('<I', disk, fat_offset + 44, root_cluster)      # Root Directory Cluster
    struct.pack_into('<H', disk, fat_offset + 48, 1)                 # FSInfo Sector
    struct.pack_into('<H', disk, fat_offset + 50, 6)                 # Backup Boot Sector
    disk[fat_offset + 66] = 0x29                                      # Extended Boot Signature
    struct.pack_into('<I', disk, fat_offset + 67, 0x12345678)        # Volume ID
    disk[fat_offset + 71:fat_offset + 82] = b'EQUANT FAT32'          # Volume Label
    disk[fat_offset + 82:fat_offset + 90] = b'FAT32   '              # System ID String
    
    # Boot Sector Signature
    struct.pack_into('<H', disk, fat_offset + 510, 0xAA55)

    with open(filename, "wb") as f:
        f.write(disk)
    print(f"[SUCCESS] Created MBR + FAT32 Disk Image: '{filename}' ({size_mb} MB)")


def create_gpt_ext2_disk(filename="disk_gpt_ext2.img", size_mb=32):
    """Generates a 32MB disk image formatted with GPT partition table and an EXT2 filesystem."""
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)
    total_sectors = img_size // 512

    # --------------------------------------------------------------------------
    # 1. LBA 0: Protective MBR
    # --------------------------------------------------------------------------
    struct.pack_into('<H', disk, 510, 0xAA55)
    struct.pack_into('<B', disk, 446 + 4, 0xEE)            # OS Type: 0xEE (Protective MBR)
    struct.pack_into('<I', disk, 446 + 8, 1)               # Starting LBA
    struct.pack_into('<I', disk, 446 + 12, total_sectors - 1)

    # --------------------------------------------------------------------------
    # 2. LBA 1: GPT Header
    # --------------------------------------------------------------------------
    gpt_sig = b'EFI PART'
    struct.pack_into('<8sIIIIQQQQ16sQIII', disk, 512,
        gpt_sig,            # Signature
        0x00010000,         # Revision 1.0
        92,                 # Header size
        0,                  # CRC32 (stub)
        0,                  # Reserved
        1,                  # Current LBA (1)
        total_sectors - 1,  # Backup LBA
        34,                 # First Usable LBA
        total_sectors - 34, # Last Usable LBA
        b'\x01' * 16,       # Disk GUID
        2,                  # Partition Entries LBA (2)
        128,                # Number of Partition Entries
        128,                # Partition Entry Size
        0                   # CRC32 of Partition Array (stub)
    )

    # --------------------------------------------------------------------------
    # 3. LBA 2: GPT Partition Entry #0 (Linux Filesystem GUID)
    # --------------------------------------------------------------------------
    # Linux Filesystem Data GUID: 0FC63DA0-8483-4772-8E79-3D69D8477DE4
    linux_guid = bytes([
        0xA0, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    ])
    part_guid = b'\x02' * 16
    part_start_lba = 2048
    part_end_lba = total_sectors - 34

    struct.pack_into('<16s16sQQQ', disk, 1024,
        linux_guid,         # Partition Type GUID
        part_guid,          # Unique Partition GUID
        part_start_lba,     # Starting LBA (2048)
        part_end_lba,       # Ending LBA
        0                   # Attributes
    )

    # --------------------------------------------------------------------------
    # 4. LBA 2048 + 2 (Offset 1024 bytes into partition): EXT2 Superblock
    # --------------------------------------------------------------------------
    part_offset = part_start_lba * 512
    sb_offset = part_offset + 1024  # EXT2 Superblock is always at byte offset 1024
    
    block_size = 1024  # 1KB block size (s_log_block_size = 0)
    
    struct.pack_into('<I', disk, sb_offset + 0, 1024)      # s_inodes_count
    struct.pack_into('<I', disk, sb_offset + 4, 30000)     # s_blocks_count
    struct.pack_into('<I', disk, sb_offset + 12, 29000)   # s_free_blocks_count
    struct.pack_into('<I', disk, sb_offset + 16, 1000)    # s_free_inodes_count
    struct.pack_into('<I', disk, sb_offset + 20, 1)       # s_first_data_block (1 for 1KB block size)
    struct.pack_into('<I', disk, sb_offset + 24, 0)       # s_log_block_size (0 -> 1024 bytes)
    struct.pack_into('<I', disk, sb_offset + 32, 8192)    # s_blocks_per_group
    struct.pack_into('<I', disk, sb_offset + 40, 1024)    # s_inodes_per_group
    struct.pack_into('<H', disk, sb_offset + 56, 0xEF53)   # s_magic (EXT2 Magic Number!)
    struct.pack_into('<H', disk, sb_offset + 58, 1)        # s_state (Clean)
    struct.pack_into('<I', disk, sb_offset + 84, 11)      # s_first_ino
    struct.pack_into('<H', disk, sb_offset + 88, 128)     # s_inode_size

    # --------------------------------------------------------------------------
    # 5. Block Group Descriptor (BGD) at Block 2 (Offset 2048 bytes into partition)
    # --------------------------------------------------------------------------
    bgd_offset = part_offset + 2048
    struct.pack_into('<I', disk, bgd_offset + 0, 3)        # bg_block_bitmap
    struct.pack_into('<I', disk, bgd_offset + 4, 4)        # bg_inode_bitmap
    struct.pack_into('<I', disk, bgd_offset + 8, 5)        # bg_inode_table
    struct.pack_into('<H', disk, bgd_offset + 12, 29000)  # bg_free_blocks_count
    struct.pack_into('<H', disk, bgd_offset + 14, 1000)   # bg_free_inodes_count
    struct.pack_into('<H', disk, bgd_offset + 16, 1)      # bg_used_dirs_count

    # --------------------------------------------------------------------------
    # 6. Root Inode (Inode #2) in Inode Table at Block 5 (Offset 5120 bytes)
    # --------------------------------------------------------------------------
    inode_table_offset = part_offset + (5 * block_size)
    root_inode_offset = inode_table_offset + (1 * 128) # Inode #2 (index 1)
    
    struct.pack_into('<H', disk, root_inode_offset + 0, 0x41ED) # i_mode (Directory, 0755)
    struct.pack_into('<I', disk, root_inode_offset + 4, 1024)   # i_size
    struct.pack_into('<H', disk, root_inode_offset + 26, 2)    # i_links_count
    struct.pack_into('<I', disk, root_inode_offset + 28, 2)    # i_blocks (512-byte sectors)
    struct.pack_into('<I', disk, root_inode_offset + 40, 6)    # i_block[0] -> Block 6

    # --------------------------------------------------------------------------
    # 7. Root Directory Entries at Block 6 (Offset 6144 bytes)
    # --------------------------------------------------------------------------
    root_dir_offset = part_offset + (6 * block_size)
    
    # Entry 1: "." (Current Dir)
    struct.pack_into('<I', disk, root_dir_offset + 0, 2)       # inode 2
    struct.pack_into('<H', disk, root_dir_offset + 4, 12)      # rec_len
    disk[root_dir_offset + 6] = 1                              # name_len
    disk[root_dir_offset + 7] = 2                              # file_type (Directory)
    disk[root_dir_offset + 8:root_dir_offset + 9] = b'.'
    
    # Entry 2: ".." (Parent Dir)
    struct.pack_into('<I', disk, root_dir_offset + 12, 2)      # inode 2
    struct.pack_into('<H', disk, root_dir_offset + 14, 1012)   # rec_len
    disk[root_dir_offset + 16] = 2                             # name_len
    disk[root_dir_offset + 17] = 2                             # file_type (Directory)
    disk[root_dir_offset + 18:root_dir_offset + 20] = b'..'

    with open(filename, "wb") as f:
        f.write(disk)
    print(f"[SUCCESS] Created GPT + EXT2 Disk Image: '{filename}' ({size_mb} MB)")


if __name__ == "__main__":
    create_mbr_fat32_disk()
    create_gpt_ext2_disk()
