#ifndef IO_H
#define IO_H

#include <stdint.h>

/**
 * @brief Read a byte from an I/O port.
 * @param port The I/O port address.
 * @return The byte read from the port.
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__ ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief Write a byte to an I/O port.
 * @param port The I/O port address.
 * @param val The byte to write.
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__ ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief Read a word (16-bit) from an I/O port.
 * @param port The I/O port address.
 * @return The 16-bit word read.
 */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__ ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief Write a word (16-bit) to an I/O port.
 * @param port The I/O port address.
 * @param val The 16-bit word to write.
 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__ ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief Read a double word (32-bit) from an I/O port.
 * @param port The I/O port address.
 * @return The 32-bit double word read.
 */
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__ ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief Write a double word (32-bit) to an I/O port.
 * @param port The I/O port address.
 * @param val The 32-bit double word to write.
 */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__ ("outl %0, %1" : : "a"(val), "Nd"(port));
}

#endif // IO_H