// drivers/serial.c
#include <drivers/serial.h>
#include <kernel/io.h> // No more ../../../!

// Serial port register offsets
#define SERIAL_DATA         0
#define SERIAL_INT_ENABLE   1
#define SERIAL_FIFO_CTRL    2
#define SERIAL_LINE_CTRL    3
#define SERIAL_MODEM_CTRL   4
#define SERIAL_LINE_STATUS  5

#define SERIAL_LS_DATA_READY        0x01
#define SERIAL_LS_TRANSMIT_EMPTY    0x20

// Simple spinlock for synchronizing output across cores/threads
// We use GCC built-in atomics. 
static volatile int serial_lock = 0;

static inline void serial_acquire_lock(void) {
    while (__sync_lock_test_and_set(&serial_lock, 1)) {
        __asm__ volatile("pause");
    }
}

static inline void serial_release_lock(void) {
    __sync_lock_release(&serial_lock);
}

void serial_init(uint16_t port) {
    outb(port + SERIAL_INT_ENABLE, 0x00);    // Disable all interrupts
    outb(port + SERIAL_LINE_CTRL, 0x80);     // Enable DLAB (set baud rate divisor)
    outb(port + SERIAL_DATA, 0x01);          // Set divisor to 1 (lo byte) 115200 baud
    outb(port + SERIAL_INT_ENABLE, 0x00);    // (hi byte)
    outb(port + SERIAL_LINE_CTRL, 0x03);     // 8 bits, no parity, one stop bit
    outb(port + SERIAL_FIFO_CTRL, 0xC7);     // Enable FIFO, clear them, with 14-byte threshold
    outb(port + SERIAL_MODEM_CTRL, 0x0B);    // IRQs enabled, RTS/DSR set
    outb(port + SERIAL_MODEM_CTRL, 0x1E);    // Set in loopback mode, test the serial chip
    outb(port + SERIAL_DATA, 0xAE);          // Test serial chip (send byte 0xAE and check if return is same)
    
    // Check if serial is faulty
    if (inb(port + SERIAL_DATA) != 0xAE) {
        return;
    }
    
    // Set in normal operation mode
    outb(port + SERIAL_MODEM_CTRL, 0x0F);
}

static int serial_transmit_empty(uint16_t port) {
    return inb(port + SERIAL_LINE_STATUS) & SERIAL_LS_TRANSMIT_EMPTY;
}

void serial_putchar(uint16_t port, char c) {
    while (serial_transmit_empty(port) == 0) {
        __asm__ volatile("pause"); // Good practice to pause in busy-wait loops
    }
    outb(port + SERIAL_DATA, c);
}

void serial_puts(uint16_t port, const char *str) {
    serial_acquire_lock();
    while (*str) {
        serial_putchar(port, *str++);
    }
    serial_release_lock();
}

static int serial_received(uint16_t port) {
    return inb(port + SERIAL_LINE_STATUS) & SERIAL_LS_DATA_READY;
}

char serial_getchar(uint16_t port) {
    while (serial_received(port) == 0) {
        __asm__ volatile("pause");
    }
    return inb(port + SERIAL_DATA);
}