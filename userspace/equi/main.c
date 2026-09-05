// userspace/equi/main.c - Master UI Showcase Workbench
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "render/draw.h"
#include "render/font.h"
#include "wm/cursor.h"
#include "wm/window.h"

// Modular OOP Widgets
#include "widgets/widget.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/slider.h"
#include "widgets/progress.h"
#include "widgets/input.h"
#include "widgets/label.h"

#define FBIOGET_VSCREENINFO 0x4600

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t bits_per_pixel;
};

typedef struct {
    uint16_t type;
    uint16_t code;
    int32_t value;
} __attribute__((packed)) input_event_t;

#define EV_KEY 0x01
#define EV_REL 0x02
#define REL_X  0x00
#define REL_Y  0x01

#define BTN_LEFT      0x110
#define KEY_BACKSPACE 0x0E
#define KEY_LEFTSHIFT 0x2A
#define KEY_RIGHTSHIFT 0x36

static window_t gallery_win;
static window_t status_win;
static int button_click_count = 0;
static widget_t *dyn_progress_ref = NULL;

static void on_test_button_click(widget_t *w) {
    button_click_count++;
    char buf[64];
    snprintf(buf, sizeof(buf), "Clicked: %d times", button_click_count);
    button_set_text(w, buf);
}

static void on_slider_move(widget_t *w, int new_val) {
    (void)w;
    if (dyn_progress_ref) {
        progress_set_value(dyn_progress_ref, new_val); // Synchronous real-time progress update
    }
}

static char scancode_to_char(uint16_t code, bool shift) {
    if (code >= 0x02 && code <= 0x0B) {
        const char *n = "1234567890";
        const char *s = "!@#$%^&*()";
        return shift ? s[code - 0x02] : n[code - 0x02];
    }
    switch (code) {
        case 0x10: return shift ? 'Q' : 'q';
        case 0x11: return shift ? 'W' : 'w';
        case 0x12: return shift ? 'E' : 'e';
        case 0x13: return shift ? 'R' : 'r';
        case 0x14: return shift ? 'T' : 't';
        case 0x15: return shift ? 'Y' : 'y';
        case 0x16: return shift ? 'U' : 'u';
        case 0x17: return shift ? 'I' : 'i';
        case 0x18: return shift ? 'O' : 'o';
        case 0x19: return shift ? 'P' : 'p';
        case 0x1E: return shift ? 'A' : 'a';
        case 0x1F: return shift ? 'S' : 's';
        case 0x20: return shift ? 'D' : 'd';
        case 0x21: return shift ? 'F' : 'f';
        case 0x22: return shift ? 'G' : 'g';
        case 0x23: return shift ? 'H' : 'h';
        case 0x24: return shift ? 'J' : 'j';
        case 0x25: return shift ? 'K' : 'k';
        case 0x26: return shift ? 'L' : 'l';
        case 0x2C: return shift ? 'Z' : 'z';
        case 0x2D: return shift ? 'X' : 'x';
        case 0x2E: return shift ? 'C' : 'c';
        case 0x2F: return shift ? 'V' : 'v';
        case 0x30: return shift ? 'B' : 'b';
        case 0x31: return shift ? 'N' : 'n';
        case 0x32: return shift ? 'M' : 'm';
        case 0x39: return ' ';
        default: return 0;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return 1;

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fb_fd);
        return 1;
    }

    size_t fb_size = vinfo.xres * vinfo.yres * 4;
    uint32_t *fb_mem = (uint32_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) return 1;

    uint32_t *backbuffer = (uint32_t *)malloc(fb_size);
    if (!backbuffer) return 1;

    surface_t canvas = {
        .buffer = backbuffer,
        .width = vinfo.xres,
        .height = vinfo.yres,
        .pitch = vinfo.xres
    };

    int input_fd = open("/dev/input0", O_RDONLY | O_NONBLOCK);

    // ========================================================================
    // Setup Modular Showcase Windows & Dynamic Widgets
    // ========================================================================
    window_init(&gallery_win, 80, 60, 480, 360, "EquantOS Modular UI Gallery");

    window_add_widget(&gallery_win, label_create(20, 16, "1. Interactive Push Button:", COLOR_RGB(220, 225, 235)));
    window_add_widget(&gallery_win, button_create(20, 38, 200, 32, "Click Me!", on_test_button_click));

    window_add_widget(&gallery_win, label_create(20, 84, "2. Checkbox Component:", COLOR_RGB(220, 225, 235)));
    window_add_widget(&gallery_win, checkbox_create(20, 106, "Enable Hardware Acceleration", true, NULL));

    window_add_widget(&gallery_win, label_create(20, 140, "3. Real-time Slider & Sync Progress:", COLOR_RGB(220, 225, 235)));
    window_add_widget(&gallery_win, slider_create(20, 164, 260, 0, 100, 45, on_slider_move));
    
    dyn_progress_ref = progress_create(295, 158, 160, 24, 45);
    window_add_widget(&gallery_win, dyn_progress_ref);

    window_add_widget(&gallery_win, label_create(20, 204, "4. Editable Text Input Field:", COLOR_RGB(220, 225, 235)));
    window_add_widget(&gallery_win, input_create(20, 226, 435, 36, "Click here and start typing..."));

    // Second Window: System Metrics
    window_init(&status_win, 590, 60, 320, 200, "Kernel & Video Pipeline");
    window_add_widget(&status_win, label_create(16, 20, "Architecture: x86_64 Long Mode", COLOR_RGB(255, 255, 255)));
    window_add_widget(&status_win, label_create(16, 50, "Security: Salted SHA-256 Auth", COLOR_RGB(200, 205, 215)));
    window_add_widget(&status_win, label_create(16, 80, "Renderer: Porter-Duff Alpha Blend", COLOR_RGB(87, 242, 135)));
    window_add_widget(&status_win, label_create(16, 110, "Widgets: Fully Modular C OOP", COLOR_RGB(88, 101, 242)));

    int mouse_x = vinfo.xres / 2;
    int mouse_y = vinfo.yres / 2;
    bool mouse_pressed = false;
    bool shift_active = false;

    // Main Event Loop
    for (;;) {
        if (input_fd >= 0) {
            input_event_t ev;
            while (read(input_fd, &ev, sizeof(input_event_t)) == sizeof(input_event_t)) {
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) mouse_x += ev.value;
                    if (ev.code == REL_Y) mouse_y += ev.value;
                }

                if (ev.type == EV_KEY) {
                    if (ev.code == BTN_LEFT) {
                        mouse_pressed = (ev.value == 1);
                    }
                    if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                        shift_active = (ev.value == 1);
                    }

                    if (ev.value == 1) {
                        char c = scancode_to_char(ev.code, shift_active);
                        bool is_bs = (ev.code == KEY_BACKSPACE);
                        window_handle_key(&gallery_win, c, is_bs);
                        window_handle_key(&status_win, c, is_bs);
                    }
                }
            }
        }

        // Clamp Cursor Bounds
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= (int)vinfo.xres) mouse_x = vinfo.xres - 1;
        if (mouse_y >= (int)vinfo.yres) mouse_y = vinfo.yres - 1;

        // Dispatch Mouse to Windows
        window_handle_mouse(&status_win, mouse_x, mouse_y, mouse_pressed);
        window_handle_mouse(&gallery_win, mouse_x, mouse_y, mouse_pressed);

        // 1. Draw Desktop Background
        draw_clear(&canvas, COLOR_RGB(20, 22, 26));

        // 2. Render Windows
        window_render(&canvas, &status_win);
        window_render(&canvas, &gallery_win);

        // 3. Render Modern Taskbar
        draw_fill_rect(&canvas, 0, canvas.height - 42, canvas.width, 42, COLOR_RGB(28, 30, 36));
        draw_rect(&canvas, 0, canvas.height - 42, canvas.width, 1, COLOR_RGB(45, 48, 56));
        
        draw_fill_rounded_rect(&canvas, 8, canvas.height - 36, 86, 30, 6, COLOR_RGB(88, 101, 242));
        font_draw_string(&canvas, 24, canvas.height - 29, "Equant", COLOR_RGB(255, 255, 255), 1);

        font_draw_string(&canvas, canvas.width - 64, canvas.height - 29, "12:00", COLOR_RGB(200, 205, 215), 1);

        // 4. Render Mouse Pointer
        cursor_draw(&canvas, mouse_x, mouse_y);

        // 5. Flip Framebuffer
        memcpy(fb_mem, backbuffer, fb_size);
    }

    return 0;
}