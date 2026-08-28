#include "tty.h"
#include "../input.h"
#include "../serial/serial.h"
#include "../../proc/sched.h"
#include <stdbool.h>

static bool shift_held = false;
static bool caps_locked = false;

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

    if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
        shift_held = (ev.value == KEY_PRESS || ev.value == KEY_REPEAT);
        return 0;
    }
    if (ev.code == KEY_CAPSLOCK && ev.value == KEY_PRESS) {
        caps_locked = !caps_locked;
        return 0;
    }

    if (ev.value != KEY_PRESS && ev.value != KEY_REPEAT) {
        return 0;
    }

    if (ev.code < 128) {
        bool uppercase = shift_held ^ caps_locked;
        char c = uppercase ? keymap_ascii_upper[ev.code] : keymap_ascii_lower[ev.code];
        return c;
    }
    return 0;
}

// Блокирующее чтение символа (Keyboard + Serial)
char tty_getchar(void) {
    input_event_t ev;

    for (;;) {
        __asm__ volatile("sti");

        // 1. Проверка ввода из Serial терминала (COM1)
        if (serial_received(COM1)) {
            char c = serial_getchar(COM1);
            if (c == '\r') c = '\n';
            tty_putchar(c); // Эхо на экран и в serial
            return c;
        }

        // 2. Проверка событий физической клавиатуры
        if (input_pop_event(&ev)) {
            char c = input_event_to_ascii(ev);
            if (c != 0) {
                if (c == '\r') c = '\n';
                tty_putchar(c); // Мгновенно отображаем символ на мониторе!
                return c;
            }
        }

        __asm__ volatile("pause");
        sched_yield();
    }
}
