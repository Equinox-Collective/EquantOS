// src/kernel/drivers/keyboard/keyboard.c - PS/2 Driver mapped to Input Subsystem
#include "keyboard.h"
#include "../input.h"
#include "../../core/gen/io.h"
#include <stdbool.h>
#include <stdint.h>

// PS/2 Set 1 Scancode to Universal input_event_t Keycode Mapping
static const uint16_t ps2_to_input_table[128] = {
    [0x01] = KEY_ESC,
    [0x02] = KEY_1, [0x03] = KEY_2, [0x04] = KEY_3, [0x05] = KEY_4,
    [0x06] = KEY_5, [0x07] = KEY_6, [0x08] = KEY_7, [0x09] = KEY_8,
    [0x0A] = KEY_9, [0x0B] = KEY_0, [0x0C] = KEY_MINUS, [0x0D] = KEY_EQUAL,
    [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = KEY_Q, [0x11] = KEY_W, [0x12] = KEY_E, [0x13] = KEY_R,
    [0x14] = KEY_T, [0x15] = KEY_Y, [0x16] = KEY_U, [0x17] = KEY_I,
    [0x18] = KEY_O, [0x19] = KEY_P,
    [0x1C] = KEY_ENTER, [0x1D] = KEY_LEFTCTRL,
    [0x1E] = KEY_A, [0x1F] = KEY_S, [0x20] = KEY_D, [0x21] = KEY_F,
    [0x22] = KEY_G, [0x23] = KEY_H, [0x24] = KEY_J, [0x25] = KEY_K,
    [0x26] = KEY_L, [0x2A] = KEY_LEFTSHIFT,
    [0x2C] = KEY_Z, [0x2D] = KEY_X, [0x2E] = KEY_C, [0x2F] = KEY_V,
    [0x30] = KEY_B, [0x31] = KEY_N, [0x32] = KEY_M,
    [0x33] = KEY_COMMA, [0x34] = KEY_DOT, [0x35] = KEY_SLASH,
    [0x39] = KEY_SPACE, [0x3A] = KEY_CAPSLOCK
};

void keyboard_callback(void) {
    if (!(inb(0x64) & 0x01)) return;

    uint8_t scancode = inb(0x60);
    if (scancode == 0xFA || scancode == 0xFE || scancode == 0xFF) return;

    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (code < 128) {
        uint16_t keycode = ps2_to_input_table[code];
        if (keycode != KEY_RESERVED) {
            // Push event into Centralized Input Event Subsystem
            input_push_event(EV_KEY, keycode, is_release ? KEY_RELEASE : KEY_PRESS);
        }
    }
}

void keyboard_init(void) {
    while (inb(0x64) & 0x01) {
        inb(0x60);
    }
}
