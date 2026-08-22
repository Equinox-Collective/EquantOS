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
#define KEY_0          10
#define KEY_MINUS      11
#define KEY_EQUAL      12
#define KEY_BACKSPACE  13
#define KEY_TAB        14

#define KEY_Q          15
#define KEY_W          16
#define KEY_E          17
#define KEY_R          18
#define KEY_T          19
#define KEY_Y          20
#define KEY_U          21
#define KEY_I          22
#define KEY_O          23
#define KEY_P          24
#define KEY_LEFTBRACE  25
#define KEY_RIGHTBRACE 26
#define KEY_ENTER      27

#define KEY_LEFTCTRL   28
#define KEY_A          29
#define KEY_S          30
#define KEY_D          31
#define KEY_F          32
#define KEY_G          33
#define KEY_H          34
#define KEY_J          35
#define KEY_K          36
#define KEY_L          37
#define KEY_SEMICOLON  38
#define KEY_APOSTROPHE 39
#define KEY_GRAVE      40

#define KEY_LEFTSHIFT  41
#define KEY_BACKSLASH  42
#define KEY_Z          43
#define KEY_X          44
#define KEY_C          45
#define KEY_V          46
#define KEY_B          47
#define KEY_N          48
#define KEY_M          49
#define KEY_COMMA      50
#define KEY_DOT        51
#define KEY_SLASH      52
#define KEY_RIGHTSHIFT 53

#define KEY_KPASTERISK 54
#define KEY_LEFTALT    55
#define KEY_SPACE      56
#define KEY_CAPSLOCK   57

// Function Keys
#define KEY_F1         58
#define KEY_F2         59
#define KEY_F3         60
#define KEY_F4         61
#define KEY_F5         62
#define KEY_F6         63
#define KEY_F7         64
#define KEY_F8         65
#define KEY_F9         66
#define KEY_F10        67
#define KEY_F11        68
#define KEY_F12        69

// Keypad / Numpad
#define KEY_NUMLOCK    70
#define KEY_SCROLLLOCK 71
#define KEY_KP7        72
#define KEY_KP8        73
#define KEY_KP9        74
#define KEY_KPMINUS    75
#define KEY_KP4        76
#define KEY_KP5        77
#define KEY_KP6        78
#define KEY_KPPLUS     79
#define KEY_KP1        80
#define KEY_KP2        81
#define KEY_KP3        82
#define KEY_KP0        83
#define KEY_KPDOT      84

// Extended Navigation Keys (E0 Prefix)
#define KEY_KPENTER    90
#define KEY_RIGHTCTRL  91
#define KEY_KPSLASH    92
#define KEY_SYSRQ      93
#define KEY_RIGHTALT   94
#define KEY_HOME       95
#define KEY_UP         96
#define KEY_PAGEUP     97
#define KEY_LEFT       98
#define KEY_RIGHT      99
#define KEY_END        100
#define KEY_DOWN       101
#define KEY_PAGEDOWN   102
#define KEY_INSERT     103
#define KEY_DELETE     104
#define KEY_LEFTMETA   105 // Left Windows / Super
#define KEY_RIGHTMETA  106 // Right Windows / Super
#define KEY_MENU       107

typedef struct {
    uint16_t type;  // EV_KEY, EV_REL, EV_ABS
    uint16_t code;  // KEY_A, KEY_ENTER, KEY_UP, etc.
    int32_t  value; // KEY_PRESS (1), KEY_RELEASE (0)
} input_event_t;

void input_init(void);
void input_push_event(uint16_t type, uint16_t code, int32_t value);
bool input_pop_event(input_event_t *out_event);

#endif // INPUT_H