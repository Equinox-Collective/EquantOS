#ifndef PSF2_H
#define PSF2_H

#include <stdint.h>
#include <stdbool.h>

#define PSF2_MAGIC 0x864AB572

typedef struct __attribute__((packed)) psf2_header {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t numglyph;
    uint32_t bytesperglyph;
    uint32_t height;
    uint32_t width;
} psf2_header_t;

typedef struct psf2_font {
    bool                  loaded;
    const psf2_header_t  *hdr;
    const uint8_t        *bitmaps;
    uint16_t              codepoint_to_glyph[0x10000];
    bool                  has_unicode;
} psf2_font_t;

bool psf2_load(psf2_font_t *out, const void *data, uint32_t size);

// FIX: Parameter fb_pitch_pixels uses hardware stride instead of screen width
int psf2_draw_char(const psf2_font_t *f, uint32_t *fb, int fb_pitch_pixels, int fb_h,
                   int x, int y, uint32_t cp, uint32_t color, uint32_t bg_color);

int psf2_draw_string(const psf2_font_t *font,
                     uint32_t *fb, int fb_pitch_pixels, int fb_h,
                     int x, int y, const char *utf8, uint32_t color);

extern psf2_font_t kernel_psf2_font;
bool psf2_init_default(const void *data, uint32_t size);

#endif /* PSF2_H */