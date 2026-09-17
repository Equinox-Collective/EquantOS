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
#include "kernel/drivers/tty/tty.h"
#include "equterm/shell.h"
#include "kernel/drivers/display/psf2.h"
#include "string.h"
#include "kernel/proc/initproc.h"

__attribute__((used, section(".requests")))
volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

struct limine_framebuffer *kernel_fb = NULL;
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
    serial_init(COM1);
    enable_fpu_sse();
    init_gdt();
    init_idt();

    if (hhdm_request.response == NULL || hhdm_request.response->offset == 0) {
        PANIC("Limine HHDM response is NULL!");
    }
    hhdm_offset = hhdm_request.response->offset;

    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count > 0) {
        kernel_fb = framebuffer_request.response->framebuffers[0];
        tty_init(kernel_fb->address, kernel_fb->width, kernel_fb->height, kernel_fb->pitch);
    }

    pmm_init();
    vmm_init();
    init_heap(0, 0);

    asm volatile ("sti");

    serial_puts(COM1, "[KERNEL] Executing Initcalls...\n");
    do_initcalls();

    tty_print("\nWelcome to EquantOS!\n\n");
    kernel_start_userland();

    for (;;) {
        asm volatile ("hlt");
    }
}