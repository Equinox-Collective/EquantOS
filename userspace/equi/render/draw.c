#include "draw.h"
#include <string.h>

void draw_pixel(surface_t *surf, int x, int y, uint32_t color) {
    if (!surf || !surf->buffer) return;
    if (x < 0 || y < 0 || (uint32_t)x >= surf->width || (uint32_t)y >= surf->height) return;

    uint32_t offset = y * surf->pitch + x;
    surf->buffer[offset] = blend_pixel(surf->buffer[offset], color);
}

void draw_fill_rect(surface_t *surf, int x, int y, int w, int h, uint32_t color) {
    if (!surf || !surf->buffer || w <= 0 || h <= 0) return;

    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = (x + w) > (int)surf->width ? (int)surf->width : (x + w);
    int y2 = (y + h) > (int)surf->height ? (int)surf->height : (y + h);

    uint32_t alpha = (color >> 24) & 0xFF;

    for (int cy = y1; cy < y2; cy++) {
        uint32_t *row = &surf->buffer[cy * surf->pitch + x1];
        if (alpha == 255) {
            // Fast Fill for fully opaque rectangles
            for (int cx = x1; cx < x2; cx++) {
                *row++ = color;
            }
        } else {
            // Alpha Blended Fill
            for (int cx = x1; cx < x2; cx++) {
                *row = blend_pixel(*row, color);
                row++;
            }
        }
    }
}

void draw_rect(surface_t *surf, int x, int y, int w, int h, uint32_t color) {
    draw_fill_rect(surf, x, y, w, 1, color);
    draw_fill_rect(surf, x, y + h - 1, w, 1, color);
    draw_fill_rect(surf, x, y, 1, h, color);
    draw_fill_rect(surf, x + w - 1, y, 1, h, color);
}

void draw_clear(surface_t *surf, uint32_t color) {
    if (!surf || !surf->buffer) return;
    size_t total_pixels = surf->pitch * surf->height;
    for (size_t i = 0; i < total_pixels; i++) {
        surf->buffer[i] = color;
    }
}