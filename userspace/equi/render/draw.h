#ifndef EQUI_DRAW_H
#define EQUI_DRAW_H

#include <stdint.h>
#include <stddef.h>
#include "blend.h"

typedef struct {
    uint32_t *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch; // in pixels
} surface_t;

#define COLOR_ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define COLOR_RGB(r, g, b)     COLOR_ARGB(255, r, g, b)

void draw_pixel(surface_t *surf, int x, int y, uint32_t color);
void draw_fill_rect(surface_t *surf, int x, int y, int w, int h, uint32_t color);
void draw_rect(surface_t *surf, int x, int y, int w, int h, uint32_t color);
void draw_clear(surface_t *surf, uint32_t color);

// New Advanced 2D Primitives
void draw_fill_rounded_rect(surface_t *surf, int x, int y, int w, int h, int radius, uint32_t color);
void draw_rounded_rect(surface_t *surf, int x, int y, int w, int h, int radius, uint32_t color);
void draw_fill_circle(surface_t *surf, int cx, int cy, int radius, uint32_t color);
void draw_gradient_v(surface_t *surf, int x, int y, int w, int h, uint32_t color_top, uint32_t color_bottom);

#endif // EQUI_DRAW_H