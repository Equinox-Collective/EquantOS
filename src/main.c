// main.c - EquantOS Kernel Entry Point
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "panic.h"
#include "serial.h"
#include "term.h"
#include "timer.h"
#include "gdt.h"
#include "idt.h"
#include "task.h"
#include "keyboard.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "ramfs.h"
#include "syscall.h"
#include "loader.h"
#include "sched.h"
#include "ata.h"
#include "mbr.h"
#include "pci.h"
#include "memory.h"

// Limine base revision request (revision 3)
__attribute__((used, section(".requests")))
static volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(3);

// Limine framebuffer request
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

// Global Higher-Half Direct Map offset provided by Limine
uint64_t hhdm_offset = 0;

// Limine HHDM (Higher-Half Direct Map) request
__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = { 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b, 0x3e7e279702be32af, 0xca1c4f3bd1280cee },
    .revision = 0,
    .response = NULL
};

static void init_sse(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // Clear CR0.EM (Emulation bit)
    cr0 |= (1 << 1);  // Set CR0.MP (Monitor Coprocessor bit)
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);  // Set CR4.OSFXSR (Enable FXSAVE/FXRSTOR for SSE)
    cr4 |= (1 << 10); // Set CR4.OSXMMEXCPT (Enable unmasked SIMD exceptions)
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void _start(void) {
    // 1. Initialize serial port & CPU descriptors
    serial_init(COM1);
    init_sse();
    init_gdt();
    init_idt();
    init_timer(100);

    // 2. Safely obtain HHDM offset from Limine
    if (hhdm_request.response == NULL) {
        PANIC("Limine HHDM response is NULL!");
    }
    hhdm_offset = hhdm_request.response->offset;

    if (hhdm_offset == 0) {
        PANIC("HHDM offset is zero!");
    }

    // 3. Initialize Memory Management (PMM & VMM)
    pmm_init();
    vmm_init();
    serial_puts(COM1, "[KERNEL] PMM and VMM Initialized successfully.\n");

    // 4. Initialize Kernel Heap (allocate a safe 1 MB continuous block)
    void *heap_phys = pmm_alloc_continuous(256); // 256 pages * 4KB = 1 MB
    if (heap_phys == NULL) {
        PANIC("Failed to allocate physical memory for kernel heap!");
    }
    
    uint64_t heap_virt = VIRT(heap_phys);
    init_heap(heap_virt, 256 * 4096);
    serial_puts(COM1, "[KERNEL] Kernel Heap Initialized (1 MB).\n");

    // 5. Initialize Tasking, Scheduler & System Calls
    task_init();
    sched_init(current_task);
    init_syscalls();
    serial_puts(COM1, "[KERNEL] Tasking, Scheduler & System Calls Initialized.\n");

    // 6. Initialize VFS and RAMFS
    vfs_init();
    vfs_node_t *ramfs_root = ramfs_create_root();
    vfs_mount("/", ramfs_root);
    serial_puts(COM1, "[KERNEL] VFS and RAMFS mounted at root '/'.\n");

    // Populate RAMFS with Limine boot modules
    if (module_request.response != NULL && module_request.response->module_count > 0) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file *mod = module_request.response->modules[i];
            const char *filename = (mod->path[0] == '/') ? mod->path + 1 : mod->path;
            ramfs_create_file(ramfs_root, filename, mod->address, mod->size);
            
            serial_puts(COM1, "[KERNEL] RAMFS loaded module: /");
            serial_puts(COM1, filename);
            serial_puts(COM1, "\n");
        }
    } else {
        serial_puts(COM1, "[KERNEL] No boot modules found by Limine to mount.\n");
    }

    // 7. Initialize Hardware Drivers (PCI, ATA, MBR Partition Scanning)
    pci_init();
    ata_identify();
    mbr_init();
    serial_puts(COM1, "[KERNEL] Hardware drivers and MBR scan completed.\n");

    // 8. Framebuffer & Terminal
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        PANIC("No graphic framebuffers provided by Limine!");
    }
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    term_init(fb->address, fb->width, fb->height, fb->pitch);

    // 9. Load and spawn equantmemtest ELF module from VFS (Clean single load)
    vfs_node_t *test_file = vfs_open("/equantmemtest.elf", 0);
    if (test_file != NULL) {
        serial_puts(COM1, "[KERNEL] Found equantmemtest.elf in VFS. Spawning...\n");
        ramfs_file_data_t *fdata = (ramfs_file_data_t *)test_file->ptr;
        if (fdata && elf_load(fdata->buffer, test_file->length)) {
            serial_puts(COM1, "[KERNEL] equantmemtest loaded and spawned successfully from VFS!\n");
        } else {
            serial_puts(COM1, "[KERNEL PANIC] Failed to parse/load equantmemtest ELF from VFS!\n");
        }
    } else {
        serial_puts(COM1, "[KERNEL] equantmemtest.elf not found in VFS.\n");
    }

    // 10. Keyboard & Interrupts
    keyboard_init();
    asm volatile ("sti");

    // 11. Shell welcome & idle loop
    term_print("Welcome to EquantOS!\n\n");
    term_print("EquantOS> ");

    for (;;) {
        term_poll_keyboard();
        asm volatile ("hlt");
    }
}