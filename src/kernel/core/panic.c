// panic.c - Visual Kernel Panic Handler with Enter-to-Reboot
#include "panic.h"
#include "../drivers/serial/serial.h"
#include "../misc/power.h"
#include "gen/io.h"
#include "../../equterm/term.h"
#include <string.h>
#include <stdbool.h>

static bool in_panic = false;

static void poll_keyboard_enter_for_reboot(void) {
    while (1) {
        if (inb(0x64) & 0x01) {
            uint8_t scancode = inb(0x60);
            if (scancode == 0x1C) {
                break;
            }
        }
    }
}

void __attribute__((noreturn)) panic_handler(cpu_state_t *state) {
    kernel_panic(state, NULL, 0, "Unhandled CPU Exception");
}

void __attribute__((noreturn)) kernel_panic(cpu_state_t *state, const char *file, int line, const char *message) {
    asm volatile ("cli");

    if (in_panic) {
        serial_puts(COM1, "\n[DOUBLE PANIC DETECTED] CPU Halted to prevent infinite loop.\n");
        for (;;) { asm volatile ("hlt"); }
    }
    in_panic = true;

    char buf[32];
    uint64_t cr2_fault_addr = 0;
    if (state != NULL && state->int_no == 14) {
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2_fault_addr));
    }

    // 1. ВЫВОД В SERIAL ПОРТ (100% Гарантированный и изолированный)
    serial_puts(COM1, "\n\n==================================================\n");
    serial_puts(COM1, "                 [EQUANTOS PANIC]                 \n");
    serial_puts(COM1, "==================================================\n");

    if (message) {
        serial_puts(COM1, "Reason  : ");
        serial_puts(COM1, message);
        serial_puts(COM1, "\n");
    }

    if (file) {
        serial_puts(COM1, "File    : ");
        serial_puts(COM1, file);
        serial_puts(COM1, "\n");
    }

    if (state != NULL) {
        serial_puts(COM1, "\n--- CPU EXCEPTION / INTERRUPT FRAME ---\n");

        serial_puts(COM1, "Vector  : 0x");
        itoa_hex(state->int_no, buf);
        serial_puts(COM1, buf);

        serial_puts(COM1, " | Error Code : 0x");
        itoa_hex(state->error_code, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, "\n");

        if (state->int_no == 14) {
            serial_puts(COM1, "CR2 Fault Address : 0x");
            itoa_hex(cr2_fault_addr, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, "\n");
        }

        serial_puts(COM1, "RIP     : 0x");
        itoa_hex(state->rip, buf);
        serial_puts(COM1, buf);

        serial_puts(COM1, " | RSP  : 0x");
        itoa_hex(state->user_rsp, buf);
        serial_puts(COM1, buf);
        serial_puts(COM1, "\n");
    }

    serial_puts(COM1, "==================================================\n");

    // 2. ОПЦИОНАЛЬНЫЙ ВЫВОД НА ЭКРАН МОНИТОРА
    term_clear();
    term_set_color(0x00FF0000);
    term_print("==================================================\n");
    term_print("                 [EQUANTOS PANIC]                 \n");
    term_print("==================================================\n\n");

    if (message) {
        term_print("Reason  : ");
        term_print(message);
        term_print("\n");
    }

    if (file) {
        term_print("File    : ");
        term_print(file);
        term_print("\n");
    }

    if (state != NULL) {
        term_print("\n--- CPU EXCEPTION / INTERRUPT FRAME ---\n");

        term_print("Vector  : 0x");
        itoa_hex(state->int_no, buf);
        term_print(buf);

        term_print(" | Err : 0x");
        itoa_hex(state->error_code, buf);
        term_print(buf);
        term_print("\n");

        if (state->int_no == 14) {
            term_print("CR2 Fault Address : 0x");
            itoa_hex(cr2_fault_addr, buf);
            term_print(buf);
            term_print("\n");
        }

        term_print("RIP     : 0x");
        itoa_hex(state->rip, buf);
        term_print(buf);

        term_print(" | RSP : 0x");
        itoa_hex(state->user_rsp, buf);
        term_print(buf);
        term_print("\n");
    }

    term_print("\n==================================================\n");
    term_print("   System halted. Press [ENTER] to reboot system.   \n");
    term_print("==================================================\n");

    poll_keyboard_enter_for_reboot();
    system_reboot();

    for (;;) {
        asm volatile ("cli; hlt");
    }
}