/* Shipped C11 codec session adapter for D10 T02 s8:
 *   - RFC_FORMAT_QOI (bundled phoboslab/qoi 0.0.0-ffb2d2cb: lossless RGB8 and
 *     RGBA8 preserving RGB under alpha == 0, sRGB and linear colorspace
 *     round-trip, zero-allocation strict chunk-stream preflight, and explicit
 *     rfc_budget reservation covering libqoi's live heap allocations). */

#include "rfc_codecs.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#ifndef QOI_NO_STDIO
#define QOI_NO_STDIO
#endif
#include <qoi.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RFC_QOI_HDR_SIZE 14u
#define RFC_QOI_END_SIZE 8u
#define RFC_QOI_MIN_SIZE (RFC_QOI_HDR_SIZE + RFC_QOI_END_SIZE)
/* Matches QOI_PIXELS_MAX in upstream qoi.h (400 million pixels). */
#define RFC_QOI_PIXELS_MAX 400000000u

static const uint8_t k_qoi_end_marker[RFC_QOI_END_SIZE] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u
};

/* ------------------------------------------------------------------------ */
/* Test hook, allocator, limits, session, and budget helpers                */
/* ------------------------------------------------------------------------ */

static _Atomic(void (*)(void *)) g_qoi_hook_fn = NULL;
static _Atomic(void *) g_qoi_hook_ctx = NULL;

void rfc_codecs_qoi_set_hook_for_testing(void (*hook)(void *), void *ctx) {
    atomic_store_explicit(&g_qoi_hook_ctx, ctx, memory_order_release);
    atomic_store_explicit(&g_qoi_hook_fn, hook, memory_order_release);
}

static void qoi_run_test_hook(void) {
    void (*fn)(void *) = atomic_load_explicit(&g_qoi_hook_fn, memory_order_acquire);
    if (fn) {
        void *ctx = atomic_load_explicit(&g_qoi_hook_ctx, memory_order_acquire);
        fn(ctx);
    }
}

static void *qoi_stdlib_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void qoi_stdlib_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const rfc_allocator k_qoi_stdlib_alloc = {
    qoi_stdlib_alloc, qoi_stdlib_free, NULL
};

static const rfc_allocator *qoi_alloc_of(const rfc_exec_context *exec) {
    const rfc_allocator *a = rfc_exec_allocator(exec);
    return (a && a->alloc && a->free) ? a : &k_qoi_stdlib_alloc;
}

static const rfc_limits *qoi_limits_of(const rfc_exec_context *exec,
                                       rfc_limits *scratch) {
    const rfc_limits *l = rfc_exec_limits(exec);
    if (l) {
        return l;
    }
    rfc_limits_default(scratch);
    return scratch;
}

typedef struct qoi_session {
    atomic_int cancelled;
} qoi_session;

static int qoi_is_cancelled(const void *session, const rfc_exec_context *exec) {
    const qoi_session *s = (const qoi_session *)session;
    if (s && atomic_load_explicit(&s->cancelled, memory_order_acquire) != 0) {
        return 1;
    }
    return exec ? rfc_exec_is_cancelled(exec) : 0;
}

typedef struct qoi_budget_guard {
    rfc_budget *budget;
    size_t bytes;
    int active;
} qoi_budget_guard;

static rfc_status qoi_reserve_budget(const rfc_exec_context *exec,
                                     size_t bytes,
                                     qoi_budget_guard *out) {
    memset(out, 0, sizeof(*out));
    if (!exec || !exec->budget || bytes == 0u) {
        return RFC_OK;
    }
    rfc_status st = rfc_budget_reserve(exec->budget, bytes);
    if (st != RFC_OK) {
        return st;
    }
    out->budget = exec->budget;
    out->bytes = bytes;
    out->active = 1;
    return RFC_OK;
}

static void qoi_release_budget(qoi_budget_guard *g) {
    if (g && g->active && g->budget) {
        rfc_budget_release(g->budget, g->bytes);
        g->budget = NULL;
        g->bytes = 0u;
        g->active = 0;
    }
}

static uint32_t qoi_rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int qoi_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0u && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static int qoi_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static void qoi_fill_linear_colorimetry(rfc_colorimetry *col) {
    memset(col, 0, sizeof(*col));
    /* QOI colorspace=1 specifies all channels linear without primaries:
     * cICP {2 (unspecified primaries), 8 (linear transfer), 0 (RGB), 1 (full)}. */
    col->has_cicp = 1u;
    col->cicp[0] = 2u;
    col->cicp[1] = 8u;
    col->cicp[2] = 0u;
    col->cicp[3] = 1u;
}

static int qoi_is_linear_colorimetry(const rfc_colorimetry *col) {
    if (!col) {
        return 0;
    }
    return (!col->has_srgb && !col->has_gama && !col->has_chrm &&
            !col->has_mdcv && !col->has_clli &&
            col->has_cicp &&
            col->cicp[0] == 2u && col->cicp[1] == 8u &&
            col->cicp[2] == 0u && col->cicp[3] == 1u)
               ? 1
               : 0;
}

/* ------------------------------------------------------------------------ */
/* Settings parsing                                                         */
/* ------------------------------------------------------------------------ */

static rfc_status qoi_parse_settings(const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_metadata_policy *out_meta_policy,
                                     rfc_exactness_class *out_exactness) {
    *out_meta_policy = RFC_META_POLICY_PRESERVE;
    *out_exactness = RFC_EXACT_PIXEL_EXACT;
    if (setting_count > 0u && !settings) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (setting_count > 0u) {
        const char *reason = NULL;
        rfc_status st = rfc_registry_validate_settings(
            RFC_FORMAT_QOI, settings, setting_count, &reason);
        if (st != RFC_OK) {
            return st;
        }
        for (size_t i = 0u; i < setting_count; i++) {
            if (strcmp(settings[i].id, "metadata") == 0) {
                st = rfc_metadata_policy_parse(settings[i].value,
                                               out_meta_policy);
                if (st != RFC_OK) {
                    return st;
                }
            } else if (strcmp(settings[i].id, "exactness") == 0) {
                if (strcmp(settings[i].value, "pixel-exact") == 0) {
                    *out_exactness = RFC_EXACT_PIXEL_EXACT;
                } else if (strcmp(settings[i].value, "byte-identical") == 0) {
                    *out_exactness = RFC_EXACT_BYTE_IDENTICAL;
                } else {
                    return RFC_ERR_SETTING_VALUE;
                }
            }
        }
    }
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Zero-allocation strict QOI preflight                                     */
/* ------------------------------------------------------------------------ */

typedef struct qoi_stream_info {
    uint32_t width;
    uint32_t height;
    uint8_t channels;
    uint8_t colorspace;
    size_t decoded_bytes;
} qoi_stream_info;

static rfc_status qoi_preflight_stream(const uint8_t *bytes,
                                       size_t size,
                                       const rfc_limits *limits,
                                       const void *session,
                                       const rfc_exec_context *exec,
                                       qoi_stream_info *out) {
    if (!bytes || !limits || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }
    if (size > (size_t)INT_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (size < 4u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (memcmp(bytes, "qoif", 4u) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < RFC_QOI_MIN_SIZE) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    uint32_t w = qoi_rd32be(bytes + 4u);
    uint32_t h = qoi_rd32be(bytes + 8u);
    uint8_t ch = bytes[12u];
    uint8_t cs = bytes[13u];
    if (w == 0u || h == 0u || (ch != 3u && ch != 4u) || cs > 1u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = (ch == 4u) ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8;
    lay.alpha = (ch == 4u) ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
    lay.width = w;
    lay.height = h;
    lay.stride_bytes = 0u;
    lay.bits_per_sample = 8u;

    st = rfc_limits_check_image(limits, &lay, 1u);
    if (st != RFC_OK) {
        return st;
    }

    uint64_t total_pixels = 0u;
    uint64_t decoded_bytes = 0u;
    if (!qoi_mul_u64((uint64_t)w, (uint64_t)h, &total_pixels) ||
        !qoi_mul_u64(total_pixels, (uint64_t)ch, &decoded_bytes) ||
        decoded_bytes > (uint64_t)SIZE_MAX ||
        decoded_bytes > (uint64_t)INT_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    /* Match upstream qoi_decode guard: desc->height >= QOI_PIXELS_MAX / desc->width. */
    if (h >= RFC_QOI_PIXELS_MAX / w) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    st = rfc_limits_check_decompression(limits, (uint64_t)size, decoded_bytes);
    if (st != RFC_OK) {
        return st;
    }

    if (memcmp(bytes + size - RFC_QOI_END_SIZE, k_qoi_end_marker,
               RFC_QOI_END_SIZE) != 0) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    /* Strict chunk-stream validation with zero heap allocation:
     *   - every multi-byte op must fit strictly before the 8-byte end marker;
     *   - every run must fit within the remaining image pixel count;
     *   - two consecutive QOI_OP_INDEX chunks naming the same slot are refused
     *     per the QOI specification (QOI_OP_RUN must be used instead);
     *   - all pixels must be covered before reaching the end marker;
     *   - no trailing bytes may remain between the last chunk and the end marker. */
    size_t pos = RFC_QOI_HDR_SIZE;
    size_t chunks_end = size - RFC_QOI_END_SIZE;
    uint64_t pixels_done = 0u;
    uint32_t step = 0u;
    int prev_op_index = -1;

    qoi_run_test_hook();
    if (qoi_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    while (pixels_done < total_pixels) {
        if ((step++ & 4095u) == 0u) {
            if (step > 1u) {
                qoi_run_test_hook();
            }
            if (qoi_is_cancelled(session, exec)) {
                return RFC_ERR_CANCELLED;
            }
        }
        if (pos >= chunks_end) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint8_t tag = bytes[pos++];
        if (tag == 0xFEu) {
            if (chunks_end - pos < 3u) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            pos += 3u;
            pixels_done += 1u;
            prev_op_index = -1;
        } else if (tag == 0xFFu) {
            if (chunks_end - pos < 4u) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            pos += 4u;
            pixels_done += 1u;
            prev_op_index = -1;
        } else if ((tag & 0xC0u) == 0x80u) {
            if (chunks_end - pos < 1u) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            pos += 1u;
            pixels_done += 1u;
            prev_op_index = -1;
        } else if ((tag & 0xC0u) == 0xC0u) {
            uint64_t run_len = (uint64_t)(tag & 0x3Fu) + 1u;
            if (run_len > total_pixels - pixels_done) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            pixels_done += run_len;
            prev_op_index = -1;
        } else if ((tag & 0xC0u) == 0x00u) {
            if ((int)tag == prev_op_index) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            prev_op_index = (int)tag;
            pixels_done += 1u;
        } else {
            prev_op_index = -1;
            pixels_done += 1u;
        }
    }

    if (pos != chunks_end) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    out->width = w;
    out->height = h;
    out->channels = ch;
    out->colorspace = cs;
    out->decoded_bytes = (size_t)decoded_bytes;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Session lifecycle, probe, decode, encode                                 */
/* ------------------------------------------------------------------------ */

static rfc_status qoi_open(const rfc_exec_context *exec,
                           void **out_session) {
    if (!out_session) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_session = NULL;
    if (exec && rfc_exec_is_cancelled(exec)) {
        return RFC_ERR_CANCELLED;
    }
    qoi_session *s = (qoi_session *)calloc(1u, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&s->cancelled, 0);
    *out_session = s;
    return RFC_OK;
}

static void qoi_close(void *session) {
    free(session);
}

static void qoi_cancel(void *session) {
    if (!session) {
        return;
    }
    qoi_session *s = (qoi_session *)session;
    atomic_store_explicit(&s->cancelled, 1, memory_order_release);
}

static rfc_status qoi_probe(void *session,
                            const rfc_exec_context *exec,
                            const uint8_t *bytes,
                            size_t size,
                            rfc_image_probe *out_probe) {
    if (!session || !bytes || !out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (qoi_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_limits lim_def;
    const rfc_limits *limits = qoi_limits_of(exec, &lim_def);

    qoi_stream_info info;
    rfc_status st =
        qoi_preflight_stream(bytes, size, limits, session, exec, &info);
    if (st != RFC_OK) {
        return st;
    }

    out_probe->first_frame.format =
        (info.channels == 4u) ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha =
        (info.channels == 4u) ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
    out_probe->first_frame.width = info.width;
    out_probe->first_frame.height = info.height;
    out_probe->first_frame.stride_bytes =
        (size_t)info.width * (size_t)info.channels;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)info.width * (uint64_t)info.height;
    out_probe->total_decoded_bytes = (uint64_t)info.decoded_bytes;
    out_probe->container_meta.orientation = RFC_ORIENTATION_UNSPECIFIED;
    if (info.colorspace == QOI_LINEAR) {
        qoi_fill_linear_colorimetry(&out_probe->container_meta.color);
    }

    return rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
}

static rfc_status qoi_decode_session(void *session,
                                     const rfc_exec_context *exec,
                                     const uint8_t *bytes,
                                     size_t size,
                                     const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_frame **out_frames,
                                     size_t *out_frame_count,
                                     rfc_metadata *out_meta) {
    if (!session || !bytes || !out_frames || !out_frame_count) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (qoi_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_metadata_policy meta_policy = RFC_META_POLICY_PRESERVE;
    rfc_exactness_class exactness = RFC_EXACT_PIXEL_EXACT;
    rfc_status st =
        qoi_parse_settings(settings, setting_count, &meta_policy, &exactness);
    if (st != RFC_OK) {
        return st;
    }

    rfc_limits lim_def;
    const rfc_limits *limits = qoi_limits_of(exec, &lim_def);

    qoi_stream_info info;
    st = qoi_preflight_stream(bytes, size, limits, session, exec, &info);
    if (st != RFC_OK) {
        return st;
    }

    /* Reserve libqoi's internal malloc(width * height * channels) buffer
     * before calling qoi_decode so budget enforcement covers provider heap. */
    qoi_budget_guard dec_guard;
    st = qoi_reserve_budget(exec, info.decoded_bytes, &dec_guard);
    if (st != RFC_OK) {
        return st;
    }

    if (qoi_is_cancelled(session, exec)) {
        qoi_release_budget(&dec_guard);
        return RFC_ERR_CANCELLED;
    }

    qoi_desc desc;
    memset(&desc, 0, sizeof(desc));
    void *decoded = qoi_decode(bytes, (int)size, &desc, (int)info.channels);
    if (!decoded) {
        qoi_release_budget(&dec_guard);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    if (desc.width != info.width || desc.height != info.height ||
        desc.channels != info.channels || desc.colorspace != info.colorspace) {
        free(decoded);
        qoi_release_budget(&dec_guard);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    qoi_run_test_hook();
    if (qoi_is_cancelled(session, exec)) {
        free(decoded);
        qoi_release_budget(&dec_guard);
        return RFC_ERR_CANCELLED;
    }

    const rfc_allocator *alloc = qoi_alloc_of(exec);
    rfc_frame *frames = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(*frames));
    if (!frames) {
        free(decoded);
        qoi_release_budget(&dec_guard);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(frames, 0, sizeof(*frames));

    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = (info.channels == 4u) ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8;
    lay.alpha = (info.channels == 4u) ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
    lay.width = info.width;
    lay.height = info.height;
    lay.stride_bytes = (size_t)info.width * (size_t)info.channels;
    lay.bits_per_sample = 8u;

    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.orientation = RFC_ORIENTATION_UNSPECIFIED;
    if (info.colorspace == QOI_LINEAR) {
        qoi_fill_linear_colorimetry(&meta.color);
    }

    st = rfc_exec_alloc_frame(exec, &frames[0], &lay, &meta);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, frames);
        free(decoded);
        qoi_release_budget(&dec_guard);
        return st;
    }

    memcpy(frames[0].data, decoded, info.decoded_bytes);
    free(decoded);
    qoi_release_budget(&dec_guard);

    if (out_meta) {
        *out_meta = meta;
    }
    *out_frames = frames;
    *out_frame_count = 1u;
    return RFC_OK;
}

static rfc_status qoi_encode_session(void *session,
                                     const rfc_exec_context *exec,
                                     const rfc_frame *frames,
                                     size_t frame_count,
                                     const rfc_metadata *meta,
                                     const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_encoded *out_encoded) {
    if (!session || !frames || !out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    if (frame_count == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count > 1u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (qoi_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_metadata_policy meta_policy = RFC_META_POLICY_PRESERVE;
    rfc_exactness_class exactness = RFC_EXACT_PIXEL_EXACT;
    rfc_status st =
        qoi_parse_settings(settings, setting_count, &meta_policy, &exactness);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_frame *fr = &frames[0];
    if (!fr->data) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_layout norm;
    st = rfc_layout_normalize_stride(&fr->layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    rfc_layout eff_lay = fr->layout;
    if (eff_lay.stride_bytes == 0u) {
        eff_lay.stride_bytes = norm.stride_bytes;
    }
    size_t min_bytes = 0u;
    st = rfc_layout_validate(&eff_lay, &min_bytes);
    if (st != RFC_OK) {
        return st;
    }
    if (fr->data_len < min_bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (fr->layout.format != RFC_SAMPLE_RGB8 &&
        fr->layout.format != RFC_SAMPLE_RGBA8) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (fr->layout.bits_per_sample != 8u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (fr->layout.format == RFC_SAMPLE_RGB8) {
        if (fr->layout.alpha != RFC_ALPHA_NONE) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
    } else {
        if (fr->layout.alpha != RFC_ALPHA_STRAIGHT) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
    }

    rfc_limits lim_def;
    const rfc_limits *limits = qoi_limits_of(exec, &lim_def);
    st = rfc_limits_check_image(limits, &norm, 1u);
    if (st != RFC_OK) {
        return st;
    }

    uint32_t w = eff_lay.width;
    uint32_t h = eff_lay.height;
    uint8_t ch = (eff_lay.format == RFC_SAMPLE_RGBA8) ? 4u : 3u;
    if (h >= RFC_QOI_PIXELS_MAX / w) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    if ((fr->metadata.icc_len > 0u && !fr->metadata.icc) ||
        (fr->metadata.exif_len > 0u && !fr->metadata.exif) ||
        (fr->metadata.xmp_len > 0u && !fr->metadata.xmp) ||
        (meta && ((meta->icc_len > 0u && !meta->icc) ||
                  (meta->exif_len > 0u && !meta->exif) ||
                  (meta->xmp_len > 0u && !meta->xmp)))) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    const uint8_t *eff_icc = NULL;
    size_t eff_icc_len = 0u;
    const uint8_t *eff_exif = NULL;
    size_t eff_exif_len = 0u;
    const uint8_t *eff_xmp = NULL;
    size_t eff_xmp_len = 0u;
    rfc_orientation eff_ori = RFC_ORIENTATION_UNSPECIFIED;
    rfc_colorimetry eff_color;
    memset(&eff_color, 0, sizeof(eff_color));

    if (fr->metadata.icc_len > 0u && fr->metadata.icc != NULL) {
        eff_icc = fr->metadata.icc;
        eff_icc_len = fr->metadata.icc_len;
    } else if (meta && meta->icc_len > 0u && meta->icc != NULL) {
        eff_icc = meta->icc;
        eff_icc_len = meta->icc_len;
    }
    if (fr->metadata.exif_len > 0u && fr->metadata.exif != NULL) {
        eff_exif = fr->metadata.exif;
        eff_exif_len = fr->metadata.exif_len;
    } else if (meta && meta->exif_len > 0u && meta->exif != NULL) {
        eff_exif = meta->exif;
        eff_exif_len = meta->exif_len;
    }
    if (fr->metadata.xmp_len > 0u && fr->metadata.xmp != NULL) {
        eff_xmp = fr->metadata.xmp;
        eff_xmp_len = fr->metadata.xmp_len;
    } else if (meta && meta->xmp_len > 0u && meta->xmp != NULL) {
        eff_xmp = meta->xmp;
        eff_xmp_len = meta->xmp_len;
    }
    if (fr->metadata.orientation != RFC_ORIENTATION_UNSPECIFIED) {
        eff_ori = fr->metadata.orientation;
    } else if (meta && meta->orientation != RFC_ORIENTATION_UNSPECIFIED) {
        eff_ori = meta->orientation;
    }
    if (rfc_colorimetry_has_any(&fr->metadata.color)) {
        eff_color = fr->metadata.color;
    } else if (meta && rfc_colorimetry_has_any(&meta->color)) {
        eff_color = meta->color;
    }
    if (!rfc_colorimetry_is_valid(&eff_color) ||
        eff_ori > RFC_ORIENTATION_ROTATE_270) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_metadata check_m = {eff_icc, eff_icc_len, eff_ori, 0u, eff_color,
                            eff_exif, eff_exif_len, eff_xmp, eff_xmp_len};
    st = rfc_limits_check_metadata(limits, &check_m);
    if (st != RFC_OK) {
        return st;
    }

    if (!exec || !exec->job) {
        st = rfc_metadata_check_source_preflight(
            RFC_FORMAT_QOI, meta_policy, exactness, meta, frames, frame_count,
            0u, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }

    uint8_t qoi_cs = QOI_SRGB;
    if (meta_policy == RFC_META_POLICY_PRESERVE && eff_icc_len == 0u &&
        qoi_is_linear_colorimetry(&eff_color)) {
        qoi_cs = QOI_LINEAR;
    }

    uint64_t pixels = 0u;
    uint64_t row_bytes64 = 0u;
    uint64_t packed_bytes64 = 0u;
    uint64_t worst_qoi64 = 0u;
    if (!qoi_mul_u64((uint64_t)w, (uint64_t)h, &pixels) ||
        !qoi_mul_u64((uint64_t)w, (uint64_t)ch, &row_bytes64) ||
        !qoi_mul_u64(row_bytes64, (uint64_t)h, &packed_bytes64) ||
        !qoi_mul_u64(pixels, (uint64_t)(ch + 1u), &worst_qoi64) ||
        !qoi_add_u64(worst_qoi64, RFC_QOI_MIN_SIZE, &worst_qoi64) ||
        worst_qoi64 > (uint64_t)INT_MAX ||
        packed_bytes64 > (uint64_t)SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    int need_pack = (eff_lay.stride_bytes != (size_t)row_bytes64) ? 1 : 0;
    qoi_budget_guard pack_guard;
    memset(&pack_guard, 0, sizeof(pack_guard));
    if (need_pack) {
        st = qoi_reserve_budget(exec, (size_t)packed_bytes64, &pack_guard);
        if (st != RFC_OK) {
            return st;
        }
    }

    qoi_budget_guard enc_guard;
    st = qoi_reserve_budget(exec, (size_t)worst_qoi64, &enc_guard);
    if (st != RFC_OK) {
        qoi_release_budget(&pack_guard);
        return st;
    }

    qoi_run_test_hook();
    if (qoi_is_cancelled(session, exec)) {
        qoi_release_budget(&enc_guard);
        qoi_release_budget(&pack_guard);
        return RFC_ERR_CANCELLED;
    }

    const uint8_t *src_pixels = fr->data;
    uint8_t *packed = NULL;
    if (need_pack) {
        packed = (uint8_t *)malloc((size_t)packed_bytes64);
        if (!packed) {
            qoi_release_budget(&enc_guard);
            qoi_release_budget(&pack_guard);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        size_t row_b = (size_t)row_bytes64;
        for (uint32_t y = 0u; y < h; y++) {
            if ((y & 63u) == 0u) {
                if (y > 0u) {
                    qoi_run_test_hook();
                }
                if (qoi_is_cancelled(session, exec)) {
                    free(packed);
                    qoi_release_budget(&enc_guard);
                    qoi_release_budget(&pack_guard);
                    return RFC_ERR_CANCELLED;
                }
            }
            memcpy(packed + (size_t)y * row_b,
                   fr->data + (size_t)y * eff_lay.stride_bytes, row_b);
        }
        src_pixels = packed;
    }

    qoi_desc desc;
    desc.width = w;
    desc.height = h;
    desc.channels = ch;
    desc.colorspace = qoi_cs;

    int encoded_len = 0;
    void *encoded_raw = qoi_encode(src_pixels, &desc, &encoded_len);
    free(packed);
    qoi_release_budget(&pack_guard);
    if (!encoded_raw || encoded_len <= 0) {
        free(encoded_raw);
        qoi_release_budget(&enc_guard);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    qoi_run_test_hook();
    if (qoi_is_cancelled(session, exec)) {
        free(encoded_raw);
        qoi_release_budget(&enc_guard);
        return RFC_ERR_CANCELLED;
    }

    const rfc_allocator *alloc = qoi_alloc_of(exec);
    uint8_t *final_buf = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)encoded_len);
    if (!final_buf) {
        free(encoded_raw);
        qoi_release_budget(&enc_guard);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(final_buf, encoded_raw, (size_t)encoded_len);
    free(encoded_raw);
    qoi_release_budget(&enc_guard);

    out_encoded->data = final_buf;
    out_encoded->len = (size_t)encoded_len;
    out_encoded->allocator = alloc;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Adapter registration                                                     */
/* ------------------------------------------------------------------------ */

static const rfc_session_adapter k_qoi_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_QOI,
    qoi_open,
    qoi_close,
    qoi_probe,
    qoi_encode_session,
    qoi_decode_session,
    qoi_cancel,
    NULL
};

rfc_status rfc_codecs_register_qoi(void) {
    return rfc_registry_register_session(&k_qoi_adapter);
}

rfc_status rfc_codecs_unregister_qoi(void) {
    return rfc_registry_unregister_session(RFC_FORMAT_QOI);
}
