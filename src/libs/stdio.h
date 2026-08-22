// src/libs/stdio.h - Kernel Standard I/O Formatting Declarations
#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

// Safe length-bounded buffer string formatting
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);

// Unbounded buffer string formatting
int sprintf(char *buffer, const char *format, ...);
int vsprintf(char *buffer, const char *format, va_list args);

// Print formatted string to default OS terminal
void printf(const char *format, ...);

#endif // LIBC_STDIO_H