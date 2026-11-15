#include "rfc_job.h"

#include <stddef.h>
#include <stdint.h>

static int mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static size_t sample_storage_bits(rfc_sample_format fmt) {
    size_t bytes = rfc_sample_bytes(fmt);
    if (bytes == 0) {
        return 0;
    }
    switch (fmt) {
        case RFC_SAMPLE_GRAY8:
        case RFC_SAMPLE_RGB8:
        case RFC_SAMPLE_RGBA8:
            return 8u;
        case RFC_SAMPLE_GRAY16:
        case RFC_SAMPLE_RGB16:
        case RFC_SAMPLE_RGBA16:
        case RFC_SAMPLE_RGB_HALF:
        case RFC_SAMPLE_RGBA_HALF:
            return 16u;
        case RFC_SAMPLE_RGB_FLOAT:
        case RFC_SAMPLE_RGBA_FLOAT:
            return 32u;
        default:
            return 0u;
    }
}

void rfc_limits_default(rfc_limits *out) {
    if (!out) {
        return;
    }
    out->max_width = 65536u;
    out->max_height = 65536u;
    out->max_pixels = 512ull * 1024ull * 1024ull;
    out->max_frames = 10000u;
    out->max_total_pixels = 2048ull * 1024ull * 1024ull;
    out->max_decoded_bytes = 2048ull * 1024ull * 1024ull;
    out->max_metadata_bytes = RFC_MAX_ICC_BYTES;
    out->max_compressed_bytes = 512ull * 1024ull * 1024ull;
    out->max_expansion_ratio = 1024u;
}

rfc_status rfc_limits_validate(const rfc_limits *limits) {
    if (!limits) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (limits->max_width == 0 || limits->max_width > RFC_MAX_DIMENSION ||
        limits->max_height == 0 || limits->max_height > RFC_MAX_DIMENSION ||
        limits->max_pixels == 0 ||
        limits->max_frames == 0 || limits->max_frames > RFC_MAX_FRAMES ||
        limits->max_total_pixels == 0 ||
        limits->max_decoded_bytes == 0 ||
        limits->max_metadata_bytes > RFC_MAX_ICC_BYTES ||
        limits->max_expansion_ratio == 0) {
        return RFC_ERR_SETTING_VALUE;
    }
    return RFC_OK;
}

rfc_status rfc_limits_compute_frame_bytes(const rfc_layout *layout,
                                          size_t frame_count,
                                          uint64_t *out_bytes) {
    if (!layout || !out_bytes || layout->width == 0 || layout->height == 0 ||
        frame_count == 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t bpp = rfc_sample_bytes(layout->format);
    size_t sbits = sample_storage_bits(layout->format);
    if (bpp == 0 || sbits == 0) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (layout->alpha != RFC_ALPHA_NONE &&
        layout->alpha != RFC_ALPHA_OPAQUE &&
        layout->alpha != RFC_ALPHA_STRAIGHT &&
        layout->alpha != RFC_ALPHA_PREMULTIPLIED) {
        return RFC_ERR_UNSUPPORTED_ALPHA;
    }
    if (layout->bits_per_sample == 0 || layout->bits_per_sample > sbits) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t min_row = 0;
    if (!mul_u64((uint64_t)layout->width, (uint64_t)bpp, &min_row)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t row_bytes = min_row;
    if (layout->stride_bytes != 0) {
        if ((uint64_t)layout->stride_bytes < min_row) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        row_bytes = (uint64_t)layout->stride_bytes;
    }
    uint64_t frame_bytes = 0;
    uint64_t total_bytes = 0;
    if (!mul_u64(row_bytes, (uint64_t)layout->height, &frame_bytes) ||
        !mul_u64(frame_bytes, (uint64_t)frame_count, &total_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    *out_bytes = total_bytes;
    return RFC_OK;
}

rfc_status rfc_limits_check_image(const rfc_limits *limits,
                                  const rfc_layout *layout,
                                  size_t frame_count) {
    rfc_status st = rfc_limits_validate(limits);
    if (st != RFC_OK) {
        return st;
    }
    if (!layout || frame_count == 0 || layout->width == 0 || layout->height == 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t total_bytes = 0;
    st = rfc_limits_compute_frame_bytes(layout, frame_count, &total_bytes);
    if (st != RFC_OK) {
        return st;
    }
    if (layout->width > RFC_MAX_DIMENSION ||
        layout->height > RFC_MAX_DIMENSION ||
        layout->width > limits->max_width ||
        layout->height > limits->max_height ||
        frame_count > RFC_MAX_FRAMES ||
        frame_count > limits->max_frames) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t pixels = 0;
    uint64_t total_pixels = 0;
    if (!mul_u64((uint64_t)layout->width, (uint64_t)layout->height, &pixels) ||
        pixels > limits->max_pixels ||
        !mul_u64(pixels, (uint64_t)frame_count, &total_pixels) ||
        total_pixels > limits->max_total_pixels ||
        total_bytes > limits->max_decoded_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return RFC_OK;
}

rfc_status rfc_limits_check_metadata(const rfc_limits *limits,
                                     const rfc_metadata *meta) {
    rfc_status st = rfc_limits_validate(limits);
    if (st != RFC_OK) {
        return st;
    }
    if (!meta) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (meta->icc_len > RFC_MAX_ICC_BYTES ||
        meta->icc_len > limits->max_metadata_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return RFC_OK;
}

rfc_status rfc_limits_check_decompression(const rfc_limits *limits,
                                          uint64_t compressed_bytes,
                                          uint64_t decoded_bytes) {
    rfc_status st = rfc_limits_validate(limits);
    if (st != RFC_OK) {
        return st;
    }
    if (limits->max_compressed_bytes > 0 &&
        compressed_bytes > limits->max_compressed_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (decoded_bytes > limits->max_decoded_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (compressed_bytes == 0 && decoded_bytes > 0) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t max_allowed = 0;
    if (mul_u64(compressed_bytes, (uint64_t)limits->max_expansion_ratio,
                &max_allowed)) {
        if (decoded_bytes > max_allowed) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }
    return RFC_OK;
}

rfc_status rfc_query_provider_memory_contract(rfc_format_id id,
                                              rfc_provider_memory_info *out) {
    if (!out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const rfc_capability *cap = rfc_registry_find(id);
    if (!cap) {
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    out->format = cap->id;
    switch (cap->id) {
        case RFC_FORMAT_PNG:
        case RFC_FORMAT_APNG:
            out->mode = RFC_MEM_HOOK_ENFORCED;
            out->working_set_multiplier = 3u;
            out->fixed_overhead_bytes = 512ull * 1024ull;
            out->limitation_reason =
                "libpng and zlib allocations route through rfc_budget hooks";
            break;
        case RFC_FORMAT_JPEG_XL:
            out->mode = RFC_MEM_HOOK_ENFORCED;
            out->working_set_multiplier = 4u;
            out->fixed_overhead_bytes = 2ull * 1024ull * 1024ull;
            out->limitation_reason =
                "libjxl allocations route through JxlMemoryManager rfc_budget hooks";
            break;
        case RFC_FORMAT_RASTERFOLD:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 3u;
            out->fixed_overhead_bytes = 1024ull * 1024ull;
            out->limitation_reason =
                "RasterFold CM internal tables; enforced by rfc_limits preflight and scheduler reservation";
            break;
        case RFC_FORMAT_CAMERA_RAW:
        case RFC_FORMAT_DNG:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 6u;
            out->fixed_overhead_bytes = 8ull * 1024ull * 1024ull;
            out->limitation_reason =
                "LibRaw uses internal C++ heap allocations; enforced by rfc_limits preflight and conservative reservation";
            break;
        case RFC_FORMAT_OPENEXR:
        case RFC_FORMAT_TIFF:
        case RFC_FORMAT_HEIF:
        case RFC_FORMAT_HEIC:
        case RFC_FORMAT_AVIF:
        case RFC_FORMAT_JPEG2000:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 5u;
            out->fixed_overhead_bytes = 4ull * 1024ull * 1024ull;
            out->limitation_reason =
                "provider uses internal heap allocation; enforced by rfc_limits preflight and conservative reservation";
            break;
        default:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 2u;
            out->fixed_overhead_bytes = 256ull * 1024ull;
            out->limitation_reason =
                "internal provider buffers; enforced by rfc_limits preflight and conservative reservation";
            break;
    }
    return RFC_OK;
}
