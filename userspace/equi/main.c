#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "render/draw.h"

#define FBIOGET_VSCREENINFO 0x4600

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t bits_per_pixel;
};

typedef struct {
    uint16_t type;
    uint16_t code;
    int32_t value;
} __attribute__((packed)) input_event_t;

#define EV_KEY 0x01
#define EV_REL 0x02
#define REL_X  0x00
#define REL_Y  0x01
#define BTN_LEFT 0x110

// Software Cursor (Simple Arrow)
static void draw_cursor(surface_t *surf, int mx, int my) {
    draw_fill_rect(surf, mx, my, 12, 12, COLOR_RGB(255, 255, 255));
    draw_rect(surf, mx, my, 12, 12, COLOR_RGB(0, 0, 0));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("[EQUI] Initializing EquantOS Display Server...\n");

    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("[EQUI] Error: Failed to open /dev/fb0\n");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        printf("[EQUI] Error: Failed to query FBIOGET_VSCREENINFO\n");
        close(fb_fd);
        return 1;
    }

    printf("[EQUI] Screen Resolution: %ux%u, %u bpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    size_t fb_size = vinfo.xres * vinfo.yres * 4;
    uint32_t *fb_mem = (uint32_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        printf("[EQUI] Error: Failed to mmap framebuffer\n");
        close(fb_fd);
        return 1;
    }

    // Allocate RAM Double Buffer
    uint32_t *backbuffer = (uint32_t *)malloc(fb_size);
    if (!backbuffer) {
        printf("[EQUI] Error: Out of memory for backbuffer\n");
        return 1;
    }

    surface_t canvas = {
        .buffer = backbuffer,
        .width = vinfo.xres,
        .height = vinfo.yres,
        .pitch = vinfo.xres
    };

    int input_fd = open("/dev/input0", O_RDONLY | O_NONBLOCK);
    if (input_fd < 0) {
        printf("[EQUI] Warning: /dev/input0 not available, running without mouse\n");
    }

    int mouse_x = vinfo.xres / 2;
    int mouse_y = vinfo.yres / 2;

    printf("[EQUI] Compositor is running! Entering main event loop...\n");

    for (;;) {
        // 1. Process Hardware Input Events
        if (input_fd >= 0) {
            input_event_t ev;
            while (read(input_fd, &ev, sizeof(input_event_t)) == sizeof(input_event_t)) {
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) mouse_x += ev.value;
                    if (ev.code == REL_Y) mouse_y += ev.value;
                }
            }
        }

        // Clamp Cursor Bounds
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= (int)vinfo.xres) mouse_x = vinfo.xres - 1;
        if (mouse_y >= (int)vinfo.yres) mouse_y = vinfo.yres - 1;

        // 2. Render Desktop Background (Deep Modern Dark Theme)
        draw_clear(&canvas, COLOR_RGB(24, 25, 28));

        // 3. Render a Test Window (Taskbar + Window Card)
        // Taskbar at bottom
        draw_fill_rect(&canvas, 0, vinfo.yres - 40, vinfo.xres, 40, COLOR_RGB(32, 34, 37));
        draw_fill_rect(&canvas, 10, vinfo.yres - 35, 80, 30, COLOR_RGB(88, 101, 242)); // Discord Blurple Start Button

        // Test Window
        int win_x = 100, win_y = 100, win_w = 400, win_h = 250;
        draw_fill_rect(&canvas, win_x, win_y, win_w, 30, COLOR_RGB(47, 49, 54)); // Titlebar
        draw_fill_rect(&canvas, win_x, win_y + 30, win_w, win_h - 30, COLOR_RGB(54, 57, 63)); // Window Body
        draw_rect(&canvas, win_x, win_y, win_w, win_h, COLOR_RGB(32, 34, 37)); // Window Border

        // 4. Render Hardware Mouse Cursor
        draw_cursor(&canvas, mouse_x, mouse_y);

        // 5. Flip Double Buffer to Screen via High-Speed Memory Blit
        memcpy(fb_mem, backbuffer, fb_size);
    }

    return 0;
}