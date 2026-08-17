// src/kernel/core/initcall.c
#include "initcall.h"
#include "../drivers/serial/serial.h"

extern initcall_t __initcall_start[];
extern initcall_t __initcall_end[];

void do_initcalls(void) {
    for (initcall_t *call = __initcall_start; call < __initcall_end; call++) {
        if (*call) {
            int ret = (*call)();
            if (ret != 0) {
                serial_puts(COM1, "[INITCALL] Warning: Subsystem initialization returned error code!\n");
            }
        }
    }
}