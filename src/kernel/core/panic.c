// src/kernel/core/panic.c - Production Full Register Dump
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
        serial_puts(COM1, "\n[DOUBLE PANIC DETECTED] CPU Halted.\n");
        for (;;) { asm volatile ("hlt"); }
    }
    in_panic = true;

    char buf[32];
    uint64_t cr2_fault_addr = 0;
    if (state != NULL && state->int_no == 14) {
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2_fault_addr));
    }

    // 1. ВЫВОД В SERIAL ПОРТ
    serial_puts(COM1, "\n\n==================================================\n");
    serial_puts(COM1, "                 [EQUANTOS PANIC]                 \n");
    serial_puts(COM1, "==================================================\n");

    if (message) {
        serial_puts(COM1, "Reason  : "); serial_puts(COM1, message); serial_puts(COM1, "\n");
    }
    if (file) {
        serial_puts(COM1, "File    : "); serial_puts(COM1, file); serial_puts(COM1, "\n");
    }

    if (state != NULL) {
        serial_puts(COM1, "\n--- CPU EXCEPTION / REGISTER DUMP ---\n");
        serial_puts(COM1, "Vector  : 0x"); itoa_hex(state->int_no, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | Error Code : 0x"); itoa_hex(state->error_code, buf); serial_puts(COM1, buf); serial_puts(COM1, "\n");

        serial_puts(COM1, "RIP     : 0x"); itoa_hex(state->rip, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | CS : 0x"); itoa_hex(state->cs, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | RFLAGS : 0x"); itoa_hex(state->rflags, buf); serial_puts(COM1, buf); serial_puts(COM1, "\n");

        serial_puts(COM1, "RSP     : 0x"); itoa_hex(state->user_rsp, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | SS : 0x"); itoa_hex(state->ss, buf); serial_puts(COM1, buf); serial_puts(COM1, "\n");

        serial_puts(COM1, "RAX     : 0x"); itoa_hex(state->rax, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | RBX : 0x"); itoa_hex(state->rbx, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | RCX : 0x"); itoa_hex(state->rcx, buf); serial_puts(COM1, buf); serial_puts(COM1, "\n");

        serial_puts(COM1, "RDX     : 0x"); itoa_hex(state->rdx, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | RSI : 0x"); itoa_hex(state->rsi, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | RDI : 0x"); itoa_hex(state->rdi, buf); serial_puts(COM1, buf); serial_puts(COM1, "\n");

        serial_puts(COM1, "RBP     : 0x"); itoa_hex(state->rbp, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | R8  : 0x"); itoa_hex(state->r8, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | R9  : 0x"); itoa_hex(state->r9, buf); serial_puts(COM1, buf); serial_puts(COM1, "\n");
    }
    serial_puts(COM1, "==================================================\n");

    // 2. ВЫВОД НА ЭКРАН ТЕРМИНАЛА
    term_clear();
    term_set_color(0x00FF0000);
    term_print("==================================================\n");
    term_print("                 [EQUANTOS PANIC]                 \n");
    term_print("==================================================\n\n");

    if (state != NULL) {
        term_print("Vector : 0x"); itoa_hex(state->int_no, buf); term_print(buf);
        term_print(" | Err : 0x"); itoa_hex(state->error_code, buf); term_print(buf); term_print("\n");

        term_print("RIP    : 0x"); itoa_hex(state->rip, buf); term_print(buf);
        term_print(" | CS  : 0x"); itoa_hex(state->cs, buf); term_print(buf); term_print("\n");

        term_print("RSP    : 0x"); itoa_hex(state->user_rsp, buf); term_print(buf);
        term_print(" | SS  : 0x"); itoa_hex(state->ss, buf); term_print(buf); term_print("\n");

        term_print("RAX    : 0x"); itoa_hex(state->rax, buf); term_print(buf);
        term_print(" | RBX : 0x"); itoa_hex(state->rbx, buf); term_print(buf); term_print("\n");
        term_print("RCX    : 0x"); itoa_hex(state->rcx, buf); term_print(buf);
        term_print(" | RDX : 0x"); itoa_hex(state->rdx, buf); term_print(buf); term_print("\n");
    }

    term_print("\nPress [ENTER] to reboot system.\n");
    poll_keyboard_enter_for_reboot();
    system_reboot();

    for (;;) { asm volatile ("cli; hlt"); }
}