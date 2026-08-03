#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "panic.h"
#include "io.h"
#include "serial.h"
#include "term.h"
#include "timer.h"
#include "gdt.h"
#include "idt.h"
#include "stdio.h"

// Limine base revision request (revision 3)
__attribute__((used, section(".requests")))
static volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(3);

// Limine framebuffer request
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

void _start(void) {
    // 1. Initialize serial port for early core logging
    serial_init(COM1);
    serial_puts(COM1, "[KERNEL] EquantOS kernel execution started.\n");

    // 2. Initialize CPU architecture tables (GDT & IDT)
    init_gdt();
    serial_puts(COM1, "GDT INTED.\n");
    init_idt();
    serial_puts(COM1, "IDT INTED.\n");

    serial_puts(COM1, "[KERNEL] GDT and IDT initialized successfully.\n");

    // 3. Initialize Programmable Interval Timer (PIT) at 100 Hz
    init_timer(100);
    serial_puts(COM1, "[KERNEL] PIT Timer initialized at 100 Hz.\n");

    // 4. Verify Limine base revision compliance
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) {
        PANIC("Unsupported Limine base revision!");
    }

    // 5. Verify graphical framebuffer availability
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        PANIC("No graphic framebuffers provided by Limine!");
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    // 6. Initialize graphical terminal over Limine framebuffer
    term_init(fb->address, fb->width, fb->height, fb->pitch);
    serial_puts(COM1, "[KERNEL] Graphical terminal initialized.\n");

    // 7. Test syslibc printf output on both screen and serial
    printf("booted\n");
    printf("Framebuffer  : %ux%u @ %u bpp\n", (unsigned int)fb->width, (unsigned int)fb->height, fb->bpp);

    term_init(fb->address, fb->width, fb->height, fb->pitch);

    keyboard_init();
    serial_puts(COM1, "[KERNEL] Keyboard initialized.\n");
    
    // Enable global CPU interrupts safely
    asm volatile ("sti");

    // Print welcome prompt
    term_print("Welcome\n");
    term_print("Type 'eqfetch' or 'uptime'.\n\n");
    term_print("EquantOS> ");

    // 9. Main system idle loop
    for (;;) {
        asm volatile ("hlt");
    }
}