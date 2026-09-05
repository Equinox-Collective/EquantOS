// src/kernel/drivers/tty/tty_input.c - Extended input processor with Control keys
#include "tty.h"
#include "../input.h"
#include "../serial/serial.h"
#include "../../proc/sched.h"
#include <stdbool.h>

static bool shift_held = false;
static bool ctrl_held = false;
static bool caps_locked = false;

// Standard US QWERTY lower translation
static const char keymap_ascii_lower[128] = {
    [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5',
    [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0',
    [KEY_MINUS] = '-', [KEY_EQUAL] = '=', [KEY_BACKSPACE] = '\b', [KEY_TAB] = '\t',
    [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't',
    [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p',
    [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']', [KEY_ENTER] = '\n',
    [KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g',
    [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l', [KEY_SEMICOLON] = ';',
    [KEY_APOSTROPHE] = '\'', [KEY_GRAVE] = '`', [KEY_BACKSLASH] = '\\',
    [KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b',
    [KEY_N] = 'n', [KEY_M] = 'm', [KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/',
    [KEY_SPACE] = ' ', [KEY_KPENTER] = '\n'
};

static const char keymap_ascii_upper[128] = {
    [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$', [KEY_5] = '%',
    [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*', [KEY_9] = '(', [KEY_0] = ')',
    [KEY_MINUS] = '_', [KEY_EQUAL] = '+', [KEY_BACKSPACE] = '\b', [KEY_TAB] = '\t',
    [KEY_Q] = 'Q', [KEY_W] = 'W', [KEY_E] = 'E', [KEY_R] = 'R', [KEY_T] = 'T',
    [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I', [KEY_O] = 'O', [KEY_P] = 'P',
    [KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}', [KEY_ENTER] = '\n',
    [KEY_A] = 'A', [KEY_S] = 'S', [KEY_D] = 'D', [KEY_F] = 'F', [KEY_G] = 'G',
    [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L', [KEY_SEMICOLON] = ':',
    [KEY_APOSTROPHE] = '"', [KEY_GRAVE] = '~', [KEY_BACKSLASH] = '|',
    [KEY_Z] = 'Z', [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V', [KEY_B] = 'B',
    [KEY_N] = 'N', [KEY_M] = 'M', [KEY_COMMA] = '<', [KEY_DOT] = '>', [KEY_SLASH] = '?',
    [KEY_SPACE] = ' ', [KEY_KPENTER] = '\n'
};

char input_event_to_ascii(input_event_t ev) {
    if (ev.type != EV_KEY) return 0;

    // Track Modifier Keys
    if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
        shift_held = (ev.value == KEY_PRESS || ev.value == KEY_REPEAT);
        return 0;
    }
    if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
        ctrl_held = (ev.value == KEY_PRESS || ev.value == KEY_REPEAT);
        return 0;
    }
    if (ev.code == KEY_CAPSLOCK && ev.value == KEY_PRESS) {
        caps_locked = !caps_locked;
        return 0;
    }

    if (ev.value != KEY_PRESS && ev.value != KEY_REPEAT) {
        return 0;
    }

    // Handle Control Sequences for Bash (Ctrl+C = 0x03, Ctrl+D = 0x04, etc.)
    if (ctrl_held && ev.code < 128) {
        char base = keymap_ascii_lower[ev.code];
        if (base >= 'a' && base <= 'z') {
            return (char)(base - 'a' + 1); // 1 = Ctrl+A, 3 = Ctrl+C, 4 = Ctrl+D
        }
    }

    if (ev.code < 128) {
        bool uppercase = shift_held ^ caps_locked;
        return uppercase ? keymap_ascii_upper[ev.code] : keymap_ascii_lower[ev.code];
    }
    return 0;
}

char tty_getchar(void) {
    input_event_t ev;

    for (;;) {
        __asm__ volatile("sti");

        // 1. Check Serial Port (COM1)
        if (serial_received(COM1)) {
            char c = serial_getchar(COM1);
            if (c == '\r') c = '\n';
            return c;
        }

        // 2. Check PS/2 and USB Keyboard Queue
        if (input_pop_event(&ev)) {
            char c = input_event_to_ascii(ev);
            if (c != 0) {
                if (c == '\r') c = '\n';
                return c;
            }
        }

        __asm__ volatile("pause");
        sched_yield();
    }
}