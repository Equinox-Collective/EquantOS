// src/equterm/term.h - Clean Display Interface
#ifndef TERM_H
#define TERM_H

#include <stdint.h>
#include <stddef.h>

void term_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch);
void term_putchar_raw(char c);
void term_clear_screen(void);

// Standard API Wrappers forwarded to TTY
void term_putchar(char c);
void term_print(const char *str);
void term_clear(void);
void term_set_color(uint32_t color);
uint32_t term_get_color(void);

#endif // TERM_H