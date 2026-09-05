// src/kernel/misc/installer.h - Archinstall-Grade UEFI Storage & Deployment Subsystem
#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../fs/partition.h"
#include "../drivers/disk/block.h"

#define INSTALLER_MAX_DISKS 8
#define INSTALLER_MAX_NAME  64

// Arch Linux Signature Palette (24-bit True Color)
#define COLOR_ARCH_CYAN        0x001793D1
#define COLOR_ARCH_DARK        0x00151719
#define COLOR_ARCH_HEADER_BG   0x000F1012
#define COLOR_ARCH_PANEL       0x001E2126
#define COLOR_ARCH_BORDER      0x001793D1
#define COLOR_ARCH_BORDER_DIM  0x003A414D
#define COLOR_ARCH_TEXT        0x00ECEFF4
#define COLOR_ARCH_MUTED       0x007E8494
#define COLOR_ARCH_SEL_BG      0x001793D1
#define COLOR_ARCH_SEL_FG      0x00FFFFFF
#define COLOR_ARCH_VALUE       0x0088C0D0
#define COLOR_ARCH_KEY         0x00EBCB8B
#define COLOR_ARCH_GREEN       0x00A3BE8C
#define COLOR_ARCH_WARN        0x00BF616A
#define COLOR_ARCH_TITLE_ACCENT 0x0081A1C1

typedef enum {
    INSTALL_STRATEGY_AUTO_GPT,     // Wipe entire disk, partition with GPT (ESP FAT32 + Root EXT2)
    INSTALL_STRATEGY_MANUAL_PART   // Install root to existing designated partition
} install_strategy_t;

typedef enum {
    PROFILE_STANDARD = 0,         // Base OS + Shell + Utilities + POSIX Tools
    PROFILE_MINIMAL  = 1,         // Minimal Kernel + Init Base
    PROFILE_DEV      = 2          // Full Workstation with Diagnostics & Benchmarks
} software_profile_t;

typedef struct {
    char            name[INSTALLER_MAX_NAME];
    char            dev_node[16];
    block_device_t  bdev;
    uint64_t        total_sectors;
    uint32_t        sector_size;
    bool            is_nvme;
} installer_disk_t;

typedef struct {
    char                hostname[64];
    char                root_password[64];
    bool                autologin;
    int                 boot_timeout;
    char                boot_cmdline[128];
    software_profile_t  profile;
    uint32_t            esp_size_mb;
} system_config_t;

typedef struct {
    installer_disk_t   disks[INSTALLER_MAX_DISKS];
    int                disk_count;
    int                selected_disk_idx;

    install_strategy_t strategy;
    partition_info_t   selected_partition;

    // Runtime partition layout parameters
    uint64_t           esp_start_lba;
    uint64_t           esp_sector_count;
    uint64_t           root_start_lba;
    uint64_t           root_sector_count;

    system_config_t    sys_cfg;
    char               error_msg[256];
} installer_ctx_t;

// TUI Declarations
void tui_draw_box(int col, int row, int width, int height, const char *title, uint32_t fg, uint32_t bg);
void tui_draw_progress(int col, int row, int width, int percent, uint32_t fg, uint32_t bg);
int  tui_select_menu(int col, int row, int width, const char *title, const char **items, const char **descs, int count, int initial_sel);
bool tui_dialog_confirm(const char *title, const char *warning_text);
bool tui_input_string(int col, int row, int width, int max_len, char *out_buf, bool is_password);

void installer_log_verbose(const char *fmt, ...);
void installer_run(void);

#endif // INSTALLER_H