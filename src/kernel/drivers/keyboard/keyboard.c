#include "keyboard.h"
#include "../../core/gen/io.h"
#include <stdbool.h>
#include <stdint.h>

static uint8_t key_buffer[128];
static int key_head = 0;
static int key_tail = 0;

static bool shift_pressed = false;
static bool ctrl_pressed  = false;
static bool alt_pressed   = false;
static bool super_pressed = false;
static bool extended      = false;

bool keyboard_super_pressed(void) { return super_pressed; }
bool keyboard_alt_pressed(void)   { return alt_pressed; }
bool keyboard_ctrl_pressed(void)  { return ctrl_pressed; }
bool keyboard_shift_pressed(void) { return shift_pressed; }

static const char ascii_table[] = {
    0,   27,  '1',  '2',  '3',  '4', '5', '6',  '7', '8', '9', '0',
    '-', '=', '\b', '\t', 'q',  'w', 'e', 'r',  't', 'y', 'u', 'i',
    'o', 'p', '[',  ']',  '\n', 0,   'a', 's',  'd', 'f', 'g', 'h',
    'j', 'k', 'l',  ';',  '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm',  ',',  '.',  '/', 0,   '*',  0,   ' '};

static const char ascii_table_shift[] = {
    0,   27,  '!',  '@',  '#',  '$', '%', '^', '&', '*', '(', ')',
    '_', '+', '\b', '\t', 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{',  '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H',
    'J', 'K', 'L',  ':',  '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M',  '<',  '>',  '?', 0,   '*', 0,   ' '};

char get_ascii_char(uint8_t scancode) {
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = true;
        return 0;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = false;
        return 0;
    }
    if (scancode & 0x80) return 0;
    if (scancode >= sizeof(ascii_table)) return 0;
    return shift_pressed ? ascii_table_shift[scancode] : ascii_table[scancode];
}

void keyboard_push(uint8_t scancode) {
    int next = (key_head + 1) % 128;
    if (next != key_tail) {
        key_buffer[key_head] = scancode;
        key_head = next;
    }
}

uint8_t keyboard_pop(void) {
    
    if (key_head == key_tail) return 0;
    uint8_t sc = key_buffer[key_tail];
    key_tail = (key_tail + 1) % 128;
    return sc;
}

void keyboard_callback(void) {
    uint8_t scancode = inb(0x60);


    // 1. Handle extended scancode prefix
    if (scancode == 0xE0) {
        extended = true;
        keyboard_push(0xE0);
        return;
    }

    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;
    bool was_extended = extended;
    extended = false;

    // 2. Update modifier keys state
    if (code == 0x1D) {
        ctrl_pressed = !is_release;
    } else if (code == 0x38) {
        alt_pressed = !is_release;
    } else if (code == 0x2A || code == 0x36) {
        shift_pressed = !is_release;
    } else if (was_extended && (code == 0x5B || code == 0x5C)) {
        super_pressed = !is_release;
    }

    // 3. Push raw scancode into the circular buffer
    keyboard_push(scancode);
}

void keyboard_init(void) {
    // Flush any leftover bytes in the PS/2 output buffer
    while (inb(0x64) & 0x01) {
        inb(0x60);
    }
}