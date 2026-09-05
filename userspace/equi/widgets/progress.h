#ifndef EQUI_PROGRESS_H
#define EQUI_PROGRESS_H

#include "widget.h"

typedef struct {
    int value; // 0..100
} progress_data_t;

widget_t *progress_create(int x, int y, int w, int h, int initial_val);
void progress_set_value(widget_t *w, int val);
int progress_get_value(widget_t *w);

#endif // EQUI_PROGRESS_H