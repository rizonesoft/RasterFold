/* Bounded ISOBMFF reader for HEIF and HEIC (D10 T02 s7), shared by the
 * submit-time sniff and the HEIF session adapter so both reach the same
 * decision about a file. It answers what the Windows Imaging Component does
 * not report faithfully: the primary picture's coded and displayed
 * geometry, its significant depth and coding, alpha and other auxiliary
 * images, the number of top-level images, derived and sequence content, the
 * primary's colour boxes, and whether Exif or XMP items are present.
 *
 * The top-level boxes are walked with positional reads; the `meta` box is
 * then read once into a buffer from the caller's allocator (at most
 * RFC_HEIF_MAX_META_BYTES) and parsed in memory, every field checked against
 * the box that contains it, with the item, reference, property and
 * association tables indexed once (ISO/IEC 14496-12 and 23008-12). */
#ifndef RFC_HEIF_SCAN_H
#define RFC_HEIF_SCAN_H

#include "rfc_codec.h"

#include <stddef.h>
#include <stdint.h>

#define RFC_HEIF_MAX_ITEMS 4096u
#define RFC_HEIF_MAX_PROPERTIES 4096u
#define RFC_HEIF_MAX_REFERENCES 16384u
#define RFC_HEIF_MAX_ASSOCIATIONS 16384u
#define RFC_HEIF_MAX_META_BYTES ((uint64_t)RFC_MAX_ICC_BYTES + (4u << 20))
#define RFC_HEIF_SIZE_UNKNOWN UINT64_MAX

/* Positional read: up to n bytes at off into dst, the count in *got (fewer
 * only at the end of the input). */
typedef rfc_status (*rfc_heif_read_fn)(void *ctx, uint64_t off, uint8_t *dst,
                                       size_t n, size_t *got);

typedef enum rfc_heif_coding {
    RFC_HEIF_CODING_HEVC = 1,
    RFC_HEIF_CODING_AV1 = 2
} rfc_heif_coding;

typedef struct rfc_heif_info {
    uint32_t width, height;    /* displayed: after clap and irot */
    uint32_t coded_width, coded_height; /* the coded picture, turned by irot */
    int dims_known;            /* width and height were read */
    uint32_t bit_depth;        /* largest significant depth of any channel */
    rfc_heif_coding coding;    /* of the primary picture (all grid tiles) */
    uint32_t primary_type;     /* item type of the primary item */
    int has_icc;               /* colr prof/rICC on the primary */
    uint64_t icc_offset;       /* in the file */
    uint32_t icc_len;
    int has_nclx;              /* colr nclx on the primary (no ICC) */
    uint16_t primaries, transfer, matrix;
    uint8_t full_range;
    int has_exif, has_xmp;     /* Exif / XMP items present */
    uint8_t irot;              /* quarter turns anticlockwise (0..3) */
    int has_imir;
    rfc_layout layout;         /* rgb8 at width x height */
    rfc_layout coded_layout;   /* rgb8 at coded_width x coded_height */
} rfc_heif_info;

/* Scan a HEIF/HEIC file of `size` bytes (RFC_HEIF_SIZE_UNKNOWN: read to the
 * end). alloc (NULL: the C heap) holds the meta buffer and the tables for the
 * duration of the call. RFC_OK: one opaque 8-bit top-level picture the
 * adapter may decode. Refusals: INVALID_ARGUMENT or TRUNCATED_INPUT
 * (malformed), LIMIT_EXCEEDED (a table or the meta box over its bound),
 * UNSUPPORTED_COMBINATION (a second image, a derived transform or overlay, a
 * sequence track, a non-alpha auxiliary image, an HDR transfer, an
 * unsupported or mixed grid coding, a clean aperture off the pixel grid, a
 * mirror combined with a rotation), UNSUPPORTED_ALPHA (an alpha auxiliary),
 * UNSUPPORTED_SAMPLE (any channel above 8 bits). *detail (may be NULL)
 * names the reason. A failure returned by read_at is returned unchanged and
 * *read_failed set, so a caller can tell a reader failure from a verdict.
 * width, height and dims_known are filled once read, even on a refusal. */
rfc_status rfc_heif_scan(rfc_heif_read_fn read_at, void *ctx, uint64_t size,
                         const rfc_allocator *alloc, rfc_heif_info *out,
                         const char **detail, int *read_failed);

#endif /* RFC_HEIF_SCAN_H */
