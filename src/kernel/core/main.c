#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../drivers/serial/serial.h"

// Limine bootloader protocol structures definitions (v3/vmax standard)
#define LIMINE_BASE_REVISION_REQUEST { { 0xf9562b2d5c95a6c8, 0x6a7b384944536bdc }, 0, NULL }
#define LIMINE_FRAMEBUFFER_REQUEST   { { 0x9d5827dcd881ddf5, 0xa3144504f4ab0111 }, 0, NULL }

struct limine_base_revision {
    uint64_t id[2];
    uint64_t revision;
};

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
    // Extended fields omitted for brevity
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer **framebuffers;
};

struct limine_base_revision_request {
    uint64_t id[2];
    uint64_t revision;
    struct limine_base_revision *response;
};

struct limine_framebuffer_request {
    uint64_t id[2];
    uint64_t revision;
    struct limine_framebuffer_response *response;
};

// Request base revision protocol from Limine
static volatile struct limine_base_revision_request base_revision_request = {
    .id = { 0xf9562b2d5c95a6c8, 0x6a7b384944536bdc },
    .revision = 0
};

// Request graphical framebuffer from Limine
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = { 0x9d5827dcd881ddf5, 0xa3144504f4ab0111 },
    .revision = 0
};

// Helper macro to check base revision status
#define LIMINE_BASE_REVISION_SUPPORTED(req) (base_revision_request.response != NULL && base_revision_request.response->revision == 0)

void _start(void) {
    // Initialize COM1 serial port for critical kernel debugging output
    serial_init(COM1);
    serial_puts(COM1, "[KERNEL] EquantOS booting up successfully...\n");

    // Check if Limine base revision is valid and supported
    if (base_revision_request.response == NULL || base_revision_request.response->revision == 0) {
        serial_puts(COM1, "[KERNEL PANIC] Limine base revision mismatch or unsupported bootloader!\n");
        for (;;) { asm volatile ("cli; hlt"); }
    }

    // Verify framebuffer allocation
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        serial_puts(COM1, "[KERNEL PANIC] No graphic framebuffers provided by Limine!\n");
        for (;;) { asm volatile ("cli; hlt"); }
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    serial_puts(COM1, "[KERNEL] Framebuffer acquired successfully. Starting graphics subsystem...\n");

    // Paint a test pattern on screen (e.g., dark blue gradient or simple fill)
    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    for (size_t y = 0; y < 100; y++) {
        for (size_t x = 0; x < fb->width; x++) {
            fb_ptr[y * (fb->pitch / 4) + x] = 0x00FF00FF; // Magenta color strip
        }
    }

    serial_puts(COM1, "[KERNEL] Initialization completed. Halting execution loop.\n");

    // Main system idle loop
    for (;;) {
        asm volatile ("cli; hlt");
    }
}