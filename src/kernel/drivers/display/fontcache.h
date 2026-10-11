// src/kernel/drivers/display/fontcache.h - On-disk font atlas (boot font cache)
//
// Atlas files (CACHE/FONTATLAS.BIN) are generated on the host by
// tools/gen_font_atlas.py from res/font.psf and seeded into the FAT32
// disk images by create_disks.py (also shipped on the ISO under /CACHE).
// The format must stay in sync with the generator. All integers LE.
//
//   off  size  field
//   0    4     magic 0x41465145 ('EQFA')
//   4    4     version = 1
//   8    4     width (pixels), 12 height, 16 numglyph, 20 bytesperglyph
//   24   4     flags (PSF2 flags copy, bit0 = has unicode table)
//   28   4     crc32 of (bitmaps || map_bytes)
//   32   N     bitmaps (numglyph * bytesperglyph, raw PSF2 order)
//   32+N 131072 unicode map, 65536 x u16 LE (glyph index per codepoint)
#ifndef FONTCACHE_H
#define FONTCACHE_H

#include <stdint.h>

#define FONT_ATLAS_MAGIC   0x41465145u
#define FONT_ATLAS_VERSION 1u
#define FONT_ATLAS_MAP_ENTRIES 65536u
#define FONT_ATLAS_MAX_GLYPHS  4096u

// Populate kernel_psf2_font from the on-disk atlas (preferred) or from a
// FONT.PSF fallback. Runs as a late initcall, after storage is mounted.
// If a font is already loaded (e.g. from the LiveCD ISO), it is kept.
void fontcache_init(void);

#endif // FONTCACHE_H
