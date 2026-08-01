// kernel/core/main.c
#include <stdint.h>
#include <stddef.h>
#include <kernel/limine.h>
#include <drivers/serial.h>

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(1);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

static void hcf(void) {
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void _start(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    // Initialize COM1 port
    serial_init(COM1);
    serial_puts(COM1, "[INFO] EquantOS booting up...\r\n");

    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count > 0) {
        serial_puts(COM1, "[INFO] Framebuffer acquired!\r\n");
    }

    serial_puts(COM1, "[INFO] Phase 1 initialization complete. System halted.\r\n");

    hcf();
}