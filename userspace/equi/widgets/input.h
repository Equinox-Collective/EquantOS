#ifndef EQUI_INPUT_H
#define EQUI_INPUT_H

#include "widget.h"

typedef struct {
    char placeholder[64];
    char text[128];
    size_t length;
} input_data_t;

widget_t *input_create(int x, int y, int w, int h, const char *placeholder);
const char *input_get_text(widget_t *w);
void input_set_text(widget_t *w, const char *text);

#endif // EQUI_INPUT_H