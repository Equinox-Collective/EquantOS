#include "cursor.h"

// 12x19 Classic Hardware Cursor Bitmap
// '.' = Transparent, '#' = Black Outline, 'W' = White Fill
static const char *cursor_shape[19] = {
    "#...........",
    "##..........",
    "#W#.........",
    "#WW#........",
    "#WWW#.......",
    "#WWWW#......",
    "#WWWWW#.....",
    "#WWWWWW#....",
    "#WWWWWWW#...",
    "#WWWWWWWW#..",
    "#WWWWWW###..",
    "#WWWW#......",
    "#WW#W#......",
    "#W#.#W#.....",
    "##..#W#.....",
    "#....#W#....",
    ".....#W#....",
    "......##....",
    "............"
};

void cursor_draw(surface_t *surf, int x, int y) {
    if (!surf || !surf->buffer) return;

    for (int cy = 0; cy < 19; cy++) {
        for (int cx = 0; cx < 12; cx++) {
            char p = cursor_shape[cy][cx];
            if (p == '#') {
                draw_pixel(surf, x + cx, y + cy, COLOR_RGB(0, 0, 0));
            } else if (p == 'W') {
                draw_pixel(surf, x + cx, y + cy, COLOR_RGB(255, 255, 255));
            }
        }
    }
}