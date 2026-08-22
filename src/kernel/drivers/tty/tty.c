// src/kernel/drivers/tty/tty.c - Stream Log Virtual Consoles
#include "tty.h"
#include "../../core/globalkeybinds.h"
#include "../serial/serial.h"
#include "../../../equterm/term.h"
#include "../../../equterm/shell.h"
#include "../input.h"
#include "string.h"

static tty_t ttys[MAX_TTYS];
static int current_tty_id = 0;
static bool shift_pressed = false;

static void tty_append_log(tty_t *tty, char c) {
    if (tty->log_len < TTY_LOG_SIZE - 1) {
        tty->log_buf[tty->log_len++] = c;
        tty->log_buf[tty->log_len] = '\0';
    } else {
        // Shift buffer left by 1024 bytes if log is full
        memmove(tty->log_buf, tty->log_buf + 1024, tty->log_len - 1024);
        tty->log_len -= 1024;
        tty->log_buf[tty->log_len++] = c;
        tty->log_buf[tty->log_len] = '\0';
    }
}

void tty_putchar(char c) {
    tty_t *tty = &ttys[current_tty_id];

    // Handle Backspace in Stream Log
    if (c == '\b') {
        if (tty->log_len > 0 && tty->log_buf[tty->log_len - 1] != '\n') {
            tty->log_len--;
            tty->log_buf[tty->log_len] = '\0';
        }
    } else {
        tty_append_log(tty, c);
    }

    serial_putchar(COM1, c);

    // If this TTY is active, pass directly to term.c
    if (tty->active) {
        term_putchar_raw(c);
    }
}

void tty_print(const char *str) {
    while (*str) {
        tty_putchar(*str++);
    }
}

void tty_clear(void) {
    tty_t *tty = &ttys[current_tty_id];
    tty->log_len = 0;
    tty->log_buf[0] = '\0';
    if (tty->active) {
        term_clear();
    }
}

void tty_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch) {
    term_init(fb_addr, width, height, pitch);

    for (int i = 0; i < MAX_TTYS; i++) {
        ttys[i].id = i;
        ttys[i].active = (i == 0);
        ttys[i].initialized = false;
        ttys[i].line_len = 0;
        ttys[i].history_count = 0;
        ttys[i].history_idx = -1;
        ttys[i].log_len = 0;
        ttys[i].log_buf[0] = '\0';
    }

    current_tty_id = 0;
    ttys[0].initialized = true;

    serial_puts(COM1, "[TTY] Virtual Consoles initialized successfully.\n");
}

void tty_switch(int index) {
    if (index < 0 || index >= MAX_TTYS || index == current_tty_id) return;

    ttys[current_tty_id].active = false;
    current_tty_id = index;

    tty_t *tty = &ttys[current_tty_id];
    tty->active = true;

    // Reset native term.c renderer cleanly
    term_clear();

    if (!tty->initialized) {
        tty->initialized = true;
        tty_print("=== EquantOS Virtual Console TTY");
        char num[4] = {'1' + (char)index, '\n', '\0'};
        tty_print(num);
        shell_init();
    } else {
        // Replay saved output stream into term.c
        term_print(tty->log_buf);
        term_print("EquantOS> ");
        term_print(tty->line_buf);
    }
}

tty_t *tty_get_current(void) {
    return &ttys[current_tty_id];
}

static char input_code_to_ascii(uint16_t code, bool shift) {
    if (shift) {
        switch (code) {
            case KEY_1: return '!'; case KEY_2: return '@'; case KEY_3: return '#';
            case KEY_4: return '$'; case KEY_5: return '%'; case KEY_6: return '^';
            case KEY_7: return '&'; case KEY_8: return '*'; case KEY_9: return '(';
            case KEY_0: return ')'; case KEY_MINUS: return '_'; case KEY_EQUAL: return '+';
            case KEY_Q: return 'Q'; case KEY_W: return 'W'; case KEY_E: return 'E';
            case KEY_R: return 'R'; case KEY_T: return 'T'; case KEY_Y: return 'Y';
            case KEY_U: return 'U'; case KEY_I: return 'I'; case KEY_O: return 'O';
            case KEY_P: return 'P'; case KEY_A: return 'A'; case KEY_S: return 'S';
            case KEY_D: return 'D'; case KEY_F: return 'F'; case KEY_G: return 'G';
            case KEY_H: return 'H'; case KEY_J: return 'J'; case KEY_K: return 'K';
            case KEY_L: return 'L'; case KEY_Z: return 'Z'; case KEY_X: return 'X';
            case KEY_C: return 'C'; case KEY_V: return 'V'; case KEY_B: return 'B';
            case KEY_N: return 'N'; case KEY_M: return 'M'; case KEY_SPACE: return ' ';
            case KEY_DOT: return '>'; case KEY_SLASH: return '?'; case KEY_COMMA: return '<';
            default: return 0;
        }
    }
    switch (code) {
        case KEY_1: return '1'; case KEY_2: return '2'; case KEY_3: return '3';
        case KEY_4: return '4'; case KEY_5: return '5'; case KEY_6: return '6';
        case KEY_7: return '7'; case KEY_8: return '8'; case KEY_9: return '9';
        case KEY_0: return '0'; case KEY_MINUS: return '-'; case KEY_EQUAL: return '=';
        case KEY_Q: return 'q'; case KEY_W: return 'w'; case KEY_E: return 'e';
        case KEY_R: return 'r'; case KEY_T: return 't'; case KEY_Y: return 'y';
        case KEY_U: return 'u'; case KEY_I: return 'i'; case KEY_O: return 'o';
        case KEY_P: return 'p'; case KEY_A: return 'a'; case KEY_S: return 's';
        case KEY_D: return 'd'; case KEY_F: return 'f'; case KEY_G: return 'g';
        case KEY_H: return 'h'; case KEY_J: return 'j'; case KEY_K: return 'k';
        case KEY_L: return 'l'; case KEY_Z: return 'z'; case KEY_X: return 'x';
        case KEY_C: return 'c'; case KEY_V: return 'v'; case KEY_B: return 'b';
        case KEY_N: return 'n'; case KEY_M: return 'm'; case KEY_SPACE: return ' ';
        case KEY_DOT: return '.'; case KEY_SLASH: return '/'; case KEY_COMMA: return ',';
        default: return 0;
    }
}

static void tty_redraw_input_line(tty_t *tty) {
    while (tty->line_len > 0) {
        tty_putchar('\b');
    }
    for (int i = 0; i < tty->line_len; i++) {
        tty_putchar(tty->line_buf[i]);
    }
}

void tty_poll_input(void) {
    input_event_t ev;
    while (input_pop_event(&ev)) {

        // 1. Global Keybinds Check
        if (globalkeybinds_process(&ev)) {
            continue;
        }

        if (ev.type != EV_KEY) continue;

        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            shift_pressed = (ev.value == KEY_PRESS);
            continue;
        }

        if (ev.value != KEY_PRESS) continue;

        tty_t *tty = &ttys[current_tty_id];

        // 2. Command History Up (Arrow Up)
        if (ev.code == KEY_UP) {
            if (tty->history_count > 0 && tty->history_idx < tty->history_count - 1) {
                tty->history_idx++;
                strcpy(tty->line_buf, tty->history[tty->history_count - 1 - tty->history_idx]);
                tty->line_len = strlen(tty->line_buf);
                tty_redraw_input_line(tty);
            }
            continue;
        }

        // 3. Command History Down (Arrow Down)
        if (ev.code == KEY_DOWN) {
            if (tty->history_idx > 0) {
                tty->history_idx--;
                strcpy(tty->line_buf, tty->history[tty->history_count - 1 - tty->history_idx]);
                tty->line_len = strlen(tty->line_buf);
                tty_redraw_input_line(tty);
            } else if (tty->history_idx == 0) {
                tty->history_idx = -1;
                tty->line_buf[0] = '\0';
                tty->line_len = 0;
                tty_redraw_input_line(tty);
            }
            continue;
        }

        // 4. Enter Key Execution
        if (ev.code == KEY_ENTER) {
            tty_putchar('\n');
            tty->line_buf[tty->line_len] = '\0';

            if (tty->line_len > 0) {
                if (tty->history_count < HISTORY_MAX) {
                    strcpy(tty->history[tty->history_count++], tty->line_buf);
                } else {
                    for (int i = 0; i < HISTORY_MAX - 1; i++) {
                        strcpy(tty->history[i], tty->history[i + 1]);
                    }
                    strcpy(tty->history[HISTORY_MAX - 1], tty->line_buf);
                }
            }

            shell_execute(tty->line_buf);
            tty->line_len = 0;
            tty->history_idx = -1;
            continue;
        }

        // 5. Backspace Key
        if (ev.code == KEY_BACKSPACE) {
            if (tty->line_len > 0) {
                tty->line_len--;
                tty->line_buf[tty->line_len] = '\0';
                tty_putchar('\b');
            }
            continue;
        }

        // 6. Regular ASCII Key Output
        char c = input_code_to_ascii(ev.code, shift_pressed);
        if (c != 0 && tty->line_len < TTY_BUF_SIZE - 1) {
            tty->line_buf[tty->line_len++] = c;
            tty->line_buf[tty->line_len] = '\0';
            tty_putchar(c);
        }
    }
}