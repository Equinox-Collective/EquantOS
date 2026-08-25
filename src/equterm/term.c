// src/equterm/term.c - Pure Display Driver with Mid-Line Redraw & Extended Escapes
#include "term.h"
#include "../kernel/drivers/tty/tty.h"
#include "string.h"
#include "../kernel/drivers/display/font8x8.h"
#include "../kernel/drivers/display/psf2.h"
#include <stdbool.h>

static uint32_t *term_fb_address = NULL;
static uint64_t term_width = 0;
static uint64_t term_height = 0;
static uint64_t term_pitch = 0;

static size_t cursor_x = 0;
static size_t cursor_y = 0;

static uint32_t term_fg_color = 0x00FFFFFF;
static uint32_t default_fg_color = 0x00FFFFFF;

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

size_t term_get_cursor_x(void) { return cursor_x; }
size_t term_get_cursor_y(void) { return cursor_y; }

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
                                             (int)term_width, (int)term_height, 
                                             (int)cursor_x, (int)cursor_y, 
                                             (uint32_t)(unsigned char)*ptr, fg);
            cursor_x += drawn_width;
        } else {
            if ((unsigned char)*ptr < 128) {
                const uint8_t *glyph = font8x8_basic[(unsigned char)*ptr];
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

static void parse_ansi_color(const char *code) {
    if (strcmp(code, "0") == 0 || strcmp(code, "37") == 0) {
        term_fg_color = default_fg_color;
    } else if (strcmp(code, "31") == 0) {
        term_fg_color = 0x00FF5555;
    } else if (strcmp(code, "32") == 0) {
        term_fg_color = 0x0055FF55;
    } else if (strcmp(code, "33") == 0) {
        term_fg_color = 0x00FFFF55;
    } else if (strcmp(code, "34") == 0) {
        term_fg_color = 0x005555FF;
    } else if (strcmp(code, "36") == 0) {
        term_fg_color = 0x0055FFFF;
    }
}

void term_putchar_raw(char c) {
    if (!term_fb_address) return;

    if (in_escape) {
        if (c == 'm') {
            esc_buf[esc_len] = '\0';
            parse_ansi_color(esc_buf);
            in_escape = false;
            esc_len = 0;
        } else if (esc_len < (int)sizeof(esc_buf) - 1) {
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

    // Tab handling
    if (c == '\t') {
        cursor_x += gw * 4;
        if (cursor_x >= term_width) term_advance_line();
        return;
    }

    term_draw_cursor(false);

    if (c == '\b') {
        if (cursor_x >= (size_t)gw) {
            cursor_x -= (size_t)gw;
            for (size_t y = 0; y < (size_t)gh; y++) {
                for (size_t x = 0; x < (size_t)gw; x++) {
                    size_t px = cursor_x + x;
                    size_t py = cursor_y + y;
                    if (px < term_width && py < term_height) {
                        term_fb_address[py * term_pitch + px] = 0x00000000;
                    }
                }
            }
        }
        term_draw_cursor(true);
        return;
    }

    if (cursor_x + gw >= term_width) {
        term_advance_line();
    }

    if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
        int drawn_width = psf2_draw_char(&kernel_psf2_font, term_fb_address, 
                                         (int)term_width, (int)term_height, 
                                         (int)cursor_x, (int)cursor_y, 
                                         (uint32_t)(unsigned char)c, term_fg_color);
        cursor_x += drawn_width;
    } else {
        if ((unsigned char)c < 128) {
            const uint8_t *glyph = font8x8_basic[(unsigned char)c];
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

void term_clear(void) {
    tty_clear();
}

void term_set_color(uint32_t color) {
    tty_set_color(color);
}

uint32_t term_get_color(void) {
    return term_fg_color;
}