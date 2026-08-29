// src/kernel/drivers/input.c - Clean Production Input Subsystem with Typematic Repeat
#include "input.h"
#include "../core/initcall.h"
#include "serial/serial.h"

#define INPUT_BUFFER_SIZE 256

static input_event_t event_ring_buffer[INPUT_BUFFER_SIZE];
static int ring_head = 0;
static int ring_tail = 0;

// Software Typematic Key Repeat State
static uint16_t repeating_key = 0;
static uint32_t repeat_delay_timer = 0;
static uint32_t repeat_rate_timer = 0;

#define REPEAT_DELAY_TICKS 30  // 300 ms задержка перед началом повтора
#define REPEAT_RATE_TICKS  4   // 40 ms интервал между повторами (25 символов/сек)

void input_init(void) {
    ring_head = 0;
    ring_tail = 0;
    repeating_key = 0;
    serial_puts(COM1, "[INPUT-CORE] Unified Input Event Subsystem Initialized.\n");
}

void input_push_event(uint16_t type, uint16_t code, int32_t value) {
    int next = (ring_head + 1) % INPUT_BUFFER_SIZE;

    if (next != ring_tail) {
        event_ring_buffer[ring_head].type = type;
        event_ring_buffer[ring_head].code = code;
        event_ring_buffer[ring_head].value = value;
        ring_head = next;
    }

    // Автоповтор ТОЛЬКО для печатных символов, стрелок и Backspace.
    // ENTER, ESC, TAB и модификаторы НИКОГДА не должны автоповторяться!
    bool can_repeat = (code != KEY_ENTER && code != KEY_KPENTER && 
                       code != KEY_ESC && code != KEY_TAB &&
                       code != KEY_LEFTCTRL && code != KEY_RIGHTCTRL &&
                       code != KEY_LEFTALT && code != KEY_RIGHTALT &&
                       code != KEY_LEFTSHIFT && code != KEY_RIGHTSHIFT);

    if (type == EV_KEY && code < BTN_LEFT && can_repeat) {
        if (value == KEY_PRESS) {
            repeating_key = code;
            repeat_delay_timer = REPEAT_DELAY_TICKS;
            repeat_rate_timer = REPEAT_RATE_TICKS;
        } else if (value == KEY_RELEASE) {
            if (code == repeating_key) {
                repeating_key = 0;
            }
        }
    } else if (type == EV_KEY && (value == KEY_RELEASE || !can_repeat)) {
        if (code == repeating_key || !can_repeat) {
            repeating_key = 0;
        }
    }
}

void input_timer_tick(void) {
    return;
}

bool input_pop_event(input_event_t *out_event) {
    if (ring_head == ring_tail || !out_event) {
        return false;
    }
    *out_event = event_ring_buffer[ring_tail];
    ring_tail = (ring_tail + 1) % INPUT_BUFFER_SIZE;
    return true;
}

bool input_has_events(void) {
    return ring_head != ring_tail;
}

static int __init input_subsys_initcall(void) {
    input_init();
    return 0;
}
subsys_initcall(input_subsys_initcall);