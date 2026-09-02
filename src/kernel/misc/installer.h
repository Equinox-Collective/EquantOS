// src/kernel/misc/installer.h - Modular Storage Discovery and UEFI Installer Engine
#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdint.h>
#include <stdbool.h>
#include "../fs/partition.h"
#include "../drivers/disk/block.h"

#define INSTALLER_MAX_DISKS 4
#define INSTALLER_MAX_NAME  64

typedef enum {
    INSTALL_STRATEGY_AUTO_GPT,     // Wipe entire disk, create UEFI ESP + EXT2 Root
    INSTALL_STRATEGY_MANUAL_PART   // Install root to an existing selected partition
} install_strategy_t;

typedef enum {
    STATE_WELCOME,
    STATE_SELECT_DRIVE,
    STATE_SELECT_STRATEGY,
    STATE_SELECT_PARTITION,
    STATE_CONFIGURE_SYSTEM,
    STATE_CONFIRM_ACTIONS,
    STATE_EXECUTE_INSTALL,
    STATE_FINISH,
    STATE_ERROR
} install_state_t;

typedef struct {
    char            name[INSTALLER_MAX_NAME];
    block_device_t  bdev;
    uint64_t        total_sectors;
    uint32_t        sector_size;
    bool            is_nvme;
} installer_disk_t;

typedef struct {
    char hostname[32];
    int  boot_timeout;
    char boot_cmdline[128];
} system_config_t;

typedef struct {
    install_state_t    current_state;
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

// TUI primitives
void tui_draw_box(int col, int row, int width, int height, const char *title, uint32_t fg, uint32_t bg);
void tui_draw_progress(int col, int row, int width, int percent, uint32_t fg, uint32_t bg);
int  tui_select_menu(int col, int row, int width, const char *title, const char **items, int count);
bool tui_dialog_confirm(const char *title, const char *warning_text);
void tui_input_string(int col, int row, int max_len, char *out_buf);

void installer_log_verbose(const char *fmt, ...);
void installer_run(void);

#endif // INSTALLER_H