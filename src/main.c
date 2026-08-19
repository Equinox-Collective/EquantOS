// src/main.c - Ultra-Clean Kernel Entry Point
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "kernel/core/panic.h"
#include "kernel/drivers/serial/serial.h"
#include "equterm/term.h"
#include "kernel/core/gen/gdt.h"
#include "kernel/core/gen/idt.h"
#include "kernel/core/gen/cpu.h"
#include "kernel/core/mem/pmm.h"
#include "kernel/core/mem/vmm.h"
#include "kernel/core/mem/memory.h"
#include "kernel/core/initcall.h"

__attribute__((used, section(".requests")))
volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

uint64_t hhdm_offset = 0;

__attribute__((used, section(".requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_module_request module_request = {
    .id = { 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b, 0x3e7e279702be32af, 0xca1c4f3bd1280cee },
    .revision = 0,
    .response = NULL
};

void _start(void) {
    // 1. Core Hardware Bootstrap
    serial_init(COM1);
    enable_fpu_sse();
    init_gdt();
    init_idt();

    if (hhdm_request.response == NULL || hhdm_request.response->offset == 0) {
        PANIC("Limine HHDM response is NULL!");
    }
    hhdm_offset = hhdm_request.response->offset;

    // 2. Graphical Terminal Display
    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        term_init(fb->address, fb->width, fb->height, fb->pitch);
        term_print("[KERNEL] Framebuffer Terminal initialized.\n");
    }

    // 3. Buddy Memory & Slab Allocators
    pmm_init();
    vmm_init();

    void *heap_phys = pmm_alloc_continuous(256);
    if (!heap_phys) {
        PANIC("Failed to allocate physical memory for kernel heap!");
    }
    init_heap(VIRT(heap_phys), 256 * 4096);

    // 4. Load Limine Boot Modules into memory if present
    if (module_request.response != NULL && module_request.response->module_count > 0) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file *mod = module_request.response->modules[i];
            serial_puts(COM1, "[KERNEL] Boot Module detected: ");
            serial_puts(COM1, mod->path);
            serial_puts(COM1, "\n");
        }
    }

    // 5. Execute all Initcalls (Syscalls, Timer, Tasking, VFS, PCI, Drivers)
    serial_puts(COM1, "[KERNEL] Executing Initcalls...\n");
    do_initcalls();

    // 6. Enable Interrupts & Launch Interactive Shell
    asm volatile ("sti");

    term_print("\nWelcome to EquantOS!\n\n");
    term_print("EquantOS> ");

    for (;;) {
        term_poll_keyboard();
        asm volatile ("hlt");
    }
}