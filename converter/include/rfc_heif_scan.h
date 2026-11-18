/* Bounded ISOBMFF reader for HEIF and HEIC (D10 T02 s7), shared by the
 * submit-time sniff and the HEIF session adapter so both reach the same
 * decision about a file under the same limits. It answers what the Windows
 * Imaging Component does not report faithfully: the primary image's
 * geometry as the system decoder returns it, its significant depth and
 * coding, alpha and other auxiliary images, the number of top-level images,
 * derived and sequence content, the primary's colour boxes, and whether
 * Exif or XMP items are present. Boxes are queried on demand through
 * positional reads (ISO/IEC 14496-12 and 23008-12); nothing is allocated. */
#ifndef RFC_HEIF_SCAN_H
#define RFC_HEIF_SCAN_H

#include "rfc_codec.h"

#include <stddef.h>
#include <stdint.h>

#define RFC_HEIF_MAX_ITEMS 1024u
#define RFC_HEIF_MAX_PROPERTIES 1024u

/* Positional read: up to n bytes at off into dst, the count in *got (fewer
 * only at the end of the input). */
typedef rfc_status (*rfc_heif_read_fn)(void *ctx, uint64_t off, uint8_t *dst,
                                       size_t n, size_t *got);

typedef struct rfc_heif_info {
    uint32_t width, height;    /* as the system decoder returns them */
    int dims_known;            /* width and height were read */
    uint32_t bit_depth;        /* significant bits per sample; 8 if unstated */
    int hevc;                  /* the primary picture is HEVC coded */
    uint32_t primary_type;     /* item type of the primary item */
    int has_icc;               /* colr prof/rICC on the primary */
    uint64_t icc_offset;
    uint32_t icc_len;
    int has_nclx;              /* colr nclx on the primary (no ICC) */
    uint16_t primaries, transfer, matrix;
    uint8_t full_range;
    int has_exif, has_xmp;     /* Exif / XMP items present */
    uint8_t irot;              /* quarter turns anticlockwise (0..3) */
    int has_imir;
    uint8_t imir_axis;
    rfc_layout layout;         /* rgb8 at width x height */
} rfc_heif_info;

/* Scan a HEIF/HEIC file of `size` bytes. RFC_OK: one opaque 8-bit
 * top-level picture the adapter may decode. Refusals: INVALID_ARGUMENT or
 * TRUNCATED_INPUT (malformed), LIMIT_EXCEEDED (more than the item or
 * property bound), UNSUPPORTED_COMBINATION (a second image, a derived
 * transform or overlay, a sequence track, a non-alpha auxiliary image, an
 * HDR transfer, a fractional clean aperture, a mirror combined with a
 * rotation), UNSUPPORTED_ALPHA (an alpha auxiliary), UNSUPPORTED_SAMPLE
 * (more than 8 significant bits). *detail (may be NULL) names the reason.
 * A failure returned by read_at is returned unchanged and *read_failed set,
 * so a caller can tell a reader failure from a verdict. width, height and
 * dims_known are filled as soon as they are read, even on a refusal. */
rfc_status rfc_heif_scan(rfc_heif_read_fn read_at, void *ctx, uint64_t size,
                         rfc_heif_info *out, const char **detail,
                         int *read_failed);

#endif /* RFC_HEIF_SCAN_H */
