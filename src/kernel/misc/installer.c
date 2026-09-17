#include "installer.h"
#include "power.h"
#include "../../equterm/term.h"
#include "../drivers/tty/tty.h"
#include "../drivers/input.h"
#include "../drivers/disk/nvme.h"
#include "../drivers/disk/ata.h"
#include "../fs/vfs.h"
#include "../fs/ext2.h"
#include "../fs/fat32.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "stdio.h"
#include "../drivers/serial/serial.h"
#include "../../limine.h"

#define COPY_CHUNK_SIZE 65536

__attribute__((used, section(".requests")))
static volatile struct limine_efi_system_table_request efi_table_request = {
    .id = { 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b, 0x5ceba5163eaaf6d6, 0x0a6981610cf65fcc },
    .revision = 0,
    .response = NULL
};

typedef struct __attribute__((packed)) {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t size_partition_entry;
    uint32_t partition_array_crc32;
} gpt_header_raw_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t partition_name[36];
} gpt_entry_raw_t;

static const uint8_t GUID_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static const uint8_t GUID_WIN_BASIC_DATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

static const uint8_t GUID_WIN_MSR[16] = {
    0x10, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D, 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE
};

static const uint8_t GUID_WIN_RECOVERY[16] = {
    0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D, 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC
};

static const uint8_t GUID_LINUX_ROOT[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

static installer_session_t g_inst;
static int g_screen_cols = 80;
static int g_screen_rows = 25;
static int g_gw = 8;
static int g_gh = 16;
static int g_log_row = 15;

static uint32_t crc32(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return ~crc;
}

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
    }
}

static void tui_gotoxy(int col, int row) {
    term_set_cursor((size_t)col * g_gw, (size_t)row * g_gh);
}

static void tui_clear(uint32_t bg) {
    uint64_t fb_w = term_get_fb_width();
    uint64_t fb_h = term_get_fb_height();
    term_draw_rect(0, 0, (size_t)fb_w, (size_t)fb_h, bg);
}

static void tui_draw_box(int col, int row, int width, int height, const char *title, uint32_t fg, uint32_t bg) {
    size_t x = (size_t)col * g_gw;
    size_t y = (size_t)row * g_gh;
    size_t w = (size_t)width * g_gw;
    size_t h = (size_t)height * g_gh;
    term_draw_rect(x, y, w, h, bg);

    tui_gotoxy(col, row);
    term_set_custom_colors(fg, bg);
    term_print_raw("\xe2\x94\x8c");
    for (int i = 0; i < width - 2; i++) term_print_raw("\xe2\x94\x80");
    term_print_raw("\xe2\x94\x90");

    if (title && *title) {
        int tlen = strlen(title);
        if (tlen + 4 < width) {
            tui_gotoxy(col + (width - tlen - 2) / 2, row);
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
        term_print_raw("\xe2\x94\x82");
        tui_gotoxy(col + width - 1, row + r);
        term_print_raw("\xe2\x94\x82");
    }

    tui_gotoxy(col, row + height - 1);
    term_set_custom_colors(fg, bg);
    term_print_raw("\xe2\x94\x94");
    for (int i = 0; i < width - 2; i++) term_print_raw("\xe2\x94\x80");
    term_print_raw("\xe2\x94\x98");
}

static void tui_header(const char *title) {
    uint64_t fb_w = term_get_fb_width();
    size_t bar_h = (size_t)g_gh * 2;
    term_draw_rect(0, 0, (size_t)fb_w, bar_h, COLOR_ARCH_HEADER_BG);
    term_draw_rect(0, bar_h - 2, (size_t)fb_w, 2, COLOR_ARCH_CYAN);

    tui_gotoxy(2, 0);
    term_set_custom_colors(COLOR_ARCH_CYAN, COLOR_ARCH_HEADER_BG);
    term_print_raw("EQUANT OS");
    term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_HEADER_BG);
    term_print_raw(" // ");
    term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_HEADER_BG);
    term_print_raw(title ? title : "Installer");

    tui_gotoxy(2, 1);
    term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_HEADER_BG);
    term_print_raw(g_inst.is_uefi_mode ? "Firmware: UEFI x86_64" : "Firmware: Legacy BIOS (MBR)");

    const char *mode_tag = g_inst.is_uefi_mode ? "[ UEFI ]" : "[ BIOS ]";
    tui_gotoxy(g_screen_cols - strlen(mode_tag) - 3, 0);
    term_set_custom_colors(COLOR_ARCH_VALUE, COLOR_ARCH_HEADER_BG);
    term_print_raw(mode_tag);
}

static void tui_footer(const char *hints) {
    uint64_t fb_w = term_get_fb_width();
    int row_start = g_screen_rows - 2;
    size_t y = (size_t)row_start * g_gh;
    term_draw_rect(0, y, (size_t)fb_w, (size_t)g_gh * 2, COLOR_ARCH_HEADER_BG);
    term_draw_rect(0, y, (size_t)fb_w, 1, COLOR_ARCH_BORDER_DIM);

    tui_gotoxy(2, row_start);
    term_set_custom_colors(COLOR_ARCH_KEY, COLOR_ARCH_HEADER_BG);
    term_print_raw(hints ? hints : "[UP/DN] Navigate   [ENTER] Select   [ESC] Exit");
}

static void utf16_to_ascii(const void *src, char *dst, size_t max_len) {
    const uint8_t *s = (const uint8_t *)src;
    size_t i = 0;
    while (i + 1 < max_len) {
        uint16_t ch = (uint16_t)s[i * 2] | ((uint16_t)s[i * 2 + 1] << 8);
        if (ch == 0) break;
        dst[i] = (char)(ch & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

static void probe_region_filesystem(block_device_t dev, disk_region_t *reg) {
    uint8_t buf[1024];
    if (dev.read(reg->start_lba, 2, buf) != 0) return;

    if (memcmp(buf + 3, "NTFS    ", 8) == 0) {
        strcpy(reg->fs_label, "NTFS");
        reg->type = REGION_TYPE_WINDOWS_NTFS;
        return;
    }

    if (memcmp(buf + 82, "FAT32   ", 8) == 0 || (buf[11] == 0x00 && buf[12] == 0x02 && buf[16] == 2)) {
        strcpy(reg->fs_label, "FAT32");
        if (reg->type != REGION_TYPE_ESP) {
            reg->type = REGION_TYPE_ESP;
        }
        return;
    }

    uint8_t ext_buf[1024];
    if (dev.read(reg->start_lba + 2, 2, ext_buf) == 0) {
        if (*(uint16_t *)&ext_buf[56] == 0xEF53) {
            strcpy(reg->fs_label, "EXT2");
            reg->type = REGION_TYPE_LINUX_ROOT;
            return;
        }
    }

    strcpy(reg->fs_label, "RAW");
}

static void scan_disk_topology(installer_disk_t *disk) {
    disk->region_count = 0;
    disk->esp_region_idx = -1;
    disk->has_gpt = false;

    uint8_t sec_buf[512];
    if (disk->bdev.read(1, 1, sec_buf) != 0) return;

    gpt_header_raw_t *gpt = (gpt_header_raw_t *)sec_buf;
    if (gpt->signature != 0x5452415020494645ULL) {
        return;
    }

    disk->has_gpt = true;
    uint32_t num_entries = gpt->num_partition_entries;
    uint32_t entry_size = gpt->size_partition_entry;
    uint64_t entries_lba = gpt->partition_entries_lba;
    uint32_t total_bytes = num_entries * entry_size;
    uint32_t sectors_to_read = (total_bytes + 511) / 512;

    uint8_t *entries = (uint8_t *)kmalloc(sectors_to_read * 512);
    if (!entries) return;

    if (disk->bdev.read(entries_lba, sectors_to_read, entries) != 0) {
        kfree(entries);
        return;
    }

    uint64_t cur_lba = gpt->first_usable_lba;
    if (cur_lba < 2048) cur_lba = 2048;

    for (uint32_t i = 0; i < num_entries; i++) {
        gpt_entry_raw_t *e = (gpt_entry_raw_t *)(entries + (i * entry_size));
        bool empty = true;
        for (int b = 0; b < 16; b++) {
            if (e->type_guid[b] != 0) { empty = false; break; }
        }
        if (empty) continue;

        if (e->starting_lba > cur_lba) {
            uint64_t gap = e->starting_lba - cur_lba;
            if (gap >= 65536 && disk->region_count < INSTALLER_MAX_REGIONS) {
                disk_region_t *free_reg = &disk->regions[disk->region_count++];
                free_reg->type = REGION_TYPE_FREE_SPACE;
                free_reg->part_index = -1;
                free_reg->start_lba = cur_lba;
                free_reg->sector_count = gap;
                strcpy(free_reg->name, "[ UNALLOCATED SPACE ]");
                strcpy(free_reg->fs_label, "FREE");
                free_reg->is_writable_target = true;
                free_reg->is_esp = false;
            }
        }

        if (disk->region_count < INSTALLER_MAX_REGIONS) {
            disk_region_t *p_reg = &disk->regions[disk->region_count++];
            p_reg->part_index = i + 1;
            p_reg->start_lba = e->starting_lba;
            p_reg->sector_count = e->ending_lba - e->starting_lba + 1;
            p_reg->is_writable_target = false;
            p_reg->is_esp = false;

            utf16_to_ascii(e->partition_name, p_reg->name, sizeof(p_reg->name));

            if (memcmp(e->type_guid, GUID_ESP, 16) == 0) {
                p_reg->type = REGION_TYPE_ESP;
                p_reg->is_esp = true;
                strcpy(p_reg->fs_label, "FAT32");
                disk->esp_region_idx = disk->region_count - 1;
            } else if (memcmp(e->type_guid, GUID_WIN_BASIC_DATA, 16) == 0) {
                p_reg->type = REGION_TYPE_WINDOWS_NTFS;
                probe_region_filesystem(disk->bdev, p_reg);
            } else if (memcmp(e->type_guid, GUID_WIN_MSR, 16) == 0) {
                p_reg->type = REGION_TYPE_MSR;
                strcpy(p_reg->fs_label, "MSR");
            } else if (memcmp(e->type_guid, GUID_WIN_RECOVERY, 16) == 0) {
                p_reg->type = REGION_TYPE_RECOVERY;
                strcpy(p_reg->fs_label, "WinRE");
            } else if (memcmp(e->type_guid, GUID_LINUX_ROOT, 16) == 0) {
                p_reg->type = REGION_TYPE_LINUX_ROOT;
                probe_region_filesystem(disk->bdev, p_reg);
            } else {
                p_reg->type = REGION_TYPE_UNKNOWN;
                probe_region_filesystem(disk->bdev, p_reg);
            }
        }

        cur_lba = e->ending_lba + 1;
        if (cur_lba % 2048 != 0) {
            cur_lba = (cur_lba + 2047) & ~2047ULL;
        }
    }

    if (cur_lba < gpt->last_usable_lba && disk->region_count < INSTALLER_MAX_REGIONS) {
        uint64_t tail_gap = gpt->last_usable_lba - cur_lba + 1;
        if (tail_gap >= 65536) {
            disk_region_t *tail_reg = &disk->regions[disk->region_count++];
            tail_reg->type = REGION_TYPE_FREE_SPACE;
            tail_reg->part_index = -1;
            tail_reg->start_lba = cur_lba;
            tail_reg->sector_count = tail_gap;
            strcpy(tail_reg->name, "[ UNALLOCATED SPACE ]");
            strcpy(tail_reg->fs_label, "FREE");
            tail_reg->is_writable_target = true;
            tail_reg->is_esp = false;
        }
    }

    kfree(entries);
}

static int installer_ata_read(uint64_t lba, uint32_t count, void *buf) {
    read_sectors_ata_pio((uintptr_t)buf, lba, count);
    return 0;
}

static int installer_ata_write(uint64_t lba, uint32_t count, void *buf) {
    write_sectors_ata_pio((uintptr_t)buf, lba, count);
    return 0;
}

static int probe_hardware_disks(void) {
    g_inst.disk_count = 0;

    if (nvme_init() == NVME_SUCCESS) {
        installer_disk_t *d = &g_inst.disks[g_inst.disk_count++];
        strcpy(d->name, "PCIe NVMe SSD");
        strcpy(d->dev_node, "nvme0n1");
        d->bdev = nvme_get_block_device();
        d->total_sectors = d->bdev.total_sectors ? d->bdev.total_sectors : 131072;
        d->sector_size = d->bdev.sector_size ? d->bdev.sector_size : 512;
        d->is_nvme = true;
        scan_disk_topology(d);
    }

    block_device_t ata = {
        .read = installer_ata_read,
        .write = installer_ata_write,
        .sector_size = 512,
        .total_sectors = 131072
    };
    uint8_t probe_buf[512];
    if (ata.read(0, 1, probe_buf) == 0 && probe_buf[510] == 0x55 && probe_buf[511] == 0xAA) {
        installer_disk_t *d = &g_inst.disks[g_inst.disk_count++];
        strcpy(d->name, "ATA Fixed Disk");
        strcpy(d->dev_node, "sda");
        d->bdev = ata;
        d->total_sectors = 131072;
        d->sector_size = 512;
        d->is_nvme = false;
        scan_disk_topology(d);
    }

    return g_inst.disk_count;
}

static void tui_pad(char *dest, const char *src, int width) {
    int len = 0;
    while (src && src[len] && len < width) {
        dest[len] = src[len];
        len++;
    }
    while (len < width) {
        dest[len++] = ' ';
    }
    dest[len] = '\0';
}

static void draw_disk_visual_bar(installer_disk_t *disk, int col, int row, int width) {
    tui_gotoxy(col, row);
    term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_PANEL);
    term_putchar_raw('[');

    int bar_cols = width - 2;
    uint64_t total = disk->total_sectors ? disk->total_sectors : 1;

    for (int i = 0; i < disk->region_count; i++) {
        disk_region_t *r = &disk->regions[i];
        int w = (int)((r->sector_count * (uint64_t)bar_cols) / total);
        if (w <= 0) w = 1;

        uint32_t c = COLOR_ARCH_MUTED;
        if (r->type == REGION_TYPE_FREE_SPACE) c = COLOR_ARCH_GREEN;
        else if (r->type == REGION_TYPE_ESP) c = COLOR_ARCH_CYAN;
        else if (r->type == REGION_TYPE_WINDOWS_NTFS) c = COLOR_ARCH_WIN_BLUE;
        else if (r->type == REGION_TYPE_RECOVERY) c = COLOR_ARCH_RECOVERY_GRAY;
        else if (r->type == REGION_TYPE_LINUX_ROOT) c = COLOR_ARCH_KEY;

        term_set_custom_colors(c, COLOR_ARCH_PANEL);
        for (int b = 0; b < w; b++) term_print_raw("\xe2\x96\x88");
    }

    term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_PANEL);
    term_putchar_raw(']');
}

static bool confirm_destructive_overwrite(disk_region_t *reg) {
    int box_w = g_screen_cols > 72 ? 70 : (g_screen_cols - 4);
    int box_h = 10;
    int c = (g_screen_cols - box_w) / 2;
    int r = (g_screen_rows - box_h) / 2;

    tui_draw_box(c, r, box_w, box_h, " DANGER: PARTITION OVERWRITE ", COLOR_ARCH_WARN, COLOR_ARCH_PANEL);
    tui_gotoxy(c + 3, r + 2);
    term_set_custom_colors(COLOR_ARCH_WARN, COLOR_ARCH_PANEL);
    term_print_raw("WARNING: YOU ARE ABOUT TO OVERWRITE AN EXISTING PARTITION!");
    tui_gotoxy(c + 3, r + 4);
    term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
    char msg[128];
    snprintf(msg, sizeof(msg), "Partition P%d: %s (%s, %llu MB)",
             reg->part_index, reg->name, reg->fs_label, (reg->sector_count * 512) / (1024 * 1024));
    term_print_raw(msg);
    tui_gotoxy(c + 3, r + 6);
    term_set_custom_colors(COLOR_ARCH_MUTED, COLOR_ARCH_PANEL);
    term_print_raw("[ENTER] Proceed to Verification   [ESC] Abort and Protect Data");

    while (1) {
        uint16_t key = tty_getchar_raw();
        if (key == KEY_ENTER || key == KEY_KPENTER) break;
        if (key == KEY_ESC || key == KEY_Q) return false;
    }

    tui_draw_box(c, r, box_w, box_h, " FINAL CONFIRMATION ", COLOR_ARCH_DANGER, COLOR_ARCH_PANEL);
    tui_gotoxy(c + 3, r + 2);
    term_set_custom_colors(COLOR_ARCH_DANGER, COLOR_ARCH_PANEL);
    term_print_raw("ALL FILES ON THIS PARTITION WILL BE IRREVERSIBLY DESTROYED.");
    tui_gotoxy(c + 3, r + 4);
    term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
    term_print_raw("Type 'ERASE' in capital letters to confirm: ");

    char input[16];
    memset(input, 0, sizeof(input));
    int len = 0;
    bool shift = false;

    while (1) {
        input_event_t ev;
        while (!input_pop_event(&ev)) __asm__ volatile("hlt");
        if (ev.type != EV_KEY) continue;
        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            shift = (ev.value == KEY_PRESS);
            continue;
        }
        if (ev.value != KEY_PRESS) continue;
        if (ev.code == KEY_ESC) return false;
        if (ev.code == KEY_ENTER || ev.code == KEY_KPENTER) break;
        if (ev.code == KEY_BACKSPACE && len > 0) {
            len--;
            input[len] = '\0';
            tui_gotoxy(c + 47 + len, r + 4);
            term_putchar_raw(' ');
            tui_gotoxy(c + 47 + len, r + 4);
            continue;
        }
        char ch = input_code_to_ascii(ev.code, shift);
        if (ch >= 32 && ch <= 126 && len < 8) {
            input[len++] = ch;
            input[len] = '\0';
            tui_gotoxy(c + 46 + len, r + 4);
            term_set_custom_colors(COLOR_ARCH_DANGER, COLOR_ARCH_PANEL);
            term_putchar_raw(ch);
        }
    }

    return (strcmp(input, "ERASE") == 0);
}

static bool deploy_file(vfs_node_t *dst_dir, const char *name, vfs_node_t *src, uint8_t *buf, size_t bsz) {
    if (!dst_dir || !name || !src) return false;
    vfs_node_t *out = vfs_create(dst_dir, name, FS_FILE);
    if (!out) out = vfs_open(name, 0);
    if (!out) return false;

    uint64_t off = 0;
    while (off < src->length) {
        uint64_t chunk = src->length - off;
        if (chunk > bsz) chunk = bsz;
        int64_t r = vfs_read(src, off, chunk, buf);
        if (r <= 0) return false;
        int64_t w = vfs_write(out, off, r, buf);
        if (w != r) return false;
        off += w;
    }
    return true;
}

static bool commit_gpt_slot(installer_disk_t *disk, uint64_t start, uint64_t count, const uint8_t *guid, const char *label) {
    uint8_t sec[512];
    if (disk->bdev.read(1, 1, sec) != 0) return false;
    gpt_header_raw_t *hdr = (gpt_header_raw_t *)sec;

    uint32_t arr_bytes = hdr->num_partition_entries * hdr->size_partition_entry;
    uint32_t arr_secs = (arr_bytes + 511) / 512;
    uint8_t *entries = (uint8_t *)kmalloc(arr_secs * 512);
    if (!entries) return false;

    if (disk->bdev.read(hdr->partition_entries_lba, arr_secs, entries) != 0) {
        kfree(entries);
        return false;
    }

    int free_slot = -1;
    for (uint32_t i = 0; i < hdr->num_partition_entries; i++) {
        gpt_entry_raw_t *e = (gpt_entry_raw_t *)(entries + (i * hdr->size_partition_entry));
        bool empty = true;
        for (int b = 0; b < 16; b++) {
            if (e->type_guid[b] != 0) { empty = false; break; }
        }
        if (empty) { free_slot = (int)i; break; }
    }

    if (free_slot < 0) {
        kfree(entries);
        return false;
    }

    gpt_entry_raw_t *ne = (gpt_entry_raw_t *)(entries + (free_slot * hdr->size_partition_entry));
    memcpy(ne->type_guid, guid, 16);
    memset(ne->unique_guid, 0x55 + free_slot, 16);
    ne->starting_lba = start;
    ne->ending_lba = start + count - 1;
    ne->attributes = 0;

    memset(ne->partition_name, 0, sizeof(ne->partition_name));
    for (int i = 0; i < 35 && label[i] != '\0'; i++) {
        ne->partition_name[i] = (uint16_t)(uint8_t)label[i];
    }

    for (uint32_t s = 0; s < arr_secs; s++) {
        disk->bdev.write(hdr->partition_entries_lba + s, 1, entries + (s * 512));
        disk->bdev.write(hdr->backup_lba - arr_secs + s, 1, entries + (s * 512));
    }

    hdr->partition_array_crc32 = crc32(entries, arr_bytes);
    hdr->header_crc32 = 0;
    hdr->header_crc32 = crc32(hdr, hdr->header_size);
    disk->bdev.write(hdr->current_lba, 1, sec);

    gpt_header_raw_t bhdr;
    memcpy(&bhdr, hdr, sizeof(gpt_header_raw_t));
    bhdr.current_lba = hdr->backup_lba;
    bhdr.backup_lba = hdr->current_lba;
    bhdr.partition_entries_lba = hdr->backup_lba - arr_secs;
    bhdr.header_crc32 = 0;
    bhdr.header_crc32 = crc32(&bhdr, bhdr.header_size);
    disk->bdev.write(hdr->backup_lba, 1, &bhdr);

    kfree(entries);
    return true;
}

static void render_log(const char *msg) {
    tui_gotoxy(4, g_log_row++);
    term_set_custom_colors(COLOR_ARCH_TEXT, COLOR_ARCH_PANEL);
    term_print_raw(":: ");
    term_print_raw(msg);
    if (g_log_row > g_screen_rows - 4) g_log_row = 15;
}

static bool run_installer_engine(void) {
    installer_disk_t *disk = &g_inst.disks[g_inst.selected_disk_idx];
    tui_clear(COLOR_ARCH_PANEL);
    tui_header("Deployment Pipeline");
    g_log_row = 4;

    render_log("Locking target storage device...");

    if (g_inst.action == INSTALL_ACTION_USE_FREE_SPACE) {
        render_log("Allocating new GPT partition entry from unallocated space...");
        if (!commit_gpt_slot(disk, g_inst.target_root_start, g_inst.target_root_sectors,
                             GUID_LINUX_ROOT, "EquantOS Root")) {
            strcpy(g_inst.error_msg, "Failed to write GPT partition table entry.");
            return false;
        }
    }

    render_log("Formatting Root Partition with EXT2...");
    if (mkfs_ext2(disk->bdev, (uint32_t)g_inst.target_root_start,
                  (uint32_t)g_inst.target_root_sectors, "EQUANT_ROOT") != 0) {
        strcpy(g_inst.error_msg, "EXT2 filesystem format failed.");
        return false;
    }

    vfs_node_t *root_vfs = ext2_mount_partition(disk->bdev, (uint32_t)g_inst.target_root_start);
    if (!root_vfs) {
        strcpy(g_inst.error_msg, "Failed to mount target EXT2 root filesystem.");
        return false;
    }

    vfs_node_t *r_etc  = vfs_create(root_vfs, "etc", FS_DIRECTORY);
    vfs_node_t *r_boot = vfs_create(root_vfs, "boot", FS_DIRECTORY);
    vfs_node_t *r_bin  = vfs_create(root_vfs, "bin", FS_DIRECTORY);
    vfs_create(root_vfs, "dev", FS_DIRECTORY);
    vfs_create(root_vfs, "proc", FS_DIRECTORY);
    vfs_create(root_vfs, "sys", FS_DIRECTORY);
    vfs_create(root_vfs, "tmp", FS_DIRECTORY);
    vfs_create(root_vfs, "root", FS_DIRECTORY);

    vfs_node_t *esp_vfs = NULL;
    if (g_inst.is_uefi_mode) {
        if (!g_inst.cfg.reuse_existing_esp) {
            render_log("Formatting new EFI System Partition with FAT32...");
            if (mkfs_fat32(disk->bdev, (uint32_t)g_inst.target_esp_start,
                           (uint32_t)g_inst.target_esp_sectors, "ESP") != 0) {
                strcpy(g_inst.error_msg, "Failed to format EFI System Partition.");
                return false;
            }
            commit_gpt_slot(disk, g_inst.target_esp_start, g_inst.target_esp_sectors,
                            GUID_ESP, "EFI System");
        }

        render_log("Mounting EFI System Partition (FAT32)...");
        esp_vfs = fat32_mount_partition(disk->bdev, (uint32_t)g_inst.target_esp_start,
                                        (uint32_t)g_inst.target_esp_sectors);
        if (!esp_vfs) {
            strcpy(g_inst.error_msg, "Failed to mount EFI System Partition.");
            return false;
        }
    }

    uint8_t *cbuf = (uint8_t *)kmalloc(COPY_CHUNK_SIZE);
    if (!cbuf) {
        strcpy(g_inst.error_msg, "Out of memory allocating transfer buffer.");
        return false;
    }

    render_log("Deploying Kernel and Userspace Payloads...");
    const char *payloads[] = {
        "kernel.elf", "font.psf", "bash.elf", "busybox.elf",
        ".bashrc", "hello.elf", "start.sh"
    };
    int p_count = sizeof(payloads) / sizeof(payloads[0]);

    vfs_node_t *bbox_src = NULL;

    for (int i = 0; i < p_count; i++) {
        char p_path[64];
        snprintf(p_path, sizeof(p_path), "/bin/%s", payloads[i]);
        vfs_node_t *src = vfs_open(p_path, 0);
        if (!src) {
            snprintf(p_path, sizeof(p_path), "/%s", payloads[i]);
            src = vfs_open(p_path, 0);
        }
        if (!src) {
            snprintf(p_path, sizeof(p_path), "/boot/%s", payloads[i]);
            src = vfs_open(p_path, 0);
        }
        if (!src) {
            snprintf(p_path, sizeof(p_path), "/sys/bin/%s", payloads[i]);
            src = vfs_open(p_path, 0);
        }
        if (!src) continue;

        if (strstr(payloads[i], "busybox")) {
            bbox_src = src;
        }

        if (strcmp(payloads[i], "kernel.elf") == 0) {
            deploy_file(r_boot, payloads[i], src, cbuf, COPY_CHUNK_SIZE);
        } else if (strcmp(payloads[i], ".bashrc") == 0) {
            deploy_file(r_etc, ".bashrc", src, cbuf, COPY_CHUNK_SIZE);
            vfs_node_t *r_root_dir = vfs_finddir(root_vfs, "root");
            if (r_root_dir) deploy_file(r_root_dir, ".bashrc", src, cbuf, COPY_CHUNK_SIZE);
            deploy_file(root_vfs, ".bashrc", src, cbuf, COPY_CHUNK_SIZE);
        } else {
            deploy_file(r_bin, payloads[i], src, cbuf, COPY_CHUNK_SIZE);
            if (strcmp(payloads[i], "bash.elf") == 0) {
                deploy_file(r_bin, "bash", src, cbuf, COPY_CHUNK_SIZE);
            }
        }
    }

    if (bbox_src && r_bin) {
        render_log("Creating BusyBox core utility symlinks in /bin...");
        static const char *core_utils[] = {
            "ls", "cat", "cp", "mv", "rm", "mkdir", "rmdir", "touch",
            "clear", "echo", "grep", "uname", "df", "free", "ps", "sh", NULL
        };
        for (int u = 0; core_utils[u] != NULL; u++) {
            deploy_file(r_bin, core_utils[u], bbox_src, cbuf, COPY_CHUNK_SIZE);
        }
    }

    if (g_inst.is_uefi_mode && esp_vfs) {
        render_log("Configuring Limine UEFI Bootloader on ESP...");
        vfs_node_t *efi_dir = vfs_create(esp_vfs, "EFI", FS_DIRECTORY);
        vfs_node_t *boot_dir = efi_dir ? vfs_create(efi_dir, "BOOT", FS_DIRECTORY) : NULL;

        vfs_node_t *src_efi = vfs_open("/BOOTX64.EFI", 0);
        if (!src_efi) src_efi = vfs_open("/cdrom/EFI/BOOT/BOOTX64.EFI", 0);
        if (!src_efi) src_efi = vfs_open("/cdrom/BOOTX64.EFI", 0);
        if (!src_efi) src_efi = vfs_open("/EFI/BOOT/BOOTX64.EFI", 0);
        if (!src_efi) src_efi = vfs_open("/bin/BOOTX64.EFI", 0);

        if (!src_efi || !boot_dir) {
            strcpy(g_inst.error_msg, "CRITICAL: BOOTX64.EFI not found on media!");
            return false;
        }
        deploy_file(boot_dir, "BOOTX64.EFI", src_efi, cbuf, COPY_CHUNK_SIZE);

        vfs_node_t *src_kern = vfs_open("/kernel.elf", 0);
        if (!src_kern) src_kern = vfs_open("/cdrom/boot/kernel.elf", 0);
        if (!src_kern) src_kern = vfs_open("/cdrom/kernel.elf", 0);
        if (!src_kern) src_kern = vfs_open("/boot/kernel.elf", 0);

        if (!src_kern) {
            strcpy(g_inst.error_msg, "CRITICAL: kernel.elf not found on media!");
            return false;
        }

        vfs_node_t *esp_boot_dir = vfs_create(esp_vfs, "boot", FS_DIRECTORY);
        if (esp_boot_dir) {
            deploy_file(esp_boot_dir, "kernel.elf", src_kern, cbuf, COPY_CHUNK_SIZE);
        }
        deploy_file(r_boot, "kernel.elf", src_kern, cbuf, COPY_CHUNK_SIZE);

        char lconf[256];
        snprintf(lconf, sizeof(lconf),
                 "timeout: %d\n\n"
                 "/EquantOS (Installed)\n"
                 "    protocol: limine\n"
                 "    kernel_path: boot():/boot/kernel.elf\n",
                 g_inst.cfg.boot_timeout);

        vfs_node_t *cf = vfs_create(esp_vfs, "limine.conf", FS_FILE);
        if (cf) vfs_write(cf, 0, strlen(lconf), (uint8_t *)lconf);

        const char *nsh = "FS0:\r\n\\EFI\\BOOT\\BOOTX64.EFI\r\n";
        vfs_node_t *nsh_file = vfs_create(esp_vfs, "startup.nsh", FS_FILE);
        if (nsh_file) vfs_write(nsh_file, 0, strlen(nsh), (uint8_t *)nsh);
    }

    if (r_etc) {
        render_log("Writing /etc configuration files...");
        vfs_node_t *hn = vfs_create(r_etc, "hostname", FS_FILE);
        if (hn) vfs_write(hn, 0, strlen(g_inst.cfg.hostname), (uint8_t *)g_inst.cfg.hostname);

        const char *osrel = "NAME=\"EquantOS\"\nID=equantos\nPRETTY_NAME=\"EquantOS Rolling\"\n";
        vfs_node_t *os = vfs_create(r_etc, "os-release", FS_FILE);
        if (os) vfs_write(os, 0, strlen(osrel), (uint8_t *)osrel);
    }

    kfree(cbuf);
    render_log("Syncing buffers... Installation completed successfully!");
    return true;
}

void installer_run(void) {
    tui_update_metrics();

    memset(&g_inst, 0, sizeof(installer_session_t));
    g_inst.is_uefi_mode = (efi_table_request.response != NULL && efi_table_request.response->address != 0);
    strcpy(g_inst.cfg.hostname, "equantos");
    strcpy(g_inst.cfg.root_password, "root");
    g_inst.cfg.boot_timeout = 3;
    g_inst.cfg.root_fs = FS_TARGET_EXT2;
    g_inst.cfg.reuse_existing_esp = true;

    if (probe_hardware_disks() == 0) {
        tui_clear(COLOR_ARCH_DARK);
        tui_draw_box(4, 4, g_screen_cols - 8, 8, " HARDWARE ERROR ", COLOR_ARCH_WARN, COLOR_ARCH_PANEL);
        tui_gotoxy(6, 7);
        term_set_custom_colors(COLOR_ARCH_WARN, COLOR_ARCH_PANEL);
        term_print_raw("No supported block devices (NVMe or ATA) detected!");
        tui_footer("[ESC] Exit");
        while (tty_getchar_raw() != KEY_ESC);
        term_clear_screen();
        return;
    }

    int cur_disk_idx = 0;
    int cur_reg_idx = 0;

    while (1) {
        tui_clear(COLOR_ARCH_DARK);
        tui_header("Storage & Partition Layout");

        installer_disk_t *disk = &g_inst.disks[cur_disk_idx];
        int box_w = g_screen_cols - 4;
        int box_h = g_screen_rows - 5;
        tui_draw_box(2, 3, box_w, box_h, " PHYSICAL DISK TOPOLOGY ", COLOR_ARCH_CYAN, COLOR_ARCH_PANEL);

        tui_gotoxy(4, 5);
        term_set_custom_colors(COLOR_ARCH_VALUE, COLOR_ARCH_PANEL);
        char dinfo[128];
        snprintf(dinfo, sizeof(dinfo), "Disk: /dev/%s (%s, %llu MB) | %s",
                 disk->dev_node, disk->name,
                 (disk->total_sectors * disk->sector_size) / (1024 * 1024),
                 disk->has_gpt ? "GPT Active" : "No GPT / Raw");
        term_print_raw(dinfo);

        draw_disk_visual_bar(disk, 4, 7, box_w - 6);

        tui_gotoxy(4, 9);
        term_set_custom_colors(COLOR_ARCH_KEY, COLOR_ARCH_PANEL);
        term_print_raw("  #    Type         FS      Size       Label / Status");

        tui_gotoxy(4, 10);
        term_set_custom_colors(COLOR_ARCH_BORDER_DIM, COLOR_ARCH_PANEL);
        for (int i = 0; i < box_w - 8; i++) term_putchar_raw('-');

        for (int i = 0; i < disk->region_count && i < 10; i++) {
            disk_region_t *r = &disk->regions[i];
            tui_gotoxy(4, 11 + i);
            bool sel = (i == cur_reg_idx);

            uint32_t fg = sel ? COLOR_ARCH_SEL_FG : COLOR_ARCH_TEXT;
            uint32_t bg = sel ? COLOR_ARCH_SEL_BG : COLOR_ARCH_PANEL;
            if (!sel && r->type == REGION_TYPE_FREE_SPACE) fg = COLOR_ARCH_GREEN;
            if (!sel && r->type == REGION_TYPE_WINDOWS_NTFS) fg = COLOR_ARCH_WIN_BLUE;

            term_set_custom_colors(fg, bg);
            term_print_raw(sel ? "> " : "  ");

            uint64_t mb = (r->sector_count * 512) / (1024 * 1024);
            const char *tname = (r->type == REGION_TYPE_FREE_SPACE) ? "FREE" :
                                (r->type == REGION_TYPE_ESP) ? "ESP" :
                                (r->type == REGION_TYPE_WINDOWS_NTFS) ? "WIN11" :
                                (r->type == REGION_TYPE_RECOVERY) ? "RECOVERY" : "ROOT";

            char col_kind[8], col_type[12], col_fs[8], col_sz[16], col_name[32];
            tui_pad(col_kind, (r->part_index >= 0) ? "PART" : "HOLE", 5);
            tui_pad(col_type, tname, 10);
            tui_pad(col_fs, r->fs_label, 7);

            char sz_buf[16], num_buf[16];
            itoa((int64_t)mb, 10, num_buf);
            strcpy(sz_buf, num_buf);
            strcat(sz_buf, " MB");
            tui_pad(col_sz, sz_buf, 10);

            tui_pad(col_name, r->name, 24);

            char row_str[128];
            strcpy(row_str, col_kind);
            strcat(row_str, col_type);
            strcat(row_str, col_fs);
            strcat(row_str, col_sz);
            strcat(row_str, col_name);

            term_print_raw(row_str);
        }

        disk_region_t *selected = &disk->regions[cur_reg_idx];
        tui_gotoxy(4, box_h);
        if (selected->type == REGION_TYPE_FREE_SPACE) {
            term_set_custom_colors(COLOR_ARCH_GREEN, COLOR_ARCH_PANEL);
            term_print_raw("STATUS: SAFE TO INSTALL (No data loss)");
        } else {
            term_set_custom_colors(COLOR_ARCH_WARN, COLOR_ARCH_PANEL);
            term_print_raw("STATUS: OVERWRITE TARGET (Requires ERASE verification)");
        }

        tui_footer("[UP/DN] Navigate Regions   [ENTER] Select Target   [TAB] Change Disk   [ESC] Quit");

        uint16_t key = tty_getchar_raw();
        if (key == KEY_UP) {
            cur_reg_idx--;
            if (cur_reg_idx < 0) cur_reg_idx = disk->region_count - 1;
        } else if (key == KEY_DOWN) {
            cur_reg_idx++;
            if (cur_reg_idx >= disk->region_count) cur_reg_idx = 0;
        } else if (key == KEY_TAB) {
            cur_disk_idx = (cur_disk_idx + 1) % g_inst.disk_count;
            cur_reg_idx = 0;
        } else if (key == KEY_ESC || key == KEY_Q) {
            term_clear_screen();
            return;
        } else if (key == KEY_ENTER || key == KEY_KPENTER) {
            g_inst.selected_disk_idx = cur_disk_idx;
            g_inst.selected_region_idx = cur_reg_idx;

            if (selected->type == REGION_TYPE_FREE_SPACE) {
                g_inst.action = INSTALL_ACTION_USE_FREE_SPACE;
                g_inst.target_root_start = selected->start_lba;
                g_inst.target_root_sectors = selected->sector_count;
            } else {
                if (!confirm_destructive_overwrite(selected)) {
                    continue;
                }
                g_inst.action = INSTALL_ACTION_OVERWRITE_PARTITION;
                g_inst.target_root_start = selected->start_lba;
                g_inst.target_root_sectors = selected->sector_count;
            }

            if (g_inst.is_uefi_mode) {
                if (disk->esp_region_idx >= 0) {
                    disk_region_t *esp = &disk->regions[disk->esp_region_idx];
                    g_inst.target_esp_start = esp->start_lba;
                    g_inst.target_esp_sectors = esp->sector_count;
                    g_inst.cfg.reuse_existing_esp = true;
                } else {
                    g_inst.target_esp_start = g_inst.target_root_start;
                    g_inst.target_esp_sectors = 81920; // 40 MB (>65525 clusters for FAT32!)
                    g_inst.target_root_start += 81920;
                    g_inst.target_root_sectors -= 81920;
                    g_inst.cfg.reuse_existing_esp = false;
                }
            }

            bool ok = run_installer_engine();
            tui_clear(COLOR_ARCH_DARK);
            tui_header(ok ? "Complete" : "Failed");
            tui_draw_box(4, 4, g_screen_cols - 8, 8, ok ? " SUCCESS " : " ERROR ",
                         ok ? COLOR_ARCH_GREEN : COLOR_ARCH_WARN, COLOR_ARCH_PANEL);
            tui_gotoxy(6, 7);
            term_set_custom_colors(ok ? COLOR_ARCH_GREEN : COLOR_ARCH_WARN, COLOR_ARCH_PANEL);
            term_print_raw(ok ? "EquantOS installed successfully! Press [R] to reboot." : g_inst.error_msg);
            tui_footer("[R] Reboot   [ESC] Exit to Shell");

            while (1) {
                uint16_t k = tty_getchar_raw();
                if (k == KEY_R) system_reboot();
                if (k == KEY_ESC || k == KEY_ENTER || k == KEY_KPENTER) break;
            }
            term_clear_screen();
            return;
        }
    }
}