#include "term.h"
#include "serial.h"
#include "keyboard.h"
#include "shell.h"
#include "string.h"
#include "../drivers/display/font8x8.h"

static uint32_t *term_fb_address = NULL;
static uint64_t term_width = 0;
static uint64_t term_height = 0;
static uint64_t term_pitch = 0;

static size_t cursor_x = 0;
static size_t cursor_y = 0;

static uint32_t term_fg_color = 0x00FFFFFF;

#define GLYPH_W 8
#define GLYPH_H 8
#define LINE_H  12 // 8px glyph + 4px spacing

#define CMD_BUF_SIZE 256
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len = 0;

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
    shell_init(); // prints the first prompt
}

// Shifts the whole framebuffer up by one text line instead of wiping
// the entire screen every time the cursor reaches the bottom.
static void term_scroll(void) {
    if (!term_fb_address) return;

    size_t visible_rows = term_height - LINE_H;
    size_t row_bytes = term_width * sizeof(uint32_t);

    for (size_t y = 0; y < visible_rows; y++) {
        memcpy(&term_fb_address[y * term_pitch],
               &term_fb_address[(y + LINE_H) * term_pitch],
               row_bytes);
    }

    term_clear_rect(visible_rows, term_height);
}

// Moves the cursor to the start of the next line, scrolling if the
// screen is full. Used both for '\n' and for automatic line wrap.
static void term_advance_line(void) {
    cursor_x = 0;
    if (cursor_y + LINE_H + GLYPH_H >= term_height) {
        term_scroll();
        cursor_y = term_height - LINE_H;
    } else {
        cursor_y += LINE_H;
    }
}

void term_putchar(char c) {
    // Dual output: Serial port + Graphical Screen
    serial_putchar(COM1, c);

    if (!term_fb_address) return;

    if (c == '\n') {
        term_advance_line();
        return;
    }

    if (c == '\b') {
        if (cursor_x >= GLYPH_W) {
            cursor_x -= GLYPH_W;
            for (size_t y = 0; y < GLYPH_H; y++) {
                for (size_t x = 0; x < GLYPH_W; x++) {
                    term_fb_address[(cursor_y + y) * term_pitch + (cursor_x + x)] = 0x00000000;
                }
            }
        }
        // Note: only erases within the current visual line. Fine for
        // now since command input can't wrap past the buffer before
        // Enter is pressed with the current shell.
        return;
    }

    if (cursor_x + GLYPH_W >= term_width) {
        term_advance_line();
    }

    if ((unsigned char)c < 128) {
        const uint8_t *glyph = font8x8_basic[(unsigned char)c];
        for (size_t y = 0; y < GLYPH_H; y++) {
            uint8_t row = glyph[y];
            for (size_t x = 0; x < GLYPH_W; x++) {
                if (row & (1 << x)) {
                    size_t px = cursor_x + x;
                    size_t py = cursor_y + y;
                    term_fb_address[py * term_pitch + px] = term_fg_color;
                }
            }
        }
    }
    cursor_x += GLYPH_W;
}

void term_print(const char *str) {
    while (*str) {
        term_putchar(*str++);
    }
}

void term_poll_keyboard(void) {
    uint8_t scancode = keyboard_pop();
    if (scancode == 0) return;

    char c = get_ascii_char(scancode);
    if (c == 0) return;

    if (c == '\n') {
        term_putchar('\n');
        cmd_buf[cmd_len] = '\0';
        shell_execute(cmd_buf);
        cmd_len = 0;
    } else if (c == '\b') {
        if (cmd_len > 0) {
            cmd_len--;
            term_putchar('\b');
        }
    } else {
        if (cmd_len < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_len++] = c;
            term_putchar(c);
        }
    }
}