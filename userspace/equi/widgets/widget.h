#ifndef EQUI_WIDGET_H
#define EQUI_WIDGET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../render/draw.h"

typedef enum {
    WIDGET_LABEL,
    WIDGET_BUTTON,
    WIDGET_CHECKBOX,
    WIDGET_SLIDER,
    WIDGET_PROGRESS,
    WIDGET_INPUT
} widget_type_t;

typedef struct widget {
    widget_type_t type;
    int x, y, w, h;       // Local coordinates relative to window
    char text[64];
    
    // Values for Checkbox (0/1), Slider (min..max), Progress (0..100)
    int value;
    int min_val;
    int max_val;
    
    // Text input buffer
    char input_buf[64];
    size_t input_len;
    
    // Interactive states
    bool hovered;
    bool pressed;
    bool focused;
    
    // Callbacks
    void (*on_click)(struct widget *w);
    void (*on_change)(struct widget *w, int new_val);
} widget_t;

void widget_init_button(widget_t *w, int x, int y, int width, int height, const char *text, void (*on_click)(widget_t *));
void widget_init_checkbox(widget_t *w, int x, int y, const char *label, bool checked, void (*on_change)(widget_t *, int));
void widget_init_slider(widget_t *w, int x, int y, int width, int min, int max, int val, void (*on_change)(widget_t *, int));
void widget_init_progress(widget_t *w, int x, int y, int width, int height, int val);
void widget_init_input(widget_t *w, int x, int y, int width, int height, const char *placeholder);
void widget_init_label(widget_t *w, int x, int y, const char *text);

void widget_render(surface_t *surf, widget_t *w, int win_abs_x, int win_abs_y);
bool widget_handle_mouse(widget_t *w, int win_abs_x, int win_abs_y, int mx, int my, bool mouse_down);
void widget_handle_key(widget_t *w, char c, bool is_backspace);

#endif // EQUI_WIDGET_H