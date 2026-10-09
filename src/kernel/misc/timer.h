#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/**
 * @brief Initialize the Programmable Interval Timer (PIT) at a given frequency.
 * @param freq Frequency in Hertz (e.g., 100 Hz for 10ms ticks).
 */
void init_timer(uint32_t freq);

/**
 * @brief Simple busy-wait sleep function based on PIT ticks.
 * @param ms Milliseconds to sleep.
 */
void sleep(uint32_t ms);

// Scheduler tick rate (LAPIC timer, see apic_init)
#define TIMER_HZ 250

// Global tick counter incremented on every timer interrupt.
// Marked volatile to prevent compiler optimization during sleep loops.
extern volatile uint32_t tick; 

#endif // TIMER_H