// src/kernel/drivers/display/fontcache.c - On-disk font atlas loader
//
// Boot font cache: prefer a prebuilt CACHE/FONTATLAS.BIN (validated,
// self-contained: bitmaps + unicode map), fall back to FONT.PSF.
// If a font is already resident (LiveCD ISO path), it is kept as-is.
#include "fontcache.h"
#include "psf2.h"
#include "../../fs/vfs.h"
#include "../../core/mem/memory.h"
#include "../../core/initcall.h"
#include "../../drivers/serial/serial.h"
#include "../../../libs/string.h"

typedef struct __attribute__((packed)) font_atlas_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t numglyph;
    uint32_t bytesperglyph;
    uint32_t flags;
    uint32_t crc;
} font_atlas_hdr_t;

#define FONTATLAS_MAX_FILE (2u * 1024u * 1024u)

static uint32_t atlas_crc32(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

static bool fontcache_install_atlas(const uint8_t *buf, uint64_t len, const char *src_path) {
    if (len < sizeof(font_atlas_hdr_t)) return false;

    font_atlas_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.magic != FONT_ATLAS_MAGIC || hdr.version != FONT_ATLAS_VERSION) return false;
    if (hdr.width == 0 || hdr.width > 64 || hdr.height == 0 || hdr.height > 128) return false;
    if (hdr.numglyph == 0 || hdr.numglyph > FONT_ATLAS_MAX_GLYPHS) return false;
    if (hdr.bytesperglyph == 0 || hdr.bytesperglyph > 1024) return false;

    uint64_t bitmap_bytes = (uint64_t)hdr.numglyph * hdr.bytesperglyph;
    uint64_t want = sizeof(hdr) + bitmap_bytes + (uint64_t)FONT_ATLAS_MAP_ENTRIES * 2;
    if (len < want) return false;

    const uint8_t *bitmaps = buf + sizeof(hdr);
    const uint8_t *map = bitmaps + bitmap_bytes;
    if (atlas_crc32(bitmaps, bitmap_bytes + (uint64_t)FONT_ATLAS_MAP_ENTRIES * 2) != hdr.crc) {
        serial_puts(COM1, "[FONTCACHE] atlas CRC mismatch, ignoring.\n");
        return false;
    }

    uint8_t *bmp_copy = (uint8_t *)kmalloc(bitmap_bytes);
    psf2_header_t *hdr_copy = (psf2_header_t *)kmalloc(sizeof(psf2_header_t));
    if (!bmp_copy || !hdr_copy) {
        if (bmp_copy) kfree(bmp_copy);
        if (hdr_copy) kfree(hdr_copy);
        return false;
    }
    memcpy(bmp_copy, bitmaps, bitmap_bytes);

    hdr_copy->magic = PSF2_MAGIC;
    hdr_copy->version = 0;
    hdr_copy->headersize = sizeof(psf2_header_t);
    hdr_copy->flags = hdr.flags;
    hdr_copy->numglyph = hdr.numglyph;
    hdr_copy->bytesperglyph = hdr.bytesperglyph;
    hdr_copy->height = hdr.height;
    hdr_copy->width = hdr.width;

    kernel_psf2_font.loaded = true;
    kernel_psf2_font.hdr = hdr_copy;
    kernel_psf2_font.bitmaps = bmp_copy;
    kernel_psf2_font.has_unicode = (hdr.flags & 1) != 0;
    for (uint32_t i = 0; i < FONT_ATLAS_MAP_ENTRIES; i++) {
        kernel_psf2_font.codepoint_to_glyph[i] =
            (uint16_t)(map[i * 2] | ((uint16_t)map[i * 2 + 1] << 8));
    }

    serial_puts(COM1, "[FONTCACHE] font atlas loaded from ");
    serial_puts(COM1, src_path);
    serial_puts(COM1, ".\n");
    return true;
}

static bool fontcache_try_atlas(const char *path) {
    vfs_node_t *node = vfs_open(path, 0);
    if (!node || !(node->flags & FS_FILE) || node->length == 0 ||
        node->length > FONTATLAS_MAX_FILE) {
        return false;
    }
    uint8_t *buf = (uint8_t *)kmalloc((size_t)node->length);
    if (!buf) return false;
    int64_t r = vfs_read(node, 0, node->length, buf);
    bool ok = (r == (int64_t)node->length) && fontcache_install_atlas(buf, node->length, path);
    kfree(buf);
    return ok;
}

static bool fontcache_try_psf(const char *path) {
    vfs_node_t *node = vfs_open(path, 0);
    if (!node || !(node->flags & FS_FILE) || node->length == 0 ||
        node->length > FONTATLAS_MAX_FILE) {
        return false;
    }
    uint8_t *buf = (uint8_t *)kmalloc((size_t)node->length);
    if (!buf) return false;
    int64_t r = vfs_read(node, 0, node->length, buf);
    bool ok = false;
    if (r == (int64_t)node->length) {
        // psf2_load borrows the buffer, so it must stay resident on success.
        psf2_font_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (psf2_load(&tmp, buf, (uint32_t)node->length)) {
            kernel_psf2_font = tmp;
            serial_puts(COM1, "[FONTCACHE] font loaded from ");
            serial_puts(COM1, path);
            serial_puts(COM1, ".\n");
            ok = true;
        }
    }
    if (!ok) kfree(buf);
    return ok;
}

void fontcache_init(void) {
    if (kernel_psf2_font.loaded) {
        serial_puts(COM1, "[FONTCACHE] resident font already loaded, keeping it.\n");
        return;
    }

    static const char *atlas_paths[] = {
        "/drives/fat32_nvme/CACHE/FONTATLAS.BIN",
        "/drives/fat32_ata/CACHE/FONTATLAS.BIN",
        "/cdrom/CACHE/FONTATLAS.BIN",
        "/drives/ext2_nvme/cache/FONTATLAS.BIN",
        "/drives/ext2_ata/cache/FONTATLAS.BIN",
        NULL,
    };
    for (int i = 0; atlas_paths[i]; i++) {
        if (fontcache_try_atlas(atlas_paths[i])) return;
    }

    static const char *psf_paths[] = {
        "/drives/fat32_nvme/FONTS/FONT.PSF",
        "/drives/fat32_ata/FONTS/FONT.PSF",
        "/cdrom/font.psf",
        "/cdrom/FONTS/FONT.PSF",
        "/drives/ext2_nvme/bin/font.psf",
        "/drives/ext2_nvme/cache/FONT.PSF",
        "/drives/ext2_ata/bin/font.psf",
        NULL,
    };
    for (int i = 0; psf_paths[i]; i++) {
        if (fontcache_try_psf(psf_paths[i])) return;
    }

    serial_puts(COM1, "[FONTCACHE] no atlas/font found on disk, keeping built-in fallback.\n");
}

static int __init fontcache_initcall(void) {
    fontcache_init();
    return 0;
}
late_initcall(fontcache_initcall);
