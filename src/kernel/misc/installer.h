#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../fs/partition.h"
#include "../drivers/disk/block.h"

#define INSTALLER_MAX_DISKS      8
#define INSTALLER_MAX_REGIONS    32
#define INSTALLER_MAX_NAME       64

#define COLOR_ARCH_CYAN          0x001793D1
#define COLOR_ARCH_DARK          0x00151719
#define COLOR_ARCH_HEADER_BG     0x000F1012
#define COLOR_ARCH_PANEL         0x001E2126
#define COLOR_ARCH_BORDER        0x001793D1
#define COLOR_ARCH_BORDER_DIM    0x003A414D
#define COLOR_ARCH_TEXT          0x00ECEFF4
#define COLOR_ARCH_MUTED         0x007E8494
#define COLOR_ARCH_SEL_BG        0x001793D1
#define COLOR_ARCH_SEL_FG        0x00FFFFFF
#define COLOR_ARCH_VALUE         0x0088C0D0
#define COLOR_ARCH_KEY           0x00EBCB8B
#define COLOR_ARCH_GREEN         0x00A3BE8C
#define COLOR_ARCH_WARN          0x00BF616A
#define COLOR_ARCH_DANGER        0x00FF3333
#define COLOR_ARCH_WIN_BLUE      0x004A90E2
#define COLOR_ARCH_RECOVERY_GRAY 0x005E6573

typedef enum {
    REGION_TYPE_FREE_SPACE = 0,
    REGION_TYPE_ESP,
    REGION_TYPE_WINDOWS_NTFS,
    REGION_TYPE_MSR,
    REGION_TYPE_RECOVERY,
    REGION_TYPE_LINUX_ROOT,
    REGION_TYPE_UNKNOWN
} disk_region_type_t;

typedef enum {
    FS_TARGET_EXT2 = 0,
    FS_TARGET_FAT32
} target_fs_type_t;

typedef enum {
    INSTALL_ACTION_USE_FREE_SPACE = 0,
    INSTALL_ACTION_OVERWRITE_PARTITION,
    INSTALL_ACTION_WIPE_ENTIRE_DISK
} install_action_t;

typedef struct {
    disk_region_type_t type;
    int                part_index; // -1 if unallocated free space
    uint64_t           start_lba;
    uint64_t           sector_count;
    char               name[48];
    char               fs_label[32];
    bool               is_writable_target;
    bool               is_esp;
} disk_region_t;

typedef struct {
    char               name[INSTALLER_MAX_NAME];
    char               dev_node[16];
    block_device_t     bdev;
    uint64_t           total_sectors;
    uint32_t           sector_size;
    bool               is_nvme;
    bool               has_gpt;

    disk_region_t      regions[INSTALLER_MAX_REGIONS];
    int                region_count;
    int                esp_region_idx; // -1 if no ESP found
} installer_disk_t;

typedef struct {
    char               hostname[64];
    char               root_password[64];
    bool               autologin;
    int                boot_timeout;
    target_fs_type_t   root_fs;
    bool               reuse_existing_esp;
    int                selected_esp_idx;
} installer_config_t;

typedef struct {
    installer_disk_t   disks[INSTALLER_MAX_DISKS];
    int                disk_count;
    int                selected_disk_idx;
    int                selected_region_idx;

    install_action_t   action;
    bool               is_uefi_mode;

    uint64_t           target_root_start;
    uint64_t           target_root_sectors;

    uint64_t           target_esp_start;
    uint64_t           target_esp_sectors;

    installer_config_t cfg;
    char               error_msg[256];
} installer_session_t;

void installer_run(void);

#endif