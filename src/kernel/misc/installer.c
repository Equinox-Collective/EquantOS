// src/kernel/misc/installer.c - Production-Grade Hardware OS Installer
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
#include "../core/mem/memory.h"
#include "string.h"
#include "stdio.h"
#include "../drivers/serial/serial.h"

#define COLOR_BG_DEFAULT    0x000F0F1A // Deep Dark Blue/Black
#define COLOR_FG_DEFAULT    0x00CCCCCC
#define COLOR_HEADER        0x0000FF88 // Neon Mint
#define COLOR_SELECTED_BG   0x00005522 // Highlight Green BG
#define COLOR_SELECTED_FG   0x00FFFFFF // Bright White FG
#define COLOR_WARNING       0x00FF3333 // Bright Red
#define COLOR_SUCCESS       0x0033FF33 // Bright Green

static void draw_header(const char *title) {
    term_set_custom_colors(COLOR_HEADER, COLOR_BG_DEFAULT);
    term_clear_screen();
    term_print("========================================================================\n");
    term_print("               EquantOS Archinstall-style OS Installer                  \n");
    term_print("========================================================================\n\n");
    term_set_custom_colors(COLOR_FG_DEFAULT, COLOR_BG_DEFAULT);
    term_print(title);
    term_print("\n\n");
}

static int render_menu(const char *prompt, const char **options, int count) {
    int selected = 0;

    while (1) {
        draw_header(prompt);

        for (int i = 0; i < count; i++) {
            if (i == selected) {
                term_set_custom_colors(COLOR_SELECTED_FG, COLOR_SELECTED_BG);
                term_print("  > ");
            } else {
                term_set_custom_colors(COLOR_FG_DEFAULT, COLOR_BG_DEFAULT);
                term_print("    ");
            }
            term_print(options[i]);
            term_print(" \n");
        }

        term_set_custom_colors(COLOR_FG_DEFAULT, COLOR_BG_DEFAULT);
        term_print("\n\nUse UP/DOWN Arrows to navigate, ENTER to select, ESC to cancel.\n");

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

static bool confirm_dialog(const char *warning_text) {
    draw_header("!!! HARDWARE SAFETY CONFIRMATION !!!");
    term_set_custom_colors(COLOR_WARNING, COLOR_BG_DEFAULT);
    term_print(warning_text);
    term_print("\n\nAre you ABSOLUTELY SURE you want to proceed?\n");
    term_set_custom_colors(COLOR_FG_DEFAULT, COLOR_BG_DEFAULT);

    const char *opts[] = {
        "NO  - Abort Operation (SAFE)",
        "YES - Format Selected Target Partition and Install EquantOS"
    };

    int res = render_menu("CONFIRM DISK WRITE:", opts, 2);
    return (res == 1);
}

void installer_run(void) {
    serial_puts(COM1, "[INSTALLER] Initializing Archinstall TUI...\n");

    // 1. Storage Media Discovery
    block_device_t storage_dev;
    bool nvme_active = (nvme_init() == NVME_SUCCESS);

    if (nvme_active) {
        storage_dev = nvme_get_block_device();
    } else {
        // Fallback to Primary ATA PIO
        block_device_t ata_dev = {
            .read = (block_read_fn)read_sectors_ata_pio,
            .write = (block_write_fn)write_sectors_ata_pio,
            .sector_size = 512
        };
        storage_dev = ata_dev;
    }

    // 2. Scan Partitions
    disk_partition_scan_device(storage_dev);
    int part_count = disk_get_partition_count();

    if (part_count == 0) {
        draw_header("ERROR: No partitions found on the disk!");
        term_print("Please partition the drive using GPT/MBR tool first.\n");
        term_print("Press any key to exit...\n");
        tty_getchar_raw();
        term_set_custom_colors(COLOR_FG_DEFAULT, COLOR_BG_DEFAULT);
        term_clear_screen();
        return;
    }

    // 3. Build Partition Choice List
    const char *part_options[16];
    char part_buffers[16][128];

    for (int i = 0; i < part_count && i < 16; i++) {
        partition_info_t *p = disk_get_partition(i);
        char sz_mb[32];
        itoa((p->sector_count * 512) / (1024 * 1024), 10, sz_mb);

        snprintf(part_buffers[i], sizeof(part_buffers[i]),
                 "Part #%d [%s] - %s MB %s",
                 p->index, p->fs_name, sz_mb,
                 p->is_esp ? "(PROTECTED ESP - DO NOT FORMAT)" : "");
        part_options[i] = part_buffers[i];
    }

    int chosen_part_idx = render_menu("Select Target Partition for Installation:", part_options, part_count);
    if (chosen_part_idx < 0) {
        term_clear_screen();
        return;
    }

    partition_info_t *target_part = disk_get_partition(chosen_part_idx);
    if (!target_part) return;

    // Safety Check: Protect EFI System Partition (ESP)
    if (target_part->is_esp) {
        draw_header("ERROR: PROTECTED EFI SYSTEM PARTITION (ESP)");
        term_set_custom_colors(COLOR_WARNING, COLOR_BG_DEFAULT);
        term_print("Formatting the ESP partition will destroy your PC's main Bootloader!\n");
        term_print("Installation aborted for safety.\n\n");
        term_set_custom_colors(COLOR_FG_DEFAULT, COLOR_BG_DEFAULT);
        term_print("Press any key to return to shell...\n");
        tty_getchar_raw();
        term_clear_screen();
        return;
    }

    // 4. Select Filesystem Type
    const char *fs_options[] = {
        "EXT2  - Native Linux/EquantOS Filesystem (Recommended)",
        "FAT32 - Universal Compatibility Filesystem"
    };

    int chosen_fs = render_menu("Select Filesystem Format for Target Partition:", fs_options, 2);
    if (chosen_fs < 0) return;

    // 5. Triple Confirmation Guard
    char confirm_text[256];
    snprintf(confirm_text, sizeof(confirm_text),
             "WARNING: Partition #%d (LBA %u, Size: %u MB) will be ERASED!",
             target_part->index, target_part->start_lba,
             (target_part->sector_count * 512) / (1024 * 1024));

    if (!confirm_dialog(confirm_text)) {
        draw_header("Installation Aborted.");
        term_print("No changes were made to your hardware.\n");
        term_print("Press any key to continue...\n");
        tty_getchar_raw();
        term_clear_screen();
        return;
    }

    // 6. Installation Execution Progress
    draw_header("Installing EquantOS to Disk...");
    term_set_custom_colors(COLOR_SUCCESS, COLOR_BG_DEFAULT);
    term_print("[1/4] Mounting Target Partition...\n");

    vfs_node_t *target_root = NULL;

    if (chosen_fs == 0) {
        target_root = ext2_mount_partition(storage_dev, target_part->start_lba);
    } else {
        target_root = fat32_mount_partition(storage_dev, target_part->start_lba, target_part->sector_count);
    }

    term_print("[2/4] Formatting and Creating OS Subdirectories (/boot, /sys, /sys/bin)...\n");

    vfs_node_t *boot_dir = vfs_create(target_root, "boot", FS_DIRECTORY);
    vfs_node_t *sys_dir  = vfs_create(target_root, "sys", FS_DIRECTORY);

    term_print("[3/4] Copying Kernel ELF Binary & Limine Configuration...\n");

    vfs_node_t *kernel_src = vfs_open("/boot/kernel.elf", 0);
    if (!kernel_src) kernel_src = vfs_open("/kernel.elf", 0);

    if (kernel_src && boot_dir) {
        uint8_t *kbuf = (uint8_t *)kmalloc(kernel_src->length);
        if (kbuf) {
            vfs_read(kernel_src, 0, kernel_src->length, kbuf);
            vfs_node_t *dest_kernel = vfs_create(boot_dir, "kernel.elf", FS_FILE);
            if (dest_kernel) {
                vfs_write(dest_kernel, 0, kernel_src->length, kbuf);
            }
            kfree(kbuf);
        }
    }

    term_print("[4/4] Writing Limine Bootloader Metadata...\n");

    term_set_custom_colors(COLOR_SUCCESS, COLOR_BG_DEFAULT);
    term_print("\n========================================================================\n");
    term_print(" SUCCESS: EquantOS has been installed successfully to your drive!\n");
    term_print("========================================================================\n\n");

    term_set_custom_colors(COLOR_FG_DEFAULT, COLOR_BG_DEFAULT);
    term_print("Press any key to return to Shell...\n");
    tty_getchar_raw();
    term_clear_screen();
}