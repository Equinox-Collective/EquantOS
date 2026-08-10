// ext2.c - EXT2 Filesystem Driver Implementation
#include "ext2.h"
#include "mbr.h"
#include "../drivers/ata/ata.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "stdio.h"
#include "../drivers/serial/serial.h"

// Volume context state
typedef struct {
    uint32_t partition_lba;
    uint32_t block_size;
    uint32_t sectors_per_block;
    ext2_superblock_t sb;
    ext2_bgd_t bg0;
} ext2_volume_t;

static ext2_volume_t ext2_vol;

// Convert block number to absolute disk LBA sector
static uint32_t ext2_block_to_lba(uint32_t block) {
    return ext2_vol.partition_lba + (block * ext2_vol.sectors_per_block);
}

// Read specific inode data from disk
static void ext2_read_inode(uint32_t inode_num, ext2_inode_t *out_inode) {
    uint32_t inodes_per_group = ext2_vol.sb.s_inodes_per_group;
    uint32_t block_group = (inode_num - 1) / inodes_per_group;
    uint32_t index = (inode_num - 1) % inodes_per_group;

    // Read Block Group Descriptor Table (Group 0 is right after superblock block)
    uint32_t bgd_block = (ext2_vol.block_size == 1024) ? 2 : 1;
    ext2_bgd_t bgd;
    uint8_t sector_buf[512];
    
    // Read BGD sector
    uint32_t lba = ext2_block_to_lba(bgd_block) + (index * sizeof(ext2_inode_t)) / 512;
    // For simplicity, let's read the whole inode table block group 0
    uint32_t inode_table_block = ext2_vol.bg0.bg_inode_table;
    uint32_t inode_offset = index * sizeof(ext2_inode_t);
    uint32_t block_offset = inode_offset / ext2_vol.block_size;
    uint32_t byte_offset = inode_offset % ext2_vol.block_size;

    uint32_t target_block = inode_table_block + block_offset;
    uint8_t *block_buf = (uint8_t *)kmalloc(ext2_vol.block_size);
    
    read_sectors_ata_pio((uintptr_t)block_buf, ext2_block_to_lba(target_block), ext2_vol.sectors_per_block);
    memcpy(out_inode, block_buf + byte_offset, sizeof(ext2_inode_t));

    kfree(block_buf);
}

// Read block data for file contents
static int64_t ext2_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    uint32_t inode_num = (uint32_t)(uintptr_t)node->ptr;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);

    if (offset >= inode.i_size) return 0;
    uint64_t bytes_to_read = size;
    if (offset + size > inode.i_size) {
        bytes_to_read = inode.i_size - offset;
    }

    uint32_t bs = ext2_vol.block_size;
    uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
    uint64_t bytes_read = 0;

    while (bytes_read < bytes_to_read) {
        uint64_t file_off = offset + bytes_read;
        uint32_t block_idx = file_off / bs;
        uint32_t block_in_ino = inode.i_block[block_idx]; // Direct block pointer (simplest case)

        read_sectors_ata_pio((uintptr_t)blk_buf, ext2_block_to_lba(block_in_ino), ext2_vol.sectors_per_block);

        uint64_t chunk_off = file_off % bs;
        uint64_t chunk_size = bs - chunk_off;
        if (chunk_size > bytes_to_read - bytes_read) {
            chunk_size = bytes_to_read - bytes_read;
        }

        memcpy(buffer + bytes_read, blk_buf + chunk_off, chunk_size);
        bytes_read += chunk_size;
    }

    kfree(blk_buf);
    return bytes_read;
}

// VFS Readdir operation for EXT2 directories
static vfs_node_t *ext2_readdir(vfs_node_t *node, uint32_t index) {
    uint32_t inode_num = (uint32_t)(uintptr_t)node->ptr;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);

    if (!(inode.i_mode & 0x4000)) return NULL; // Not a directory

    uint32_t bs = ext2_vol.block_size;
    uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
    uint32_t current_index = 0;

    // Read first direct block of directory
    read_sectors_ata_pio((uintptr_t)blk_buf, ext2_block_to_lba(inode.i_block[0]), ext2_vol.sectors_per_block);

    uint32_t offset = 0;
    while (offset < inode.i_size && offset < bs) {
        ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(blk_buf + offset);
        if (entry->inode == 0 || entry->rec_len == 0) break;

        // Skip '.' and '..' entries
        if (!(entry->name_len == 1 && entry->name[0] == '.') && 
            !(entry->name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.')) {
            
            if (current_index == index) {
                vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                memcpy(vnode->name, entry->name, entry->name_len);
                vnode->name[entry->name_len] = '\0';
                vnode->inode = entry->inode;
                vnode->ptr = (vfs_node_t *)(uintptr_t)entry->inode;

                if (entry->file_type == EXT2_FT_DIR) {
                    vnode->flags = FS_DIRECTORY;
                } else {
                    vnode->flags = FS_FILE;
                }

                static vfs_file_operations_t ext2_fops = {
                    .read = ext2_read,
                    .write = NULL,
                    .open = NULL,
                    .close = NULL,
                    .readdir = ext2_readdir,
                    .finddir = NULL
                };
                vnode->ops = &ext2_fops;

                kfree(blk_buf);
                return vnode;
            }
            current_index++;
        }
        offset += entry->rec_len;
    }

    kfree(blk_buf);
    return NULL;
}

vfs_node_t *ext2_mount_partition(uint32_t partition_lba) {
    ext2_vol.partition_lba = partition_lba;

    uint8_t sector_buf[1024];
    // Superblock is at offset 1024 bytes (sectors partition_lba + 2 for 512-byte sectors)
    read_sectors_ata_pio((uintptr_t)sector_buf, partition_lba + 2, 2);
    memcpy(&ext2_vol.sb, sector_buf, sizeof(ext2_superblock_t));

    if (ext2_vol.sb.s_magic != EXT2_SUPER_MAGIC) {
        serial_puts(COM1, "[EXT2 ERROR] Invalid EXT2 magic number!\n");
        return NULL;
    }

    ext2_vol.block_size = 1024 << ext2_vol.sb.s_log_block_size;
    ext2_vol.sectors_per_block = ext2_vol.block_size / 512;

    serial_puts(COM1, "[EXT2] Valid superblock found! Block size: ");
    char buf[16];
    itoa(ext2_vol.block_size, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, " bytes\n");

    // Read Block Group Descriptor 0 (located right after superblock block)
    uint32_t bgd_lba = ext2_block_to_lba((ext2_vol.block_size == 1024) ? 2 : 1);
    read_sectors_ata_pio((uintptr_t)sector_buf, bgd_lba, ext2_vol.sectors_per_block);
    memcpy(&ext2_vol.bg0, sector_buf, sizeof(ext2_bgd_t));

    serial_puts(COM1, "[EXT2] Block Group 0 Inode Table block address loaded successfully.\n");

    // Create Root VFS Node for EXT2 (Root directory inode is always #2)
    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->length = 0;
    root->ptr = (vfs_node_t *)2; // Root inode is 2

    static vfs_file_operations_t ext2_root_fops = {
        .read = ext2_read,
        .write = NULL,
        .open = NULL,
        .close = NULL,
        .readdir = ext2_readdir,
        .finddir = NULL
    };
    root->ops = &ext2_root_fops;

    return root;
}

void ext2_init(void) {
    serial_puts(COM1, "[EXT2] Initializing EXT2 filesystem driver...\n");
    
    int p_count = mbr_get_partition_count();
    for (int i = 0; i < p_count; i++) {
        partition_info_t *part = mbr_get_partition(i);
        if (part && part->type == 0x83) { // 0x83 is Linux native partition (EXT2/3/4)
            serial_puts(COM1, "[EXT2] Found Linux native partition (0x83). Mounting...\n");
            vfs_node_t *ext2_root = ext2_mount_partition(part->start_lba);
            if (ext2_root) {
                // Mount EXT2 under /ext2 or root
                vfs_node_t *ext2_dir = ramfs_create_directory(vfs_root, "ext2");
                ext2_dir->flags |= FS_MOUNTPOINT;
                ext2_dir->ptr = (struct vfs_node *)ext2_root;
                serial_puts(COM1, "[EXT2] Filesystem successfully mounted at /ext2!\n");
                break;
            }
        }
    }
}