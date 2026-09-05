#ifndef EQUI_SLIDER_H
#define EQUI_SLIDER_H

#include "widget.h"

typedef struct {
    int min_val;
    int max_val;
    int value;
    void (*on_change)(widget_t *w, int val);
} slider_data_t;

widget_t *slider_create(int x, int y, int w, int min, int max, int val, void (*on_change)(widget_t *, int));
int slider_get_value(widget_t *w);
void slider_set_value(widget_t *w, int val);

#endif // EQUI_SLIDER_H