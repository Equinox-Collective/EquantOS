#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../drivers/serial/serial.h"

// Limine base revision tag (requested revision set to 3)
__attribute__((used, section(".requests")))
static volatile uint64_t base_revision[3] = { 0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 3 };

// Framebuffer request structure definition
struct limine_framebuffer {
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    uint8_t unused[7];
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer **framebuffers;
};

struct limine_framebuffer_request {
    uint64_t id[2];
    uint64_t revision;
    struct limine_framebuffer_response *response;
};

// Request graphical framebuffer from Limine, placed in .requests section
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = { 0x9d5827dcd881ddf5, 0xa3144504f4ab0111 },
    .revision = 0,
    .response = NULL
};

void _start(void) {
    // Initialize COM1 serial port for kernel logging
    serial_init(COM1);
    serial_puts(COM1, "[KERNEL] EquantOS booting up successfully...\n");

    // Check if Limine successfully validated our requested base revision.
    // If supported, Limine sets the 3rd element (base_revision[2]) to 0.
    if (base_revision[2] != 0) {
        serial_puts(COM1, "[KERNEL PANIC] Limine base revision mismatch or unsupported bootloader!\n");
        for (;;) { asm volatile ("cli; hlt"); }
    }
    serial_puts(COM1, "[KERNEL] Base revision verified successfully by Limine.\n");

    // Verify if we got a valid framebuffer response
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        serial_puts(COM1, "[KERNEL PANIC] No graphic framebuffers provided by Limine!\n");
        for (;;) { asm volatile ("cli; hlt"); }
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    serial_puts(COM1, "[KERNEL] Framebuffer acquired successfully. Painting screen...\n");

    // Draw a test color pattern to the screen (Magenta strip)
    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    for (size_t y = 0; y < 200; y++) {
        for (size_t x = 0; x < fb->width; x++) {
            fb_ptr[y * (fb->pitch / 4) + x] = 0x00FF00FF; // Magenta
        }
    }

    serial_puts(COM1, "[KERNEL] Initialization sequence completed cleanly. Halting.\n");

    // Hang securely
    for (;;) {
        asm volatile ("cli; hlt");
    }
}