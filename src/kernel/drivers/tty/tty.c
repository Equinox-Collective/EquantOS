// src/kernel/drivers/tty/tty.c - Extended with Raw Pass-through Mode for GNU Readline / Bash
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

void tty_set_raw_mode(bool enable) {
    ttys[current_tty_id].raw_mode = enable;
}

bool tty_is_raw_mode(void) {
    return ttys[current_tty_id].raw_mode;
}

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
        tty->prompt_x = term_get_cursor_x();
        tty->prompt_y = term_get_cursor_y();
    }
}

void tty_print(const char *str) {
    if (!str) return;
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
        ttys[i].raw_mode = false;
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

    serial_puts(COM1, "[TTY] Line Editor & Raw Passthrough Engine initialized.\n");
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
        if (!tty->raw_mode) {
            term_redraw_input_line(tty->prompt_x, tty->prompt_y, tty->line_buf, tty->cursor_pos);
        }
    }
}

tty_t *tty_get_current(void) {
    return &ttys[current_tty_id];
}

void tty_poll_input(void) {
    tty_t *tty = &ttys[current_tty_id];

    // If terminal is in RAW mode (userspace app running), skip kernel shell intercept
    if (tty->raw_mode) {
        return;
    }

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

        if (ev.code == KEY_LEFT) {
            if (tty->cursor_pos > 0) {
                tty->cursor_pos--;
                tty_refresh_line(tty);
            }
            continue;
        }

        if (ev.code == KEY_RIGHT) {
            if (tty->cursor_pos < tty->line_len) {
                tty->cursor_pos++;
                tty_refresh_line(tty);
            }
            continue;
        }

        if (ev.code == KEY_HOME) {
            tty->cursor_pos = 0;
            tty_refresh_line(tty);
            continue;
        }

        if (ev.code == KEY_END) {
            tty->cursor_pos = tty->line_len;
            tty_refresh_line(tty);
            continue;
        }

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

        char c = input_code_to_ascii(ev.code, shift_pressed);
        if (c != 0 && tty->line_len < TTY_BUF_SIZE - 1) {
            if (tty->cursor_pos < tty->line_len) {
                memmove(&tty->line_buf[tty->cursor_pos + 1],
                        &tty->line_buf[tty->cursor_pos],
                        tty->line_len - tty->cursor_pos + 1);
                tty->line_buf[tty->cursor_pos] = c;
                tty->line_len++;
                tty->cursor_pos++;
                tty->line_buf[tty->line_len] = '\0';
                tty_refresh_line(tty);
            } else {
                tty->line_buf[tty->cursor_pos] = c;
                tty->line_len++;
                tty->cursor_pos++;
                tty->line_buf[tty->line_len] = '\0';
                term_putchar_raw(c);
            }
        }
    }
}