// src/kernel/core/globalkeybinds.c - Kernel-Level Global Hotkeys
#include "globalkeybinds.h"
#include "../misc/power.h"
#include "../drivers/tty/tty.h"

static bool left_ctrl = false;
static bool right_ctrl = false;
static bool left_alt = false;
static bool right_alt = false;
static bool left_shift = false;
static bool right_shift = false;

bool globalkeybinds_process(const input_event_t *ev) {
    if (ev->type != EV_KEY) return false;

    bool pressed = (ev->value == KEY_PRESS);

    // Track Modifiers State
    switch (ev->code) {
        case KEY_LEFTCTRL:  left_ctrl = pressed; break;
        case KEY_RIGHTCTRL: right_ctrl = pressed; break;
        case KEY_LEFTALT:   left_alt = pressed; break;
        case KEY_RIGHTALT:  right_alt = pressed; break;
        case KEY_LEFTSHIFT: left_shift = pressed; break;
        case KEY_RIGHTSHIFT:right_shift = pressed; break;
    }

    bool ctrl_active  = left_ctrl || right_ctrl;
    bool alt_active   = left_alt || right_alt;
    bool shift_active = left_shift || right_shift;

    if (!pressed) return false;

    // 1. CTRL + ALT + DEL -> Instant System Reboot
    if (ctrl_active && alt_active && ev->code == KEY_DELETE) {
        system_reboot();
        return true;
    }

    // 2. SHIFT + ALT + F1..F6 -> Switch Virtual Console (TTY1..TTY6)
    if (shift_active && alt_active) {
        if (ev->code >= KEY_F1 && ev->code <= KEY_F6) {
            int target_tty = ev->code - KEY_F1;
            tty_switch(target_tty);
            return true;
        }
    }

    return false;
}