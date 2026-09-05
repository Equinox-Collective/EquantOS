#ifndef EQUI_FONT_H
#define EQUI_FONT_H

#include <stdint.h>
#include "draw.h"

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

void font_draw_char(surface_t *surf, int x, int y, char c, uint32_t color, int scale);
void font_draw_string(surface_t *surf, int x, int y, const char *str, uint32_t color, int scale);
int font_get_string_width(const char *str, int scale);

#endif // EQUI_FONT_H