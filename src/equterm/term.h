#ifndef TERM_H
#define TERM_H

#include <stdint.h>
#include <stddef.h>

void term_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch);
void term_putchar_raw(char c);
void term_clear_screen(void);

void term_putchar(char c);
void term_print(const char *str);
void term_clear(void);
void term_set_color(uint32_t color);
void term_set_custom_colors(uint32_t fg, uint32_t bg);
uint32_t term_get_color(void);

size_t term_get_cursor_x(void);
size_t term_get_cursor_y(void);
void term_set_cursor(size_t x, size_t y);

#endif // TERM_H