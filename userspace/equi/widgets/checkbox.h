#ifndef EQUI_CHECKBOX_H
#define EQUI_CHECKBOX_H

#include "widget.h"

typedef struct {
    char text[64];
    bool checked;
    void (*on_change)(widget_t *w, bool checked);
} checkbox_data_t;

widget_t *checkbox_create(int x, int y, const char *text, bool initial_val, void (*on_change)(widget_t *, bool));

#endif // EQUI_CHECKBOX_H