#ifndef RFC_VERIFY_H
#define RFC_VERIFY_H

#include "rfc_codec.h"
#include "rfc_job.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RFC_VERIFY_DETAIL_MAX 160u
#define RFC_VERIFY_PROVENANCE_MAX 64u

typedef enum rfc_exactness_class {
    RFC_EXACT_PIXEL_EXACT = 1,
    RFC_EXACT_BYTE_IDENTICAL = 2,
    RFC_EXACT_COEFFICIENT_EXACT = 3,
    RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION = 4
} rfc_exactness_class;

typedef enum rfc_metadata_policy {
    RFC_META_POLICY_PRESERVE = 1,
    RFC_META_POLICY_STRIP = 2
} rfc_metadata_policy;

/* Intentionally non-overlapping with rfc_exactness_class. */
typedef enum rfc_verify_meta_outcome {
    RFC_VERIFY_META_PRESERVED = 101,
    RFC_VERIFY_META_STRIPPED_EXPLICITLY = 102,
    RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED = 103
} rfc_verify_meta_outcome;

typedef enum rfc_mismatch_kind {
    RFC_MISMATCH_NONE = 0,
    RFC_MISMATCH_FRAME_COUNT = 1,
    RFC_MISMATCH_DIMENSIONS = 2,
    RFC_MISMATCH_SAMPLE_FORMAT = 3,
    RFC_MISMATCH_BITS_PER_SAMPLE = 4,
    RFC_MISMATCH_ALPHA_MODE = 5,
    RFC_MISMATCH_DURATION_MS = 6,
    RFC_MISMATCH_LOW_PADDING_BITS = 7,
    RFC_MISMATCH_SAMPLE_VALUE = 8,
    RFC_MISMATCH_TRANSPARENT_RGB = 9,
    RFC_MISMATCH_FLOAT_BIT_PATTERN = 10,
    RFC_MISMATCH_BYTE_HASH = 11,
    RFC_MISMATCH_JPEG_DIMENSIONS = 12,
    RFC_MISMATCH_JPEG_CODING_PARAMS = 13,
    RFC_MISMATCH_JPEG_COMPONENT_SPEC = 14,
    RFC_MISMATCH_JPEG_QUANT_TABLE = 15,
    RFC_MISMATCH_JPEG_DCT_COEFF = 16,
    RFC_MISMATCH_METADATA_LOST = 17,
    RFC_MISMATCH_METADATA_RETAINED = 18,
    RFC_MISMATCH_METADATA_UNSUPPORTED = 19
} rfc_mismatch_kind;

typedef struct rfc_sample_mismatch {
    rfc_mismatch_kind kind;
    size_t frame_index;
    uint32_t x;
    uint32_t y;
    uint32_t channel;
    uint32_t expected_bits;
    uint32_t actual_bits;
    char detail[RFC_VERIFY_DETAIL_MAX];
} rfc_sample_mismatch;

typedef struct rfc_metadata_report {
    rfc_verify_meta_outcome outcome;
    uint32_t present_source_items;
    uint32_t target_supported_items;
    uint32_t preserved_items;
    uint32_t stripped_items;
    uint32_t unsupported_items;
    uint32_t lost_supported_items;
    uint32_t retained_removable_items;
    char detail[RFC_VERIFY_DETAIL_MAX];
} rfc_metadata_report;

typedef struct rfc_jpeg_component_spec {
    uint8_t component_id;
    uint8_t h_samp_factor;
    uint8_t v_samp_factor;
    uint8_t quant_table_idx;
    uint8_t dc_table_idx;
    uint8_t ac_table_idx;
    uint32_t blocks_wide;
    uint32_t blocks_high;
    const int16_t *dct_blocks; /* blocks_wide * blocks_high * 64 coefficients */
} rfc_jpeg_component_spec;

typedef struct rfc_jpeg_quant_table {
    int populated;
    uint16_t values[64];
} rfc_jpeg_quant_table;

typedef struct rfc_jpeg_coeff_image {
    uint32_t width;
    uint32_t height;
    int progressive;
    int arithmetic_coding;
    int baseline;
    uint32_t component_count; /* 1..4 */
    rfc_jpeg_component_spec components[4];
    rfc_jpeg_quant_table quant_tables[4];
} rfc_jpeg_coeff_image;

typedef struct rfc_verify_report {
    int valid;
    rfc_status status;
    rfc_exactness_class exactness;
    rfc_verify_meta_outcome meta_outcome;
    int independent_decoder;
    char decoder_provenance[RFC_VERIFY_PROVENANCE_MAX];
    uint8_t source_hash[32];
    uint8_t verified_hash[32];
    rfc_sample_mismatch mismatch;
    rfc_metadata_report metadata;
} rfc_verify_report;

typedef struct rfc_preview_frame {
    uint32_t width;
    uint32_t height;
    size_t stride_bytes;
    uint8_t *rgba8_srgb;
    size_t data_len;
    int not_for_verification;
    rfc_allocator allocator;
} rfc_preview_frame;

const char *rfc_exactness_class_name(rfc_exactness_class cls);
rfc_status rfc_exactness_class_parse(const char *str, rfc_exactness_class *out);
const char *rfc_metadata_policy_name(rfc_metadata_policy policy);
rfc_status rfc_metadata_policy_parse(const char *str, rfc_metadata_policy *out);
const char *rfc_verify_meta_outcome_name(rfc_verify_meta_outcome outcome);
const char *rfc_mismatch_kind_name(rfc_mismatch_kind kind);

void rfc_bytes_hash_sha256(const uint8_t *data, size_t len, uint8_t out_hash[32]);
rfc_status rfc_frames_hash_sha256(const rfc_frame *frames,
                                  size_t frame_count,
                                  uint8_t out_hash[32]);

rfc_status rfc_verify_frames_equal(const rfc_frame *expected,
                                   size_t expected_count,
                                   const rfc_frame *actual,
                                   size_t actual_count,
                                   rfc_sample_mismatch *out_mismatch);

rfc_status rfc_verify_bytes_exact(const uint8_t *expected_bytes,
                                  size_t expected_len,
                                  const uint8_t *actual_bytes,
                                  size_t actual_len,
                                  rfc_exactness_class exactness,
                                  const char *decoder_provenance,
                                  int independent_decoder,
                                  rfc_verify_report *out_report);

rfc_status rfc_verify_original_byte_reconstruction(
    const uint8_t *original_jpeg,
    size_t original_len,
    const uint8_t *reconstructed_jpeg,
    size_t reconstructed_len,
    rfc_verify_report *out_report);

rfc_status rfc_verify_jpeg_coefficients(const rfc_jpeg_coeff_image *expected,
                                        const rfc_jpeg_coeff_image *actual,
                                        rfc_sample_mismatch *out_mismatch);

uint32_t rfc_format_metadata_caps(rfc_format_id id);
void rfc_metadata_items_format(uint32_t items, char *out_buf, size_t out_cap);
uint32_t rfc_metadata_source_items(const rfc_metadata *container_meta,
                                   const rfc_frame *frames,
                                   size_t frame_count);
rfc_status rfc_metadata_check_preflight(rfc_format_id target_format,
                                        rfc_metadata_policy policy,
                                        rfc_exactness_class exactness,
                                        uint32_t present_source_items,
                                        uint32_t acknowledged_unsupported,
                                        rfc_metadata_report *out_report);
rfc_status rfc_metadata_check_source_preflight(rfc_format_id target_format,
                                               rfc_metadata_policy policy,
                                               rfc_exactness_class exactness,
                                               const rfc_metadata *src_meta,
                                               const rfc_frame *src_frames,
                                               size_t src_frame_count,
                                               uint32_t acknowledged_unsupported,
                                               rfc_metadata_report *out_report);
rfc_status rfc_metadata_verify(rfc_format_id target_format,
                               rfc_metadata_policy policy,
                               rfc_exactness_class exactness,
                               const rfc_metadata *src_meta,
                               const rfc_frame *src_frames,
                               size_t src_frame_count,
                               const rfc_metadata *dst_meta,
                               const rfc_frame *dst_frames,
                               size_t dst_frame_count,
                               uint32_t acknowledged_unsupported,
                               rfc_metadata_report *out_report,
                               rfc_sample_mismatch *out_mismatch);

rfc_status rfc_verify_encoded_fresh(rfc_format_id target_format,
                                    const rfc_encoded *encoded,
                                    const rfc_frame *expected_frames,
                                    size_t expected_count,
                                    const rfc_metadata *expected_meta,
                                    rfc_metadata_policy meta_policy,
                                    uint32_t acknowledged_unsupported,
                                    const rfc_setting_value *decode_settings,
                                    size_t decode_setting_count,
                                    const rfc_limits *limits,
                                    const rfc_allocator *alloc,
                                    rfc_verify_report *out_report);

rfc_status rfc_job_verify_report_get(const rfc_job *job, rfc_verify_report *out);
rfc_status rfc_job_set_verified_jpeg_coeffs(
    rfc_job *job,
    const rfc_jpeg_coeff_image *source_coeffs,
    const rfc_jpeg_coeff_image *verified_coeffs,
    const rfc_metadata *verified_meta);
rfc_status rfc_job_set_verified_reconstructed_bytes(
    rfc_job *job,
    const uint8_t *reconstructed_jpeg,
    size_t reconstructed_len,
    const rfc_metadata *reconstructed_meta);

rfc_status rfc_preview_render_srgb8(const rfc_frame *frame,
                                    uint8_t matte_r,
                                    uint8_t matte_g,
                                    uint8_t matte_b,
                                    int apply_orientation,
                                    const rfc_allocator *alloc,
                                    rfc_preview_frame *out_preview);
void rfc_preview_free(rfc_preview_frame *preview);

#ifdef __cplusplus
}
#endif

#endif /* RFC_VERIFY_H */
