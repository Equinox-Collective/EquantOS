// src/kernel/misc/installer.c - Highly Verbose & Reliable OS Installer
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

#define COLOR_TUI_BG        0x000F0F1A
#define COLOR_TUI_FG        0x00CCCCCC
#define COLOR_HEADER        0x0000FF88
#define COLOR_SEL_BG        0x00005522
#define COLOR_SEL_FG        0x00FFFFFF
#define COLOR_BORDER        0x0000AACC
#define COLOR_WARN_FG       0x00FF3333
#define COLOR_SUCCESS_FG    0x0033FF33
#define COLOR_LOG_FG        0x00FFFF55

static installer_ctx_t g_installer_ctx;
static int g_log_row = 12;

// Detailed Verbose Logging Function
void installer_log_verbose(const char *fmt, ...) {
    char log_buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);

    // 1. Output to Serial COM1
    serial_puts(COM1, "[INSTALLER] ");
    serial_puts(COM1, log_buf);
    serial_puts(COM1, "\n");

    // 2. Output on TUI Log Console
    term_set_cursor(4 * 16, g_log_row * 32);
    term_set_custom_colors(COLOR_LOG_FG, COLOR_TUI_BG);
    
    // Clear line padding
    char line_pad[72];
    memset(line_pad, ' ', sizeof(line_pad) - 1);
    line_pad[sizeof(line_pad) - 1] = '\0';
    term_print(line_pad);

    term_set_cursor(4 * 16, g_log_row * 32);
    term_print(log_buf);

    g_log_row++;
    if (g_log_row > 18) {
        g_log_row = 12; // Scroll log window
    }

    // 3. Write to /var/log/installer.log
    vfs_node_t *var_dir = vfs_open("/var", 0);
    if (!var_dir) {
        vfs_node_t *root = vfs_open("/", 0);
        var_dir = vfs_create(root, "var", FS_DIRECTORY);
    }
    if (var_dir) {
        vfs_node_t *log_dir = vfs_open("/var/log", 0);
        if (!log_dir) {
            log_dir = vfs_create(var_dir, "log", FS_DIRECTORY);
        }
        if (log_dir) {
            vfs_node_t *logfile = vfs_open("/var/log/installer.log", 0);
            if (!logfile) {
                logfile = vfs_create(log_dir, "installer.log", FS_FILE);
            }
            if (logfile) {
                vfs_write(logfile, logfile->length, strlen(log_buf), (uint8_t *)log_buf);
                vfs_write(logfile, logfile->length, 1, (uint8_t *)"\n");
            }
        }
    }
}

void tui_draw_box(int col, int row, int width, int height, const char *title, uint32_t fg, uint32_t bg) {
    (void)bg;
    uint32_t old_color = term_get_color();
    term_set_custom_colors(fg, COLOR_TUI_BG);

    term_set_cursor(col * 16, row * 32);
    term_putchar_raw('+');
    for (int i = 0; i < width - 2; i++) term_putchar_raw('-');
    term_putchar_raw('+');

    if (title && strlen(title) > 0) {
        int title_len = strlen(title);
        int start_x = col + (width - title_len) / 2;
        term_set_cursor(start_x * 16, row * 32);
        term_print(title);
    }

    for (int r = 1; r < height - 1; r++) {
        term_set_cursor(col * 16, (row + r) * 32);
        term_putchar_raw('|');
        for (int c = 0; c < width - 2; c++) term_putchar_raw(' ');
        term_putchar_raw('|');
    }

    term_set_cursor(col * 16, (row + height - 1) * 32);
    term_putchar_raw('+');
    for (int i = 0; i < width - 2; i++) term_putchar_raw('-');
    term_putchar_raw('+');

    term_set_custom_colors(old_color, COLOR_TUI_BG);
}

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

void installer_run(void) {
    memset(&g_installer_ctx, 0, sizeof(installer_ctx_t));
    g_installer_ctx.current_state = STATE_WELCOME;
    g_log_row = 12;

    installer_log_verbose("Session Started. Initializing TUI Subsystem...");

    while (1) {
        switch (g_installer_ctx.current_state) {

            case STATE_WELCOME: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 14, " EquantOS Automated Installer ", COLOR_HEADER, COLOR_TUI_BG);

                term_set_cursor(5 * 16, 5 * 32);
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print("Welcome to EquantOS Automated Installation Wizard!");

                term_set_cursor(5 * 16, 7 * 32);
                term_print("This wizard will install EquantOS to your local drive.");

                const char *opts[] = {
                    "Start Installation Wizard",
                    "Exit to EquantOS Shell"
                };

                int res = tui_select_menu(5, 9, 68, " ACTION ", opts, 2);
                if (res == 0) {
                    g_installer_ctx.current_state = STATE_SELECT_DISK;
                } else {
                    term_clear_screen();
                    return;
                }
                break;
            }

            case STATE_SELECT_DISK: {
                term_clear_screen();
                installer_log_verbose("Scanning physical storage devices...");

                if (nvme_init() == NVME_SUCCESS) {
                    g_installer_ctx.target_dev = nvme_get_block_device();
                    g_installer_ctx.is_nvme = true;
                    installer_log_verbose("Storage Controller: Active NVMe PCIe SSD");
                } else {
                    block_device_t ata_dev = {
                        .read = (block_read_fn)read_sectors_ata_pio,
                        .write = (block_write_fn)write_sectors_ata_pio,
                        .sector_size = 512
                    };
                    g_installer_ctx.target_dev = ata_dev;
                    g_installer_ctx.is_nvme = false;
                    installer_log_verbose("Storage Controller: Primary ATA PIO Channel");
                }

                disk_partition_scan_device(g_installer_ctx.target_dev);
                int p_count = disk_get_partition_count();

                if (p_count == 0) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "No valid partitions detected on drive!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                const char *part_opts[16];
                char part_bufs[16][128];

                for (int i = 0; i < p_count && i < 16; i++) {
                    partition_info_t *p = disk_get_partition(i);
                    uint64_t size_mb = (uint64_t)(p->sector_count * 512) / (1024 * 1024);

                    snprintf(part_bufs[i], sizeof(part_bufs[i]),
                             "Part #%d | Type: 0x%02X | Start LBA: %u | Size: %llu MB",
                             p->index, p->type, p->start_lba, size_mb);
                    part_opts[i] = part_bufs[i];
                }

                int chosen = tui_select_menu(2, 2, 76, " SELECT TARGET PARTITION ", part_opts, p_count);
                if (chosen < 0) {
                    g_installer_ctx.current_state = STATE_WELCOME;
                    break;
                }

                partition_info_t *selected = disk_get_partition(chosen);
                if (!selected) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "Selection Error!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                if (selected->type == 0xEF || selected->is_esp) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "PROTECTION: EFI System Partition (0xEF) cannot be formatted!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                g_installer_ctx.target_partition = *selected;
                g_installer_ctx.selected_partition_idx = chosen;
                g_installer_ctx.current_state = STATE_CONFIRM_FORMAT;
                break;
            }

            case STATE_CONFIRM_FORMAT: {
                term_clear_screen();
                char warn[256];
                snprintf(warn, sizeof(warn),
                         "CRITICAL: Partition #%d (LBA %u, Size %u MB) WILL BE FORMATTED!",
                         g_installer_ctx.target_partition.index,
                         g_installer_ctx.target_partition.start_lba,
                         (g_installer_ctx.target_partition.sector_count * 512) / (1024 * 1024));

                if (tui_dialog_confirm("!!! FORMAT CONFIRMATION !!!", warn)) {
                    g_installer_ctx.current_state = STATE_PARTITION_FORMAT;
                } else {
                    installer_log_verbose("User cancelled format operation.");
                    g_installer_ctx.current_state = STATE_WELCOME;
                }
                break;
            }

            case STATE_PARTITION_FORMAT: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 18, " FORMATTING TARGET PARTITION ", COLOR_HEADER, COLOR_TUI_BG);

                installer_log_verbose("Formatting Partition #%d (LBA %u)...",
                                     g_installer_ctx.target_partition.index,
                                     g_installer_ctx.target_partition.start_lba);

                tui_draw_progress(4, 5, 72, 10, COLOR_HEADER, COLOR_TUI_BG);

                int res = mkfs_ext2(g_installer_ctx.target_dev,
                                    g_installer_ctx.target_partition.start_lba,
                                    g_installer_ctx.target_partition.sector_count,
                                    "EQUANT_SYS");

                if (res != 0) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "mkfs_ext2 failed at LBA %u!", g_installer_ctx.target_partition.start_lba);
                    g_installer_ctx.current_state = STATE_ERROR;
                } else {
                    tui_draw_progress(4, 5, 72, 100, COLOR_HEADER, COLOR_TUI_BG);
                    installer_log_verbose("Superblock & Inode Table written successfully.");
                    g_installer_ctx.current_state = STATE_COPY_FILES;
                }
                break;
            }

            case STATE_COPY_FILES: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 20, " COPYING OS PAYLOAD FILES ", COLOR_HEADER, COLOR_TUI_BG);
                g_log_row = 11;

                installer_log_verbose("Mounting target Ext2 volume...");

                vfs_node_t *target_root = ext2_mount_partition(g_installer_ctx.target_dev,
                                                                g_installer_ctx.target_partition.start_lba);

                if (!target_root) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "Failed to mount Ext2 partition at LBA %u!",
                             g_installer_ctx.target_partition.start_lba);
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                installer_log_verbose("Creating Root Directory Tree (/boot, /sys, /sys/bin)...");
                vfs_node_t *boot_dir = vfs_create(target_root, "boot", FS_DIRECTORY);
                vfs_node_t *sys_dir  = vfs_create(target_root, "sys", FS_DIRECTORY);
                vfs_node_t *bin_dir  = sys_dir ? vfs_create(sys_dir, "bin", FS_DIRECTORY) : NULL;

                if (boot_dir) installer_log_verbose("Created DIR '/boot'");
                if (sys_dir)  installer_log_verbose("Created DIR '/sys'");
                if (bin_dir)  installer_log_verbose("Created DIR '/sys/bin'");

                const char *files_to_copy[] = {
                    "kernel.elf", "limine.conf", "font.psf", "bash.elf", "busybox.elf", ".bashrc", "hello.elf"
                };
                int file_count = sizeof(files_to_copy) / sizeof(files_to_copy[0]);

                for (int i = 0; i < file_count; i++) {
                    const char *fname = files_to_copy[i];

                    char search_paths[5][128];
                    snprintf(search_paths[0], 128, "/%s", fname);
                    snprintf(search_paths[1], 128, "/sys/bin/%s", fname);
                    snprintf(search_paths[2], 128, "/boot/%s", fname);
                    snprintf(search_paths[3], 128, "/cdrom/%s", fname);
                    snprintf(search_paths[4], 128, "/mnt/iso/%s", fname);

                    vfs_node_t *src_node = NULL;
                    for (int p = 0; p < 5; p++) {
                        src_node = vfs_open(search_paths[p], 0);
                        if (src_node) {
                            installer_log_verbose("Found '%s' at '%s' (%llu bytes)", fname, search_paths[p], src_node->length);
                            break;
                        }
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
                                    int64_t w = vfs_write(dest_node, 0, r, fbuf);
                                    installer_log_verbose("SUCCESS: Written %lld bytes to '%s/%s'", w, dest_dir->name, fname);
                                } else {
                                    installer_log_verbose("ERROR: Failed vfs_create for '%s'", fname);
                                }
                            }
                            kfree(fbuf);
                        } else {
                            installer_log_verbose("ERROR: Out of memory allocating %llu bytes", src_node->length);
                        }
                    } else {
                        installer_log_verbose("WARNING: Source file '%s' NOT FOUND in VFS!", fname);
                    }

                    int pct = 10 + ((i + 1) * 80) / file_count;
                    tui_draw_progress(4, 5, 72, pct, COLOR_HEADER, COLOR_TUI_BG);
                }

                g_installer_ctx.current_state = STATE_INSTALL_BOOTLOADER;
                break;
            }

            case STATE_INSTALL_BOOTLOADER: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 18, " LIMINE BOOTLOADER DEPLOYMENT ", COLOR_HEADER, COLOR_TUI_BG);
                g_log_row = 10;

                installer_log_verbose("Deploying Limine BIOS Boot Code to LBA 0 (Protective MBR)...");

                // Write Limine Stage 1 MBR Boot Code to LBA 0 while preserving Partition Table
                uint8_t sector0[512];
                if (g_installer_ctx.target_dev.read(0, 1, sector0) == 0) {
                    vfs_node_t *boot_bin = vfs_open("/boot/limine-bios-cd.bin", 0);
                    if (!boot_bin) boot_bin = vfs_open("/limine-bios-cd.bin", 0);

                    if (boot_bin && boot_bin->length >= 446) {
                        uint8_t boot_code[512];
                        vfs_read(boot_bin, 0, 446, boot_code);
                        // Preserve existing Partition Table (offsets 446..509)
                        memcpy(sector0, boot_code, 446);
                        sector0[510] = 0x55;
                        sector0[511] = 0xAA;
                        g_installer_ctx.target_dev.write(0, 1, sector0);
                        installer_log_verbose("Limine MBR Stage 1 written to LBA 0 successfully.");
                    } else {
                        installer_log_verbose("Skipping MBR code write (limine-bios-cd.bin not found).");
                    }
                }

                // Generate /boot/limine.conf on Target Volume
                vfs_node_t *target_root = ext2_mount_partition(g_installer_ctx.target_dev,
                                                                g_installer_ctx.target_partition.start_lba);
                if (target_root) {
                    vfs_node_t *boot_dir = vfs_finddir(target_root, "boot");
                    if (!boot_dir) boot_dir = vfs_create(target_root, "boot", FS_DIRECTORY);

                    if (boot_dir) {
                        const char *limine_cfg = 
                            "TIMEOUT=3\n"
                            "VERBOSE=yes\n\n"
                            ":EquantOS\n"
                            "    PROTOCOL=limine\n"
                            "    KERNEL_PATH=boot:///boot/kernel.elf\n"
                            "    MODULE_PATH=boot:///sys/bin/font.psf\n";

                        vfs_node_t *cfg_file = vfs_create(boot_dir, "limine.conf", FS_FILE);
                        if (cfg_file) {
                            vfs_write(cfg_file, 0, strlen(limine_cfg), (uint8_t *)limine_cfg);
                            installer_log_verbose("Created '/boot/limine.conf' configuration.");
                        }
                    }
                }

                tui_draw_progress(4, 5, 72, 100, COLOR_HEADER, COLOR_TUI_BG);
                g_installer_ctx.current_state = STATE_FINISH;
                break;
            }

            case STATE_FINISH: {
                term_clear_screen();
                tui_draw_box(2, 4, 76, 12, " INSTALLATION COMPLETE ", COLOR_SUCCESS_FG, COLOR_TUI_BG);

                term_set_cursor(4 * 16, 6 * 32);
                term_set_custom_colors(COLOR_SUCCESS_FG, COLOR_TUI_BG);
                term_print("EquantOS has been successfully installed & configured!");

                term_set_cursor(4 * 16, 8 * 32);
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print("Check /var/log/installer.log or Serial COM1 for full logs.");

                term_set_cursor(4 * 16, 10 * 32);
                term_print("Press ENTER to exit to Shell...");

                tty_getchar_raw();
                term_clear_screen();
                return;
            }

            case STATE_ERROR: {
                term_clear_screen();
                tui_draw_box(2, 4, 76, 12, " INSTALLATION FAILURE ", COLOR_WARN_FG, COLOR_TUI_BG);

                term_set_cursor(4 * 16, 6 * 32);
                term_set_custom_colors(COLOR_WARN_FG, COLOR_TUI_BG);
                term_print("Fatal Error:");

                term_set_cursor(4 * 16, 8 * 32);
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print(g_installer_ctx.error_msg);

                term_set_cursor(4 * 16, 11 * 32);
                term_print("Press any key to return to Shell...");

                tty_getchar_raw();
                term_clear_screen();
                return;
            }
        }
    }
}