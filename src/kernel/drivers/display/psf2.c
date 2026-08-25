// src/kernel/drivers/display/psf2.c - Hardware-Safe PSF2 Font Engine
#include "psf2.h"
#include "../../../libs/string.h"

psf2_font_t kernel_psf2_font = {0};

static uint32_t utf8_next(const char **src) {
    if (!src || !*src) return 0xFFFD;
    const uint8_t *s = (const uint8_t *)*src;
    uint32_t c = *s++;
    uint32_t cp = 0;
    int extra = 0;

    if (c < 0x80) { 
        cp = c; 
        extra = 0; 
    } else if ((c & 0xE0) == 0xC0) { 
        cp = c & 0x1F; 
        extra = 1; 
    } else if ((c & 0xF0) == 0xE0) { 
        cp = c & 0x0F; 
        extra = 2; 
    } else if ((c & 0xF8) == 0xF0) { 
        cp = c & 0x07; 
        extra = 3; 
    } else { 
        *src = (const char *)s; 
        return 0xFFFD; 
    }

    for (int i = 0; i < extra; ++i) {
        if ((*s & 0xC0) != 0x80) {
            *src = (const char *)s;
            return 0xFFFD;
        }
        cp = (cp << 6) | (*s & 0x3F);
        s++;
    }
    *src = (const char *)s;
    return cp;
}

static void psf2_build_unicode_table(psf2_font_t *f, const uint8_t *table_start, const uint8_t *table_end) {
    if (!f || !table_start || !table_end || table_start >= table_end) return;

    memset(f->codepoint_to_glyph, 0, sizeof(f->codepoint_to_glyph));
    const uint8_t *p = table_start;
    uint32_t glyph = 0;

    while (p < table_end && glyph < f->hdr->numglyph) {
        if (*p == 0xFF) { glyph++; p++; continue; }
        if (*p == 0xFE) { p++; continue; }

        const char *cur = (const char *)p;
        uint32_t cp = utf8_next(&cur);
        p = (const uint8_t *)cur;

        if (cp < 0x10000 && f->codepoint_to_glyph[cp] == 0) {
            f->codepoint_to_glyph[cp] = (uint16_t)glyph;
        }
    }
}

bool psf2_load(psf2_font_t *out, const void *data, uint32_t size) {
    if (!out || !data || size < sizeof(psf2_header_t)) return false;

    const psf2_header_t *hdr = (const psf2_header_t *)data;
    if (hdr->magic != PSF2_MAGIC) return false;
    if (hdr->headersize > size || hdr->bytesperglyph == 0) return false;

    uint64_t bitmap_bytes = (uint64_t)hdr->numglyph * hdr->bytesperglyph;
    if ((uint64_t)hdr->headersize + bitmap_bytes > size) return false;

    out->loaded = true;
    out->hdr = hdr;
    out->bitmaps = (const uint8_t *)data + hdr->headersize;
    out->has_unicode = (hdr->flags & 1) != 0;

    memset(out->codepoint_to_glyph, 0, sizeof(out->codepoint_to_glyph));
    if (out->has_unicode) {
        const uint8_t *table_start = out->bitmaps + bitmap_bytes;
        const uint8_t *table_end = (const uint8_t *)data + size;
        psf2_build_unicode_table(out, table_start, table_end);
    }
    return true;
}

static uint32_t glyph_index(const psf2_font_t *f, uint32_t cp) {
    if (f->has_unicode) {
        if (cp >= 0x10000) return 0;
        uint16_t g = f->codepoint_to_glyph[cp];
        if (g != 0 || cp == 0) return g;
    }
    if (cp < f->hdr->numglyph) return cp;
    return 0;
}

int psf2_draw_char(const psf2_font_t *f, uint32_t *fb, int fb_pitch_pixels, int fb_h,
                   int x, int y, uint32_t cp, uint32_t color, uint32_t bg_color) {
    if (!f || !f->loaded || !fb || fb_pitch_pixels <= 0) return 0;

    uint32_t gi = glyph_index(f, cp);
    const uint8_t *glyph = f->bitmaps + gi * f->hdr->bytesperglyph;
    uint32_t bpr = (f->hdr->width + 7) / 8;

    for (uint32_t row = 0; row < f->hdr->height; ++row) {
        int py = y + (int)row;
        if (py < 0 || py >= fb_h) { 
            glyph += bpr; 
            continue; 
        }

        for (uint32_t col = 0; col < f->hdr->width; ++col) {
            int px = x + (int)col;
            if (px < 0 || px >= fb_pitch_pixels) continue;

            uint8_t byte = glyph[col / 8];
            bool bit = (byte & (0x80 >> (col & 7))) != 0;

            if (bit) {
                fb[py * fb_pitch_pixels + px] = color;
            } else if (bg_color != 0x00000000) {
                fb[py * fb_pitch_pixels + px] = bg_color;
            }
        }
        glyph += bpr;
    }
    return (int)f->hdr->width;
}