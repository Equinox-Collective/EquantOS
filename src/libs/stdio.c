// src/libs/stdio.c - Kernel Standard I/O Implementation with Safe Buffer Bounds
#include "stdio.h"
#include "string.h"
#include <stdbool.h>

extern void term_print(const char *str);

int vsnprintf(char *buffer, size_t size, const char *format, va_list args) {
    if (!buffer || size == 0) return 0;

    char *ptr = buffer;
    const char *f = format;
    char temp_buf[64];
    size_t written = 0;
    size_t max_chars = size - 1; // Leave 1 byte for null-terminator

    while (*f && written < max_chars) {
        if (*f != '%') {
            *ptr++ = *f++;
            written++;
            continue;
        }

        f++; // Skip '%'

        int width = 0;
        char pad = ' ';
        if (*f == '0') {
            pad = '0';
            f++;
        }
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f - '0');
            f++;
        }

        // Check for long modifiers (%lx, %lld, etc.)
        bool is_long = false;
        if (*f == 'l') {
            is_long = true;
            f++;
            if (*f == 'l') {
                f++;
            }
        }

        if (*f == 'u' || *f == 'd' || *f == 'x' || *f == 'X' || *f == 'p') {
            uint64_t val;
            if (*f == 'p' || is_long) {
                val = va_arg(args, uint64_t);
            } else {
                val = (uint64_t)va_arg(args, unsigned int);
            }

            if (*f == 'p' || *f == 'x' || *f == 'X') {
                itoa_hex(val, temp_buf);
            } else {
                itoa((int64_t)val, 10, temp_buf);
            }

            int len = strlen(temp_buf);
            while (len < width && written < max_chars) {
                *ptr++ = pad;
                written++;
                len++;
            }

            char *t = temp_buf;
            while (*t && written < max_chars) {
                *ptr++ = *t++;
                written++;
            }
        } else if (*f == 's') {
            char *s = va_arg(args, char *);
            if (!s) s = "(null)";
            while (*s && written < max_chars) {
                *ptr++ = *s++;
                written++;
            }
        } else if (*f == 'c') {
            *ptr++ = (char)va_arg(args, int);
            written++;
        } else if (*f == '%') {
            *ptr++ = '%';
            written++;
        }

        if (*f) f++;
    }

    *ptr = '\0';
    return (int)written;
}

int snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, size, format, args);
    va_end(args);
    return len;
}

int vsprintf(char *buffer, const char *format, va_list args) {
    return vsnprintf(buffer, (size_t)-1, format, args);
}

int sprintf(char *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int len = vsprintf(buffer, format, args);
    va_end(args);
    return len;
}

void printf(const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    term_print(buffer);
}