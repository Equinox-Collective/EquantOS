#include "slider.h"
#include <stdlib.h>
#include <string.h>

#define SLIDER_HEIGHT 16

static void slider_render(surface_t *surf, widget_t *w, int abs_x, int abs_y) {
    slider_data_t *data = (slider_data_t *)w->priv_data;
    if (!data) return;

    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    int track_h = 6;
    int track_y = ry + (w->h - track_h) / 2;

    // Track background
    draw_fill_rounded_rect(surf, rx, track_y, w->w, track_h, 3, COLOR_RGB(36, 39, 47));

    // Filled progress track
    float ratio = (float)(data->value - data->min_val) / (float)(data->max_val - data->min_val);
    int fill_w = (int)(ratio * (float)w->w);
    if (fill_w > 0) {
        draw_fill_rounded_rect(surf, rx, track_y, fill_w, track_h, 3, COLOR_RGB(88, 101, 242));
    }

    // Draggable circular/rounded thumb
    int thumb_x = rx + fill_w - 6;
    draw_fill_circle(surf, thumb_x + 6, ry + (w->h / 2), 6, COLOR_RGB(255, 255, 255));
    draw_fill_circle(surf, thumb_x + 6, ry + (w->h / 2), 3, COLOR_RGB(88, 101, 242));
}

static bool slider_on_mouse(widget_t *w, int abs_x, int abs_y, int mx, int my, bool down) {
    slider_data_t *data = (slider_data_t *)w->priv_data;
    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    bool inside = widget_contains_point(mx, my, rx, ry, w->w, w->h);
    w->hovered = inside;

    if ((inside && down) || (w->pressed && down)) {
        w->pressed = true;
        w->focused = true;

        float ratio = (float)(mx - rx) / (float)w->w;
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;

        data->value = data->min_val + (int)(ratio * (float)(data->max_val - data->min_val));
        if (data->on_change) {
            data->on_change(w, data->value);
        }
        return true;
    }

    if (!down) {
        w->pressed = false;
    }

    return inside;
}

static void slider_destroy(widget_t *w) {
    if (w->priv_data) {
        free(w->priv_data);
        w->priv_data = NULL;
    }
}

static const widget_ops_t slider_ops = {
    .render = slider_render,
    .on_mouse = slider_on_mouse,
    .on_key = NULL,
    .destroy = slider_destroy
};

widget_t *slider_create(int x, int y, int w, int min, int max, int val, void (*on_change)(widget_t *, int)) {
    widget_t *slider = (widget_t *)malloc(sizeof(widget_t));
    slider_data_t *data = (slider_data_t *)malloc(sizeof(slider_data_t));

    memset(data, 0, sizeof(slider_data_t));
    data->min_val = min;
    data->max_val = max;
    data->value = val;
    data->on_change = on_change;

    widget_base_init(slider, x, y, w, SLIDER_HEIGHT, &slider_ops);
    slider->priv_data = data;
    return slider;
}

int slider_get_value(widget_t *w) {
    if (!w || !w->priv_data) return 0;
    return ((slider_data_t *)w->priv_data)->value;
}

void slider_set_value(widget_t *w, int val) {
    if (!w || !w->priv_data) return;
    slider_data_t *data = (slider_data_t *)w->priv_data;
    if (val < data->min_val) val = data->min_val;
    if (val > data->max_val) val = data->max_val;
    data->value = val;
}