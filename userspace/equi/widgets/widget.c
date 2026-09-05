#include "widget.h"
#include <string.h>

void widget_base_init(widget_t *w, int x, int y, int width, int height, const widget_ops_t *ops) {
    memset(w, 0, sizeof(widget_t));
    w->x = x;
    w->y = y;
    w->w = width;
    w->h = height;
    w->visible = true;
    w->ops = ops;
}

void widget_render(surface_t *surf, widget_t *w, int abs_x, int abs_y) {
    if (!w || !w->visible || !w->ops || !w->ops->render) return;
    w->ops->render(surf, w, abs_x, abs_y);
}

bool widget_dispatch_mouse(widget_t *w, int abs_x, int abs_y, int mx, int my, bool down) {
    if (!w || !w->visible || !w->ops || !w->ops->on_mouse) return false;
    return w->ops->on_mouse(w, abs_x, abs_y, mx, my, down);
}

void widget_dispatch_key(widget_t *w, char c, bool backspace) {
    if (!w || !w->visible || !w->ops || !w->ops->on_key) return;
    w->ops->on_key(w, c, backspace);
}