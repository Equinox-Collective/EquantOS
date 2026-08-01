#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

// Standard COM port base addresses
#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

/**
 * @brief Initialize the serial port communication hardware.
 * @param port The base port address (e.g., COM1).
 */
void serial_init(uint16_t port);

/**
 * @brief Write a single character to the serial port.
 * @param port The base port address.
 * @param c Character to send.
 */
void serial_putchar(uint16_t port, char c);

/**
 * @brief Write a null-terminated string to the serial port.
 * @param port The base port address.
 * @param str Pointer to the string.
 */
void serial_puts(uint16_t port, const char *str);

/**
 * @brief Check if the transmit holding register is empty.
 * @param port The base port address.
 * @return Non-zero if empty, zero otherwise.
 */
int serial_transmit_empty(uint16_t port);

/**
 * @brief Read a character from the serial port (blocking).
 * @param port The base port address.
 * @return Received character.
 */
char serial_getchar(uint16_t port);

/**
 * @brief Check if data is available to read from the serial port.
 * @param port The base port address.
 * @return Non-zero if data is ready, zero otherwise.
 */
int serial_received(uint16_t port);

#endif // SERIAL_H