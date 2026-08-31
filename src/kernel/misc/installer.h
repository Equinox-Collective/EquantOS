// src/kernel/misc/installer.h - Advanced Verbose TUI Installer Engine
#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdint.h>
#include <stdbool.h>
#include "../fs/partition.h"
#include "../drivers/disk/block.h"

typedef enum {
    STATE_WELCOME,
    STATE_SELECT_DISK,
    STATE_CONFIRM_FORMAT,
    STATE_PARTITION_FORMAT,
    STATE_COPY_FILES,
    STATE_INSTALL_BOOTLOADER,
    STATE_FINISH,
    STATE_ERROR
} install_state_t;

typedef struct {
    install_state_t current_state;
    block_device_t  target_dev;
    partition_info_t target_partition;
    int              selected_partition_idx;
    bool             is_nvme;
    char             error_msg[256];
    int              progress_percent;
} installer_ctx_t;

void tui_draw_box(int col, int row, int width, int height, const char *title, uint32_t fg, uint32_t bg);
void tui_draw_progress(int col, int row, int width, int percent, uint32_t fg, uint32_t bg);
int  tui_select_menu(int col, int row, int width, const char *title, const char **items, int count);
bool tui_dialog_confirm(const char *title, const char *warning_text);

void installer_log_verbose(const char *fmt, ...);
void installer_run(void);

#endif // INSTALLER_H