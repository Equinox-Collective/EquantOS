#include "progress.h"
#include "../render/font.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void progress_render(surface_t *surf, widget_t *w, int abs_x, int abs_y) {
    progress_data_t *data = (progress_data_t *)w->priv_data;
    if (!data) return;

    int rx = abs_x + w->x;
    int ry = abs_y + w->y;

    // Track Background
    draw_fill_rounded_rect(surf, rx, ry, w->w, w->h, 6, COLOR_RGB(32, 35, 42));

    // Green Active Fill
    int fill_w = (data->value * w->w) / 100;
    if (fill_w > w->w) fill_w = w->w;
    if (fill_w > 0) {
        draw_fill_rounded_rect(surf, rx, ry, fill_w, w->h, 6, COLOR_RGB(87, 242, 135));
    }

    // Centered Percentage String
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", data->value);
    int tw = font_get_string_width(pct_str, 1);
    int tx = rx + (w->w - tw) / 2;
    int ty = ry + (w->h - FONT_HEIGHT) / 2;
    font_draw_string(surf, tx, ty, pct_str, COLOR_RGB(20, 20, 20), 1);
}

static void progress_destroy(widget_t *w) {
    if (w->priv_data) {
        free(w->priv_data);
        w->priv_data = NULL;
    }
}

static const widget_ops_t progress_ops = {
    .render = progress_render,
    .on_mouse = NULL,
    .on_key = NULL,
    .destroy = progress_destroy
};

widget_t *progress_create(int x, int y, int w, int h, int initial_val) {
    widget_t *p = (widget_t *)malloc(sizeof(widget_t));
    progress_data_t *data = (progress_data_t *)malloc(sizeof(progress_data_t));

    memset(data, 0, sizeof(progress_data_t));
    data->value = initial_val;

    widget_base_init(p, x, y, w, h, &progress_ops);
    p->priv_data = data;
    return p;
}

void progress_set_value(widget_t *w, int val) {
    if (!w || !w->priv_data) return;
    progress_data_t *data = (progress_data_t *)w->priv_data;
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    data->value = val;
}

int progress_get_value(widget_t *w) {
    if (!w || !w->priv_data) return 0;
    return ((progress_data_t *)w->priv_data)->value;
}