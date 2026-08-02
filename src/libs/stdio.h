#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h> // Встроенная магия компилятора для "..."

// Печатает форматированный текст в буфер (строку)
int sprintf(char* buffer, const char* format, ...);

// Печатает форматированный текст сразу в терминал ОС
void printf(const char* format, ...);

// Версия для списка аргументов (нужна внутри sprintf)
int vsprintf(char* buffer, const char* format, va_list args);

#endif