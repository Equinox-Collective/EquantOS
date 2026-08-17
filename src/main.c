// src/main.c
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
static volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

uint64_t hhdm_offset = 0;

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

void _start(void) {
    // Stage 0: Low-level CPU Core Bootstrap
    serial_init(COM1);
    enable_fpu_sse();
    init_gdt();
    init_idt();

    if (hhdm_request.response == NULL || hhdm_request.response->offset == 0) {
        PANIC("Limine HHDM response missing!");
    }
    hhdm_offset = hhdm_request.response->offset;

    // Stage 1: Display Early Terminal
    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        term_init(fb->address, fb->width, fb->height, fb->pitch);
    }

    // Stage 2: Core Memory Infrastructure
    pmm_init();
    vmm_init();

    void *heap_phys = pmm_alloc_continuous(256);
    if (!heap_phys) {
        PANIC("Failed to allocate physical memory for kernel heap!");
    }
    init_heap(VIRT(heap_phys), 256 * 4096);

    serial_puts(COM1, "[KERNEL] Core Memory and CPU initialised. Executing Initcalls...\n");

    // Stage 3: Dynamic Drivers & Subsystem Auto-Discovery
    // Everything else (PCI, USB, Storage, VFS, Sched) registers itself via initcalls!
    do_initcalls();

    asm volatile ("sti");

    term_print("\nEquantOS Kernel booted successfully!\n");
    term_print("EquantOS> ");

    for (;;) {
        term_poll_keyboard();
        asm volatile ("hlt");
    }
}