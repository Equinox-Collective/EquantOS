#include "timer.h"
#include "../core/gen/io.h"
#include "../core/initcall.h"
#include "../drivers/usb/usb_hid.h"
#include "../drivers/input.h"
#include "../proc/sched.h"
#include "../drivers/net/rtl8139.h"

volatile uint32_t tick = 0;

void timer_callback() {
    tick++;

    sched_timer_tick(tick);

    xhci_timer_tick();
    input_timer_tick();
    rtl8139_poll();
}

void init_timer(uint32_t freq) {
    uint32_t divisor = 1193182 / freq;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void sleep(uint32_t ms) {
    uint32_t start_tick = tick;
    while (tick < start_tick + ms) {
        __asm__ __volatile__("pause");
    }
}

// // THIS SHOULD BELONG TO BOTTOM, DO NOT REWRITE IN ANY CASE // //

static int __init timer_arch_initcall(void) {
    init_timer(100); // Initialize PIT at 100 Hz (10ms quantum ticks)
    return 0;
}
arch_initcall(timer_arch_initcall);