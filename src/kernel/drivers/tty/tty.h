// src/kernel/drivers/tty/tty.h - Clean TTY Subsystem Header
#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_TTYS 6
#define TTY_BUF_SIZE 256
#define TTY_LOG_SIZE 4096
#define HISTORY_MAX 16

typedef struct {
    int id;
    bool active;
    bool initialized;

    char line_buf[TTY_BUF_SIZE];
    int line_len;

    char history[HISTORY_MAX][TTY_BUF_SIZE];
    int history_count;
    int history_idx;

    char log_buf[TTY_LOG_SIZE];
    size_t log_len;
} tty_t;

void tty_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch);
void tty_switch(int index);
tty_t *tty_get_current(void);

void tty_putchar(char c);
void tty_print(const char *str);
void tty_set_color(uint32_t color);
void tty_clear(void);
void tty_poll_input(void);

#endif // TTY_H