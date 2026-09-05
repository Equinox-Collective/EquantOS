#include "checkbox.h"
#include "../render/font.h"
#include <stdlib.h>
#include <string.h>

#define CHECKBOX_SIZE 18

static void checkbox_render(surface_t *surf, widget_t *w, int abs_x, int abs_y) {
    checkbox_data_t *data = (checkbox_data_t *)w->priv_data;
    if (!data) return;

    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    uint32_t box_col = data->checked ? COLOR_RGB(88, 101, 242) : COLOR_RGB(40, 44, 52);
    if (w->hovered && !data->checked) box_col = COLOR_RGB(55, 60, 70);

    // Rounded checkbox box
    draw_fill_rounded_rect(surf, rx, ry, CHECKBOX_SIZE, CHECKBOX_SIZE, 4, box_col);
    draw_fill_rounded_rect(surf, rx + 1, ry + 1, CHECKBOX_SIZE - 2, CHECKBOX_SIZE - 2, 3, 
                           data->checked ? COLOR_RGB(88, 101, 242) : COLOR_RGB(25, 27, 33));

    // Internal check glyph
    if (data->checked) {
        draw_fill_rounded_rect(surf, rx + 4, ry + 4, 10, 10, 2, COLOR_RGB(255, 255, 255));
    }

    // Label Text
    font_draw_string(surf, rx + CHECKBOX_SIZE + 10, ry + 2, data->text, COLOR_RGB(220, 225, 235), 1);
}

static bool checkbox_on_mouse(widget_t *w, int abs_x, int abs_y, int mx, int my, bool down) {
    checkbox_data_t *data = (checkbox_data_t *)w->priv_data;
    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    int total_width = CHECKBOX_SIZE + 10 + font_get_string_width(data->text, 1);
    bool inside = widget_contains_point(mx, my, rx, ry, total_width, CHECKBOX_SIZE);
    w->hovered = inside;

    if (inside && down && !w->pressed) {
        w->pressed = true;
        data->checked = !data->checked;
        if (data->on_change) {
            data->on_change(w, data->checked);
        }
        return true;
    }

    if (!down) {
        w->pressed = false;
    }

    return inside;
}

static void checkbox_destroy(widget_t *w) {
    if (w->priv_data) {
        free(w->priv_data);
        w->priv_data = NULL;
    }
}

static const widget_ops_t checkbox_ops = {
    .render = checkbox_render,
    .on_mouse = checkbox_on_mouse,
    .on_key = NULL,
    .destroy = checkbox_destroy
};

widget_t *checkbox_create(int x, int y, const char *text, bool initial_val, void (*on_change)(widget_t *, bool)) {
    widget_t *cb = (widget_t *)malloc(sizeof(widget_t));
    checkbox_data_t *data = (checkbox_data_t *)malloc(sizeof(checkbox_data_t));

    memset(data, 0, sizeof(checkbox_data_t));
    strncpy(data->text, text, sizeof(data->text) - 1);
    data->checked = initial_val;
    data->on_change = on_change;

    widget_base_init(cb, x, y, CHECKBOX_SIZE, CHECKBOX_SIZE, &checkbox_ops);
    cb->priv_data = data;
    return cb;
}