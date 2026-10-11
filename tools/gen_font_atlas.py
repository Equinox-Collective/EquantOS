#!/usr/bin/env python3
"""Generate EquantOS on-disk caches from res/font.psf.

Outputs (into --out-dir, default build/cache):
  FONTATLAS.BIN - prebuilt font atlas (magic 'EQFA', version 1):
                    header + raw glyph bitmaps + flattened u16 unicode map.
                    The kernel (fontcache.c) validates and loads it directly
                    into kernel_psf2_font, without needing font.psf.
  PRELOAD.TXT   - hot-file list for the page-cache prewarm step.
                    Absolute VFS paths, '#' comments, missing files skipped.

Atlas layout (all little-endian):
  off  size  field
  0    4     magic 0x41465145 ('EQFA')
  4    4     version = 1
  8    4     width      (pixels)
  12   4     height     (pixels)
  16   4     numglyph
  20   4     bytesperglyph
  24   4     flags      (PSF2 flags copy, bit0 = has unicode table)
  28   4     crc32 of (bitmaps || map_bytes)
  32   N     bitmaps (numglyph * bytesperglyph, raw PSF2 order)
  32+N 131072 unicode map, 65536 x u16 LE (glyph index per codepoint)

Must stay in sync with src/kernel/drivers/display/fontcache.h.
"""
import argparse
import os
import struct
import sys
import zlib

PSF2_MAGIC = 0x864AB572
ATLAS_MAGIC = 0x41465145  # 'EQFA'
ATLAS_VERSION = 1
MAP_ENTRIES = 0x10000


def utf8_next(buf, pos):
    """Mirror of psf2.c utf8_next(). Returns (codepoint, new_pos)."""
    if pos >= len(buf):
        return 0xFFFD, pos
    c = buf[pos]
    pos += 1
    if c < 0x80:
        return c, pos
    if (c & 0xE0) == 0xC0:
        cp, extra = c & 0x1F, 1
    elif (c & 0xF0) == 0xE0:
        cp, extra = c & 0x0F, 2
    elif (c & 0xF8) == 0xF0:
        cp, extra = c & 0x07, 3
    else:
        return 0xFFFD, pos
    for _ in range(extra):
        if pos >= len(buf) or (buf[pos] & 0xC0) != 0x80:
            return 0xFFFD, pos
        cp = (cp << 6) | (buf[pos] & 0x3F)
        pos += 1
    return cp, pos


def build_unicode_map(table, numglyph):
    """Mirror of psf2.c psf2_build_unicode_table()."""
    glyph_map = [0] * MAP_ENTRIES
    p = 0
    glyph = 0
    n = len(table)
    while p < n and glyph < numglyph:
        b = table[p]
        if b == 0xFF:
            glyph += 1
            p += 1
            continue
        if b == 0xFE:
            p += 1
            continue
        cp, p = utf8_next(table, p)
        if cp < MAP_ENTRIES and glyph_map[cp] == 0:
            glyph_map[cp] = glyph
    return glyph_map


HOT_FILES = [
    # Seeded onto the FAT32 images by create_disks.py (disk paths first,
    # CD-ROM fallbacks second; missing entries are skipped at boot).
    "/drives/fat32_nvme/CACHE/FONTATLAS.BIN",
    "/drives/fat32_ata/CACHE/FONTATLAS.BIN",
    "/drives/fat32_nvme/FONTS/FONT.PSF",
    "/drives/fat32_ata/FONTS/FONT.PSF",
    "/cdrom/CACHE/FONTATLAS.BIN",
    "/cdrom/font.psf",
    "/cdrom/FONTS/FONT.PSF",
    # Hot user payloads (populated after install; skipped if absent).
    "/drives/ext2_nvme/bin/bash",
    "/drives/ext2_nvme/bin/busybox",
    "/drives/ext2_nvme/bin/font.psf",
    "/drives/ext2_ata/bin/bash",
    "/drives/ext2_ata/bin/busybox",
    "/cdrom/bash.elf",
    "/cdrom/busybox.elf",
    "/cdrom/hello.elf",
    "/cdrom/kernel.elf",
]


def main():
    ap = argparse.ArgumentParser(description="Generate EquantOS font atlas + preload list")
    ap.add_argument("--font", default="res/font.psf", help="input PSF2 font")
    ap.add_argument("--out-dir", default="build/cache", help="output directory")
    args = ap.parse_args()

    with open(args.font, "rb") as f:
        data = f.read()

    if len(data) < 32:
        sys.exit("font file too small for PSF2 header")
    magic, _ver, hdrsize, flags, numglyph, bpg, height, width = struct.unpack("<IIIIIIII", data[:32])
    if magic != PSF2_MAGIC:
        sys.exit(f"bad PSF2 magic {magic:08x}")
    if hdrsize > len(data) or bpg == 0 or numglyph == 0 or numglyph > 4096:
        sys.exit("insane PSF2 header values")
    bitmap_bytes = numglyph * bpg
    if hdrsize + bitmap_bytes > len(data):
        sys.exit("PSF2 bitmaps overflow file size")
    if width > 64 or height > 128 or bpg > 1024:
        sys.exit("insane PSF2 glyph dimensions")

    bitmaps = data[hdrsize:hdrsize + bitmap_bytes]
    glyph_map = [0] * MAP_ENTRIES
    if flags & 1:
        glyph_map = build_unicode_map(data[hdrsize + bitmap_bytes:], numglyph)

    map_bytes = struct.pack("<%dH" % MAP_ENTRIES, *glyph_map)
    crc = zlib.crc32(bitmaps + map_bytes) & 0xFFFFFFFF
    header = struct.pack("<IIIIIIII",
                         ATLAS_MAGIC, ATLAS_VERSION, width, height,
                         numglyph, bpg, flags, crc)

    os.makedirs(args.out_dir, exist_ok=True)
    atlas_path = os.path.join(args.out_dir, "FONTATLAS.BIN")
    with open(atlas_path, "wb") as f:
        f.write(header + bitmaps + map_bytes)

    preload_path = os.path.join(args.out_dir, "PRELOAD.TXT")
    with open(preload_path, "w") as f:
        f.write("# EquantOS page-cache prewarm list (generated, missing files skipped)\n")
        for entry in HOT_FILES:
            f.write(entry + "\n")

    print(f"[CACHE] {args.font} ({len(data)} B, {numglyph} glyphs {width}x{height})")
    print(f"[CACHE] wrote {atlas_path} ({len(header + bitmaps + map_bytes)} B, crc {crc:08x})")
    print(f"[CACHE] wrote {preload_path} ({len(HOT_FILES)} entries)")


if __name__ == "__main__":
    main()
