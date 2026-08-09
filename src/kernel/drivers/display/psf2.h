#ifndef PSF2_H
#define PSF2_H

/*
 * PSF2 (PC Screen Font v2) parser & renderer.
 *
 * Format (little-endian on disk):
 *   uint32_t magic;            // 0x864AB572
 *   uint32_t version;          // 0
 *   uint32_t headersize;       // offset to bitmaps
 *   uint32_t flags;            // 1 = has Unicode table
 *   uint32_t numglyph;
 *   uint32_t bytesperglyph;
 *   uint32_t height;
 *   uint32_t width;            // pixels
 *   <glyph_bitmaps>            // numglyph * bytesperglyph bytes
 *   <unicode_table?>           // present iff flags & 1
 *
 * Each glyph is `height` rows of ceil(width/8) bytes.
 */

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
    const uint8_t        *bitmaps;      /* points into the same buffer */
    /* Unicode table (optional) — a simple direct-mapping cache: */
    uint16_t              codepoint_to_glyph[0x10000];
    bool                  has_unicode;
} psf2_font_t;

/* Initialize a psf2_font_t from a raw file buffer (lifetime-bound). */
bool psf2_load(psf2_font_t *out, const void *data, uint32_t size);

/* Render a single Unicode codepoint at (x, y) into a 32-bit linear FB.
 * Returns the width of the glyph in pixels (== font->hdr->width). */
int psf2_draw_char(const psf2_font_t *font,
                   uint32_t *fb, int fb_w, int fb_h,
                   int x, int y, uint32_t codepoint, uint32_t color);

/* Render a UTF-8 string left-to-right. Returns total pixel width drawn. */
int psf2_draw_string(const psf2_font_t *font,
                     uint32_t *fb, int fb_w, int fb_h,
                     int x, int y, const char *utf8, uint32_t color);

/* The kernel's default loaded font (filled by psf2_init_default). */
extern psf2_font_t kernel_psf2_font;
bool psf2_init_default(const void *data, uint32_t size);

#endif /* PSF2_H */
