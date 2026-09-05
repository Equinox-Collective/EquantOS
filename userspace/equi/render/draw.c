#include "draw.h"

void draw_pixel(surface_t *surf, int x, int y, uint32_t color) {
    if (!surf || !surf->buffer) return;
    if (x < 0 || y < 0 || (uint32_t)x >= surf->width || (uint32_t)y >= surf->height) return;

    uint32_t offset = y * surf->pitch + x;
    surf->buffer[offset] = blend_argb(surf->buffer[offset], color);
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
            for (int cx = x1; cx < x2; cx++) {
                *row++ = color;
            }
        } else {
            for (int cx = x1; cx < x2; cx++) {
                *row = blend_argb(*row, color);
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
    size_t total = surf->pitch * surf->height;
    for (size_t i = 0; i < total; i++) {
        surf->buffer[i] = color;
    }
}

void draw_fill_circle(surface_t *surf, int cx, int cy, int radius, uint32_t color) {
    if (radius <= 0) return;
    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        int y = cy + dy;
        if (y < 0 || (uint32_t)y >= surf->height) continue;
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= r2) {
                int x = cx + dx;
                if (x >= 0 && (uint32_t)x < surf->width) {
                    draw_pixel(surf, x, y, color);
                }
            }
        }
    }
}

void draw_fill_rounded_rect(surface_t *surf, int x, int y, int w, int h, int r, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        draw_fill_rect(surf, x, y, w, h, color);
        return;
    }

    // 1. Central cross rectangles
    draw_fill_rect(surf, x + r, y, w - (2 * r), h, color);
    draw_fill_rect(surf, x, y + r, r, h - (2 * r), color);
    draw_fill_rect(surf, x + w - r, y + r, r, h - (2 * r), color);

    // 2. Four circular corners
    int r2 = r * r;
    int cx[4] = { x + r, x + w - r - 1, x + r, x + w - r - 1 };
    int cy[4] = { y + r, y + r, y + h - r - 1, y + h - r - 1 };

    for (int dy = 0; dy <= r; dy++) {
        for (int dx = 0; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r2) {
                // Top-Left
                draw_pixel(surf, cx[0] - dx, cy[0] - dy, color);
                // Top-Right
                draw_pixel(surf, cx[1] + dx, cy[1] - dy, color);
                // Bottom-Left
                draw_pixel(surf, cx[2] - dx, cy[2] + dy, color);
                // Bottom-Right
                draw_pixel(surf, cx[3] + dx, cy[3] + dy, color);
            }
        }
    }
}

void draw_gradient_v(surface_t *surf, int x, int y, int w, int h, uint32_t color_top, uint32_t color_bottom) {
    if (w <= 0 || h <= 0) return;
    for (int cy = 0; cy < h; cy++) {
        uint8_t factor = (uint8_t)((cy * 255) / (h > 1 ? h - 1 : 1));
        uint32_t row_color = blend_lerp(color_top, color_bottom, factor);
        draw_fill_rect(surf, x, y + cy, w, 1, row_color);
    }
}

void draw_rounded_rect(surface_t *surf, int x, int y, int w, int h, int r, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        draw_rect(surf, x, y, w, h, color);
        return;
    }

    // 1. Draw 4 straight line segments
    draw_fill_rect(surf, x + r, y, w - (2 * r), 1, color);             // Top border
    draw_fill_rect(surf, x + r, y + h - 1, w - (2 * r), 1, color);     // Bottom border
    draw_fill_rect(surf, x, y + r, 1, h - (2 * r), color);             // Left border
    draw_fill_rect(surf, x + w - 1, y + r, 1, h - (2 * r), color);     // Right border

    // 2. Draw 4 corner arcs via Midpoint Arc Algorithm
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int cx = 0;
    int cy = r;

    int x_tl = x + r;
    int y_tl = y + r;
    int x_tr = x + w - r - 1;
    int y_tr = y + r;
    int x_bl = x + r;
    int y_bl = y + h - r - 1;
    int x_br = x + w - r - 1;
    int y_br = y + h - r - 1;

    while (cx <= cy) {
        // Top-Left corner
        draw_pixel(surf, x_tl - cx, y_tl - cy, color);
        draw_pixel(surf, x_tl - cy, y_tl - cx, color);

        // Top-Right corner
        draw_pixel(surf, x_tr + cx, y_tr - cy, color);
        draw_pixel(surf, x_tr + cy, y_tr - cx, color);

        // Bottom-Left corner
        draw_pixel(surf, x_bl - cx, y_bl + cy, color);
        draw_pixel(surf, x_bl - cy, y_bl + cx, color);

        // Bottom-Right corner
        draw_pixel(surf, x_br + cx, y_br + cy, color);
        draw_pixel(surf, x_br + cy, y_br + cx, color);

        if (f >= 0) {
            cy--;
            ddF_y += 2;
            f += ddF_y;
        }
        cx++;
        ddF_x += 2;
        f += ddF_x;
    }
}