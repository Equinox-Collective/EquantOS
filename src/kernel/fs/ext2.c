// ext2.c - EXT2 Filesystem Driver Implementation with Block Device Abstraction
#include "ext2.h"
#include "mbr.h"
#include "partition.h"
#include "../drivers/disk/block.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "stdio.h"
#include "../drivers/serial/serial.h"
#include "../core/initcall.h"

typedef struct {
    block_device_t dev;
    uint32_t partition_lba;
    uint32_t block_size;
    uint32_t sectors_per_block;
    ext2_superblock_t sb;
    ext2_bgd_t bg0;
} ext2_volume_t;

static ext2_volume_t ext2_vol;

// Forward declarations
static int64_t ext2_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static int64_t ext2_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static vfs_node_t *ext2_readdir(vfs_node_t *node, uint32_t index);
static vfs_node_t *ext2_finddir(vfs_node_t *node, const char *name);
static vfs_node_t *ext2_create(vfs_node_t *dir, const char *name, uint32_t flags);

static vfs_file_operations_t ext2_fops = {
    .read = ext2_read,
    .write = ext2_write,
    .open = NULL,
    .close = NULL,
    .readdir = ext2_readdir,
    .finddir = ext2_finddir,
    .create = ext2_create
};

static uint32_t ext2_block_to_lba(uint32_t block) {
    return ext2_vol.partition_lba + (block * ext2_vol.sectors_per_block);
}

static void ext2_write_superblock(void) {
    uint8_t sector_buf[1024];
    memset(sector_buf, 0, 1024);
    memcpy(sector_buf, &ext2_vol.sb, sizeof(ext2_superblock_t));
    ext2_vol.dev.write(ext2_vol.partition_lba + 2, 2, sector_buf);
}

static void ext2_write_bgd(void) {
    uint32_t bgd_lba = ext2_block_to_lba((ext2_vol.block_size == 1024) ? 2 : 1);
    uint8_t *sector_buf = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!sector_buf) return;
    ext2_vol.dev.read(bgd_lba, ext2_vol.sectors_per_block, sector_buf);
    memcpy(sector_buf, &ext2_vol.bg0, sizeof(ext2_bgd_t));
    ext2_vol.dev.write(bgd_lba, ext2_vol.sectors_per_block, sector_buf);
    kfree(sector_buf);
}

static void ext2_read_inode(uint32_t inode_num, ext2_inode_t *out_inode) {
    if (inode_num == 0) return;

    uint32_t inodes_per_group = ext2_vol.sb.s_inodes_per_group;
    uint32_t local_index = (inode_num - 1) % inodes_per_group;

    uint32_t inode_table_block = ext2_vol.bg0.bg_inode_table;
    uint32_t inode_size = ext2_vol.sb.s_inode_size ? ext2_vol.sb.s_inode_size : 128;
    
    uint32_t inode_offset = local_index * inode_size;
    uint32_t block_offset = inode_offset / ext2_vol.block_size;
    uint32_t byte_offset = inode_offset % ext2_vol.block_size;

    uint32_t target_block = inode_table_block + block_offset;
    uint8_t *block_buf = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!block_buf) return;
    
    ext2_vol.dev.read(ext2_block_to_lba(target_block), ext2_vol.sectors_per_block, block_buf);
    memcpy(out_inode, block_buf + byte_offset, sizeof(ext2_inode_t));

    kfree(block_buf);
}

static void ext2_write_inode(uint32_t inode_num, ext2_inode_t *inode) {
    if (inode_num == 0) return;

    uint32_t inodes_per_group = ext2_vol.sb.s_inodes_per_group;
    uint32_t local_index = (inode_num - 1) % inodes_per_group;

    uint32_t inode_table_block = ext2_vol.bg0.bg_inode_table;
    uint32_t inode_size = ext2_vol.sb.s_inode_size ? ext2_vol.sb.s_inode_size : 128;

    uint32_t inode_offset = local_index * inode_size;
    uint32_t block_offset = inode_offset / ext2_vol.block_size;
    uint32_t byte_offset = inode_offset % ext2_vol.block_size;

    uint32_t target_block = inode_table_block + block_offset;
    uint8_t *block_buf = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!block_buf) return;

    ext2_vol.dev.read(ext2_block_to_lba(target_block), ext2_vol.sectors_per_block, block_buf);
    memcpy(block_buf + byte_offset, inode, sizeof(ext2_inode_t));
    ext2_vol.dev.write(ext2_block_to_lba(target_block), ext2_vol.sectors_per_block, block_buf);

    kfree(block_buf);
}

static uint32_t ext2_alloc_block(void) {
    uint8_t *bitmap = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!bitmap) return 0;

    uint32_t bitmap_block = ext2_vol.bg0.bg_block_bitmap;
    ext2_vol.dev.read(ext2_block_to_lba(bitmap_block), ext2_vol.sectors_per_block, bitmap);

    uint32_t total_blocks = ext2_vol.sb.s_blocks_per_group;
    uint32_t allocated_block = 0;

    for (uint32_t i = 0; i < total_blocks; i++) {
        uint32_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;
        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            bitmap[byte_idx] |= (1 << bit_idx);
            allocated_block = ext2_vol.sb.s_first_data_block + i;
            break;
        }
    }

    if (allocated_block != 0) {
        ext2_vol.dev.write(ext2_block_to_lba(bitmap_block), ext2_vol.sectors_per_block, bitmap);
        ext2_vol.bg0.bg_free_blocks_count--;
        ext2_vol.sb.s_free_blocks_count--;
        ext2_write_bgd();
        ext2_write_superblock();
    }

    kfree(bitmap);
    return allocated_block;
}

static uint32_t ext2_alloc_inode(void) {
    uint8_t *bitmap = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!bitmap) return 0;

    uint32_t bitmap_block = ext2_vol.bg0.bg_inode_bitmap;
    ext2_vol.dev.read(ext2_block_to_lba(bitmap_block), ext2_vol.sectors_per_block, bitmap);

    uint32_t total_inodes = ext2_vol.sb.s_inodes_per_group;
    uint32_t allocated_inode = 0;

    for (uint32_t i = 0; i < total_inodes; i++) {
        uint32_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;
        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            bitmap[byte_idx] |= (1 << bit_idx);
            allocated_inode = i + 1;
            break;
        }
    }

    if (allocated_inode != 0) {
        ext2_vol.dev.write(ext2_block_to_lba(bitmap_block), ext2_vol.sectors_per_block, bitmap);
        ext2_vol.bg0.bg_free_inodes_count--;
        ext2_vol.sb.s_free_inodes_count--;
        ext2_write_bgd();
        ext2_write_superblock();
    }

    kfree(bitmap);
    return allocated_inode;
}

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
    if (!blk_buf) return -1;
    uint64_t bytes_read = 0;

    while (bytes_read < bytes_to_read) {
        uint64_t file_off = offset + bytes_read;
        uint32_t block_idx = file_off / bs;
        uint32_t block_in_ino = inode.i_block[block_idx];

        ext2_vol.dev.read(ext2_block_to_lba(block_in_ino), ext2_vol.sectors_per_block, blk_buf);

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

static int64_t ext2_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    uint32_t inode_num = (uint32_t)(uintptr_t)node->ptr;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);

    uint32_t bs = ext2_vol.block_size;
    uint64_t bytes_written = 0;

    while (bytes_written < size) {
        uint64_t file_off = offset + bytes_written;
        uint32_t block_idx = file_off / bs;
        
        if (block_idx >= 12) {
            break;
        }

        uint32_t block_num = inode.i_block[block_idx];
        if (block_num == 0) {
            block_num = ext2_alloc_block();
            if (block_num == 0) break;
            inode.i_block[block_idx] = block_num;
        }

        uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
        if (!blk_buf) break;

        ext2_vol.dev.read(ext2_block_to_lba(block_num), ext2_vol.sectors_per_block, blk_buf);

        uint64_t chunk_off = file_off % bs;
        uint64_t chunk_size = bs - chunk_off;
        if (chunk_size > size - bytes_written) {
            chunk_size = size - bytes_written;
        }

        memcpy(blk_buf + chunk_off, buffer + bytes_written, chunk_size);
        ext2_vol.dev.write(ext2_block_to_lba(block_num), ext2_vol.sectors_per_block, blk_buf);

        bytes_written += chunk_size;
        kfree(blk_buf);

        if (offset + bytes_written > inode.i_size) {
            inode.i_size = offset + bytes_written;
        }
    }

    inode.i_blocks = ((inode.i_size + 511) / 512);
    ext2_write_inode(inode_num, &inode);
    node->length = inode.i_size;
    return bytes_written;
}

static int ext2_add_dir_entry(uint32_t dir_inode_num, uint32_t new_inode_num, const char *name, uint8_t file_type) {
    ext2_inode_t dir_inode;
    ext2_read_inode(dir_inode_num, &dir_inode);

    uint32_t bs = ext2_vol.block_size;
    uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
    if (!blk_buf) return 0;

    uint8_t name_len = strlen(name);
    uint16_t required_len = (sizeof(ext2_dir_entry_t) + name_len + 3) & ~3;
    int added = 0;

    for (int b = 0; b < 12; b++) {
        uint32_t dir_block = dir_inode.i_block[b];
        
        if (dir_block == 0) {
            dir_block = ext2_alloc_block();
            if (dir_block == 0) break;
            dir_inode.i_block[b] = dir_block;
            dir_inode.i_size += bs;
            dir_inode.i_blocks += ext2_vol.sectors_per_block;
            ext2_write_inode(dir_inode_num, &dir_inode);
            
            memset(blk_buf, 0, bs);
            ext2_dir_entry_t *first_entry = (ext2_dir_entry_t *)blk_buf;
            first_entry->inode = 0;
            first_entry->rec_len = bs;
        } else {
            ext2_vol.dev.read(ext2_block_to_lba(dir_block), ext2_vol.sectors_per_block, blk_buf);
        }

        uint32_t offset = 0;
        while (offset < bs) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(blk_buf + offset);
            if (entry->rec_len == 0) break;

            uint16_t min_entry_len = (sizeof(ext2_dir_entry_t) + entry->name_len + 3) & ~3;
            if (entry->inode == 0) {
                min_entry_len = sizeof(ext2_dir_entry_t);
            }

            if (entry->rec_len - min_entry_len >= required_len) {
                uint16_t old_rec_len = entry->rec_len;
                entry->rec_len = min_entry_len;

                uint32_t new_entry_offset = offset + min_entry_len;
                ext2_dir_entry_t *new_entry = (ext2_dir_entry_t *)(blk_buf + new_entry_offset);
                new_entry->inode = new_inode_num;
                new_entry->rec_len = old_rec_len - min_entry_len;
                new_entry->name_len = name_len;
                new_entry->file_type = file_type;
                memcpy(new_entry->name, name, name_len);

                added = 1;
                break;
            }

            offset += entry->rec_len;
        }

        if (added) {
            ext2_vol.dev.write(ext2_block_to_lba(dir_block), ext2_vol.sectors_per_block, blk_buf);
            break;
        }
    }

    kfree(blk_buf);
    return added;
}

static vfs_node_t *ext2_readdir(vfs_node_t *node, uint32_t index) {
    uint32_t inode_num = (uint32_t)(uintptr_t)node->ptr;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);

    if (!(inode.i_mode & EXT2_S_IFDIR)) return NULL;

    uint32_t bs = ext2_vol.block_size;
    uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
    if (!blk_buf) return NULL;
    uint32_t current_index = 0;

    for (int b = 0; b < 12; b++) {
        uint32_t dir_block = inode.i_block[b];
        if (dir_block == 0) break;

        ext2_vol.dev.read(ext2_block_to_lba(dir_block), ext2_vol.sectors_per_block, blk_buf);

        uint32_t offset = 0;
        while (offset < bs) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(blk_buf + offset);
            if (entry->rec_len == 0) break;

            if (entry->inode != 0) {
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

                        ext2_inode_t child_ino;
                        ext2_read_inode(entry->inode, &child_ino);
                        vnode->length = child_ino.i_size;
                        vnode->ops = &ext2_fops;

                        kfree(blk_buf);
                        return vnode;
                    }
                    current_index++;
                }
            }
            offset += entry->rec_len;
        }
    }

    kfree(blk_buf);
    return NULL;
}

static vfs_node_t *ext2_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    (void)flags;
    uint32_t dir_inode_num = (uint32_t)(uintptr_t)dir->ptr;

    uint32_t new_inode_num = ext2_alloc_inode();
    if (new_inode_num == 0) return NULL;

    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(ext2_inode_t));
    new_inode.i_mode = EXT2_S_IFREG | 0644;
    new_inode.i_size = 0;
    new_inode.i_links_count = 1;
    new_inode.i_blocks = 0;
    ext2_write_inode(new_inode_num, &new_inode);

    if (!ext2_add_dir_entry(dir_inode_num, new_inode_num, name, EXT2_FT_REG_FILE)) {
        return NULL;
    }

    vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strncpy(vnode->name, name, sizeof(vnode->name) - 1);
    vnode->flags = FS_FILE;
    vnode->length = 0;
    vnode->inode = new_inode_num;
    vnode->ptr = (vfs_node_t *)(uintptr_t)new_inode_num;
    vnode->ops = &ext2_fops;

    return vnode;
}

static vfs_node_t *ext2_finddir(vfs_node_t *node, const char *name) {
    uint32_t inode_num = (uint32_t)(uintptr_t)node->ptr;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);

    if (!(inode.i_mode & EXT2_S_IFDIR)) return NULL;

    uint32_t bs = ext2_vol.block_size;
    uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
    if (!blk_buf) return NULL;

    for (int b = 0; b < 12; b++) {
        uint32_t dir_block = inode.i_block[b];
        if (dir_block == 0) break;

        ext2_vol.dev.read(ext2_block_to_lba(dir_block), ext2_vol.sectors_per_block, blk_buf);

        uint32_t offset = 0;
        while (offset < bs) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(blk_buf + offset);
            if (entry->rec_len == 0) break;

            if (entry->inode != 0) {
                if (entry->name_len == strlen(name) && memcmp(entry->name, name, entry->name_len) == 0) {
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

                    ext2_inode_t child_ino;
                    ext2_read_inode(entry->inode, &child_ino);
                    vnode->length = child_ino.i_size;
                    vnode->ops = &ext2_fops;

                    kfree(blk_buf);
                    return vnode;
                }
            }
            offset += entry->rec_len;
        }
    }

    kfree(blk_buf);
    return NULL;
}

vfs_node_t *ext2_mount_partition(block_device_t dev, uint32_t partition_lba) {
    ext2_vol.dev = dev;
    ext2_vol.partition_lba = partition_lba;

    uint8_t sector_buf[1024];
    ext2_vol.dev.read(partition_lba + 2, 2, sector_buf);
    memcpy(&ext2_vol.sb, sector_buf, sizeof(ext2_superblock_t));

    if (ext2_vol.sb.s_magic != EXT2_SUPER_MAGIC) {
        serial_puts(COM1, "[EXT2 ERROR] Invalid EXT2 magic number!\n");
        return NULL;
    }

    ext2_vol.block_size = 1024 << ext2_vol.sb.s_log_block_size;
    ext2_vol.sectors_per_block = ext2_vol.block_size / 512;

    uint32_t bgd_lba = ext2_block_to_lba((ext2_vol.block_size == 1024) ? 2 : 1);
    ext2_vol.dev.read(bgd_lba, ext2_vol.sectors_per_block, sector_buf);
    memcpy(&ext2_vol.bg0, sector_buf, sizeof(ext2_bgd_t));

    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->length = 0;
    root->ptr = (vfs_node_t *)2;
    root->ops = &ext2_fops;

    return root;
}

void ext2_init(void) {
    serial_puts(COM1, "[EXT2] Initializing EXT2 file system driver...\n");
}

int mkfs_ext2(block_device_t dev, uint32_t start_lba, uint32_t sector_count, const char *vol_label) {
    if (!dev.write || sector_count < 2048) { // Require at least ~1MB size
        serial_puts(COM1, "[MKFS-EXT2 ERROR] Invalid device or sector count too small!\n");
        return -1;
    }

    uint32_t block_size = 1024; // Standard 1KB block size
    uint32_t sectors_per_block = block_size / 512; // 2 sectors per block
    uint32_t total_blocks = sector_count / sectors_per_block;

    if (total_blocks < 100) {
        serial_puts(COM1, "[MKFS-EXT2 ERROR] Total block count too low for Ext2 layout!\n");
        return -1;
    }

    uint32_t blocks_per_group = 8192;
    uint32_t inodes_per_group = 1024;
    uint32_t num_groups = (total_blocks + blocks_per_group - 1) / blocks_per_group;

    if (num_groups > 1) {
        // Keep single block group for simplicity during installation target
        num_groups = 1;
        total_blocks = blocks_per_group;
    }

    uint32_t total_inodes = inodes_per_group * num_groups;
    uint32_t inode_size = 128;
    uint32_t inode_table_blocks = (inodes_per_group * inode_size) / block_size; // 128 blocks for inodes

    // Layout for Group 0:
    // Block 0: Reserved for MBR / Bootloader (LBA start_lba .. start_lba+1)
    // Block 1: Superblock (LBA start_lba + 2)
    // Block 2: Block Group Descriptor Table (LBA start_lba + 4)
    // Block 3: Block Bitmap
    // Block 4: Inode Bitmap
    // Block 5..(5 + inode_table_blocks - 1): Inode Table (Blocks 5..132)
    // Block (5 + inode_table_blocks): Root Directory Data Block (#2)

    uint32_t block_bitmap_blk = 3;
    uint32_t inode_bitmap_blk = 4;
    uint32_t inode_table_blk  = 5;
    uint32_t root_dir_blk     = 5 + inode_table_blocks;

    uint32_t reserved_system_blocks = root_dir_blk + 1; // All blks up to & including root dir data

    // 1. Prepare Superblock
    ext2_superblock_t sb;
    memset(&sb, 0, sizeof(ext2_superblock_t));

    sb.s_inodes_count       = total_inodes;
    sb.s_blocks_count       = total_blocks;
    sb.s_r_blocks_count     = 0;
    sb.s_free_blocks_count  = total_blocks - reserved_system_blocks;
    sb.s_free_inodes_count  = total_inodes - 11; // Reserved inodes 1..10 used, 11 free
    sb.s_first_data_block   = 1; // 1KB block size offset
    sb.s_log_block_size     = 0; // 1024 << 0 = 1024
    sb.s_log_frag_size      = 0;
    sb.s_blocks_per_group   = blocks_per_group;
    sb.s_frags_per_group    = blocks_per_group;
    sb.s_inodes_per_group   = inodes_per_group;
    sb.s_magic              = EXT2_SUPER_MAGIC; // 0xEF53
    sb.s_state              = EXT2_VALID_FS;
    sb.s_errors             = 1; // Continue on errors
    sb.s_minor_rev_level    = 0;
    sb.s_rev_level          = 0; // Revision 0
    sb.s_first_ino          = 11;
    sb.s_inode_size         = 128;
    sb.s_block_group_nr     = 0;

    if (vol_label) {
        strncpy(sb.s_volume_name, vol_label, 15);
    } else {
        strcpy(sb.s_volume_name, "EQUANT_EXT2");
    }

    // Write Superblock to Block 1 (LBA start_lba + 2)
    uint8_t *block_buf = (uint8_t *)kzalloc(block_size);
    if (!block_buf) return -1;

    memcpy(block_buf, &sb, sizeof(ext2_superblock_t));
    dev.write(start_lba + (1 * sectors_per_block), sectors_per_block, block_buf);

    // 2. Prepare Block Group Descriptor Table (BGD)
    ext2_bgd_t bgd;
    memset(&bgd, 0, sizeof(ext2_bgd_t));

    bgd.bg_block_bitmap      = block_bitmap_blk;
    bgd.bg_inode_bitmap      = inode_bitmap_blk;
    bgd.bg_inode_table       = inode_table_blk;
    bgd.bg_free_blocks_count = (uint16_t)sb.s_free_blocks_count;
    bgd.bg_free_inodes_count = (uint16_t)sb.s_free_inodes_count;
    bgd.bg_used_dirs_count   = 1; // Root directory

    memset(block_buf, 0, block_size);
    memcpy(block_buf, &bgd, sizeof(ext2_bgd_t));
    dev.write(start_lba + (2 * sectors_per_block), sectors_per_block, block_buf);

    // 3. Write Block Bitmap
    memset(block_buf, 0, block_size);
    for (uint32_t i = 0; i < reserved_system_blocks; i++) {
        block_buf[i / 8] |= (1 << (i % 8));
    }
    dev.write(start_lba + (block_bitmap_blk * sectors_per_block), sectors_per_block, block_buf);

    // 4. Write Inode Bitmap
    memset(block_buf, 0, block_size);
    // Mark inodes 1..10 as used
    for (uint32_t i = 0; i < 10; i++) {
        block_buf[i / 8] |= (1 << (i % 8));
    }
    dev.write(start_lba + (inode_bitmap_blk * sectors_per_block), sectors_per_block, block_buf);

    // 5. Zero out Inode Table
    memset(block_buf, 0, block_size);
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        dev.write(start_lba + ((inode_table_blk + i) * sectors_per_block), sectors_per_block, block_buf);
    }

    // 6. Write Root Inode (#2) into Inode Table Block 5 (Offset 128 bytes)
    ext2_inode_t root_inode;
    memset(&root_inode, 0, sizeof(ext2_inode_t));

    root_inode.i_mode        = EXT2_S_IFDIR | 0755;
    root_inode.i_size        = block_size;
    root_inode.i_links_count = 2; // '.' and '..'
    root_inode.i_blocks      = sectors_per_block;
    root_inode.i_block[0]    = root_dir_blk;

    dev.read(start_lba + (inode_table_blk * sectors_per_block), sectors_per_block, block_buf);
    memcpy(block_buf + inode_size, &root_inode, sizeof(ext2_inode_t)); // Inode 2 is index 1 -> offset 128
    dev.write(start_lba + (inode_table_blk * sectors_per_block), sectors_per_block, block_buf);

    // 7. Initialize Root Directory Data Block
    memset(block_buf, 0, block_size);

    ext2_dir_entry_t *entry_dot = (ext2_dir_entry_t *)block_buf;
    entry_dot->inode     = 2;
    entry_dot->rec_len   = 12;
    entry_dot->name_len = 1;
    entry_dot->file_type = EXT2_FT_DIR;
    entry_dot->name[0]   = '.';

    ext2_dir_entry_t *entry_dotdot = (ext2_dir_entry_t *)(block_buf + 12);
    entry_dotdot->inode     = 2;
    entry_dotdot->rec_len   = block_size - 12; // Occupies remaining block space
    entry_dotdot->name_len = 2;
    entry_dotdot->file_type = EXT2_FT_DIR;
    entry_dotdot->name[0]   = '.';
    entry_dotdot->name[1]   = '.';

    dev.write(start_lba + (root_dir_blk * sectors_per_block), sectors_per_block, block_buf);

    kfree(block_buf);
    serial_puts(COM1, "[MKFS-EXT2 SUCCESS] Formatted target partition with Ext2 File System!\n");
    return 0;
}

// // THIS SHOULD BELONG TO BOTTOM, DO NOT REWRITE IN ANY CASE // //

static int __init ext2_fs_initcall(void) {
    ext2_init();
    return 0;
}
fs_initcall(ext2_fs_initcall);