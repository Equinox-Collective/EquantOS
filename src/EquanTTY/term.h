#ifndef TERM_H
#define TERM_H

#include <stdint.h>
#include <stddef.h>

void term_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch);
void term_print(const char *str);
void term_putchar(char c);
void term_poll_keyboard(void); // Polls keyboard buffer and handles shell input

void term_clear(void);              // Clear the screen and home the cursor
void term_set_color(uint32_t color); // 0x00RRGGBB
uint32_t term_get_color(void);

#endif // TERM_H