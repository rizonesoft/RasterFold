/* Shipped C11 codec session adapter for D10 T02 s2:
 *   - RFC_FORMAT_WEBP (bundled libwebp 1.6.0: lossless exact-RGB VP8L encode,
 *     lossless + lossy single-frame decode, ICCP/EXIF/XMP mux/demux,
 *     zero-allocation RIFF/VP8X preflight, and rfc_budget tracking). */

#include "exif_util.h"
#include "rfc_codecs.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

/* ------------------------------------------------------------------------ */
/* Session, allocator, budget, arithmetic, and endian helpers               */
/* ------------------------------------------------------------------------ */

static void *webp_stdlib_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void webp_stdlib_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const rfc_allocator k_webp_stdlib_alloc = {
    webp_stdlib_alloc, webp_stdlib_free, NULL
};

static const rfc_allocator *webp_alloc_of(const rfc_exec_context *exec) {
    const rfc_allocator *a = rfc_exec_allocator(exec);
    return (a && a->alloc && a->free) ? a : &k_webp_stdlib_alloc;
}

static const rfc_limits *webp_limits_of(const rfc_exec_context *exec,
                                        rfc_limits *scratch) {
    const rfc_limits *l = rfc_exec_limits(exec);
    if (l) {
        return l;
    }
    rfc_limits_default(scratch);
    return scratch;
}

typedef struct webp_session {
    atomic_int cancelled;
} webp_session;

static int webp_is_cancelled(const void *session,
                             const rfc_exec_context *exec) {
    const webp_session *s = (const webp_session *)session;
    if (s && atomic_load_explicit(&s->cancelled, memory_order_acquire) != 0) {
        return 1;
    }
    return exec ? rfc_exec_is_cancelled(exec) : 0;
}

typedef struct webp_budget_guard {
    rfc_budget *budget;
    size_t bytes;
    int active;
} webp_budget_guard;

static rfc_status webp_reserve_budget(const rfc_exec_context *exec,
                                      size_t bytes,
                                      webp_budget_guard *out) {
    memset(out, 0, sizeof(*out));
    if (!exec || !exec->budget) {
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

static void webp_release_budget(webp_budget_guard *g) {
    if (g && g->active && g->budget) {
        rfc_budget_release(g->budget, g->bytes);
        g->budget = NULL;
        g->bytes = 0u;
        g->active = 0;
    }
}

static int webp_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0u && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static int webp_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static uint32_t webp_rd24le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* ------------------------------------------------------------------------ */
/* Zero-allocation RIFF/VP8X preflight                                      */
/* ------------------------------------------------------------------------ */

typedef struct webp_riff_info {
    uint32_t width;
    uint32_t height;
    int has_alpha;
    rfc_orientation orientation;
    size_t icc_off;
    size_t icc_len;
    size_t exif_off;
    size_t exif_len;
    size_t xmp_off;
    size_t xmp_len;
} webp_riff_info;

static rfc_status webp_check_meta_len(const rfc_limits *limits,
                                      size_t len,
                                      size_t total_meta) {
    rfc_metadata probe_m = {0};
    if (len > RFC_MAX_ICC_BYTES || len > limits->max_metadata_bytes ||
        total_meta > RFC_MAX_ICC_BYTES ||
        total_meta > limits->max_metadata_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    probe_m.icc_len = total_meta;
    return rfc_limits_check_metadata(limits, &probe_m);
}

static rfc_status webp_preflight_riff(const uint8_t *bytes,
                                      size_t size,
                                      const rfc_limits *limits,
                                      webp_riff_info *out) {
    if (!bytes || !limits || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->orientation = RFC_ORIENTATION_UNSPECIFIED;

    if (size < 12u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WEBP", 4) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint32_t riff_sz = rfc_exif_rd32le(bytes + 4);
    if (riff_sz < 12u || riff_sz > 0xFFFFFFF7u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t riff_end = 8ull + (uint64_t)riff_sz;
    if ((uint64_t)size > riff_end) {
        return RFC_ERR_INVALID_ARGUMENT; /* trailing garbage after RIFF */
    }
    int container_truncated = ((uint64_t)size < riff_end) ? 1 : 0;

    int saw_vp8x = 0;
    uint8_t vp8x_flags = 0u;
    uint32_t canvas_w = 0u;
    uint32_t canvas_h = 0u;
    int saw_iccp = 0;
    int saw_exif = 0;
    int saw_xmp = 0;
    int saw_alph = 0;
    int saw_vp8l = 0;
    int saw_image = 0;
    size_t total_meta = 0u;

    size_t pos = 12u;
    size_t chunk_idx = 0u;
    size_t scan_end =
        container_truncated ? size : (size_t)riff_end;

    while ((uint64_t)pos < riff_end) {
        if (pos + 8u > scan_end) {
            return container_truncated ? RFC_ERR_TRUNCATED_INPUT
                                       : RFC_ERR_INVALID_ARGUMENT;
        }
        const uint8_t *tag = bytes + pos;
        uint32_t clen = rfc_exif_rd32le(bytes + pos + 4u);
        if (clen > 0xFFFFFFE0u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        uint64_t pad_len = (uint64_t)clen + ((uint64_t)clen & 1ull);
        uint64_t next_pos = (uint64_t)pos + 8ull + pad_len;
        if (next_pos > riff_end) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if ((uint64_t)pos + 8ull + (uint64_t)clen > (uint64_t)size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (next_pos > (uint64_t)size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }

        size_t payload_off = pos + 8u;
        const uint8_t *payload = bytes + payload_off;

        if (memcmp(tag, "ANIM", 4) == 0 || memcmp(tag, "ANMF", 4) == 0) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        } else if (memcmp(tag, "VP8X", 4) == 0) {
            if (chunk_idx != 0u || saw_vp8x || clen != 10u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_vp8x = 1;
            vp8x_flags = payload[0];
            if ((vp8x_flags & (uint8_t)(~ALL_VALID_FLAGS)) != 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (payload[1] != 0u || payload[2] != 0u || payload[3] != 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (vp8x_flags & (uint8_t)ANIMATION_FLAG) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
            canvas_w = 1u + webp_rd24le(payload + 4u);
            canvas_h = 1u + webp_rd24le(payload + 7u);
            if (canvas_w == 0u || canvas_h == 0u ||
                canvas_w > WEBP_MAX_DIMENSION ||
                canvas_h > WEBP_MAX_DIMENSION) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            uint64_t area = 0u;
            if (!webp_mul_u64((uint64_t)canvas_w, (uint64_t)canvas_h, &area) ||
                area >= (1ull << 32)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
        } else if (memcmp(tag, "ICCP", 4) == 0) {
            if (!saw_vp8x || saw_iccp || saw_image || clen == 0u ||
                !(vp8x_flags & (uint8_t)ICCP_FLAG)) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_iccp = 1;
            if ((size_t)clen > SIZE_MAX - total_meta) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            total_meta += (size_t)clen;
            rfc_status mst = webp_check_meta_len(limits, (size_t)clen, total_meta);
            if (mst != RFC_OK) {
                return mst;
            }
            out->icc_off = payload_off;
            out->icc_len = (size_t)clen;
        } else if (memcmp(tag, "EXIF", 4) == 0) {
            if (!saw_vp8x || saw_exif || clen == 0u ||
                !(vp8x_flags & (uint8_t)EXIF_FLAG)) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_exif = 1;
            if ((size_t)clen > SIZE_MAX - total_meta) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            total_meta += (size_t)clen;
            rfc_status mst = webp_check_meta_len(limits, (size_t)clen, total_meta);
            if (mst != RFC_OK) {
                return mst;
            }
            rfc_status est =
                rfc_exif_parse_orientation(payload, (size_t)clen,
                                           &out->orientation);
            if (est != RFC_OK) {
                return est;
            }
            out->exif_off = payload_off;
            out->exif_len = (size_t)clen;
        } else if (memcmp(tag, "XMP ", 4) == 0) {
            if (!saw_vp8x || saw_xmp || clen == 0u ||
                !(vp8x_flags & (uint8_t)XMP_FLAG)) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_xmp = 1;
            if ((size_t)clen > SIZE_MAX - total_meta) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            total_meta += (size_t)clen;
            rfc_status mst = webp_check_meta_len(limits, (size_t)clen, total_meta);
            if (mst != RFC_OK) {
                return mst;
            }
            out->xmp_off = payload_off;
            out->xmp_len = (size_t)clen;
        } else if (memcmp(tag, "ALPH", 4) == 0) {
            if (!saw_vp8x || saw_alph || saw_image || clen == 0u ||
                !(vp8x_flags & (uint8_t)ALPHA_FLAG)) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_alph = 1;
        } else if (memcmp(tag, "VP8 ", 4) == 0) {
            if (saw_image || clen < 10u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_image = 1;
        } else if (memcmp(tag, "VP8L", 4) == 0) {
            if (saw_image || saw_alph || clen < 5u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_vp8l = 1;
            saw_image = 1;
        } else {
            if (!saw_vp8x) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }

        pos = (size_t)next_pos;
        chunk_idx++;
    }

    if (container_truncated) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (!saw_image) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (saw_vp8x) {
        if (((vp8x_flags & (uint8_t)ICCP_FLAG) != 0u) != (saw_iccp != 0) ||
            ((vp8x_flags & (uint8_t)EXIF_FLAG) != 0u) != (saw_exif != 0) ||
            ((vp8x_flags & (uint8_t)XMP_FLAG) != 0u) != (saw_xmp != 0)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
    }
    (void)saw_vp8l;

    WebPBitstreamFeatures features;
    VP8StatusCode vst = WebPGetFeatures(bytes, size, &features);
    if (vst == VP8_STATUS_NOT_ENOUGH_DATA) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (vst == VP8_STATUS_OUT_OF_MEMORY) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (vst != VP8_STATUS_OK) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (features.has_animation) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (features.width <= 0 || features.height <= 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (features.width > WEBP_MAX_DIMENSION ||
        features.height > WEBP_MAX_DIMENSION) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (saw_vp8x && ((uint32_t)features.width != canvas_w ||
                     (uint32_t)features.height != canvas_h)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (saw_vp8x && (features.has_alpha || saw_alph) &&
        !(vp8x_flags & (uint8_t)ALPHA_FLAG)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    out->width = (uint32_t)features.width;
    out->height = (uint32_t)features.height;
    out->has_alpha = (features.has_alpha || saw_alph) ? 1 : 0;
    return RFC_OK;
}

static rfc_status webp_prepare_probe(const uint8_t *bytes,
                                     size_t size,
                                     const rfc_limits *limits,
                                     webp_riff_info *out_info,
                                     rfc_image_probe *out_probe) {
    memset(out_probe, 0, sizeof(*out_probe));
    rfc_status st = webp_preflight_riff(bytes, size, limits, out_info);
    if (st != RFC_OK) {
        return st;
    }

    rfc_sample_format sf =
        out_info->has_alpha ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8;
    rfc_alpha_mode am =
        out_info->has_alpha ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
    uint64_t bpp = out_info->has_alpha ? 4ull : 3ull;
    uint64_t row_bytes = 0u;
    uint64_t pixels = 0u;
    uint64_t dec_bytes = 0u;
    if (!webp_mul_u64((uint64_t)out_info->width, bpp, &row_bytes) ||
        row_bytes > (uint64_t)SIZE_MAX ||
        !webp_mul_u64((uint64_t)out_info->width, (uint64_t)out_info->height,
                      &pixels) ||
        !webp_mul_u64(pixels, bpp, &dec_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    out_probe->first_frame.format = sf;
    out_probe->first_frame.alpha = am;
    out_probe->first_frame.width = out_info->width;
    out_probe->first_frame.height = out_info->height;
    out_probe->first_frame.stride_bytes = (size_t)row_bytes;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = pixels;
    out_probe->total_decoded_bytes = dec_bytes;
    out_probe->container_meta.orientation = out_info->orientation;
    /* Keep container_meta pointers NULL until after limits pass cleanly. */
    out_probe->container_meta.icc_len = out_info->icc_len;
    out_probe->container_meta.exif_len = out_info->exif_len;
    out_probe->container_meta.xmp_len = out_info->xmp_len;

    st = rfc_limits_check_probe(limits, out_probe, (uint64_t)size,
                                size > 0u ? 1 : 0);
    if (st != RFC_OK) {
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Session lifecycle, probe, decode, encode                                 */
/* ------------------------------------------------------------------------ */

static rfc_status webp_open(const rfc_exec_context *exec, void **out_sess) {
    (void)exec;
    if (!out_sess) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    webp_session *s = (webp_session *)malloc(sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&s->cancelled, 0);
    *out_sess = s;
    return RFC_OK;
}

static void webp_close(void *session) {
    free(session);
}

static void webp_cancel(void *session) {
    webp_session *s = (webp_session *)session;
    if (s) {
        atomic_store_explicit(&s->cancelled, 1, memory_order_release);
    }
}

static rfc_status webp_probe(void *session,
                             const rfc_exec_context *exec,
                             const uint8_t *bytes,
                             size_t size,
                             rfc_image_probe *out_probe) {
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (webp_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    const rfc_limits *limits = webp_limits_of(exec, &lim_def);
    webp_riff_info info;
    rfc_status st = webp_prepare_probe(bytes, size, limits, &info, out_probe);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_allocator *alloc = webp_alloc_of(exec);
    uint8_t *icc = NULL;
    uint8_t *exif = NULL;
    uint8_t *xmp = NULL;
    if (info.icc_len > 0u) {
        icc = (uint8_t *)alloc->alloc(alloc->ctx, info.icc_len);
        if (!icc) {
            memset(out_probe, 0, sizeof(*out_probe));
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(icc, bytes + info.icc_off, info.icc_len);
    }
    if (info.exif_len > 0u) {
        exif = (uint8_t *)alloc->alloc(alloc->ctx, info.exif_len);
        if (!exif) {
            if (icc) {
                alloc->free(alloc->ctx, icc);
            }
            memset(out_probe, 0, sizeof(*out_probe));
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(exif, bytes + info.exif_off, info.exif_len);
    }
    if (info.xmp_len > 0u) {
        xmp = (uint8_t *)alloc->alloc(alloc->ctx, info.xmp_len);
        if (!xmp) {
            if (exif) {
                alloc->free(alloc->ctx, exif);
            }
            if (icc) {
                alloc->free(alloc->ctx, icc);
            }
            memset(out_probe, 0, sizeof(*out_probe));
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(xmp, bytes + info.xmp_off, info.xmp_len);
    }

    out_probe->container_meta.icc = icc;
    out_probe->container_meta.exif = exif;
    out_probe->container_meta.xmp = xmp;
    return RFC_OK;
}

static rfc_status map_vp8_dec_status(VP8StatusCode code) {
    switch (code) {
        case VP8_STATUS_OK: return RFC_OK;
        case VP8_STATUS_NOT_ENOUGH_DATA: return RFC_ERR_TRUNCATED_INPUT;
        case VP8_STATUS_OUT_OF_MEMORY: return RFC_ERR_OUT_OF_MEMORY;
        case VP8_STATUS_USER_ABORT: return RFC_ERR_CANCELLED;
        case VP8_STATUS_UNSUPPORTED_FEATURE:
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        default: return RFC_ERR_INVALID_ARGUMENT;
    }
}

static rfc_status webp_decode(void *session,
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
            RFC_FORMAT_WEBP, settings, setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (webp_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    const rfc_limits *limits = webp_limits_of(exec, &lim_def);
    webp_riff_info info;
    rfc_image_probe probe;
    rfc_status st = webp_prepare_probe(bytes, size, limits, &info, &probe);
    if (st != RFC_OK) {
        return st;
    }

    rfc_metadata meta = {0};
    meta.orientation = info.orientation;
    if (info.icc_len > 0u) {
        meta.icc = bytes + info.icc_off;
        meta.icc_len = info.icc_len;
    }
    if (info.exif_len > 0u) {
        meta.exif = bytes + info.exif_off;
        meta.exif_len = info.exif_len;
    }
    if (info.xmp_len > 0u) {
        meta.xmp = bytes + info.xmp_off;
        meta.xmp_len = info.xmp_len;
    }

    /* Conservative libwebp internal decode scratch: RGBA scratch + 64 KiB. */
    uint64_t dec_ws64 = 0u;
    if (!webp_mul_u64(probe.total_pixels, 4ull, &dec_ws64) ||
        !webp_add_u64(dec_ws64, 64ull * 1024ull, &dec_ws64) ||
        dec_ws64 > (uint64_t)SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    webp_budget_guard dec_guard;
    st = webp_reserve_budget(exec, (size_t)dec_ws64, &dec_guard);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_allocator *alloc = webp_alloc_of(exec);
    rfc_frame *frames = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(rfc_frame));
    if (!frames) {
        webp_release_budget(&dec_guard);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(frames, 0, sizeof(*frames));
    st = exec ? rfc_exec_alloc_frame(exec, &frames[0], &probe.first_frame, &meta)
              : rfc_frame_alloc(&frames[0], &probe.first_frame, &meta, alloc);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, frames);
        webp_release_budget(&dec_guard);
        return st;
    }

    WebPDecoderConfig dcfg;
    if (!WebPInitDecoderConfig(&dcfg)) {
        rfc_frame_free(&frames[0]);
        alloc->free(alloc->ctx, frames);
        webp_release_budget(&dec_guard);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t threads = rfc_exec_threads(exec);
    dcfg.options.use_threads = (threads > 1u) ? 1 : 0;
    dcfg.output.colorspace = info.has_alpha ? MODE_RGBA : MODE_RGB;
    dcfg.output.is_external_memory = 1;
    dcfg.output.u.RGBA.rgba = frames[0].data;
    dcfg.output.u.RGBA.stride = (int)frames[0].layout.stride_bytes;
    dcfg.output.u.RGBA.size = frames[0].data_len;

    VP8StatusCode dst = WebPDecode(bytes, size, &dcfg);
    WebPFreeDecBuffer(&dcfg.output);
    webp_release_budget(&dec_guard);

    if (webp_is_cancelled(session, exec)) {
        rfc_frame_free(&frames[0]);
        alloc->free(alloc->ctx, frames);
        return RFC_ERR_CANCELLED;
    }
    if (dst != VP8_STATUS_OK) {
        rfc_frame_free(&frames[0]);
        alloc->free(alloc->ctx, frames);
        return map_vp8_dec_status(dst);
    }

    *out_frames = frames;
    *out_frame_count = 1u;
    if (out_meta) {
        *out_meta = frames[0].metadata;
    }
    return RFC_OK;
}

typedef struct webp_writer_buf {
    const rfc_allocator *alloc;
    uint8_t *data;
    size_t len;
    size_t cap;
    int oom;
} webp_writer_buf;

static int webp_writer_cb(const uint8_t *chunk,
                          size_t chunk_len,
                          const WebPPicture *pic) {
    webp_writer_buf *w = (webp_writer_buf *)pic->custom_ptr;
    if (!w || w->oom) {
        return 0;
    }
    if (chunk_len == 0u) {
        return 1;
    }
    if (chunk_len > SIZE_MAX - w->len) {
        w->oom = 1;
        return 0;
    }
    size_t need = w->len + chunk_len;
    if (need > w->cap) {
        size_t next_cap = w->cap ? w->cap : 1024u;
        while (next_cap < need) {
            if (next_cap > SIZE_MAX / 2u) {
                next_cap = need;
                break;
            }
            next_cap *= 2u;
        }
        uint8_t *nb = (uint8_t *)w->alloc->alloc(w->alloc->ctx, next_cap);
        if (!nb) {
            w->oom = 1;
            return 0;
        }
        if (w->data && w->len > 0u) {
            memcpy(nb, w->data, w->len);
        }
        if (w->data) {
            w->alloc->free(w->alloc->ctx, w->data);
        }
        w->data = nb;
        w->cap = next_cap;
    }
    memcpy(w->data + w->len, chunk, chunk_len);
    w->len += chunk_len;
    return 1;
}

typedef struct webp_progress_ctx {
    const void *session;
    const rfc_exec_context *exec;
    int cancelled;
} webp_progress_ctx;

static int webp_progress_cb(int percent, const WebPPicture *pic) {
    (void)percent;
    webp_progress_ctx *p = (webp_progress_ctx *)pic->user_data;
    if (!p) {
        return 1;
    }
    if (webp_is_cancelled(p->session, p->exec)) {
        p->cancelled = 1;
        return 0;
    }
    return 1;
}

static rfc_status webp_encode(void *session,
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

    rfc_status st = rfc_registry_validate_settings(
        RFC_FORMAT_WEBP, setting_count > 0u ? settings : NULL, setting_count,
        NULL);
    if (st != RFC_OK) {
        return st;
    }

    rfc_metadata_policy meta_policy = RFC_META_POLICY_PRESERVE;
    rfc_exactness_class exactness = RFC_EXACT_PIXEL_EXACT;
    for (size_t i = 0u; i < setting_count; i++) {
        if (!settings[i].id || !settings[i].value) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(settings[i].id, "metadata") == 0) {
            st = rfc_metadata_policy_parse(settings[i].value, &meta_policy);
            if (st != RFC_OK) {
                return st;
            }
        } else if (strcmp(settings[i].id, "exactness") == 0) {
            st = rfc_exactness_class_parse(settings[i].value, &exactness);
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
    if (fr->layout.width > WEBP_MAX_DIMENSION ||
        fr->layout.height > WEBP_MAX_DIMENSION) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    rfc_limits lim_def;
    const rfc_limits *limits = webp_limits_of(exec, &lim_def);
    st = rfc_limits_check_image(limits, &norm, 1u);
    if (st != RFC_OK) {
        return st;
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

    /* Validate supplied EXIF and resolve EXIF vs orientation precedence rules
     * under metadata=preserve prior to encoding or buffer reservation. */
    uint8_t synth_exif[26];
    const uint8_t *mux_exif = NULL;
    size_t mux_exif_len = 0u;
    if (meta_policy == RFC_META_POLICY_PRESERVE) {
        if (eff_exif_len > 0u) {
            rfc_orientation exif_ori = RFC_ORIENTATION_UNSPECIFIED;
            st = rfc_exif_parse_orientation(eff_exif, eff_exif_len, &exif_ori);
            if (st != RFC_OK) {
                return st;
            }
            if (exif_ori != RFC_ORIENTATION_UNSPECIFIED) {
                if (eff_ori != RFC_ORIENTATION_UNSPECIFIED &&
                    eff_ori != exif_ori) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
            } else if (eff_ori > RFC_ORIENTATION_NORMAL) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            mux_exif = eff_exif;
            mux_exif_len = eff_exif_len;
        } else if (eff_ori > RFC_ORIENTATION_NORMAL) {
            st = rfc_exif_build_orientation_tiff26(eff_ori, synth_exif);
            if (st != RFC_OK) {
                return st;
            }
            mux_exif = synth_exif;
            mux_exif_len = sizeof(synth_exif);
        }
    }

    if (!exec || !exec->job) {
        st = rfc_metadata_check_source_preflight(
            RFC_FORMAT_WEBP, meta_policy, exactness, meta, frames, frame_count,
            0u, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }

    if (webp_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    uint64_t pixels = 0u;
    uint64_t argb_bytes = 0u;
    uint64_t enc_ws64 = 0u;
    if (!webp_mul_u64((uint64_t)eff_lay.width, (uint64_t)eff_lay.height,
                      &pixels) ||
        !webp_mul_u64(pixels, 4ull, &argb_bytes) ||
        argb_bytes > (uint64_t)SIZE_MAX ||
        !webp_mul_u64(pixels, 6ull, &enc_ws64) ||
        !webp_add_u64(enc_ws64, 128ull * 1024ull, &enc_ws64) ||
        enc_ws64 > (uint64_t)SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    webp_budget_guard enc_guard;
    st = webp_reserve_budget(exec, (size_t)enc_ws64, &enc_guard);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_allocator *alloc = webp_alloc_of(exec);
    uint32_t *argb = (uint32_t *)alloc->alloc(alloc->ctx, (size_t)argb_bytes);
    if (!argb) {
        webp_release_budget(&enc_guard);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    uint32_t w = eff_lay.width;
    uint32_t h = eff_lay.height;
    int is_rgba = (eff_lay.format == RFC_SAMPLE_RGBA8) ? 1 : 0;
    for (uint32_t y = 0u; y < h; y++) {
        const uint8_t *row = fr->data + (size_t)y * eff_lay.stride_bytes;
        uint32_t *dst = argb + (size_t)y * (size_t)w;
        if (is_rgba) {
            for (uint32_t x = 0u; x < w; x++) {
                const uint8_t *px = row + (size_t)x * 4u;
                dst[x] = ((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) |
                         ((uint32_t)px[1] << 8) | (uint32_t)px[2];
            }
        } else {
            for (uint32_t x = 0u; x < w; x++) {
                const uint8_t *px = row + (size_t)x * 3u;
                dst[x] = 0xFF000000u | ((uint32_t)px[0] << 16) |
                         ((uint32_t)px[1] << 8) | (uint32_t)px[2];
            }
        }
    }

    WebPConfig cfg;
    if (!WebPConfigInit(&cfg) || !WebPConfigLosslessPreset(&cfg, 6)) {
        alloc->free(alloc->ctx, argb);
        webp_release_budget(&enc_guard);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t threads = rfc_exec_threads(exec);
    cfg.lossless = 1;
    cfg.exact = 1;
    cfg.thread_level = (threads > 1u) ? 1 : 0;
    if (!WebPValidateConfig(&cfg)) {
        alloc->free(alloc->ctx, argb);
        webp_release_budget(&enc_guard);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    WebPPicture pic;
    if (!WebPPictureInit(&pic)) {
        alloc->free(alloc->ctx, argb);
        webp_release_budget(&enc_guard);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    webp_writer_buf wbuf;
    memset(&wbuf, 0, sizeof(wbuf));
    wbuf.alloc = alloc;

    webp_progress_ctx pctx;
    pctx.session = session;
    pctx.exec = exec;
    pctx.cancelled = 0;

    pic.use_argb = 1;
    pic.width = (int)w;
    pic.height = (int)h;
    pic.argb = argb;
    pic.argb_stride = (int)w;
    pic.writer = webp_writer_cb;
    pic.custom_ptr = &wbuf;
    pic.progress_hook = webp_progress_cb;
    pic.user_data = &pctx;

    int enc_ok = WebPEncode(&cfg, &pic);
    WebPEncodingError enc_err = pic.error_code;

    alloc->free(alloc->ctx, argb);
    webp_release_budget(&enc_guard);

    if (!enc_ok) {
        if (wbuf.data) {
            alloc->free(alloc->ctx, wbuf.data);
        }
        if (pctx.cancelled || enc_err == VP8_ENC_ERROR_USER_ABORT ||
            webp_is_cancelled(session, exec)) {
            return RFC_ERR_CANCELLED;
        }
        if (wbuf.oom || enc_err == VP8_ENC_ERROR_OUT_OF_MEMORY ||
            enc_err == VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        return RFC_ERR_INVALID_ARGUMENT;
    }

    /* Preserve RGBA8 sample format even when all alpha bytes are 255:
     * VP8LEncode clears alpha_is_used when opaque, which would otherwise
     * decode back as RGB8 and trigger RFC_MISMATCH_SAMPLE_FORMAT. */
    if (is_rgba && wbuf.len >= 25u && memcmp(wbuf.data + 12, "VP8L", 4) == 0) {
        wbuf.data[24] |= 0x10u;
    }

    int need_mux = (meta_policy == RFC_META_POLICY_PRESERVE &&
                    (eff_icc_len > 0u || mux_exif_len > 0u ||
                     eff_xmp_len > 0u))
                       ? 1
                       : 0;
    if (!need_mux) {
        out_encoded->data = wbuf.data;
        out_encoded->len = wbuf.len;
        out_encoded->allocator = alloc;
        return RFC_OK;
    }

    uint64_t mux_ws64 = (uint64_t)wbuf.len + (uint64_t)eff_icc_len +
                        (uint64_t)mux_exif_len + (uint64_t)eff_xmp_len +
                        4096ull;
    webp_budget_guard mux_guard;
    st = webp_reserve_budget(exec,
                             mux_ws64 > (uint64_t)SIZE_MAX ? SIZE_MAX
                                                           : (size_t)mux_ws64,
                             &mux_guard);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, wbuf.data);
        return st;
    }

    WebPMux *mux = WebPMuxNew();
    if (!mux) {
        alloc->free(alloc->ctx, wbuf.data);
        webp_release_budget(&mux_guard);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    WebPData img_data;
    img_data.bytes = wbuf.data;
    img_data.size = wbuf.len;
    WebPMuxError merr = WebPMuxSetImage(mux, &img_data, 0);
    if (merr == WEBP_MUX_OK && eff_icc_len > 0u) {
        WebPData d = {eff_icc, eff_icc_len};
        merr = WebPMuxSetChunk(mux, "ICCP", &d, 0);
    }
    if (merr == WEBP_MUX_OK && mux_exif_len > 0u) {
        WebPData d = {mux_exif, mux_exif_len};
        merr = WebPMuxSetChunk(mux, "EXIF", &d, 0);
    }
    if (merr == WEBP_MUX_OK && eff_xmp_len > 0u) {
        WebPData d = {eff_xmp, eff_xmp_len};
        merr = WebPMuxSetChunk(mux, "XMP ", &d, 0);
    }

    WebPData assembled;
    WebPDataInit(&assembled);
    if (merr == WEBP_MUX_OK) {
        merr = WebPMuxAssemble(mux, &assembled);
    }
    WebPMuxDelete(mux);
    alloc->free(alloc->ctx, wbuf.data);
    wbuf.data = NULL;
    wbuf.len = 0u;

    if (merr != WEBP_MUX_OK || !assembled.bytes || assembled.size == 0u) {
        WebPDataClear(&assembled);
        webp_release_budget(&mux_guard);
        return (merr == WEBP_MUX_MEMORY_ERROR) ? RFC_ERR_OUT_OF_MEMORY
                                               : RFC_ERR_INVALID_ARGUMENT;
    }

    uint8_t *final_buf = (uint8_t *)alloc->alloc(alloc->ctx, assembled.size);
    if (!final_buf) {
        WebPDataClear(&assembled);
        webp_release_budget(&mux_guard);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(final_buf, assembled.bytes, assembled.size);
    size_t final_len = assembled.size;
    WebPDataClear(&assembled);
    webp_release_budget(&mux_guard);

    out_encoded->data = final_buf;
    out_encoded->len = final_len;
    out_encoded->allocator = alloc;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Adapter registration                                                     */
/* ------------------------------------------------------------------------ */

static const rfc_session_adapter k_webp_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_WEBP,
    webp_open,
    webp_close,
    webp_probe,
    webp_encode,
    webp_decode,
    webp_cancel,
    NULL
};

rfc_status rfc_codecs_register_webp(void) {
    return rfc_registry_register_session(&k_webp_adapter);
}

rfc_status rfc_codecs_unregister_webp(void) {
    return rfc_registry_unregister_session(RFC_FORMAT_WEBP);
}
