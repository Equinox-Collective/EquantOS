// userspace/equi/main.c - Comprehensive Interactive GUI Showcase & Testbed
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

#define BTN_LEFT       0x110
#define BTN_RIGHT      0x111
#define KEY_BACKSPACE  0x0E
#define KEY_ENTER      0x1C
#define KEY_LEFTSHIFT  0x2A
#define KEY_RIGHTSHIFT 0x36

// ============================================================================
// Interactive Component States
// ============================================================================

typedef struct {
    int x, y, w, h;
    char title[64];
    bool is_dragging;
    int drag_off_x;
    int drag_off_y;
    bool closed;
} gui_window_t;

static gui_window_t main_win = {
    .x = 80, .y = 60, .w = 520, .h = 420,
    .title = "EquantOS Interactive Component Showcase",
    .is_dragging = false, .closed = false
};

static gui_window_t info_win = {
    .x = 640, .y = 60, .w = 320, .h = 260,
    .title = "System Metrics & Input State",
    .is_dragging = false, .closed = false
};

// Controls State
static int button_clicks = 0;
static bool checkbox_checked = true;
static int radio_selected = 0; // 0 = Discord Blurple, 1 = Emerald Green, 2 = Crimson Red
static int slider_value = 65;  // 0 - 100%
static bool slider_dragging = false;

static char text_input_buf[64] = "Hello, EquantOS!";
static size_t text_input_len = 16;
static bool text_input_focused = false;

static int progress_val = 0;
static bool start_menu_open = false;

// Helpers
static inline bool point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return (px >= rx && px < (rx + rw) && py >= ry && py < (ry + rh));
}

static char scancode_to_char(uint16_t code, bool shift) {
    if (code >= 0x02 && code <= 0x0B) {
        const char *num = "1234567890";
        const char *sym = "!@#$%^&*()";
        return shift ? sym[code - 0x02] : num[code - 0x02];
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
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x0C: return shift ? '_' : '-';
        case 0x0D: return shift ? '+' : '=';
        default: return 0;
    }
}

// ============================================================================
// Drawing Helpers
// ============================================================================

static void draw_window_frame(surface_t *canvas, gui_window_t *win) {
    if (win->closed) return;

    // Drop Shadow (Translucent Black Layer around window)
    draw_fill_rect(canvas, win->x + 6, win->y + 6, win->w, win->h, COLOR_ARGB(90, 0, 0, 0));

    // Titlebar
    uint32_t title_color = win->is_dragging ? COLOR_RGB(60, 64, 75) : COLOR_RGB(42, 44, 52);
    draw_fill_rect(canvas, win->x, win->y, win->w, 36, title_color);
    font_draw_string(canvas, win->x + 14, win->y + 10, win->title, COLOR_RGB(240, 240, 245), 1);

    // Close Button (Red Circle/Square)
    draw_fill_rect(canvas, win->x + win->w - 28, win->y + 10, 16, 16, COLOR_RGB(237, 66, 69));

    // Window Body
    draw_fill_rect(canvas, win->x, win->y + 36, win->w, win->h - 36, COLOR_RGB(48, 51, 60));
    draw_rect(canvas, win->x, win->y, win->w, win->h, COLOR_RGB(30, 32, 38));
}

// ============================================================================
// Main Compositor Engine
// ============================================================================

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
    if (fb_mem == MAP_FAILED) {
        close(fb_fd);
        return 1;
    }

    uint32_t *backbuffer = (uint32_t *)malloc(fb_size);
    if (!backbuffer) return 1;

    surface_t canvas = {
        .buffer = backbuffer,
        .width = vinfo.xres,
        .height = vinfo.yres,
        .pitch = vinfo.xres
    };

    int input_fd = open("/dev/input0", O_RDONLY | O_NONBLOCK);

    int mouse_x = vinfo.xres / 2;
    int mouse_y = vinfo.yres / 2;
    bool btn_left = false;
    bool btn_left_prev = false;
    bool shift_held = false;

    uint32_t frame_counter = 0;

    for (;;) {
        frame_counter++;
        btn_left_prev = btn_left;

        // 1. Process Input Stream
        if (input_fd >= 0) {
            input_event_t ev;
            while (read(input_fd, &ev, sizeof(input_event_t)) == sizeof(input_event_t)) {
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) mouse_x += ev.value;
                    if (ev.code == REL_Y) mouse_y += ev.value;
                } else if (ev.type == EV_KEY) {
                    if (ev.code == BTN_LEFT) {
                        btn_left = (ev.value == 1);
                    } else if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                        shift_held = (ev.value == 1);
                    } else if (ev.value == 1 && text_input_focused) {
                        if (ev.code == KEY_BACKSPACE) {
                            if (text_input_len > 0) {
                                text_input_buf[--text_input_len] = '\0';
                            }
                        } else {
                            char c = scancode_to_char(ev.code, shift_held);
                            if (c != 0 && text_input_len + 1 < sizeof(text_input_buf)) {
                                text_input_buf[text_input_len++] = c;
                                text_input_buf[text_input_len] = '\0';
                            }
                        }
                    }
                }
            }
        }

        // Clamp Cursor
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= (int)vinfo.xres) mouse_x = vinfo.xres - 1;
        if (mouse_y >= (int)vinfo.yres) mouse_y = vinfo.yres - 1;

        bool click_down = (btn_left && !btn_left_prev);
        bool click_up   = (!btn_left && btn_left_prev);

        // 2. Handle Window Dragging & Interactions
        // Check Main Window Drag
        if (!main_win.closed) {
            if (click_down && point_in_rect(mouse_x, mouse_y, main_win.x, main_win.y, main_win.w - 36, 36)) {
                main_win.is_dragging = true;
                main_win.drag_off_x = mouse_x - main_win.x;
                main_win.drag_off_y = mouse_y - main_win.y;
            }
            if (click_down && point_in_rect(mouse_x, mouse_y, main_win.x + main_win.w - 28, main_win.y + 10, 16, 16)) {
                main_win.closed = true;
            }
        }
        if (main_win.is_dragging) {
            if (btn_left) {
                main_win.x = mouse_x - main_win.drag_off_x;
                main_win.y = mouse_y - main_win.drag_off_y;
            } else {
                main_win.is_dragging = false;
            }
        }

        // Check Info Window Drag
        if (!info_win.closed) {
            if (click_down && point_in_rect(mouse_x, mouse_y, info_win.x, info_win.y, info_win.w - 36, 36)) {
                info_win.is_dragging = true;
                info_win.drag_off_x = mouse_x - info_win.x;
                info_win.drag_off_y = mouse_y - info_win.y;
            }
            if (click_down && point_in_rect(mouse_x, mouse_y, info_win.x + info_win.w - 28, info_win.y + 10, 16, 16)) {
                info_win.closed = true;
            }
        }
        if (info_win.is_dragging) {
            if (btn_left) {
                info_win.x = mouse_x - info_win.drag_off_x;
                info_win.y = mouse_y - info_win.drag_off_y;
            } else {
                info_win.is_dragging = false;
            }
        }

        // Update Animation
        if ((frame_counter % 2) == 0) {
            progress_val = (progress_val + 1) % 101;
        }

        // 3. Clear Background (Modern Mesh Pattern)
        draw_clear(&canvas, COLOR_RGB(20, 22, 27));

        // Subtle desktop grid lines
        for (int y = 0; y < (int)vinfo.yres; y += 40) {
            draw_fill_rect(&canvas, 0, y, vinfo.xres, 1, COLOR_RGB(26, 28, 34));
        }
        for (int x = 0; x < (int)vinfo.xres; x += 40) {
            draw_fill_rect(&canvas, x, 0, 1, vinfo.yres, COLOR_RGB(26, 28, 34));
        }

        // 4. Render Main Window Components
        if (!main_win.closed) {
            draw_window_frame(&canvas, &main_win);

            int cx = main_win.x + 24;
            int cy = main_win.y + 56;

            // SECTION 1: BUTTON TEST
            font_draw_string(&canvas, cx, cy, "1. Push Button with Counter:", COLOR_RGB(200, 205, 215), 1);
            cy += 20;

            int btn_x = cx, btn_y = cy, btn_w = 160, btn_h = 32;
            bool btn_hover = point_in_rect(mouse_x, mouse_y, btn_x, btn_y, btn_w, btn_h);
            if (click_down && btn_hover) {
                button_clicks++;
            }

            uint32_t btn_col = btn_hover ? (btn_left ? COLOR_RGB(71, 82, 196) : COLOR_RGB(105, 117, 245))
                                         : COLOR_RGB(88, 101, 242);
            draw_fill_rect(&canvas, btn_x, btn_y, btn_w, btn_h, btn_col);
            draw_rect(&canvas, btn_x, btn_y, btn_w, btn_h, COLOR_RGB(40, 40, 50));
            
            char btn_lbl[32];
            snprintf(btn_lbl, sizeof(btn_lbl), "Clicks: %d", button_clicks);
            font_draw_string(&canvas, btn_x + 36, btn_y + 8, btn_lbl, COLOR_RGB(255, 255, 255), 1);

            // SECTION 2: CHECKBOX TEST
            cy += 46;
            font_draw_string(&canvas, cx, cy, "2. Toggle Checkbox:", COLOR_RGB(200, 205, 215), 1);
            cy += 20;

            int chk_x = cx, chk_y = cy, chk_box_s = 18;
            bool chk_hover = point_in_rect(mouse_x, mouse_y, chk_x, chk_y, 220, chk_box_s);
            if (click_down && chk_hover) {
                checkbox_checked = !checkbox_checked;
            }

            draw_fill_rect(&canvas, chk_x, chk_y, chk_box_s, chk_box_s, COLOR_RGB(34, 36, 44));
            draw_rect(&canvas, chk_x, chk_y, chk_box_s, chk_box_s, COLOR_RGB(90, 95, 110));
            if (checkbox_checked) {
                draw_fill_rect(&canvas, chk_x + 4, chk_y + 4, chk_box_s - 8, chk_box_s - 8, COLOR_RGB(87, 242, 135));
            }
            font_draw_string(&canvas, chk_x + 28, chk_y + 2, "Enable High-DPI Anti-Aliasing", COLOR_RGB(220, 220, 230), 1);

            // SECTION 3: RADIO BUTTONS (Theme Selector)
            cy += 36;
            font_draw_string(&canvas, cx, cy, "3. Radio Buttons (Accent Color):", COLOR_RGB(200, 205, 215), 1);
            cy += 20;

            const char *radio_labels[3] = { "Blurple Theme", "Emerald Theme", "Crimson Theme" };
            uint32_t radio_accents[3]   = { COLOR_RGB(88, 101, 242), COLOR_RGB(87, 242, 135), COLOR_RGB(237, 66, 69) };

            for (int r = 0; r < 3; r++) {
                int rx = cx + (r * 150), ry = cy, rs = 16;
                bool r_hover = point_in_rect(mouse_x, mouse_y, rx, ry, 140, rs);
                if (click_down && r_hover) {
                    radio_selected = r;
                }

                draw_fill_rect(&canvas, rx, ry, rs, rs, COLOR_RGB(34, 36, 44));
                draw_rect(&canvas, rx, ry, rs, rs, (radio_selected == r) ? radio_accents[r] : COLOR_RGB(90, 95, 110));
                if (radio_selected == r) {
                    draw_fill_rect(&canvas, rx + 4, ry + 4, rs - 8, rs - 8, radio_accents[r]);
                }
                font_draw_string(&canvas, rx + 22, ry + 1, radio_labels[r], COLOR_RGB(210, 215, 225), 1);
            }

            // SECTION 4: SLIDER (Draggable Range)
            cy += 36;
            char slider_txt[64];
            snprintf(slider_txt, sizeof(slider_txt), "4. Interactive Slider: %d%%", slider_value);
            font_draw_string(&canvas, cx, cy, slider_txt, COLOR_RGB(200, 205, 215), 1);
            cy += 20;

            int s_bar_x = cx, s_bar_y = cy + 6, s_bar_w = 320, s_bar_h = 8;
            if (click_down && point_in_rect(mouse_x, mouse_y, s_bar_x - 10, s_bar_y - 10, s_bar_w + 20, 28)) {
                slider_dragging = true;
            }
            if (!btn_left) {
                slider_dragging = false;
            }
            if (slider_dragging) {
                int rel = mouse_x - s_bar_x;
                if (rel < 0) rel = 0;
                if (rel > s_bar_w) rel = s_bar_w;
                slider_value = (rel * 100) / s_bar_w;
            }

            // Track Background
            draw_fill_rect(&canvas, s_bar_x, s_bar_y, s_bar_w, s_bar_h, COLOR_RGB(34, 36, 44));
            // Fill
            int filled_w = (s_bar_w * slider_value) / 100;
            draw_fill_rect(&canvas, s_bar_x, s_bar_y, filled_w, s_bar_h, radio_accents[radio_selected]);
            // Thumb knob
            draw_fill_rect(&canvas, s_bar_x + filled_w - 6, s_bar_y - 5, 12, 18, COLOR_RGB(255, 255, 255));
            draw_rect(&canvas, s_bar_x + filled_w - 6, s_bar_y - 5, 12, 18, COLOR_RGB(0, 0, 0));

            // SECTION 5: TEXT INPUT FIELD
            cy += 36;
            font_draw_string(&canvas, cx, cy, "5. Editable Text Input (Click to type):", COLOR_RGB(200, 205, 215), 1);
            cy += 20;

            int in_x = cx, in_y = cy, in_w = 340, in_h = 32;
            if (click_down) {
                text_input_focused = point_in_rect(mouse_x, mouse_y, in_x, in_y, in_w, in_h);
            }

            uint32_t in_border = text_input_focused ? radio_accents[radio_selected] : COLOR_RGB(70, 75, 90);
            draw_fill_rect(&canvas, in_x, in_y, in_w, in_h, COLOR_RGB(30, 32, 40));
            draw_rect(&canvas, in_x, in_y, in_w, in_h, in_border);

            font_draw_string(&canvas, in_x + 10, in_y + 8, text_input_buf, COLOR_RGB(255, 255, 255), 1);

            // Blinking cursor in text box
            if (text_input_focused && ((frame_counter / 15) % 2 == 0)) {
                int cur_px = in_x + 10 + font_get_string_width(text_input_buf, 1);
                draw_fill_rect(&canvas, cur_px + 2, in_y + 6, 2, 20, COLOR_RGB(255, 255, 255));
            }

            // SECTION 6: ANIMATED PROGRESS BAR
            cy += 46;
            char p_txt[64];
            snprintf(p_txt, sizeof(p_txt), "6. Live Loop Progress Bar: %d%%", progress_val);
            font_draw_string(&canvas, cx, cy, p_txt, COLOR_RGB(200, 205, 215), 1);
            cy += 18;

            int p_bar_x = cx, p_bar_y = cy, p_bar_w = 460, p_bar_h = 16;
            draw_fill_rect(&canvas, p_bar_x, p_bar_y, p_bar_w, p_bar_h, COLOR_RGB(34, 36, 44));
            draw_rect(&canvas, p_bar_x, p_bar_y, p_bar_w, p_bar_h, COLOR_RGB(60, 64, 75));
            int p_fill = (p_bar_w * progress_val) / 100;
            draw_fill_rect(&canvas, p_bar_x + 2, p_bar_y + 2, p_fill, p_bar_h - 4, radio_accents[radio_selected]);
        }

        // 5. Render Info & Metrics Window
        if (!info_win.closed) {
            draw_window_frame(&canvas, &info_win);

            int ix = info_win.x + 18;
            int iy = info_win.y + 48;

            char buf[64];
            snprintf(buf, sizeof(buf), "Resolution : %ux%u", vinfo.xres, vinfo.yres);
            font_draw_string(&canvas, ix, iy, buf, COLOR_RGB(220, 225, 235), 1);

            iy += 24;
            snprintf(buf, sizeof(buf), "Cursor Pos : X:%-4d Y:%-4d", mouse_x, mouse_y);
            font_draw_string(&canvas, ix, iy, buf, COLOR_RGB(220, 225, 235), 1);

            iy += 24;
            snprintf(buf, sizeof(buf), "Mouse Left : %s", btn_left ? "PRESSED [DOWN]" : "RELEASED [UP]");
            font_draw_string(&canvas, ix, iy, buf, btn_left ? COLOR_RGB(87, 242, 135) : COLOR_RGB(160, 165, 175), 1);

            iy += 24;
            snprintf(buf, sizeof(buf), "Active User: root (UID 0)");
            font_draw_string(&canvas, ix, iy, buf, COLOR_RGB(237, 66, 69), 1);

            iy += 24;
            snprintf(buf, sizeof(buf), "Render Mode: Double-Buffered");
            font_draw_string(&canvas, ix, iy, buf, COLOR_RGB(88, 101, 242), 1);

            iy += 24;
            snprintf(buf, sizeof(buf), "Frame Clock: #%u", frame_counter);
            font_draw_string(&canvas, ix, iy, buf, COLOR_RGB(140, 145, 160), 1);
        }

        // 6. Render Taskbar
        int tb_h = 42;
        int tb_y = vinfo.yres - tb_h;
        draw_fill_rect(&canvas, 0, tb_y, vinfo.xres, tb_h, COLOR_RGB(28, 30, 36));
        draw_rect(&canvas, 0, tb_y, vinfo.xres, 1, COLOR_RGB(45, 48, 56));

        // Start Button
        bool start_hover = point_in_rect(mouse_x, mouse_y, 8, tb_y + 5, 90, 32);
        if (click_down && start_hover) {
            start_menu_open = !start_menu_open;
        }
        uint32_t start_col = start_hover ? COLOR_RGB(105, 117, 245) : COLOR_RGB(88, 101, 242);
        draw_fill_rect(&canvas, 8, tb_y + 5, 90, 32, start_col);
        font_draw_string(&canvas, 24, tb_y + 13, "Equant", COLOR_RGB(255, 255, 255), 1);

        // Taskbar Clock & Date
        font_draw_string(&canvas, vinfo.xres - 90, tb_y + 13, "12:00:00", COLOR_RGB(220, 220, 220), 1);

        // 7. Render Start Menu Popup if open
        if (start_menu_open) {
            int sm_x = 8, sm_w = 200, sm_h = 160, sm_y = tb_y - sm_h - 4;
            draw_fill_rect(&canvas, sm_x + 4, sm_y + 4, sm_w, sm_h, COLOR_ARGB(120, 0, 0, 0)); // Shadow
            draw_fill_rect(&canvas, sm_x, sm_y, sm_w, sm_h, COLOR_RGB(36, 38, 46));
            draw_rect(&canvas, sm_x, sm_y, sm_w, sm_h, COLOR_RGB(60, 64, 76));

            font_draw_string(&canvas, sm_x + 14, sm_y + 12, "EquantOS Menu", COLOR_RGB(88, 101, 242), 1);
            draw_fill_rect(&canvas, sm_x + 10, sm_y + 32, sm_w - 20, 1, COLOR_RGB(50, 54, 66));

            const char *menu_items[4] = { "1. Reopen Windows", "2. Open Terminal", "3. System Settings", "4. Lock / Logout" };
            for (int m = 0; m < 4; m++) {
                int mi_y = sm_y + 40 + (m * 28);
                bool mi_h = point_in_rect(mouse_x, mouse_y, sm_x + 4, mi_y, sm_w - 8, 24);
                if (mi_h) {
                    draw_fill_rect(&canvas, sm_x + 4, mi_y, sm_w - 8, 24, COLOR_RGB(55, 60, 75));
                    if (click_down && m == 0) {
                        main_win.closed = false;
                        info_win.closed = false;
                        start_menu_open = false;
                    }
                }
                font_draw_string(&canvas, sm_x + 14, mi_y + 5, menu_items[m], COLOR_RGB(220, 225, 235), 1);
            }
        }

        // 8. Render Arrow Cursor
        cursor_draw(&canvas, mouse_x, mouse_y);

        // 9. High-Speed Flip to Screen
        memcpy(fb_mem, backbuffer, fb_size);
    }

    return 0;
}