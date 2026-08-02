#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "panic.h"
#include "../../drivers/serial/serial.h"

// Global framebuffer state variables for subsystem access (like Panic)
void *kernel_fb_address = NULL;
uint64_t kernel_fb_pitch = 0;
uint64_t kernel_fb_width = 0;
uint64_t kernel_fb_height = 0;

// Set base revision using the official macro from limine.h
__attribute__((used, section(".requests")))
static volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(3);

// Framebuffer request using official ID from limine.h
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

void _start(void) {
    // Initialize COM1 serial port for kernel logging
    serial_init(COM1);
    serial_puts(COM1, "[KERNEL] EquantOS booting up successfully...\n");

    // Check if Limine supports our requested base revision
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) {
        PANIC("Unsupported Limine base revision!");
    }
    serial_puts(COM1, "[KERNEL] Base revision verified successfully by Limine.\n");

    // Verify if we got a valid framebuffer response
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        PANIC("No graphic framebuffers provided by Limine!");
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    
    // Save parameters globally
    kernel_fb_address = fb->address;
    kernel_fb_pitch = fb->pitch;
    kernel_fb_width = fb->width;
    kernel_fb_height = fb->height;

    serial_puts(COM1, "[KERNEL] Framebuffer acquired successfully. Painting screen...\n");

    // Draw a test color pattern to the screen (Magenta strip)
    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    for (size_t y = 0; y < 200; y++) {
        for (size_t x = 0; x < fb->width; x++) {
            fb_ptr[y * (fb->pitch / 4) + x] = 0x00FF00FF; // Magenta color
        }
    }

    serial_puts(COM1, "[KERNEL] Initialization sequence completed cleanly.\n");

    // --- TEST PANIC HANDLER (Uncomment to test panic screen/serial log) ---
    // PANIC("Testing EquantOS panic subsystem handler!");

    // Main system idle loop
    for (;;) {
        asm volatile ("cli; hlt");
    }
}