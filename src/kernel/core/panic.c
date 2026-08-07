// panic.c - Visual Kernel Panic Handler with Enter-to-Reboot
#include "panic.h"
#include "../drivers/serial/serial.h"
#include "../misc/power.h"
#include "gen/io.h"
#include "../../equterm/term.h"
#include <string.h>

// Direct polling loop for PS/2 keyboard waiting for Enter key (scancode 0x1C)
static void poll_keyboard_enter_for_reboot(void) {
    while (1) {
        // Check if PS/2 output buffer is full (key data ready to read)
        if (inb(0x64) & 0x01) {
            uint8_t scancode = inb(0x60);
            // 0x1C is the make scancode for Enter key
            if (scancode == 0x1C) {
                break;
            }
        }
    }
}

// Bridge for assembly exception stubs in interrupt.asm
void __attribute__((noreturn)) panic_handler(cpu_state_t *state) {
    kernel_panic(state, NULL, 0, "Unhandled CPU Exception");
}

void __attribute__((noreturn)) kernel_panic(cpu_state_t *state, const char *file, int line, const char *message) {
    // 1. Instantly disable all CPU interrupts
    asm volatile ("cli");

    // 2. Log details to serial port COM1
    serial_puts(COM1, "\n\n");
    serial_puts(COM1, "==================================================\n");
    serial_puts(COM1, "                 [EQUANTOS PANIC]                 \n");
    serial_puts(COM1, "==================================================\n");

    if (message) {
        serial_puts(COM1, "Reason : ");
        serial_puts(COM1, message);
        serial_puts(COM1, "\n");
    }

    if (file) {
        serial_puts(COM1, "File   : ");
        serial_puts(COM1, file);
        serial_puts(COM1, "\n");
    }

    if (state != NULL) {
        char buf[32];
        serial_puts(COM1, "\n--- CPU EXCEPTION / INTERRUPT FRAME ---\n");

        serial_puts(COM1, "Vector Int : 0x");
        itoa_hex(state->int_no, buf);
        serial_puts(COM1, buf);

        serial_puts(COM1, " | Error Code : 0x");
        itoa_hex(state->error_code, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, "\n");

        serial_puts(COM1, "RIP        : 0x");
        itoa_hex(state->rip, buf);
        serial_puts(COM1, buf);

        serial_puts(COM1, " | RSP        : 0x");
        itoa_hex(state->user_rsp, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, "\n");
    }

    serial_puts(COM1, "==================================================\n");

    // 3. VISUAL SCREEN PANIC (BSOD style on graphical framebuffer)
    term_clear();
    term_set_color(0x00FF0000); // Bright Red text on black screen
    term_print("==================================================\n");
    term_print("                 [EQUANTOS PANIC]                 \n");
    term_print("==================================================\n\n");

    if (message) {
        term_print("Reason : ");
        term_print(message);
        term_print("\n");
    }

    if (file) {
        term_print("File   : ");
        term_print(file);
        term_print("\n");
    }

    if (state != NULL) {
        char buf[32];
        term_print("\n--- CPU EXCEPTION / INTERRUPT FRAME ---\n");
        term_print("Vector Int : 0x");
        itoa_hex(state->int_no, buf);
        term_print(buf);
        term_print(" | Error Code : 0x");
        itoa_hex(state->error_code, buf);
        term_print(buf);
        term_print("\n");

        term_print("RIP        : 0x");
        itoa_hex(state->rip, buf);
        term_print(buf);
        term_print("\n");
    }

    term_print("\n==================================================\n");
    term_print("   System halted. Press [ENTER] to reboot system.   \n");
    term_print("==================================================\n");

    // 4. Wait for Enter keypress directly via polling
    poll_keyboard_enter_for_reboot();

    // 5. Trigger Hardware Reboot
    system_reboot();

    for (;;) {
        asm volatile ("cli; hlt");
    }
}