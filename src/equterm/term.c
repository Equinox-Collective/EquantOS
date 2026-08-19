// src/equterm/term.c - Advanced Terminal with Input Subsystem & Tab Completion
#include "term.h"
#include "../kernel/drivers/serial/serial.h"
#include "../kernel/drivers/input.h"
#include "../kernel/fs/vfs.h"
#include "shell.h"
#include "string.h"
#include "../kernel/drivers/display/font8x8.h"
#include "../kernel/drivers/display/psf2.h"
#include <stdbool.h>

static uint32_t *term_fb_address = NULL;
static uint64_t term_width = 0;
static uint64_t term_height = 0;
static uint64_t term_pitch = 0;

static size_t cursor_x = 0;
static size_t cursor_y = 0;

static uint32_t term_fg_color = 0x00FFFFFF;
static uint32_t default_fg_color = 0x00FFFFFF;

#define CMD_BUF_SIZE 256
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len = 0;
static int cursor_pos = 0;

static bool in_escape = false;
static char esc_buf[16];
static int esc_len = 0;

static int get_glyph_width(void) {
    if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
        return (int)kernel_psf2_font.hdr->width;
    }
    return 8;
}

static int get_glyph_height(void) {
    if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
        return (int)kernel_psf2_font.hdr->height;
    }
    return 8;
}

static int get_line_height(void) {
    return get_glyph_height() + 4;
}

static void term_clear_rect(size_t y0, size_t y1) {
    if (!term_fb_address) return;
    for (size_t y = y0; y < y1 && y < term_height; y++) {
        memset(&term_fb_address[y * term_pitch], 0, term_width * sizeof(uint32_t));
    }
}

static void term_draw_cursor(bool visible) {
    if (!term_fb_address) return;
    int gw = get_glyph_width();
    int gh = get_glyph_height();
    uint32_t color = visible ? term_fg_color : 0x00000000;

    for (int y = gh - 3; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            size_t px = cursor_x + x;
            size_t py = cursor_y + y;
            if (px < term_width && py < term_height) {
                term_fb_address[py * term_pitch + px] = color;
            }
        }
    }
}

void term_clear(void) {
    term_clear_rect(0, term_height);
    cursor_x = 0;
    cursor_y = 0;
}

void term_set_color(uint32_t color) {
    term_fg_color = color;
}

uint32_t term_get_color(void) {
    return term_fg_color;
}

void term_init(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch) {
    term_fb_address = (uint32_t *)fb_addr;
    term_width = width;
    term_height = height;
    term_pitch = pitch / 4;
    term_fg_color = 0x00FFFFFF;
    default_fg_color = 0x00FFFFFF;

    term_clear();
    shell_init();
    term_draw_cursor(true);
}

static void term_scroll(void) {
    if (!term_fb_address) return;

    int lh = get_line_height();
    size_t visible_rows = term_height - lh;

    for (size_t y = 0; y < visible_rows; y++) {
        memcpy(&term_fb_address[y * term_pitch],
               &term_fb_address[(y + lh) * term_pitch],
               term_width * sizeof(uint32_t));
    }

    term_clear_rect(visible_rows, term_height);
}

static void term_advance_line(void) {
    term_draw_cursor(false);
    cursor_x = 0;
    int gh = get_glyph_height();
    int lh = get_line_height();
    if (cursor_y + lh + gh >= term_height) {
        term_scroll();
        cursor_y = term_height - lh;
    } else {
        cursor_y += lh;
    }
    term_draw_cursor(true);
}

static void parse_ansi_color(const char *code) {
    if (strcmp(code, "0") == 0 || strcmp(code, "37") == 0) {
        term_fg_color = default_fg_color;
    } else if (strcmp(code, "31") == 0) {
        term_fg_color = 0x00FF5555;
    } else if (strcmp(code, "32") == 0) {
        term_fg_color = 0x0055FF55;
    } else if (strcmp(code, "33") == 0) {
        term_fg_color = 0x00FFFF55;
    } else if (strcmp(code, "34") == 0) {
        term_fg_color = 0x005555FF;
    } else if (strcmp(code, "36") == 0) {
        term_fg_color = 0x0055FFFF;
    }
}

void term_putchar_raw(char c) {
    if (!term_fb_address) return;

    if (in_escape) {
        if (c == 'm') {
            esc_buf[esc_len] = '\0';
            parse_ansi_color(esc_buf);
            in_escape = false;
            esc_len = 0;
        } else if (esc_len < (int)sizeof(esc_buf) - 1) {
            esc_buf[esc_len++] = c;
        }
        return;
    }

    if (c == 27) {
        in_escape = true;
        esc_len = 0;
        return;
    }

    if (c == '\n') {
        term_advance_line();
        return;
    }

    int gw = get_glyph_width();
    int gh = get_glyph_height();

    term_draw_cursor(false);

    if (c == '\b') {
        if (cursor_x >= (size_t)gw) {
            cursor_x -= (size_t)gw;
            for (size_t y = 0; y < (size_t)gh; y++) {
                for (size_t x = 0; x < (size_t)gw; x++) {
                    size_t px = cursor_x + x;
                    size_t py = cursor_y + y;
                    if (px < term_width && py < term_height) {
                        term_fb_address[py * term_pitch + px] = 0x00000000;
                    }
                }
            }
        }
        term_draw_cursor(true);
        return;
    }

    if (cursor_x + gw >= term_width) {
        term_advance_line();
    }

    if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
        int drawn_width = psf2_draw_char(&kernel_psf2_font, term_fb_address, 
                                         (int)term_width, (int)term_height, 
                                         (int)cursor_x, (int)cursor_y, 
                                         (uint32_t)(unsigned char)c, term_fg_color);
        cursor_x += drawn_width;
    } else {
        if ((unsigned char)c < 128) {
            const uint8_t *glyph = font8x8_basic[(unsigned char)c];
            for (size_t y = 0; y < 8; y++) {
                uint8_t row = glyph[y];
                for (size_t x = 0; x < 8; x++) {
                    if (row & (1 << x)) {
                        size_t px = cursor_x + x;
                        size_t py = cursor_y + y;
                        if (px < term_width && py < term_height) {
                            term_fb_address[py * term_pitch + px] = term_fg_color;
                        }
                    }
                }
            }
        }
        cursor_x += gw;
    }

    term_draw_cursor(true);
}

void term_putchar(char c) {
    serial_putchar(COM1, c);
    term_putchar_raw(c);
}

void term_print(const char *str) {
    while (*str) {
        term_putchar(*str++);
    }
}

static bool shift_pressed = false;

// Convert Input Event Keycode to ASCII Character
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

// Tab Autocompletion Engine
static void term_handle_tab_completion(void) {
    if (cmd_len == 0 || !vfs_root) return;

    vfs_node_t *dir = vfs_root;
    uint32_t idx = 0;
    vfs_node_t *child = NULL;

    while ((child = vfs_readdir(dir, idx++)) != NULL) {
        if (strncmp(child->name, cmd_buf, cmd_len) == 0) {
            // Autocomplete matching prefix
            const char *match = child->name + cmd_len;
            while (*match) {
                if (cmd_len < CMD_BUF_SIZE - 1) {
                    cmd_buf[cmd_len++] = *match;
                    term_putchar(*match);
                }
                match++;
            }
            break;
        }
    }
}

void term_poll_keyboard(void) {
    input_event_t ev;
    if (!input_pop_event(&ev)) return;

    if (ev.type != EV_KEY) return;

    // Отслеживаем зажатие и отпускание Shift
    if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
        shift_pressed = (ev.value == KEY_PRESS);
        return;
    }

    // Обрабатываем нажатия обычных клавиш
    if (ev.value != KEY_PRESS) return;

    if (ev.code == KEY_ENTER) {
        term_draw_cursor(false);
        term_putchar('\n');
        cmd_buf[cmd_len] = '\0';
        
        shell_execute(cmd_buf);
        cmd_len = 0;
        cursor_pos = 0;
        term_draw_cursor(true);
        return;
    }

    if (ev.code == KEY_TAB) {
        term_handle_tab_completion();
        return;
    }

    if (ev.code == KEY_BACKSPACE) {
        if (cmd_len > 0) {
            term_draw_cursor(false);
            cmd_len--;
            term_putchar('\b');
            term_draw_cursor(true);
        }
        return;
    }

    char c = input_code_to_ascii(ev.code, shift_pressed);
    if (c != 0 && cmd_len < CMD_BUF_SIZE - 1) {
        term_draw_cursor(false);
        cmd_buf[cmd_len++] = c;
        term_putchar(c);
        term_draw_cursor(true);
    }
}