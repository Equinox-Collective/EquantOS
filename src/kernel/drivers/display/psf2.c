#include "psf2.h"
#include "string.h"

extern void term_print(const char *str);

psf2_font_t kernel_psf2_font;

/* ---- Minimal UTF-8 decoder ------------------------------------------ */
/* Returns codepoint and advances *src; on bad input returns 0xFFFD. */
static uint32_t utf8_next(const char **src) {
    const uint8_t *s = (const uint8_t *)*src;
    uint32_t c = *s++;
    uint32_t cp;
    int extra;
    if      (c < 0x80) { cp = c;          extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { *src = (const char *)s; return 0xFFFD; }

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

/* ---- Parse Unicode table -------------------------------------------- */

static void psf2_build_unicode_table(psf2_font_t *f, const uint8_t *table_start,
                                     const uint8_t *table_end) {
    memset(f->codepoint_to_glyph, 0, sizeof(f->codepoint_to_glyph));

    /*
     * Format per glyph entry (terminator = 0xFF):
     *   <unicodes>  (UTF-8 sequences, multiple allowed)
     *   0xFE        (optional sequence separator, ignored here)
     *   0xFF        (end of this glyph)
     */
    const uint8_t *p = table_start;
    uint32_t glyph = 0;
    while (p < table_end && glyph < f->hdr->numglyph) {
        if (*p == 0xFF) { glyph++; p++; continue; }
        if (*p == 0xFE) { p++; continue; }

        /* Decode one UTF-8 codepoint. */
        const char *cur = (const char *)p;
        uint32_t cp = utf8_next(&cur);
        p = (const uint8_t *)cur;

        if (cp < 0x10000 && f->codepoint_to_glyph[cp] == 0)
            f->codepoint_to_glyph[cp] = (uint16_t)glyph;
    }
}

bool psf2_load(psf2_font_t *out, const void *data, uint32_t size) {
    if (!out || !data || size < sizeof(psf2_header_t)) return false;
    const psf2_header_t *hdr = (const psf2_header_t *)data;
    if (hdr->magic != PSF2_MAGIC) {
        term_print("[PSF2] Bad magic.\n");
        return false;
    }
    if (hdr->headersize > size || hdr->bytesperglyph == 0) {
        term_print("[PSF2] Malformed header.\n");
        return false;
    }
    uint32_t bitmap_bytes = hdr->numglyph * hdr->bytesperglyph;
    if ((uint64_t)hdr->headersize + bitmap_bytes > size) {
        term_print("[PSF2] Truncated bitmaps.\n");
        return false;
    }

    out->loaded     = true;
    out->hdr        = hdr;
    out->bitmaps    = (const uint8_t *)data + hdr->headersize;
    out->has_unicode = (hdr->flags & 1) != 0;

    memset(out->codepoint_to_glyph, 0, sizeof(out->codepoint_to_glyph));
    if (out->has_unicode) {
        const uint8_t *table_start = out->bitmaps + bitmap_bytes;
        const uint8_t *table_end   = (const uint8_t *)data + size;
        psf2_build_unicode_table(out, table_start, table_end);
    }
    return true;
}

bool psf2_init_default(const void *data, uint32_t size) {
    bool ok = psf2_load(&kernel_psf2_font, data, size);
    if (ok) term_print("[PSF2] Default font loaded.\n");
    return ok;
}

/* ---- Glyph lookup --------------------------------------------------- */

static uint32_t glyph_index(const psf2_font_t *f, uint32_t cp) {
    if (f->has_unicode) {
        if (cp >= 0x10000) return 0;
        uint16_t g = f->codepoint_to_glyph[cp];
        if (g != 0 || cp == 0) return g;
        /* fall through: try literal index for ASCII */
    }
    if (cp < f->hdr->numglyph) return cp;
    return 0;
}

int psf2_draw_char(const psf2_font_t *f,
                   uint32_t *fb, int fb_w, int fb_h,
                   int x, int y, uint32_t cp, uint32_t color) {
    if (!f->loaded) return 0;
    uint32_t gi  = glyph_index(f, cp);
    const uint8_t *glyph = f->bitmaps + gi * f->hdr->bytesperglyph;
    uint32_t bpr = (f->hdr->width + 7) / 8;

    for (uint32_t row = 0; row < f->hdr->height; ++row) {
        int py = y + (int)row;
        if (py < 0 || py >= fb_h) { glyph += bpr; continue; }
        for (uint32_t col = 0; col < f->hdr->width; ++col) {
            int px = x + (int)col;
            if (px < 0 || px >= fb_w) continue;
            uint8_t byte = glyph[col / 8];
            if (byte & (0x80 >> (col & 7)))
                fb[py * fb_w + px] = color;
        }
        glyph += bpr;
    }
    return (int)f->hdr->width;
}

int psf2_draw_string(const psf2_font_t *f,
                     uint32_t *fb, int fb_w, int fb_h,
                     int x, int y, const char *utf8, uint32_t color) {
    if (!f->loaded) return 0;
    int x0 = x;
    while (*utf8) {
        const char *cur = utf8;
        uint32_t cp = utf8_next(&cur);
        utf8 = cur;
        x += psf2_draw_char(f, fb, fb_w, fb_h, x, y, cp, color);
    }
    return x - x0;
}
