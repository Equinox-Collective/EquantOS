#ifndef TERM_H
#define TERM_H

#include <stdint.h>

void term_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch);
void term_print(const char *str);
void term_putchar(char c);

#endif // TERM_H