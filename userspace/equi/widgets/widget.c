#include "widget.h"
#include "../render/font.h"
#include <string.h>
#include <stdio.h>

static bool point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

void widget_init_button(widget_t *w, int x, int y, int width, int height, const char *text, void (*on_click)(widget_t *)) {
    memset(w, 0, sizeof(widget_t));
    w->type = WIDGET_BUTTON;
    w->x = x; w->y = y; w->w = width; w->h = height;
    strncpy(w->text, text, sizeof(w->text) - 1);
    w->on_click = on_click;
}

void widget_init_checkbox(widget_t *w, int x, int y, const char *label, bool checked, void (*on_change)(widget_t *, int)) {
    memset(w, 0, sizeof(widget_t));
    w->type = WIDGET_CHECKBOX;
    w->x = x; w->y = y; w->w = 18; w->h = 18;
    w->value = checked ? 1 : 0;
    strncpy(w->text, label, sizeof(w->text) - 1);
    w->on_change = on_change;
}

void widget_init_slider(widget_t *w, int x, int y, int width, int min, int max, int val, void (*on_change)(widget_t *, int)) {
    memset(w, 0, sizeof(widget_t));
    w->type = WIDGET_SLIDER;
    w->x = x; w->y = y; w->w = width; w->h = 16;
    w->min_val = min;
    w->max_val = max;
    w->value = val;
    w->on_change = on_change;
}

void widget_init_progress(widget_t *w, int x, int y, int width, int height, int val) {
    memset(w, 0, sizeof(widget_t));
    w->type = WIDGET_PROGRESS;
    w->x = x; w->y = y; w->w = width; w->h = height;
    w->value = val;
}

void widget_init_input(widget_t *w, int x, int y, int width, int height, const char *placeholder) {
    memset(w, 0, sizeof(widget_t));
    w->type = WIDGET_INPUT;
    w->x = x; w->y = y; w->w = width; w->h = height;
    strncpy(w->text, placeholder, sizeof(w->text) - 1);
}

void widget_init_label(widget_t *w, int x, int y, const char *text) {
    memset(w, 0, sizeof(widget_t));
    w->type = WIDGET_LABEL;
    w->x = x; w->y = y;
    strncpy(w->text, text, sizeof(w->text) - 1);
}

void widget_render(surface_t *surf, widget_t *w, int win_abs_x, int win_abs_y) {
    int rx = win_abs_x + w->x;
    int ry = win_abs_y + w->y;

    switch (w->type) {
        case WIDGET_LABEL:
            font_draw_string(surf, rx, ry, w->text, COLOR_RGB(220, 225, 235), 1);
            break;

        case WIDGET_BUTTON: {
            uint32_t bg = COLOR_RGB(88, 101, 242); // Default Blurple
            if (w->pressed) bg = COLOR_RGB(71, 82, 196);
            else if (w->hovered) bg = COLOR_RGB(105, 117, 245);

            draw_fill_rect(surf, rx, ry, w->w, w->h, bg);
            draw_rect(surf, rx, ry, w->w, w->h, COLOR_RGB(40, 45, 60));

            int tw = font_get_string_width(w->text, 1);
            int tx = rx + (w->w - tw) / 2;
            int ty = ry + (w->h - FONT_HEIGHT) / 2;
            font_draw_string(surf, tx, ty, w->text, COLOR_RGB(255, 255, 255), 1);
            break;
        }

        case WIDGET_CHECKBOX: {
            uint32_t box_bg = w->hovered ? COLOR_RGB(45, 48, 56) : COLOR_RGB(32, 34, 40);
            draw_fill_rect(surf, rx, ry, 18, 18, box_bg);
            draw_rect(surf, rx, ry, 18, 18, w->value ? COLOR_RGB(88, 101, 242) : COLOR_RGB(80, 85, 95));

            if (w->value) {
                // Draw internal active check square
                draw_fill_rect(surf, rx + 4, ry + 4, 10, 10, COLOR_RGB(88, 101, 242));
            }

            font_draw_string(surf, rx + 26, ry + 2, w->text, COLOR_RGB(220, 225, 235), 1);
            break;
        }

        case WIDGET_SLIDER: {
            int track_h = 6;
            int track_y = ry + (w->h - track_h) / 2;
            draw_fill_rect(surf, rx, track_y, w->w, track_h, COLOR_RGB(40, 44, 52));
            draw_rect(surf, rx, track_y, w->w, track_h, COLOR_RGB(25, 27, 32));

            // Fill active part of slider
            float ratio = (float)(w->value - w->min_val) / (float)(w->max_val - w->min_val);
            int fill_w = (int)(ratio * (float)w->w);
            draw_fill_rect(surf, rx, track_y, fill_w, track_h, COLOR_RGB(88, 101, 242));

            // Thumb
            int thumb_x = rx + fill_w - 6;
            draw_fill_rect(surf, thumb_x, ry, 12, w->h, COLOR_RGB(255, 255, 255));
            draw_rect(surf, thumb_x, ry, 12, w->h, COLOR_RGB(150, 150, 160));
            break;
        }

        case WIDGET_PROGRESS: {
            draw_fill_rect(surf, rx, ry, w->w, w->h, COLOR_RGB(32, 35, 42));
            draw_rect(surf, rx, ry, w->w, w->h, COLOR_RGB(50, 54, 66));

            int fill_w = (w->value * w->w) / 100;
            if (fill_w > w->w) fill_w = w->w;
            if (fill_w > 0) {
                draw_fill_rect(surf, rx + 1, ry + 1, fill_w - 2, w->h - 2, COLOR_RGB(87, 242, 135)); // Neon Green
            }

            char pct_str[16];
            snprintf(pct_str, sizeof(pct_str), "%d%%", w->value);
            int tw = font_get_string_width(pct_str, 1);
            font_draw_string(surf, rx + (w->w - tw) / 2, ry + (w->h - FONT_HEIGHT) / 2, pct_str, COLOR_RGB(255, 255, 255), 1);
            break;
        }

        case WIDGET_INPUT: {
            uint32_t border = w->focused ? COLOR_RGB(88, 101, 242) : (w->hovered ? COLOR_RGB(80, 85, 100) : COLOR_RGB(50, 54, 66));
            draw_fill_rect(surf, rx, ry, w->w, w->h, COLOR_RGB(25, 27, 33));
            draw_rect(surf, rx, ry, w->w, w->h, border);

            if (w->input_len > 0) {
                font_draw_string(surf, rx + 8, ry + (w->h - FONT_HEIGHT) / 2, w->input_buf, COLOR_RGB(255, 255, 255), 1);
            } else {
                font_draw_string(surf, rx + 8, ry + (w->h - FONT_HEIGHT) / 2, w->text, COLOR_RGB(100, 105, 120), 1);
            }

            // Blinking/Active caret if focused
            if (w->focused) {
                int cx = rx + 8 + font_get_string_width(w->input_buf, 1);
                draw_fill_rect(surf, cx + 1, ry + 6, 2, w->h - 12, COLOR_RGB(255, 255, 255));
            }
            break;
        }
    }
}

bool widget_handle_mouse(widget_t *w, int win_abs_x, int win_abs_y, int mx, int my, bool mouse_down) {
    int rx = win_abs_x + w->x;
    int ry = win_abs_y + w->y;
    int hit_w = w->w;
    int hit_h = w->h;

    if (w->type == WIDGET_CHECKBOX) {
        hit_w = 18 + font_get_string_width(w->text, 1) + 30;
    }

    bool inside = point_in_rect(mx, my, rx, ry, hit_w, hit_h);
    w->hovered = inside;

    if (inside && mouse_down) {
        w->focused = true;
        w->pressed = true;

        if (w->type == WIDGET_CHECKBOX) {
            w->value = !w->value;
            if (w->on_change) w->on_change(w, w->value);
        } else if (w->type == WIDGET_SLIDER) {
            float ratio = (float)(mx - rx) / (float)w->w;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            w->value = w->min_val + (int)(ratio * (float)(w->max_val - w->min_val));
            if (w->on_change) w->on_change(w, w->value);
        }
        return true;
    }

    if (!mouse_down && w->pressed) {
        w->pressed = false;
        if (inside && w->type == WIDGET_BUTTON && w->on_click) {
            w->on_click(w);
        }
    }

    if (!inside && mouse_down) {
        w->focused = false;
    }

    return inside;
}

void widget_handle_key(widget_t *w, char c, bool is_backspace) {
    if (!w->focused || w->type != WIDGET_INPUT) return;

    if (is_backspace) {
        if (w->input_len > 0) {
            w->input_buf[--w->input_len] = '\0';
        }
    } else if (c >= 32 && c <= 126) {
        if (w->input_len + 1 < sizeof(w->input_buf)) {
            w->input_buf[w->input_len++] = c;
            w->input_buf[w->input_len] = '\0';
        }
    }
}