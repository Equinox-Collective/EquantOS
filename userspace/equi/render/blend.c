#include "blend.h"

uint32_t blend_lerp(uint32_t col1, uint32_t col2, uint8_t t) {
    uint32_t inv_t = 255 - t;

    uint32_t a = ((((col1 >> 24) & 0xFF) * inv_t + ((col2 >> 24) & 0xFF) * t) >> 8) & 0xFF;
    uint32_t r = ((((col1 >> 16) & 0xFF) * inv_t + ((col2 >> 16) & 0xFF) * t) >> 8) & 0xFF;
    uint32_t g = ((((col1 >> 8)  & 0xFF) * inv_t + ((col2 >> 8)  & 0xFF) * t) >> 8) & 0xFF;
    uint32_t b = ((((col1)       & 0xFF) * inv_t + ((col2)       & 0xFF) * t) >> 8) & 0xFF;

    return (a << 24) | (r << 16) | (g << 8) | b;
}