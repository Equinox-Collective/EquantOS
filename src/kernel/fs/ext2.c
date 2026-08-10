// ext2.c - EXT2 Filesystem Driver Implementation
#include "ext2.h"
#include "mbr.h"
#include "../drivers/disk/ata.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "stdio.h"
#include "../drivers/serial/serial.h"

typedef struct {
    uint8_t drive;
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
    write_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)sector_buf, ext2_vol.partition_lba + 2, 2);
}

static void ext2_write_bgd(void) {
    uint32_t bgd_lba = ext2_block_to_lba((ext2_vol.block_size == 1024) ? 2 : 1);
    uint8_t *sector_buf = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!sector_buf) return;
    read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)sector_buf, bgd_lba, ext2_vol.sectors_per_block);
    memcpy(sector_buf, &ext2_vol.bg0, sizeof(ext2_bgd_t));
    write_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)sector_buf, bgd_lba, ext2_vol.sectors_per_block);
    kfree(sector_buf);
}

static void ext2_read_inode(uint32_t inode_num, ext2_inode_t *out_inode) {
    uint32_t inodes_per_group = ext2_vol.sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) / inodes_per_group;

    uint32_t inode_table_block = ext2_vol.bg0.bg_inode_table;
    uint32_t inode_offset = index * sizeof(ext2_inode_t);
    uint32_t block_offset = inode_offset / ext2_vol.block_size;
    uint32_t byte_offset = inode_offset % ext2_vol.block_size;

    uint32_t target_block = inode_table_block + block_offset;
    uint8_t *block_buf = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!block_buf) return;
    
    read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)block_buf, ext2_block_to_lba(target_block), ext2_vol.sectors_per_block);
    memcpy(out_inode, block_buf + byte_offset, sizeof(ext2_inode_t));

    kfree(block_buf);
}

static void ext2_write_inode(uint32_t inode_num, ext2_inode_t *inode) {
    uint32_t inodes_per_group = ext2_vol.sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) / inodes_per_group;

    uint32_t inode_table_block = ext2_vol.bg0.bg_inode_table;
    uint32_t inode_offset = index * sizeof(ext2_inode_t);
    uint32_t block_offset = inode_offset / ext2_vol.block_size;
    uint32_t byte_offset = inode_offset % ext2_vol.block_size;

    uint32_t target_block = inode_table_block + block_offset;
    uint8_t *block_buf = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!block_buf) return;

    read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)block_buf, ext2_block_to_lba(target_block), ext2_vol.sectors_per_block);
    memcpy(block_buf + byte_offset, inode, sizeof(ext2_inode_t));
    write_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)block_buf, ext2_block_to_lba(target_block), ext2_vol.sectors_per_block);

    kfree(block_buf);
}

static uint32_t ext2_alloc_block(void) {
    uint8_t *bitmap = (uint8_t *)kmalloc(ext2_vol.block_size);
    if (!bitmap) return 0;

    uint32_t bitmap_block = ext2_vol.bg0.bg_block_bitmap;
    read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)bitmap, ext2_block_to_lba(bitmap_block), ext2_vol.sectors_per_block);

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
        write_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)bitmap, ext2_block_to_lba(bitmap_block), ext2_vol.sectors_per_block);

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
    read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)bitmap, ext2_block_to_lba(bitmap_block), ext2_vol.sectors_per_block);

    uint32_t total_inodes = ext2_vol.sb.s_inodes_per_group;
    uint32_t allocated_inode = 0;

    for (uint32_t i = 0; i < total_inodes; i++) {
        uint32_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;
        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            bitmap[byte_idx] |= (1 << bit_idx);
            allocated_inode = i + 1; // Inodes are 1-indexed
            break;
        }
    }

    if (allocated_inode != 0) {
        write_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)bitmap, ext2_block_to_lba(bitmap_block), ext2_vol.sectors_per_block);

        ext2_vol.bg0.bg_free_inodes_count--;
        ext2_vol.sb.s_free_inodes_count--;
        ext2_write_bgd();
        ext2_write_superblock();
    }

    kfree(bitmap);
    return allocated_inode;
}

static int ext2_add_dir_entry(uint32_t dir_inode_num, uint32_t new_inode_num, const char *name, uint8_t file_type) {
    ext2_inode_t dir_inode;
    ext2_read_inode(dir_inode_num, &dir_inode);

    uint32_t bs = ext2_vol.block_size;
    uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
    if (!blk_buf) return 0;

    uint8_t name_len = strlen(name);
    uint16_t required_len = (sizeof(ext2_dir_entry_t) + name_len + 3) & ~3;

    uint32_t dir_block = dir_inode.i_block[0];
    if (dir_block == 0) {
        dir_block = ext2_alloc_block();
        if (dir_block == 0) {
            kfree(blk_buf);
            return 0;
        }
        dir_inode.i_block[0] = dir_block;
        dir_inode.i_size = bs;
        dir_inode.i_blocks = ext2_vol.sectors_per_block;
        ext2_write_inode(dir_inode_num, &dir_inode);
        memset(blk_buf, 0, bs);
    } else {
        read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)blk_buf, ext2_block_to_lba(dir_block), ext2_vol.sectors_per_block);
    }

    uint32_t offset = 0;
    int added = 0;

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
        write_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)blk_buf, ext2_block_to_lba(dir_block), ext2_vol.sectors_per_block);
    }

    kfree(blk_buf);
    return added;
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

        read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)blk_buf, ext2_block_to_lba(block_in_ino), ext2_vol.sectors_per_block);

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
            serial_puts(COM1, "[EXT2 ERROR] Indirect blocks write not supported yet!\n");
            break; // Currently supports 12 direct blocks (~48KB)
        }

        uint32_t block_num = inode.i_block[block_idx];
        if (block_num == 0) {
            block_num = ext2_alloc_block();
            if (block_num == 0) {
                serial_puts(COM1, "[EXT2 ERROR] No space left on device (Out of blocks)!\n");
                break;
            }
            inode.i_block[block_idx] = block_num;
        }

        uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
        if (!blk_buf) break;

        read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)blk_buf, ext2_block_to_lba(block_num), ext2_vol.sectors_per_block);

        uint64_t chunk_off = file_off % bs;
        uint64_t chunk_size = bs - chunk_off;
        if (chunk_size > size - bytes_written) {
            chunk_size = size - bytes_written;
        }

        memcpy(blk_buf + chunk_off, buffer + bytes_written, chunk_size);

        write_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)blk_buf, ext2_block_to_lba(block_num), ext2_vol.sectors_per_block);

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

static vfs_node_t *ext2_readdir(vfs_node_t *node, uint32_t index) {
    uint32_t inode_num = (uint32_t)(uintptr_t)node->ptr;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);

    if (!(inode.i_mode & EXT2_S_IFDIR)) return NULL;

    uint32_t bs = ext2_vol.block_size;
    uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
    if (!blk_buf) return NULL;
    uint32_t current_index = 0;

    read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)blk_buf, ext2_block_to_lba(inode.i_block[0]), ext2_vol.sectors_per_block);

    uint32_t offset = 0;
    while (offset < inode.i_size && offset < bs) {
        ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(blk_buf + offset);
        if (entry->inode == 0 || entry->rec_len == 0) break;

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
        offset += entry->rec_len;
    }

    kfree(blk_buf);
    return NULL;
}

static vfs_node_t *ext2_finddir(vfs_node_t *node, const char *name) {
    uint32_t inode_num = (uint32_t)(uintptr_t)node->ptr;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);

    if (!(inode.i_mode & EXT2_S_IFDIR)) return NULL;

    uint32_t bs = ext2_vol.block_size;
    uint8_t *blk_buf = (uint8_t *)kmalloc(bs);
    if (!blk_buf) return NULL;

    if (inode.i_block[0] == 0) {
        kfree(blk_buf);
        return NULL;
    }

    read_sectors_ata_pio_drive(ext2_vol.drive, (uintptr_t)blk_buf, ext2_block_to_lba(inode.i_block[0]), ext2_vol.sectors_per_block);

    uint32_t offset = 0;
    while (offset < inode.i_size && offset < bs) {
        ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(blk_buf + offset);
        if (entry->inode == 0 || entry->rec_len == 0) break;

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
        offset += entry->rec_len;
    }

    kfree(blk_buf);
    return NULL;
}

static vfs_node_t *ext2_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    (void)flags;
    uint32_t dir_inode_num = (uint32_t)(uintptr_t)dir->ptr;

    uint32_t new_inode_num = ext2_alloc_inode();
    if (new_inode_num == 0) {
        serial_puts(COM1, "[EXT2 ERROR] Failed to allocate inode!\n");
        return NULL;
    }

    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(ext2_inode_t));
    new_inode.i_mode = EXT2_S_IFREG | 0644;
    new_inode.i_size = 0;
    new_inode.i_links_count = 1;
    new_inode.i_blocks = 0;
    ext2_write_inode(new_inode_num, &new_inode);

    if (!ext2_add_dir_entry(dir_inode_num, new_inode_num, name, EXT2_FT_REG_FILE)) {
        serial_puts(COM1, "[EXT2 ERROR] Failed to add directory entry!\n");
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

vfs_node_t *ext2_mount_partition(uint8_t drive, uint32_t partition_lba) {
    ext2_vol.drive = drive;
    ext2_vol.partition_lba = partition_lba;

    uint8_t sector_buf[1024];
    read_sectors_ata_pio_drive(drive, (uintptr_t)sector_buf, partition_lba + 2, 2);
    memcpy(&ext2_vol.sb, sector_buf, sizeof(ext2_superblock_t));

    if (ext2_vol.sb.s_magic != EXT2_SUPER_MAGIC) {
        serial_puts(COM1, "[EXT2 ERROR] Invalid EXT2 magic number!\n");
        return NULL;
    }

    ext2_vol.block_size = 1024 << ext2_vol.sb.s_log_block_size;
    ext2_vol.sectors_per_block = ext2_vol.block_size / 512;

    serial_puts(COM1, "[EXT2] Valid superblock found on Drive ");
    char buf[16];
    itoa(drive, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "! Block size: ");
    itoa(ext2_vol.block_size, 10, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, " bytes\n");

    // Check filesystem clean / dirty state
    if (ext2_vol.sb.s_state != EXT2_VALID_FS) {
        serial_puts(COM1, "[EXT2 WARNING] Volume was NOT cleanly unmounted (DIRTY/IN-USE by another OS)!\n");
    } else {
        serial_puts(COM1, "[EXT2] Volume clean status check PASSED.\n");
    }

    // Mark filesystem active / dirty on mount
    ext2_vol.sb.s_state = EXT2_ERROR_FS;
    ext2_write_superblock();

    uint32_t bgd_lba = ext2_block_to_lba((ext2_vol.block_size == 1024) ? 2 : 1);
    read_sectors_ata_pio_drive(drive, (uintptr_t)sector_buf, bgd_lba, ext2_vol.sectors_per_block);
    memcpy(&ext2_vol.bg0, sector_buf, sizeof(ext2_bgd_t));

    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->length = 0;
    root->ptr = (vfs_node_t *)2; // Root inode is #2
    root->ops = &ext2_fops;

    return root;
}

void ext2_init(void) {
    serial_puts(COM1, "[EXT2] Initializing EXT2 file system driver...\n");
}