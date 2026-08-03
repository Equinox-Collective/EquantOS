#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

void keyboard_init(void);
void keyboard_callback(void);
char get_ascii_char(uint8_t scancode);
void keyboard_push(uint8_t scancode);
uint8_t keyboard_pop(void);

bool keyboard_super_pressed(void);
bool keyboard_alt_pressed(void);
bool keyboard_ctrl_pressed(void);
bool keyboard_shift_pressed(void);

#endif // KEYBOARD_H