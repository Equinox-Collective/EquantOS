// src/equterm/term.c - Pure Display Driver with Mid-Line Redraw & Extended Escapes
#include "term.h"
#include "../kernel/drivers/tty/tty.h"
#include "string.h"
#include "../kernel/drivers/display/font8x8.h"
#include "../kernel/drivers/display/psf2.h"
#include "term.h"
#include <stdbool.h>

extern bool devfs_is_gui_active(void);

static uint32_t *term_fb_address = NULL;
static uint64_t term_width = 0;
static uint64_t term_height = 0;
static uint64_t term_pitch = 0;
static uint32_t utf8_cp = 0;
static int utf8_remain = 0;

static size_t cursor_x = 0;
static size_t cursor_y = 0;

static uint32_t term_fg_color = 0x00FFFFFF;
static uint32_t term_bg_color = 0x00000000;
static uint32_t default_fg_color = 0x00FFFFFF;

size_t term_get_cursor_x(void) { return cursor_x; }
size_t term_get_cursor_y(void) { return cursor_y; }

static bool in_escape = false;
static char esc_buf[16];
static int esc_len = 0;

static int get_glyph_width(void) {
    if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
        return (int)kernel_psf2_font.hdr->width;
    }
    return 8;
}

static int get_glyph_height(void) {
    if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
        return (int)kernel_psf2_font.hdr->height;
    }
    return 8;
}

static int get_line_height(void) {
    return get_glyph_height() + 4;
}

static void term_clear_rect(size_t y0, size_t y1) {
    if (!term_fb_address) return;
    for (size_t y = y0; y < y1 && y < term_height; y++) {
        memset(&term_fb_address[y * term_pitch], 0, term_width * sizeof(uint32_t));
    }
}

static void term_draw_cursor(bool visible) {
    if (!term_fb_address) return;
    int gw = get_glyph_width();
    int gh = get_glyph_height();
    uint32_t color = visible ? term_fg_color : 0x00000000;

    for (int y = gh - 3; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            size_t px = cursor_x + x;
            size_t py = cursor_y + y;
            if (px < term_width && py < term_height) {
                term_fb_address[py * term_pitch + px] = color;
            }
        }
    }
}

void term_redraw_input_line(size_t start_x, size_t start_y, const char *line, size_t cursor_idx) {
    term_draw_cursor(false);

    int gw = get_glyph_width();
    int gh = get_glyph_height();

    uint32_t fg = (term_fg_color == 0) ? 0x00FFFFFF : term_fg_color;

    size_t erase_width = (strlen(line) + 20) * gw;
    for (size_t y = 0; y < (size_t)gh; y++) {
        for (size_t x = 0; x < erase_width; x++) {
            size_t px = start_x + x;
            size_t py = start_y + y;
            if (px < term_width && py < term_height) {
                term_fb_address[py * term_pitch + px] = 0x00000000;
            }
        }
    }

    cursor_x = start_x;
    cursor_y = start_y;

    const char *ptr = line;
    while (*ptr) {
        if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
            int drawn_width = psf2_draw_char(&kernel_psf2_font, term_fb_address, 
                                             (int)term_pitch, (int)term_height, 
                                             (int)cursor_x, (int)cursor_y, 
                                             (uint32_t)(unsigned char)*ptr, fg);
            cursor_x += drawn_width;
        } else {
            if ((unsigned char)*ptr < 128) {
                const uint8_t *glyph = (const uint8_t *)font8x8_basic[(unsigned char)*ptr];
                for (size_t y = 0; y < 8; y++) {
                    uint8_t row = glyph[y];
                    for (size_t x = 0; x < 8; x++) {
                        if (row & (1 << x)) {
                            size_t px = cursor_x + x;
                            size_t py = cursor_y + y;
                            if (px < term_width && py < term_height) {
                                term_fb_address[py * term_pitch + px] = fg;
                            }
                        }
                    }
                }
            }
            cursor_x += gw;
        }
        ptr++;
    }

    cursor_x = start_x + (cursor_idx * gw);
    term_draw_cursor(true);
}

void term_clear_screen(void) {
    term_clear_rect(0, term_height);
    cursor_x = 0;
    cursor_y = 0;
}

void term_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch) {
    term_fb_address = (uint32_t *)fb_addr;
    term_width = width;
    term_height = height;
    term_pitch = pitch / 4;
    term_fg_color = 0x00FFFFFF;
    default_fg_color = 0x00FFFFFF;

    term_clear_screen();
    term_draw_cursor(true);
}

static void term_scroll(void) {
    if (!term_fb_address) return;

    int lh = get_line_height();
    size_t visible_rows = term_height - lh;

    for (size_t y = 0; y < visible_rows; y++) {
        memcpy(&term_fb_address[y * term_pitch],
               &term_fb_address[(y + lh) * term_pitch],
               term_width * sizeof(uint32_t));
    }

    term_clear_rect(visible_rows, term_height);
}

static void term_advance_line(void) {
    term_draw_cursor(false);
    cursor_x = 0;
    int gh = get_glyph_height();
    int lh = get_line_height();
    if (cursor_y + lh + gh >= term_height) {
        term_scroll();
        cursor_y = term_height - lh;
    } else {
        cursor_y += lh;
    }
    term_draw_cursor(true);
}

// Foreground keeps the bright look EquantOS always used for 30-37; 90-97 map to the same set
static const uint32_t ansi_fg_palette[8] = {
    0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55, 0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
};
// Background: VGA palette for 40-47, bright variants for 100-107
static const uint32_t ansi_bg_palette[8] = {
    0x00000000, 0x00AA0000, 0x0000AA00, 0x00AA5500, 0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x00AAAAAA,
};
static const uint32_t default_bg_color = 0x00000000;

static void apply_sgr_param(int p) {
    if (p == 0) {
        term_fg_color = default_fg_color;
        term_bg_color = default_bg_color;
    } else if (p == 37 || p == 39) {
        term_fg_color = default_fg_color;
    } else if (p >= 30 && p <= 36) {
        term_fg_color = ansi_fg_palette[p - 30];
    } else if (p >= 90 && p <= 97) {
        term_fg_color = ansi_fg_palette[p - 90];
    } else if (p == 49) {
        term_bg_color = default_bg_color;
    } else if (p >= 40 && p <= 47) {
        term_bg_color = ansi_bg_palette[p - 40];
    } else if (p >= 100 && p <= 107) {
        term_bg_color = ansi_fg_palette[p - 100];
    }
    // Other attributes (bold, blink, underline...) are not rendered
}

// SGR parameters: "[1;36", "[0", or "[" (empty means reset), separated by ';'
static void parse_ansi_color(const char *code) {
    if (!code) return;
    if (code[0] == '[') code++;

    int skip = 0; // remaining arguments of an extended 38/48 color, which are not rendered
    do {
        int p = 0;
        while (*code >= '0' && *code <= '9') {
            p = p * 10 + (*code++ - '0');
        }
        if (skip == -1) {
            skip = (p == 5) ? 1 : (p == 2) ? 3 : 0; // 38;5;N or 38;2;R;G;B
        } else if (skip > 0) {
            skip--;
        } else if (p == 38 || p == 48) {
            skip = -1;
        } else {
            apply_sgr_param(p);
        }
    } while (*code++ == ';');
}

static void term_clear_to_eol(void) {
    if (!term_fb_address) return;
    int gh = get_glyph_height();
    for (size_t y = 0; y < (size_t)gh; y++) {
        size_t py = cursor_y + y;
        if (py >= term_height) break;
        for (size_t px = cursor_x; px < term_width; px++) {
            term_fb_address[py * term_pitch + px] = term_bg_color;
        }
    }
}

static void term_clear_entire_line(void) {
    if (!term_fb_address) return;
    int gh = get_glyph_height();
    for (size_t y = 0; y < (size_t)gh; y++) {
        size_t py = cursor_y + y;
        if (py >= term_height) break;
        for (size_t px = 0; px < term_width; px++) {
            term_fb_address[py * term_pitch + px] = term_bg_color;
        }
    }
}

void term_putchar_raw(char c) {
    if (!term_fb_address) return;
    if (devfs_is_gui_active()) return;

    if (in_escape) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '@' || c == '~') {
            esc_buf[esc_len] = '\0';
            
            // 1. Цвета SGR
            if (c == 'm') {
                parse_ansi_color(esc_buf);
            } 
            // 2. Очистка строки
            else if (c == 'K') {
                if (esc_buf[1] == '2') term_clear_entire_line();
                else term_clear_to_eol();
            } 
            // 3. Очистка экрана
            else if (c == 'J') {
                term_clear_screen();
            }
            // 4. ПОЗИЦИОНИРОВАНИЕ КУРСОРА: \x1b[H или \x1b[row;colH
            else if (c == 'H' || c == 'f') {
                int r = 1, col = 1;
                char *p = esc_buf;
                if (*p == '[') p++;
                if (*p >= '0' && *p <= '9') {
                    r = 0;
                    while (*p >= '0' && *p <= '9') {
                        r = r * 10 + (*p++ - '0');
                    }
                    if (*p == ';') {
                        p++;
                        col = 0;
                        while (*p >= '0' && *p <= '9') {
                            col = col * 10 + (*p++ - '0');
                        }
                    }
                }
                if (r < 1) r = 1;
                if (col < 1) col = 1;

                int gw = get_glyph_width();
                int lh = get_line_height();

                term_draw_cursor(false);
                cursor_x = (size_t)(col - 1) * gw;
                cursor_y = (size_t)(r - 1) * lh;
                if (cursor_x >= term_width) cursor_x = term_width - gw;
                if (cursor_y >= term_height) cursor_y = term_height - lh;
                term_draw_cursor(true);
            }
            // 5. Включение режимов терминала (курсор и альт-экран)
            else if (c == 'h') {
                if (strcmp(esc_buf, "[?25") == 0) {
                    term_draw_cursor(true);
                } else if (strcmp(esc_buf, "[?1049") == 0) {
                    term_clear_screen();
                }
            }
            // 6. Выключение режимов терминала
            else if (c == 'l') {
                if (strcmp(esc_buf, "[?25") == 0) {
                    term_draw_cursor(false);
                } else if (strcmp(esc_buf, "[?1049") == 0) {
                    term_clear_screen();
                }
            }

            in_escape = false;
            esc_len = 0;
            return;
        }
        if (esc_len < (int)sizeof(esc_buf) - 1) {
            esc_buf[esc_len++] = c;
        }
        return;
    }

    if (c == 27) {
        in_escape = true;
        esc_len = 0;
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    if (c == '\n') {
        term_advance_line();
        return;
    }

    int gw = get_glyph_width();
    int gh = get_glyph_height();

    if (c == '\t') {
        cursor_x += gw * 8;
        if (cursor_x >= term_width) term_advance_line();
        return;
    }

    term_draw_cursor(false);

    if (c == '\b') {
        if (cursor_x >= (size_t)gw) {
            cursor_x -= (size_t)gw;
            for (size_t y = 0; y < (size_t)gh; y++) {
                size_t py = cursor_y + y;
                if (py < term_height) {
                    for (size_t x = 0; x < (size_t)gw; x++) {
                        size_t px = cursor_x + x;
                        if (px < term_width) {
                            term_fb_address[py * term_pitch + px] = term_bg_color;
                        }
                    }
                }
            }
        }
        term_draw_cursor(true);
        return;
    }

    uint8_t b = (uint8_t)c;
    uint32_t codepoint;

    if (utf8_remain == 0) {
        if (b < 0x80) {
            codepoint = b;
        } else if ((b & 0xE0) == 0xC0) {
            utf8_cp = b & 0x1F;
            utf8_remain = 1;
            term_draw_cursor(true);
            return;
        } else if ((b & 0xF0) == 0xE0) {
            utf8_cp = b & 0x0F;
            utf8_remain = 2;
            term_draw_cursor(true);
            return;
        } else if ((b & 0xF8) == 0xF0) {
            utf8_cp = b & 0x07;
            utf8_remain = 3;
            term_draw_cursor(true);
            return;
        } else {
            codepoint = b;
        }
    } else {
        if ((b & 0xC0) == 0x80) {
            utf8_cp = (utf8_cp << 6) | (b & 0x3F);
            utf8_remain--;
            if (utf8_remain > 0) {
                term_draw_cursor(true);
                return;
            }
            codepoint = utf8_cp;
        } else {
            utf8_remain = 0;
            codepoint = b;
        }
    }

    if (cursor_x + gw >= term_width) {
        term_advance_line();
    }

    for (size_t y = 0; y < (size_t)gh; y++) {
        size_t py = cursor_y + y;
        if (py >= term_height) break;
        for (size_t x = 0; x < (size_t)gw; x++) {
            size_t px = cursor_x + x;
            if (px >= term_width) break;
            term_fb_address[py * term_pitch + px] = term_bg_color;
        }
    }

    if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
        int drawn_width = psf2_draw_char(&kernel_psf2_font, term_fb_address, 
                                         (int)term_pitch, (int)term_height, 
                                         (int)cursor_x, (int)cursor_y, 
                                         codepoint, term_fg_color);
        cursor_x += drawn_width;
    } else {
        if (codepoint < 128) {
            const uint8_t *glyph = (const uint8_t *)font8x8_basic[codepoint];
            for (size_t y = 0; y < 8; y++) {
                uint8_t row = glyph[y];
                for (size_t x = 0; x < 8; x++) {
                    if (row & (1 << x)) {
                        size_t px = cursor_x + x;
                        size_t py = cursor_y + y;
                        if (px < term_width && py < term_height) {
                            term_fb_address[py * term_pitch + px] = term_fg_color;
                        }
                    }
                }
            }
        }
        cursor_x += gw;
    }

    term_draw_cursor(true);
}

void term_putchar(char c) {
    tty_putchar(c);
}

void term_print(const char *str) {
    tty_print(str);
}

void term_print_raw(const char *str) {
    if (!str) return;
    while (*str) {
        term_putchar_raw(*str++);
    }
}

void term_clear(void) {
    tty_clear();
}

void term_set_color(uint32_t color) {
    tty_set_color(color);
}

uint32_t term_get_color(void) {
    return term_fg_color;
}

void term_set_cursor(size_t x, size_t y) {
    cursor_x = x;
    cursor_y = y;
}

void term_set_custom_colors(uint32_t fg, uint32_t bg) {
    term_fg_color = fg;
    term_bg_color = bg;
}

uint64_t term_get_fb_width(void) {
    return term_width;
}

uint64_t term_get_fb_height(void) {
    return term_height;
}

int term_get_glyph_width(void) {
    return get_glyph_width();
}

int term_get_glyph_height(void) {
    return get_glyph_height();
}

void term_draw_rect(size_t x, size_t y, size_t w, size_t h, uint32_t color) {
    if (!term_fb_address) return;
    for (size_t r = 0; r < h; r++) {
        size_t py = y + r;
        if (py >= term_height) break;
        for (size_t c = 0; c < w; c++) {
            size_t px = x + c;
            if (px >= term_width) break;
            term_fb_address[py * term_pitch + px] = color;
        }
    }
}