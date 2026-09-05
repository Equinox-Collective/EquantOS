#ifndef EQUI_WINDOW_H
#define EQUI_WINDOW_H

#include "../render/draw.h"
#include "../widgets/widget.h"
#include <stdbool.h>

#define MAX_WINDOW_WIDGETS 16
#define TITLEBAR_HEIGHT    32

typedef struct window {
    int x, y, w, h;
    char title[64];
    bool is_dragging;
    int drag_off_x;
    int drag_off_y;
    bool is_focused;
    bool is_closed;
    
    widget_t widgets[MAX_WINDOW_WIDGETS];
    int widget_count;
} window_t;

void window_init(window_t *win, int x, int y, int w, int h, const char *title);
void window_add_widget(window_t *win, widget_t w);
void window_render(surface_t *surf, window_t *win);
bool window_handle_mouse(window_t *win, int mx, int my, bool mouse_down);
void window_handle_key(window_t *win, char c, bool is_backspace);

#endif // EQUI_WINDOW_H