// src/kernel/drivers/input.c - Fully Documented Input Ring Buffer
#include "input.h"
#include "../core/initcall.h"
#include "serial/serial.h"
#include "stdio.h"

#define INPUT_BUFFER_SIZE 256

static input_event_t event_ring_buffer[INPUT_BUFFER_SIZE];
static int ring_head = 0;
static int ring_tail = 0;

void input_init(void) {
    ring_head = 0;
    ring_tail = 0;
    serial_puts(COM1, "[INPUT-CORE] Unified Input Event Subsystem Initialized.\n");
}

void input_push_event(uint16_t type, uint16_t code, int32_t value) {
    int next = (ring_head + 1) % INPUT_BUFFER_SIZE;
    
    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), 
             "[INPUT-PUSH] Type: %u | Code: %u | Value: %d | Head: %d -> %d | Tail: %d\n",
             (unsigned int)type, (unsigned int)code, (int)value, ring_head, next, ring_tail);
    serial_puts(COM1, log_buf);

    if (next != ring_tail) {
        event_ring_buffer[ring_head].type = type;
        event_ring_buffer[ring_head].code = code;
        event_ring_buffer[ring_head].value = value;
        ring_head = next;
    } else {
        serial_puts(COM1, "[INPUT-WARN] Input Event Ring Buffer Full! Dropping event.\n");
    }
}

bool input_pop_event(input_event_t *out_event) {
    if (ring_head == ring_tail || !out_event) {
        return false;
    }
    *out_event = event_ring_buffer[ring_tail];
    ring_tail = (ring_tail + 1) % INPUT_BUFFER_SIZE;

    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), 
             "[INPUT-POP] Popped Code: %u | Value: %d | New Tail: %d\n",
             (unsigned int)out_event->code, (int)out_event->value, ring_tail);
    serial_puts(COM1, log_buf);

    return true;
}

static int __init input_subsys_initcall(void) {
    input_init();
    return 0;
}
subsys_initcall(input_subsys_initcall);