// src/kernel/drivers/keyboard/keyboard.c - Clean PS/2 Driver
#include "keyboard.h"
#include "../input.h"
#include "../../core/gen/io.h"
#include "../serial/serial.h"
#include <stdbool.h>
#include <stdint.h>

static bool e0_prefix = false;

static const uint16_t ps2_normal_map[128] = {
    [0x01] = KEY_ESC,        [0x02] = KEY_1,          [0x03] = KEY_2,          [0x04] = KEY_3,
    [0x05] = KEY_4,          [0x06] = KEY_5,          [0x07] = KEY_6,          [0x08] = KEY_7,
    [0x09] = KEY_8,          [0x0A] = KEY_9,          [0x0B] = KEY_0,          [0x0C] = KEY_MINUS,
    [0x0D] = KEY_EQUAL,      [0x0E] = KEY_BACKSPACE,  [0x0F] = KEY_TAB,        [0x10] = KEY_Q,
    [0x11] = KEY_W,          [0x12] = KEY_E,          [0x13] = KEY_R,          [0x14] = KEY_T,
    [0x15] = KEY_Y,          [0x16] = KEY_U,          [0x17] = KEY_I,          [0x18] = KEY_O,
    [0x19] = KEY_P,          [0x1A] = KEY_LEFTBRACE,  [0x1B] = KEY_RIGHTBRACE, [0x1C] = KEY_ENTER,
    [0x1D] = KEY_LEFTCTRL,   [0x1E] = KEY_A,          [0x1F] = KEY_S,          [0x20] = KEY_D,
    [0x21] = KEY_F,          [0x22] = KEY_G,          [0x23] = KEY_H,          [0x24] = KEY_J,
    [0x25] = KEY_K,          [0x26] = KEY_L,          [0x27] = KEY_SEMICOLON,  [0x28] = KEY_APOSTROPHE,
    [0x29] = KEY_GRAVE,      [0x2A] = KEY_LEFTSHIFT,  [0x2B] = KEY_BACKSLASH,  [0x2C] = KEY_Z,
    [0x2D] = KEY_X,          [0x2E] = KEY_C,          [0x2F] = KEY_V,          [0x30] = KEY_B,
    [0x31] = KEY_N,          [0x32] = KEY_M,          [0x33] = KEY_COMMA,      [0x34] = KEY_DOT,
    [0x35] = KEY_SLASH,      [0x36] = KEY_RIGHTSHIFT, [0x37] = KEY_KPASTERISK, [0x38] = KEY_LEFTALT,
    [0x39] = KEY_SPACE,      [0x3A] = KEY_CAPSLOCK,   [0x3B] = KEY_F1,         [0x3C] = KEY_F2,
    [0x3D] = KEY_F3,         [0x3E] = KEY_F4,         [0x3F] = KEY_F5,         [0x40] = KEY_F6,
    [0x41] = KEY_F7,         [0x42] = KEY_F8,         [0x43] = KEY_F9,         [0x44] = KEY_F10,
    [0x45] = KEY_NUMLOCK,    [0x46] = KEY_SCROLLLOCK, [0x47] = KEY_KP7,        [0x48] = KEY_KP8,
    [0x49] = KEY_KP9,        [0x4A] = KEY_KPMINUS,    [0x4B] = KEY_KP4,        [0x4C] = KEY_KP5,
    [0x4D] = KEY_KP6,        [0x4E] = KEY_KPPLUS,     [0x4F] = KEY_KP1,        [0x50] = KEY_KP2,
    [0x51] = KEY_KP3,        [0x52] = KEY_KP0,        [0x53] = KEY_KPDOT,      [0x57] = KEY_F11,
    [0x58] = KEY_F12
};

static const uint16_t ps2_ext_map[128] = {
    [0x1C] = KEY_KPENTER,   [0x1D] = KEY_RIGHTCTRL,  [0x35] = KEY_KPSLASH,    [0x37] = KEY_SYSRQ,
    [0x38] = KEY_RIGHTALT,  [0x47] = KEY_HOME,       [0x48] = KEY_UP,         [0x49] = KEY_PAGEUP,
    [0x4B] = KEY_LEFT,      [0x4D] = KEY_RIGHT,      [0x4F] = KEY_END,        [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGEDOWN,  [0x52] = KEY_INSERT,     [0x53] = KEY_DELETE,     [0x5B] = KEY_LEFTMETA,
    [0x5C] = KEY_RIGHTMETA, [0x5D] = KEY_MENU
};

void keyboard_callback(void) {
    if (!(inb(0x64) & 0x01)) return;

    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }

    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    uint16_t keycode = KEY_RESERVED;

    if (e0_prefix) {
        if (code < 128) keycode = ps2_ext_map[code];
        e0_prefix = false;
    } else {
        if (code < 128) keycode = ps2_normal_map[code];
    }

    if (keycode != KEY_RESERVED) {
        input_push_event(EV_KEY, keycode, is_release ? KEY_RELEASE : KEY_PRESS);
    }
}

void keyboard_init(void) {
    while (inb(0x64) & 0x01) {
        inb(0x60);
    }
    e0_prefix = false;
    serial_puts(COM1, "[PS2-HW] Controller Hardware Initialized.\n");
}