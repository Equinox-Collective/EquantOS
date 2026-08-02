#include "timer.h"
#include "io.h"

// Volatile is critical to prevent the compiler from optimizing out tick checks in sleep()
volatile uint32_t tick = 0;

void timer_callback(void) {
    tick++;
}

void init_timer(uint32_t freq) {
    // PIT base frequency is 1193182 Hz.
    // Calculate the divisor for the target frequency.
    uint32_t divisor = 1193182 / freq;

    // Send the command byte: Channel 0, access mode lobyte/hibyte, mode 3 (rate generator)
    outb(0x43, 0x36);
    
    // Send the divisor byte-by-byte (low byte first, then high byte)
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void sleep(uint32_t ms) {
    // Fallback active-waiting loop for early kernel stages 
    // (will be upgraded with scheduler yields once process management is implemented)
    uint32_t start_tick = tick;
    while (tick < start_tick + ms) {
        __asm__ __volatile__("pause");
    }
}