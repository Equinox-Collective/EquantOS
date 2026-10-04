// src/kernel/misc/timer.c
#include "timer.h"
#include "../core/gen/io.h"
#include "../core/initcall.h"
#include "../drivers/usb/usb_hid.h"
#include "../drivers/input.h"
#include "../proc/sched.h"
#include "../drivers/net/rtl8139.h"
#include "../net/tcp.h"

volatile uint32_t tick = 0;

void timer_callback(void) {
    tick++;
    sched_timer_tick(tick);

    // Poll USB HID on every single tick for smooth cursor movement
    xhci_timer_tick();

    // Network polling can stay interleaved
    if ((tick % 2) == 0) {
        rtl8139_poll();
        tcp_tick_with_iface(tick * 4);
    }
}

void init_timer(uint32_t freq) {
    uint32_t divisor = 1193182 / freq;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void sleep(uint32_t ms) {
    uint32_t start_tick = tick;
    uint32_t target_ticks = (ms * 250) / 1000;
    while (tick < start_tick + target_ticks) {
        __asm__ volatile("pause");
    }
}

static int __init timer_arch_initcall(void) {
    init_timer(250); // 250 Hz provides balanced 4ms scheduling latency
    return 0;
}
arch_initcall(timer_arch_initcall);