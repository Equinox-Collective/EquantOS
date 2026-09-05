#ifndef EQUI_BUTTON_H
#define EQUI_BUTTON_H

#include "widget.h"

typedef struct {
    char text[64];
    uint32_t color_normal;
    uint32_t color_hover;
    uint32_t color_pressed;
    int corner_radius;
    void (*on_click)(widget_t *w);
} button_data_t;

widget_t *button_create(int x, int y, int w, int h, const char *text, void (*on_click)(widget_t *));
void button_set_text(widget_t *w, const char *text);

#endif // EQUI_BUTTON_H