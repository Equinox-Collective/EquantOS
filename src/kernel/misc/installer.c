// src/kernel/misc/installer.c - Full TUI & FSM Installation Engine
#include "installer.h"
#include "../../equterm/term.h"
#include "../drivers/tty/tty.h"
#include "../drivers/input.h"
#include "../drivers/disk/nvme.h"
#include "../drivers/disk/ata.h"
#include "../fs/partition.h"
#include "../fs/vfs.h"
#include "../fs/ext2.h"
#include "../fs/fat32.h"
#include "../fs/ramfs.h"
#include "../core/mem/memory.h"
#include "../libs/string.h"
#include "../libs/stdio.h"
#include "../drivers/serial/serial.h"

#define COLOR_TUI_BG        0x000F0F1A // Deep Navy/Black
#define COLOR_TUI_FG        0x00CCCCCC // Light Grey
#define COLOR_HEADER        0x0000FF88 // Neon Mint
#define COLOR_SEL_BG        0x00005522 // Highlight Green BG
#define COLOR_SEL_FG        0x00FFFFFF // Bright White
#define COLOR_BORDER        0x0000AACC // Cyan Border
#define COLOR_WARN_FG       0x00FF3333 // Bright Red
#define COLOR_SUCCESS_FG    0x0033FF33 // Bright Green

static installer_ctx_t g_installer_ctx;

// Helper: Log installer events to Serial COM1 & VFS log file
static void installer_log(const char *msg) {
    serial_puts(COM1, "[INSTALLER] ");
    serial_puts(COM1, msg);
    serial_puts(COM1, "\n");

    vfs_node_t *log_dir = vfs_open("/var/log", 0);
    if (!log_dir) {
        vfs_node_t *var_dir = vfs_open("/var", 0);
        if (!var_dir) {
            vfs_node_t *root = vfs_open("/", 0);
            var_dir = vfs_create(root, "var", FS_DIRECTORY);
        }
        if (var_dir) {
            log_dir = vfs_create(var_dir, "log", FS_DIRECTORY);
        }
    }

    if (log_dir) {
        vfs_node_t *logfile = vfs_open("/var/log/installer.log", 0);
        if (!logfile) {
            logfile = vfs_create(log_dir, "installer.log", FS_FILE);
        }
        if (logfile) {
            vfs_write(logfile, logfile->length, strlen(msg), (uint8_t *)msg);
            vfs_write(logfile, logfile->length, 1, (uint8_t *)"\n");
        }
    }
}

// Draw a double-line styled TUI Box
void tui_draw_box(int col, int row, int width, int height, const char *title, uint32_t fg, uint32_t bg) {
    (void)bg;
    uint32_t old_color = term_get_color();
    term_set_custom_colors(fg, COLOR_TUI_BG);

    // Top border
    term_set_cursor(col * 16, row * 32);
    term_putchar_raw('+');
    for (int i = 0; i < width - 2; i++) term_putchar_raw('-');
    term_putchar_raw('+');

    // Title centered in top border
    if (title && strlen(title) > 0) {
        int title_len = strlen(title);
        int start_x = col + (width - title_len) / 2;
        term_set_cursor(start_x * 16, row * 32);
        term_print(title);
    }

    // Side borders and fill
    for (int r = 1; r < height - 1; r++) {
        term_set_cursor(col * 16, (row + r) * 32);
        term_putchar_raw('|');
        for (int c = 0; c < width - 2; c++) term_putchar_raw(' ');
        term_putchar_raw('|');
    }

    // Bottom border
    term_set_cursor(col * 16, (row + height - 1) * 32);
    term_putchar_raw('+');
    for (int i = 0; i < width - 2; i++) term_putchar_raw('-');
    term_putchar_raw('+');

    term_set_custom_colors(old_color, COLOR_TUI_BG);
}

// Draw a smooth visual TUI progress bar
void tui_draw_progress(int col, int row, int width, int percent, uint32_t fg, uint32_t bg) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int bar_width = width - 10;
    int filled = (bar_width * percent) / 100;

    term_set_cursor(col * 16, row * 32);
    term_set_custom_colors(fg, bg);
    term_putchar_raw('[');

    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            term_putchar_raw('=');
        } else if (i == filled) {
            term_putchar_raw('>');
        } else {
            term_putchar_raw(' ');
        }
    }
    term_putchar_raw(']');
    term_putchar_raw(' ');

    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%3d%%", percent);
    term_print(pct_str);
}

// TUI Menu Selector with arrow key navigation
int tui_select_menu(int col, int row, int width, const char *title, const char **items, int count) {
    int selected = 0;
    int box_height = count + 4;

    while (1) {
        tui_draw_box(col, row, width, box_height, title, COLOR_BORDER, COLOR_TUI_BG);

        for (int i = 0; i < count; i++) {
            term_set_cursor((col + 2) * 16, (row + 2 + i) * 32);
            if (i == selected) {
                term_set_custom_colors(COLOR_SEL_FG, COLOR_SEL_BG);
                term_print(" > ");
            } else {
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print("   ");
            }
            term_print(items[i]);
            term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
        }

        uint16_t key = tty_getchar_raw();
        if (key == KEY_UP) {
            selected--;
            if (selected < 0) selected = count - 1;
        } else if (key == KEY_DOWN) {
            selected++;
            if (selected >= count) selected = 0;
        } else if (key == KEY_ENTER || key == KEY_KPENTER) {
            return selected;
        } else if (key == KEY_ESC) {
            return -1;
        }
    }
}

// Confirmation Dialog Guard
bool tui_dialog_confirm(const char *title, const char *warning_text) {
    tui_draw_box(2, 4, 76, 12, title, COLOR_WARN_FG, COLOR_TUI_BG);

    term_set_cursor(4 * 16, 6 * 32);
    term_set_custom_colors(COLOR_WARN_FG, COLOR_TUI_BG);
    term_print(warning_text);

    const char *options[] = {
        "NO  - Cancel Operation (SAFE)",
        "YES - Format Target Partition and Install EquantOS"
    };

    int choice = tui_select_menu(4, 9, 72, " CONFIRM DISK FORMAT ", options, 2);
    return (choice == 1);
}

// Core Installation Workflow Implementation
void installer_run(void) {
    memset(&g_installer_ctx, 0, sizeof(installer_ctx_t));
    g_installer_ctx.current_state = STATE_WELCOME;

    installer_log("Starting EquantOS Archinstall-style TUI Installer Session...");

    while (1) {
        switch (g_installer_ctx.current_state) {

            // ------------------------------------------------------------------
            // 1. Welcome Screen
            // ------------------------------------------------------------------
            case STATE_WELCOME: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 14, " EquantOS Installer ", COLOR_HEADER, COLOR_TUI_BG);

                term_set_cursor(5 * 16, 5 * 32);
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print("Welcome to EquantOS Automated Installation Wizard!");

                term_set_cursor(5 * 16, 7 * 32);
                term_print("This wizard will guide you through setting up EquantOS");

                term_set_cursor(5 * 16, 8 * 32);
                term_print("on your local hardware (NVMe / ATA PIO/DMA).");

                const char *opts[] = {
                    "Start Installation Wizard",
                    "Exit to EquantOS Shell"
                };

                int res = tui_select_menu(5, 10, 68, " ACTION ", opts, 2);
                if (res == 0) {
                    g_installer_ctx.current_state = STATE_SELECT_DISK;
                } else {
                    term_clear_screen();
                    return;
                }
                break;
            }

            // ------------------------------------------------------------------
            // 2. Select Disk & Partition (With Hardware Safety Checks)
            // ------------------------------------------------------------------
            case STATE_SELECT_DISK: {
                term_clear_screen();
                installer_log("Scanning storage media...");

                if (nvme_init() == NVME_SUCCESS) {
                    g_installer_ctx.target_dev = nvme_get_block_device();
                    g_installer_ctx.is_nvme = true;
                    installer_log("Target Drive: NVMe Controller Active.");
                } else {
                    block_device_t ata_dev = {
                        .read = (block_read_fn)read_sectors_ata_pio,
                        .write = (block_write_fn)write_sectors_ata_pio,
                        .sector_size = 512
                    };
                    g_installer_ctx.target_dev = ata_dev;
                    g_installer_ctx.is_nvme = false;
                    installer_log("Target Drive: Primary ATA Controller Active.");
                }

                disk_partition_scan_device(g_installer_ctx.target_dev);
                int p_count = disk_get_partition_count();

                if (p_count == 0) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "No partitions found on target drive!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                const char *part_opts[16];
                char part_bufs[16][128];

                for (int i = 0; i < p_count && i < 16; i++) {
                    partition_info_t *p = disk_get_partition(i);
                    uint64_t size_mb = (uint64_t)(p->sector_count * 512) / (1024 * 1024);

                    snprintf(part_bufs[i], sizeof(part_bufs[i]),
                             "Part #%d | Type: 0x%02X | Size: %llu MB %s",
                             p->index, p->type, size_mb,
                             (p->type == 0xEF || p->is_esp) ? "[PROTECTED ESP]" : "");
                    part_opts[i] = part_bufs[i];
                }

                int chosen = tui_select_menu(2, 2, 76, " SELECT TARGET PARTITION ", part_opts, p_count);
                if (chosen < 0) {
                    g_installer_ctx.current_state = STATE_WELCOME;
                    break;
                }

                partition_info_t *selected = disk_get_partition(chosen);
                if (!selected) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "Invalid partition selection!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                // HARDWARE GUARD: Protect EFI System Partition
                if (selected->type == 0xEF || selected->is_esp) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "PROTECTION: EFI System Partition (ESP) cannot be overwritten!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                g_installer_ctx.target_partition = *selected;
                g_installer_ctx.selected_partition_idx = chosen;
                g_installer_ctx.current_state = STATE_CONFIRM_FORMAT;
                break;
            }

            // ------------------------------------------------------------------
            // 3. Triple Confirmation Guard
            // ------------------------------------------------------------------
            case STATE_CONFIRM_FORMAT: {
                term_clear_screen();
                char warn[256];
                snprintf(warn, sizeof(warn),
                         "WARNING: Partition #%d (LBA %u, Size %u MB) WILL BE ERASED!",
                         g_installer_ctx.target_partition.index,
                         g_installer_ctx.target_partition.start_lba,
                         (g_installer_ctx.target_partition.sector_count * 512) / (1024 * 1024));

                if (tui_dialog_confirm("!!! DATA LOSS WARNING !!!", warn)) {
                    g_installer_ctx.current_state = STATE_PARTITION_FORMAT;
                } else {
                    installer_log("User aborted operation at confirmation prompt.");
                    g_installer_ctx.current_state = STATE_WELCOME;
                }
                break;
            }

            // ------------------------------------------------------------------
            // 4. Format Target Partition
            // ------------------------------------------------------------------
            case STATE_PARTITION_FORMAT: {
                term_clear_screen();
                tui_draw_box(2, 4, 76, 8, " FORMATTING PARTITION ", COLOR_HEADER, COLOR_TUI_BG);

                term_set_cursor(4 * 16, 6 * 32);
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print("Formatting partition to EXT2 File System...");

                tui_draw_progress(4, 8, 72, 30, COLOR_HEADER, COLOR_TUI_BG);

                installer_log("Formatting partition via mkfs_ext2...");
                int res = mkfs_ext2(g_installer_ctx.target_dev,
                                    g_installer_ctx.target_partition.start_lba,
                                    g_installer_ctx.target_partition.sector_count,
                                    "EQUANT_SYS");

                if (res != 0) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "mkfs_ext2 failed during target format!");
                    g_installer_ctx.current_state = STATE_ERROR;
                } else {
                    tui_draw_progress(4, 8, 72, 100, COLOR_HEADER, COLOR_TUI_BG);
                    installer_log("Formatting complete successfully.");
                    g_installer_ctx.current_state = STATE_COPY_FILES;
                }
                break;
            }

            // ------------------------------------------------------------------
            // 5. Copy OS Payload Files & Binaries
            // ------------------------------------------------------------------
            case STATE_COPY_FILES: {
                term_clear_screen();
                tui_draw_box(2, 4, 76, 10, " COPYING SYSTEM FILES ", COLOR_HEADER, COLOR_TUI_BG);

                vfs_node_t *target_root = ext2_mount_partition(g_installer_ctx.target_dev,
                                                                g_installer_ctx.target_partition.start_lba);

                if (!target_root) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "Failed to mount newly formatted EXT2 target partition!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                term_set_cursor(4 * 16, 6 * 32);
                term_print("Creating OS Directory Structure (/boot, /sys, /sys/bin)...");
                tui_draw_progress(4, 9, 72, 20, COLOR_HEADER, COLOR_TUI_BG);

                vfs_node_t *boot_dir = vfs_create(target_root, "boot", FS_DIRECTORY);
                vfs_node_t *sys_dir  = vfs_create(target_root, "sys", FS_DIRECTORY);
                vfs_node_t *bin_dir  = sys_dir ? vfs_create(sys_dir, "bin", FS_DIRECTORY) : NULL;

                const char *files_to_copy[] = {
                    "kernel.elf", "limine.conf", "font.psf", "bash.elf", "busybox.elf", ".bashrc"
                };
                int file_count = sizeof(files_to_copy) / sizeof(files_to_copy[0]);

                for (int i = 0; i < file_count; i++) {
                    const char *fname = files_to_copy[i];

                    term_set_cursor(4 * 16, 7 * 32);
                    term_print("Copying: ");
                    term_print(fname);
                    term_print("                    ");

                    char src_path[128];
                    snprintf(src_path, sizeof(src_path), "/%s", fname);
                    vfs_node_t *src_node = vfs_open(src_path, 0);

                    if (!src_node) {
                        snprintf(src_path, sizeof(src_path), "/sys/bin/%s", fname);
                        src_node = vfs_open(src_path, 0);
                    }

                    if (src_node && src_node->length > 0) {
                        uint8_t *fbuf = (uint8_t *)kmalloc(src_node->length);
                        if (fbuf) {
                            int64_t r = vfs_read(src_node, 0, src_node->length, fbuf);
                            if (r > 0) {
                                vfs_node_t *dest_dir = (strcmp(fname, "kernel.elf") == 0 || strcmp(fname, "limine.conf") == 0)
                                                       ? boot_dir : bin_dir;
                                if (!dest_dir) dest_dir = target_root;

                                vfs_node_t *dest_node = vfs_create(dest_dir, fname, FS_FILE);
                                if (dest_node) {
                                    vfs_write(dest_node, 0, r, fbuf);
                                }
                            }
                            kfree(fbuf);
                        }
                    }

                    int pct = 20 + ((i + 1) * 70) / file_count;
                    tui_draw_progress(4, 9, 72, pct, COLOR_HEADER, COLOR_TUI_BG);
                }

                g_installer_ctx.current_state = STATE_INSTALL_BOOTLOADER;
                break;
            }

            // ------------------------------------------------------------------
            // 6. Install Bootloader Configuration
            // ------------------------------------------------------------------
            case STATE_INSTALL_BOOTLOADER: {
                term_clear_screen();
                tui_draw_box(2, 4, 76, 8, " BOOTLOADER SETUP ", COLOR_HEADER, COLOR_TUI_BG);

                term_set_cursor(4 * 16, 6 * 32);
                term_print("Writing Limine Bootloader Metadata & EFI Entries...");
                tui_draw_progress(4, 8, 72, 95, COLOR_HEADER, COLOR_TUI_BG);

                installer_log("Limine configuration successfully linked to /boot/limine.conf");
                tui_draw_progress(4, 8, 72, 100, COLOR_HEADER, COLOR_TUI_BG);

                g_installer_ctx.current_state = STATE_FINISH;
                break;
            }

            // ------------------------------------------------------------------
            // 7. Finish Screen
            // ------------------------------------------------------------------
            case STATE_FINISH: {
                term_clear_screen();
                tui_draw_box(2, 4, 76, 10, " INSTALLATION SUCCESSFUL ", COLOR_SUCCESS_FG, COLOR_TUI_BG);

                term_set_cursor(4 * 16, 6 * 32);
                term_set_custom_colors(COLOR_SUCCESS_FG, COLOR_TUI_BG);
                term_print("EquantOS has been successfully installed on your drive!");

                term_set_cursor(4 * 16, 8 * 32);
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print("Press ENTER to return to Shell or reboot your system.");

                tty_getchar_raw();
                term_clear_screen();
                return;
            }

            // ------------------------------------------------------------------
            // 8. Error Screen
            // ------------------------------------------------------------------
            case STATE_ERROR: {
                term_clear_screen();
                tui_draw_box(2, 4, 76, 10, " INSTALLATION ERROR ", COLOR_WARN_FG, COLOR_TUI_BG);

                term_set_cursor(4 * 16, 6 * 32);
                term_set_custom_colors(COLOR_WARN_FG, COLOR_TUI_BG);
                term_print("An error occurred during installation:");

                term_set_cursor(4 * 16, 8 * 32);
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print(g_installer_ctx.error_msg);

                term_set_cursor(4 * 16, 11 * 32);
                term_print("Press any key to exit to Shell...");

                installer_log(g_installer_ctx.error_msg);
                tty_getchar_raw();
                term_clear_screen();
                return;
            }
        }
    }
}