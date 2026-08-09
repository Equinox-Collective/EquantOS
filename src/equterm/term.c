#include "term.h"
#include "../kernel/drivers/serial/serial.h"
#include "../kernel/drivers/keyboard/keyboard.h"
#include "shell.h"
#include "string.h"
#include "../kernel/drivers/display/font8x8.h"
#include "../kernel/drivers/display/psf2.h"

static uint32_t *term_fb_address = NULL;
static uint64_t term_width = 0;
static uint64_t term_height = 0;
static uint64_t term_pitch = 0;

static size_t cursor_x = 0;
static size_t cursor_y = 0;

static uint32_t term_fg_color = 0x00FFFFFF;

#define CMD_BUF_SIZE 256
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len = 0;

// Dynamic glyph dimensions based on loaded PSF2 font or fallback to 8x8
static int get_glyph_width(void) {
    if (kernel_psf2_font.loaded) {
        return (int)kernel_psf2_font.hdr->width;
    }
    return 8;
}

static int get_glyph_height(void) {
    if (kernel_psf2_font.loaded) {
        return (int)kernel_psf2_font.hdr->height;
    }
    return 8;
}

static int get_line_height(void) {
    return get_glyph_height() + 4; // Glyph height + vertical spacing
}

static void term_clear_rect(size_t y0, size_t y1) {
    if (!term_fb_address) return;
    size_t row_bytes = term_width * sizeof(uint32_t);
    for (size_t y = y0; y < y1 && y < term_height; y++) {
        memset(&term_fb_address[y * term_pitch], 0, row_bytes);
    }
}

void term_clear(void) {
    term_clear_rect(0, term_height);
    cursor_x = 0;
    cursor_y = 0;
}

void term_set_color(uint32_t color) {
    term_fg_color = color;
}

uint32_t term_get_color(void) {
    return term_fg_color;
}

void term_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch) {
    term_fb_address = (uint32_t *)fb_addr;
    term_width = width;
    term_height = height;
    term_pitch = pitch / 4;
    term_fg_color = 0x00FFFFFF;

    term_clear();
    shell_init(); 
}

static void term_scroll(void) {
    if (!term_fb_address) return;

    int lh = get_line_height();
    size_t visible_rows = term_height - lh;
    size_t row_bytes = term_width * sizeof(uint32_t);

    for (size_t y = 0; y < visible_rows; y++) {
        memcpy(&term_fb_address[y * term_pitch],
               &term_fb_address[(y + lh) * term_pitch],
               row_bytes);
    }

    term_clear_rect(visible_rows, term_height);
}

static void term_advance_line(void) {
    cursor_x = 0;
    int gh = get_glyph_height();
    int lh = get_line_height();
    if (cursor_y + lh + gh >= term_height) {
        term_scroll();
        cursor_y = term_height - lh;
    } else {
        cursor_y += lh;
    }
}

void term_putchar(char c) {
    serial_putchar(COM1, c);
    if (!term_fb_address) return;

    if (c == '\n') {
        term_advance_line();
        return;
    }

    int gw = get_glyph_width();
    int gh = get_glyph_height();

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
        return;
    }

    if (cursor_x + gw >= term_width) {
        term_advance_line();
    }

    // Render character using PSF2 font if loaded, otherwise fallback to 8x8 basic font
    if (kernel_psf2_font.loaded) {
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
}

void term_print(const char *str) {
    while (*str) {
        term_putchar(*str++);
    }
}

// Command History & Line Editor buffers
#define MAX_HISTORY 16
static char history[MAX_HISTORY][CMD_BUF_SIZE];
static int history_count = 0;
static int history_index = 0;
static int cursor_pos = 0; 

void term_poll_keyboard(void) {
    uint8_t scancode = keyboard_pop();
    if (scancode == 0) return;

    if (scancode == KEY_UP) {
        if (history_count > 0 && history_index > 0) {
            history_index--;
            while (cmd_len > 0) {
                term_putchar('\b');
                cmd_len--;
            }
            strcpy(cmd_buf, history[history_index]);
            cmd_len = strlen(cmd_buf);
            cursor_pos = cmd_len;
            term_print(cmd_buf);
        }
        return;
    }

    if (scancode == KEY_DOWN) {
        if (history_count > 0 && history_index < history_count - 1) {
            history_index++;
            while (cmd_len > 0) {
                term_putchar('\b');
                cmd_len--;
            }
            strcpy(cmd_buf, history[history_index]);
            cmd_len = strlen(cmd_buf);
            cursor_pos = cmd_len;
            term_print(cmd_buf);
        } else if (history_index >= history_count - 1) {
            history_index = history_count;
            while (cmd_len > 0) {
                term_putchar('\b');
                cmd_len--;
            }
            cmd_buf[0] = '\0';
            cmd_len = 0;
            cursor_pos = 0;
        }
        return;
    }

    char c = get_ascii_char(scancode);
    if (c == 0) return;

    if (c == '\n') {
        term_putchar('\n');
        cmd_buf[cmd_len] = '\0';
        
        if (cmd_len > 0) {
            if (history_count < MAX_HISTORY) {
                strcpy(history[history_count++], cmd_buf);
            } else {
                for (int i = 0; i < MAX_HISTORY - 1; i++) {
                    strcpy(history[i], history[i + 1]);
                }
                strcpy(history[MAX_HISTORY - 1], cmd_buf);
            }
            history_index = history_count;
        }

        shell_execute(cmd_buf);
        cmd_len = 0;
        cursor_pos = 0;
    } else if (c == '\b') {
        if (cmd_len > 0 && cursor_pos > 0) {
            for (int i = cursor_pos - 1; i < cmd_len - 1; i++) {
                cmd_buf[i] = cmd_buf[i + 1];
            }
            cmd_len--;
            cursor_pos--;
            term_putchar('\b');
        }
    } else {
        if (cmd_len < CMD_BUF_SIZE - 1) {
            for (int i = cmd_len; i > cursor_pos; i--) {
                cmd_buf[i] = cmd_buf[i - 1];
            }
            cmd_buf[cursor_pos] = c;
            cmd_len++;
            cursor_pos++;
            term_putchar(c);
        }
    }
}