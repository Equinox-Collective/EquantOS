#ifndef EQUI_LABEL_H
#define EQUI_LABEL_H

#include "widget.h"

typedef struct {
    char text[128];
    uint32_t color;
} label_data_t;

widget_t *label_create(int x, int y, const char *text, uint32_t color);
void label_set_text(widget_t *w, const char *text);

#endif // EQUI_LABEL_H