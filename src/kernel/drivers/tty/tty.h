#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_TTYS 6
#define TTY_BUF_SIZE 256
#define HISTORY_MAX 16

typedef struct {
    int id;
    bool active;
    
    // Line buffer & cursor position
    char line_buf[TTY_BUF_SIZE];
    int line_len;
    int cursor_pos;

    // Command History
    char history[HISTORY_MAX][TTY_BUF_SIZE];
    int history_count;
    int history_idx;

    // Visual State
    uint32_t fg_color;
    size_t cursor_x;
    size_t cursor_y;
} tty_t;

void tty_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch);
void tty_switch(int index);
tty_t *tty_get_current(void);
void tty_poll_input(void);

#endif // TTY_H