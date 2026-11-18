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

static int add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
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
        limits->max_compressed_bytes == 0 ||
        limits->max_metadata_bytes > RFC_MAX_ICC_BYTES ||
        limits->max_expansion_ratio == 0) {
        return RFC_ERR_SETTING_VALUE;
    }
    return RFC_OK;
}

rfc_status rfc_layout_normalize_stride(const rfc_layout *layout,
                                       rfc_layout *out_normalized) {
    if (!layout || !out_normalized || layout->width == 0 ||
        layout->height == 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t bpp = rfc_sample_bytes(layout->format);
    if (bpp == 0) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (layout->width > RFC_MAX_DIMENSION ||
        layout->height > RFC_MAX_DIMENSION) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    rfc_layout norm = *layout;
    if (norm.stride_bytes == 0) {
        uint64_t min_row = 0;
        if (!mul_u64((uint64_t)norm.width, (uint64_t)bpp, &min_row) ||
            min_row > (uint64_t)SIZE_MAX) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        norm.stride_bytes = (size_t)min_row;
    }
    size_t frame_sz = 0;
    rfc_status st = rfc_layout_validate(&norm, &frame_sz);
    if (st == RFC_ERR_OVERFLOW) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (st != RFC_OK) {
        return st;
    }
    *out_normalized = norm;
    return RFC_OK;
}

rfc_status rfc_limits_compute_frame_bytes(const rfc_layout *layout,
                                          size_t frame_count,
                                          uint64_t *out_bytes) {
    if (!layout || !out_bytes || frame_count == 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_layout norm;
    rfc_status st = rfc_layout_normalize_stride(layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    size_t frame_sz = 0;
    st = rfc_layout_validate(&norm, &frame_sz);
    if (st == RFC_ERR_OVERFLOW) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (st != RFC_OK) {
        return st;
    }
    uint64_t total_bytes = 0;
    if (!mul_u64((uint64_t)frame_sz, (uint64_t)frame_count, &total_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    *out_bytes = total_bytes;
    return RFC_OK;
}

rfc_status rfc_limits_check_compressed(const rfc_limits *limits,
                                       uint64_t compressed_bytes) {
    rfc_status st = rfc_limits_validate(limits);
    if (st != RFC_OK) {
        return st;
    }
    if (compressed_bytes > limits->max_compressed_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
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
    uint64_t sum = 0u;
    if (meta->icc_len > RFC_MAX_ICC_BYTES ||
        meta->exif_len > RFC_MAX_ICC_BYTES ||
        meta->xmp_len > RFC_MAX_ICC_BYTES ||
        !add_u64((uint64_t)meta->icc_len, (uint64_t)meta->exif_len, &sum) ||
        !add_u64(sum, (uint64_t)meta->xmp_len, &sum) ||
        sum > RFC_MAX_ICC_BYTES ||
        sum > limits->max_metadata_bytes) {
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
    if (compressed_bytes > limits->max_compressed_bytes) {
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

rfc_status rfc_limits_check_frames(const rfc_limits *limits,
                                   const rfc_frame *frames,
                                   size_t frame_count,
                                   const rfc_metadata *container_meta,
                                   uint64_t compressed_bytes,
                                   int has_compressed) {
    rfc_status st = rfc_limits_validate(limits);
    if (st != RFC_OK) {
        return st;
    }
    if (!frames || frame_count == 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count > RFC_MAX_FRAMES || frame_count > limits->max_frames) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (has_compressed) {
        st = rfc_limits_check_compressed(limits, compressed_bytes);
        if (st != RFC_OK) {
            return st;
        }
    }

    uint64_t total_pixels = 0;
    uint64_t total_bytes = 0;
    for (size_t i = 0; i < frame_count; i++) {
        st = rfc_limits_check_image(limits, &frames[i].layout, 1u);
        if (st != RFC_OK) {
            return st;
        }
        uint64_t fbytes = 0;
        st = rfc_limits_compute_frame_bytes(&frames[i].layout, 1u, &fbytes);
        if (st != RFC_OK) {
            return st;
        }
        if ((uint64_t)frames[i].data_len > fbytes) {
            fbytes = (uint64_t)frames[i].data_len;
        }
        uint64_t fpixels = 0;
        if (!mul_u64((uint64_t)frames[i].layout.width,
                     (uint64_t)frames[i].layout.height, &fpixels) ||
            !add_u64(total_pixels, fpixels, &total_pixels) ||
            total_pixels > limits->max_total_pixels ||
            !add_u64(total_bytes, fbytes, &total_bytes) ||
            total_bytes > limits->max_decoded_bytes) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        st = rfc_limits_check_metadata(limits, &frames[i].metadata);
        if (st != RFC_OK) {
            return st;
        }
    }
    if (container_meta) {
        st = rfc_limits_check_metadata(limits, container_meta);
        if (st != RFC_OK) {
            return st;
        }
    }
    if (has_compressed) {
        st = rfc_limits_check_decompression(limits, compressed_bytes,
                                            total_bytes);
        if (st != RFC_OK) {
            return st;
        }
    }
    return RFC_OK;
}

rfc_status rfc_limits_check_probe(const rfc_limits *limits,
                                  const rfc_image_probe *probe,
                                  uint64_t compressed_bytes,
                                  int has_compressed) {
    rfc_status st = rfc_limits_validate(limits);
    if (st != RFC_OK) {
        return st;
    }
    if (!probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t frame_count = probe->frame_count ? probe->frame_count : 1u;
    if (frame_count > RFC_MAX_FRAMES || frame_count > limits->max_frames) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (has_compressed) {
        st = rfc_limits_check_compressed(limits, compressed_bytes);
        if (st != RFC_OK) {
            return st;
        }
    }
    st = rfc_limits_check_image(limits, &probe->first_frame, 1u);
    if (st != RFC_OK) {
        return st;
    }
    uint64_t first_bytes = 0;
    st = rfc_limits_compute_frame_bytes(&probe->first_frame, 1u, &first_bytes);
    if (st != RFC_OK) {
        return st;
    }
    uint64_t first_pixels = 0;
    if (!mul_u64((uint64_t)probe->first_frame.width,
                 (uint64_t)probe->first_frame.height, &first_pixels)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t total_pixels = first_pixels;
    uint64_t total_bytes = first_bytes;
    if (probe->frame_count == 1u) {
        if ((probe->total_pixels > 0 && probe->total_pixels != first_pixels) ||
            (probe->total_decoded_bytes > 0 &&
             probe->total_decoded_bytes != first_bytes)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
    } else if (probe->frame_count > 1u) {
        uint64_t extra_frames = (uint64_t)(probe->frame_count - 1u);
        if (extra_frames > UINT64_MAX - first_pixels ||
            extra_frames > UINT64_MAX - first_bytes) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        uint64_t min_pixels = first_pixels + extra_frames;
        uint64_t min_bytes = first_bytes + extra_frames;
        if ((probe->total_pixels > 0 && probe->total_pixels < min_pixels) ||
            (probe->total_decoded_bytes > 0 &&
             probe->total_decoded_bytes < min_bytes)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        total_pixels =
            probe->total_pixels > 0 ? probe->total_pixels : min_pixels;
        total_bytes =
            probe->total_decoded_bytes > 0 ? probe->total_decoded_bytes : min_bytes;
    } else {
        if ((probe->total_pixels > 0 && probe->total_pixels < first_pixels) ||
            (probe->total_decoded_bytes > 0 &&
             probe->total_decoded_bytes < first_bytes)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        total_pixels =
            probe->total_pixels > 0 ? probe->total_pixels : first_pixels;
        total_bytes =
            probe->total_decoded_bytes > 0 ? probe->total_decoded_bytes : first_bytes;
    }
    if (total_pixels > limits->max_total_pixels ||
        total_bytes > limits->max_decoded_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (has_compressed) {
        st = rfc_limits_check_decompression(limits, compressed_bytes,
                                            total_bytes);
        if (st != RFC_OK) {
            return st;
        }
    }
    return rfc_limits_check_metadata(limits, &probe->container_meta);
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
            out->mode = RFC_MEM_HOOK_ENFORCED;
            /* As a target the peak is VERIFY: the retained source frame, the
             * encoded output (at most the raw rows for stored deflate), the
             * shared loader's IDAT copy (up to twice the output), its raw
             * filter rows and RGB buffer, and the decoded frame: 8x. As a
             * source the scheduler applies the decode-only figure. */
            out->working_set_multiplier = 8u;
            out->fixed_overhead_bytes = 512ull * 1024ull;
            out->limitation_reason =
                "libpng and zlib allocations route through rfc_budget hooks; "
                "baseline RGB8 shared rf_png_load pre-reserves its working set "
                "in rfc_budget; target VERIFY peak measured at 4.5x-7.7x the "
                "decoded frame (8x reserved), source decode 5x";
            break;
        case RFC_FORMAT_APNG:
            out->mode = RFC_MEM_HOOK_ENFORCED;
            out->working_set_multiplier = 3u;
            out->fixed_overhead_bytes = 512ull * 1024ull;
            out->limitation_reason =
                "libpng and zlib allocations route through rfc_budget hooks";
            break;
        case RFC_FORMAT_JPEG_XL:
            out->mode = RFC_MEM_HOOK_ENFORCED;
            out->working_set_multiplier = 16u;
            out->fixed_overhead_bytes = 16ull * 1024ull * 1024ull;
            out->limitation_reason =
                "libjxl allocations route through JxlMemoryManager rfc_budget hooks "
                "plus pixel-plane and thread-runner reservation";
            break;
        case RFC_FORMAT_RASTERFOLD:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 3u;
            out->fixed_overhead_bytes = 1024ull * 1024ull;
            out->limitation_reason =
                "RasterFold CM internal tables; enforced by rfc_limits preflight and scheduler reservation";
            break;
        case RFC_FORMAT_WEBP:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 27u;
            out->fixed_overhead_bytes = 56ull * 1024ull * 1024ull;
            out->limitation_reason =
                "libwebp uses internal heap allocations (1-thread encode 16x + "
                "28 MiB, multi-thread encode 27x + 56 MiB covering retained "
                "decoded frame, RGBA conversion, 1/2x VP8L worker scratch, and "
                "writer buffer); enforced by rfc_limits preflight, "
                "source-derived rfc_budget_reserve, and thread-aware scheduler "
                "reservation";
            break;
        case RFC_FORMAT_CAMERA_RAW:
        case RFC_FORMAT_DNG:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 6u;
            out->fixed_overhead_bytes = 8ull * 1024ull * 1024ull;
            out->limitation_reason =
                "LibRaw uses internal C++ heap allocations; enforced by rfc_limits preflight and conservative reservation";
            break;
        case RFC_FORMAT_TIFF:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            /* Target VERIFY peak: the retained source frame, the output
             * buffer (doubling, at most twice the output, which is at most
             * the raw samples plus framing), the decoded frame and libtiff's
             * capped heap; source decode is the frame, one strip or tile and
             * the capped heap. */
            out->working_set_multiplier = 6u;
            out->fixed_overhead_bytes = 16ull * 1024ull * 1024ull;
            out->limitation_reason =
                "libtiff allocations are capped per handle by "
                "TIFFOpenOptionsSetMaxCumulatedMemAlloc (8 MiB plus twice the "
                "source when reading, 8 MiB plus two strips when writing) and "
                "that cap is reserved through rfc_budget_reserve; frames, "
                "strip buffers and the output go through the job allocator";
            break;
        case RFC_FORMAT_GIF:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 3u;
            out->fixed_overhead_bytes = 2ull * 1024ull * 1024ull;
            out->limitation_reason =
                "giflib state (LZW tables, colour maps) is reserved as 1 MiB "
                "through rfc_budget_reserve after a zero-allocation structural "
                "pre-scan; frames, rows, palette indices and the output go "
                "through the job allocator";
            break;
        case RFC_FORMAT_OPENEXR:
        case RFC_FORMAT_HEIF:
        case RFC_FORMAT_HEIC:
        case RFC_FORMAT_JPEG2000:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 5u;
            out->fixed_overhead_bytes = 4ull * 1024ull * 1024ull;
            out->limitation_reason =
                "provider uses internal heap allocation; enforced by rfc_limits preflight and conservative reservation";
            break;
        case RFC_FORMAT_JPEG:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 4u;
            out->fixed_overhead_bytes = 8ull * 1024ull * 1024ull;
            out->limitation_reason =
                "libjpeg-turbo pools allocate outside rfc_alloc; the adapter "
                "reserves whole-image coefficient arrays, row buffers and "
                "saved markers from the SOF geometry through rfc_budget_reserve "
                "and caps libjpeg virtual arrays at that reservation";
            break;
        case RFC_FORMAT_AVIF:
            out->mode = RFC_MEM_RESERVATION_AND_LIMITS;
            out->working_set_multiplier = 18u;
            out->fixed_overhead_bytes = 24ull * 1024ull * 1024ull;
            out->limitation_reason =
                "libavif and libaom allocate internally outside rfc_alloc; enforced by rfc_limits preflight and calibrated working-set reservation";
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
