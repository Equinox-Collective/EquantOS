// src/kernel/drivers/tty/tty.c - Mid-Line Editor & Full Arrow Navigation
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

static void tty_refresh_line(tty_t *tty) {
    if (tty->active) {
        term_redraw_input_line(tty->prompt_x, tty->prompt_y, tty->line_buf, tty->cursor_pos);
    }
}

static void tty_log_append_char(tty_t *tty, char c) {
    if (tty->log_len < TTY_LOG_SIZE - 1) {
        tty->log_buf[tty->log_len++] = c;
        tty->log_buf[tty->log_len] = '\0';
    } else {
        memmove(tty->log_buf, tty->log_buf + 1024, tty->log_len - 1024);
        tty->log_len -= 1024;
        tty->log_buf[tty->log_len++] = c;
        tty->log_buf[tty->log_len] = '\0';
    }
}

static void tty_replay_log(tty_t *tty) {
    const char *ptr = tty->log_buf;
    while (*ptr) {
        term_putchar_raw(*ptr++);
    }
}

void tty_putchar(char c) {
    tty_t *tty = &ttys[current_tty_id];

    tty_log_append_char(tty, c);
    serial_putchar(COM1, c);

    if (tty->active) {
        term_putchar_raw(c);
        // Track screen position right after prompt
        tty->prompt_x = term_get_cursor_x();
        tty->prompt_y = term_get_cursor_y();
    }
}

void tty_print(const char *str) {
    while (*str) {
        tty_putchar(*str++);
    }
}

void tty_set_color(uint32_t color) {
    if (color == 0x0000FF00 || color == 0x0055FF55) {
        tty_print("\033[32m");
    } else if (color == 0x00FF5555) {
        tty_print("\033[31m");
    } else if (color == 0x00FFFF55) {
        tty_print("\033[33m");
    } else if (color == 0x005555FF) {
        tty_print("\033[34m");
    } else if (color == 0x0055FFFF) {
        tty_print("\033[36m");
    } else {
        tty_print("\033[0m");
    }
}

void tty_clear(void) {
    tty_t *tty = &ttys[current_tty_id];
    tty->log_len = 0;
    tty->log_buf[0] = '\0';
    if (tty->active) {
        term_clear_screen();
        tty->prompt_x = 0;
        tty->prompt_y = 0;
    }
}

void tty_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch) {
    term_init(fb_addr, width, height, pitch);

    for (int i = 0; i < MAX_TTYS; i++) {
        ttys[i].id = i;
        ttys[i].active = (i == 0);
        ttys[i].initialized = false;
        ttys[i].line_len = 0;
        ttys[i].cursor_pos = 0;
        ttys[i].prompt_x = 0;
        ttys[i].prompt_y = 0;
        ttys[i].line_buf[0] = '\0';
        ttys[i].history_count = 0;
        ttys[i].history_idx = -1;
        ttys[i].log_len = 0;
        ttys[i].log_buf[0] = '\0';
    }

    current_tty_id = 0;
    ttys[0].initialized = true;

    serial_puts(COM1, "[TTY] Line Editor & Arrow Subsystem initialized.\n");
}

void tty_switch(int index) {
    if (index < 0 || index >= MAX_TTYS || index == current_tty_id) return;

    ttys[current_tty_id].active = false;
    current_tty_id = index;

    tty_t *tty = &ttys[current_tty_id];
    tty->active = true;

    term_clear_screen();

    if (!tty->initialized) {
        tty->initialized = true;
        shell_init();
    } else {
        tty_replay_log(tty);
        tty->prompt_x = term_get_cursor_x();
        tty->prompt_y = term_get_cursor_y();
        term_redraw_input_line(tty->prompt_x, tty->prompt_y, tty->line_buf, tty->cursor_pos);
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

void tty_poll_input(void) {
    input_event_t ev;
    while (input_pop_event(&ev)) {

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

        // 1. Arrow Left
        if (ev.code == KEY_LEFT) {
            if (tty->cursor_pos > 0) {
                tty->cursor_pos--;
                tty_refresh_line(tty);
            }
            continue;
        }

        // 2. Arrow Right
        if (ev.code == KEY_RIGHT) {
            if (tty->cursor_pos < tty->line_len) {
                tty->cursor_pos++;
                tty_refresh_line(tty);
            }
            continue;
        }

        // 3. Home Key
        if (ev.code == KEY_HOME) {
            tty->cursor_pos = 0;
            tty_refresh_line(tty);
            continue;
        }

        // 4. End Key
        if (ev.code == KEY_END) {
            tty->cursor_pos = tty->line_len;
            tty_refresh_line(tty);
            continue;
        }

        // 5. Arrow Up -> History Prev
        if (ev.code == KEY_UP) {
            if (tty->history_count > 0 && tty->history_idx < tty->history_count - 1) {
                tty->history_idx++;
                strcpy(tty->line_buf, tty->history[tty->history_count - 1 - tty->history_idx]);
                tty->line_len = strlen(tty->line_buf);
                tty->cursor_pos = tty->line_len;
                tty_refresh_line(tty);
            }
            continue;
        }

        // 6. Arrow Down -> History Next
        if (ev.code == KEY_DOWN) {
            if (tty->history_idx > 0) {
                tty->history_idx--;
                strcpy(tty->line_buf, tty->history[tty->history_count - 1 - tty->history_idx]);
                tty->line_len = strlen(tty->line_buf);
                tty->cursor_pos = tty->line_len;
                tty_refresh_line(tty);
            } else if (tty->history_idx == 0) {
                tty->history_idx = -1;
                tty->line_buf[0] = '\0';
                tty->line_len = 0;
                tty->cursor_pos = 0;
                tty_refresh_line(tty);
            }
            continue;
        }

        // 7. Delete Key
        if (ev.code == KEY_DELETE) {
            if (tty->cursor_pos < tty->line_len) {
                memmove(&tty->line_buf[tty->cursor_pos],
                        &tty->line_buf[tty->cursor_pos + 1],
                        tty->line_len - tty->cursor_pos);
                tty->line_len--;
                tty_refresh_line(tty);
            }
            continue;
        }

        // 8. Backspace Key
        if (ev.code == KEY_BACKSPACE) {
            if (tty->cursor_pos > 0) {
                memmove(&tty->line_buf[tty->cursor_pos - 1],
                        &tty->line_buf[tty->cursor_pos],
                        tty->line_len - tty->cursor_pos + 1);
                tty->line_len--;
                tty->cursor_pos--;
                tty_refresh_line(tty);
            }
            continue;
        }

        // 9. Enter Key Execution
        if (ev.code == KEY_ENTER) {
            term_putchar_raw('\n');
            tty->line_buf[tty->line_len] = '\0';

            const char *cmd = tty->line_buf;
            while (*cmd) {
                tty_log_append_char(tty, *cmd++);
            }
            tty_log_append_char(tty, '\n');

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
            tty->cursor_pos = 0;
            tty->line_buf[0] = '\0';
            tty->history_idx = -1;
            continue;
        }

        // 10. Normal ASCII Character Insertion
        char c = input_code_to_ascii(ev.code, shift_pressed);
        if (c != 0 && tty->line_len < TTY_BUF_SIZE - 1) {
            if (tty->cursor_pos < tty->line_len) {
                // Вставка в середину
                memmove(&tty->line_buf[tty->cursor_pos + 1],
                        &tty->line_buf[tty->cursor_pos],
                        tty->line_len - tty->cursor_pos + 1);
                tty->line_buf[tty->cursor_pos] = c;
                tty->line_len++;
                tty->cursor_pos++;
                tty->line_buf[tty->line_len] = '\0';
                tty_refresh_line(tty);
            } else {
                // Обычная печать в конец строки — мгновенный видимый вывод!
                tty->line_buf[tty->cursor_pos] = c;
                tty->line_len++;
                tty->cursor_pos++;
                tty->line_buf[tty->line_len] = '\0';
                term_putchar_raw(c);
            }
        }
    }
}

uint16_t tty_getchar_raw(void) {
    input_event_t ev;
    while (1) {
        if (input_pop_event(&ev)) {
            if (ev.type == EV_KEY && ev.value == KEY_PRESS) {
                return ev.code;
            }
        }
        __asm__ volatile("hlt");
    }
}

void tty_set_colors(uint32_t fg_color, uint32_t bg_color) {
    term_set_custom_colors(fg_color, bg_color);
}