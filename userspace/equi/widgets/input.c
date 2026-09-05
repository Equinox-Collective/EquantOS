#include "input.h"
#include "../render/font.h"
#include <stdlib.h>
#include <string.h>

static void input_render(surface_t *surf, widget_t *w, int abs_x, int abs_y) {
    input_data_t *data = (input_data_t *)w->priv_data;
    if (!data) return;

    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    uint32_t border = w->focused ? COLOR_RGB(88, 101, 242) : (w->hovered ? COLOR_RGB(80, 85, 100) : COLOR_RGB(50, 54, 66));
    draw_fill_rounded_rect(surf, rx, ry, w->w, w->h, 6, COLOR_RGB(25, 27, 33));
    draw_rounded_rect(surf, rx, ry, w->w, w->h, 6, border);

    int text_y = ry + (w->h - FONT_HEIGHT) / 2;

    if (data->length > 0) {
        font_draw_string(surf, rx + 10, text_y, data->text, COLOR_RGB(255, 255, 255), 1);
    } else {
        font_draw_string(surf, rx + 10, text_y, data->placeholder, COLOR_RGB(100, 105, 120), 1);
    }

    // Caret
    if (w->focused) {
        int cx = rx + 10 + font_get_string_width(data->text, 1);
        draw_fill_rect(surf, cx + 1, ry + 8, 2, w->h - 16, COLOR_RGB(255, 255, 255));
    }
}

static bool input_on_mouse(widget_t *w, int abs_x, int abs_y, int mx, int my, bool down) {
    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    bool inside = widget_contains_point(mx, my, rx, ry, w->w, w->h);
    w->hovered = inside;

    if (inside && down) {
        w->focused = true;
        return true;
    }

    if (!inside && down) {
        w->focused = false;
    }

    return inside;
}

static void input_on_key(widget_t *w, char c, bool is_backspace) {
    input_data_t *data = (input_data_t *)w->priv_data;
    if (!w->focused || !data) return;

    if (is_backspace) {
        if (data->length > 0) {
            data->text[--data->length] = '\0';
        }
    } else if (c >= 32 && c <= 126) {
        if (data->length + 1 < sizeof(data->text)) {
            data->text[data->length++] = c;
            data->text[data->length] = '\0';
        }
    }
}

static void input_destroy(widget_t *w) {
    if (w->priv_data) {
        free(w->priv_data);
        w->priv_data = NULL;
    }
}

static const widget_ops_t input_ops = {
    .render = input_render,
    .on_mouse = input_on_mouse,
    .on_key = input_on_key,
    .destroy = input_destroy
};

widget_t *input_create(int x, int y, int w, int h, const char *placeholder) {
    widget_t *inp = (widget_t *)malloc(sizeof(widget_t));
    input_data_t *data = (input_data_t *)malloc(sizeof(input_data_t));

    memset(data, 0, sizeof(input_data_t));
    strncpy(data->placeholder, placeholder, sizeof(data->placeholder) - 1);

    widget_base_init(inp, x, y, w, h, &input_ops);
    inp->priv_data = data;
    return inp;
}

const char *input_get_text(widget_t *w) {
    if (!w || !w->priv_data) return "";
    return ((input_data_t *)w->priv_data)->text;
}

void input_set_text(widget_t *w, const char *text) {
    if (!w || !w->priv_data || !text) return;
    input_data_t *data = (input_data_t *)w->priv_data;
    strncpy(data->text, text, sizeof(data->text) - 1);
    data->length = strlen(data->text);
}