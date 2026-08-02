#include "term.h"
#include "../../drivers/serial/serial.h"
#include <stdint.h>
#include <stddef.h>

static uint32_t *term_fb_address = NULL;
static uint64_t term_width = 0;
static uint64_t term_height = 0;
static uint64_t term_pitch = 0;

static size_t cursor_x = 0;
static size_t cursor_y = 0;

void term_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch) {
    term_fb_address = (uint32_t *)fb_addr;
    term_width = width;
    term_height = height;
    term_pitch = pitch / 4; // Pitch in 32-bit pixels
    cursor_x = 0;
    cursor_y = 0;

    // Clear the entire screen to solid black (wipes out Limine/UEFI VRAM garbage)
    for (size_t y = 0; y < term_height; y++) {
        for (size_t x = 0; x < term_width; x++) {
            term_fb_address[y * term_pitch + x] = 0x00000000;
        }
    }
}

// Minimalist 8x8 bitmap font glyph for fallback character rendering
static const uint8_t font_8x8[128][8] = {
    ['A'] = {0x18, 0x24, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x00},
    ['B'] = {0x3C, 0x42, 0x42, 0x3C, 0x42, 0x42, 0x3C, 0x00},
    ['C'] = {0x3C, 0x42, 0x40, 0x40, 0x40, 0x42, 0x3C, 0x00},
    // (Для остальных символов рендерим простой квадратик или пробел по умолчанию)
};

void term_putchar(char c) {
    // Always mirror character to serial port COM1
    serial_putchar(COM1, c);

    if (!term_fb_address) return;

    if (c == '\n') {
        cursor_x = 0;
        cursor_y += 10; // 8 pixels font height + 2 pixels spacing
        return;
    }

    if (cursor_x + 8 >= term_width) {
        cursor_x = 0;
        cursor_y += 10;
    }

    if (cursor_y + 8 >= term_height) {
        cursor_y = 0; // Simple screen wrap for now
    }

    // Draw character onto Limine framebuffer (White text on black background)
    const uint8_t *glyph = font_8x8[(unsigned char)c];
    for (size_t y = 0; y < 8; y++) {
        uint8_t row = glyph[y];
        for (size_t x = 0; x < 8; x++) {
            if (row & (1 << (7 - x))) {
                size_t px = cursor_x + x;
                size_t py = cursor_y + y;
                term_fb_address[py * term_pitch + px] = 0x00FFFFFF; // White
            }
        }
    }
    cursor_x += 8;
}

void term_print(const char *str) {
    while (*str) {
        term_putchar(*str++);
    }
}