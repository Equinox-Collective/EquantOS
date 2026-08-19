// src/kernel/drivers/input.h - Unified Linux/BSD Style Input Event Subsystem
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

// Event Types
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02 // Mouse movement
#define EV_ABS 0x03 // Touchscreen / Absolute axis

// Key State Values
#define KEY_RELEASE 0
#define KEY_PRESS   1
#define KEY_REPEAT  2

// Universal Key Codes
#define KEY_RESERVED   0
#define KEY_ESC        1
#define KEY_1          2
#define KEY_2          3
#define KEY_3          4
#define KEY_4          5
#define KEY_5          6
#define KEY_6          7
#define KEY_7          8
#define KEY_8          9
#define KEY_9          10
#define KEY_0          11
#define KEY_MINUS      12
#define KEY_EQUAL      13
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_Q          16
#define KEY_W          17
#define KEY_E          18
#define KEY_R          19
#define KEY_T          20
#define KEY_Y          21
#define KEY_U          22
#define KEY_I          23
#define KEY_O          24
#define KEY_P          25
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_A          30
#define KEY_S          31
#define KEY_D          32
#define KEY_F          33
#define KEY_G          34
#define KEY_H          35
#define KEY_J          36
#define KEY_K          37
#define KEY_L          38
#define KEY_LEFTSHIFT  42
#define KEY_Z          44
#define KEY_X          45
#define KEY_C          46
#define KEY_V          47
#define KEY_B          48
#define KEY_N          49
#define KEY_M          50
#define KEY_SPACE      57
#define KEY_CAPSLOCK   58

typedef struct {
    uint16_t type;  // EV_KEY, EV_REL, EV_ABS
    uint16_t code;  // KEY_A, KEY_ENTER, etc.
    int32_t  value; // KEY_PRESS (1), KEY_RELEASE (0)
} input_event_t;

void input_init(void);
void input_push_event(uint16_t type, uint16_t code, int32_t value);
bool input_pop_event(input_event_t *out_event);

#endif // INPUT_H