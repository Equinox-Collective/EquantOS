#ifndef EQUI_WIDGET_H
#define EQUI_WIDGET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../render/draw.h"

struct widget;

typedef struct widget_ops {
    void (*render)(surface_t *surf, struct widget *w, int abs_x, int abs_y);
    bool (*on_mouse)(struct widget *w, int abs_x, int abs_y, int mx, int my, bool down);
    void (*on_key)(struct widget *w, char c, bool backspace);
    void (*destroy)(struct widget *w);
} widget_ops_t;

typedef struct widget {
    int x, y, w, h;
    bool hovered;
    bool pressed;
    bool focused;
    bool visible;
    
    const widget_ops_t *ops;
    void *priv_data; // Custom widget state (allocated or mapped)
} widget_t;

// Universal Widget Event Dispatchers
void widget_base_init(widget_t *w, int x, int y, int width, int height, const widget_ops_t *ops);
void widget_render(surface_t *surf, widget_t *w, int abs_x, int abs_y);
bool widget_dispatch_mouse(widget_t *w, int abs_x, int abs_y, int mx, int my, bool down);
void widget_dispatch_key(widget_t *w, char c, bool backspace);

// Hit-testing helper
static inline bool widget_contains_point(int px, int py, int rx, int ry, int rw, int rh) {
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

#endif // EQUI_WIDGET_H