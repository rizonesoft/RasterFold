/* Owned pixel, frame and metadata types for the converter engine.
 * Invariants are checked by rfc_layout_validate (rfc_codec.h) before any
 * allocation: callers never size a buffer from an unchecked layout. */
#ifndef RFC_IMAGE_H
#define RFC_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample formats. Byte order is little-endian host order for 16-bit
 * samples; multi-byte samples are never byte-swapped implicitly. */
typedef enum rfc_sample_format {
    RFC_SAMPLE_GRAY8 = 1,
    RFC_SAMPLE_GRAY16 = 2,
    RFC_SAMPLE_RGB8 = 3,
    RFC_SAMPLE_RGBA8 = 4,
    RFC_SAMPLE_RGB16 = 5,
    RFC_SAMPLE_RGBA16 = 6,
    RFC_SAMPLE_RGB_HALF = 7,  /* IEEE 754 half float per channel */
    RFC_SAMPLE_RGBA_HALF = 8,
    RFC_SAMPLE_RGB_FLOAT = 9, /* IEEE 754 single float per channel */
    RFC_SAMPLE_RGBA_FLOAT = 10
} rfc_sample_format;

typedef enum rfc_alpha_mode {
    RFC_ALPHA_NONE = 0,          /* no alpha channel in the format */
    RFC_ALPHA_OPAQUE = 1,        /* alpha channel present, all values opaque */
    RFC_ALPHA_STRAIGHT = 2,      /* unassociated alpha; RGB is stored verbatim */
    RFC_ALPHA_PREMULTIPLIED = 3  /* associated alpha; RGB is scaled by alpha */
} rfc_alpha_mode;

/* EXIF orientation values 1..8; RFC_ORIENTATION_UNSPECIFIED is 0. */
typedef enum rfc_orientation {
    RFC_ORIENTATION_UNSPECIFIED = 0,
    RFC_ORIENTATION_NORMAL = 1,
    RFC_ORIENTATION_FLIP_HORIZONTAL = 2,
    RFC_ORIENTATION_ROTATE_180 = 3,
    RFC_ORIENTATION_FLIP_VERTICAL = 4,
    RFC_ORIENTATION_TRANSPOSE = 5,
    RFC_ORIENTATION_ROTATE_90 = 6,
    RFC_ORIENTATION_TRANSVERSE = 7,
    RFC_ORIENTATION_ROTATE_270 = 8
} rfc_orientation;

#define RFC_MAX_ICC_BYTES ((size_t)16 * 1024 * 1024)
#define RFC_MAX_DIMENSION ((uint32_t)1 << 24)
#define RFC_MAX_FRAMES ((uint32_t)100000)

/* A pixel layout. stride_bytes is the distance between row starts and
 * must cover width * bytes-per-pixel; rows are tightly ordered top to
 * bottom with no vertical flip flag (orientation carries display intent). */
typedef struct rfc_layout {
    rfc_sample_format format;
    rfc_alpha_mode alpha;
    uint32_t width;
    uint32_t height;
    size_t stride_bytes;
    /* Significant bits per channel (native depth), distinct from the
     * storage format: 8 for RGB8, 10 or 12 for AVIF/HEIF sources stored
     * in a 16-bit format, 16 for half float, 32 for single float.
     * Integer samples are left-aligned with zero fill: the stored value
     * is exactly v << (storage_bits - bits_per_sample), the unused low
     * bits are zero, and the native maximum (2^bits - 1) therefore maps
     * to 2^storage - 2^(storage - bits), never to the storage maximum.
     * Adapters must produce and consume exactly this mapping. Must be
     * nonzero and no greater than the storage bits per channel. */
    uint32_t bits_per_sample;
} rfc_layout;

/* Non-ICC colourimetry chunks carried by containers such as PNG (W3C PNG
 * colour-space chunks: sRGB, gAMA, cHRM, cICP, mDCV, cLLI) so lossless
 * decode/encode preserves and verifies transfer/primaries metadata even when
 * no ICC profile is attached. Values store exact wire integers. */
typedef struct rfc_colorimetry {
    uint8_t has_srgb;
    uint8_t srgb_intent;
    uint8_t has_gama;
    uint8_t has_chrm;
    uint8_t has_cicp;
    uint8_t has_mdcv;
    uint8_t has_clli;
    uint8_t cicp[4];
    uint32_t gama_scaled;
    uint32_t chrm_xy[8];
    uint16_t mdcv_xy[8];
    uint32_t mdcv_max_dl;
    uint32_t mdcv_min_dl;
    uint32_t clli_max_cll;
    uint32_t clli_max_fall;
} rfc_colorimetry;

/* Per-frame metadata. icc/exif/xmp borrow caller memory unless the frame
 * owns them (rfc_frame owns a copy). duration_ms is 0 for still images. */
typedef struct rfc_metadata {
    const uint8_t *icc;
    size_t icc_len;
    rfc_orientation orientation;
    uint32_t duration_ms;
    rfc_colorimetry color;
    const uint8_t *exif;
    size_t exif_len;
    const uint8_t *xmp;
    size_t xmp_len;
} rfc_metadata;

typedef struct rfc_allocator {
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr);
    void *ctx;
} rfc_allocator;

/* An owned frame. data is owned by the frame and freed by rfc_frame_free
 * with the same allocator that created it; ownership is opaque: callers
 * do not free data directly and do not copy the struct to share ownership. */
typedef struct rfc_frame {
    rfc_layout layout;
    rfc_metadata metadata;
    uint8_t *data;
    size_t data_len; /* exactly stride_bytes * height for a valid frame */
    const rfc_allocator *allocator; /* borrowed; must outlive the frame */
} rfc_frame;

/* An owned encoded byte buffer, produced by an adapter encode and freed
 * by rfc_encoded_free with the allocator that created it. Encoded bytes
 * have no pixel layout; rfc_frame is only for decoded pixels. */
typedef struct rfc_encoded {
    uint8_t *data;
    size_t len;
    const rfc_allocator *allocator; /* borrowed; must outlive the buffer */
} rfc_encoded;

void rfc_encoded_free(rfc_encoded *encoded);

/* Free owned metadata payload buffers (icc, exif, xmp) via allocator (or
 * libc free when allocator is NULL) and clear their pointers and lengths. */
void rfc_metadata_free(rfc_metadata *meta, const rfc_allocator *allocator);

/* Bytes per pixel for a sample format, or 0 for an unknown format. */
size_t rfc_sample_bytes(rfc_sample_format format);

/* Return 1 if any colourimetry flag is set, else 0. */
int rfc_colorimetry_has_any(const rfc_colorimetry *color);

/* Validate flag ranges and PNG-bounded integer fields of colorimetry (1=ok). */
int rfc_colorimetry_is_valid(const rfc_colorimetry *color);

#ifdef __cplusplus
}
#endif
#endif
