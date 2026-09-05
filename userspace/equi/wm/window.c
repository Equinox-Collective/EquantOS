#include "window.h"
#include "../render/font.h"
#include <string.h>

void window_init(window_t *win, int x, int y, int w, int h, const char *title) {
    memset(win, 0, sizeof(window_t));
    win->x = x; win->y = y; win->w = w; win->h = h;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->widget_count = 0;
}

void window_add_widget(window_t *win, widget_t w) {
    if (win->widget_count < MAX_WINDOW_WIDGETS) {
        win->widgets[win->widget_count++] = w;
    }
}

void window_render(surface_t *surf, window_t *win) {
    if (win->is_closed) return;

    // 1. Window Drop Shadow (Simple Ambient Shadow)
    draw_fill_rect(surf, win->x + 4, win->y + 4, win->w, win->h, COLOR_ARGB(100, 10, 10, 15));

    // 2. Window Titlebar
    uint32_t title_color = win->is_focused ? COLOR_RGB(42, 45, 54) : COLOR_RGB(32, 34, 40);
    draw_fill_rect(surf, win->x, win->y, win->w, TITLEBAR_HEIGHT, title_color);
    font_draw_string(surf, win->x + 12, win->y + (TITLEBAR_HEIGHT - FONT_HEIGHT) / 2, win->title, COLOR_RGB(255, 255, 255), 1);

    // Close Button (Red Mac/Unix style circle/box)
    draw_fill_rect(surf, win->x + win->w - 24, win->y + 8, 16, 16, COLOR_RGB(237, 66, 69));

    // 3. Window Body
    draw_fill_rect(surf, win->x, win->y + TITLEBAR_HEIGHT, win->w, win->h - TITLEBAR_HEIGHT, COLOR_RGB(50, 54, 62));
    draw_rect(surf, win->x, win->y, win->w, win->h, COLOR_RGB(25, 27, 32));

    // 4. Render Child Widgets
    int body_y = win->y + TITLEBAR_HEIGHT;
    for (int i = 0; i < win->widget_count; i++) {
        widget_render(surf, &win->widgets[i], win->x, body_y);
    }
}

bool window_handle_mouse(window_t *win, int mx, int my, bool mouse_down) {
    if (win->is_closed) return false;

    // 1. Titlebar Hit Testing & Dragging
    if (mouse_down) {
        if (!win->is_dragging && mx >= win->x && mx < (win->x + win->w) && my >= win->y && my < (win->y + TITLEBAR_HEIGHT)) {
            // Check Close button
            if (mx >= (win->x + win->w - 26) && mx < (win->x + win->w - 8)) {
                win->is_closed = true;
                return true;
            }
            win->is_dragging = true;
            win->drag_off_x = mx - win->x;
            win->drag_off_y = my - win->y;
            win->is_focused = true;
            return true;
        }
    } else {
        win->is_dragging = false;
    }

    if (win->is_dragging) {
        win->x = mx - win->drag_off_x;
        win->y = my - win->drag_off_y;
        return true;
    }

    // 2. Dispatch to Widgets inside Window Body
    int body_y = win->y + TITLEBAR_HEIGHT;
    bool widget_consumed = false;
    for (int i = 0; i < win->widget_count; i++) {
        if (widget_handle_mouse(&win->widgets[i], win->x, body_y, mx, my, mouse_down)) {
            widget_consumed = true;
        }
    }

    bool inside_window = (mx >= win->x && mx < win->x + win->w && my >= win->y && my < win->y + win->h);
    if (inside_window && mouse_down) {
        win->is_focused = true;
    }

    return (inside_window || widget_consumed);
}

void window_handle_key(window_t *win, char c, bool is_backspace) {
    if (!win->is_focused || win->is_closed) return;
    for (int i = 0; i < win->widget_count; i++) {
        widget_handle_key(&win->widgets[i], c, is_backspace);
    }
}