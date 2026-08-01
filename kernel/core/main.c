// kernel/core/main.c
#include <stdint.h>
#include <stddef.h>
#include <kernel/limine.h>
// #include "drivers/serial.h" // Раскомментируем, когда добавим Serial

/* Tell Limine to use the base revision we support */
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(1);

/* Request the memory map from the bootloader */
__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

/* Request a framebuffer for future GUI/tty */
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

// We will halt the CPU if something goes terribly wrong early on
static void hcf(void) {
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

// The actual entry point
void _start(void) {
    // Ensure the bootloader actually understands our base revision
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    // TODO: serial_init();
    // TODO: serial_print("[INFO] EquantOS booting up...\n");

    // Let's just check if we got a framebuffer as a test
    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count > 0) {
        // TODO: serial_print("[INFO] Framebuffer initialized!\n");
    }

    // TODO: serial_print("[INFO] Initialization complete. Halting.\n");

    hcf();
}