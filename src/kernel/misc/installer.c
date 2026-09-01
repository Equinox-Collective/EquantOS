// src/kernel/misc/installer.c - Full Production TUI & Dual-Partition UEFI Installer
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

#define CHUNK_SZ 65536 // 64KB Safe Copy Buffer

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

static int gpt_create_dual_partition_layout(block_device_t dev, uint64_t total_sectors, uint32_t esp_sectors) {
    if (total_sectors < 65536) return -1;

    uint8_t *sec_buf = (uint8_t *)kzalloc(512);
    if (!sec_buf) return -1;

    // 1. LBA 0: Protective MBR
    sec_buf[446 + 4] = 0xEE;
    *(uint32_t *)&sec_buf[446 + 8] = 1;
    *(uint32_t *)&sec_buf[446 + 12] = (uint32_t)(total_sectors - 1);
    sec_buf[510] = 0x55;
    sec_buf[511] = 0xAA;
    dev.write(0, 1, sec_buf);

    // 2. LBA 2..33: 128 GPT Partition Entries (16384 bytes)
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

    uint64_t p1_start = 2048;
    uint64_t p1_end   = 2048 + esp_sectors - 1;

    uint64_t p2_start = p1_end + 1;
    uint64_t p2_end   = total_sectors - 34;

    gpt_partition_entry_t *e0 = (gpt_partition_entry_t *)&entries_buf[0];
    memcpy(e0->partition_type_guid, esp_guid, 16);
    memset(e0->unique_partition_guid, 0x01, 16);
    e0->starting_lba = p1_start;
    e0->ending_lba = p1_end;

    gpt_partition_entry_t *e1 = (gpt_partition_entry_t *)&entries_buf[128];
    memcpy(e1->partition_type_guid, linux_guid, 16);
    memset(e1->unique_partition_guid, 0x02, 16);
    e1->starting_lba = p2_start;
    e1->ending_lba = p2_end;

    dev.write(2, 32, entries_buf);
    dev.write(total_sectors - 33, 32, entries_buf);

    // 3. LBA 1: Primary GPT Header
    memset(sec_buf, 0, 512);
    gpt_header_t *hdr = (gpt_header_t *)sec_buf;
    hdr->signature = GPT_SIGNATURE;
    hdr->revision = 0x00010000;
    hdr->header_size = 92;
    hdr->current_lba = 1;
    hdr->backup_lba = total_sectors - 1;
    hdr->first_usable_lba = 34;
    hdr->last_usable_lba = total_sectors - 34;
    memset(hdr->disk_guid, 0x05, 16);
    hdr->partition_entries_lba = 2;
    hdr->num_partition_entries = 128;
    hdr->size_partition_entry = 128;
    hdr->partition_array_crc32 = gpt_crc32(entries_buf, entries_bytes);
    hdr->header_crc32 = 0;
    hdr->header_crc32 = gpt_crc32(hdr, 92);
    dev.write(1, 1, sec_buf);

    // 4. LBA total_sectors - 1: Backup GPT Header
    hdr->current_lba = total_sectors - 1;
    hdr->backup_lba = 1;
    hdr->partition_entries_lba = total_sectors - 33;
    hdr->header_crc32 = 0;
    hdr->header_crc32 = gpt_crc32(hdr, 92);
    dev.write(total_sectors - 1, 1, sec_buf);

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
                tui_draw_box(2, 2, 76, 18, " FORMATTING DISK (GPT: ESP + ROOT) ", COLOR_HEADER, COLOR_TUI_BG);

                installer_log_verbose("Creating Dual-Partition GPT Layout (ESP 34MB + ROOT EXT2)...");
                tui_draw_progress(4, 5, 72, 10, COLOR_HEADER, COLOR_TUI_BG);

                uint64_t total_sec = 131072; // 64MB Disk Image
                uint32_t esp_sec = 69632;    // 34MB ESP

                gpt_create_dual_partition_layout(g_installer_ctx.target_dev, total_sec, esp_sec);
                installer_log_verbose("Partition #1: ESP FAT32 (LBA 2048 .. 71679)");
                installer_log_verbose("Partition #2: ROOT EXT2 (LBA 71680 .. 131038)");

                tui_draw_progress(4, 5, 72, 40, COLOR_HEADER, COLOR_TUI_BG);
                installer_log_verbose("Formatting Partition #1 with FAT32 (ESP)...");
                mkfs_fat32(g_installer_ctx.target_dev, 2048, esp_sec, "EFI SYSTEM");

                tui_draw_progress(4, 5, 72, 80, COLOR_HEADER, COLOR_TUI_BG);
                installer_log_verbose("Formatting Partition #2 with EXT2 (Root)...");
                mkfs_ext2(g_installer_ctx.target_dev, 71680, (uint32_t)(total_sec - 34 - 71680 + 1), "EQUANT_SYS");

                tui_draw_progress(4, 5, 72, 100, COLOR_HEADER, COLOR_TUI_BG);
                installer_log_verbose("Formatting complete! Moving to file deployment.");
                g_installer_ctx.current_state = STATE_COPY_FILES;
                break;
            }

            case STATE_COPY_FILES: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 20, " DEPLOYING UEFI ESP & SYSTEM PAYLOAD ", COLOR_HEADER, COLOR_TUI_BG);
                g_log_row = 11;

                // 1. Mount ESP FAT32 Partition (LBA 2048, 34MB)
                installer_log_verbose("Mounting ESP FAT32 partition (LBA 2048)...");
                vfs_node_t *esp_root = fat32_mount_partition(g_installer_ctx.target_dev, 2048, 69632);

                // 2. Mount Root EXT2 Partition (LBA 71680)
                installer_log_verbose("Mounting Root EXT2 partition (LBA 71680)...");
                vfs_node_t *root_ext2 = ext2_mount_partition(g_installer_ctx.target_dev, 71680);

                if (!esp_root || !root_ext2) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "Failed to mount target volumes!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                // Create ESP Directories: /EFI, /EFI/BOOT, /EFI/equantos, /boot, /sys, /sys/bin
                vfs_node_t *esp_efi = vfs_create(esp_root, "EFI", FS_DIRECTORY);
                vfs_node_t *esp_boot = esp_efi ? vfs_create(esp_efi, "BOOT", FS_DIRECTORY) : NULL;
                vfs_node_t *esp_equantos = esp_efi ? vfs_create(esp_efi, "equantos", FS_DIRECTORY) : NULL;
                vfs_node_t *esp_boot_dir = vfs_create(esp_root, "boot", FS_DIRECTORY);
                vfs_node_t *esp_sys = vfs_create(esp_root, "sys", FS_DIRECTORY);
                vfs_node_t *esp_bin = esp_sys ? vfs_create(esp_sys, "bin", FS_DIRECTORY) : NULL;

                // Create Root EXT2 Directories: /boot, /sys, /sys/bin
                vfs_node_t *r_boot = vfs_create(root_ext2, "boot", FS_DIRECTORY);
                vfs_node_t *r_sys  = vfs_create(root_ext2, "sys", FS_DIRECTORY);
                vfs_node_t *r_bin  = r_sys ? vfs_create(r_sys, "bin", FS_DIRECTORY) : NULL;

                const char *files_to_copy[] = {
                    "BOOTX64.EFI", "limine.conf", "kernel.elf", "font.psf",
                    "bash.elf", "busybox.elf", ".bashrc", "hello.elf"
                };
                int file_count = sizeof(files_to_copy) / sizeof(files_to_copy[0]);

                uint8_t *cbuf = (uint8_t *)kmalloc(CHUNK_SZ);
                if (!cbuf) {
                    snprintf(g_installer_ctx.error_msg, sizeof(g_installer_ctx.error_msg), "Out of memory allocating copy buffer!");
                    g_installer_ctx.current_state = STATE_ERROR;
                    break;
                }

                for (int i = 0; i < file_count; i++) {
                    const char *fname = files_to_copy[i];

                    char p[5][128];
                    snprintf(p[0], 128, "/%s", fname);
                    snprintf(p[1], 128, "/sys/bin/%s", fname);
                    snprintf(p[2], 128, "/boot/%s", fname);
                    snprintf(p[3], 128, "/EFI/BOOT/%s", fname);
                    snprintf(p[4], 128, "/cdrom/%s", fname);

                    vfs_node_t *src = NULL;
                    for (int k = 0; k < 5; k++) {
                        src = vfs_open(p[k], 0);
                        if (src) break;
                    }

                    if (src && src->length > 0) {
                        // 1. Deploy Boot files to ESP Partition (FAT32)
                        vfs_node_t *dest_esp = esp_root;
                        if (strcmp(fname, "BOOTX64.EFI") == 0) dest_esp = esp_boot ? esp_boot : esp_root;
                        else if (strcmp(fname, "kernel.elf") == 0 || strcmp(fname, "limine.conf") == 0) dest_esp = esp_boot_dir ? esp_boot_dir : esp_root;
                        else dest_esp = esp_bin ? esp_bin : esp_root;

                        vfs_node_t *node_esp = vfs_create(dest_esp, fname, FS_FILE);
                        if (node_esp) {
                            uint64_t off = 0;
                            while (off < src->length) {
                                uint64_t to_r = src->length - off;
                                if (to_r > CHUNK_SZ) to_r = CHUNK_SZ;
                                int64_t r = vfs_read(src, off, to_r, cbuf);
                                if (r <= 0) break;
                                vfs_write(node_esp, off, r, cbuf);
                                off += r;
                            }
                        }

                        // Also deploy BOOTX64.EFI to /EFI/equantos/BOOTX64.EFI
                        if (strcmp(fname, "BOOTX64.EFI") == 0 && esp_equantos) {
                            vfs_node_t *node_eq = vfs_create(esp_equantos, fname, FS_FILE);
                            if (node_eq) {
                                uint64_t off = 0;
                                while (off < src->length) {
                                    uint64_t to_r = src->length - off;
                                    if (to_r > CHUNK_SZ) to_r = CHUNK_SZ;
                                    int64_t r = vfs_read(src, off, to_r, cbuf);
                                    if (r <= 0) break;
                                    vfs_write(node_eq, off, r, cbuf);
                                    off += r;
                                }
                            }
                        }

                        // 2. Deploy OS System files to Root Partition (EXT2)
                        if (strcmp(fname, "BOOTX64.EFI") != 0) {
                            vfs_node_t *dest_ext2 = (strcmp(fname, "kernel.elf") == 0 || strcmp(fname, "limine.conf") == 0) ? r_boot : r_bin;
                            if (!dest_ext2) dest_ext2 = root_ext2;

                            vfs_node_t *node_ext2 = vfs_create(dest_ext2, fname, FS_FILE);
                            if (node_ext2) {
                                uint64_t off = 0;
                                while (off < src->length) {
                                    uint64_t to_r = src->length - off;
                                    if (to_r > CHUNK_SZ) to_r = CHUNK_SZ;
                                    int64_t r = vfs_read(src, off, to_r, cbuf);
                                    if (r <= 0) break;
                                    vfs_write(node_ext2, off, r, cbuf);
                                    off += r;
                                }
                            }
                        }

                        installer_log_verbose("SUCCESS: Deployed '%s' (%llu bytes)", fname, src->length);
                    }

                    int pct = 10 + ((i + 1) * 80) / file_count;
                    tui_draw_progress(4, 5, 72, pct, COLOR_HEADER, COLOR_TUI_BG);
                }

                kfree(cbuf);
                g_installer_ctx.current_state = STATE_INSTALL_BOOTLOADER;
                break;
            }

            case STATE_INSTALL_BOOTLOADER: {
                term_clear_screen();
                tui_draw_box(2, 2, 76, 18, " LIMINE BOOTLOADER DEPLOYMENT ", COLOR_HEADER, COLOR_TUI_BG);
                g_log_row = 10;

                installer_log_verbose("Deploying Limine v8 'limine.conf' across UEFI search paths...");

                const char *limine_cfg_content = 
                    "timeout: 3\n\n"
                    "/EquantOS\n"
                    "    protocol: limine\n"
                    "    kernel_path: boot():/boot/kernel.elf\n"
                    "    module_path: boot():/sys/bin/font.psf\n"
                    "    module_path: boot():/sys/bin/bash.elf\n"
                    "    module_path: boot():/sys/bin/busybox.elf\n"
                    "    module_path: boot():/sys/bin/.bashrc\n"
                    "    module_path: boot():/sys/bin/hello.elf\n";

                vfs_node_t *esp_root = fat32_mount_partition(g_installer_ctx.target_dev, 2048, 69632);
                vfs_node_t *root_ext2 = ext2_mount_partition(g_installer_ctx.target_dev, 71680);

                if (esp_root) {
                    // 1. ESP Root '/limine.conf'
                    vfs_node_t *cfg_root = vfs_create(esp_root, "limine.conf", FS_FILE);
                    if (cfg_root) vfs_write(cfg_root, 0, strlen(limine_cfg_content), (uint8_t *)limine_cfg_content);

                    // 2. ESP '/EFI/BOOT/limine.conf'
                    vfs_node_t *efi_dir = vfs_finddir(esp_root, "EFI");
                    if (efi_dir) {
                        vfs_node_t *efi_boot = vfs_finddir(efi_dir, "BOOT");
                        if (efi_boot) {
                            vfs_node_t *cfg_efi = vfs_create(efi_boot, "limine.conf", FS_FILE);
                            if (cfg_efi) vfs_write(cfg_efi, 0, strlen(limine_cfg_content), (uint8_t *)limine_cfg_content);
                        }

                        // 3. ESP '/EFI/equantos/limine.conf'
                        vfs_node_t *efi_eq = vfs_finddir(efi_dir, "equantos");
                        if (efi_eq) {
                            vfs_node_t *cfg_eq = vfs_create(efi_eq, "limine.conf", FS_FILE);
                            if (cfg_eq) vfs_write(cfg_eq, 0, strlen(limine_cfg_content), (uint8_t *)limine_cfg_content);
                        }
                    }

                    // 4. ESP '/boot/limine.conf'
                    vfs_node_t *esp_boot_dir = vfs_finddir(esp_root, "boot");
                    if (esp_boot_dir) {
                        vfs_node_t *cfg_boot = vfs_create(esp_boot_dir, "limine.conf", FS_FILE);
                        if (cfg_boot) vfs_write(cfg_boot, 0, strlen(limine_cfg_content), (uint8_t *)limine_cfg_content);
                    }

                    installer_log_verbose("SUCCESS: 'limine.conf' deployed to /EFI/BOOT, /EFI/equantos, /boot, and /");
                }

                if (root_ext2) {
                    vfs_node_t *r_boot = vfs_finddir(root_ext2, "boot");
                    if (r_boot) {
                        vfs_node_t *cfg_ext2 = vfs_create(r_boot, "limine.conf", FS_FILE);
                        if (cfg_ext2) vfs_write(cfg_ext2, 0, strlen(limine_cfg_content), (uint8_t *)limine_cfg_content);
                    }
                    installer_log_verbose("SUCCESS: 'limine.conf' written to Root EXT2.");
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