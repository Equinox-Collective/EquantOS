// main.c - EquantOS Kernel Entry Point
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "kernel/core/panic.h"
#include "kernel/drivers/serial/serial.h"
#include "equterm/term.h"
#include "kernel/misc/timer.h"
#include "kernel/core/gen/gdt.h"
#include "kernel/core/gen/idt.h"
#include "kernel/core/gen/cpu.h"
#include "kernel/proc/task.h"
#include "kernel/drivers/keyboard/keyboard.h"
#include "kernel/core/mem/vmm.h"
#include "kernel/core/mem/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/ramfs.h"
#include "kernel/proc/syscall.h"
#include "kernel/proc/loader.h"
#include "kernel/proc/sched.h"
#include "kernel/drivers/disk/ata.h"
#include "kernel/fs/mbr.h"
#include "kernel/drivers/pci/pci.h"
#include "kernel/core/mem/memory.h"
#include "kernel/fs/fat32.h"
#include "kernel/drivers/display/psf2.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/gpt.h"
#include "kernel/fs/partition.h"
#include "kernel/drivers/disk/nvme.h"
#include "kernel/drivers/disk/block.h"
#include "string.h"

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

void _start(void) {
    // 1. Initialize serial port & CPU descriptors
    serial_init(COM1);
    enable_fpu_sse();
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

    // 3. ИНИЦИАЛИЗИРУЕМ ЭКРАН СРАЗУ (ДО ВСЕХ ДРАЙВЕРОВ И ПАМЯТИ)
    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        term_init(fb->address, fb->width, fb->height, fb->pitch);
        term_print("[KERNEL] Early Framebuffer & Terminal initialized successfully.\n");
    }

    // 4. Initialize Memory Management (PMM & VMM)
    pmm_init();
    vmm_init();
    serial_puts(COM1, "[KERNEL] PMM and VMM Initialized successfully.\n");

    // 5. Initialize Kernel Heap
    void *heap_phys = pmm_alloc_continuous(256);
    if (heap_phys == NULL) {
        PANIC("Failed to allocate physical memory for kernel heap!");
    }
    uint64_t heap_virt = VIRT(heap_phys);
    init_heap(heap_virt, 256 * 4096);
    serial_puts(COM1, "[KERNEL] Kernel Heap Initialized (1 MB).\n");

    // 6. Tasking, Scheduler & System Calls
    task_init();
    sched_init(current_task);
    init_syscalls();
    serial_puts(COM1, "[KERNEL] Tasking, Scheduler & System Calls Initialized.\n");

    // 7. Initialize VFS and RAMFS
    vfs_init();
    vfs_node_t *ramfs_root = ramfs_create_root();
    vfs_mount("/", ramfs_root);
    serial_puts(COM1, "[KERNEL] VFS and RAMFS mounted at root '/'.\n");

    if (module_request.response != NULL && module_request.response->module_count > 0) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file *mod = module_request.response->modules[i];
            const char *filename = strrchr(mod->path, '/');
            filename = (filename != NULL) ? filename + 1 : mod->path;
            ramfs_create_file(ramfs_root, filename, mod->address, mod->size);
            
            serial_puts(COM1, "[KERNEL] RAMFS loaded module: /");
            serial_puts(COM1, filename);
            serial_puts(COM1, "\n");
        }
    }

    // 8. Теперь сканирование PCI и NVMe БУДЕТ ВЫВОДИТЬСЯ ПРЯМО НА МОНИТОР!
    pci_init();
    
    if (nvme_init() == NVME_SUCCESS) {
        block_device_t nvme_disk = nvme_get_block_device();
        disk_partition_scan_device(nvme_disk);

        int p_count = disk_get_partition_count();
        for (int i = 0; i < p_count; i++) {
            partition_info_t *part = disk_get_partition(i);
            if (!part) continue;

            vfs_node_t *fat_root = fat32_mount_partition(nvme_disk, part->start_lba, part->sector_count);
            if (fat_root) {
                vfs_node_t *disk_dir = ramfs_create_directory(vfs_root, "disk");
                if (disk_dir) {
                    disk_dir->flags |= FS_MOUNTPOINT;
                    disk_dir->ptr = (struct vfs_node *)fat_root;
                    serial_puts(COM1, "[FAT32 SUCCESS] Mounted FAT32 partition to /disk\n");
                }
                continue;
            }

            vfs_node_t *ext2_root = ext2_mount_partition(nvme_disk, part->start_lba);
            if (ext2_root) {
                vfs_node_t *ext2_dir = ramfs_create_directory(vfs_root, "ext2");
                if (ext2_dir) {
                    ext2_dir->flags |= FS_MOUNTPOINT;
                    ext2_dir->ptr = (struct vfs_node *)ext2_root;
                    serial_puts(COM1, "[EXT2 SUCCESS] Mounted EXT2 partition to /ext2\n");
                }
                continue;
            }
        }
    } else {
        serial_puts(COM1, "[KERNEL WARNING] NVMe controller not found or init failed.\n");
    }

    // 9. Keyboard & Interrupts
    keyboard_init();
    asm volatile ("sti");

    // 10. Shell welcome
    term_print("\nWelcome to EquantOS!\n\n");
    term_print("EquantOS> ");

    for (;;) {
        term_poll_keyboard();
        asm volatile ("hlt");
    }
}