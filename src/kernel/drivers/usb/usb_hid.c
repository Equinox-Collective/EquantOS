// src/kernel/drivers/usb/usb_hid.c - Production Layer 3 USB HID Driver
#include "usb_hid.h"
#include "../input.h"
#include <stdbool.h>

static uint8_t prev_keycodes[6] = {0};
static uint8_t prev_modifiers = 0;
static uint8_t prev_mouse_buttons = 0;

static const uint16_t hid_to_system_keymap[256] = {
    [0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D,
    [0x08] = KEY_E, [0x09] = KEY_F, [0x0A] = KEY_G, [0x0B] = KEY_H,
    [0x0C] = KEY_I, [0x0D] = KEY_J, [0x0E] = KEY_K, [0x0F] = KEY_L,
    [0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O, [0x13] = KEY_P,
    [0x14] = KEY_Q, [0x15] = KEY_R, [0x16] = KEY_S, [0x17] = KEY_T,
    [0x18] = KEY_U, [0x19] = KEY_V, [0x1A] = KEY_W, [0x1B] = KEY_X,
    [0x1C] = KEY_Y, [0x1D] = KEY_Z,
    [0x1E] = KEY_1, [0x1F] = KEY_2, [0x20] = KEY_3, [0x21] = KEY_4,
    [0x22] = KEY_5, [0x23] = KEY_6, [0x24] = KEY_7, [0x25] = KEY_8,
    [0x26] = KEY_9, [0x27] = KEY_0,
    [0x28] = KEY_ENTER,     [0x29] = KEY_ESC,       [0x2A] = KEY_BACKSPACE,
    [0x2B] = KEY_TAB,       [0x2C] = KEY_SPACE,     [0x2D] = KEY_MINUS,
    [0x2E] = KEY_EQUAL,     [0x4F] = KEY_RIGHT,     [0x50] = KEY_LEFT,
    [0x51] = KEY_DOWN,      [0x52] = KEY_UP
};

void usb_hid_parse_keyboard_report(const uint8_t *report, size_t len) {
    if (!report || len < 8) return;

    uint8_t modifiers = report[0];
    uint8_t changed_mods = modifiers ^ prev_modifiers;

    if (changed_mods & (1 << 1)) {
        input_push_event(EV_KEY, KEY_LEFTSHIFT, (modifiers & (1 << 1)) ? KEY_PRESS : KEY_RELEASE);
    }
    if (changed_mods & (1 << 0)) {
        input_push_event(EV_KEY, KEY_LEFTCTRL, (modifiers & (1 << 0)) ? KEY_PRESS : KEY_RELEASE);
    }
    if (changed_mods & (1 << 2)) {
        input_push_event(EV_KEY, KEY_LEFTALT, (modifiers & (1 << 2)) ? KEY_PRESS : KEY_RELEASE);
    }
    if (changed_mods & (1 << 3)) {
        input_push_event(EV_KEY, KEY_LEFTMETA, (modifiers & (1 << 3)) ? KEY_PRESS : KEY_RELEASE);
    }
    prev_modifiers = modifiers;

    // Process Released Keys
    for (int i = 0; i < 6; i++) {
        uint8_t old_code = prev_keycodes[i];
        if (old_code == 0) continue;

        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (report[2 + j] == old_code) {
                still_pressed = true;
                break;
            }
        }

        if (!still_pressed) {
            uint16_t sys_key = hid_to_system_keymap[old_code];
            if (sys_key != KEY_RESERVED) {
                input_push_event(EV_KEY, sys_key, KEY_RELEASE);
            }
        }
    }

    // Process Pressed Keys
    for (int i = 0; i < 6; i++) {
        uint8_t new_code = report[2 + i];
        if (new_code == 0) continue;

        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (prev_keycodes[j] == new_code) {
                was_pressed = true;
                break;
            }
        }

        if (!was_pressed) {
            uint16_t sys_key = hid_to_system_keymap[new_code];
            if (sys_key != KEY_RESERVED) {
                input_push_event(EV_KEY, sys_key, KEY_PRESS);
            }
        }

        prev_keycodes[i] = new_code;
    }
}

void usb_hid_parse_mouse_report(const uint8_t *report, size_t len) {
    if (!report || len < 3) return;

    uint8_t buttons = report[0];
    int8_t dx = (int8_t)report[1];
    int8_t dy = (int8_t)report[2];
    int8_t wheel = (len >= 4) ? (int8_t)report[3] : 0;

    uint8_t changed = buttons ^ prev_mouse_buttons;
    if (changed & 0x01) {
        input_push_event(EV_KEY, BTN_LEFT, (buttons & 0x01) ? KEY_PRESS : KEY_RELEASE);
    }
    if (changed & 0x02) {
        input_push_event(EV_KEY, BTN_RIGHT, (buttons & 0x02) ? KEY_PRESS : KEY_RELEASE);
    }
    if (changed & 0x04) {
        input_push_event(EV_KEY, BTN_MIDDLE, (buttons & 0x04) ? KEY_PRESS : KEY_RELEASE);
    }
    prev_mouse_buttons = buttons;

    if (dx != 0) {
        input_push_event(EV_REL, REL_X, dx);
    }
    if (dy != 0) {
        input_push_event(EV_REL, REL_Y, dy);
    }
    if (wheel != 0) {
        input_push_event(EV_REL, REL_WHEEL, wheel);
    }
}

void usb_hid_parse_report(const uint8_t *report, size_t len) {
    if (!report || len == 0) return;

    // Distinguish USB HID Boot Mouse (3 or 4 bytes) vs Boot Keyboard (8 bytes)
    if (len == 3 || len == 4) {
        usb_hid_parse_mouse_report(report, len);
    } else if (len >= 8) {
        if (report[1] == 0) {
            usb_hid_parse_keyboard_report(report, len);
        } else {
            usb_hid_parse_mouse_report(report, len);
        }
    }
}