// src/kernel/drivers/tty/tty_input.c
#include "tty.h"
#include "../input.h"
#include "../serial/serial.h"
#include "../../proc/sched.h"
#include "../../proc/task.h"
#include <stdbool.h>
#include "stdio.h"

static bool shift_held = false;
static bool ctrl_held = false;
static bool caps_locked = false;

// ANSI escape sequence queue for Arrow Keys & Special Keys
static char escape_seq_buf[8];
static int escape_seq_len = 0;
static int escape_seq_pos = 0;

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

static void queue_escape_seq(const char *seq) {
    escape_seq_len = 0;
    escape_seq_pos = 0;
    while (*seq && escape_seq_len < (int)sizeof(escape_seq_buf)) {
        escape_seq_buf[escape_seq_len++] = *seq++;
    }
}

char input_event_to_ascii(input_event_t ev) {
    if (ev.type != EV_KEY) return 0;

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

    if (ev.code == KEY_UP)    { queue_escape_seq("\033[A"); return 0; }
    if (ev.code == KEY_DOWN)  { queue_escape_seq("\033[B"); return 0; }
    if (ev.code == KEY_RIGHT) { queue_escape_seq("\033[C"); return 0; }
    if (ev.code == KEY_LEFT)  { queue_escape_seq("\033[D"); return 0; }
    if (ev.code == KEY_HOME)  { queue_escape_seq("\033[H"); return 0; }
    if (ev.code == KEY_END)   { queue_escape_seq("\033[F"); return 0; }

    if (ctrl_held && ev.code < 128) {
        char base = keymap_ascii_lower[ev.code];
        if (base >= 'a' && base <= 'z') {
            return (char)(base - 'a' + 1);
        }
    }

    if (ev.code < 128) {
        bool uppercase = shift_held ^ caps_locked;
        return uppercase ? keymap_ascii_upper[ev.code] : keymap_ascii_lower[ev.code];
    }
    return 0;
}

// ---- keymap for KDGKBENT -------------------------------------------------------
// Linux key types (linux/keyboard.h)
#define KT_LATIN  0
#define KT_FN     1
#define KT_SPEC   2
#define KT_PAD    3
#define KT_CUR    6
#define KT_SHIFT  7
#define KT_LETTER 11
#define KBV(type, value) ((uint16_t)(((type) << 8) | (uint8_t)(value)))
#define K_HOLE      KBV(KT_SPEC, 0)
#define K_NOSUCHMAP KBV(KT_SPEC, 127)

uint16_t tty_get_kb_entry(uint8_t table, uint8_t index) {
    if (table > 1) return K_NOSUCHMAP;          // only plain and shift maps exist
    if (index >= 128) return K_HOLE;

    switch (index) {
        case KEY_ESC:        return KBV(KT_LATIN, 27);
        case KEY_BACKSPACE:  return KBV(KT_LATIN, 127);
        case KEY_TAB:        return KBV(KT_LATIN, 9);
        case KEY_ENTER:      return KBV(KT_SPEC, 1);    // K_ENTER
        case KEY_CAPSLOCK:   return KBV(KT_SPEC, 7);    // K_CAPS
        case KEY_NUMLOCK:    return KBV(KT_SPEC, 8);    // K_NUM
        case 70:             return KBV(KT_SPEC, 9);    // Scroll Lock (K_HOLD)
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT: return KBV(KT_SHIFT, 0);   // K_SHIFT
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:  return KBV(KT_SHIFT, 2);   // K_CTRL
        case KEY_LEFTALT:
        case KEY_RIGHTALT:   return KBV(KT_SHIFT, 3);   // K_ALT
        case 87:             return KBV(KT_FN, 10);     // F11
        case KEY_F12:        return KBV(KT_FN, 11);
        case KEY_HOME:       return KBV(KT_FN, 20);     // K_FIND
        case KEY_INSERT:     return KBV(KT_FN, 21);
        case KEY_DELETE:     return KBV(KT_FN, 22);     // K_REMOVE
        case KEY_END:        return KBV(KT_FN, 23);     // K_SELECT
        case KEY_PAGEUP:     return KBV(KT_FN, 24);
        case 109:            return KBV(KT_FN, 25);     // Page Down
        case KEY_DOWN:       return KBV(KT_CUR, 0);
        case KEY_LEFT:       return KBV(KT_CUR, 1);
        case KEY_RIGHT:      return KBV(KT_CUR, 2);
        case KEY_UP:         return KBV(KT_CUR, 3);
        case 82:             return KBV(KT_PAD, 0);     // keypad 0..9
        case 79:             return KBV(KT_PAD, 1);
        case 80:             return KBV(KT_PAD, 2);
        case 81:             return KBV(KT_PAD, 3);
        case 75:             return KBV(KT_PAD, 4);
        case 76:             return KBV(KT_PAD, 5);
        case 77:             return KBV(KT_PAD, 6);
        case KEY_KP7:        return KBV(KT_PAD, 7);
        case 72:             return KBV(KT_PAD, 8);
        case 73:             return KBV(KT_PAD, 9);
        case 78:             return KBV(KT_PAD, 10);    // keypad +
        case 74:             return KBV(KT_PAD, 11);    // keypad -
        case 55:             return KBV(KT_PAD, 12);    // keypad *
        case 98:             return KBV(KT_PAD, 13);    // keypad /
        case KEY_KPENTER:    return KBV(KT_PAD, 14);
        case 83:             return KBV(KT_PAD, 16);    // keypad .
        default: break;
    }
    if (index >= KEY_F1 && index <= KEY_F10) return KBV(KT_FN, index - KEY_F1);

    char c = table ? keymap_ascii_upper[index] : keymap_ascii_lower[index];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return KBV(KT_LETTER, c);
    if (c >= ' ' && c < 127) return KBV(KT_LATIN, c);
    return K_HOLE;
}

// ---- raw keyboard mode ---------------------------------------------------------
static int kb_mode = K_XLATE;
static uint64_t kb_owner = 0;          // pid that asked for raw keys
static uint8_t raw_pending = 0;        // second byte of a two-byte scancode
static bool raw_has_pending = false;

void tty_set_kb_mode(int mode) {
    raw_has_pending = false;
    if (mode == K_RAW || mode == K_MEDIUMRAW) {
        kb_mode = mode;
        kb_owner = (current_task && current_task->process) ? current_task->process->pid : 0;
    } else {
        kb_mode = K_XLATE;
        kb_owner = 0;
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "[TTY] Keyboard mode %d (pid %d)\n", kb_mode,
             (int)((current_task && current_task->process) ? current_task->process->pid : 0));
    serial_puts(COM1, msg);
}

int tty_get_kb_mode(void) {
    return kb_mode;
}

// Is the keyboard held in raw mode by a process that is still alive?
bool tty_kb_grabbed(void) {
    if (kb_mode == K_XLATE) return false;
    process_t *owner = kb_owner ? process_find(kb_owner) : NULL;
    if (!owner || owner->exited) {
        // The owner went away without restoring the mode
        kb_mode = K_XLATE;
        kb_owner = 0;
        raw_has_pending = false;
        return false;
    }
    return true;
}

static bool tty_kb_mine(void) {
    return current_task && current_task->process && current_task->process->pid == kb_owner;
}

// Keys that AT set 1 sends with an 0xE0 prefix; 0 for all others
static uint8_t at_extended_code(uint16_t code) {
    switch (code) {
        case KEY_KPENTER:   return 0x1C;
        case KEY_RIGHTCTRL: return 0x1D;
        case 98:            return 0x35;    // keypad /
        case 100:           return 0x38;    // right Alt
        case KEY_HOME:      return 0x47;
        case KEY_UP:        return 0x48;
        case 104:           return 0x49;    // Page Up
        case KEY_LEFT:      return 0x4B;
        case KEY_RIGHT:     return 0x4D;
        case KEY_END:       return 0x4F;
        case KEY_DOWN:      return 0x50;
        case 109:           return 0x51;    // Page Down
        case 110:           return 0x52;    // Insert
        case KEY_DELETE:    return 0x53;
        case KEY_LEFTMETA:  return 0x5B;
        case 126:           return 0x5C;    // right Super
        case 127:           return 0x5D;    // Menu
        default:            return 0;
    }
}

// Next byte for the raw keyboard owner, or -1
static int tty_raw_next(void) {
    if (raw_has_pending) {
        raw_has_pending = false;
        return raw_pending;
    }

    input_event_t ev;
    while (input_pop_event(&ev)) {
        if (ev.type != EV_KEY || ev.code == 0 || ev.code >= 128) continue;  // not a keyboard key
        uint8_t up = (ev.value == KEY_RELEASE) ? 0x80 : 0x00;

        if (kb_mode == K_MEDIUMRAW) return (int)(ev.code | up);

        uint8_t ext = at_extended_code(ev.code);
        if (ext) {
            raw_pending = ext | up;
            raw_has_pending = true;
            return 0xE0;
        }
        if (ev.code <= KEY_F12) return (int)(ev.code | up);     // set 1 == key code here
    }
    return -1;
}

// Check if any character is immediately available to read without blocking
bool tty_has_char(void) {
    if (tty_kb_grabbed()) {
        if (tty_kb_mine()) return raw_has_pending || input_has_events();
        return serial_received(COM1);
    }
    if (escape_seq_pos < escape_seq_len) return true;
    if (serial_received(COM1)) return true;
    return input_has_events();
}

// Non-blocking character getter: returns -1 immediately if buffer is empty
int tty_getchar_nonblock(void) {
    if (tty_kb_grabbed()) {
        if (tty_kb_mine()) return tty_raw_next();
        // Keys belong to the raw reader; the serial console still works
        if (serial_received(COM1)) {
            char c = serial_getchar(COM1);
            if (c == '\r') c = '\n';
            return (unsigned char)c;
        }
        return -1;
    }

    if (escape_seq_pos < escape_seq_len) {
        return (unsigned char)escape_seq_buf[escape_seq_pos++];
    }

    if (serial_received(COM1)) {
        char c = serial_getchar(COM1);
        if (c == '\r') c = '\n';
        return (unsigned char)c;
    }

    input_event_t ev;
    while (input_pop_event(&ev)) {
        char c = input_event_to_ascii(ev);
        if (escape_seq_pos < escape_seq_len) {
            return (unsigned char)escape_seq_buf[escape_seq_pos++];
        }
        if (c != 0) {
            if (c == '\r') c = '\n';
            return (unsigned char)c;
        }
    }

    return -1; // No character ready
}

// Standard blocking character getter for Bash and shell
char tty_getchar(void) {
    for (;;) {
        int c = tty_getchar_nonblock();
        if (c != -1) {
            return (char)c;
        }
        __asm__ volatile("sti; hlt");
    }
}