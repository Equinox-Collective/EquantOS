// term.c - Professional Terminal with ANSI colors, Visual Cursor & Line Editing
#include "term.h"
#include "../kernel/drivers/serial/serial.h"
#include "../kernel/drivers/keyboard/keyboard.h"
#include "shell.h"
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

#define CMD_BUF_SIZE 256
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len = 0;
static int cursor_pos = 0;

// ANSI escape sequence state machine variables
static bool in_escape = false;
static char esc_buf[16];
static int esc_len = 0;

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
    return get_glyph_height() + 4;
}

static void term_clear_rect(size_t y0, size_t y1) {
    if (!term_fb_address) return;
    for (size_t y = y0; y < y1 && y < term_height; y++) {
        // FIX: Use pitch (in 32-bit words) for row offset
        memset(&term_fb_address[y * term_pitch], 0, term_width * sizeof(uint32_t));
    }
}

// Draw or clear visual cursor block at (cursor_x, cursor_y)
static void term_draw_cursor(bool visible) {
    if (!term_fb_address) return;
    int gw = get_glyph_width();
    int gh = get_glyph_height();
    uint32_t color = visible ? term_fg_color : 0x00000000;

    // Draw solid underline block at the bottom of the character cell
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
    default_fg_color = 0x00FFFFFF;

    term_clear();
    shell_init();
    term_draw_cursor(true);
}

static void term_scroll(void) {
    if (!term_fb_address) return;

    int lh = get_line_height();
    size_t visible_rows = term_height - lh;

    for (size_t y = 0; y < visible_rows; y++) {
        // FIX: Copy each row using actual hardware pitch
        memcpy(&term_fb_address[y * term_pitch],
               &term_fb_address[(y + lh) * term_pitch],
               term_width * sizeof(uint32_t));
    }

    term_clear_rect(visible_rows, term_height);
}

static void term_advance_line(void) {
    term_draw_cursor(false); // Clear old cursor
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

// Parse ANSI escape color codes like \033[31m (Red), \033[32m (Green), \033[0m (Reset)
static void parse_ansi_color(const char *code) {
    if (strcmp(code, "0") == 0 || strcmp(code, "37") == 0) {
        term_fg_color = default_fg_color; // Reset to default white
    } else if (strcmp(code, "31") == 0) {
        term_fg_color = 0x00FF5555; // Light Red
    } else if (strcmp(code, "32") == 0) {
        term_fg_color = 0x0055FF55; // Light Green
    } else if (strcmp(code, "33") == 0) {
        term_fg_color = 0x00FFFF55; // Light Yellow
    } else if (strcmp(code, "34") == 0) {
        term_fg_color = 0x005555FF; // Light Blue
    } else if (strcmp(code, "36") == 0) {
        term_fg_color = 0x0055FFFF; // Cyan
    }
}

void term_putchar(char c) {
    serial_putchar(COM1, c);
    if (!term_fb_address) return;

    // Handle ANSI escape sequences state machine
    if (in_escape) {
        if (c == 'm') { // End of color sequence
            esc_buf[esc_len] = '\0';
            parse_ansi_color(esc_buf);
            in_escape = false;
            esc_len = 0;
        } else if (esc_len < (int)sizeof(esc_buf) - 1) {
            esc_buf[esc_len++] = c;
        }
        return;
    }

    if (c == 27) { // ESC character (ASCII 27 / \033)
        in_escape = true;
        esc_len = 0;
        return;
    }

    if (c == '\n') {
        term_advance_line();
        return;
    }

    int gw = get_glyph_width();
    int gh = get_glyph_height();

    term_draw_cursor(false); // Hide cursor before drawing

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

    term_draw_cursor(true); // Redraw cursor at new position
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

void term_poll_keyboard(void) {
    uint8_t scancode = keyboard_pop();
    if (scancode == 0) return;

    // Handle Ctrl + L shortcut to clear screen
    if (keyboard_ctrl_pressed()) {
        char c = get_ascii_char(scancode);
        if (c == 'l' || c == 'L') {
            term_clear();
            shell_init();
            cmd_len = 0;
            cursor_pos = 0;
            term_draw_cursor(true);
            return;
        }
    }

    if (scancode == KEY_UP) {
        if (history_count > 0 && history_index > 0) {
            history_index--;
            term_draw_cursor(false);
            while (cmd_len > 0) {
                term_putchar('\b');
                cmd_len--;
            }
            strcpy(cmd_buf, history[history_index]);
            cmd_len = strlen(cmd_buf);
            cursor_pos = cmd_len;
            term_print(cmd_buf);
            term_draw_cursor(true);
        }
        return;
    }

    if (scancode == KEY_DOWN) {
        if (history_count > 0 && history_index < history_count - 1) {
            history_index++;
            term_draw_cursor(false);
            while (cmd_len > 0) {
                term_putchar('\b');
                cmd_len--;
            }
            strcpy(cmd_buf, history[history_index]);
            cmd_len = strlen(cmd_buf);
            cursor_pos = cmd_len;
            term_print(cmd_buf);
            term_draw_cursor(true);
        } else if (history_index >= history_count - 1) {
            history_index = history_count;
            term_draw_cursor(false);
            while (cmd_len > 0) {
                term_putchar('\b');
                cmd_len--;
            }
            cmd_buf[0] = '\0';
            cmd_len = 0;
            cursor_pos = 0;
            term_draw_cursor(true);
        }
        return;
    }

    char c = get_ascii_char(scancode);
    if (c == 0) return;

    if (c == '\n') {
        term_draw_cursor(false);
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
        term_draw_cursor(true);
    } else if (c == '\b') {
        // Handle Ctrl + Backspace (delete previous word)
        if (keyboard_ctrl_pressed()) {
            term_draw_cursor(false);
            while (cmd_len > 0 && cursor_pos > 0 && cmd_buf[cursor_pos - 1] == ' ') {
                // Skip trailing spaces
                for (int i = cursor_pos - 1; i < cmd_len - 1; i++) cmd_buf[i] = cmd_buf[i + 1];
                cmd_len--; cursor_pos--;
                term_putchar('\b');
            }
            while (cmd_len > 0 && cursor_pos > 0 && cmd_buf[cursor_pos - 1] != ' ') {
                // Delete word characters
                for (int i = cursor_pos - 1; i < cmd_len - 1; i++) cmd_buf[i] = cmd_buf[i + 1];
                cmd_len--; cursor_pos--;
                term_putchar('\b');
            }
            term_draw_cursor(true);
        } else {
            // Regular Backspace
            if (cmd_len > 0 && cursor_pos > 0) {
                term_draw_cursor(false);
                for (int i = cursor_pos - 1; i < cmd_len - 1; i++) {
                    cmd_buf[i] = cmd_buf[i + 1];
                }
                cmd_len--;
                cursor_pos--;
                term_putchar('\b');
                term_draw_cursor(true);
            }
        }
    } else {
        if (cmd_len < CMD_BUF_SIZE - 1) {
            term_draw_cursor(false);
            for (int i = cmd_len; i > cursor_pos; i--) {
                cmd_buf[i] = cmd_buf[i - 1];
            }
            cmd_buf[cursor_pos] = c;
            cmd_len++;
            cursor_pos++;
            term_putchar(c);
            term_draw_cursor(true);
        }
    }
}