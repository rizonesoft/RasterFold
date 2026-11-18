/* Converter-private zero-allocation JPEG marker scanner shared by the JPEG XL
 * reconstruction adapter (D10 T02 s3) and the JPEG adapter (D10 T02 s5).
 * The scanner records structure and metadata locations; each adapter applies
 * its own acceptance policy to what it records. */
#ifndef RFC_JPEG_SCAN_H
#define RFC_JPEG_SCAN_H

#include "rfc_codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Accept arithmetic-coded SOF9/SOF10 instead of refusing them. */
#define RFC_JPEG_SCAN_ALLOW_ARITHMETIC 1u

typedef struct rfc_jpeg_icc_chunk_ref {
    size_t off;
    size_t len;
} rfc_jpeg_icc_chunk_ref;

typedef struct rfc_jpeg_scan_info {
    uint32_t width;
    uint32_t height;
    uint32_t components;
    uint32_t bits_per_sample;
    uint8_t sof_marker;    /* first SOF marker code (0xC0, 0xC1, 0xC2, 0xC9, 0xCA) */
    uint32_t sof_count;    /* SOF segments seen before EOI */
    uint32_t sos_count;    /* SOS segments seen before EOI */
    uint8_t component_ids[4];
    uint8_t h_samp[4];
    uint8_t v_samp[4];
    rfc_orientation orientation;
    int has_exif;
    size_t exif_off;
    size_t exif_len;
    uint32_t exif_count;   /* APP1 "Exif\0\0" segments */
    int has_xmp;
    size_t xmp_off;
    size_t xmp_len;
    uint32_t xmp_count;    /* APP1 standard XMP segments */
    uint32_t xmp_ext_count;/* APP1 Extended XMP segments */
    uint32_t mpf_count;    /* APP2 "MPF\0" segments */
    int saw_jfif;          /* libjpeg-valid JFIF APP0 (>= 14 payload bytes) anywhere */
    int saw_adobe;         /* libjpeg-valid Adobe APP14 (>= 12 payload bytes) anywhere */
    int adobe_transform;   /* -1 when no Adobe APP14 */
    /* Structural headers seen before the first SOS: what the decoder uses to
     * pick the colour interpretation. late_structural counts JFIF/Adobe
     * headers that appear after the first SOS. */
    int header_jfif;
    int header_adobe;
    int header_adobe_transform; /* -1 when none before the first SOS */
    uint32_t late_structural;
    uint8_t quant_slot[4];     /* SOF Tq per component */
    uint8_t dc_sel[4];         /* DC selector from the first DC scan (Ss == 0, Ah == 0) */
    uint8_t ac_sel[4];         /* AC selector from scans with Se > 0 */
    uint32_t sos_malformed;    /* SOS headers whose layout or components are invalid */
    int selector_conflict;     /* one component's scans use different AC (or DC) tables */
    int quant_missing;         /* a scanned component's slot had no DQT */
    int quant_redefined;       /* a slot changed after a component using it was first scanned */
    size_t marker_bytes;   /* total APPn + COM segment bytes (marker + length + payload) */
    uint32_t marker_count; /* APPn + COM segments */
    size_t eoi_end;        /* offset just past the first EOI */
    size_t trailing_nonpad;/* bytes after EOI that are not 0x00 or 0xFF */
    uint8_t icc_total_chunks;
    int icc_invalid;
    uint8_t icc_seen[256];
    rfc_jpeg_icc_chunk_ref icc_chunks[256];
} rfc_jpeg_scan_info;

/* Scan markers up to the first EOI. Returns RFC_ERR_UNSUPPORTED_SAMPLE for
 * non-8-bit precision or a component count other than 1 or 3,
 * RFC_ERR_UNSUPPORTED_COMBINATION for lossless, hierarchical, and (without
 * RFC_JPEG_SCAN_ALLOW_ARITHMETIC) arithmetic SOF markers, and
 * RFC_ERR_LIMIT_EXCEEDED when a single EXIF or XMP payload exceeds meta_cap. */
rfc_status rfc_jpeg_scan(const uint8_t *bytes,
                         size_t size,
                         size_t meta_cap,
                         uint32_t flags,
                         rfc_jpeg_scan_info *out);

/* Copy the first EXIF payload, the first standard XMP payload, and a complete
 * consistent ICC chunk set into allocator-owned metadata. Orientation is the
 * EXIF value, or RFC_ORIENTATION_NORMAL without EXIF orientation. */
rfc_status rfc_jpeg_scan_materialize_meta(const rfc_jpeg_scan_info *info,
                                          const uint8_t *bytes,
                                          const rfc_allocator *alloc,
                                          size_t meta_cap,
                                          rfc_metadata *out_meta);

/* Walk APPn and COM segments in stream order, skipping entropy-coded data.
 * *pos starts at 2 (after SOI) and advances; returns 1 with the segment's
 * marker and payload, or 0 at EOI or at the first malformed byte. */
int rfc_jpeg_next_app_segment(const uint8_t *bytes,
                              size_t size,
                              size_t *pos,
                              uint8_t *out_marker,
                              const uint8_t **out_payload,
                              size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* RFC_JPEG_SCAN_H */
