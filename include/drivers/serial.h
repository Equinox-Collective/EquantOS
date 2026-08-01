// include/drivers/serial.h
#pragma once
#include <stdint.h>

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

// Initializes the serial port for communication (115200 baud)
void serial_init(uint16_t port);

// Writes a single character to the serial port
void serial_putchar(uint16_t port, char c);

// Writes a null-terminated string to the serial port
void serial_puts(uint16_t port, const char *str);

// Reads a character from the serial port (blocking)
char serial_getchar(uint16_t port);