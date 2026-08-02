#include "panic.h"
#include "../../drivers/serial/serial.h"
#include <string.h>

// Bridge for assembly exception stubs in interrupt.asm
void __attribute__((noreturn)) panic_handler(cpu_state_t *state) {
    kernel_panic(state, NULL, 0, "Unhandled CPU Exception");
}

void __attribute__((noreturn)) kernel_panic(cpu_state_t *state, const char *file, int line, const char *message) {
    // 1. Instantly disable all CPU interrupts
    asm volatile ("cli");

    // 2. Print header banner to serial port COM1
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

    // 3. If triggered by a CPU exception, dump registers and error codes
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

        serial_puts(COM1, "RAX: 0x"); itoa_hex(state->rax, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | RBX: 0x"); itoa_hex(state->rbx, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | RCX: 0x"); itoa_hex(state->rcx, buf); serial_puts(COM1, buf);
        serial_puts(COM1, " | RDX: 0x"); itoa_hex(state->rdx, buf); serial_puts(COM1, buf);
        serial_puts(COM1, "\n");
    }

    serial_puts(COM1, "==================================================\n");
    serial_puts(COM1, "System halted safely. Kernel execution stopped.\n");

    for (;;) {
        asm volatile ("cli; hlt");
    }
}