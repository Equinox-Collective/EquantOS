#ifndef EQUI_BLEND_H
#define EQUI_BLEND_H

#include <stdint.h>

// Fast Integer Alpha Blending (Porter-Duff Over)
// Uses approximation: (val * alpha + 128) >> 8
static inline uint32_t blend_argb(uint32_t bg, uint32_t fg) {
    uint32_t alpha = (fg >> 24) & 0xFF;
    if (alpha == 255) return fg;
    if (alpha == 0)   return bg;

    uint32_t inv_alpha = 255 - alpha;

    uint32_t rb = ((fg & 0x00FF00FF) * alpha + (bg & 0x00FF00FF) * inv_alpha) >> 8;
    uint32_t g  = ((fg & 0x0000FF00) * alpha + (bg & 0x0000FF00) * inv_alpha) >> 8;

    return (0xFF000000) | (rb & 0x00FF00FF) | (g & 0x0000FF00);
}

// Linear color interpolation for gradients (t: 0..255)
uint32_t blend_lerp(uint32_t col1, uint32_t col2, uint8_t t);

#endif // EQUI_BLEND_H