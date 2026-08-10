// ext2.h - EXT2 Filesystem Driver Definitions for EquantOS
#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

#define EXT2_SUPER_MAGIC 0xEF53

// EXT2 Superblock structure (located at byte offset 1024 from partition start)
typedef struct {
    uint32_t s_inodes_count;         // Total inodes count
    uint32_t s_blocks_count;         // Total blocks count
    uint32_t s_r_blocks_count;       // Reserved blocks count
    uint32_t s_free_blocks_count;    // Free blocks count
    uint32_t s_free_inodes_count;    // Free inodes count
    uint32_t s_first_data_block;     // First Data Block
    uint32_t s_log_block_size;       // Block size calculation: 1024 << s_log_block_size
    uint32_t s_log_frag_size;        // Fragment size
    uint32_t s_blocks_per_group;     // Blocks per group
    uint32_t s_frags_per_group;      // Fragments per group
    uint32_t s_inodes_per_group;     // Inodes per group
    uint32_t s_mtime;                // Mount time
    uint32_t s_wtime;                // Write time
    uint16_t s_mnt_count;            // Mount count
    uint16_t s_max_mnt_count;        // Maximal mount count
    uint16_t s_magic;                // Magic signature (must be 0xEF53)
    uint16_t s_state;                // File system state
    uint16_t s_errors;               // Behaviour when detecting errors
    uint16_t s_minor_rev_level;      // Minor revision level
    uint32_t s_lastcheck;            // Time of last check
    uint32_t s_checkinterval;        // Maximum time between checks
    uint32_t s_creator_os;           // Creator OS
    uint32_t s_rev_level;            // Revision level
    uint16_t s_def_resuid;           // Default UID for reserved blocks
    uint16_t s_def_resgid;           // Default GID for reserved blocks
    // --- Extended fields ---
    uint32_t s_first_ino;            // First non-reserved inode
    uint16_t s_inode_size;           // Size of inode structure
    uint16_t s_block_group_nr;       // Block group # of this superblock
    uint32_t s_feature_compat;       // Compatible feature set
    uint32_t s_feature_incompat;     // Incompatible feature set
    uint32_t s_feature_ro_compat;    // Read-only compatible feature set
    uint8_t  s_uuid[16];             // Disk UUID
    char     s_volume_name[16];      // Volume name
    char     s_last_mounted[64];     // Path where last mounted
    uint32_t s_algo_bitmap;          // For compression
} __attribute__((packed)) ext2_superblock_t;

// Block Group Descriptor
typedef struct {
    uint32_t bg_block_bitmap;        // Block address of block bitmap
    uint32_t bg_inode_bitmap;        // Block address of inode bitmap
    uint32_t bg_inode_table;         // Block address of inode table
    uint16_t bg_free_blocks_count;   // Free blocks count
    uint16_t bg_free_inodes_count;   // Free inodes count
    uint16_t bg_used_dirs_count;     // Directories count
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

// EXT2 Inode structure
typedef struct {
    uint16_t i_mode;                 // File type and access rights
    uint16_t i_uid;                  // User ID
    uint32_t i_size;                 // Size in bytes
    uint32_t i_atime;                // Access time
    uint32_t i_ctime;                // Creation time
    uint32_t i_mtime;                // Modification time
    uint32_t i_dtime;                // Deletion time
    uint16_t i_gid;                  // Group ID
    uint16_t i_links_count;          // Links count
    uint32_t i_blocks;               // Blocks count (in 512-byte sectors)
    uint32_t i_flags;                // File flags
    uint32_t i_osd1;                 // OS dependent 1
    uint32_t i_block[15];            // Block pointers (12 direct, 1 singly, 1 doubly, 1 triply indirect)
    uint32_t i_generation;           // File version
    uint32_t i_file_acl;             // File ACL
    uint32_t i_dir_acl;              // Directory ACL
    uint32_t i_faddr;                // Fragment address
    uint8_t  osd2[12];               // OS dependent 2
} __attribute__((packed)) ext2_inode_t;

// EXT2 Directory Entry
typedef struct {
    uint32_t inode;                  // Inode number
    uint16_t rec_len;                // Directory entry length
    uint8_t  name_len;               // Name length
    uint8_t  file_type;              // File type
    char     name[];                 // Variable length file name
} __attribute__((packed)) ext2_dir_entry_t;

// File types
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_SYMLINK  7

void ext2_init(void);
vfs_node_t *ext2_mount_partition(uint32_t partition_lba);

#endif // EXT2_H