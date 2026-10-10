#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../input.h"

#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01
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
    int cursor_pos;

    size_t prompt_x;
    size_t prompt_y;

    char history[HISTORY_MAX][TTY_BUF_SIZE];
    int history_count;
    int history_idx;

    char log_buf[TTY_LOG_SIZE];
    size_t log_len;
} tty_t;

void tty_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch);
void tty_switch(int index);
char tty_getchar(void);
int  tty_getchar_nonblock(void);
bool tty_has_char(void);
tty_t *tty_get_current(void);

void tty_putchar(char c);
void tty_print(const char *str);
void tty_set_color(uint32_t color);
void tty_set_colors(uint32_t fg_color, uint32_t bg_color);
void tty_clear(void);
void tty_poll_input(void);
char input_code_to_ascii(uint16_t code, bool shift);
uint16_t tty_getchar_raw(void);
void tty_set_kd_mode(int mode);
int tty_get_kd_mode(void);

// Console keyboard modes (KDSKBMODE/KDGKBMODE, as on Linux)
#define K_RAW       0x00    // AT set 1 scancodes
#define K_XLATE     0x01    // characters (the default)
#define K_MEDIUMRAW 0x02    // key codes, bit 7 set on release

// A process that switches the keyboard to K_RAW/K_MEDIUMRAW (the X server)
// becomes the only reader of key presses until it restores K_XLATE or exits;
// meanwhile other readers of the console only see the serial line.
void tty_set_kb_mode(int mode);
int tty_get_kb_mode(void);
bool tty_kb_grabbed(void);

// KDGKBENT: entry of the console keymap for a key code, encoded like Linux
// does (type << 8 | value); table 0 is the plain map, 1 the shifted one
uint16_t tty_get_kb_entry(uint8_t table, uint8_t index);

#endif