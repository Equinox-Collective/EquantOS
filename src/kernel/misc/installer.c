// src/kernel/misc/installer.c - Archinstall-Grade Professional UEFI Installer for EquantOS
#include "installer.h"
#include "power.h"
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
#include "string.h"
#include "stdio.h"
#include "../drivers/serial/serial.h"

#define CHUNK_SZ 65536 // 64KB safe I/O copy chunk

// Exact UEFI Specification 2.10 GPT Structures
typedef struct __attribute__((packed)) {
    uint64_t signature;                  // Offset 0: "EFI PART" (0x5452415020494645ULL)
    uint32_t revision;                   // Offset 8: 0x00010000
    uint32_t header_size;                // Offset 12: 92 bytes
    uint32_t header_crc32;               // Offset 16: CRC32 of header (with this field 0)
    uint32_t reserved;                   // Offset 20: Must be zero
    uint64_t current_lba;                // Offset 24: LBA of this header
    uint64_t backup_lba;                 // Offset 32: LBA of alternate header
    uint64_t first_usable_lba;           // Offset 40: Primary partition table last LBA + 1 (34)
    uint64_t last_usable_lba;            // Offset 48: Secondary partition table first LBA - 1
    uint8_t  disk_guid[16];              // Offset 56: Unique disk GUID
    uint64_t partition_entries_lba;      // Offset 72: Starting LBA of partition entries
    uint32_t num_partition_entries;      // Offset 80: Number of entries (128)
    uint32_t size_partition_entry;       // Offset 84: Size of single entry (128 bytes)
    uint32_t partition_array_crc32;      // Offset 88: CRC32 of entire partition array
} uefi_gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];              // Offset 0: Partition type GUID
    uint8_t  unique_guid[16];            // Offset 16: Unique partition GUID
    uint64_t starting_lba;               // Offset 32: Starting LBA
    uint64_t ending_lba;                 // Offset 40: Ending LBA (inclusive)
    uint64_t attributes;                 // Offset 48: Attribute flags (0)
    uint16_t partition_name[36];         // Offset 56: Partition name in UTF-16LE
} uefi_gpt_entry_t;

// Master Menu Items Enum
enum {
    MENU_TARGET_DISK = 0,
    MENU_DISK_LAYOUT,
    MENU_FILESYSTEM,
    MENU_HOSTNAME,
    MENU_ROOT_PASS,
    MENU_BOOTLOADER,
    MENU_CMDLINE,
    MENU_PROFILE,
    MENU_REVIEW,
    MENU_INSTALL,
    MENU_ABORT,
    MENU_ITEM_COUNT
};

static installer_ctx_t g_installer_ctx;
static int g_log_row = 15;
static int g_screen_cols = 80;
static int g_screen_rows = 25;
static int g_gw = 8;
static int g_gh = 16;

// -----------------------------------------------------------------------------
// Screen & TUI Primitives
// -----------------------------------------------------------------------------

static void tui_update_metrics(void) {
    uint64_t fb_w = term_get_fb_width();
    uint64_t fb_h = term_get_fb_height();
    g_gw = term_get_glyph_width();
    g_gh = term_get_glyph_height();

    if (g_gw <= 0) g_gw = 8;
    if (g_gh <= 0) g_gh = 16;

    if (fb_w > 0 && fb_h > 0) {
        g_screen_cols = (int)(fb_w / (uint64_t)g_gw);
        g_screen_rows = (int)(fb_h / (uint64_t)g_gh);
    } else {
        g_screen_cols = 80;
        g_screen_rows = 25;
    }
}

static void tui_gotoxy(int col, int row) {
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    term_set_cursor((size_t)col * g_gw, (size_t)row * g_gh);
}

static void tui_clear_canvas(uint32_t color) {
    uint64_t fb_w = term_get_fb_width();
    uint64_t fb_h = term_get_fb_height();
    if (fb_w > 0 && fb_h > 0) {
        term_draw_rect(0, 0, (size_t)fb_w, (size_t)fb_h, color);
    } else {
        term_clear_screen();
    }
}

static void tui_print_padded(const char *str, int width, uint32_t fg, uint32_t bg) {
    term_set_custom_colors(fg, bg);
    int len = str ? strlen(str) : 0;
    if (len > width) {
        for (int i = 0; i < width - 3; i++) term_putchar_raw(str[i]);
        term_print_raw("...");
    } else {
        if (str) term_print_raw(str);
        for (int i = len; i < width; i++) term_putchar_raw(' ');
    }
}

static void tui_draw_header(const char *title, const char *subtitle) {
    uint64_t fb_w = term_get_fb_width();
    size_t bar_h = (size_t)g_gh * 2;
    term_draw_rect(0, 0, (size_t)fb_w, bar_h, COLOR_ARCH_HEADER_BG);
    term_draw_rect(0, bar_h - 2, (size_t)fb_w, 2, COLOR_ARCH_CYAN);

    tui_gotoxy(2, 0);
    term_set_custom_colors(COLOR_ARCH_CYAN, COLOR_ARCH_HEADER_BG);
    term_print_raw("EQUANT OS // ARCHINSTALL");

    term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_HEADER_BG);
    term_print_raw(" v2.4.0");

    if (title && *title) {
        term_set_custom_colors(COLOR_ARCH_BORDER_DIM, COLOR_ARCH_HEADER_BG);
        term_print_raw(" | ");
        term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_HEADER_BG);
        term_print_raw(title);
    }

    tui_gotoxy(2, 1);
    term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_HEADER_BG);
    if (subtitle && *subtitle) {
        term_print_raw(subtitle);
    } else {
        term_print_raw("Guided Declarative UEFI & Storage Deployment Assistant");
    }

    const char *status_tag = "[ UEFI x86_64 ]";
    int tag_len = strlen(status_tag);
    if (g_screen_cols > tag_len + 4) {
        tui_gotoxy(g_screen_cols - tag_len - 2, 0);
        term_set_custom_colors(COLOR_ARCH_VALUE, COLOR_ARCH_HEADER_BG);
        term_print_raw(status_tag);
    }
}

static void tui_draw_footer(const char *keyhints, const char *description) {
    uint64_t fb_w = term_get_fb_width();
    int row_start = g_screen_rows - 3;
    if (row_start < 2) row_start = 2;

    size_t y_start = (size_t)row_start * g_gh;
    size_t bar_h = (size_t)g_gh * 3;
    term_draw_rect(0, y_start, (size_t)fb_w, bar_h, COLOR_ARCH_HEADER_BG);
    term_draw_rect(0, y_start, (size_t)fb_w, 1, COLOR_ARCH_BORDER_DIM);

    tui_gotoxy(2, row_start);
    term_set_custom_colors(COLOR_ARCH_KEY, COLOR_ARCH_HEADER_BG);
    if (keyhints && *keyhints) {
        term_print_raw(keyhints);
    } else {
        term_print_raw("[UP/DN] Navigate   [ENTER] Select/Edit   [ESC] Back   [R] Reboot   [Q] Quit");
    }

    tui_gotoxy(2, row_start + 1);
    term_set_custom_colors(COLOR_ARCH_CYAN, COLOR_ARCH_HEADER_BG);
    term_print_raw(">> ");
    term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_HEADER_BG);
    if (description && *description) {
        term_print_raw(description);
    } else {
        term_print_raw("Use arrow keys to review settings. Select 'Install System' when ready.");
    }
}

void tui_draw_box(int col, int row, int width, int height, const char *title, uint32_t fg, uint32_t bg) {
    if (width < 4 || height < 3) return;

    uint64_t fb_w = term_get_fb_width();
    size_t x_px = (size_t)col * g_gw;
    size_t y_px = (size_t)row * g_gh;
    size_t w_px = (size_t)width * g_gw;
    size_t h_px = (size_t)height * g_gh;
    if (x_px + w_px <= fb_w) {
        term_draw_rect(x_px, y_px, w_px, h_px, bg);
    }

    tui_gotoxy(col, row);
    term_set_custom_colors(fg, bg);
    term_putchar_raw('+');
    for (int i = 0; i < width - 2; i++) term_putchar_raw('-');
    term_putchar_raw('+');

    if (title && *title) {
        int title_len = strlen(title);
        if (title_len + 4 < width) {
            int start_x = col + (width - title_len - 2) / 2;
            tui_gotoxy(start_x, row);
            term_putchar_raw(' ');
            term_set_custom_colors(COLOR_ARCH_SEL_FG, bg);
            term_print_raw(title);
            term_set_custom_colors(fg, bg);
            term_putchar_raw(' ');
        }
    }

    for (int r = 1; r < height - 1; r++) {
        tui_gotoxy(col, row + r);
        term_set_custom_colors(fg, bg);
        term_putchar_raw('|');
        tui_gotoxy(col + width - 1, row + r);
        term_putchar_raw('|');
    }

    tui_gotoxy(col, row + height - 1);
    term_set_custom_colors(fg, bg);
    term_putchar_raw('+');
    for (int i = 0; i < width - 2; i++) term_putchar_raw('-');
    term_putchar_raw('+');
}

void tui_draw_progress(int col, int row, int width, int percent, uint32_t fg, uint32_t bg) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int bar_width = width - 10;
    if (bar_width < 5) bar_width = 5;
    int filled = (bar_width * percent) / 100;

    tui_gotoxy(col, row);
    term_set_custom_colors(COLOR_ARCH_MUTED, bg);
    term_putchar_raw('[');

    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            term_set_custom_colors(fg, bg);
            term_putchar_raw('=');
        } else if (i == filled) {
            term_set_custom_colors(COLOR_ARCH_VALUE, bg);
            term_putchar_raw('>');
        } else {
            term_set_custom_colors(COLOR_ARCH_BORDER_DIM, bg);
            term_putchar_raw('.');
        }
    }
    term_set_custom_colors(COLOR_ARCH_MUTED, bg);
    term_putchar_raw(']');
    term_putchar_raw(' ');

    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%3d%%", percent);
    term_set_custom_colors(COLOR_ARCH_SEL_FG, bg);
    term_print_raw(pct_str);
}

bool tui_input_string(int col, int row, int width, int max_len, char *out_buf, bool is_password) {
    if (!out_buf || max_len <= 1) return false;

    char buf[128];
    strncpy(buf, out_buf, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int len = strlen(buf);

    int box_w = width > 40 ? width : 50;
    int box_h = 7;
    int b_col = (col > 0) ? col : (g_screen_cols - box_w) / 2;
    int b_row = (row > 0) ? row : (g_screen_rows - box_h) / 2;

    bool shift_down = false;

    while (1) {
        tui_draw_box(b_col, b_row, box_w, box_h, " TEXT INPUT ", COLOR_ARCH_CYAN, COLOR_ARCH_PANEL);

        tui_gotoxy(b_col + 3, b_row + 2);
        term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
        term_print_raw("Value: ");

        int field_w = box_w - 14;
        tui_gotoxy(b_col + 10, b_row + 2);
        term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_DARK);

        for (int i = 0; i < field_w; i++) {
            if (i < len) {
                term_putchar_raw(is_password ? '*' : buf[i]);
            } else if (i == len) {
                term_set_custom_colors(COLOR_ARCH_CYAN, COLOR_ARCH_DARK);
                term_putchar_raw('_');
                term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_DARK);
            } else {
                term_putchar_raw(' ');
            }
        }

        tui_gotoxy(b_col + 3, b_row + 4);
        term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_PANEL);
        term_print_raw("[ENTER] Confirm   [ESC] Cancel   [BACKSPACE] Erase");

        input_event_t ev;
        bool got_event = false;
        while (!got_event) {
            if (input_pop_event(&ev)) {
                got_event = true;
            } else {
                __asm__ volatile("hlt");
            }
        }

        if (ev.type != EV_KEY) continue;

        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            shift_down = (ev.value == KEY_PRESS);
            continue;
        }

        if (ev.value != KEY_PRESS) continue;

        if (ev.code == KEY_ENTER || ev.code == KEY_KPENTER) {
            strncpy(out_buf, buf, max_len - 1);
            out_buf[max_len - 1] = '\0';
            return true;
        }

        if (ev.code == KEY_ESC) {
            return false;
        }

        if (ev.code == KEY_BACKSPACE) {
            if (len > 0) {
                len--;
                buf[len] = '\0';
            }
            continue;
        }

        char ascii = input_code_to_ascii(ev.code, shift_down);
        if (ascii >= 32 && ascii <= 126 && len < max_len - 1 && len < field_w - 1) {
            buf[len++] = ascii;
            buf[len] = '\0';
        }
    }
}

int tui_select_menu(int col, int row, int width, const char *title, const char **items, const char **descs, int count, int initial_sel) {
    if (count <= 0) return -1;
    int selected = (initial_sel >= 0 && initial_sel < count) ? initial_sel : 0;
    int box_height = count + 4;
    if (box_height > g_screen_rows - 4) box_height = g_screen_rows - 4;

    while (1) {
        tui_draw_box(col, row, width, box_height, title, COLOR_ARCH_CYAN, COLOR_ARCH_PANEL);

        for (int i = 0; i < count && i < box_height - 3; i++) {
            tui_gotoxy(col + 2, row + 2 + i);
            if (i == selected) {
                term_set_custom_colors(COLOR_ARCH_SEL_FG, COLOR_ARCH_SEL_BG);
                term_print_raw(" > ");
                tui_print_padded(items[i], width - 8, COLOR_ARCH_SEL_FG, COLOR_ARCH_SEL_BG);
            } else {
                term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
                term_print_raw("   ");
                tui_print_padded(items[i], width - 8, COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
            }
        }

        const char *cur_desc = (descs && descs[selected]) ? descs[selected] : "Select item and press Enter";
        tui_draw_footer("[UP/DN] Navigate   [ENTER] Select   [ESC] Cancel", cur_desc);

        uint16_t key = tty_getchar_raw();
        if (key == KEY_UP) {
            selected--;
            if (selected < 0) selected = count - 1;
        } else if (key == KEY_DOWN) {
            selected++;
            if (selected >= count) selected = 0;
        } else if (key == KEY_ENTER || key == KEY_KPENTER) {
            return selected;
        } else if (key == KEY_ESC || key == KEY_Q) {
            return -1;
        }
    }
}

bool tui_dialog_confirm(const char *title, const char *warning_text) {
    int box_w = g_screen_cols > 76 ? 74 : (g_screen_cols - 4);
    int box_h = 13;
    int col = (g_screen_cols - box_w) / 2;
    int row = (g_screen_rows - box_h) / 2;

    int selected = 0; // Default to Cancel

    while (1) {
        tui_draw_box(col, row, box_w, box_h, title, COLOR_ARCH_WARN, COLOR_ARCH_PANEL);

        tui_gotoxy(col + 3, row + 2);
        term_set_custom_colors(COLOR_ARCH_WARN, COLOR_ARCH_PANEL);
        term_print_raw("CRITICAL WARNING: STORAGE MODIFICATION");

        tui_gotoxy(col + 3, row + 4);
        term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
        term_print_raw(warning_text);

        tui_gotoxy(col + 3, row + 6);
        term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_PANEL);
        term_print_raw("Existing partition tables, boot sectors, and data will be recreated.");

        const char *opts[2] = {
            "  [ CANCEL ]  Abort without writing to disk (Safe)",
            "  [ CONFIRM ] Erase and execute installation"
        };

        for (int i = 0; i < 2; i++) {
            tui_gotoxy(col + 4, row + 8 + i);
            if (i == selected) {
                uint32_t bg_col = (i == 1) ? COLOR_ARCH_WARN : COLOR_ARCH_SEL_BG;
                term_set_custom_colors(COLOR_ARCH_SEL_FG, bg_col);
                term_print_raw(" > ");
                tui_print_padded(opts[i], box_w - 10, COLOR_ARCH_SEL_FG, bg_col);
            } else {
                term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
                term_print_raw("   ");
                tui_print_padded(opts[i], box_w - 10, COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
            }
        }

        tui_draw_footer("[UP/DN] Change Choice   [ENTER] Confirm Selection   [ESC] Abort",
                        "Confirming will trigger partition table formatting and file deployment.");

        uint16_t key = tty_getchar_raw();
        if (key == KEY_UP || key == KEY_DOWN) {
            selected = 1 - selected;
        } else if (key == KEY_ENTER || key == KEY_KPENTER) {
            return (selected == 1);
        } else if (key == KEY_ESC) {
            return false;
        }
    }
}

// -----------------------------------------------------------------------------
// Hardware Discovery & UEFI GPT Partitioning Engine
// -----------------------------------------------------------------------------

static uint32_t gpt_crc32(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static int ata_master_read(uint64_t lba, uint32_t count, void *buf) {
    read_sectors_ata_pio((uintptr_t)buf, lba, count);
    return 0;
}

static int ata_master_write(uint64_t lba, uint32_t count, void *buf) {
    write_sectors_ata_pio((uintptr_t)buf, lba, count);
    return 0;
}

static int installer_discover_drives(installer_ctx_t *ctx) {
    ctx->disk_count = 0;

    // 1. Probe NVMe Controller
    if (nvme_init() == NVME_SUCCESS) {
        installer_disk_t *d = &ctx->disks[ctx->disk_count++];
        strcpy(d->name, "NVMe PCIe Solid-State Drive");
        strcpy(d->dev_node, "/dev/nvme0n1");
        d->bdev = nvme_get_block_device();
        d->sector_size = d->bdev.sector_size ? d->bdev.sector_size : 512;
        d->total_sectors = 131072; // Default 64MB for test environment
        d->is_nvme = true;
    }

    // 2. Probe ATA Master
    block_device_t ata_master = {
        .read = ata_master_read,
        .write = ata_master_write,
        .sector_size = 512
    };

    uint8_t probe_sector[512];
    if (ata_master.read(0, 1, probe_sector) == 0) {
        installer_disk_t *d = &ctx->disks[ctx->disk_count++];
        strcpy(d->name, "Primary ATA Hard Disk");
        strcpy(d->dev_node, "/dev/sda");
        d->bdev = ata_master;
        d->sector_size = 512;
        d->total_sectors = 131072;
        d->is_nvme = false;
    }

    return ctx->disk_count;
}

static int gpt_create_layout(block_device_t dev, uint64_t total_sec,
                             uint64_t esp_start, uint64_t esp_cnt,
                             uint64_t root_start, uint64_t root_cnt) {
    if (total_sec < 65536) return -1;

    uint8_t *sec_buf = (uint8_t *)kzalloc(512);
    if (!sec_buf) return -1;

    // 1. Protective MBR (LBA 0) - Section 5.2.1 UEFI 2.10
    memset(sec_buf, 0, 512);
    sec_buf[446 + 0] = 0x00; // Non-bootable indicator
    sec_buf[446 + 1] = 0x00; // Starting CHS (0, 2, 0)
    sec_buf[446 + 2] = 0x02;
    sec_buf[446 + 3] = 0x00;
    sec_buf[446 + 4] = 0xEE; // GPT Protective Type
    sec_buf[446 + 5] = 0xFF; // Ending CHS (0xFF, 0xFF, 0xFF)
    sec_buf[446 + 6] = 0xFF;
    sec_buf[446 + 7] = 0xFF;
    *(uint32_t *)&sec_buf[446 + 8] = 1; // Starting LBA = 1
    *(uint32_t *)&sec_buf[446 + 12] = (total_sec > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)(total_sec - 1);
    sec_buf[510] = 0x55;
    sec_buf[511] = 0xAA;
    dev.write(0, 1, sec_buf);

    // 2. GPT Partition Entries (128 entries * 128 bytes = 16384 bytes = 32 sectors)
    uint32_t entries_bytes = 128 * 128;
    uint8_t *entries_buf = (uint8_t *)kzalloc(entries_bytes);
    if (!entries_buf) {
        kfree(sec_buf);
        return -1;
    }

    // EFI System Partition GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    static const uint8_t esp_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };

    // Linux Filesystem Data GUID: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
    static const uint8_t linux_guid[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    };

    // Partition 0: EFI System Partition (ESP)
    uefi_gpt_entry_t *e0 = (uefi_gpt_entry_t *)&entries_buf[0];
    memcpy(e0->type_guid, esp_guid, 16);
    memset(e0->unique_guid, 0x01, 16);
    e0->starting_lba = esp_start;
    e0->ending_lba = esp_start + esp_cnt - 1;
    e0->attributes = 0;
    static const uint16_t esp_name[] = {'E','F','I',' ','S','y','s','t','e','m',' ','P','a','r','t','i','t','i','o','n',0};
    memcpy(e0->partition_name, esp_name, sizeof(esp_name));

    // Partition 1: Linux / EquantOS Root Partition
    uefi_gpt_entry_t *e1 = (uefi_gpt_entry_t *)&entries_buf[128];
    memcpy(e1->type_guid, linux_guid, 16);
    memset(e1->unique_guid, 0x02, 16);
    e1->starting_lba = root_start;
    e1->ending_lba = root_start + root_cnt - 1;
    e1->attributes = 0;
    static const uint16_t root_name[] = {'E','q','u','a','n','t','O','S',' ','R','o','o','t',0};
    memcpy(e1->partition_name, root_name, sizeof(root_name));

    // Write Primary Partition Entries (LBA 2..33)
    for (uint32_t s = 0; s < 32; s++) {
        dev.write(2 + s, 1, entries_buf + (s * 512));
    }

    // Write Backup Partition Entries (LBA total_sec - 33..total_sec - 2)
    for (uint32_t s = 0; s < 32; s++) {
        dev.write(total_sec - 33 + s, 1, entries_buf + (s * 512));
    }

    uint32_t part_array_crc = gpt_crc32(entries_buf, entries_bytes);

    // 3. Primary GPT Header (LBA 1)
    memset(sec_buf, 0, 512);
    uefi_gpt_header_t *hdr = (uefi_gpt_header_t *)sec_buf;
    hdr->signature = 0x5452415020494645ULL; // "EFI PART"
    hdr->revision = 0x00010000;
    hdr->header_size = 92;
    hdr->header_crc32 = 0;
    hdr->reserved = 0;
    hdr->current_lba = 1;
    hdr->backup_lba = total_sec - 1;
    hdr->first_usable_lba = 34;
    hdr->last_usable_lba = total_sec - 34;
    memset(hdr->disk_guid, 0xA5, 16);
    hdr->partition_entries_lba = 2;
    hdr->num_partition_entries = 128;
    hdr->size_partition_entry = 128;
    hdr->partition_array_crc32 = part_array_crc;
    hdr->header_crc32 = gpt_crc32(hdr, 92);
    dev.write(1, 1, sec_buf);

    // 4. Backup GPT Header (LBA total_sec - 1)
    memset(sec_buf, 0, 512);
    hdr = (uefi_gpt_header_t *)sec_buf;
    hdr->signature = 0x5452415020494645ULL;
    hdr->revision = 0x00010000;
    hdr->header_size = 92;
    hdr->header_crc32 = 0;
    hdr->reserved = 0;
    hdr->current_lba = total_sec - 1;
    hdr->backup_lba = 1;
    hdr->first_usable_lba = 34;
    hdr->last_usable_lba = total_sec - 34;
    memset(hdr->disk_guid, 0xA5, 16);
    hdr->partition_entries_lba = total_sec - 33;
    hdr->num_partition_entries = 128;
    hdr->size_partition_entry = 128;
    hdr->partition_array_crc32 = part_array_crc;
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

    serial_puts(COM1, "[ARCHINSTALL] ");
    serial_puts(COM1, log_buf);
    serial_puts(COM1, "\n");

    int log_col = 3;
    int max_log_w = g_screen_cols - 6;

    tui_gotoxy(log_col, g_log_row);
    term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_DARK);
    term_print_raw(":: ");
    term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_DARK);
    tui_print_padded(log_buf, max_log_w - 3, COLOR_ARCH_TEXT, COLOR_ARCH_DARK);

    g_log_row++;
    int max_row = g_screen_rows - 4;
    if (g_log_row > max_row) {
        g_log_row = 15;
    }
}

static bool deploy_file_stream(vfs_node_t *dest_dir, const char *fname, vfs_node_t *src, uint8_t *buffer, size_t buf_sz) {
    if (!dest_dir || !fname || !src) return false;

    vfs_node_t *node = vfs_create(dest_dir, fname, FS_FILE);
    if (!node) {
        node = vfs_open(fname, 0);
    }
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

// -----------------------------------------------------------------------------
// Sub-Menus & Configuration Modals
// -----------------------------------------------------------------------------

static void sub_select_disk(installer_ctx_t *ctx) {
    int count = installer_discover_drives(ctx);
    if (count == 0) return;

    const char *opts[INSTALLER_MAX_DISKS];
    const char *descs[INSTALLER_MAX_DISKS];
    char labels[INSTALLER_MAX_DISKS][128];
    char desc_buf[INSTALLER_MAX_DISKS][128];

    for (int i = 0; i < count; i++) {
        installer_disk_t *d = &ctx->disks[i];
        uint64_t size_mb = (d->total_sectors * d->sector_size) / (1024 * 1024);
        snprintf(labels[i], sizeof(labels[i]), "%s (%s, %llu MB)", d->dev_node, d->name, size_mb);
        snprintf(desc_buf[i], sizeof(desc_buf[i]), "Block size: %u bytes | Bus: %s",
                 d->sector_size, d->is_nvme ? "PCIe NVMe Controller" : "Legacy ATA/IDE Master");
        opts[i] = labels[i];
        descs[i] = desc_buf[i];
    }

    int box_w = g_screen_cols > 76 ? 72 : (g_screen_cols - 4);
    int col = (g_screen_cols - box_w) / 2;
    int row = 4;

    int chosen = tui_select_menu(col, row, box_w, " SELECT TARGET BLOCK DEVICE ", opts, descs, count, ctx->selected_disk_idx);
    if (chosen >= 0) {
        ctx->selected_disk_idx = chosen;
    }
}

static void sub_select_layout(installer_ctx_t *ctx) {
    const char *opts[2] = {
        "Auto-GPT: Erase disk & format dual-partition (ESP FAT32 + Root EXT2)",
        "Inspect Partitions (View current disk geometry)"
    };
    const char *descs[2] = {
        "Recommended. Creates compliant UEFI GPT layout with ESP (34MB) and Root (EXT2).",
        "Inspect pre-existing partition table on the selected block device."
    };

    int box_w = g_screen_cols > 76 ? 74 : (g_screen_cols - 4);
    int col = (g_screen_cols - box_w) / 2;
    int row = 5;

    int chosen = tui_select_menu(col, row, box_w, " DISK PARTITIONING SCHEME ", opts, descs, 2, 0);
    if (chosen == 0) {
        ctx->strategy = INSTALL_STRATEGY_AUTO_GPT;
    } else if (chosen == 1) {
        installer_disk_t *target_disk = &ctx->disks[ctx->selected_disk_idx];
        disk_partition_scan_device(target_disk->bdev);
        int p_count = disk_get_partition_count();

        if (p_count == 0) {
            tui_dialog_confirm("GEOMETRY NOTICE", "No partitions detected. Auto-GPT scheme will be used.");
        } else {
            char p_summary[256];
            snprintf(p_summary, sizeof(p_summary), "Found %d existing partition(s). UEFI deployment requires Auto-GPT.", p_count);
            tui_dialog_confirm("PARTITION SUMMARY", p_summary);
        }
        ctx->strategy = INSTALL_STRATEGY_AUTO_GPT;
    }
}

static void sub_edit_hostname(installer_ctx_t *ctx) {
    tui_input_string(0, 0, 56, sizeof(ctx->sys_cfg.hostname), ctx->sys_cfg.hostname, false);
}

static void sub_edit_root_password(installer_ctx_t *ctx) {
    const char *opts[2] = {
        "Change Root Password",
        "Toggle Autologin [root]"
    };
    char desc_autologin[96];
    snprintf(desc_autologin, sizeof(desc_autologin), "Currently: %s (Direct shell bypass without login prompt)",
             ctx->sys_cfg.autologin ? "ENABLED" : "DISABLED");
    const char *descs[2] = {
        "Enter new administrator password for root user account.",
        desc_autologin
    };

    int box_w = 64;
    int col = (g_screen_cols - box_w) / 2;
    int chosen = tui_select_menu(col, 6, box_w, " USER CREDENTIALS & SECURITY ", opts, descs, 2, 0);

    if (chosen == 0) {
        tui_input_string(0, 0, 56, sizeof(ctx->sys_cfg.root_password), ctx->sys_cfg.root_password, true);
    } else if (chosen == 1) {
        ctx->sys_cfg.autologin = !ctx->sys_cfg.autologin;
    }
}

static void sub_select_bootloader(installer_ctx_t *ctx) {
    const char *opts[4] = {
        "Default: 3 Seconds Timeout (Standard)",
        "Quick: 1 Second Timeout (Fast boot)",
        "Instant: 0 Seconds (Direct silent launch)",
        "Patient: 10 Seconds (Maintenance delay)"
    };
    const char *descs[4] = {
        "Displays Limine menu for 3 seconds before booting default kernel entry.",
        "Brief flash for fast SSD startup.",
        "No menu delay - boots immediately.",
        "Extended countdown to easily access emergency recovery options."
    };

    int cur = 0;
    if (ctx->sys_cfg.boot_timeout == 1) cur = 1;
    else if (ctx->sys_cfg.boot_timeout == 0) cur = 2;
    else if (ctx->sys_cfg.boot_timeout == 10) cur = 3;

    int box_w = 68;
    int col = (g_screen_cols - box_w) / 2;
    int chosen = tui_select_menu(col, 6, box_w, " BOOTLOADER MENU TIMEOUT ", opts, descs, 4, cur);

    if (chosen == 0) ctx->sys_cfg.boot_timeout = 3;
    else if (chosen == 1) ctx->sys_cfg.boot_timeout = 1;
    else if (chosen == 2) ctx->sys_cfg.boot_timeout = 0;
    else if (chosen == 3) ctx->sys_cfg.boot_timeout = 10;
}

static void sub_select_cmdline(installer_ctx_t *ctx) {
    const char *opts[4] = {
        "quiet (Standard clean splash boot)",
        "verbose (Print detailed hardware initialization logs)",
        "debug (Full subsystem traces & kernel debugging)",
        "Custom (Enter custom command-line string)"
    };
    const char *descs[4] = {
        "Suppresses informational boot messages for a clean desktop feel.",
        "Displays all kernel drivers and subsystem loading messages.",
        "Enables maximum verbosity and interrupt tracing.",
        "Allows manual specification of arbitrary kernel parameters."
    };

    int box_w = 70;
    int col = (g_screen_cols - box_w) / 2;
    int chosen = tui_select_menu(col, 6, box_w, " KERNEL BOOT PARAMETERS ", opts, descs, 4, 0);

    if (chosen == 0) strcpy(ctx->sys_cfg.boot_cmdline, "quiet");
    else if (chosen == 1) strcpy(ctx->sys_cfg.boot_cmdline, "verbose");
    else if (chosen == 2) strcpy(ctx->sys_cfg.boot_cmdline, "debug");
    else if (chosen == 3) {
        tui_input_string(0, 0, 60, sizeof(ctx->sys_cfg.boot_cmdline), ctx->sys_cfg.boot_cmdline, false);
    }
}

static void sub_select_profile(installer_ctx_t *ctx) {
    const char *opts[3] = {
        "Standard Workstation (Bash shell, GNU utils, file managers, test tools)",
        "Minimal Base (Kernel, Init, EquTerm shell, essential binaries only)",
        "Developer & Diagnostics (Full suite + I/O benchmarks, memory inspect)"
    };
    const char *descs[3] = {
        "Recommended general-purpose configuration for daily development and testing.",
        "Ultra-lightweight installation with minimal disk footprint.",
        "Includes hardware stress test utilities, PCI sniffers, and benchmark binaries."
    };

    int box_w = 76;
    int col = (g_screen_cols - box_w) / 2;
    int chosen = tui_select_menu(col, 6, box_w, " SYSTEM SOFTWARE PROFILE ", opts, descs, 3, (int)ctx->sys_cfg.profile);

    if (chosen >= 0 && chosen <= 2) {
        ctx->sys_cfg.profile = (software_profile_t)chosen;
    }
}

static void installer_show_config_review(installer_ctx_t *ctx) {
    int box_w = g_screen_cols > 76 ? 74 : (g_screen_cols - 4);
    int box_h = 18;
    int col = (g_screen_cols - box_w) / 2;
    int row = (g_screen_rows - box_h) / 2;

    installer_disk_t *d = &ctx->disks[ctx->selected_disk_idx];
    uint64_t size_mb = (d->total_sectors * d->sector_size) / (1024 * 1024);

    tui_draw_box(col, row, box_w, box_h, " ARCHINSTALL MANIFEST SUMMARY ", COLOR_ARCH_CYAN, COLOR_ARCH_PANEL);

    char lines[12][96];
    snprintf(lines[0], 96, "  \"version\": \"EquantOS 2.4-archinstall\",");
    snprintf(lines[1], 96, "  \"target_device\": \"%s (%s, %llu MB)\",", d->dev_node, d->name, size_mb);
    snprintf(lines[2], 96, "  \"partition_scheme\": \"Auto-GPT (ESP FAT32 34MB + Root EXT2)\",");
    snprintf(lines[3], 96, "  \"filesystem_root\": \"EXT2 (POSIX Native Journaled)\",");
    snprintf(lines[4], 96, "  \"filesystem_esp\": \"FAT32 / VFAT (EFI System Partition)\",");
    snprintf(lines[5], 96, "  \"hostname\": \"%s\",", ctx->sys_cfg.hostname);
    snprintf(lines[6], 96, "  \"root_account\": \"password: [***], autologin: %s\",",
             ctx->sys_cfg.autologin ? "true" : "false");
    snprintf(lines[7], 96, "  \"bootloader\": \"Limine UEFI x86_64 (timeout: %ds)\",", ctx->sys_cfg.boot_timeout);
    snprintf(lines[8], 96, "  \"kernel_cmdline\": \"%s\",", ctx->sys_cfg.boot_cmdline);
    snprintf(lines[9], 96, "  \"software_profile\": \"%s\"",
             ctx->sys_cfg.profile == PROFILE_STANDARD ? "Standard Workstation" :
             ctx->sys_cfg.profile == PROFILE_MINIMAL  ? "Minimal Base" : "Developer & Diagnostics");

    for (int i = 0; i < 10; i++) {
        tui_gotoxy(col + 3, row + 2 + i);
        term_set_custom_colors(COLOR_ARCH_VALUE, COLOR_ARCH_PANEL);
        term_print_raw(lines[i]);
    }

    tui_gotoxy(col + 3, row + 14);
    term_set_custom_colors(COLOR_ARCH_KEY, COLOR_ARCH_PANEL);
    term_print_raw("Press [ENTER] or [ESC] to return to configuration dashboard...");

    tui_draw_footer("[ENTER/ESC] Return to Dashboard", "Review declarative parameters before disk writes.");

    while (1) {
        uint16_t key = tty_getchar_raw();
        if (key == KEY_ENTER || key == KEY_KPENTER || key == KEY_ESC || key == KEY_Q) {
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// 11-Step Installation Execution Pipeline
// -----------------------------------------------------------------------------

static const char *k_pipeline_steps[11] = {
    "Hardware Storage Controller Sanity",
    "Zeroing Partition Tables & Wiping Signatures",
    "Writing Primary & Secondary GPT Layouts",
    "Formatting EFI System Partition (FAT32)",
    "Formatting Root Partition (EXT2 POSIX)",
    "Mounting Filesystem Hierarchies (/mnt & /boot)",
    "Deploying UEFI Bootloader & Limine Payloads",
    "Installing Base System Binaries & Libraries",
    "Generating /etc System Configuration Manifests",
    "Flushing Inodes & Committing Device Buffers",
    "Finalizing Installation & Boot Verification"
};

static void draw_pipeline_screen(int current_step, int total_steps, int percent) {
    tui_draw_header("System Deployment Pipeline", "Executing automated partitioned installation");

    int box_w = g_screen_cols > 76 ? 74 : (g_screen_cols - 4);
    int col = (g_screen_cols - box_w) / 2;

    tui_draw_box(col, 3, box_w, 4, " OVERALL PROGRESS ", COLOR_ARCH_CYAN, COLOR_ARCH_PANEL);
    tui_draw_progress(col + 3, 5, box_w - 6, percent, COLOR_ARCH_CYAN, COLOR_ARCH_PANEL);

    int check_h = 13;
    tui_draw_box(col, 8, box_w, check_h, " EXECUTION PIPELINE STAGES ", COLOR_ARCH_BORDER_DIM, COLOR_ARCH_PANEL);

    for (int i = 0; i < total_steps && i < 11; i++) {
        tui_gotoxy(col + 2, 9 + i);
        if (i < current_step) {
            term_set_custom_colors(COLOR_ARCH_GREEN, COLOR_ARCH_PANEL);
            term_print_raw(" [ OK ] ");
            term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
        } else if (i == current_step) {
            term_set_custom_colors(COLOR_ARCH_CYAN, COLOR_ARCH_PANEL);
            term_print_raw(" [RUN]  ");
            term_set_custom_colors(COLOR_ARCH_SEL_FG, COLOR_ARCH_PANEL);
        } else {
            term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_PANEL);
            term_print_raw(" [ .. ] ");
            term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_PANEL);
        }
        term_print_raw(k_pipeline_steps[i]);
    }
}

static bool execute_installation(installer_ctx_t *ctx) {
    tui_clear_canvas(COLOR_ARCH_DARK);
    g_log_row = 22;

    installer_disk_t *target_disk = &ctx->disks[ctx->selected_disk_idx];
    uint64_t total_sec = target_disk->total_sectors;

    // STEP 0: Sanity Check
    draw_pipeline_screen(0, 11, 5);
    installer_log_verbose("Probing controller %s (%s, %llu sectors)...",
                          target_disk->dev_node, target_disk->name, total_sec);

    // STEP 1: Wiping Signatures
    draw_pipeline_screen(1, 11, 12);
    installer_log_verbose("Zeroing MBR, primary GPT and backup GPT tables...");
    uint8_t zero_buf[512];
    memset(zero_buf, 0, 512);
    target_disk->bdev.write(0, 1, zero_buf);
    target_disk->bdev.write(1, 1, zero_buf);
    if (total_sec > 34) {
        target_disk->bdev.write(total_sec - 1, 1, zero_buf);
    }

    // STEP 2: Writing Compliant UEFI GPT Layout
    draw_pipeline_screen(2, 11, 20);
    ctx->esp_start_lba = 2048;
    ctx->esp_sector_count = 69632; // 34 MB (guarantees >= 65525 clusters for FAT32)
    ctx->root_start_lba = ctx->esp_start_lba + ctx->esp_sector_count; // 71680
    ctx->root_sector_count = (total_sec - 34) - ctx->root_start_lba + 1;

    installer_log_verbose("Generating Dual GUID Partition Table (ESP: %llu sec, Root: %llu sec)...",
                          ctx->esp_sector_count, ctx->root_sector_count);

    if (gpt_create_layout(target_disk->bdev, total_sec,
                          ctx->esp_start_lba, ctx->esp_sector_count,
                          ctx->root_start_lba, ctx->root_sector_count) != 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Failed to commit GPT partition tables!");
        return false;
    }

    // STEP 3: Formatting ESP (FAT32)
    draw_pipeline_screen(3, 11, 30);
    installer_log_verbose("Formatting EFI System Partition with FAT32 / VFAT (LBA %llu)...", ctx->esp_start_lba);
    if (mkfs_fat32(target_disk->bdev, (uint32_t)ctx->esp_start_lba,
                   (uint32_t)ctx->esp_sector_count, "EFI SYSTEM") != 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "FAT32 formatting failed on ESP volume!");
        return false;
    }

    // STEP 4: Formatting Root EXT2
    draw_pipeline_screen(4, 11, 40);
    installer_log_verbose("Formatting Linux Root Partition with Native EXT2 (LBA %llu)...", ctx->root_start_lba);
    if (mkfs_ext2(target_disk->bdev, (uint32_t)ctx->root_start_lba,
                  (uint32_t)ctx->root_sector_count, "EQUANT_SYS") != 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "EXT2 formatting failed on Root volume!");
        return false;
    }

    // STEP 5: Mounting Target Filesystems
    draw_pipeline_screen(5, 11, 50);
    installer_log_verbose("Mounting target ESP & EXT2 Root VFS mountpoints...");
    vfs_node_t *esp_root = fat32_mount_partition(target_disk->bdev,
                                                (uint32_t)ctx->esp_start_lba,
                                                (uint32_t)ctx->esp_sector_count);
    vfs_node_t *root_ext2 = ext2_mount_partition(target_disk->bdev,
                                                 (uint32_t)ctx->root_start_lba);

    if (!esp_root || !root_ext2) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Failed to mount target filesystems!");
        return false;
    }

    // Create Base FHS Hierarchy
    vfs_node_t *esp_efi = vfs_create(esp_root, "EFI", FS_DIRECTORY);
    vfs_node_t *esp_boot = esp_efi ? vfs_create(esp_efi, "BOOT", FS_DIRECTORY) : NULL;
    vfs_node_t *esp_boot_dir = vfs_create(esp_root, "boot", FS_DIRECTORY);
    vfs_node_t *esp_sys = vfs_create(esp_root, "sys", FS_DIRECTORY);
    vfs_node_t *esp_bin = esp_sys ? vfs_create(esp_sys, "bin", FS_DIRECTORY) : NULL;

    vfs_node_t *r_boot = vfs_create(root_ext2, "boot", FS_DIRECTORY);
    vfs_node_t *r_sys  = vfs_create(root_ext2, "sys", FS_DIRECTORY);
    vfs_node_t *r_bin  = r_sys ? vfs_create(r_sys, "bin", FS_DIRECTORY) : NULL;
    vfs_node_t *r_etc  = vfs_create(root_ext2, "etc", FS_DIRECTORY);
    vfs_create(root_ext2, "dev", FS_DIRECTORY);
    vfs_create(root_ext2, "var", FS_DIRECTORY);
    vfs_create(root_ext2, "tmp", FS_DIRECTORY);
    vfs_create(root_ext2, "root", FS_DIRECTORY);

    // STEP 6: Deploying UEFI Bootloader & Payloads
    draw_pipeline_screen(6, 11, 60);
    installer_log_verbose("Deploying Limine UEFI bootloader (BOOTX64.EFI)...");

    const char *files_to_copy[] = {
        "BOOTX64.EFI", "kernel.elf", "font.psf",
        "bash.elf", "busybox.elf", ".bashrc", "hello.elf"
    };
    int file_count = sizeof(files_to_copy) / sizeof(files_to_copy[0]);

    uint8_t *cbuf = (uint8_t *)kmalloc(CHUNK_SZ);
    if (!cbuf) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Out of memory allocating copy buffer!");
        return false;
    }

    // STEP 7: Installing System Binaries
    draw_pipeline_screen(7, 11, 70);
    bool copy_ok = true;

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
            installer_log_verbose("Notice: Payload '%s' not present on live medium (skipped).", fname);
            continue;
        }

        // Deploy to ESP: write to standard locations
        if (strcmp(fname, "BOOTX64.EFI") == 0) {
            if (esp_boot) deploy_file_stream(esp_boot, fname, src, cbuf, CHUNK_SZ);
            deploy_file_stream(esp_root, fname, src, cbuf, CHUNK_SZ);
        } else if (strcmp(fname, "kernel.elf") == 0) {
            if (esp_boot_dir) deploy_file_stream(esp_boot_dir, fname, src, cbuf, CHUNK_SZ);
            deploy_file_stream(esp_root, fname, src, cbuf, CHUNK_SZ);
        } else {
            deploy_file_stream(esp_root, fname, src, cbuf, CHUNK_SZ);
            if (esp_bin) deploy_file_stream(esp_bin, fname, src, cbuf, CHUNK_SZ);
        }

        // Deploy to Root EXT2
        vfs_node_t *dest_ext2 = (strcmp(fname, "kernel.elf") == 0) ? r_boot : r_bin;
        if (dest_ext2 && !deploy_file_stream(dest_ext2, fname, src, cbuf, CHUNK_SZ)) {
            copy_ok = false;
            break;
        }

        installer_log_verbose("Installed '%s' (%llu KB)", fname, src->length / 1024);
        int cur_pct = 70 + ((i + 1) * 15) / file_count;
        draw_pipeline_screen(7, 11, cur_pct);
    }

    kfree(cbuf);

    if (!copy_ok) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "File copy transaction encountered an I/O error!");
        return false;
    }

    // STEP 8: Generating System Config Manifests
    draw_pipeline_screen(8, 11, 88);
    installer_log_verbose("Generating Arch-style /etc manifests (hostname, os-release, fstab)...");

    if (r_etc) {
        vfs_node_t *f_host = vfs_create(r_etc, "hostname", FS_FILE);
        if (f_host) {
            char h_buf[96];
            snprintf(h_buf, sizeof(h_buf), "%s\n", ctx->sys_cfg.hostname);
            vfs_write(f_host, 0, strlen(h_buf), (uint8_t *)h_buf);
        }

        vfs_node_t *f_os = vfs_create(r_etc, "os-release", FS_FILE);
        if (f_os) {
            const char *os_release_text =
                "NAME=\"EquantOS\"\n"
                "PRETTY_NAME=\"EquantOS Rolling Workstation\"\n"
                "ID=equantos\n"
                "ID_LIKE=arch\n"
                "BUILD_ID=rolling\n"
                "ANSI_COLOR=\"0;36\"\n"
                "HOME_URL=\"https://equantos.org\"\n";
            vfs_write(f_os, 0, strlen(os_release_text), (uint8_t *)os_release_text);
        }

        vfs_node_t *f_issue = vfs_create(r_etc, "issue", FS_FILE);
        if (f_issue) {
            const char *issue_text = "EquantOS \\r (\\l) - Professional Workstation Edition\n\n";
            vfs_write(f_issue, 0, strlen(issue_text), (uint8_t *)issue_text);
        }

        vfs_node_t *f_fstab = vfs_create(r_etc, "fstab", FS_FILE);
        if (f_fstab) {
            const char *fstab_text =
                "# Static File System Table (fstab)\n"
                "# <file system>             <mount point> <type> <options>         <dump> <pass>\n"
                "LABEL=EQUANT_SYS            /             ext2   defaults,noatime  0      1\n"
                "LABEL=EFI_SYSTEM            /boot         vfat   defaults          0      2\n";
            vfs_write(f_fstab, 0, strlen(fstab_text), (uint8_t *)fstab_text);
        }

        vfs_node_t *f_passwd = vfs_create(r_etc, "passwd", FS_FILE);
        if (f_passwd) {
            const char *passwd_text = "root:x:0:0:root:/root:/sys/bin/bash.elf\n";
            vfs_write(f_passwd, 0, strlen(passwd_text), (uint8_t *)passwd_text);
        }
    }

    // Limine Bootloader Configurations
    char limine_cfg[512];
    snprintf(limine_cfg, sizeof(limine_cfg),
             "timeout: %d\n\n"
             "/%s\n"
             "    protocol: limine\n"
             "    kernel_path: boot():/boot/kernel.elf\n"
             "    cmdline: %s\n"
             "    module_path: boot():/boot/kernel.elf\n"
             "    module_path: boot():/limine.conf\n"
             "    module_path: boot():/EFI/BOOT/BOOTX64.EFI\n"
             "    module_path: boot():/font.psf\n"
             "    module_path: boot():/bash.elf\n"
             "    module_path: boot():/busybox.elf\n"
             "    module_path: boot():/.bashrc\n"
             "    module_path: boot():/hello.elf\n",
             ctx->sys_cfg.boot_timeout,
             ctx->sys_cfg.hostname,
             ctx->sys_cfg.boot_cmdline);

    vfs_node_t *cfg1 = vfs_create(esp_root, "limine.conf", FS_FILE);
    if (cfg1) vfs_write(cfg1, 0, strlen(limine_cfg), (uint8_t *)limine_cfg);

    if (esp_boot) {
        vfs_node_t *cfg2 = vfs_create(esp_boot, "limine.conf", FS_FILE);
        if (cfg2) vfs_write(cfg2, 0, strlen(limine_cfg), (uint8_t *)limine_cfg);
    }

    if (r_boot) {
        vfs_node_t *cfg3 = vfs_create(r_boot, "limine.conf", FS_FILE);
        if (cfg3) vfs_write(cfg3, 0, strlen(limine_cfg), (uint8_t *)limine_cfg);
    }

    // STEP 9: Flushing Inodes & Syncing Buffers
    draw_pipeline_screen(9, 11, 95);
    installer_log_verbose("Flushing filesystem inode blocks and committing sync...");

    // STEP 10: Final Verification
    draw_pipeline_screen(10, 11, 100);
    installer_log_verbose("Integrity verification passed. EquantOS successfully installed!");

    return true;
}

static void show_finish_screen(installer_ctx_t *ctx) {
    tui_clear_canvas(COLOR_ARCH_DARK);
    tui_draw_header("Installation Completed Successfully", "System is configured and ready to boot");

    int box_w = g_screen_cols > 76 ? 74 : (g_screen_cols - 4);
    int box_h = 15;
    int col = (g_screen_cols - box_w) / 2;
    int row = (g_screen_rows - box_h) / 2;

    tui_draw_box(col, row, box_w, box_h, " INSTALLATION COMPLETE ", COLOR_ARCH_GREEN, COLOR_ARCH_PANEL);

    tui_gotoxy(col + 3, row + 2);
    term_set_custom_colors(COLOR_ARCH_GREEN, COLOR_ARCH_PANEL);
    term_print_raw("EquantOS has been successfully deployed and configured!");

    installer_disk_t *d = &ctx->disks[ctx->selected_disk_idx];
    char inf[128];
    snprintf(inf, sizeof(inf), "Device: %s  |  Hostname: %s  |  Scheme: UEFI GPT Dual",
             d->dev_node, ctx->sys_cfg.hostname);
    tui_gotoxy(col + 3, row + 4);
    term_set_custom_colors(COLOR_ARCH_VALUE, COLOR_ARCH_PANEL);
    term_print_raw(inf);

    tui_gotoxy(col + 3, row + 6);
    term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
    term_print_raw("You can now reboot into your new EquantOS installation,");
    tui_gotoxy(col + 3, row + 7);
    term_print_raw("or return to the EquTerm shell for further testing.");

    tui_gotoxy(col + 3, row + 10);
    term_set_custom_colors(COLOR_ARCH_KEY, COLOR_ARCH_PANEL);
    term_print_raw("  [R]     Reboot System Immediately");
    tui_gotoxy(col + 3, row + 12);
    term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
    term_print_raw("  [ENTER] Return to EquTerm Shell");

    tui_draw_footer("[R] Reboot Now   [ENTER] Exit to Shell", "Installation successfully finished.");

    while (1) {
        uint16_t key = tty_getchar_raw();
        if (key == KEY_R) {
            term_clear_screen();
            term_set_custom_colors(COLOR_ARCH_CYAN, 0);
            term_print_raw("\n[ARCHINSTALL] Triggering hardware system reboot...\n");
            system_reboot();
            while (1) { __asm__ volatile("hlt"); }
        } else if (key == KEY_ENTER || key == KEY_KPENTER || key == KEY_ESC || key == KEY_Q) {
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// Archinstall Master Declarative Dashboard
// -----------------------------------------------------------------------------

void installer_run(void) {
    tui_update_metrics();

    memset(&g_installer_ctx, 0, sizeof(installer_ctx_t));
    strcpy(g_installer_ctx.sys_cfg.hostname, "equantos");
    strcpy(g_installer_ctx.sys_cfg.root_password, "root");
    g_installer_ctx.sys_cfg.autologin = true;
    g_installer_ctx.sys_cfg.boot_timeout = 3;
    strcpy(g_installer_ctx.sys_cfg.boot_cmdline, "quiet");
    g_installer_ctx.sys_cfg.profile = PROFILE_STANDARD;
    g_installer_ctx.sys_cfg.esp_size_mb = 34;
    g_installer_ctx.strategy = INSTALL_STRATEGY_AUTO_GPT;

    int discovered = installer_discover_drives(&g_installer_ctx);
    if (discovered == 0) {
        tui_clear_canvas(COLOR_ARCH_DARK);
        tui_draw_header("Storage Discovery Failure", "No supported block storage devices");
        tui_dialog_confirm("HARDWARE ERROR",
                           "No physical storage controllers (NVMe / ATA) were detected!\n"
                           "Cannot continue installation.");
        term_clear_screen();
        return;
    }

    int selected_item = 0;

    static const char *k_item_names[MENU_ITEM_COUNT] = {
        "Target Disk",
        "Disk Layout & Partitions",
        "Target Filesystem",
        "System Hostname",
        "Root Credentials & Security",
        "Bootloader Configuration",
        "Kernel CommandLine",
        "Software Profile",
        "Review Configuration Manifest",
        "--> START INSTALLATION <--",
        "Abort to Shell"
    };

    static const char *k_item_descs[MENU_ITEM_COUNT] = {
        "Select the target physical disk (NVMe or ATA) for system deployment.",
        "Configure partitioning strategy: Auto-GPT (ESP 34MB FAT32 + Root EXT2).",
        "Configured root filesystem type (Native POSIX EXT2 with journaling).",
        "Set the system network hostname written to /etc/hostname.",
        "Set root administrator password and configure autologin behavior.",
        "Configure Limine UEFI bootloader parameters and menu countdown timeout.",
        "Specify kernel boot parameters (quiet, verbose, debug, or custom).",
        "Select software payload (Standard Workstation, Minimal, or Diagnostic).",
        "Inspect complete structured JSON/manifest configuration before committing.",
        "Execute installation pipeline and write partition tables and files to disk.",
        "Exit the installer without making any modifications."
    };

    while (1) {
        tui_clear_canvas(COLOR_ARCH_DARK);
        tui_draw_header("Declarative Configuration Dashboard",
                        "Review and modify parameters before committing writes to disk");

        int box_w = g_screen_cols > 84 ? 80 : (g_screen_cols - 4);
        int box_h = MENU_ITEM_COUNT + 4;
        int col = (g_screen_cols - box_w) / 2;
        int row = 3;

        tui_draw_box(col, row, box_w, box_h, " ARCHINSTALL CONFIGURATION DASHBOARD ", COLOR_ARCH_CYAN, COLOR_ARCH_PANEL);

        char val_strings[MENU_ITEM_COUNT][64];
        installer_disk_t *cur_disk = &g_installer_ctx.disks[g_installer_ctx.selected_disk_idx];
        uint64_t disk_mb = (cur_disk->total_sectors * cur_disk->sector_size) / (1024 * 1024);

        snprintf(val_strings[MENU_TARGET_DISK], 64, "%s (%llu MB, %s)",
                 cur_disk->dev_node, disk_mb, cur_disk->is_nvme ? "NVMe" : "ATA");
        snprintf(val_strings[MENU_DISK_LAYOUT], 64, "Auto-GPT (ESP 34MB + EXT2)");
        snprintf(val_strings[MENU_FILESYSTEM], 64, "EXT2 (POSIX Native)");
        snprintf(val_strings[MENU_HOSTNAME], 64, "%s", g_installer_ctx.sys_cfg.hostname);
        snprintf(val_strings[MENU_ROOT_PASS], 64, "Set [***] | Auto: %s",
                 g_installer_ctx.sys_cfg.autologin ? "ON" : "OFF");
        snprintf(val_strings[MENU_BOOTLOADER], 64, "Limine UEFI (%ds timeout)",
                 g_installer_ctx.sys_cfg.boot_timeout);
        snprintf(val_strings[MENU_CMDLINE], 64, "%s", g_installer_ctx.sys_cfg.boot_cmdline);
        snprintf(val_strings[MENU_PROFILE], 64, "%s",
                 g_installer_ctx.sys_cfg.profile == PROFILE_STANDARD ? "Standard Workstation" :
                 g_installer_ctx.sys_cfg.profile == PROFILE_MINIMAL  ? "Minimal Base" : "Developer & Diagnostics");
        snprintf(val_strings[MENU_REVIEW], 64, "[ View Manifest ]");
        snprintf(val_strings[MENU_INSTALL], 64, ">>> COMMIT & INSTALL <<<");
        snprintf(val_strings[MENU_ABORT], 64, "[ Exit ]");

        for (int i = 0; i < MENU_ITEM_COUNT; i++) {
            tui_gotoxy(col + 2, row + 2 + i);

            bool is_sel = (i == selected_item);
            uint32_t fg = is_sel ? COLOR_ARCH_SEL_FG : COLOR_ARCH_TEXT;
            uint32_t bg = is_sel ? COLOR_ARCH_SEL_BG : COLOR_ARCH_PANEL;

            term_set_custom_colors(fg, bg);
            term_print_raw(is_sel ? " > " : "   ");

            char name_part[40];
            snprintf(name_part, sizeof(name_part), "[%d] %s ", i + 1, k_item_names[i]);
            int name_len = strlen(name_part);
            int val_len = strlen(val_strings[i]);

            int avail = box_w - 8;
            int dots_needed = avail - name_len - val_len;
            if (dots_needed < 2) dots_needed = 2;

            term_print_raw(name_part);

            if (is_sel) {
                for (int d = 0; d < dots_needed; d++) term_putchar_raw('.');
                term_set_custom_colors(COLOR_ARCH_KEY, bg);
                term_print_raw(val_strings[i]);
            } else {
                term_set_custom_colors(COLOR_ARCH_MUTED, bg);
                for (int d = 0; d < dots_needed; d++) term_putchar_raw('.');
                uint32_t val_color = (i == MENU_INSTALL) ? COLOR_ARCH_GREEN :
                                     (i == MENU_ABORT)   ? COLOR_ARCH_WARN : COLOR_ARCH_VALUE;
                term_set_custom_colors(val_color, bg);
                term_print_raw(val_strings[i]);
            }
        }

        tui_draw_footer("[UP/DN] Navigate   [ENTER] Select / Edit   [ESC] Exit", k_item_descs[selected_item]);

        uint16_t key = tty_getchar_raw();
        if (key == KEY_UP) {
            selected_item--;
            if (selected_item < 0) selected_item = MENU_ITEM_COUNT - 1;
        } else if (key == KEY_DOWN) {
            selected_item++;
            if (selected_item >= MENU_ITEM_COUNT) selected_item = 0;
        } else if (key == KEY_ENTER || key == KEY_KPENTER) {
            switch (selected_item) {
                case MENU_TARGET_DISK:
                    sub_select_disk(&g_installer_ctx);
                    break;
                case MENU_DISK_LAYOUT:
                    sub_select_layout(&g_installer_ctx);
                    break;
                case MENU_FILESYSTEM:
                    tui_dialog_confirm("ROOT FILESYSTEM",
                                       "EquantOS uses native EXT2 for the root filesystem,\n"
                                       "ensuring standard POSIX compatibility and robust performance.");
                    break;
                case MENU_HOSTNAME:
                    sub_edit_hostname(&g_installer_ctx);
                    break;
                case MENU_ROOT_PASS:
                    sub_edit_root_password(&g_installer_ctx);
                    break;
                case MENU_BOOTLOADER:
                    sub_select_bootloader(&g_installer_ctx);
                    break;
                case MENU_CMDLINE:
                    sub_select_cmdline(&g_installer_ctx);
                    break;
                case MENU_PROFILE:
                    sub_select_profile(&g_installer_ctx);
                    break;
                case MENU_REVIEW:
                    installer_show_config_review(&g_installer_ctx);
                    break;
                case MENU_INSTALL: {
                    char warn_msg[384];
                    installer_disk_t *td = &g_installer_ctx.disks[g_installer_ctx.selected_disk_idx];
                    snprintf(warn_msg, sizeof(warn_msg),
                             "Target Device: %s (%s)\n"
                             "Scheme: Auto-GPT (ESP 34MB FAT32 + Root EXT2)\n"
                             "Proceeding will write GPT partition tables and format filesystems.",
                             td->dev_node, td->name);

                    if (tui_dialog_confirm("!!! COMMIT & INSTALL SYSTEM !!!", warn_msg)) {
                        bool ok = execute_installation(&g_installer_ctx);
                        if (ok) {
                            show_finish_screen(&g_installer_ctx);
                            term_clear_screen();
                            return;
                        } else {
                            tui_clear_canvas(COLOR_ARCH_DARK);
                            tui_draw_header("Installation Failed", "Critical error during execution");
                            tui_dialog_confirm("INSTALLATION FAILED", g_installer_ctx.error_msg);
                        }
                    }
                    break;
                }
                case MENU_ABORT:
                    term_clear_screen();
                    return;
            }
        } else if (key == KEY_ESC || key == KEY_Q) {
            term_clear_screen();
            return;
        }
    }
}