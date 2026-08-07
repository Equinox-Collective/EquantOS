// power.c - Low-level x86_64 reboot and shutdown implementation
#include "power.h"
#include "../core/gen/io.h"
#include "../drivers/serial/serial.h"

void system_reboot(void) {
    serial_puts(COM1, "[POWER] Initiating system reboot...\n");
    
    // 1. Pulse CPU reset line via 8042 Keyboard Controller
    uint8_t temp = 0x02;
    while (temp & 0x02) {
        temp = inb(0x64);
    }
    outb(0x64, 0xFE);

    // 2. Fallback: Triple fault via invalid IDT descriptor
    __asm__ volatile (
        "cli\n\t"
        "xor rax, rax\n\t"
        "lidt [rax]\n\t"
        "int 3"
    );

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void system_power_off(void) {
    serial_puts(COM1, "[POWER] Initiating system shutdown...\n");

    // Send magic shutdown values to common QEMU / Bochs / VirtualBox ports
    outw(0xB004, 0x2000); // QEMU older versions
    outw(0x604, 0x2000);  // QEMU alternative chipset
    outw(0x4004, 0x3400); // VirtualBox / modern QEMU ACPI power-off

    serial_puts(COM1, "[POWER] Shutdown signal sent. Halting CPU.\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}