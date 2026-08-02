#include "panic.h"
#include "../../drivers/serial/serial.h"

// Forward declaration or global reference to framebuffer info if needed for visual panic screen
extern void *kernel_fb_address;
extern uint64_t kernel_fb_pitch;
extern uint64_t kernel_fb_width;
extern uint64_t kernel_fb_height;

void __attribute__((noreturn)) kernel_panic(const char *file, int line, const char *message) {
    // 1. Disable all CPU interrupts immediately
    asm volatile ("cli");

    // 2. Output structured error report to serial port COM1
    serial_puts(COM1, "\n\n");
    serial_puts(COM1, "==================================================\n");
    serial_puts(COM1, "                 [EQUANTOS PANIC]                 \n");
    serial_puts(COM1, "==================================================\n");
    serial_puts(COM1, "Reason : ");
    serial_puts(COM1, message);
    serial_puts(COM1, "\n");
    serial_puts(COM1, "File   : ");
    serial_puts(COM1, file);
    serial_puts(COM1, "\n");
    serial_puts(COM1, "Status : System halted safely. Kernel execution stopped.\n");
    serial_puts(COM1, "==================================================\n");

    // 3. Optional: Paint the entire screen red if framebuffer parameters are initialized
    // (We will wire up global FB pointers in main.c so panic can use them)
    
    // 4. Infinite safe halt loop
    for (;;) {
        asm volatile ("cli; hlt");
    }
}