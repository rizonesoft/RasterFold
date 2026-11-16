/* Shared TIFF/EXIF IFD0 Orientation parsing and minimal carrier synthesis. */
#ifndef RFC_EXIF_UTIL_H
#define RFC_EXIF_UTIL_H

#include "rfc_codec.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint16_t rfc_exif_rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rfc_exif_rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint16_t rfc_exif_rd16be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t rfc_exif_rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Validate raw TIFF ("II*\0" / "MM\0*") or "Exif\0\0"-prefixed TIFF IFD0
 * bounds and extract EXIF Orientation tag (0x0112) into *out_ori
 * (RFC_ORIENTATION_UNSPECIFIED if tag 0x0112 is absent). */
static inline rfc_status rfc_exif_parse_orientation(const uint8_t *data,
                                                    size_t len,
                                                    rfc_orientation *out_ori) {
    if (!out_ori) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_ori = RFC_ORIENTATION_UNSPECIFIED;
    if (!data || len < 8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t tiff_off = 0u;
    if (len >= 14u && memcmp(data, "Exif\x00\x00", 6) == 0) {
        tiff_off = 6u;
    }
    const uint8_t *t = data + tiff_off;
    size_t tlen = len - tiff_off;
    if (tlen < 8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int little = 0;
    if (t[0] == 'I' && t[1] == 'I' && t[2] == 0x2Au && t[3] == 0x00u) {
        little = 1;
    } else if (t[0] == 'M' && t[1] == 'M' && t[2] == 0x00u && t[3] == 0x2Au) {
        little = 0;
    } else {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t ifd_off = little ? rfc_exif_rd32le(t + 4) : rfc_exif_rd32be(t + 4);
    if ((uint64_t)ifd_off + 2ull > (uint64_t)tlen) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint16_t count =
        little ? rfc_exif_rd16le(t + ifd_off) : rfc_exif_rd16be(t + ifd_off);
    uint64_t entries_end =
        (uint64_t)ifd_off + 2ull + (uint64_t)count * 12ull;
    if (entries_end > (uint64_t)tlen) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    for (uint16_t i = 0u; i < count; i++) {
        const uint8_t *e = t + ifd_off + 2u + (size_t)i * 12u;
        uint16_t tag = little ? rfc_exif_rd16le(e) : rfc_exif_rd16be(e);
        if (tag == 0x0112u) {
            uint16_t ty = little ? rfc_exif_rd16le(e + 2) : rfc_exif_rd16be(e + 2);
            uint32_t cnt =
                little ? rfc_exif_rd32le(e + 4) : rfc_exif_rd32be(e + 4);
            if (ty != 3u || cnt != 1u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            uint16_t val =
                little ? rfc_exif_rd16le(e + 8) : rfc_exif_rd16be(e + 8);
            if (val < 1u || val > 8u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            *out_ori = (rfc_orientation)val;
            return RFC_OK;
        }
    }
    return RFC_OK;
}

/* Build canonical 26-byte RFC 9649 raw little-endian TIFF IFD0 carrying only
 * tag 0x0112 (Orientation = 1..8). */
static inline rfc_status rfc_exif_build_orientation_tiff26(
    rfc_orientation ori,
    uint8_t out_tiff[26]) {
    if (!out_tiff || ori < RFC_ORIENTATION_NORMAL ||
        ori > RFC_ORIENTATION_ROTATE_270) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    static const uint8_t k_tmpl[26] = {
        'I',  'I',  0x2Au, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u,
        0x01u, 0x00u,
        0x12u, 0x01u, 0x03u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
        0x01u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u
    };
    memcpy(out_tiff, k_tmpl, 26u);
    out_tiff[18] = (uint8_t)(unsigned)ori;
    return RFC_OK;
}

#endif /* RFC_EXIF_UTIL_H */
