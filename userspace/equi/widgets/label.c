#include "label.h"
#include "../render/font.h"
#include <stdlib.h>
#include <string.h>

static void label_render(surface_t *surf, widget_t *w, int abs_x, int abs_y) {
    label_data_t *data = (label_data_t *)w->priv_data;
    if (!data) return;
    font_draw_string(surf, abs_x + w->x, abs_y + w->y, data->text, data->color, 1);
}

static void label_destroy(widget_t *w) {
    if (w->priv_data) {
        free(w->priv_data);
        w->priv_data = NULL;
    }
}

static const widget_ops_t label_ops = {
    .render = label_render,
    .on_mouse = NULL,
    .on_key = NULL,
    .destroy = label_destroy
};

widget_t *label_create(int x, int y, const char *text, uint32_t color) {
    widget_t *w = (widget_t *)malloc(sizeof(widget_t));
    label_data_t *data = (label_data_t *)malloc(sizeof(label_data_t));

    memset(data, 0, sizeof(label_data_t));
    strncpy(data->text, text, sizeof(data->text) - 1);
    data->color = color;

    widget_base_init(w, x, y, font_get_string_width(text, 1), FONT_HEIGHT, &label_ops);
    w->priv_data = data;
    return w;
}

void label_set_text(widget_t *w, const char *text) {
    if (!w || !w->priv_data || !text) return;
    label_data_t *data = (label_data_t *)w->priv_data;
    strncpy(data->text, text, sizeof(data->text) - 1);
    w->w = font_get_string_width(text, 1);
}