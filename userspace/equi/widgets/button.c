#include "button.h"
#include "../render/font.h"
#include <stdlib.h>
#include <string.h>

static void button_render(surface_t *surf, widget_t *w, int abs_x, int abs_y) {
    button_data_t *data = (button_data_t *)w->priv_data;
    if (!data) return;

    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    uint32_t bg = data->color_normal;
    if (w->pressed) bg = data->color_pressed;
    else if (w->hovered) bg = data->color_hover;

    // Render smooth rounded button
    draw_fill_rounded_rect(surf, rx, ry, w->w, w->h, data->corner_radius, bg);

    // Subtle 3D Top Highlight
    if (!w->pressed) {
        draw_fill_rounded_rect(surf, rx + 1, ry + 1, w->w - 2, 2, 2, COLOR_ARGB(60, 255, 255, 255));
    }

    // Centered Text
    int tw = font_get_string_width(data->text, 1);
    int tx = rx + (w->w - tw) / 2;
    int ty = ry + (w->h - FONT_HEIGHT) / 2;
    if (w->pressed) ty += 1; // Click sink visual effect

    font_draw_string(surf, tx, ty, data->text, COLOR_RGB(255, 255, 255), 1);
}

static bool button_on_mouse(widget_t *w, int abs_x, int abs_y, int mx, int my, bool down) {
    button_data_t *data = (button_data_t *)w->priv_data;
    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    bool inside = widget_contains_point(mx, my, rx, ry, w->w, w->h);
    w->hovered = inside;

    if (inside && down) {
        w->pressed = true;
        w->focused = true;
        return true;
    }

    if (!down && w->pressed) {
        w->pressed = false;
        if (inside && data && data->on_click) {
            data->on_click(w);
        }
        return true;
    }

    return inside;
}

static void button_destroy(widget_t *w) {
    if (w->priv_data) {
        free(w->priv_data);
        w->priv_data = NULL;
    }
}

static const widget_ops_t button_ops = {
    .render = button_render,
    .on_mouse = button_on_mouse,
    .on_key = NULL,
    .destroy = button_destroy
};

widget_t *button_create(int x, int y, int w, int h, const char *text, void (*on_click)(widget_t *)) {
    widget_t *btn = (widget_t *)malloc(sizeof(widget_t));
    button_data_t *data = (button_data_t *)malloc(sizeof(button_data_t));

    memset(data, 0, sizeof(button_data_t));
    strncpy(data->text, text, sizeof(data->text) - 1);
    data->color_normal = COLOR_RGB(88, 101, 242);  // Discord Blurple
    data->color_hover  = COLOR_RGB(105, 117, 245);
    data->color_pressed= COLOR_RGB(71, 82, 196);
    data->corner_radius = 6;
    data->on_click = on_click;

    widget_base_init(btn, x, y, w, h, &button_ops);
    btn->priv_data = data;
    return btn;
}