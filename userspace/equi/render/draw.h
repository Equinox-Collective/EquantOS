#ifndef EQUI_DRAW_H
#define EQUI_DRAW_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch; // in pixels
} surface_t;

// Color Macros (ARGB 8:8:8:8)
#define COLOR_ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define COLOR_RGB(r, g, b)     COLOR_ARGB(255, r, g, b)

static inline uint32_t blend_pixel(uint32_t bg, uint32_t fg) {
    uint32_t alpha = (fg >> 24) & 0xFF;
    if (alpha == 255) return fg;
    if (alpha == 0)   return bg;

    uint32_t inv_alpha = 255 - alpha;

    uint32_t r = (((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * inv_alpha) / 255;
    uint32_t g = (((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * inv_alpha) / 255;
    uint32_t b = ((fg & 0xFF) * alpha + (bg & 0xFF) * inv_alpha) / 255;

    return COLOR_ARGB(255, r, g, b);
}

void draw_pixel(surface_t *surf, int x, int y, uint32_t color);
void draw_fill_rect(surface_t *surf, int x, int y, int w, int h, uint32_t color);
void draw_rect(surface_t *surf, int x, int y, int w, int h, uint32_t color);
void draw_clear(surface_t *surf, uint32_t color);

#endif // EQUI_DRAW_H