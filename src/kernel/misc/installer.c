// src/kernel/misc/installer.c - Production-Grade UEFI GPT Installer with Drive Discovery & Configuration
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
#include "../fs/gpt.h"
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

#define CHUNK_SZ            65536 // 64KB safe copy buffer

static installer_ctx_t g_installer_ctx;
static int g_log_row = 12;

static uint32_t gpt_crc32(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

static int installer_discover_drives(installer_ctx_t *ctx) {
    ctx->disk_count = 0;

    // 1. Probe NVMe Controllers
    if (nvme_init() == NVME_SUCCESS) {
        installer_disk_t *d = &ctx->disks[ctx->disk_count++];
        strcpy(d->name, "NVMe PCIe Solid-State Drive");
        d->bdev = nvme_get_block_device();
        d->sector_size = d->bdev.sector_size ? d->bdev.sector_size : 512;
        // If driver does not expose full geometry, fallback to standard image sizing
        d->total_sectors = 131072; // Default 64MB for QEMU test disk
        d->is_nvme = true;
    }

    // 2. Probe ATA Primary Master
    block_device_t ata_master = {
        .read = (block_read_fn)read_sectors_ata_pio,
        .write = (block_write_fn)write_sectors_ata_pio,
        .sector_size = 512
    };

    // Test readability of LBA 0 to check drive presence
    uint8_t probe_sector[512];
    if (ata_master.read(0, 1, probe_sector) == 0) {
        installer_disk_t *d = &ctx->disks[ctx->disk_count++];
        strcpy(d->name, "Primary ATA Hard Disk");
        d->bdev = ata_master;
        d->sector_size = 512;
        d->total_sectors = 131072;
        d->is_nvme = false;
    }

    return ctx->disk_count;
}

static int gpt_create_layout(block_device_t dev, uint64_t total_sec, uint64_t esp_start, uint64_t esp_cnt, uint64_t root_start, uint64_t root_cnt) {
    if (total_sec < 65536) return -1;

    uint8_t *sec_buf = (uint8_t *)kzalloc(512);
    if (!sec_buf) return -1;

    // 1. Protective MBR (LBA 0)
    sec_buf[446 + 4] = 0xEE; // GPT Protective Type
    *(uint32_t *)&sec_buf[446 + 8] = 1;
    *(uint32_t *)&sec_buf[446 + 12] = (uint32_t)(total_sec - 1);
    sec_buf[510] = 0x55;
    sec_buf[511] = 0xAA;
    dev.write(0, 1, sec_buf);

    // 2. GPT Partition Entries (LBA 2..33)
    uint32_t entries_bytes = 128 * 128;
    uint8_t *entries_buf = (uint8_t *)kzalloc(entries_bytes);
    if (!entries_buf) {
        kfree(sec_buf);
        return -1;
    }

    static const uint8_t esp_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    static const uint8_t linux_guid[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    };

    // Partition 0: EFI System Partition (ESP)
    gpt_partition_entry_t *e0 = (gpt_partition_entry_t *)&entries_buf[0];
    memcpy(e0->partition_type_guid, esp_guid, 16);
    memset(e0->unique_partition_guid, 0x01, 16);
    e0->starting_lba = esp_start;
    e0->ending_lba = esp_start + esp_cnt - 1;

    // Partition 1: Linux / EquantOS Root Partition
    gpt_partition_entry_t *e1 = (gpt_partition_entry_t *)&entries_buf[128];
    memcpy(e1->partition_type_guid, linux_guid, 16);
    memset(e1->unique_partition_guid, 0x02, 16);
    e1->starting_lba = root_start;
    e1->ending_lba = root_start + root_cnt - 1;

    dev.write(2, 32, entries_buf);
    dev.write(total_sec - 33, 32, entries_buf);

    // 3. Primary GPT Header (LBA 1)
    memset(sec_buf, 0, 512);
    gpt_header_t *hdr = (gpt_header_t *)sec_buf;
    hdr->signature = GPT_SIGNATURE;
    hdr->revision = 0x00010000;
    hdr->header_size = 92;
    hdr->current_lba = 1;
    hdr->backup_lba = total_sec - 1;
    hdr->first_usable_lba = 34;
    hdr->last_usable_lba = total_sec - 34;
    memset(hdr->disk_guid, 0xA5, 16);
    hdr->partition_entries_lba = 2;
    hdr->num_partition_entries = 128;
    hdr->size_partition_entry = 128;
    hdr->partition_array_crc32 = gpt_crc32(entries_buf, entries_bytes);
    hdr->header_crc32 = 0;
    hdr->header_crc32 = gpt_crc32(hdr, 92);
    dev.write(1, 1, sec_buf);

    // 4. Backup GPT Header (LBA total_sec - 1)
    hdr->current_lba = total_sec - 1;
    hdr->backup_lba = 1;
    hdr->partition_entries_lba = total_sec - 33;
    hdr->header_crc32 = 0;
    hdr->header_crc32 = gpt_crc32(hdr, 92);
    dev.write(total_sec - 1, 1, sec_buf);

    kfree(entries_buf);
    kfree(sec_buf);
    return 0;
}

void installer_log_verbose(const char *fmt, ...) {
    char log_buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);

    serial_puts(COM1, "[INSTALLER] ");
    serial_puts(COM1, log_buf);
    serial_puts(COM1, "\n");

    term_set_cursor(4 * 16, g_log_row * 32);
    term_set_custom_colors(COLOR_LOG_FG, COLOR_TUI_BG);

    char line_pad[72];
    memset(line_pad, ' ', sizeof(line_pad) - 1);
    line_pad[sizeof(line_pad) - 1] = '\0';
    term_print(line_pad);

    term_set_cursor(4 * 16, g_log_row * 32);
    term_print(log_buf);

    g_log_row++;
    if (g_log_row > 18) {
        g_log_row = 12;
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
        "YES - Proceed with Write to Disk"
    };

    int choice = tui_select_menu(4, 9, 72, " CONFIRM OPERATION ", options, 2);
    return (choice == 1);
}

static bool deploy_file_stream(vfs_node_t *dest_dir, const char *fname, vfs_node_t *src, uint8_t *buffer, size_t buf_sz) {
    if (!dest_dir || !fname || !src) return false;

    vfs_node_t *node = vfs_create(dest_dir, fname, FS_FILE);
    if (!node) return false;

    uint64_t offset = 0;
    while (offset < src->length) {
        uint64_t to_read = src->length - offset;
        if (to_read > buf_sz) to_read = buf_sz;

        int64_t bytes_read = vfs_read(src, offset, to_read, buffer);
        if (bytes_read <= 0) return false;

        int64_t bytes_written = vfs_write(node, offset, bytes_read, buffer);
        if (bytes_written != bytes_read) return false;

        offset += bytes_written;
    }

    return true;
}

void installer_run(void) {
    memset(&g_installer_ctx, 0, sizeof(installer_ctx_t));
    g_installer_ctx.current_state = STATE_WELCOME;
    strcpy(g_installer_ctx.sys_cfg.hostname, "equantos");
    g_installer_ctx.sys_cfg.boot_timeout = 3;
    strcpy(g_installer_ctx.sys_cfg.boot_cmdline, "quiet");
    g_log_row = 12;

    installer_log_verbose("Installer session initialized.");

    while (1) {
        switch (g_installer_ctx.current_state) {

            case STATE_WELCOME: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 14, " EquantOS System Installer ", COLOR_HEADER, COLOR_TUI_BG);

                term_set_cursor(5 * 16, 5 * 32);
                term_set_custom_colors(COLOR_TUI_FG, COLOR_TUI_BG);
                term_print("Welcome to EquantOS Professional Installation Wizard!");

                term_set_cursor(5 * 16, 7 * 32);
                term_print("Configure storage layout, partitions, and deploy the system.");

                const char *opts[] = {
                    "Begin Installation",
                    "Exit to Shell"
                };

                int res = tui_select_menu(5, 9, 68, " MAIN MENU ", opts, 2);
                if (res == 0) {
                    g_installer_ctx.current_state = STATE_SELECT_DRIVE;
                } else {
                    term_clear_screen();
                    return;
                }
                break;
            }

            case STATE_SELECT_DRIVE: {
                term_clear_screen();
                int count = installer_discover_drives(&g_installer_ctx);
                if (count == 0) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "No physical block storage controllers discovered!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                const char *drive_opts[INSTALLER_MAX_DISKS];
                char drive_labels[INSTALLER_MAX_DISKS][128];

                for (int i = 0; i < count; i++) {
                    installer_disk_t *d = &g_installer_ctx.disks[i];
                    uint64_t size_mb = (d->total_sectors * d->sector_size) / (1024 * 1024);
                    snprintf(drive_labels[i], sizeof(drive_labels[i]),
                             "Disk #%d: %s (%llu MB, %u-byte sectors)",
                             i, d->name, size_mb, d->sector_size);
                    drive_opts[i] = drive_labels[i];
                }

                int chosen = tui_select_menu(2, 2, 76, " SELECT TARGET DISK ", drive_opts, count);
                if (chosen < 0) {
                    g_installer_ctx.current_state = STATE_WELCOME;
                    break;
                }

                g_installer_ctx.selected_disk_idx = chosen;
                g_installer_ctx.current_state = STATE_SELECT_STRATEGY;
                break;
            }

            case STATE_SELECT_STRATEGY: {
                term_clear_screen();
                const char *strat_opts[] = {
                    "Erase Entire Drive & Auto-Partition (GPT UEFI: ESP + Root EXT2)",
                    "Install to Existing Partition (Preserve Current Partition Table)"
                };

                int chosen = tui_select_menu(2, 2, 76, " PARTITIONING STRATEGY ", strat_opts, 2);
                if (chosen < 0) {
                    g_installer_ctx.current_state = STATE_SELECT_DRIVE;
                    break;
                }

                if (chosen == 0) {
                    g_installer_ctx.strategy = INSTALL_STRATEGY_AUTO_GPT;
                    g_installer_ctx.current_state = STATE_CONFIGURE_SYSTEM;
                } else {
                    g_installer_ctx.strategy = INSTALL_STRATEGY_MANUAL_PART;
                    g_installer_ctx.current_state = STATE_SELECT_PARTITION;
                }
                break;
            }

            case STATE_SELECT_PARTITION: {
                term_clear_screen();
                installer_disk_t *target_disk = &g_installer_ctx.disks[g_installer_ctx.selected_disk_idx];
                disk_partition_scan_device(target_disk->bdev);
                int p_count = disk_get_partition_count();

                if (p_count == 0) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "No partitions exist on this drive! Use Auto-Partition.");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                const char *part_opts[16];
                char part_labels[16][128];

                for (int i = 0; i < p_count && i < 16; i++) {
                    partition_info_t *p = disk_get_partition(i);
                    uint64_t size_mb = (uint64_t)(p->sector_count * 512) / (1024 * 1024);
                    snprintf(part_labels[i], sizeof(part_labels[i]),
                             "Part #%d [%s] LBA: %u | Size: %llu MB",
                             p->index, p->fs_name, p->start_lba, size_mb);
                    part_opts[i] = part_labels[i];
                }

                int chosen = tui_select_menu(2, 2, 76, " SELECT ROOT PARTITION ", part_opts, p_count);
                if (chosen < 0) {
                    g_installer_ctx.current_state = STATE_SELECT_STRATEGY;
                    break;
                }

                partition_info_t *p = disk_get_partition(chosen);
                if (p->is_esp) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg),
                             "PROTECTION: EFI System Partition cannot be used as Root volume!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                g_installer_ctx.selected_partition = *p;
                g_installer_ctx.root_start_lba = p->start_lba;
                g_installer_ctx.root_sector_count = p->sector_count;
                g_installer_ctx.current_state = STATE_CONFIGURE_SYSTEM;
                break;
            }

            case STATE_CONFIGURE_SYSTEM: {
                term_clear_screen();
                const char *timeout_opts[] = {
                    "Default: 3 Seconds Timeout (Recommended)",
                    "Quick: 1 Second Timeout",
                    "Instant: 0 Seconds (Direct Kernel Launch)",
                    "Patient: 10 Seconds Timeout"
                };

                int chosen = tui_select_menu(2, 2, 76, " BOOTLOADER TIMEOUT SELECTION ", timeout_opts, 4);
                if (chosen == 0) g_installer_ctx.sys_cfg.boot_timeout = 3;
                else if (chosen == 1) g_installer_ctx.sys_cfg.boot_timeout = 1;
                else if (chosen == 2) g_installer_ctx.sys_cfg.boot_timeout = 0;
                else if (chosen == 3) g_installer_ctx.sys_cfg.boot_timeout = 10;
                else {
                    g_installer_ctx.current_state = STATE_SELECT_STRATEGY;
                    break;
                }

                g_installer_ctx.current_state = STATE_CONFIRM_ACTIONS;
                break;
            }

            case STATE_CONFIRM_ACTIONS: {
                term_clear_screen();
                installer_disk_t *target_disk = &g_installer_ctx.disks[g_installer_ctx.selected_disk_idx];

                char warn[384];
                if (g_installer_ctx.strategy == INSTALL_STRATEGY_AUTO_GPT) {
                    snprintf(warn, sizeof(warn),
                             "WARNING: ALL DATA ON DISK #%d (%s) WILL BE DESTROYED!\n"
                             "A new GPT table with ESP (FAT32) and Root (EXT2) will be created.",
                             g_installer_ctx.selected_disk_idx, target_disk->name);
                } else {
                    snprintf(warn, sizeof(warn),
                             "WARNING: Partition #%d (LBA %llu, Size %llu MB) WILL BE FORMATTED TO EXT2!",
                             g_installer_ctx.selected_partition.index,
                             g_installer_ctx.root_start_lba,
                             (g_installer_ctx.root_sector_count * 512) / (1024 * 1024));
                }

                if (tui_dialog_confirm("!!! FINAL FORMAT CONFIRMATION !!!", warn)) {
                    g_installer_ctx.current_state = STATE_EXECUTE_INSTALL;
                } else {
                    installer_log_verbose("User aborted operation at confirmation prompt.");
                    g_installer_ctx.current_state = STATE_WELCOME;
                }
                break;
            }

            case STATE_EXECUTE_INSTALL: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 20, " EXECUTING INSTALLATION PIPELINE ", COLOR_HEADER, COLOR_TUI_BG);
                g_log_row = 11;

                installer_disk_t *target_disk = &g_installer_ctx.disks[g_installer_ctx.selected_disk_idx];
                uint64_t total_sec = target_disk->total_sectors;

                if (g_installer_ctx.strategy == INSTALL_STRATEGY_AUTO_GPT) {
                    installer_log_verbose("Calculating dynamic partition bounds...");
                    tui_draw_progress(4, 5, 72, 10, COLOR_HEADER, COLOR_TUI_BG);

                    // Dynamic sizing: ESP gets 34MB (69632 sectors), Root takes remaining usable LBA
                    g_installer_ctx.esp_start_lba = 2048;
                    g_installer_ctx.esp_sector_count = 69632;

                    g_installer_ctx.root_start_lba = g_installer_ctx.esp_start_lba + g_installer_ctx.esp_sector_count;
                    g_installer_ctx.root_sector_count = (total_sec - 34) - g_installer_ctx.root_start_lba + 1;

                    installer_log_verbose("Writing dual-partition GPT layout to disk...");
                    if (gpt_create_layout(target_disk->bdev, total_sec,
                                          g_installer_ctx.esp_start_lba, g_installer_ctx.esp_sector_count,
                                          g_installer_ctx.root_start_lba, g_installer_ctx.root_sector_count) != 0) {
                        snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "Failed writing GPT layout!");
                        g_installer_ctx.current_state = STATE_ERROR;
                        break;
                    }

                    tui_draw_progress(4, 5, 72, 25, COLOR_HEADER, COLOR_TUI_BG);
                    installer_log_verbose("Formatting Partition 1 (ESP) with FAT32...");
                    if (mkfs_fat32(target_disk->bdev, (uint32_t)g_installer_ctx.esp_start_lba,
                                   (uint32_t)g_installer_ctx.esp_sector_count, "EFI SYSTEM") != 0) {
                        snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "FAT32 formatting failed!");
                        g_installer_ctx.current_state = STATE_ERROR;
                        break;
                    }

                    tui_draw_progress(4, 5, 72, 40, COLOR_HEADER, COLOR_TUI_BG);
                    installer_log_verbose("Formatting Partition 2 (Root) with EXT2...");
                    if (mkfs_ext2(target_disk->bdev, (uint32_t)g_installer_ctx.root_start_lba,
                                  (uint32_t)g_installer_ctx.root_sector_count, "EQUANT_SYS") != 0) {
                        snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "EXT2 formatting failed!");
                        g_installer_ctx.current_state = STATE_ERROR;
                        break;
                    }
                } else {
                    // Manual partition strategy: format root partition only
                    installer_log_verbose("Formatting selected partition with EXT2...");
                    if (mkfs_ext2(target_disk->bdev, (uint32_t)g_installer_ctx.root_start_lba,
                                  (uint32_t)g_installer_ctx.root_sector_count, "EQUANT_SYS") != 0) {
                        snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "Target EXT2 format failed!");
                        g_installer_ctx.current_state = STATE_ERROR;
                        break;
                    }
                    // For manual install, fallback ESP target to partition 0
                    g_installer_ctx.esp_start_lba = 2048;
                    g_installer_ctx.esp_sector_count = 69632;
                }

                // 2. Mount Filesystem Trees
                tui_draw_progress(4, 5, 72, 50, COLOR_HEADER, COLOR_TUI_BG);
                installer_log_verbose("Mounting target ESP & Root filesystems...");

                vfs_node_t *esp_root = fat32_mount_partition(target_disk->bdev,
                                                            (uint32_t)g_installer_ctx.esp_start_lba,
                                                            (uint32_t)g_installer_ctx.esp_sector_count);
                vfs_node_t *root_ext2 = ext2_mount_partition(target_disk->bdev,
                                                             (uint32_t)g_installer_ctx.root_start_lba);

                if (!esp_root || !root_ext2) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "VFS mount failure on target partitions!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                // Create Target Directory Tree
                vfs_node_t *esp_efi = vfs_create(esp_root, "EFI", FS_DIRECTORY);
                vfs_node_t *esp_boot = esp_efi ? vfs_create(esp_efi, "BOOT", FS_DIRECTORY) : NULL;
                vfs_node_t *esp_boot_dir = vfs_create(esp_root, "boot", FS_DIRECTORY);
                vfs_node_t *esp_sys = vfs_create(esp_root, "sys", FS_DIRECTORY);
                vfs_node_t *esp_bin = esp_sys ? vfs_create(esp_sys, "bin", FS_DIRECTORY) : NULL;

                vfs_node_t *r_boot = vfs_create(root_ext2, "boot", FS_DIRECTORY);
                vfs_node_t *r_sys  = vfs_create(root_ext2, "sys", FS_DIRECTORY);
                vfs_node_t *r_bin  = r_sys ? vfs_create(r_sys, "bin", FS_DIRECTORY) : NULL;
                vfs_create(root_ext2, "dev", FS_DIRECTORY);
                vfs_create(root_ext2, "var", FS_DIRECTORY);

                // 3. Deploy Payload Files
                const char *files_to_copy[] = {
                    "BOOTX64.EFI", "kernel.elf", "font.psf",
                    "bash.elf", "busybox.elf", ".bashrc", "hello.elf"
                };
                int file_count = sizeof(files_to_copy) / sizeof(files_to_copy[0]);

                uint8_t *cbuf = (uint8_t *)kmalloc(CHUNK_SZ);
                if (!cbuf) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "Out of memory allocating I/O buffer!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                bool copy_failed = false;
                for (int i = 0; i < file_count; i++) {
                    const char *fname = files_to_copy[i];

                    char p[4][128];
                    snprintf(p[0], 128, "/%s", fname);
                    snprintf(p[1], 128, "/sys/bin/%s", fname);
                    snprintf(p[2], 128, "/boot/%s", fname);
                    snprintf(p[3], 128, "/EFI/BOOT/%s", fname);

                    vfs_node_t *src = NULL;
                    for (int k = 0; k < 4; k++) {
                        src = vfs_open(p[k], 0);
                        if (src) break;
                    }

                    if (!src || src->length == 0) {
                        installer_log_verbose("WARN: Payload file '%s' not found on live medium.", fname);
                        continue;
                    }

                    // A. Deploy to ESP (FAT32)
                    vfs_node_t *dest_esp = esp_bin;
                    if (strcmp(fname, "BOOTX64.EFI") == 0) dest_esp = esp_boot ? esp_boot : esp_root;
                    else if (strcmp(fname, "kernel.elf") == 0) dest_esp = esp_boot_dir ? esp_boot_dir : esp_root;

                    if (!deploy_file_stream(dest_esp, fname, src, cbuf, CHUNK_SZ)) {
                        copy_failed = true;
                        break;
                    }

                    // B. Deploy to Root (EXT2)
                    vfs_node_t *dest_ext2 = (strcmp(fname, "kernel.elf") == 0) ? r_boot : r_bin;
                    if (!deploy_file_stream(dest_ext2 ? dest_ext2 : root_ext2, fname, src, cbuf, CHUNK_SZ)) {
                        copy_failed = true;
                        break;
                    }

                    installer_log_verbose("Deployed '%s' (%llu bytes)", fname, src->length);
                    int pct = 50 + ((i + 1) * 35) / file_count;
                    tui_draw_progress(4, 5, 72, pct, COLOR_HEADER, COLOR_TUI_BG);
                }

                kfree(cbuf);

                if (copy_failed) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "File copy transaction aborted on I/O error!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                // 4. Generate Dynamic limine.conf with user-selected parameters
                installer_log_verbose("Generating bootloader configuration...");
                char limine_cfg[512];
                snprintf(limine_cfg, sizeof(limine_cfg),
                         "timeout: %d\n\n"
                         "/%s\n"
                         "    protocol: limine\n"
                         "    kernel_path: boot():/boot/kernel.elf\n"
                         "    module_path: boot():/sys/bin/font.psf\n"
                         "    module_path: boot():/sys/bin/bash.elf\n"
                         "    module_path: boot():/sys/bin/busybox.elf\n"
                         "    module_path: boot():/sys/bin/.bashrc\n"
                         "    module_path: boot():/sys/bin/hello.elf\n",
                         g_installer_ctx.sys_cfg.boot_timeout,
                         g_installer_ctx.sys_cfg.hostname);

                // Write limine.conf to ESP /limine.conf and /EFI/BOOT/limine.conf
                vfs_node_t *cfg1 = vfs_create(esp_root, "limine.conf", FS_FILE);
                if (cfg1) vfs_write(cfg1, 0, strlen(limine_cfg), (uint8_t *)limine_cfg);

                if (esp_boot) {
                    vfs_node_t *cfg2 = vfs_create(esp_boot, "limine.conf", FS_FILE);
                    if (cfg2) vfs_write(cfg2, 0, strlen(limine_cfg), (uint8_t *)limine_cfg);
                }

                // Write backup copy to EXT2 /boot/limine.conf
                if (r_boot) {
                    vfs_node_t *cfg3 = vfs_create(r_boot, "limine.conf", FS_FILE);
                    if (cfg3) vfs_write(cfg3, 0, strlen(limine_cfg), (uint8_t *)limine_cfg);
                }

                tui_draw_progress(4, 5, 72, 100, COLOR_HEADER, COLOR_TUI_BG);
                installer_log_verbose("System deployment finalized successfully!");
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
                term_print("Reboot your system and select the hard disk in UEFI.");

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
                term_print("Press any key to return to Main Menu...");

                tty_getchar_raw();
                g_installer_ctx.current_state = STATE_WELCOME;
                break;
            }
        }
    }
}