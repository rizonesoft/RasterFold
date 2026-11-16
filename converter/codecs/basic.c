/* Shipped C11 codec session adapters for D10 T02 §1:
 *   - RFC_FORMAT_RASTERFOLD (public include/rasterfold.h + bounded preflight)
 *   - RFC_FORMAT_BMP (shared D07 rf_bmp_* for 24-bit + GRAY8 / RGBA8 / V4-V5 ICC)
 *   - RFC_FORMAT_PNG (shared D07 rf_png_load with rfc_budget reservation +
 *                     bundled libpng 1.6.59 / zlib 1.4 with rfc_budget hooks)
 *   - RFC_FORMAT_PNM (shared D07 rf_ppm_* for P6 8-bit + full P1..P6)
 *   - RFC_FORMAT_PAM (in-tree P7 BLACKANDWHITE / GRAYSCALE / RGB / RGB_ALPHA) */

#include "rfc_codecs.h"
#include "rfc_job.h"
#include "rfc_verify.h"
#include "rf_shared_io.h"

#include <limits.h>
#include <png.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------------ */
/* Shared allocator, budget, arithmetic, endian, and session primitives     */
/* ------------------------------------------------------------------------ */

static void *stdlib_alloc_cb(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void stdlib_free_cb(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const rfc_allocator k_stdlib_alloc = {
    stdlib_alloc_cb,
    stdlib_free_cb,
    NULL
};

static const rfc_allocator *basic_alloc_of(const rfc_exec_context *exec) {
    if (exec && exec->allocator && exec->allocator->alloc &&
        exec->allocator->free) {
        return exec->allocator;
    }
    return &k_stdlib_alloc;
}

static const rfc_limits *basic_limits_of(const rfc_exec_context *exec,
                                         rfc_limits *scratch) {
    if (exec) {
        return &exec->limits;
    }
    rfc_limits_default(scratch);
    return scratch;
}

typedef struct basic_session {
    rfc_format_id fmt;
    atomic_int cancel_requested;
} basic_session;

/* Note: close_session does not receive exec, so sessions themselves are
 * allocated from calloc/free directly while working buffers and outputs use
 * exec's budget-backed allocator so rfc_job leak/budget tracking holds cleanly. */
static rfc_status open_session_generic(rfc_format_id fmt,
                                       const rfc_exec_context *exec,
                                       void **out_session) {
    (void)exec;
    if (!out_session) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_session = NULL;
    basic_session *s = (basic_session *)calloc(1u, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    s->fmt = fmt;
    atomic_init(&s->cancel_requested, 0);
    *out_session = s;
    return RFC_OK;
}

static void close_session_generic(void *session) {
    free(session);
}

static void cancel_session_generic(void *session) {
    basic_session *s = (basic_session *)session;
    if (s) {
        atomic_store_explicit(&s->cancel_requested, 1, memory_order_release);
    }
}

static int is_cancelled_generic(void *session, const rfc_exec_context *exec) {
    basic_session *s = (basic_session *)session;
    if (s && atomic_load_explicit(&s->cancel_requested, memory_order_acquire)) {
        return 1;
    }
    if (exec && rfc_exec_is_cancelled(exec)) {
        return 1;
    }
    return 0;
}

static void free_encoded_generic(void *session, rfc_encoded *encoded) {
    (void)session;
    rfc_encoded_free(encoded);
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static int mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0u && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd16be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static rfc_status alloc_single_frame(const rfc_exec_context *exec,
                                     const rfc_layout *layout,
                                     const rfc_metadata *meta,
                                     rfc_frame **out_frames,
                                     size_t *out_frame_count,
                                     rfc_metadata *out_meta) {
    if (!layout) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_layout norm;
    rfc_status nst = rfc_layout_normalize_stride(layout, &norm);
    if (nst != RFC_OK) {
        return nst;
    }
    const rfc_allocator *alloc = basic_alloc_of(exec);
    rfc_frame *arr = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(rfc_frame));
    if (!arr) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, sizeof(*arr));
    rfc_status st = exec
                        ? rfc_exec_alloc_frame(exec, &arr[0], &norm, meta)
                        : rfc_frame_alloc(&arr[0], &norm, meta, alloc);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, arr);
        return st;
    }
    *out_frames = arr;
    *out_frame_count = 1u;
    if (out_meta) {
        *out_meta = arr[0].metadata;
    }
    return RFC_OK;
}

static void free_single_frame(const rfc_exec_context *exec,
                              rfc_frame **frames,
                              size_t *count,
                              rfc_metadata *out_meta) {
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (!frames || !*frames) {
        if (count) {
            *count = 0u;
        }
        return;
    }
    const rfc_allocator *alloc = basic_alloc_of(exec);
    rfc_frame_free(&(*frames)[0]);
    alloc->free(alloc->ctx, *frames);
    *frames = NULL;
    if (count) {
        *count = 0u;
    }
}

typedef struct basic_budget_guard {
    rfc_budget *budget;
    size_t bytes;
    int active;
} basic_budget_guard;

static rfc_status reserve_budget(const rfc_exec_context *exec,
                                 size_t bytes,
                                 basic_budget_guard *out_guard) {
    memset(out_guard, 0, sizeof(*out_guard));
    if (!exec || !exec->budget) {
        return RFC_OK;
    }
    rfc_status st = rfc_budget_reserve(exec->budget, bytes);
    if (st != RFC_OK) {
        return st;
    }
    out_guard->budget = exec->budget;
    out_guard->bytes = bytes;
    out_guard->active = 1;
    return RFC_OK;
}

static void release_budget(basic_budget_guard *guard) {
    if (guard && guard->active && guard->budget) {
        rfc_budget_release(guard->budget, guard->bytes);
        guard->budget = NULL;
        guard->bytes = 0u;
        guard->active = 0;
    }
}

typedef struct encode_common_check {
    rfc_metadata_policy meta_policy;
    rfc_exactness_class exactness;
    rfc_layout norm_layout;
    uint32_t eff_bps;
    const uint8_t *eff_icc;
    size_t eff_icc_len;
    rfc_orientation eff_ori;
} encode_common_check;

static uint32_t sample_channels(rfc_sample_format fmt) {
    switch (fmt) {
        case RFC_SAMPLE_GRAY8:
        case RFC_SAMPLE_GRAY16:
            return 1u;
        case RFC_SAMPLE_RGB8:
        case RFC_SAMPLE_RGB16:
            return 3u;
        case RFC_SAMPLE_RGBA8:
        case RFC_SAMPLE_RGBA16:
            return 4u;
        default:
            return 0u;
    }
}

static uint32_t sample_storage_bits(rfc_sample_format fmt) {
    switch (fmt) {
        case RFC_SAMPLE_GRAY8:
        case RFC_SAMPLE_RGB8:
        case RFC_SAMPLE_RGBA8:
            return 8u;
        case RFC_SAMPLE_GRAY16:
        case RFC_SAMPLE_RGB16:
        case RFC_SAMPLE_RGBA16:
            return 16u;
        default:
            return 0u;
    }
}

static rfc_status prepare_encode_common(rfc_format_id fmt,
                                        const rfc_exec_context *exec,
                                        const rfc_frame *frames,
                                        size_t frame_count,
                                        const rfc_metadata *meta,
                                        const rfc_setting_value *settings,
                                        size_t setting_count,
                                        encode_common_check *out) {
    memset(out, 0, sizeof(*out));
    out->meta_policy = RFC_META_POLICY_PRESERVE;
    out->exactness = RFC_EXACT_PIXEL_EXACT;
    out->eff_ori = RFC_ORIENTATION_NORMAL;

    rfc_status st = rfc_registry_validate_settings(
        fmt, setting_count > 0u ? settings : NULL, setting_count, NULL);
    if (st != RFC_OK) {
        return st;
    }
    for (size_t i = 0u; i < setting_count; i++) {
        if (!settings[i].id || !settings[i].value) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(settings[i].id, "metadata") == 0) {
            st = rfc_metadata_policy_parse(settings[i].value,
                                           &out->meta_policy);
            if (st != RFC_OK) {
                return st;
            }
        } else if (strcmp(settings[i].id, "exactness") == 0) {
            st = rfc_exactness_class_parse(settings[i].value,
                                           &out->exactness);
            if (st != RFC_OK) {
                return st;
            }
        }
    }

    if (frame_count == 0u || !frames) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count > 1u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }

    const rfc_frame *fr = &frames[0];
    if (!fr->data) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    st = rfc_layout_normalize_stride(&fr->layout, &out->norm_layout);
    if (st != RFC_OK) {
        return st;
    }
    rfc_layout eff_lay = fr->layout;
    if (eff_lay.stride_bytes == 0u) {
        eff_lay.stride_bytes = out->norm_layout.stride_bytes;
    }
    size_t min_bytes = 0u;
    st = rfc_layout_validate(&eff_lay, &min_bytes);
    if (st != RFC_OK) {
        return st;
    }
    if (fr->data_len < min_bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    out->eff_bps = out->norm_layout.bits_per_sample;

    uint32_t sbits = sample_storage_bits(fr->layout.format);
    uint32_t ch = sample_channels(fr->layout.format);
    if (sbits == 0u || ch == 0u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (ch == 4u) {
        if (fr->layout.alpha == RFC_ALPHA_PREMULTIPLIED) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
        if (fr->layout.alpha != RFC_ALPHA_STRAIGHT) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
    } else if (fr->layout.alpha != RFC_ALPHA_NONE) {
        return RFC_ERR_UNSUPPORTED_ALPHA;
    }

    if (out->eff_bps < sbits) {
        uint16_t pad_mask = (uint16_t)((1u << (sbits - out->eff_bps)) - 1u);
        size_t stride = fr->layout.stride_bytes ? fr->layout.stride_bytes
                                                : out->norm_layout.stride_bytes;
        for (uint32_t y = 0u; y < fr->layout.height; y++) {
            const uint8_t *row = fr->data + (size_t)y * stride;
            if (sbits == 8u) {
                size_t row_s = (size_t)fr->layout.width * (size_t)ch;
                for (size_t k = 0u; k < row_s; k++) {
                    if ((row[k] & (uint8_t)pad_mask) != 0u) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                }
            } else {
                const uint16_t *r16 = (const uint16_t *)row;
                size_t row_s = (size_t)fr->layout.width * (size_t)ch;
                for (size_t k = 0u; k < row_s; k++) {
                    uint16_t v = 0u;
                    memcpy(&v, (const uint8_t *)r16 + k * 2u, 2u);
                    if ((v & pad_mask) != 0u) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                }
            }
        }
    }

    if (fr->metadata.icc_len > 0u && fr->metadata.icc != NULL) {
        out->eff_icc = fr->metadata.icc;
        out->eff_icc_len = fr->metadata.icc_len;
    } else if (meta && meta->icc_len > 0u && meta->icc != NULL) {
        out->eff_icc = meta->icc;
        out->eff_icc_len = meta->icc_len;
    }
    if (fr->metadata.orientation != RFC_ORIENTATION_UNSPECIFIED) {
        out->eff_ori = fr->metadata.orientation;
    } else if (meta && meta->orientation != RFC_ORIENTATION_UNSPECIFIED) {
        out->eff_ori = meta->orientation;
    }

    rfc_limits lim_def;
    const rfc_limits *lim = basic_limits_of(exec, &lim_def);
    if (out->eff_icc_len > 0u) {
        rfc_metadata check_m = {out->eff_icc, out->eff_icc_len, out->eff_ori, 0u};
        st = rfc_limits_check_metadata(lim, &check_m);
        if (st != RFC_OK) {
            return st;
        }
    }

    /* Inside an active rfc_job pipeline, STAGE_VERIFY runs
     * rfc_metadata_check_preflight and rfc_metadata_verify before STAGE_EXPORT
     * so refusals record a populated rfc_verify_report naming unsupported
     * items. Standalone direct encode_session callers lack STAGE_VERIFY and
     * must refuse unacknowledged unsupported metadata here. */
    if (!exec || !exec->job) {
        uint32_t present = rfc_metadata_source_items(meta, frames, frame_count);
        st = rfc_metadata_check_preflight(fmt, out->meta_policy,
                                          out->exactness, present, 0u, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }
    return RFC_OK;
}

static rfc_status copy_bytes_to_encoded(const rfc_exec_context *exec,
                                        const uint8_t *src,
                                        size_t len,
                                        rfc_encoded *out_encoded) {
    const rfc_allocator *alloc = basic_alloc_of(exec);
    uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, len ? len : 1u);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (len > 0u && src) {
        memcpy(buf, src, len);
    }
    out_encoded->data = buf;
    out_encoded->len = len;
    out_encoded->allocator = alloc;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Format 1: RasterFold (.fold)                                             */
/* ------------------------------------------------------------------------ */

static rfc_status fold_preflight_header(const uint8_t *bytes,
                                        size_t size,
                                        const rfc_limits *limits,
                                        rfc_image_probe *out_probe) {
    memset(out_probe, 0, sizeof(*out_probe));
    if (!bytes && size > 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size == 0u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (size >= 3u &&
        (bytes[0] != 'R' || bytes[1] != 'F' || bytes[2] != 'I')) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < RF_HEADER_SIZE) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    uint8_t ver8 = bytes[3];
    uint16_t ver16 = rd16le(bytes + 4);
    if ((uint16_t)ver8 != ver16) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (ver8 != 2u && ver8 != RF_VERSION) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (rd16le(bytes + 6) != RF_HEADER_SIZE) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (rd16le(bytes + 18) != RF_FORMAT_RGB8) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (rf_crc32(bytes, 28u) != rd32le(bytes + 28)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t method = bytes[16];
    if (method > RF_METHOD_SC2 || (method >= RF_METHOD_PAL3 && ver8 != RF_VERSION)) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (bytes[17] != 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint32_t w = rd32le(bytes + 8);
    uint32_t h = rd32le(bytes + 12);
    if (w == 0u || h == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint32_t plen = rd32le(bytes + 20);
    if ((uint64_t)RF_HEADER_SIZE + (uint64_t)plen > (uint64_t)size) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if ((uint64_t)RF_HEADER_SIZE + (uint64_t)plen != (uint64_t)size) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.bits_per_sample = 8u;
    uint64_t row_bytes = 0u;
    if (mul_u64((uint64_t)w, 3ull, &row_bytes) && row_bytes <= SIZE_MAX) {
        out_probe->first_frame.stride_bytes = (size_t)row_bytes;
    }
    out_probe->frame_count = 1u;

    uint64_t pixels = 0u;
    uint64_t dec_bytes = 0u;
    if (!mul_u64((uint64_t)w, (uint64_t)h, &pixels) ||
        !mul_u64(pixels, 3ull, &dec_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    out_probe->total_pixels = pixels;
    out_probe->total_decoded_bytes = dec_bytes;

    rfc_status st =
        rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
    if (st != RFC_OK) {
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }
    if (w > RF_MAX_DIM || h > RF_MAX_DIM || pixels > RF_MAX_PIXELS ||
        (uint64_t)size > RF_MAX_FILE) {
        memset(out_probe, 0, sizeof(*out_probe));
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (method == RF_METHOD_RAW) {
        if ((uint64_t)plen < dec_bytes) {
            memset(out_probe, 0, sizeof(*out_probe));
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if ((uint64_t)plen != dec_bytes) {
            memset(out_probe, 0, sizeof(*out_probe));
            return RFC_ERR_INVALID_ARGUMENT;
        }
    }
    return RFC_OK;
}

static rfc_status map_rf_err(int code) {
    switch (code) {
        case RF_OK:
            return RFC_OK;
        case RF_ERR_LIMIT:
            return RFC_ERR_LIMIT_EXCEEDED;
        case RF_ERR_UNSUPPORTED:
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        case RF_ERR_USAGE:
        case RF_ERR_FORMAT:
        default:
            return RFC_ERR_INVALID_ARGUMENT;
    }
}

static rfc_status fold_open(const rfc_exec_context *exec, void **out_sess) {
    return open_session_generic(RFC_FORMAT_RASTERFOLD, exec, out_sess);
}

static rfc_status fold_probe(void *session,
                             const rfc_exec_context *exec,
                             const uint8_t *bytes,
                             size_t size,
                             rfc_image_probe *out_probe) {
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_limits lim_def;
    return fold_preflight_header(bytes, size, basic_limits_of(exec, &lim_def),
                                 out_probe);
}

static rfc_status fold_decode(void *session,
                              const rfc_exec_context *exec,
                              const uint8_t *bytes,
                              size_t size,
                              const rfc_setting_value *settings,
                              size_t setting_count,
                              rfc_frame **out_frames,
                              size_t *out_frame_count,
                              rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (setting_count > 0u) {
        rfc_status vst = rfc_registry_validate_settings(
            RFC_FORMAT_RASTERFOLD, settings, setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    rfc_image_probe probe;
    rfc_status st = fold_preflight_header(
        bytes, size, basic_limits_of(exec, &lim_def), &probe);
    if (st != RFC_OK) {
        return st;
    }

    if (rf_crc32(bytes + RF_HEADER_SIZE, size - RF_HEADER_SIZE) !=
        rd32le(bytes + 24)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_frame *frames = NULL;
    size_t fcnt = 0u;
    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.orientation = RFC_ORIENTATION_UNSPECIFIED;
    st = alloc_single_frame(exec, &probe.first_frame, &meta, &frames, &fcnt,
                            out_meta);
    if (st != RFC_OK) {
        return st;
    }

    if (is_cancelled_generic(session, exec)) {
        free_single_frame(exec, &frames, &fcnt, out_meta);
        return RFC_ERR_CANCELLED;
    }

    basic_budget_guard ws_guard;
    st = reserve_budget(exec, (size_t)probe.total_decoded_bytes, &ws_guard);
    if (st != RFC_OK) {
        free_single_frame(exec, &frames, &fcnt, out_meta);
        return st;
    }

    uint32_t th = exec ? rfc_exec_threads(exec) : 1u;
    if (th < 1u) {
        th = 1u;
    } else if (th > 9u) {
        th = 9u;
    }
    rf_options opts = {(uint32_t)sizeof(opts), th, {0}};
    rf_pixels px = {0};
    rf_error err = {0};
    int rc = rf_decode_image(bytes, size, &opts, &px, &err);
    if (rc != RF_OK) {
        rf_free(px.data);
        release_budget(&ws_guard);
        free_single_frame(exec, &frames, &fcnt, out_meta);
        return map_rf_err(rc);
    }

    size_t row_bytes = (size_t)probe.first_frame.width * 3u;
    for (uint32_t y = 0u; y < probe.first_frame.height; y++) {
        memcpy(frames[0].data + (size_t)y * frames[0].layout.stride_bytes,
               px.data + (size_t)y * px.stride, row_bytes);
    }
    rf_free(px.data);
    release_budget(&ws_guard);

    if (is_cancelled_generic(session, exec)) {
        free_single_frame(exec, &frames, &fcnt, out_meta);
        return RFC_ERR_CANCELLED;
    }

    *out_frames = frames;
    *out_frame_count = fcnt;
    return RFC_OK;
}

static rfc_status fold_encode(void *session,
                              const rfc_exec_context *exec,
                              const rfc_frame *frames,
                              size_t frame_count,
                              const rfc_metadata *meta,
                              const rfc_setting_value *settings,
                              size_t setting_count,
                              rfc_encoded *out_encoded) {
    if (!out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    encode_common_check chk;
    rfc_status st = prepare_encode_common(
        RFC_FORMAT_RASTERFOLD, exec, frames, frame_count, meta, settings,
        setting_count, &chk);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_frame *fr = &frames[0];
    if (fr->layout.format != RFC_SAMPLE_RGB8 || chk.eff_bps != 8u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (fr->layout.width > RF_MAX_DIM || fr->layout.height > RF_MAX_DIM) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t pixels = 0u;
    uint64_t raw_bytes = 0u;
    if (!mul_u64((uint64_t)fr->layout.width, (uint64_t)fr->layout.height,
                 &pixels) ||
        pixels > RF_MAX_PIXELS || !mul_u64(pixels, 3ull, &raw_bytes) ||
        raw_bytes > SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    basic_budget_guard ws_guard;
    st = reserve_budget(exec, (size_t)raw_bytes, &ws_guard);
    if (st != RFC_OK) {
        return st;
    }

    size_t src_stride = fr->layout.stride_bytes
                            ? fr->layout.stride_bytes
                            : chk.norm_layout.stride_bytes;
    rf_pixels px = {fr->layout.width, fr->layout.height, src_stride,
                    RF_LAYOUT_RGB8, fr->data, fr->data_len};
    uint32_t th = exec ? rfc_exec_threads(exec) : 1u;
    if (th < 1u) {
        th = 1u;
    } else if (th > 9u) {
        th = 9u;
    }
    rf_options opts = {(uint32_t)sizeof(opts), th, {0}};
    rf_buffer enc = {0};
    rf_error err = {0};
    int rc = rf_encode_image(&px, &opts, &enc, &err);
    if (rc != RF_OK) {
        rf_free(enc.data);
        release_budget(&ws_guard);
        return map_rf_err(rc);
    }

    if (is_cancelled_generic(session, exec)) {
        rf_free(enc.data);
        release_budget(&ws_guard);
        return RFC_ERR_CANCELLED;
    }

    st = copy_bytes_to_encoded(exec, enc.data, enc.size, out_encoded);
    rf_free(enc.data);
    release_budget(&ws_guard);
    return st;
}

static const rfc_session_adapter k_fold_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_RASTERFOLD,
    fold_open,
    close_session_generic,
    fold_probe,
    fold_encode,
    fold_decode,
    cancel_session_generic,
    free_encoded_generic
};

/* ------------------------------------------------------------------------ */
/* Format 2: BMP (.bmp)                                                     */
/* ------------------------------------------------------------------------ */

#define BMP_BI_RGB 0u
#define BMP_BI_BITFIELDS 3u
#define BMP_LCS_SRGB 0x73524742u
#define BMP_PROFILE_LINKED 0x4C494E4Bu
#define BMP_PROFILE_EMBEDDED 0x4D424544u

typedef struct bmp_parsed {
    uint32_t width;
    uint32_t height;
    uint16_t bpp;
    uint32_t dib_size;
    uint32_t comp;
    uint32_t off_bits;
    int top_down;
    int bgr_order; /* 1 for standard BGRA, 0 for RGBA bitfields */
    uint64_t src_stride;
    size_t pal_off;
    uint32_t pal_count;
    rfc_sample_format sample;
    rfc_alpha_mode alpha;
    size_t icc_off;
    size_t icc_len;
} bmp_parsed;

static rfc_status bmp_parse_header(const uint8_t *bytes,
                                   size_t size,
                                   const rfc_limits *limits,
                                   bmp_parsed *out,
                                   rfc_image_probe *out_probe) {
    memset(out, 0, sizeof(*out));
    memset(out_probe, 0, sizeof(*out_probe));
    if (!bytes && size > 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size == 0u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (size >= 2u && (bytes[0] != 'B' || bytes[1] != 'M')) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < 18u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    uint32_t file_size = rd32le(bytes + 2);
    uint32_t off_bits = rd32le(bytes + 10);
    uint32_t dib_size = rd32le(bytes + 14);
    if (dib_size == 12u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (dib_size != 40u && dib_size != 52u && dib_size != 56u &&
        dib_size != 108u && dib_size != 124u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if ((uint64_t)14u + (uint64_t)dib_size > (uint64_t)size) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    int32_t raw_w = (int32_t)rd32le(bytes + 18);
    int32_t raw_h = (int32_t)rd32le(bytes + 22);
    uint16_t planes = rd16le(bytes + 26);
    uint16_t bpp = rd16le(bytes + 28);
    uint32_t comp = rd32le(bytes + 30);
    uint32_t clr_used = rd32le(bytes + 46);
    if (raw_w <= 0 || raw_h == 0 || raw_h == INT32_MIN || planes != 1u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint32_t w = (uint32_t)raw_w;
    int top_down = raw_h < 0 ? 1 : 0;
    uint32_t h = top_down ? (uint32_t)(-(int64_t)raw_h) : (uint32_t)raw_h;

    if (bpp == 1u || bpp == 4u || bpp == 16u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (bpp != 8u && bpp != 24u && bpp != 32u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    int bgr_order = 1;
    size_t mask_extra = 0u;
    if (comp == BMP_BI_RGB) {
        /* ok for 8, 24, 32 bpp */
    } else if (comp == BMP_BI_BITFIELDS) {
        if (bpp != 32u) {
            return RFC_ERR_UNSUPPORTED_SAMPLE;
        }
        if (dib_size < 56u) {
            /* BITMAPINFOHEADER/V2 does not carry an explicit alpha mask */
            return RFC_ERR_UNSUPPORTED_SAMPLE;
        }
        uint32_t rm = rd32le(bytes + 54);
        uint32_t gm = rd32le(bytes + 58);
        uint32_t bm = rd32le(bytes + 62);
        uint32_t am = rd32le(bytes + 66);
        if (rm == 0x00FF0000u && gm == 0x0000FF00u && bm == 0x000000FFu &&
            am == 0xFF000000u) {
            bgr_order = 1;
        } else if (rm == 0x000000FFu && gm == 0x0000FF00u &&
                   bm == 0x00FF0000u && am == 0xFF000000u) {
            bgr_order = 0;
        } else {
            return RFC_ERR_UNSUPPORTED_SAMPLE;
        }
    } else {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }

    rfc_sample_format sample = RFC_SAMPLE_RGB8;
    rfc_alpha_mode alpha = RFC_ALPHA_NONE;
    if (bpp == 8u) {
        sample = RFC_SAMPLE_GRAY8;
    } else if (bpp == 32u) {
        sample = RFC_SAMPLE_RGBA8;
        alpha = RFC_ALPHA_STRAIGHT;
    }

    size_t pal_off = (size_t)14u + (size_t)dib_size + mask_extra;
    uint32_t pal_count = 0u;
    if (bpp == 8u) {
        pal_count = (clr_used == 0u) ? 256u : clr_used;
        if (pal_count > 256u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        uint64_t pal_end = (uint64_t)pal_off + (uint64_t)pal_count * 4ull;
        if (pal_end > (uint64_t)size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (pal_end > (uint64_t)off_bits) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        for (uint32_t i = 0u; i < pal_count; i++) {
            const uint8_t *pe = bytes + pal_off + (size_t)i * 4u;
            if (pe[0] != pe[1] || pe[1] != pe[2]) {
                return RFC_ERR_UNSUPPORTED_SAMPLE;
            }
        }
    } else {
        if ((uint64_t)off_bits < (uint64_t)pal_off) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
    }

    /* Check V5 embedded/linked ICC profiles */
    size_t icc_off = 0u;
    size_t icc_len = 0u;
    if (dib_size >= 124u) {
        uint32_t cs_type = rd32le(bytes + 14u + 56u);
        uint32_t p_off = rd32le(bytes + 14u + 112u);
        uint32_t p_sz = rd32le(bytes + 14u + 116u);
        if (cs_type == BMP_PROFILE_LINKED) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
        if (cs_type == BMP_PROFILE_EMBEDDED) {
            if (p_sz == 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if ((uint64_t)14u + (uint64_t)p_off > (uint64_t)size ||
                (uint64_t)p_sz >
                    (uint64_t)size - ((uint64_t)14u + (uint64_t)p_off)) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            if (p_sz > RFC_MAX_ICC_BYTES || p_sz > limits->max_metadata_bytes) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            icc_off = (size_t)14u + (size_t)p_off;
            icc_len = (size_t)p_sz;
        }
    }

    uint64_t row_bits = 0u;
    if (!mul_u64((uint64_t)w, (uint64_t)bpp, &row_bits) ||
        row_bits > UINT64_MAX - 31ull) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t src_stride = ((row_bits + 31ull) / 32ull) * 4ull;
    uint64_t img_bytes = 0u;
    uint64_t end_off = 0u;
    if (!mul_u64(src_stride, (uint64_t)h, &img_bytes) ||
        !add_u64((uint64_t)off_bits, img_bytes, &end_off)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (end_off > (uint64_t)size) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (file_size >= end_off && (uint64_t)file_size > (uint64_t)size) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = sample;
    out_probe->first_frame.alpha = alpha;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.bits_per_sample = 8u;
    uint32_t ch = sample_channels(sample);
    uint64_t out_row = (uint64_t)w * (uint64_t)ch;
    if (out_row <= SIZE_MAX) {
        out_probe->first_frame.stride_bytes = (size_t)out_row;
    }
    out_probe->frame_count = 1u;
    uint64_t pixels = 0u;
    uint64_t dec_bytes = 0u;
    if (!mul_u64((uint64_t)w, (uint64_t)h, &pixels) ||
        !mul_u64(pixels, (uint64_t)ch, &dec_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    out_probe->total_pixels = pixels;
    out_probe->total_decoded_bytes = dec_bytes;
    out_probe->container_meta.orientation = RFC_ORIENTATION_UNSPECIFIED;
    out_probe->container_meta.icc_len = icc_len;

    rfc_status st =
        rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
    if (st != RFC_OK) {
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }

    out->width = w;
    out->height = h;
    out->bpp = bpp;
    out->dib_size = dib_size;
    out->comp = comp;
    out->off_bits = off_bits;
    out->top_down = top_down;
    out->bgr_order = bgr_order;
    out->src_stride = src_stride;
    out->pal_off = pal_off;
    out->pal_count = pal_count;
    out->sample = sample;
    out->alpha = alpha;
    out->icc_off = icc_off;
    out->icc_len = icc_len;
    return RFC_OK;
}

static rfc_status bmp_open(const rfc_exec_context *exec, void **out_sess) {
    return open_session_generic(RFC_FORMAT_BMP, exec, out_sess);
}

static rfc_status bmp_probe(void *session,
                            const rfc_exec_context *exec,
                            const uint8_t *bytes,
                            size_t size,
                            rfc_image_probe *out_probe) {
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_limits lim_def;
    bmp_parsed info;
    rfc_status st = bmp_parse_header(bytes, size, basic_limits_of(exec, &lim_def),
                                     &info, out_probe);
    if (st != RFC_OK) {
        return st;
    }
    if (info.icc_len > 0u) {
        const rfc_allocator *alloc = basic_alloc_of(exec);
        uint8_t *icc = (uint8_t *)alloc->alloc(alloc->ctx, info.icc_len);
        if (!icc) {
            memset(out_probe, 0, sizeof(*out_probe));
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(icc, bytes + info.icc_off, info.icc_len);
        out_probe->container_meta.icc = icc;
        out_probe->container_meta.icc_len = info.icc_len;
    }
    return RFC_OK;
}

static rfc_status bmp_decode(void *session,
                             const rfc_exec_context *exec,
                             const uint8_t *bytes,
                             size_t size,
                             const rfc_setting_value *settings,
                             size_t setting_count,
                             rfc_frame **out_frames,
                             size_t *out_frame_count,
                             rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (setting_count > 0u) {
        rfc_status vst = rfc_registry_validate_settings(
            RFC_FORMAT_BMP, settings, setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    bmp_parsed info;
    rfc_image_probe probe;
    rfc_status st = bmp_parse_header(bytes, size, basic_limits_of(exec, &lim_def),
                                     &info, &probe);
    if (st != RFC_OK) {
        return st;
    }

    /* Validate 8-bit palette indices before pixel allocation. */
    if (info.bpp == 8u) {
        for (uint32_t y = 0u; y < info.height; y++) {
            const uint8_t *src_row =
                bytes + info.off_bits + (size_t)y * (size_t)info.src_stride;
            for (uint32_t x = 0u; x < info.width; x++) {
                if (src_row[x] >= info.pal_count) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
            }
        }
    }

    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.orientation = RFC_ORIENTATION_UNSPECIFIED;
    if (info.icc_len > 0u) {
        meta.icc = bytes + info.icc_off;
        meta.icc_len = info.icc_len;
    }

    rfc_frame *frames = NULL;
    size_t fcnt = 0u;
    st = alloc_single_frame(exec, &probe.first_frame, &meta, &frames, &fcnt,
                            out_meta);
    if (st != RFC_OK) {
        return st;
    }

    /* Delegate canonical 24-bit bottom-up BITMAPINFOHEADER BMP to shared D07 rf_bmp_load. */
    if (info.bpp == 24u && info.dib_size == 40u && info.comp == BMP_BI_RGB &&
        !info.top_down && info.width <= RF_MAX_DIM &&
        info.height <= RF_MAX_DIM && probe.total_pixels <= RF_MAX_PIXELS &&
        (uint64_t)size <= RF_MAX_FILE && info.off_bits >= 54u) {
        uint64_t ws_u64 = (uint64_t)info.width * (uint64_t)info.height * 3ull;
        basic_budget_guard ws_guard;
        st = reserve_budget(exec, (size_t)ws_u64, &ws_guard);
        if (st != RFC_OK) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return st;
        }
        rf_image img = {0};
        rf_error err = {0};
        if (rf_bmp_load(bytes, size, &img, &err) == RF_OK) {
            size_t row_bytes = (size_t)info.width * 3u;
            for (uint32_t y = 0u; y < info.height; y++) {
                memcpy(frames[0].data + (size_t)y * frames[0].layout.stride_bytes,
                       img.rgb + (size_t)y * row_bytes, row_bytes);
            }
            rf_free(img.rgb);
            release_budget(&ws_guard);
            if (is_cancelled_generic(session, exec)) {
                free_single_frame(exec, &frames, &fcnt, out_meta);
                return RFC_ERR_CANCELLED;
            }
            *out_frames = frames;
            *out_frame_count = fcnt;
            return RFC_OK;
        }
        rf_free(img.rgb);
        release_budget(&ws_guard);
        /* Fall through only if rf_bmp_load failed due to harmless non-standard file size check */
    }

    for (uint32_t y = 0u; y < info.height; y++) {
        if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return RFC_ERR_CANCELLED;
        }
        uint32_t src_y = info.top_down ? y : (info.height - 1u - y);
        const uint8_t *srow =
            bytes + info.off_bits + (size_t)src_y * (size_t)info.src_stride;
        uint8_t *drow =
            frames[0].data + (size_t)y * frames[0].layout.stride_bytes;
        if (info.bpp == 8u) {
            for (uint32_t x = 0u; x < info.width; x++) {
                drow[x] = bytes[info.pal_off + (size_t)srow[x] * 4u + 2u];
            }
        } else if (info.bpp == 24u) {
            for (uint32_t x = 0u; x < info.width; x++) {
                drow[x * 3u + 0u] = srow[x * 3u + 2u];
                drow[x * 3u + 1u] = srow[x * 3u + 1u];
                drow[x * 3u + 2u] = srow[x * 3u + 0u];
            }
        } else {
            if (info.bgr_order) {
                for (uint32_t x = 0u; x < info.width; x++) {
                    drow[x * 4u + 0u] = srow[x * 4u + 2u];
                    drow[x * 4u + 1u] = srow[x * 4u + 1u];
                    drow[x * 4u + 2u] = srow[x * 4u + 0u];
                    drow[x * 4u + 3u] = srow[x * 4u + 3u];
                }
            } else {
                memcpy(drow, srow, (size_t)info.width * 4u);
            }
        }
    }

    if (is_cancelled_generic(session, exec)) {
        free_single_frame(exec, &frames, &fcnt, out_meta);
        return RFC_ERR_CANCELLED;
    }
    *out_frames = frames;
    *out_frame_count = fcnt;
    return RFC_OK;
}

static rfc_status bmp_encode(void *session,
                             const rfc_exec_context *exec,
                             const rfc_frame *frames,
                             size_t frame_count,
                             const rfc_metadata *meta,
                             const rfc_setting_value *settings,
                             size_t setting_count,
                             rfc_encoded *out_encoded) {
    if (!out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    encode_common_check chk;
    rfc_status st = prepare_encode_common(
        RFC_FORMAT_BMP, exec, frames, frame_count, meta, settings,
        setting_count, &chk);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_frame *fr = &frames[0];
    if (fr->layout.format != RFC_SAMPLE_GRAY8 &&
        fr->layout.format != RFC_SAMPLE_RGB8 &&
        fr->layout.format != RFC_SAMPLE_RGBA8) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (chk.eff_bps != 8u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    uint32_t w = fr->layout.width;
    uint32_t h = fr->layout.height;
    size_t src_stride = fr->layout.stride_bytes
                            ? fr->layout.stride_bytes
                            : chk.norm_layout.stride_bytes;

    /* Delegate standard RGB8 within CLI limits to shared D07 rf_bmp_store. */
    if (fr->layout.format == RFC_SAMPLE_RGB8 && w <= RF_MAX_DIM &&
        h <= RF_MAX_DIM && (uint64_t)w * (uint64_t)h <= RF_MAX_PIXELS) {
        size_t packed_row = (size_t)w * 3u;
        uint64_t bmp_stride = ((uint64_t)packed_row + 3ull) & ~3ull;
        uint64_t bmp_bytes = 54ull + bmp_stride * (uint64_t)h;
        uint64_t ws_u64 = bmp_bytes;
        uint8_t *packed = NULL;
        const rfc_allocator *alloc = basic_alloc_of(exec);
        if (src_stride != packed_row) {
            uint64_t p_u64 = (uint64_t)packed_row * (uint64_t)h;
            if (p_u64 > SIZE_MAX) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            packed = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)p_u64);
            if (!packed) {
                return RFC_ERR_OUT_OF_MEMORY;
            }
            for (uint32_t y = 0u; y < h; y++) {
                memcpy(packed + (size_t)y * packed_row,
                       fr->data + (size_t)y * src_stride, packed_row);
            }
        }
        if (ws_u64 > SIZE_MAX) {
            if (packed) {
                alloc->free(alloc->ctx, packed);
            }
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        basic_budget_guard ws_guard;
        st = reserve_budget(exec, (size_t)ws_u64, &ws_guard);
        if (st != RFC_OK) {
            if (packed) {
                alloc->free(alloc->ctx, packed);
            }
            return st;
        }
        rf_image img = {w, h, packed ? packed : fr->data};
        uint8_t *bmp = NULL;
        size_t bmp_sz = 0u;
        rf_error err = {0};
        int rc = rf_bmp_store(&img, &bmp, &bmp_sz, &err);
        if (packed) {
            alloc->free(alloc->ctx, packed);
        }
        if (rc != RF_OK) {
            rf_free(bmp);
            release_budget(&ws_guard);
            return map_rf_err(rc);
        }
        if (is_cancelled_generic(session, exec)) {
            rf_free(bmp);
            release_budget(&ws_guard);
            return RFC_ERR_CANCELLED;
        }
        st = copy_bytes_to_encoded(exec, bmp, bmp_sz, out_encoded);
        rf_free(bmp);
        release_budget(&ws_guard);
        return st;
    }

    /* In-tree encoder for GRAY8 (BITMAPINFOHEADER + 256 gray palette),
     * RGBA8 (BITMAPV4HEADER BI_BITFIELDS preserving A==0 RGB), and large RGB8. */
    uint32_t bpp = (fr->layout.format == RFC_SAMPLE_GRAY8)
                       ? 8u
                       : (fr->layout.format == RFC_SAMPLE_RGB8 ? 24u : 32u);
    uint32_t dib_sz = (bpp == 32u) ? 108u : 40u;
    uint32_t pal_bytes = (bpp == 8u) ? (256u * 4u) : 0u;
    uint64_t off_bits = 14ull + (uint64_t)dib_sz + (uint64_t)pal_bytes;
    uint64_t row_bytes = (uint64_t)w * (uint64_t)(bpp / 8u);
    uint64_t dst_stride = (row_bytes + 3ull) & ~3ull;
    uint64_t img_bytes = 0u;
    uint64_t total_u64 = 0u;
    if (!mul_u64(dst_stride, (uint64_t)h, &img_bytes) ||
        !add_u64(off_bits, img_bytes, &total_u64) ||
        total_u64 > UINT32_MAX || total_u64 > SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    const rfc_allocator *alloc = basic_alloc_of(exec);
    uint8_t *out = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)total_u64);
    if (!out) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(out, 0, (size_t)off_bits);

    out[0] = 'B';
    out[1] = 'M';
    wr32le(out + 2, (uint32_t)total_u64);
    wr32le(out + 10, (uint32_t)off_bits);
    wr32le(out + 14, dib_sz);
    wr32le(out + 18, w);
    wr32le(out + 22, h);
    wr16le(out + 26, 1u);
    wr16le(out + 28, (uint16_t)bpp);
    wr32le(out + 30, bpp == 32u ? BMP_BI_BITFIELDS : BMP_BI_RGB);
    wr32le(out + 34, (uint32_t)img_bytes);
    wr32le(out + 38, 2835u);
    wr32le(out + 42, 2835u);
    if (bpp == 8u) {
        wr32le(out + 46, 256u);
        wr32le(out + 50, 256u);
        for (uint32_t i = 0u; i < 256u; i++) {
            uint8_t *pe = out + 54u + (size_t)i * 4u;
            pe[0] = (uint8_t)i;
            pe[1] = (uint8_t)i;
            pe[2] = (uint8_t)i;
            pe[3] = 0u;
        }
    } else if (bpp == 32u) {
        wr32le(out + 54, 0x00FF0000u);
        wr32le(out + 58, 0x0000FF00u);
        wr32le(out + 62, 0x000000FFu);
        wr32le(out + 66, 0xFF000000u);
        wr32le(out + 70, BMP_LCS_SRGB);
    }

    for (uint32_t y = 0u; y < h; y++) {
        if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
            alloc->free(alloc->ctx, out);
            return RFC_ERR_CANCELLED;
        }
        const uint8_t *srow = fr->data + (size_t)y * src_stride;
        uint8_t *drow =
            out + (size_t)off_bits + (size_t)(h - 1u - y) * (size_t)dst_stride;
        size_t pad = (size_t)dst_stride - (size_t)row_bytes;
        if (pad > 0u) {
            memset(drow + (size_t)row_bytes, 0, pad);
        }
        if (bpp == 8u) {
            memcpy(drow, srow, (size_t)w);
        } else if (bpp == 24u) {
            for (uint32_t x = 0u; x < w; x++) {
                drow[x * 3u + 0u] = srow[x * 3u + 2u];
                drow[x * 3u + 1u] = srow[x * 3u + 1u];
                drow[x * 3u + 2u] = srow[x * 3u + 0u];
            }
        } else {
            for (uint32_t x = 0u; x < w; x++) {
                drow[x * 4u + 0u] = srow[x * 4u + 2u];
                drow[x * 4u + 1u] = srow[x * 4u + 1u];
                drow[x * 4u + 2u] = srow[x * 4u + 0u];
                drow[x * 4u + 3u] = srow[x * 4u + 3u];
            }
        }
    }

    out_encoded->data = out;
    out_encoded->len = (size_t)total_u64;
    out_encoded->allocator = alloc;
    return RFC_OK;
}

static const rfc_session_adapter k_bmp_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_BMP,
    bmp_open,
    close_session_generic,
    bmp_probe,
    bmp_encode,
    bmp_decode,
    cancel_session_generic,
    free_encoded_generic
};

/* ------------------------------------------------------------------------ */
/* Format 3: PNG (.png) via shared D07 rf_png_load + bundled libpng/zlib    */
/* ------------------------------------------------------------------------ */

static void *basic_z_alloc(void *opaque, uInt items, uInt size) {
    if (items != 0u && (size_t)size > SIZE_MAX / (size_t)items) {
        return NULL;
    }
    size_t n = (size_t)items * (size_t)size;
    if (opaque) {
        return rfc_budget_alloc((rfc_budget *)opaque, n ? n : 1u);
    }
    return malloc(n ? n : 1u);
}

static void basic_z_free(void *opaque, void *address) {
    if (!address) {
        return;
    }
    if (opaque) {
        rfc_budget_free((rfc_budget *)opaque, address);
    } else {
        free(address);
    }
}

typedef struct basic_png_mem {
    rfc_budget *budget;
    int oom;
    int cancelled;
} basic_png_mem;

static png_voidp basic_png_malloc(png_structp png_ptr, png_alloc_size_t size) {
    basic_png_mem *m = (basic_png_mem *)png_get_mem_ptr(png_ptr);
    if (size > SIZE_MAX) {
        if (m) {
            m->oom = 1;
        }
        return NULL;
    }
    size_t n = (size_t)size;
    void *p = (m && m->budget) ? rfc_budget_alloc(m->budget, n ? n : 1u)
                               : malloc(n ? n : 1u);
    if (!p && m) {
        m->oom = 1;
    }
    return p;
}

static void basic_png_free(png_structp png_ptr, png_voidp ptr) {
    if (!ptr) {
        return;
    }
    basic_png_mem *m = (basic_png_mem *)png_get_mem_ptr(png_ptr);
    if (m && m->budget) {
        rfc_budget_free(m->budget, ptr);
    } else {
        free(ptr);
    }
}

static void basic_png_error(png_structp png_ptr, png_const_charp msg) {
    (void)msg;
    longjmp(png_jmpbuf(png_ptr), 1);
}

static void basic_png_warn(png_structp png_ptr, png_const_charp msg) {
    (void)png_ptr;
    (void)msg;
}

typedef struct png_read_cursor {
    const uint8_t *bytes;
    size_t size;
    size_t pos;
    basic_png_mem *mem;
} png_read_cursor;

static void basic_png_read_fn(png_structp png_ptr,
                              png_bytep out,
                              png_size_t length) {
    png_read_cursor *c = (png_read_cursor *)png_get_io_ptr(png_ptr);
    if (!c || (size_t)length > c->size - c->pos) {
        png_error(png_ptr, "truncated png");
        return;
    }
    if (length > 0u) {
        memcpy(out, c->bytes + c->pos, (size_t)length);
        c->pos += (size_t)length;
    }
}

typedef struct png_write_buf {
    const rfc_allocator *alloc;
    uint8_t *data;
    size_t len;
    size_t cap;
    basic_png_mem *mem;
} png_write_buf;

static void basic_png_write_fn(png_structp png_ptr,
                               png_bytep data,
                               png_size_t length) {
    png_write_buf *w = (png_write_buf *)png_get_io_ptr(png_ptr);
    if (!w || (size_t)length > SIZE_MAX - w->len) {
        png_error(png_ptr, "png write overflow");
        return;
    }
    size_t need = w->len + (size_t)length;
    if (need > w->cap) {
        size_t next = w->cap ? w->cap : 4096u;
        while (next < need) {
            if (next > SIZE_MAX / 2u) {
                next = need;
                break;
            }
            next *= 2u;
        }
        uint8_t *grown = (uint8_t *)w->alloc->alloc(w->alloc->ctx, next);
        if (!grown) {
            if (w->mem) {
                w->mem->oom = 1;
            }
            png_error(png_ptr, "png write oom");
            return;
        }
        if (w->len > 0u && w->data) {
            memcpy(grown, w->data, w->len);
        }
        w->alloc->free(w->alloc->ctx, w->data);
        w->data = grown;
        w->cap = next;
    }
    if (length > 0u) {
        memcpy(w->data + w->len, data, (size_t)length);
        w->len += (size_t)length;
    }
}

static void basic_png_flush_fn(png_structp png_ptr) {
    (void)png_ptr;
}

static rfc_status parse_exif_orientation(const uint8_t *data,
                                         size_t len,
                                         rfc_orientation *out_ori) {
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
    uint32_t ifd_off = little ? rd32le(t + 4) : rd32be(t + 4);
    if ((uint64_t)ifd_off + 2ull > (uint64_t)tlen) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint16_t count =
        little ? rd16le(t + ifd_off) : rd16be(t + ifd_off);
    uint64_t entries_end =
        (uint64_t)ifd_off + 2ull + (uint64_t)count * 12ull;
    if (entries_end > (uint64_t)tlen) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    for (uint16_t i = 0u; i < count; i++) {
        const uint8_t *e = t + ifd_off + 2u + (size_t)i * 12u;
        uint16_t tag = little ? rd16le(e) : rd16be(e);
        if (tag == 0x0112u) {
            uint16_t ty = little ? rd16le(e + 2) : rd16be(e + 2);
            uint32_t cnt = little ? rd32le(e + 4) : rd32be(e + 4);
            if (ty != 3u || cnt != 1u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            uint16_t val = little ? rd16le(e + 8) : rd16be(e + 8);
            if (val < 1u || val > 8u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            *out_ori = (rfc_orientation)val;
            return RFC_OK;
        }
    }
    return RFC_OK;
}

static rfc_status png_inflate_iccp(const rfc_exec_context *exec,
                                   const rfc_allocator *alloc,
                                   const rfc_limits *limits,
                                   const uint8_t *chunk,
                                   size_t clen,
                                   uint8_t **out_icc,
                                   size_t *out_icc_len) {
    *out_icc = NULL;
    *out_icc_len = 0u;
    size_t nul = 0u;
    while (nul < clen && nul < 80u && chunk[nul] != 0u) {
        nul++;
    }
    if (nul == 0u || nul >= 80u || nul + 2u > clen) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (chunk[nul + 1u] != 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const uint8_t *zsrc = chunk + nul + 2u;
    size_t zlen = clen - (nul + 2u);
    if (zlen == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    size_t max_icc = limits->max_metadata_bytes < RFC_MAX_ICC_BYTES
                         ? limits->max_metadata_bytes
                         : RFC_MAX_ICC_BYTES;
    if (max_icc == 0u) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.zalloc = basic_z_alloc;
    zs.zfree = basic_z_free;
    zs.opaque = exec ? (void *)exec->budget : NULL;
    int zrc = inflateInit(&zs);
    if (zrc != Z_OK) {
        return (zrc == Z_MEM_ERROR) ? RFC_ERR_OUT_OF_MEMORY
                                    : RFC_ERR_INVALID_ARGUMENT;
    }

    size_t cap = zlen * 2u;
    if (cap < 256u) {
        cap = 256u;
    }
    if (cap > max_icc + 1u) {
        cap = max_icc + 1u;
    }
    uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        inflateEnd(&zs);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    zs.next_in = (Bytef *)(uintptr_t)zsrc;
    zs.avail_in = (uInt)(zlen > UINT_MAX ? UINT_MAX : zlen);
    size_t total = 0u;

    for (;;) {
        if (total == cap) {
            if (total >= max_icc + 1u) {
                alloc->free(alloc->ctx, buf);
                inflateEnd(&zs);
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            size_t next = cap <= (max_icc + 1u) / 2u ? cap * 2u : max_icc + 1u;
            uint8_t *grown = (uint8_t *)alloc->alloc(alloc->ctx, next);
            if (!grown) {
                alloc->free(alloc->ctx, buf);
                inflateEnd(&zs);
                return RFC_ERR_OUT_OF_MEMORY;
            }
            memcpy(grown, buf, total);
            alloc->free(alloc->ctx, buf);
            buf = grown;
            cap = next;
        }
        zs.next_out = buf + total;
        size_t avail = cap - total;
        zs.avail_out = (uInt)(avail > UINT_MAX ? UINT_MAX : avail);
        zrc = inflate(&zs, Z_NO_FLUSH);
        total = cap - (size_t)zs.avail_out;
        if (total > max_icc) {
            alloc->free(alloc->ctx, buf);
            inflateEnd(&zs);
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        if (zrc == Z_STREAM_END) {
            break;
        }
        if (zrc == Z_OK || zrc == Z_BUF_ERROR) {
            if (zs.avail_in == 0u && zs.avail_out > 0u) {
                alloc->free(alloc->ctx, buf);
                inflateEnd(&zs);
                return RFC_ERR_INVALID_ARGUMENT;
            }
            continue;
        }
        alloc->free(alloc->ctx, buf);
        inflateEnd(&zs);
        return (zrc == Z_MEM_ERROR) ? RFC_ERR_OUT_OF_MEMORY
                                    : RFC_ERR_INVALID_ARGUMENT;
    }
    inflateEnd(&zs);
    if (total == 0u) {
        alloc->free(alloc->ctx, buf);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_icc = buf;
    *out_icc_len = total;
    return RFC_OK;
}

typedef struct png_prescan_info {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t interlace;
    uint64_t idat_total;
    uint64_t idat_cap;
    uint64_t idat_realloc_peak;
    int has_actl;
    int has_iccp;
    int has_exif;
    int has_trns;
    int has_sbit;
    uint8_t *icc;
    size_t icc_len;
    rfc_orientation orientation;
    rfc_sample_format sample;
    rfc_alpha_mode alpha;
} png_prescan_info;

static void png_prescan_free_icc(const rfc_allocator *alloc,
                                 png_prescan_info *p) {
    if (p && p->icc && alloc && alloc->free) {
        alloc->free(alloc->ctx, p->icc);
        p->icc = NULL;
        p->icc_len = 0u;
    }
}

static rfc_status png_prescan(const rfc_exec_context *exec,
                              const rfc_allocator *alloc,
                              const rfc_limits *limits,
                              const uint8_t *bytes,
                              size_t size,
                              png_prescan_info *out,
                              rfc_image_probe *out_probe) {
    static const uint8_t k_sig[8] = {0x89u, 'P', 'N', 'G',
                                     0x0Du, 0x0Au, 0x1Au, 0x0Au};
    memset(out, 0, sizeof(*out));
    memset(out_probe, 0, sizeof(*out_probe));
    out->orientation = RFC_ORIENTATION_UNSPECIFIED;
    if (!bytes && size > 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size == 0u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    size_t sig_chk = size < 8u ? size : 8u;
    if (memcmp(bytes, k_sig, sig_chk) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < 8u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    size_t pos = 8u;
    int saw_ihdr = 0;
    int saw_plte = 0;
    int seen_idat = 0;
    int idat_ended = 0;
    int saw_iend = 0;
    size_t iccp_off = 0u;
    size_t iccp_len = 0u;

    while (pos < size) {
        if (size - pos < 12u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint32_t clen = rd32be(bytes + pos);
        if (clen > 0x7FFFFFFFu) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if ((uint64_t)clen > (uint64_t)(size - pos - 12u)) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint32_t crc_got = rf_crc32(bytes + pos + 4u, 4u + (size_t)clen);
        uint32_t crc_exp = rd32be(bytes + pos + 8u + (size_t)clen);
        if (crc_got != crc_exp) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        const uint8_t *ctype = bytes + pos + 4u;
        const uint8_t *cdata = bytes + pos + 8u;

        if (!saw_ihdr) {
            if (memcmp(ctype, "IHDR", 4) != 0 || clen != 13u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_ihdr = 1;
            out->width = rd32be(cdata);
            out->height = rd32be(cdata + 4);
            out->bit_depth = cdata[8];
            out->color_type = cdata[9];
            uint8_t comp = cdata[10];
            uint8_t flt = cdata[11];
            out->interlace = cdata[12];
            int bd_ok = (out->bit_depth == 1u || out->bit_depth == 2u ||
                         out->bit_depth == 4u || out->bit_depth == 8u ||
                         out->bit_depth == 16u);
            int ct_ok = (out->color_type == 0u || out->color_type == 2u ||
                         out->color_type == 3u || out->color_type == 4u ||
                         out->color_type == 6u);
            int combo = 0;
            if (bd_ok && ct_ok) {
                switch (out->color_type) {
                    case 0u:
                        combo = 1;
                        break;
                    case 2u:
                        combo = (out->bit_depth == 8u || out->bit_depth == 16u);
                        break;
                    case 3u:
                        combo = (out->bit_depth != 16u);
                        break;
                    default:
                        combo = (out->bit_depth == 8u || out->bit_depth == 16u);
                        break;
                }
            }
            if (!combo || out->width == 0u || out->height == 0u ||
                comp != 0u || flt != 0u || out->interlace > 1u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        } else {
            if (memcmp(ctype, "IHDR", 4) == 0) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (memcmp(ctype, "IDAT", 4) != 0 && seen_idat) {
                idat_ended = 1;
            }
            if (memcmp(ctype, "acTL", 4) == 0 ||
                memcmp(ctype, "fcTL", 4) == 0 ||
                memcmp(ctype, "fdAT", 4) == 0) {
                out->has_actl = 1;
            } else if (memcmp(ctype, "PLTE", 4) == 0) {
                if (saw_plte || seen_idat || clen == 0u || (clen % 3u) != 0u ||
                    clen > 768u) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                saw_plte = 1;
            } else if (memcmp(ctype, "IDAT", 4) == 0) {
                if (idat_ended) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                seen_idat = 1;
                if (out->idat_total + (uint64_t)clen > out->idat_cap) {
                    uint64_t need = 0u;
                    if (!add_u64(out->idat_total, (uint64_t)clen, &need) ||
                        need > (UINT64_MAX - 64ull) / 2ull) {
                        return RFC_ERR_LIMIT_EXCEEDED;
                    }
                    uint64_t ncap = need * 2ull + 64ull;
                    uint64_t tr = out->idat_cap + ncap;
                    if (tr > out->idat_realloc_peak) {
                        out->idat_realloc_peak = tr;
                    }
                    out->idat_cap = ncap;
                }
                out->idat_total += (uint64_t)clen;
            } else if (memcmp(ctype, "tRNS", 4) == 0) {
                if (seen_idat) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                out->has_trns = 1;
            } else if (memcmp(ctype, "sBIT", 4) == 0) {
                if (saw_plte || seen_idat || clen == 0u) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                for (uint32_t bi = 0u; bi < clen; bi++) {
                    if (cdata[bi] == 0u || cdata[bi] > out->bit_depth) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                    if (cdata[bi] < out->bit_depth) {
                        out->has_sbit = 1;
                    }
                }
            } else if (memcmp(ctype, "iCCP", 4) == 0) {
                if (saw_plte || seen_idat || out->has_iccp) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                out->has_iccp = 1;
                iccp_off = pos + 8u;
                iccp_len = (size_t)clen;
            } else if (memcmp(ctype, "eXIf", 4) == 0) {
                if (out->has_exif) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                out->has_exif = 1;
                rfc_status est =
                    parse_exif_orientation(cdata, clen, &out->orientation);
                if (est != RFC_OK) {
                    return est;
                }
            } else if (memcmp(ctype, "IEND", 4) == 0) {
                if (clen != 0u) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                saw_iend = 1;
                pos += 12u;
                break;
            } else if ((ctype[0] & 0x20u) == 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
        pos += 12u + (size_t)clen;
    }

    if (!saw_ihdr || !saw_iend) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (pos != size) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (out->color_type == 3u && !saw_plte) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (out->has_actl) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (out->idat_total == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    if (out->color_type == 3u || out->color_type == 4u ||
        out->bit_depth < 8u || out->has_trns || out->has_sbit) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (out->color_type == 0u && out->bit_depth == 8u) {
        out->sample = RFC_SAMPLE_GRAY8;
        out->alpha = RFC_ALPHA_NONE;
    } else if (out->color_type == 0u && out->bit_depth == 16u) {
        out->sample = RFC_SAMPLE_GRAY16;
        out->alpha = RFC_ALPHA_NONE;
    } else if (out->color_type == 2u && out->bit_depth == 8u) {
        out->sample = RFC_SAMPLE_RGB8;
        out->alpha = RFC_ALPHA_NONE;
    } else if (out->color_type == 2u && out->bit_depth == 16u) {
        out->sample = RFC_SAMPLE_RGB16;
        out->alpha = RFC_ALPHA_NONE;
    } else if (out->color_type == 6u && out->bit_depth == 8u) {
        out->sample = RFC_SAMPLE_RGBA8;
        out->alpha = RFC_ALPHA_STRAIGHT;
    } else if (out->color_type == 6u && out->bit_depth == 16u) {
        out->sample = RFC_SAMPLE_RGBA16;
        out->alpha = RFC_ALPHA_STRAIGHT;
    } else {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    if (out->has_iccp) {
        rfc_status ist = png_inflate_iccp(exec, alloc, limits, bytes + iccp_off,
                                          iccp_len, &out->icc, &out->icc_len);
        if (ist != RFC_OK) {
            return ist;
        }
    }

    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = out->sample;
    out_probe->first_frame.alpha = out->alpha;
    out_probe->first_frame.width = out->width;
    out_probe->first_frame.height = out->height;
    out_probe->first_frame.bits_per_sample = out->bit_depth;
    uint32_t ch = sample_channels(out->sample);
    uint64_t bpc = out->bit_depth == 16u ? 2ull : 1ull;
    uint64_t row_bytes = 0u;
    uint64_t pixels = 0u;
    uint64_t dec_bytes = 0u;
    if (!mul_u64((uint64_t)out->width, (uint64_t)ch * bpc, &row_bytes) ||
        !mul_u64((uint64_t)out->width, (uint64_t)out->height, &pixels) ||
        !mul_u64(pixels, (uint64_t)ch * bpc, &dec_bytes)) {
        png_prescan_free_icc(alloc, out);
        memset(out_probe, 0, sizeof(*out_probe));
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (row_bytes <= SIZE_MAX) {
        out_probe->first_frame.stride_bytes = (size_t)row_bytes;
    }
    out_probe->frame_count = 1u;
    out_probe->total_pixels = pixels;
    out_probe->total_decoded_bytes = dec_bytes;
    out_probe->container_meta.orientation = out->orientation;
    /* Keep container_meta.icc NULL here so failure never leaves a dangling
     * pointer; png_probe transfers pinfo.icc ownership only on RFC_OK. */
    out_probe->container_meta.icc = NULL;
    out_probe->container_meta.icc_len = out->icc_len;

    rfc_status st =
        rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
    if (st != RFC_OK) {
        png_prescan_free_icc(alloc, out);
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }
    return RFC_OK;
}

static rfc_status compute_shared_png_working_set(const png_prescan_info *p,
                                                size_t *out_ws) {
    /* src/png.c allocates idat (doubling *2+64), then raw_filter + img.rgb. */
    uint64_t w3 = 0u;
    uint64_t raw_row = 0u;
    uint64_t raw_bytes = 0u;
    uint64_t rgb_bytes = 0u;
    uint64_t sum1 = 0u;
    uint64_t dec_peak = 0u;
    if (!mul_u64((uint64_t)p->width, 3ull, &w3) ||
        !add_u64(1ull, w3, &raw_row) ||
        !mul_u64(raw_row, (uint64_t)p->height, &raw_bytes) ||
        !mul_u64(w3, (uint64_t)p->height, &rgb_bytes) ||
        !add_u64(p->idat_cap, raw_bytes, &sum1) ||
        !add_u64(sum1, rgb_bytes, &dec_peak)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t peak =
        p->idat_realloc_peak > dec_peak ? p->idat_realloc_peak : dec_peak;
    if (peak > SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    *out_ws = (size_t)peak;
    return RFC_OK;
}

static rfc_status png_open(const rfc_exec_context *exec, void **out_sess) {
    return open_session_generic(RFC_FORMAT_PNG, exec, out_sess);
}

#if defined(__GNUC__) || defined(__clang__)
#define BASIC_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define BASIC_NOINLINE __declspec(noinline)
#else
#define BASIC_NOINLINE
#endif

static rfc_status png_probe(void *session,
                            const rfc_exec_context *exec,
                            const uint8_t *bytes,
                            size_t size,
                            rfc_image_probe *out_probe) {
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_limits lim_def;
    const rfc_allocator *alloc = basic_alloc_of(exec);
    png_prescan_info pinfo;
    rfc_status st = png_prescan(exec, alloc, basic_limits_of(exec, &lim_def),
                                bytes, size, &pinfo, out_probe);
    if (st != RFC_OK) {
        png_prescan_free_icc(alloc, &pinfo);
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }
    if (pinfo.icc) {
        out_probe->container_meta.icc = pinfo.icc;
        out_probe->container_meta.icc_len = pinfo.icc_len;
        pinfo.icc = NULL;
        pinfo.icc_len = 0u;
    }
    return RFC_OK;
}

typedef struct png_decode_ctx {
    void *session;
    const rfc_exec_context *exec;
    const png_prescan_info *pinfo;
    rfc_frame *frame;
    basic_png_mem mem;
    png_read_cursor cur;
    png_structp png_ptr;
    png_infop info_ptr;
} png_decode_ctx;

BASIC_NOINLINE static rfc_status png_decode_libpng_run(png_decode_ctx *ctx) {
    ctx->png_ptr = png_create_read_struct_2(
        PNG_LIBPNG_VER_STRING, &ctx->mem, basic_png_error, basic_png_warn,
        &ctx->mem, basic_png_malloc, basic_png_free);
    if (!ctx->png_ptr) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    ctx->info_ptr = png_create_info_struct(ctx->png_ptr);
    if (!ctx->info_ptr) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (setjmp(png_jmpbuf(ctx->png_ptr))) {
        if (ctx->mem.cancelled) {
            return RFC_ERR_CANCELLED;
        }
        return ctx->mem.oom ? RFC_ERR_OUT_OF_MEMORY
                            : RFC_ERR_INVALID_ARGUMENT;
    }

    png_set_read_fn(ctx->png_ptr, &ctx->cur, basic_png_read_fn);
    png_read_info(ctx->png_ptr, ctx->info_ptr);

#ifdef PNG_APNG_SUPPORTED
    {
        png_uint_32 nf = 0u, np = 0u;
        if (png_get_acTL(ctx->png_ptr, ctx->info_ptr, &nf, &np) != 0u) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
    }
#endif

#ifdef PNG_eXIf_SUPPORTED
    {
        png_bytep exif_p = NULL;
        png_uint_32 exif_n = 0u;
        if (png_get_eXIf_1(ctx->png_ptr, ctx->info_ptr, &exif_n, &exif_p) != 0u &&
            exif_p && exif_n > 0u) {
            rfc_orientation ori2 = RFC_ORIENTATION_UNSPECIFIED;
            if (parse_exif_orientation(exif_p, exif_n, &ori2) == RFC_OK &&
                ori2 > RFC_ORIENTATION_NORMAL) {
                ctx->frame->metadata.orientation = ori2;
            }
        }
    }
#endif

    if (ctx->pinfo->bit_depth == 16u) {
        png_set_swap(ctx->png_ptr);
    }
    int passes = png_set_interlace_handling(ctx->png_ptr);
    png_read_update_info(ctx->png_ptr, ctx->info_ptr);

    for (int pass = 0; pass < passes; pass++) {
        for (uint32_t y = 0u; y < ctx->pinfo->height; y++) {
            if ((y & 63u) == 0u &&
                is_cancelled_generic(ctx->session, ctx->exec)) {
                ctx->mem.cancelled = 1;
                png_error(ctx->png_ptr, "cancelled");
            }
            png_bytep row = ctx->frame->data +
                            (size_t)y * ctx->frame->layout.stride_bytes;
            png_read_row(ctx->png_ptr, row, NULL);
        }
    }
    png_read_end(ctx->png_ptr, ctx->info_ptr);
    return RFC_OK;
}

static rfc_status png_decode_libpng(void *session,
                                    const rfc_exec_context *exec,
                                    const uint8_t *bytes,
                                    size_t size,
                                    const png_prescan_info *pinfo,
                                    rfc_frame *frame) {
    png_decode_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.session = session;
    ctx.exec = exec;
    ctx.pinfo = pinfo;
    ctx.frame = frame;
    ctx.mem.budget = exec ? exec->budget : NULL;
    ctx.cur.bytes = bytes;
    ctx.cur.size = size;
    ctx.cur.pos = 0u;
    ctx.cur.mem = &ctx.mem;

    rfc_status st = png_decode_libpng_run(&ctx);
    if (ctx.png_ptr) {
        png_destroy_read_struct(&ctx.png_ptr,
                                ctx.info_ptr ? &ctx.info_ptr : NULL, NULL);
    }
    return st;
}

static rfc_status png_decode(void *session,
                             const rfc_exec_context *exec,
                             const uint8_t *bytes,
                             size_t size,
                             const rfc_setting_value *settings,
                             size_t setting_count,
                             rfc_frame **out_frames,
                             size_t *out_frame_count,
                             rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (setting_count > 0u) {
        rfc_status vst = rfc_registry_validate_settings(
            RFC_FORMAT_PNG, settings, setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    const rfc_allocator *alloc = basic_alloc_of(exec);
    png_prescan_info pinfo;
    rfc_image_probe probe;
    rfc_status st = png_prescan(
        exec, alloc, basic_limits_of(exec, &lim_def), bytes, size, &pinfo,
        &probe);
    if (st != RFC_OK) {
        return st;
    }

    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.icc = pinfo.icc;
    meta.icc_len = pinfo.icc_len;
    meta.orientation = pinfo.orientation;

    rfc_frame *frames = NULL;
    size_t fcnt = 0u;
    st = alloc_single_frame(exec, &probe.first_frame, &meta, &frames, &fcnt,
                            out_meta);
    png_prescan_free_icc(alloc, &pinfo);
    if (st != RFC_OK) {
        return st;
    }

    int can_share_cli =
        (pinfo.color_type == 2u && pinfo.bit_depth == 8u &&
         pinfo.interlace == 0u && !pinfo.has_iccp && !pinfo.has_exif &&
         !pinfo.has_trns && !pinfo.has_sbit && !pinfo.has_actl &&
         pinfo.width <= RF_MAX_DIM && pinfo.height <= RF_MAX_DIM &&
         probe.total_pixels <= RF_MAX_PIXELS && (uint64_t)size <= RF_MAX_FILE);

    if (can_share_cli) {
        size_t ws_bytes = 0u;
        st = compute_shared_png_working_set(&pinfo, &ws_bytes);
        if (st != RFC_OK) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return st;
        }
        basic_budget_guard ws_guard;
        st = reserve_budget(exec, ws_bytes, &ws_guard);
        if (st != RFC_OK) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return st;
        }
        rf_image img = {0};
        rf_error err = {0};
        int rc = rf_png_load(bytes, size, &img, &err);
        if (rc != RF_OK) {
            rf_free(img.rgb);
            release_budget(&ws_guard);
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return map_rf_err(rc);
        }
        size_t row_bytes = (size_t)pinfo.width * 3u;
        for (uint32_t y = 0u; y < pinfo.height; y++) {
            memcpy(frames[0].data + (size_t)y * frames[0].layout.stride_bytes,
                   img.rgb + (size_t)y * row_bytes, row_bytes);
        }
        rf_free(img.rgb);
        release_budget(&ws_guard);
    } else {
        st = png_decode_libpng(session, exec, bytes, size, &pinfo, &frames[0]);
        if (st != RFC_OK) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return st;
        }
        if (out_meta) {
            *out_meta = frames[0].metadata;
        }
    }

    if (is_cancelled_generic(session, exec)) {
        free_single_frame(exec, &frames, &fcnt, out_meta);
        return RFC_ERR_CANCELLED;
    }

    *out_frames = frames;
    *out_frame_count = fcnt;
    return RFC_OK;
}

typedef struct png_encode_ctx {
    void *session;
    const rfc_exec_context *exec;
    const rfc_frame *fr;
    const encode_common_check *chk;
    uint32_t sbits;
    int color_type;
    basic_png_mem mem;
    png_write_buf out_buf;
    png_structp png_ptr;
    png_infop info_ptr;
} png_encode_ctx;

BASIC_NOINLINE static rfc_status png_encode_run(png_encode_ctx *ctx) {
    ctx->png_ptr = png_create_write_struct_2(
        PNG_LIBPNG_VER_STRING, &ctx->mem, basic_png_error, basic_png_warn,
        &ctx->mem, basic_png_malloc, basic_png_free);
    if (!ctx->png_ptr) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    ctx->info_ptr = png_create_info_struct(ctx->png_ptr);
    if (!ctx->info_ptr) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (setjmp(png_jmpbuf(ctx->png_ptr))) {
        if (ctx->mem.cancelled) {
            return RFC_ERR_CANCELLED;
        }
        return ctx->mem.oom ? RFC_ERR_OUT_OF_MEMORY
                            : RFC_ERR_INVALID_ARGUMENT;
    }

    png_set_write_fn(ctx->png_ptr, &ctx->out_buf, basic_png_write_fn,
                     basic_png_flush_fn);
    png_set_IHDR(ctx->png_ptr, ctx->info_ptr, ctx->fr->layout.width,
                 ctx->fr->layout.height, (int)ctx->sbits, ctx->color_type,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

    if (ctx->chk->meta_policy == RFC_META_POLICY_PRESERVE &&
        ctx->chk->eff_icc && ctx->chk->eff_icc_len > 0u) {
        if (ctx->chk->eff_icc_len > UINT32_MAX) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        png_set_iCCP(ctx->png_ptr, ctx->info_ptr, "ICC Profile", 0,
                     (png_const_bytep)ctx->chk->eff_icc,
                     (png_uint_32)ctx->chk->eff_icc_len);
        {
            png_charp got_name = NULL;
            int got_comp = 0;
            png_bytep got_prof = NULL;
            png_uint_32 got_len = 0u;
            if (png_get_iCCP(ctx->png_ptr, ctx->info_ptr, &got_name, &got_comp,
                             &got_prof, &got_len) != PNG_INFO_iCCP ||
                got_len != (png_uint_32)ctx->chk->eff_icc_len) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
    }

    png_write_info(ctx->png_ptr, ctx->info_ptr);
    if (ctx->sbits == 16u) {
        png_set_swap(ctx->png_ptr);
    }

    size_t stride = ctx->fr->layout.stride_bytes
                        ? ctx->fr->layout.stride_bytes
                        : ctx->chk->norm_layout.stride_bytes;
    for (uint32_t y = 0u; y < ctx->fr->layout.height; y++) {
        if ((y & 63u) == 0u &&
            is_cancelled_generic(ctx->session, ctx->exec)) {
            ctx->mem.cancelled = 1;
            png_error(ctx->png_ptr, "cancelled");
        }
        png_write_row(ctx->png_ptr,
                      (png_const_bytep)(ctx->fr->data + (size_t)y * stride));
    }
    png_write_end(ctx->png_ptr, ctx->info_ptr);
    return RFC_OK;
}

static rfc_status png_encode(void *session,
                             const rfc_exec_context *exec,
                             const rfc_frame *frames,
                             size_t frame_count,
                             const rfc_metadata *meta,
                             const rfc_setting_value *settings,
                             size_t setting_count,
                             rfc_encoded *out_encoded) {
    if (!out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    encode_common_check chk;
    rfc_status st = prepare_encode_common(
        RFC_FORMAT_PNG, exec, frames, frame_count, meta, settings,
        setting_count, &chk);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_frame *fr = &frames[0];
    uint32_t sbits = sample_storage_bits(fr->layout.format);
    if (sbits == 0u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (chk.eff_bps != sbits) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    int color_type = -1;
    switch (fr->layout.format) {
        case RFC_SAMPLE_GRAY8:
        case RFC_SAMPLE_GRAY16:
            color_type = PNG_COLOR_TYPE_GRAY;
            break;
        case RFC_SAMPLE_RGB8:
        case RFC_SAMPLE_RGB16:
            color_type = PNG_COLOR_TYPE_RGB;
            break;
        case RFC_SAMPLE_RGBA8:
        case RFC_SAMPLE_RGBA16:
            color_type = PNG_COLOR_TYPE_RGBA;
            break;
        default:
            return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    const rfc_allocator *alloc = basic_alloc_of(exec);
    png_encode_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.session = session;
    ctx.exec = exec;
    ctx.fr = fr;
    ctx.chk = &chk;
    ctx.sbits = sbits;
    ctx.color_type = color_type;
    ctx.mem.budget = exec ? exec->budget : NULL;
    ctx.out_buf.alloc = alloc;
    ctx.out_buf.mem = &ctx.mem;

    st = png_encode_run(&ctx);
    if (ctx.png_ptr) {
        png_destroy_write_struct(&ctx.png_ptr,
                                 ctx.info_ptr ? &ctx.info_ptr : NULL);
    }
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, ctx.out_buf.data);
        return st;
    }

    out_encoded->data = ctx.out_buf.data;
    out_encoded->len = ctx.out_buf.len;
    out_encoded->allocator = alloc;
    return RFC_OK;
}

static const rfc_session_adapter k_png_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_PNG,
    png_open,
    close_session_generic,
    png_probe,
    png_encode,
    png_decode,
    cancel_session_generic,
    free_encoded_generic
};

/* ------------------------------------------------------------------------ */
/* Format 4 & 5: PNM (P1..P6) and PAM (P7)                                  */
/* ------------------------------------------------------------------------ */

static int pnm_is_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

static int maxval_to_bps(uint32_t maxval, uint32_t *out_bps) {
    for (uint32_t b = 1u; b <= 16u; b++) {
        if (maxval == ((1u << b) - 1u)) {
            *out_bps = b;
            return 1;
        }
    }
    return 0;
}

static rfc_status pnm_next_token(const uint8_t *bytes,
                                 size_t size,
                                 size_t *pos,
                                 char *buf,
                                 size_t buf_cap,
                                 int *had_comment,
                                 size_t *out_after_pos) {
    size_t i = *pos;
    for (;;) {
        if (i >= size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (bytes[i] == '#') {
            if (had_comment) {
                *had_comment = 1;
            }
            i++;
            while (i < size && bytes[i] != '\n' && bytes[i] != '\r') {
                i++;
            }
            continue;
        }
        if (pnm_is_ws(bytes[i])) {
            i++;
            continue;
        }
        break;
    }
    size_t len = 0u;
    while (i < size && !pnm_is_ws(bytes[i]) && bytes[i] != '#') {
        if (len + 1u >= buf_cap) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        buf[len++] = (char)bytes[i++];
    }
    buf[len] = '\0';
    *pos = i;
    if (out_after_pos) {
        *out_after_pos = i;
    }
    return RFC_OK;
}

static int parse_u32_decimal(const char *s, uint32_t *out) {
    if (!s || !*s) {
        return 0;
    }
    uint64_t v = 0u;
    for (size_t i = 0u; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        v = v * 10u + (uint64_t)(s[i] - '0');
        if (v > UINT32_MAX) {
            return 0;
        }
    }
    *out = (uint32_t)v;
    return 1;
}

typedef struct pnm_parsed {
    int magic; /* 1..6 */
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t maxval;
    uint32_t bps;
    int binary;
    int had_comment;
    size_t raster_off;
    rfc_sample_format sample;
} pnm_parsed;

static rfc_status pnm_parse_header(const uint8_t *bytes,
                                   size_t size,
                                   const rfc_limits *limits,
                                   pnm_parsed *out,
                                   rfc_image_probe *out_probe) {
    memset(out, 0, sizeof(*out));
    memset(out_probe, 0, sizeof(*out_probe));
    if (!bytes && size > 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size == 0u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (bytes[0] != 'P') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < 2u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (bytes[1] == '7') {
        return RFC_ERR_FORMAT_DISAGREEMENT;
    }
    if (bytes[1] < '1' || bytes[1] > '6') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    out->magic = bytes[1] - '0';
    out->binary = (out->magic >= 4);
    out->channels = (out->magic == 3 || out->magic == 6) ? 3u : 1u;

    size_t pos = 2u;
    if (pos >= size) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (!pnm_is_ws(bytes[pos]) && bytes[pos] != '#') {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    char tok[64];
    rfc_status st = pnm_next_token(bytes, size, &pos, tok, sizeof(tok),
                                   &out->had_comment, NULL);
    if (st != RFC_OK) {
        return st;
    }
    if (!parse_u32_decimal(tok, &out->width) || out->width == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    st = pnm_next_token(bytes, size, &pos, tok, sizeof(tok),
                        &out->had_comment, NULL);
    if (st != RFC_OK) {
        return st;
    }
    if (!parse_u32_decimal(tok, &out->height) || out->height == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    if (out->magic == 1 || out->magic == 4) {
        out->maxval = 1u;
        out->bps = 1u;
    } else {
        st = pnm_next_token(bytes, size, &pos, tok, sizeof(tok),
                            &out->had_comment, NULL);
        if (st != RFC_OK) {
            return st;
        }
        if (!parse_u32_decimal(tok, &out->maxval)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (out->maxval == 0u || out->maxval > 65535u ||
            !maxval_to_bps(out->maxval, &out->bps)) {
            return RFC_ERR_UNSUPPORTED_SAMPLE;
        }
    }

    if (out->binary) {
        /* Netpbm binary formats carry a single whitespace byte after header
         * or # comments immediately preceding that single whitespace? Wait:
         * per Netpbm specification, a single whitespace character terminates
         * height (P4) or maxval (P5/P6) and raster follows immediately. */
        if (pos >= size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (!pnm_is_ws(bytes[pos])) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        out->raster_off = pos + 1u;
    } else {
        out->raster_off = pos;
    }

    if (out->channels == 1u) {
        out->sample = (out->bps <= 8u) ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_GRAY16;
    } else {
        out->sample = (out->bps <= 8u) ? RFC_SAMPLE_RGB8 : RFC_SAMPLE_RGB16;
    }

    uint32_t sbytes = (out->bps <= 8u) ? 1u : 2u;
    uint64_t row_bytes = 0u;
    uint64_t pixels = 0u;
    uint64_t dec_bytes = 0u;
    if (!mul_u64((uint64_t)out->width, (uint64_t)out->channels * sbytes,
                 &row_bytes) ||
        !mul_u64((uint64_t)out->width, (uint64_t)out->height, &pixels) ||
        !mul_u64(pixels, (uint64_t)out->channels * sbytes, &dec_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = out->sample;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = out->width;
    out_probe->first_frame.height = out->height;
    out_probe->first_frame.bits_per_sample = (uint8_t)out->bps;
    if (row_bytes <= SIZE_MAX) {
        out_probe->first_frame.stride_bytes = (size_t)row_bytes;
    }
    out_probe->frame_count = 1u;
    out_probe->total_pixels = pixels;
    out_probe->total_decoded_bytes = dec_bytes;
    out_probe->container_meta.orientation = RFC_ORIENTATION_UNSPECIFIED;

    st = rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
    if (st != RFC_OK) {
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }

    if (out->binary) {
        uint64_t need = 0u;
        if (out->magic == 4) {
            uint64_t pbm_row = ((uint64_t)out->width + 7ull) / 8ull;
            if (!mul_u64(pbm_row, (uint64_t)out->height, &need)) {
                memset(out_probe, 0, sizeof(*out_probe));
                return RFC_ERR_LIMIT_EXCEEDED;
            }
        } else {
            need = dec_bytes;
        }
        uint64_t avail = (uint64_t)(size - out->raster_off);
        if (avail < need) {
            memset(out_probe, 0, sizeof(*out_probe));
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (avail > need) {
            memset(out_probe, 0, sizeof(*out_probe));
            return RFC_ERR_INVALID_ARGUMENT;
        }
    }
    return RFC_OK;
}

static rfc_status pnm_open(const rfc_exec_context *exec, void **out_sess) {
    return open_session_generic(RFC_FORMAT_PNM, exec, out_sess);
}

static rfc_status pnm_probe(void *session,
                            const rfc_exec_context *exec,
                            const uint8_t *bytes,
                            size_t size,
                            rfc_image_probe *out_probe) {
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_limits lim_def;
    pnm_parsed info;
    return pnm_parse_header(bytes, size, basic_limits_of(exec, &lim_def),
                            &info, out_probe);
}

static rfc_status pnm_decode_ascii_uint(const uint8_t *bytes,
                                        size_t size,
                                        size_t *pos,
                                        uint32_t maxval,
                                        uint32_t *out) {
    char tok[32];
    rfc_status st =
        pnm_next_token(bytes, size, pos, tok, sizeof(tok), NULL, NULL);
    if (st != RFC_OK) {
        return st;
    }
    uint32_t v = 0u;
    if (!parse_u32_decimal(tok, &v) || v > maxval) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out = v;
    return RFC_OK;
}

static rfc_status pnm_decode_ascii_pbm_bit(const uint8_t *bytes,
                                          size_t size,
                                          size_t *pos,
                                          uint8_t *out_bit) {
    size_t i = *pos;
    for (;;) {
        if (i >= size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (bytes[i] == '#') {
            i++;
            while (i < size && bytes[i] != '\n' && bytes[i] != '\r') {
                i++;
            }
            continue;
        }
        if (pnm_is_ws(bytes[i])) {
            i++;
            continue;
        }
        break;
    }
    if (bytes[i] != '0' && bytes[i] != '1') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_bit = (uint8_t)(bytes[i] - '0');
    *pos = i + 1u;
    return RFC_OK;
}

static rfc_status check_trailing_ascii_ws(const uint8_t *bytes,
                                          size_t size,
                                          size_t pos) {
    while (pos < size) {
        if (bytes[pos] == '#') {
            pos++;
            while (pos < size && bytes[pos] != '\n' && bytes[pos] != '\r') {
                pos++;
            }
            continue;
        }
        if (pnm_is_ws(bytes[pos])) {
            pos++;
            continue;
        }
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

static rfc_status pnm_decode(void *session,
                             const rfc_exec_context *exec,
                             const uint8_t *bytes,
                             size_t size,
                             const rfc_setting_value *settings,
                             size_t setting_count,
                             rfc_frame **out_frames,
                             size_t *out_frame_count,
                             rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (setting_count > 0u) {
        rfc_status vst = rfc_registry_validate_settings(
            RFC_FORMAT_PNM, settings, setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    pnm_parsed info;
    rfc_image_probe probe;
    rfc_status st = pnm_parse_header(bytes, size, basic_limits_of(exec, &lim_def),
                                     &info, &probe);
    if (st != RFC_OK) {
        return st;
    }

    /* Validate binary sample bounds BEFORE allocating pixels where maxval < (2^sbits - 1). */
    if (info.binary && info.magic != 4) {
        const uint8_t *p = bytes + info.raster_off;
        size_t n_samples = (size_t)info.width * (size_t)info.height *
                           (size_t)info.channels;
        if (info.bps <= 8u && info.maxval < 255u) {
            for (size_t k = 0u; k < n_samples; k++) {
                if (p[k] > info.maxval) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
            }
        } else if (info.bps > 8u && info.maxval < 65535u) {
            for (size_t k = 0u; k < n_samples; k++) {
                if (rd16be(p + k * 2u) > info.maxval) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
            }
        }
    }

    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_UNSPECIFIED, 0u};
    rfc_frame *frames = NULL;
    size_t fcnt = 0u;
    st = alloc_single_frame(exec, &probe.first_frame, &meta, &frames, &fcnt,
                            out_meta);
    if (st != RFC_OK) {
        return st;
    }

    /* Share D07 CLI rf_ppm_load on canonical P6 maxval==255 without comments. */
    if (info.magic == 6 && info.maxval == 255u && !info.had_comment &&
        info.width <= RF_MAX_DIM && info.height <= RF_MAX_DIM &&
        probe.total_pixels <= RF_MAX_PIXELS &&
        (uint64_t)size <= RF_MAX_FILE) {
        basic_budget_guard ws_guard;
        st = reserve_budget(exec, (size_t)probe.total_decoded_bytes, &ws_guard);
        if (st != RFC_OK) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return st;
        }
        rf_image img = {0};
        rf_error err = {0};
        int rc = rf_ppm_load(bytes, size, &img, &err);
        if (rc != RF_OK) {
            rf_free(img.rgb);
            release_budget(&ws_guard);
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return map_rf_err(rc);
        }
        size_t row_bytes = (size_t)info.width * 3u;
        for (uint32_t y = 0u; y < info.height; y++) {
            memcpy(frames[0].data + (size_t)y * frames[0].layout.stride_bytes,
                   img.rgb + (size_t)y * row_bytes, row_bytes);
        }
        rf_free(img.rgb);
        release_budget(&ws_guard);
        if (is_cancelled_generic(session, exec)) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return RFC_ERR_CANCELLED;
        }
        *out_frames = frames;
        *out_frame_count = fcnt;
        return RFC_OK;
    }

    if (info.magic == 1) {
        size_t pos = info.raster_off;
        for (uint32_t y = 0u; y < info.height; y++) {
            if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
                free_single_frame(exec, &frames, &fcnt, out_meta);
                return RFC_ERR_CANCELLED;
            }
            uint8_t *drow =
                frames[0].data + (size_t)y * frames[0].layout.stride_bytes;
            for (uint32_t x = 0u; x < info.width; x++) {
                uint8_t bit = 0u;
                st = pnm_decode_ascii_pbm_bit(bytes, size, &pos, &bit);
                if (st != RFC_OK) {
                    free_single_frame(exec, &frames, &fcnt, out_meta);
                    return st;
                }
                /* PBM bit 0 = white (1<<7 = 0x80), bit 1 = black (0x00). */
                drow[x] = bit ? 0x00u : 0x80u;
            }
        }
        st = check_trailing_ascii_ws(bytes, size, pos);
        if (st != RFC_OK) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return st;
        }
    } else if (info.magic == 2 || info.magic == 3) {
        size_t pos = info.raster_off;
        uint32_t shift =
            (info.bps <= 8u) ? (8u - info.bps) : (16u - info.bps);
        size_t row_s = (size_t)info.width * (size_t)info.channels;
        for (uint32_t y = 0u; y < info.height; y++) {
            if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
                free_single_frame(exec, &frames, &fcnt, out_meta);
                return RFC_ERR_CANCELLED;
            }
            uint8_t *drow =
                frames[0].data + (size_t)y * frames[0].layout.stride_bytes;
            for (size_t k = 0u; k < row_s; k++) {
                uint32_t v = 0u;
                st = pnm_decode_ascii_uint(bytes, size, &pos, info.maxval, &v);
                if (st != RFC_OK) {
                    free_single_frame(exec, &frames, &fcnt, out_meta);
                    return st;
                }
                if (info.bps <= 8u) {
                    drow[k] = (uint8_t)(v << shift);
                } else {
                    uint16_t u16 = (uint16_t)(v << shift);
                    memcpy(drow + k * 2u, &u16, 2u);
                }
            }
        }
        st = check_trailing_ascii_ws(bytes, size, pos);
        if (st != RFC_OK) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return st;
        }
    } else if (info.magic == 4) {
        size_t srow_bytes = ((size_t)info.width + 7u) / 8u;
        const uint8_t *src = bytes + info.raster_off;
        for (uint32_t y = 0u; y < info.height; y++) {
            if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
                free_single_frame(exec, &frames, &fcnt, out_meta);
                return RFC_ERR_CANCELLED;
            }
            const uint8_t *srow = src + (size_t)y * srow_bytes;
            uint8_t *drow =
                frames[0].data + (size_t)y * frames[0].layout.stride_bytes;
            for (uint32_t x = 0u; x < info.width; x++) {
                uint8_t bit = (uint8_t)((srow[x >> 3] >> (7u - (x & 7u))) & 1u);
                drow[x] = bit ? 0x00u : 0x80u;
            }
        }
    } else {
        /* P5 / P6 binary */
        const uint8_t *src = bytes + info.raster_off;
        uint32_t shift =
            (info.bps <= 8u) ? (8u - info.bps) : (16u - info.bps);
        size_t row_s = (size_t)info.width * (size_t)info.channels;
        size_t srow_bytes = row_s * (info.bps <= 8u ? 1u : 2u);
        for (uint32_t y = 0u; y < info.height; y++) {
            if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
                free_single_frame(exec, &frames, &fcnt, out_meta);
                return RFC_ERR_CANCELLED;
            }
            const uint8_t *srow = src + (size_t)y * srow_bytes;
            uint8_t *drow =
                frames[0].data + (size_t)y * frames[0].layout.stride_bytes;
            if (info.bps <= 8u) {
                if (shift == 0u) {
                    memcpy(drow, srow, row_s);
                } else {
                    for (size_t k = 0u; k < row_s; k++) {
                        drow[k] = (uint8_t)(srow[k] << shift);
                    }
                }
            } else {
                for (size_t k = 0u; k < row_s; k++) {
                    uint16_t u16 =
                        (uint16_t)(rd16be(srow + k * 2u) << shift);
                    memcpy(drow + k * 2u, &u16, 2u);
                }
            }
        }
    }

    if (is_cancelled_generic(session, exec)) {
        free_single_frame(exec, &frames, &fcnt, out_meta);
        return RFC_ERR_CANCELLED;
    }
    *out_frames = frames;
    *out_frame_count = fcnt;
    return RFC_OK;
}

static rfc_status pnm_encode(void *session,
                             const rfc_exec_context *exec,
                             const rfc_frame *frames,
                             size_t frame_count,
                             const rfc_metadata *meta,
                             const rfc_setting_value *settings,
                             size_t setting_count,
                             rfc_encoded *out_encoded) {
    if (!out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    encode_common_check chk;
    rfc_status st = prepare_encode_common(
        RFC_FORMAT_PNM, exec, frames, frame_count, meta, settings,
        setting_count, &chk);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_frame *fr = &frames[0];
    if (fr->layout.format != RFC_SAMPLE_GRAY8 &&
        fr->layout.format != RFC_SAMPLE_GRAY16 &&
        fr->layout.format != RFC_SAMPLE_RGB8 &&
        fr->layout.format != RFC_SAMPLE_RGB16) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    uint32_t w = fr->layout.width;
    uint32_t h = fr->layout.height;
    uint32_t bps = chk.eff_bps;
    uint32_t sbits = sample_storage_bits(fr->layout.format);
    uint32_t ch = sample_channels(fr->layout.format);
    if (sbits == 0u || ch == 0u || (sbits == 16u && bps <= 8u)) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    size_t src_stride = fr->layout.stride_bytes
                            ? fr->layout.stride_bytes
                            : chk.norm_layout.stride_bytes;

    /* Share D07 rf_ppm_store for canonical RGB8 8-bit within CLI bounds. */
    if (fr->layout.format == RFC_SAMPLE_RGB8 && bps == 8u && w <= RF_MAX_DIM &&
        h <= RF_MAX_DIM && (uint64_t)w * (uint64_t)h <= RF_MAX_PIXELS) {
        size_t packed_row = (size_t)w * 3u;
        uint8_t *packed = NULL;
        const rfc_allocator *alloc = basic_alloc_of(exec);
        if (src_stride != packed_row) {
            uint64_t p_u64 = (uint64_t)packed_row * (uint64_t)h;
            if (p_u64 > SIZE_MAX) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            packed = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)p_u64);
            if (!packed) {
                return RFC_ERR_OUT_OF_MEMORY;
            }
            for (uint32_t y = 0u; y < h; y++) {
                memcpy(packed + (size_t)y * packed_row,
                       fr->data + (size_t)y * src_stride, packed_row);
            }
        }
        uint64_t ws_u64 = 64ull + (uint64_t)packed_row * (uint64_t)h;
        basic_budget_guard ws_guard;
        st = reserve_budget(exec, ws_u64 <= SIZE_MAX ? (size_t)ws_u64 : SIZE_MAX,
                            &ws_guard);
        if (st != RFC_OK) {
            if (packed) {
                alloc->free(alloc->ctx, packed);
            }
            return st;
        }
        rf_image img = {w, h, packed ? packed : fr->data};
        uint8_t *ppm = NULL;
        size_t ppm_sz = 0u;
        rf_error err = {0};
        int rc = rf_ppm_store(&img, &ppm, &ppm_sz, &err);
        if (packed) {
            alloc->free(alloc->ctx, packed);
        }
        if (rc != RF_OK) {
            rf_free(ppm);
            release_budget(&ws_guard);
            return map_rf_err(rc);
        }
        if (is_cancelled_generic(session, exec)) {
            rf_free(ppm);
            release_budget(&ws_guard);
            return RFC_ERR_CANCELLED;
        }
        st = copy_bytes_to_encoded(exec, ppm, ppm_sz, out_encoded);
        rf_free(ppm);
        release_budget(&ws_guard);
        return st;
    }

    char hdr[64];
    int hlen = 0;
    uint64_t payload_bytes = 0u;
    int write_p4 = (fr->layout.format == RFC_SAMPLE_GRAY8 && bps == 1u);
    if (write_p4) {
        hlen = snprintf(hdr, sizeof(hdr), "P4\n%u %u\n", w, h);
        uint64_t rbytes = ((uint64_t)w + 7ull) / 8ull;
        if (!mul_u64(rbytes, (uint64_t)h, &payload_bytes)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    } else {
        uint32_t maxval = (1u << bps) - 1u;
        int magic = (ch == 1u) ? 5 : 6;
        hlen = snprintf(hdr, sizeof(hdr), "P%d\n%u %u\n%u\n", magic, w, h,
                        maxval);
        uint64_t bpc = (sbits == 8u) ? 1ull : 2ull;
        uint64_t row_b = 0u;
        if (!mul_u64((uint64_t)w, (uint64_t)ch * bpc, &row_b) ||
            !mul_u64(row_b, (uint64_t)h, &payload_bytes)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t total_u64 = 0u;
    if (!add_u64((uint64_t)hlen, payload_bytes, &total_u64) ||
        total_u64 > SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    const rfc_allocator *alloc = basic_alloc_of(exec);
    uint8_t *out = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)total_u64);
    if (!out) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(out, hdr, (size_t)hlen);
    uint8_t *dst = out + (size_t)hlen;

    if (write_p4) {
        size_t drow_bytes = ((size_t)w + 7u) / 8u;
        for (uint32_t y = 0u; y < h; y++) {
            if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
                alloc->free(alloc->ctx, out);
                return RFC_ERR_CANCELLED;
            }
            const uint8_t *srow = fr->data + (size_t)y * src_stride;
            uint8_t *drow = dst + (size_t)y * drow_bytes;
            memset(drow, 0, drow_bytes);
            for (uint32_t x = 0u; x < w; x++) {
                /* Left-aligned 1-bit GRAY8: 0x00=black->PBM 1, 0x80=white->PBM 0 */
                if ((srow[x] >> 7) == 0u) {
                    drow[x >> 3] |= (uint8_t)(0x80u >> (x & 7u));
                }
            }
        }
    } else {
        uint32_t shift = sbits - bps;
        size_t row_s = (size_t)w * (size_t)ch;
        size_t drow_bytes = row_s * (sbits == 8u ? 1u : 2u);
        for (uint32_t y = 0u; y < h; y++) {
            if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
                alloc->free(alloc->ctx, out);
                return RFC_ERR_CANCELLED;
            }
            const uint8_t *srow = fr->data + (size_t)y * src_stride;
            uint8_t *drow = dst + (size_t)y * drow_bytes;
            if (sbits == 8u) {
                for (size_t k = 0u; k < row_s; k++) {
                    drow[k] = (uint8_t)(srow[k] >> shift);
                }
            } else {
                for (size_t k = 0u; k < row_s; k++) {
                    uint16_t v = 0u;
                    memcpy(&v, srow + k * 2u, 2u);
                    wr16be(drow + k * 2u, (uint16_t)(v >> shift));
                }
            }
        }
    }

    out_encoded->data = out;
    out_encoded->len = (size_t)total_u64;
    out_encoded->allocator = alloc;
    return RFC_OK;
}

static const rfc_session_adapter k_pnm_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_PNM,
    pnm_open,
    close_session_generic,
    pnm_probe,
    pnm_encode,
    pnm_decode,
    cancel_session_generic,
    free_encoded_generic
};

typedef struct pam_parsed {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t maxval;
    uint32_t bps;
    int is_bw;
    size_t raster_off;
    rfc_sample_format sample;
    rfc_alpha_mode alpha;
} pam_parsed;

static rfc_status pam_parse_header(const uint8_t *bytes,
                                   size_t size,
                                   const rfc_limits *limits,
                                   pam_parsed *out,
                                   rfc_image_probe *out_probe) {
    memset(out, 0, sizeof(*out));
    memset(out_probe, 0, sizeof(*out_probe));
    if (!bytes && size > 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size == 0u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (bytes[0] != 'P') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < 3u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (bytes[1] != '7') {
        return (bytes[1] >= '1' && bytes[1] <= '6')
                   ? RFC_ERR_FORMAT_DISAGREEMENT
                   : RFC_ERR_INVALID_ARGUMENT;
    }
    if (bytes[2] == '\r') {
        if (size < 4u || bytes[3] != '\n') {
            return RFC_ERR_INVALID_ARGUMENT;
        }
    } else if (bytes[2] != '\n') {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    size_t pos = (bytes[2] == '\r') ? 4u : 3u;
    int has_w = 0, has_h = 0, has_d = 0, has_m = 0, has_end = 0;
    int has_tupl = 0;
    char tupl[32] = {0};

    while (pos < size) {
        const uint8_t *nl =
            (const uint8_t *)memchr(bytes + pos, '\n', size - pos);
        if (!nl) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        size_t line_len = (size_t)(nl - (bytes + pos));
        const uint8_t *line_ptr = bytes + pos;
        pos = (size_t)(nl - bytes) + 1u;
        if (line_len > 0u && line_ptr[line_len - 1u] == '\r') {
            line_len--;
        }

        size_t s = 0u;
        while (s < line_len && (line_ptr[s] == ' ' || line_ptr[s] == '\t')) {
            s++;
        }
        if (s == line_len || line_ptr[s] == '#') {
            continue;
        }

        /* Extract keyword */
        size_t kstart = s;
        while (s < line_len && line_ptr[s] != ' ' && line_ptr[s] != '\t') {
            s++;
        }
        size_t klen = s - kstart;
        char key[32];
        if (klen == 0u || klen >= sizeof(key)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        memcpy(key, line_ptr + kstart, klen);
        key[klen] = '\0';

        while (s < line_len && (line_ptr[s] == ' ' || line_ptr[s] == '\t')) {
            s++;
        }
        size_t vstart = s;
        while (line_len > vstart &&
               (line_ptr[line_len - 1u] == ' ' ||
                line_ptr[line_len - 1u] == '\t')) {
            line_len--;
        }
        size_t vlen = line_len - vstart;
        char val[64];
        if (vlen >= sizeof(val)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        memcpy(val, line_ptr + vstart, vlen);
        val[vlen] = '\0';

        if (strcmp(key, "ENDHDR") == 0) {
            if (vlen != 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            has_end = 1;
            break;
        } else if (strcmp(key, "WIDTH") == 0) {
            if (has_w || !parse_u32_decimal(val, &out->width) ||
                out->width == 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            has_w = 1;
        } else if (strcmp(key, "HEIGHT") == 0) {
            if (has_h || !parse_u32_decimal(val, &out->height) ||
                out->height == 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            has_h = 1;
        } else if (strcmp(key, "DEPTH") == 0) {
            if (has_d || !parse_u32_decimal(val, &out->depth) ||
                out->depth == 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            has_d = 1;
        } else if (strcmp(key, "MAXVAL") == 0) {
            if (has_m || !parse_u32_decimal(val, &out->maxval)) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            has_m = 1;
        } else if (strcmp(key, "TUPLTYPE") == 0) {
            if (vlen == 0u || has_tupl || vlen >= sizeof(tupl)) {
                return RFC_ERR_UNSUPPORTED_SAMPLE;
            }
            memcpy(tupl, val, vlen + 1u);
            has_tupl = 1;
        } else {
            return RFC_ERR_INVALID_ARGUMENT;
        }
    }

    if (!has_end) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (!has_w || !has_h || !has_d || !has_m || !has_tupl) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    if (out->maxval == 0u || out->maxval > 65535u ||
        !maxval_to_bps(out->maxval, &out->bps)) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (out->depth == 2u || strcmp(tupl, "GRAYSCALE_ALPHA") == 0) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (out->depth == 1u && strcmp(tupl, "BLACKANDWHITE") == 0) {
        if (out->maxval != 1u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        out->is_bw = 1;
        out->sample = RFC_SAMPLE_GRAY8;
        out->alpha = RFC_ALPHA_NONE;
    } else if (out->depth == 1u && strcmp(tupl, "GRAYSCALE") == 0) {
        out->sample = (out->bps <= 8u) ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_GRAY16;
        out->alpha = RFC_ALPHA_NONE;
    } else if (out->depth == 3u && strcmp(tupl, "RGB") == 0) {
        out->sample = (out->bps <= 8u) ? RFC_SAMPLE_RGB8 : RFC_SAMPLE_RGB16;
        out->alpha = RFC_ALPHA_NONE;
    } else if (out->depth == 4u && strcmp(tupl, "RGB_ALPHA") == 0) {
        out->sample = (out->bps <= 8u) ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGBA16;
        out->alpha = RFC_ALPHA_STRAIGHT;
    } else {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    out->raster_off = pos;
    uint32_t sbytes = (out->bps <= 8u) ? 1u : 2u;
    uint64_t row_bytes = 0u;
    uint64_t pixels = 0u;
    uint64_t dec_bytes = 0u;
    if (!mul_u64((uint64_t)out->width, (uint64_t)out->depth * sbytes,
                 &row_bytes) ||
        !mul_u64((uint64_t)out->width, (uint64_t)out->height, &pixels) ||
        !mul_u64(pixels, (uint64_t)out->depth * sbytes, &dec_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = out->sample;
    out_probe->first_frame.alpha = out->alpha;
    out_probe->first_frame.width = out->width;
    out_probe->first_frame.height = out->height;
    out_probe->first_frame.bits_per_sample = (uint8_t)out->bps;
    if (row_bytes <= SIZE_MAX) {
        out_probe->first_frame.stride_bytes = (size_t)row_bytes;
    }
    out_probe->frame_count = 1u;
    out_probe->total_pixels = pixels;
    out_probe->total_decoded_bytes = dec_bytes;
    out_probe->container_meta.orientation = RFC_ORIENTATION_UNSPECIFIED;

    rfc_status st =
        rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
    if (st != RFC_OK) {
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }

    uint64_t avail = (uint64_t)(size - out->raster_off);
    if (avail < dec_bytes) {
        memset(out_probe, 0, sizeof(*out_probe));
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (avail > dec_bytes) {
        memset(out_probe, 0, sizeof(*out_probe));
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

static rfc_status pam_open(const rfc_exec_context *exec, void **out_sess) {
    return open_session_generic(RFC_FORMAT_PAM, exec, out_sess);
}

static rfc_status pam_probe(void *session,
                            const rfc_exec_context *exec,
                            const uint8_t *bytes,
                            size_t size,
                            rfc_image_probe *out_probe) {
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_limits lim_def;
    pam_parsed info;
    return pam_parse_header(bytes, size, basic_limits_of(exec, &lim_def),
                            &info, out_probe);
}

static rfc_status pam_decode(void *session,
                             const rfc_exec_context *exec,
                             const uint8_t *bytes,
                             size_t size,
                             const rfc_setting_value *settings,
                             size_t setting_count,
                             rfc_frame **out_frames,
                             size_t *out_frame_count,
                             rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (setting_count > 0u) {
        rfc_status vst = rfc_registry_validate_settings(
            RFC_FORMAT_PAM, settings, setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    pam_parsed info;
    rfc_image_probe probe;
    rfc_status st = pam_parse_header(bytes, size, basic_limits_of(exec, &lim_def),
                                     &info, &probe);
    if (st != RFC_OK) {
        return st;
    }

    const uint8_t *src = bytes + info.raster_off;
    size_t n_samples =
        (size_t)info.width * (size_t)info.height * (size_t)info.depth;
    if (info.bps <= 8u && info.maxval < 255u) {
        for (size_t k = 0u; k < n_samples; k++) {
            if (src[k] > info.maxval) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
    } else if (info.bps > 8u && info.maxval < 65535u) {
        for (size_t k = 0u; k < n_samples; k++) {
            if (rd16be(src + k * 2u) > info.maxval) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
    }

    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_UNSPECIFIED, 0u};
    rfc_frame *frames = NULL;
    size_t fcnt = 0u;
    st = alloc_single_frame(exec, &probe.first_frame, &meta, &frames, &fcnt,
                            out_meta);
    if (st != RFC_OK) {
        return st;
    }

    uint32_t shift = (info.bps <= 8u) ? (8u - info.bps) : (16u - info.bps);
    size_t row_s = (size_t)info.width * (size_t)info.depth;
    size_t srow_bytes = row_s * (info.bps <= 8u ? 1u : 2u);
    for (uint32_t y = 0u; y < info.height; y++) {
        if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
            free_single_frame(exec, &frames, &fcnt, out_meta);
            return RFC_ERR_CANCELLED;
        }
        const uint8_t *srow = src + (size_t)y * srow_bytes;
        uint8_t *drow =
            frames[0].data + (size_t)y * frames[0].layout.stride_bytes;
        if (info.bps <= 8u) {
            if (shift == 0u) {
                memcpy(drow, srow, row_s);
            } else {
                for (size_t k = 0u; k < row_s; k++) {
                    drow[k] = (uint8_t)(srow[k] << shift);
                }
            }
        } else {
            for (size_t k = 0u; k < row_s; k++) {
                uint16_t u16 = (uint16_t)(rd16be(srow + k * 2u) << shift);
                memcpy(drow + k * 2u, &u16, 2u);
            }
        }
    }

    if (is_cancelled_generic(session, exec)) {
        free_single_frame(exec, &frames, &fcnt, out_meta);
        return RFC_ERR_CANCELLED;
    }
    *out_frames = frames;
    *out_frame_count = fcnt;
    return RFC_OK;
}

static rfc_status pam_encode(void *session,
                             const rfc_exec_context *exec,
                             const rfc_frame *frames,
                             size_t frame_count,
                             const rfc_metadata *meta,
                             const rfc_setting_value *settings,
                             size_t setting_count,
                             rfc_encoded *out_encoded) {
    if (!out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    if (is_cancelled_generic(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    encode_common_check chk;
    rfc_status st = prepare_encode_common(
        RFC_FORMAT_PAM, exec, frames, frame_count, meta, settings,
        setting_count, &chk);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_frame *fr = &frames[0];
    uint32_t w = fr->layout.width;
    uint32_t h = fr->layout.height;
    uint32_t bps = chk.eff_bps;
    uint32_t sbits = sample_storage_bits(fr->layout.format);
    uint32_t ch = sample_channels(fr->layout.format);
    if (sbits == 0u || ch == 0u || (sbits == 16u && bps <= 8u)) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    const char *tupl = NULL;
    if (ch == 1u) {
        tupl = (bps == 1u) ? "BLACKANDWHITE" : "GRAYSCALE";
    } else if (ch == 3u) {
        tupl = "RGB";
    } else {
        tupl = "RGB_ALPHA";
    }

    uint32_t maxval = (1u << bps) - 1u;
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "P7\nWIDTH %u\nHEIGHT %u\nDEPTH %u\nMAXVAL %u\n"
                        "TUPLTYPE %s\nENDHDR\n",
                        w, h, ch, maxval, tupl);
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint64_t bpc = (sbits == 8u) ? 1ull : 2ull;
    uint64_t row_b = 0u;
    uint64_t payload_bytes = 0u;
    uint64_t total_u64 = 0u;
    if (!mul_u64((uint64_t)w, (uint64_t)ch * bpc, &row_b) ||
        !mul_u64(row_b, (uint64_t)h, &payload_bytes) ||
        !add_u64((uint64_t)hlen, payload_bytes, &total_u64) ||
        total_u64 > SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    const rfc_allocator *alloc = basic_alloc_of(exec);
    uint8_t *out = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)total_u64);
    if (!out) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(out, hdr, (size_t)hlen);
    uint8_t *dst = out + (size_t)hlen;

    size_t src_stride = fr->layout.stride_bytes
                            ? fr->layout.stride_bytes
                            : chk.norm_layout.stride_bytes;
    uint32_t shift = sbits - bps;
    size_t row_s = (size_t)w * (size_t)ch;
    size_t drow_bytes = row_s * (sbits == 8u ? 1u : 2u);
    for (uint32_t y = 0u; y < h; y++) {
        if ((y & 63u) == 0u && is_cancelled_generic(session, exec)) {
            alloc->free(alloc->ctx, out);
            return RFC_ERR_CANCELLED;
        }
        const uint8_t *srow = fr->data + (size_t)y * src_stride;
        uint8_t *drow = dst + (size_t)y * drow_bytes;
        if (sbits == 8u) {
            if (shift == 0u) {
                memcpy(drow, srow, row_s);
            } else {
                for (size_t k = 0u; k < row_s; k++) {
                    drow[k] = (uint8_t)(srow[k] >> shift);
                }
            }
        } else {
            for (size_t k = 0u; k < row_s; k++) {
                uint16_t v = 0u;
                memcpy(&v, srow + k * 2u, 2u);
                wr16be(drow + k * 2u, (uint16_t)(v >> shift));
            }
        }
    }

    out_encoded->data = out;
    out_encoded->len = (size_t)total_u64;
    out_encoded->allocator = alloc;
    return RFC_OK;
}

static const rfc_session_adapter k_pam_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_PAM,
    pam_open,
    close_session_generic,
    pam_probe,
    pam_encode,
    pam_decode,
    cancel_session_generic,
    free_encoded_generic
};

/* ------------------------------------------------------------------------ */
/* Public basic registration & rollback                                     */
/* ------------------------------------------------------------------------ */

static const rfc_session_adapter *const k_basic_adapters[5] = {
    &k_fold_adapter,
    &k_bmp_adapter,
    &k_png_adapter,
    &k_pnm_adapter,
    &k_pam_adapter
};

rfc_status rfc_codecs_register_basic(void) {
    int registered_here[5] = {0, 0, 0, 0, 0};
    for (size_t i = 0u; i < 5u; i++) {
        rfc_status st = rfc_registry_register_session(k_basic_adapters[i]);
        if (st != RFC_OK) {
            for (size_t j = i; j > 0u; j--) {
                if (registered_here[j - 1u]) {
                    (void)rfc_registry_unregister_session(
                        k_basic_adapters[j - 1u]->format);
                }
            }
            return st;
        }
        registered_here[i] = 1;
    }
    return RFC_OK;
}

rfc_status rfc_codecs_unregister_basic(void) {
    rfc_status first_err = RFC_OK;
    for (size_t i = 0u; i < 5u; i++) {
        rfc_format_id fmt = k_basic_adapters[i]->format;
        if (rfc_registry_session_adapter(fmt) == k_basic_adapters[i]) {
            rfc_status st = rfc_registry_unregister_session(fmt);
            if (st != RFC_OK && first_err == RFC_OK) {
                first_err = st;
            }
        }
    }
    return first_err;
}
