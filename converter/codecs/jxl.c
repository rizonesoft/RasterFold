/* Shipped C11 codec session adapters for D10 T02 s3:
 *   - RFC_FORMAT_JPEG_XL (bundled libjxl 0.13.0: lossless modular pixel
 *     encode across 1..16-bit GRAY/RGB/RGBA left-aligned samples, single-frame
 *     lossless & lossy XYB decode, ICC/Exif/XMP + "brob" box handling,
 *     zero-allocation codestream/ISOBMFF preflight, cancellable parallel runner,
 *     and JxlMemoryManager rfc_budget hooks).
 *   - rfc_jpeg_reconstruct_adapter: zero-alloc JPEG marker scan, lossless
 *     JPEG -> JPEG XL transcode (JxlEncoderStoreJPEGMetadata), and fresh
 *     JXL -> JPEG original-byte reconstruction. */

#ifndef JXL_STATIC_DEFINE
#define JXL_STATIC_DEFINE
#endif
#ifndef JXL_THREADS_STATIC_DEFINE
#define JXL_THREADS_STATIC_DEFINE
#endif
#ifndef JXL_CMS_STATIC_DEFINE
#define JXL_CMS_STATIC_DEFINE
#endif

#include "rfc_codecs.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <jxl/cms.h>
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/memory_manager.h>
#include <jxl/parallel_runner.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/types.h>

/* ------------------------------------------------------------------------ */
/* Allocator, limits, cancellation, arithmetic, and endian helpers          */
/* ------------------------------------------------------------------------ */

static void *jxl_stdlib_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void jxl_stdlib_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const rfc_allocator k_jxl_stdlib_alloc = {
    jxl_stdlib_alloc, jxl_stdlib_free, NULL
};

static const rfc_allocator *jxl_alloc_of(const rfc_exec_context *exec) {
    const rfc_allocator *a = rfc_exec_allocator(exec);
    return (a && a->alloc && a->free) ? a : &k_jxl_stdlib_alloc;
}

static const rfc_limits *jxl_limits_of(const rfc_exec_context *exec,
                                       rfc_limits *scratch) {
    const rfc_limits *l = rfc_exec_limits(exec);
    if (l) {
        return l;
    }
    rfc_limits_default(scratch);
    return scratch;
}

static size_t jxl_meta_cap_of(const rfc_limits *limits) {
    uint64_t cap = RFC_MAX_ICC_BYTES;
    if (limits && limits->max_metadata_bytes > 0u &&
        limits->max_metadata_bytes < cap) {
        cap = limits->max_metadata_bytes;
    }
    if (cap > SIZE_MAX) {
        cap = SIZE_MAX;
    }
    return (size_t)cap;
}

typedef struct rfc_jxl_session_box {
    atomic_int cancelled;
    const rfc_allocator *alloc;
    rfc_allocator alloc_copy;
} rfc_jxl_session_box;

static int jxl_is_cancelled(const void *session,
                            const rfc_exec_context *exec) {
    const rfc_jxl_session_box *s = (const rfc_jxl_session_box *)session;
    if (s && atomic_load_explicit(&s->cancelled, memory_order_acquire) != 0) {
        return 1;
    }
    return exec ? rfc_exec_is_cancelled(exec) : 0;
}

typedef struct jxl_budget_guard {
    rfc_budget *budget;
    size_t bytes;
    int active;
} jxl_budget_guard;

static rfc_status jxl_reserve_budget(const rfc_exec_context *exec,
                                     size_t bytes,
                                     jxl_budget_guard *out) {
    memset(out, 0, sizeof(*out));
    rfc_budget *b = rfc_exec_budget(exec);
    if (!b || bytes == 0u) {
        return RFC_OK;
    }
    rfc_status st = rfc_budget_reserve(b, bytes);
    if (st != RFC_OK) {
        return st;
    }
    out->budget = b;
    out->bytes = bytes;
    out->active = 1;
    return RFC_OK;
}

static void jxl_release_budget(jxl_budget_guard *g) {
    if (g && g->active && g->budget) {
        rfc_budget_release(g->budget, g->bytes);
        g->budget = NULL;
        g->bytes = 0u;
        g->active = 0;
    }
}

static void *jxl_mm_alloc(void *opaque, size_t size) {
    const rfc_allocator *a = (const rfc_allocator *)opaque;
    if (!a || !a->alloc) {
        return malloc(size ? size : 1u);
    }
    return a->alloc(a->ctx, size ? size : 1u);
}

static void jxl_mm_free(void *opaque, void *address) {
    if (!address) {
        return;
    }
    const rfc_allocator *a = (const rfc_allocator *)opaque;
    if (!a || !a->free) {
        free(address);
        return;
    }
    a->free(a->ctx, address);
}

static void jxl_init_mm(const rfc_exec_context *exec,
                        const rfc_allocator *alloc,
                        JxlMemoryManager *out_mm) {
    memset(out_mm, 0, sizeof(*out_mm));
    rfc_budget *budget = rfc_exec_budget(exec);
    if (budget) {
        out_mm->opaque = budget;
        out_mm->alloc = rfc_budget_jxl_alloc;
        out_mm->free = rfc_budget_jxl_free;
    } else {
        out_mm->opaque = (void *)alloc;
        out_mm->alloc = jxl_mm_alloc;
        out_mm->free = jxl_mm_free;
    }
}

static int jxl_budget_rejected(const rfc_exec_context *exec) {
    rfc_budget *b = rfc_exec_budget(exec);
    if (!b) {
        return 0;
    }
    rfc_budget_stats bs;
    rfc_budget_stats_get(b, &bs);
    return bs.rejected_count > 0u;
}

static int jxl_add_sz(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static int jxl_mul_sz(size_t a, size_t b, size_t *out) {
    if (a != 0u && b > SIZE_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static uint16_t jxl_rd16be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t jxl_rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t jxl_rd64be(const uint8_t *p) {
    return ((uint64_t)jxl_rd32be(p) << 32) | (uint64_t)jxl_rd32be(p + 4);
}

static void jxl_wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint32_t jxl_sample_width_bits(rfc_sample_format fmt) {
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

static uint32_t jxl_sample_channels(rfc_sample_format fmt) {
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

/* ------------------------------------------------------------------------ */
/* Cancellable JxlParallelRunner wrapper                                    */
/* ------------------------------------------------------------------------ */

typedef struct jxl_runner_ctx {
    const void *session;
    const rfc_exec_context *exec;
    void *inner;
} jxl_runner_ctx;

typedef struct jxl_task_wrap {
    jxl_runner_ctx *rc;
    void *inner_opaque;
    JxlParallelRunInit inner_init;
    JxlParallelRunFunction inner_func;
} jxl_task_wrap;

static int jxl_wrap_init(void *opaque, size_t num_threads) {
    jxl_task_wrap *w = (jxl_task_wrap *)opaque;
    if (jxl_is_cancelled(w->rc->session, w->rc->exec)) {
        return JXL_PARALLEL_RET_RUNNER_ERROR;
    }
    return w->inner_init ? w->inner_init(w->inner_opaque, num_threads)
                         : JXL_PARALLEL_RET_SUCCESS;
}

static void jxl_wrap_func(void *opaque, uint32_t value, size_t thread_id) {
    jxl_task_wrap *w = (jxl_task_wrap *)opaque;
    if (w->inner_func) {
        w->inner_func(w->inner_opaque, value, thread_id);
    }
}

static JxlParallelRetCode jxl_parallel_runner(void *runner_opaque,
                                              void *jpegxl_opaque,
                                              JxlParallelRunInit init,
                                              JxlParallelRunFunction func,
                                              uint32_t start_range,
                                              uint32_t end_range) {
    jxl_runner_ctx *rc = (jxl_runner_ctx *)runner_opaque;
    if (!rc || jxl_is_cancelled(rc->session, rc->exec)) {
        return JXL_PARALLEL_RET_RUNNER_ERROR;
    }
    if (rc->inner != NULL) {
        jxl_task_wrap wrap;
        wrap.rc = rc;
        wrap.inner_opaque = jpegxl_opaque;
        wrap.inner_init = init;
        wrap.inner_func = func;
        JxlParallelRetCode ret = JxlThreadParallelRunner(
            rc->inner, &wrap, jxl_wrap_init, jxl_wrap_func, start_range,
            end_range);
        if (jxl_is_cancelled(rc->session, rc->exec)) {
            return JXL_PARALLEL_RET_RUNNER_ERROR;
        }
        return ret;
    }
    if (init && init(jpegxl_opaque, 1u) != JXL_PARALLEL_RET_SUCCESS) {
        return JXL_PARALLEL_RET_RUNNER_ERROR;
    }
    if (func) {
        for (uint32_t i = start_range; i < end_range; i++) {
            if (jxl_is_cancelled(rc->session, rc->exec)) {
                return JXL_PARALLEL_RET_RUNNER_ERROR;
            }
            func(jpegxl_opaque, i, 0u);
        }
    }
    return jxl_is_cancelled(rc->session, rc->exec)
               ? JXL_PARALLEL_RET_RUNNER_ERROR
               : JXL_PARALLEL_RET_SUCCESS;
}

static rfc_status jxl_runner_init(jxl_runner_ctx *rc,
                                  const void *session,
                                  const rfc_exec_context *exec,
                                  const JxlMemoryManager *mm) {
    memset(rc, 0, sizeof(*rc));
    rc->session = session;
    rc->exec = exec;
    uint32_t threads = rfc_exec_threads(exec);
    if (threads > 1u) {
        rc->inner = JxlThreadParallelRunnerCreate(mm, (size_t)threads);
        if (!rc->inner) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
    }
    return RFC_OK;
}

static void jxl_runner_destroy(jxl_runner_ctx *rc) {
    if (rc && rc->inner) {
        JxlThreadParallelRunnerDestroy(rc->inner);
        rc->inner = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Zero-allocation JPEG XL codestream & ISOBMFF container preflight         */
/* ------------------------------------------------------------------------ */

static const uint8_t k_jxl_box_sig[12] = {
    0x00u, 0x00u, 0x00u, 0x0Cu, 'J', 'X', 'L', ' ', 0x0Du, 0x0Au, 0x87u, 0x0Au
};

static rfc_status jxl_preflight_stream(const uint8_t *bytes, size_t size) {
    if (!bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < 2u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (bytes[0] == 0xFFu && bytes[1] == 0x0Au) {
        if (size < 3u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        return RFC_OK;
    }
    if (size < sizeof(k_jxl_box_sig)) {
        if (memcmp(bytes, k_jxl_box_sig, size) == 0) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (memcmp(bytes, k_jxl_box_sig, sizeof(k_jxl_box_sig)) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    size_t off = sizeof(k_jxl_box_sig);
    int saw_ftyp = 0;
    int saw_codestream = 0;
    while (off < size) {
        if (size - off < 8u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint64_t bsize = (uint64_t)jxl_rd32be(bytes + off);
        const uint8_t *btype = bytes + off + 4u;
        uint64_t hdr = 8u;
        if (bsize == 1u) {
            if (size - off < 16u) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            bsize = jxl_rd64be(bytes + off + 8u);
            hdr = 16u;
            if (bsize < 16u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        } else if (bsize == 0u) {
            bsize = (uint64_t)(size - off);
        } else if (bsize < 8u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (bsize > (uint64_t)(size - off)) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        const uint8_t *payload = bytes + off + hdr;
        uint64_t plen = bsize - hdr;

        if (off == sizeof(k_jxl_box_sig)) {
            if (memcmp(btype, "ftyp", 4) != 0 || plen < 8u || (plen % 4u) != 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            int has_jxl = (memcmp(payload, "jxl ", 4) == 0);
            for (uint64_t boff = 8u; !has_jxl && boff + 4u <= plen; boff += 4u) {
                if (memcmp(payload + boff, "jxl ", 4) == 0) {
                    has_jxl = 1;
                }
            }
            if (!has_jxl) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_ftyp = 1;
        }
        if (memcmp(btype, "jxlc", 4) == 0) {
            saw_codestream = 1;
        } else if (memcmp(btype, "jxlp", 4) == 0) {
            if (plen < 4u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_codestream = 1;
        }
        off += (size_t)bsize;
    }

    if (!saw_ftyp || !saw_codestream) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Shared probe & decode implementation                                     */
/* ------------------------------------------------------------------------ */

static int jxl_is_standard_srgb_ce(const JxlColorEncoding *ce) {
    if (!ce) {
        return 0;
    }
    if (ce->color_space == JXL_COLOR_SPACE_RGB) {
        return ce->white_point == JXL_WHITE_POINT_D65 &&
               ce->primaries == JXL_PRIMARIES_SRGB &&
               ce->transfer_function == JXL_TRANSFER_FUNCTION_SRGB &&
               ce->rendering_intent == JXL_RENDERING_INTENT_RELATIVE;
    }
    if (ce->color_space == JXL_COLOR_SPACE_GRAY) {
        return ce->white_point == JXL_WHITE_POINT_D65 &&
               ce->transfer_function == JXL_TRANSFER_FUNCTION_SRGB &&
               ce->rendering_intent == JXL_RENDERING_INTENT_RELATIVE;
    }
    return 0;
}

static rfc_status jxl_validate_basic_info(JxlDecoder *dec,
                                          const JxlBasicInfo *bi,
                                          rfc_layout *out_layout) {
    if (bi->xsize == 0u || bi->ysize == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (bi->have_animation) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (bi->exponent_bits_per_sample != 0u || bi->alpha_exponent_bits != 0u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (bi->bits_per_sample < 1u || bi->bits_per_sample > 16u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (bi->num_color_channels != 1u && bi->num_color_channels != 3u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (bi->num_extra_channels > 1u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (bi->num_color_channels == 1u && bi->num_extra_channels != 0u) {
        return RFC_ERR_UNSUPPORTED_ALPHA;
    }

    int has_alpha = 0;
    if (bi->num_extra_channels == 1u) {
        JxlExtraChannelInfo eci;
        memset(&eci, 0, sizeof(eci));
        if (JxlDecoderGetExtraChannelInfo(dec, 0u, &eci) != JXL_DEC_SUCCESS) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (eci.type != JXL_CHANNEL_ALPHA) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
        if (bi->alpha_premultiplied) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
        if (eci.bits_per_sample != bi->bits_per_sample ||
            eci.exponent_bits_per_sample != 0u) {
            return RFC_ERR_UNSUPPORTED_SAMPLE;
        }
        has_alpha = 1;
    } else if (bi->alpha_bits != 0u) {
        return RFC_ERR_UNSUPPORTED_ALPHA;
    }

    uint32_t store_bits = (bi->bits_per_sample <= 8u) ? 8u : 16u;
    rfc_sample_format fmt;
    uint32_t channels;
    rfc_alpha_mode alpha_mode =
        has_alpha ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
    if (bi->num_color_channels == 1u) {
        fmt = (store_bits == 8u) ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_GRAY16;
        channels = 1u;
    } else if (has_alpha) {
        fmt = (store_bits == 8u) ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGBA16;
        channels = 4u;
    } else {
        fmt = (store_bits == 8u) ? RFC_SAMPLE_RGB8 : RFC_SAMPLE_RGB16;
        channels = 3u;
    }

    size_t row_bytes = 0u;
    if (!jxl_mul_sz((size_t)bi->xsize,
                    (size_t)channels * (size_t)(store_bits / 8u), &row_bytes)) {
        return RFC_ERR_OVERFLOW;
    }

    memset(out_layout, 0, sizeof(*out_layout));
    out_layout->format = fmt;
    out_layout->alpha = alpha_mode;
    out_layout->width = bi->xsize;
    out_layout->height = bi->ysize;
    out_layout->stride_bytes = row_bytes;
    out_layout->bits_per_sample = bi->bits_per_sample;
    return RFC_OK;
}

typedef struct jxl_box_capture {
    uint8_t *buf;
    size_t cap;
    int active;
    int is_exif;
    int is_xmp;
} jxl_box_capture;

static void jxl_box_capture_reset(jxl_box_capture *cap,
                                  const rfc_allocator *alloc) {
    if (cap->buf && alloc && alloc->free) {
        alloc->free(alloc->ctx, cap->buf);
    }
    memset(cap, 0, sizeof(*cap));
}

static rfc_status jxl_box_begin(JxlDecoder *dec,
                                jxl_box_capture *cap,
                                const rfc_allocator *alloc,
                                size_t meta_cap,
                                int already_has_exif,
                                int already_has_xmp) {
    JxlBoxType btype;
    memset(btype, 0, sizeof(btype));
    if (JxlDecoderGetBoxType(dec, btype, JXL_TRUE) != JXL_DEC_SUCCESS) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int is_exif = (memcmp(btype, "Exif", 4) == 0);
    int is_xmp = (memcmp(btype, "xml ", 4) == 0);
    if (!is_exif && !is_xmp) {
        return RFC_OK;
    }
    if ((is_exif && already_has_exif) || (is_xmp && already_has_xmp)) {
        return RFC_OK;
    }
    if (meta_cap == 0u) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    size_t init_cap = 4096u;
    size_t hard_cap = (meta_cap < SIZE_MAX) ? (meta_cap + 1u) : meta_cap;
    if (is_exif && hard_cap <= SIZE_MAX - 4u && meta_cap <= SIZE_MAX - 64u) {
        hard_cap = meta_cap + 64u;
    }
    if (init_cap > hard_cap) {
        init_cap = hard_cap;
    }
    uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, init_cap);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (JxlDecoderSetBoxBuffer(dec, buf, init_cap) != JXL_DEC_SUCCESS) {
        alloc->free(alloc->ctx, buf);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    cap->buf = buf;
    cap->cap = init_cap;
    cap->active = 1;
    cap->is_exif = is_exif;
    cap->is_xmp = is_xmp;
    return RFC_OK;
}

static rfc_status jxl_box_grow(JxlDecoder *dec,
                               jxl_box_capture *cap,
                               const rfc_allocator *alloc,
                               size_t meta_cap) {
    if (!cap->active) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t rem = JxlDecoderReleaseBoxBuffer(dec);
    size_t have = (rem <= cap->cap) ? (cap->cap - rem) : cap->cap;
    size_t max_allowed =
        cap->is_exif && meta_cap <= SIZE_MAX - 4u ? (meta_cap + 4u) : meta_cap;
    if (have >= max_allowed) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    size_t hard_cap =
        (max_allowed < SIZE_MAX) ? (max_allowed + 1u) : max_allowed;
    size_t next_cap =
        (cap->cap <= SIZE_MAX / 2u) ? (cap->cap * 2u) : hard_cap;
    if (next_cap <= have) {
        next_cap = hard_cap;
    }
    if (next_cap > hard_cap) {
        next_cap = hard_cap;
    }
    uint8_t *nbuf = (uint8_t *)alloc->alloc(alloc->ctx, next_cap);
    if (!nbuf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (have > 0u) {
        memcpy(nbuf, cap->buf, have);
    }
    alloc->free(alloc->ctx, cap->buf);
    cap->buf = nbuf;
    cap->cap = next_cap;
    if (JxlDecoderSetBoxBuffer(dec, cap->buf + have, next_cap - have) !=
        JXL_DEC_SUCCESS) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

static rfc_status jxl_box_finish(JxlDecoder *dec,
                                 jxl_box_capture *cap,
                                 const rfc_allocator *alloc,
                                 size_t meta_cap,
                                 rfc_metadata *meta) {
    if (!cap->active) {
        return RFC_OK;
    }
    size_t rem = JxlDecoderReleaseBoxBuffer(dec);
    size_t total = (rem <= cap->cap) ? (cap->cap - rem) : cap->cap;
    rfc_status st = RFC_OK;
    if (cap->is_exif) {
        if (total < 4u) {
            st = RFC_ERR_INVALID_ARGUMENT;
        } else {
            uint32_t tiff_off = jxl_rd32be(cap->buf);
            if ((uint64_t)tiff_off > (uint64_t)(total - 4u)) {
                st = RFC_ERR_INVALID_ARGUMENT;
            } else {
                const uint8_t *tiff = cap->buf + 4u + (size_t)tiff_off;
                size_t tiff_len = total - 4u - (size_t)tiff_off;
                rfc_orientation dummy_ori = RFC_ORIENTATION_UNSPECIFIED;
                if (tiff_len > meta_cap) {
                    st = RFC_ERR_LIMIT_EXCEEDED;
                } else if (rfc_exif_parse_orientation(tiff, tiff_len,
                                                      &dummy_ori) != RFC_OK) {
                    st = RFC_ERR_INVALID_ARGUMENT;
                } else {
                    uint8_t *copy = (uint8_t *)alloc->alloc(alloc->ctx, tiff_len);
                    if (!copy) {
                        st = RFC_ERR_OUT_OF_MEMORY;
                    } else {
                        memcpy(copy, tiff, tiff_len);
                        meta->exif = copy;
                        meta->exif_len = tiff_len;
                    }
                }
            }
        }
    } else if (cap->is_xmp) {
        if (total > meta_cap) {
            st = RFC_ERR_LIMIT_EXCEEDED;
        } else if (total > 0u) {
            uint8_t *copy = (uint8_t *)alloc->alloc(alloc->ctx, total);
            if (!copy) {
                st = RFC_ERR_OUT_OF_MEMORY;
            } else {
                memcpy(copy, cap->buf, total);
                meta->xmp = copy;
                meta->xmp_len = total;
            }
        }
    }
    jxl_box_capture_reset(cap, alloc);
    return st;
}

static rfc_status jxl_box_discard_checked(JxlDecoder *dec,
                                          jxl_box_capture *cap,
                                          const rfc_allocator *alloc,
                                          size_t meta_cap) {
    if (!cap->active) {
        return RFC_OK;
    }
    size_t rem = JxlDecoderReleaseBoxBuffer(dec);
    size_t total = (rem <= cap->cap) ? (cap->cap - rem) : cap->cap;
    size_t bmax =
        (cap->is_exif && meta_cap <= SIZE_MAX - 4u) ? (meta_cap + 4u) : meta_cap;
    jxl_box_capture_reset(cap, alloc);
    if (total > bmax) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return RFC_OK;
}

static rfc_status jxl_unscale_left_align(rfc_frame *frame) {
    uint32_t S = jxl_sample_width_bits(frame->layout.format);
    uint32_t b = frame->layout.bits_per_sample;
    uint32_t ch = jxl_sample_channels(frame->layout.format);
    if (S == 0u || ch == 0u || b < 1u || b > S) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    uint32_t shift = S - b;
    if (shift == 0u) {
        return RFC_OK;
    }
    size_t row_samples = (size_t)frame->layout.width * (size_t)ch;
    for (uint32_t y = 0u; y < frame->layout.height; y++) {
        uint8_t *row = frame->data + (size_t)y * frame->layout.stride_bytes;
        if (S == 8u) {
            for (size_t i = 0u; i < row_samples; i++) {
                row[i] = (uint8_t)(row[i] << shift);
            }
        } else {
            for (size_t i = 0u; i < row_samples; i++) {
                uint16_t v = 0u;
                memcpy(&v, row + i * 2u, 2u);
                v = (uint16_t)(v << shift);
                memcpy(row + i * 2u, &v, 2u);
            }
        }
    }
    return RFC_OK;
}

static rfc_status jxl_decode_common(void *session,
                                    const rfc_exec_context *exec,
                                    const uint8_t *bytes,
                                    size_t size,
                                    int want_pixels,
                                    rfc_image_probe *out_probe,
                                    rfc_frame **out_frames,
                                    size_t *out_frame_count,
                                    rfc_metadata *out_meta) {
    if (out_probe) {
        memset(out_probe, 0, sizeof(*out_probe));
    }
    if (out_frames) {
        *out_frames = NULL;
    }
    if (out_frame_count) {
        *out_frame_count = 0u;
    }
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (!bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (jxl_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits scratch_lim;
    const rfc_limits *limits = jxl_limits_of(exec, &scratch_lim);
    const rfc_allocator *alloc = jxl_alloc_of(exec);
    size_t meta_cap = jxl_meta_cap_of(limits);

    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }
    st = jxl_preflight_stream(bytes, size);
    if (st != RFC_OK) {
        return st;
    }

    JxlMemoryManager mm;
    jxl_init_mm(exec, alloc, &mm);

    jxl_runner_ctx runner_ctx;
    st = jxl_runner_init(&runner_ctx, session, exec, &mm);
    if (st != RFC_OK) {
        return st;
    }

    /* Account modestly for internal C++ metadata/color vectors outside mm. */
    jxl_budget_guard bg;
    size_t reserve_est = (size < 65536u) ? size : 65536u;
    st = jxl_reserve_budget(exec, reserve_est, &bg);
    if (st != RFC_OK) {
        jxl_runner_destroy(&runner_ctx);
        return st;
    }

    JxlDecoder *dec = JxlDecoderCreate(&mm);
    if (!dec) {
        jxl_release_budget(&bg);
        jxl_runner_destroy(&runner_ctx);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    rfc_layout layout;
    memset(&layout, 0, sizeof(layout));
    rfc_frame *frames = NULL;
    size_t frame_count = 0u;
    jxl_box_capture box_cap;
    memset(&box_cap, 0, sizeof(box_cap));

    JxlBasicInfo bi;
    memset(&bi, 0, sizeof(bi));
    int have_bi = 0;
    int have_color = 0;
    int frame_seen = 0;
    int pixel_buf_set = 0;
    int full_image_done = 0;
    int subscribed_events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                            JXL_DEC_BOX | JXL_DEC_BOX_COMPLETE | JXL_DEC_FRAME;
    if (want_pixels) {
        subscribed_events |= JXL_DEC_FULL_IMAGE;
    }

    if (JxlDecoderSetParallelRunner(dec, jxl_parallel_runner, &runner_ctx) !=
            JXL_DEC_SUCCESS ||
        JxlDecoderSetKeepOrientation(dec, JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetUnpremultiplyAlpha(dec, JXL_FALSE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetDecompressBoxes(dec, JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetCms(dec, *JxlGetDefaultCms()) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(dec, subscribed_events) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, bytes, size) != JXL_DEC_SUCCESS) {
        st = jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                       : RFC_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }
    JxlDecoderCloseInput(dec);

    for (;;) {
        if (jxl_is_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        JxlDecoderStatus dstat = JxlDecoderProcessInput(dec);
        if (jxl_is_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        if (dstat == JXL_DEC_SUCCESS) {
            if (box_cap.active) {
                st = jxl_box_finish(dec, &box_cap, alloc, meta_cap, &meta);
            }
            break;
        } else if (dstat == JXL_DEC_ERROR) {
            st = jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                           : RFC_ERR_INVALID_ARGUMENT;
            break;
        } else if (dstat == JXL_DEC_NEED_MORE_INPUT) {
            st = RFC_ERR_TRUNCATED_INPUT;
            break;
        } else if (dstat == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(dec, &bi) != JXL_DEC_SUCCESS) {
                st = RFC_ERR_INVALID_ARGUMENT;
                break;
            }
            st = jxl_validate_basic_info(dec, &bi, &layout);
            if (st != RFC_OK) {
                break;
            }
            meta.orientation = (bi.orientation >= 1 && bi.orientation <= 8)
                                   ? (rfc_orientation)bi.orientation
                                   : RFC_ORIENTATION_NORMAL;
            have_bi = 1;

            rfc_image_probe pr;
            memset(&pr, 0, sizeof(pr));
            pr.frame_count = 1u;
            pr.first_frame = layout;
            pr.total_pixels = (uint64_t)layout.width * (uint64_t)layout.height;
            pr.total_decoded_bytes =
                (uint64_t)layout.height * (uint64_t)layout.stride_bytes;
            st = rfc_limits_check_probe(limits, &pr, (uint64_t)size,
                                        size > 0u);
            if (st != RFC_OK) {
                break;
            }
        } else if (dstat == JXL_DEC_COLOR_ENCODING) {
            have_color = 1;
            JxlColorEncoding orig_ce;
            memset(&orig_ce, 0, sizeof(orig_ce));
            JxlDecoderStatus est = JxlDecoderGetColorAsEncodedProfile(
                dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &orig_ce);
            if (est != JXL_DEC_SUCCESS || !jxl_is_standard_srgb_ce(&orig_ce)) {
                size_t icc_sz = 0u;
                if (JxlDecoderGetICCProfileSize(
                        dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &icc_sz) ==
                    JXL_DEC_SUCCESS &&
                    icc_sz > 0u) {
                    if (icc_sz > meta_cap) {
                        st = RFC_ERR_LIMIT_EXCEEDED;
                        break;
                    }
                    uint8_t *icc_buf =
                        (uint8_t *)alloc->alloc(alloc->ctx, icc_sz);
                    if (!icc_buf) {
                        st = RFC_ERR_OUT_OF_MEMORY;
                        break;
                    }
                    if (JxlDecoderGetColorAsICCProfile(
                            dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, icc_buf,
                            icc_sz) != JXL_DEC_SUCCESS) {
                        alloc->free(alloc->ctx, icc_buf);
                        st = RFC_ERR_INVALID_ARGUMENT;
                        break;
                    }
                    if (meta.icc) {
                        alloc->free(alloc->ctx, (void *)meta.icc);
                    }
                    meta.icc = icc_buf;
                    meta.icc_len = icc_sz;
                }
            }
            if (have_bi && !bi.uses_original_profile) {
                JxlColorEncoding target_ce;
                memset(&target_ce, 0, sizeof(target_ce));
                JxlColorEncodingSetToSRGB(
                    &target_ce,
                    (bi.num_color_channels == 1u) ? JXL_TRUE : JXL_FALSE);
                (void)JxlDecoderSetOutputColorProfile(dec, &target_ce, NULL, 0u);
            }
        } else if (dstat == JXL_DEC_BOX) {
            if (box_cap.active) {
                st = jxl_box_finish(dec, &box_cap, alloc, meta_cap, &meta);
                if (st != RFC_OK) {
                    break;
                }
            }
            st = jxl_box_begin(dec, &box_cap, alloc, meta_cap,
                               meta.exif != NULL, meta.xmp != NULL);
            if (st != RFC_OK) {
                break;
            }
        } else if (dstat == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
            st = jxl_box_grow(dec, &box_cap, alloc, meta_cap);
            if (st != RFC_OK) {
                break;
            }
        } else if (dstat == JXL_DEC_BOX_COMPLETE) {
            st = jxl_box_finish(dec, &box_cap, alloc, meta_cap, &meta);
            if (st != RFC_OK) {
                break;
            }
        } else if (dstat == JXL_DEC_FRAME) {
            frame_seen++;
            if (frame_seen > 1) {
                st = RFC_ERR_UNSUPPORTED_COMBINATION;
                break;
            }
            if (want_pixels && have_bi && !pixel_buf_set) {
                frames = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(*frames));
                if (!frames) {
                    st = RFC_ERR_OUT_OF_MEMORY;
                    break;
                }
                memset(frames, 0, sizeof(*frames));
                st = rfc_exec_alloc_frame(exec, &frames[0], &layout, NULL);
                if (st != RFC_OK) {
                    alloc->free(alloc->ctx, frames);
                    frames = NULL;
                    break;
                }
                frame_count = 1u;
                JxlPixelFormat px;
                px.num_channels = jxl_sample_channels(layout.format);
                px.data_type =
                    (jxl_sample_width_bits(layout.format) == 16u)
                        ? JXL_TYPE_UINT16
                        : JXL_TYPE_UINT8;
                px.endianness = JXL_NATIVE_ENDIAN;
                px.align = 0u;
                JxlBitDepth bd;
                bd.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
                bd.bits_per_sample = 0u;
                bd.exponent_bits_per_sample = 0u;
                size_t buf_sz = 0u;
                if (JxlDecoderImageOutBufferSize(dec, &px, &buf_sz) !=
                        JXL_DEC_SUCCESS ||
                    buf_sz != frames[0].data_len ||
                    JxlDecoderSetImageOutBuffer(dec, &px, frames[0].data,
                                                frames[0].data_len) !=
                        JXL_DEC_SUCCESS ||
                    JxlDecoderSetImageOutBitDepth(dec, &bd) !=
                        JXL_DEC_SUCCESS) {
                    st = jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                                   : RFC_ERR_INVALID_ARGUMENT;
                    break;
                }
                pixel_buf_set = 1;
            }
        } else if (dstat == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            st = RFC_ERR_INVALID_ARGUMENT;
            break;
        } else if (dstat == JXL_DEC_FULL_IMAGE) {
            full_image_done = 1;
        } else {
            st = RFC_ERR_INVALID_ARGUMENT;
            break;
        }
    }

cleanup:
    jxl_box_capture_reset(&box_cap, alloc);
    JxlDecoderDestroy(dec);
    jxl_release_budget(&bg);
    jxl_runner_destroy(&runner_ctx);

    if (st == RFC_OK && (!have_bi || !have_color || frame_seen != 1)) {
        st = RFC_ERR_TRUNCATED_INPUT;
    }
    if (st == RFC_OK && want_pixels && !full_image_done) {
        st = RFC_ERR_TRUNCATED_INPUT;
    }
    if (st == RFC_OK) {
        st = rfc_limits_check_metadata(limits, &meta);
    }

    if (st == RFC_OK && want_pixels) {
        st = jxl_unscale_left_align(&frames[0]);
    }
    if (st == RFC_OK && want_pixels) {
        frames[0].metadata = meta;
        frames[0].allocator = alloc;
        if (meta.icc && meta.icc_len > 0u) {
            uint8_t *ic = (uint8_t *)alloc->alloc(alloc->ctx, meta.icc_len);
            if (!ic) {
                st = RFC_ERR_OUT_OF_MEMORY;
            } else {
                memcpy(ic, meta.icc, meta.icc_len);
                frames[0].metadata.icc = ic;
            }
        }
        if (st == RFC_OK && meta.exif && meta.exif_len > 0u) {
            uint8_t *ex = (uint8_t *)alloc->alloc(alloc->ctx, meta.exif_len);
            if (!ex) {
                st = RFC_ERR_OUT_OF_MEMORY;
            } else {
                memcpy(ex, meta.exif, meta.exif_len);
                frames[0].metadata.exif = ex;
            }
        }
        if (st == RFC_OK && meta.xmp && meta.xmp_len > 0u) {
            uint8_t *xm = (uint8_t *)alloc->alloc(alloc->ctx, meta.xmp_len);
            if (!xm) {
                st = RFC_ERR_OUT_OF_MEMORY;
            } else {
                memcpy(xm, meta.xmp, meta.xmp_len);
                frames[0].metadata.xmp = xm;
            }
        }
    }

    if (st != RFC_OK) {
        if (frames) {
            for (size_t i = 0u; i < frame_count; i++) {
                rfc_frame_free(&frames[i]);
            }
            alloc->free(alloc->ctx, frames);
        }
        rfc_metadata_free(&meta, alloc);
        return st;
    }

    if (out_probe) {
        out_probe->frame_count = 1u;
        out_probe->first_frame = layout;
        out_probe->total_pixels =
            (uint64_t)layout.width * (uint64_t)layout.height;
        out_probe->total_decoded_bytes =
            (uint64_t)layout.height * (uint64_t)layout.stride_bytes;
        out_probe->container_meta = meta;
    } else if (out_meta) {
        *out_meta = meta;
    } else {
        rfc_metadata_free(&meta, alloc);
    }

    if (want_pixels) {
        *out_frames = frames;
        *out_frame_count = frame_count;
    }
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Session adapter: probe, encode, decode, cancel, open, close              */
/* ------------------------------------------------------------------------ */

static rfc_status jxl_box_open_session(const rfc_exec_context *exec,
                                       void **out_session) {
    if (!out_session) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const rfc_allocator *alloc = jxl_alloc_of(exec);
    rfc_jxl_session_box *s =
        (rfc_jxl_session_box *)alloc->alloc(alloc->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&s->cancelled, 0);
    s->alloc_copy = *alloc;
    s->alloc = &s->alloc_copy;
    *out_session = s;
    return RFC_OK;
}

static void jxl_box_close_session(void *session) {
    if (!session) {
        return;
    }
    rfc_jxl_session_box *s = (rfc_jxl_session_box *)session;
    rfc_allocator a = s->alloc_copy;
    if (a.free) {
        a.free(a.ctx, s);
    } else {
        free(s);
    }
}

static void jxl_box_cancel_session(void *session) {
    if (!session) {
        return;
    }
    rfc_jxl_session_box *s = (rfc_jxl_session_box *)session;
    atomic_store_explicit(&s->cancelled, 1, memory_order_release);
}

static rfc_status jxl_probe_session(void *session,
                                    const rfc_exec_context *exec,
                                    const uint8_t *bytes,
                                    size_t size,
                                    rfc_image_probe *out_probe) {
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return jxl_decode_common(session, exec, bytes, size, 0, out_probe, NULL,
                             NULL, NULL);
}

static rfc_status jxl_decode_session(void *session,
                                     const rfc_exec_context *exec,
                                     const uint8_t *bytes,
                                     size_t size,
                                     const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_frame **out_frames,
                                     size_t *out_frame_count,
                                     rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count || !out_meta) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (settings && setting_count > 0u) {
        const char *reason = NULL;
        rfc_status vst = rfc_registry_validate_settings(
            RFC_FORMAT_JPEG_XL, settings, setting_count, &reason);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    return jxl_decode_common(session, exec, bytes, size, 1, NULL, out_frames,
                             out_frame_count, out_meta);
}

static rfc_status jxl_parse_encode_settings(const rfc_setting_value *settings,
                                            size_t setting_count,
                                            rfc_metadata_policy *out_policy) {
    *out_policy = RFC_META_POLICY_PRESERVE;
    if (settings && setting_count > 0u) {
        const char *reason = NULL;
        rfc_status st = rfc_registry_validate_settings(
            RFC_FORMAT_JPEG_XL, settings, setting_count, &reason);
        if (st != RFC_OK) {
            return st;
        }
    }
    for (size_t i = 0u; i < setting_count; i++) {
        if (strcmp(settings[i].id, "metadata") == 0) {
            rfc_status st =
                rfc_metadata_policy_parse(settings[i].value, out_policy);
            if (st != RFC_OK) {
                return st;
            }
        } else if (strcmp(settings[i].id, "exactness") == 0) {
            if (strcmp(settings[i].value, "pixel-exact") != 0 &&
                strcmp(settings[i].value, "byte-identical") != 0) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
        }
    }
    return RFC_OK;
}

static rfc_status jxl_validate_and_pack_input(const rfc_frame *fr,
                                              const rfc_allocator *alloc,
                                              uint8_t **out_owned_buf,
                                              const uint8_t **out_pix,
                                              size_t *out_pix_len) {
    *out_owned_buf = NULL;
    *out_pix = NULL;
    *out_pix_len = 0u;

    uint32_t S = jxl_sample_width_bits(fr->layout.format);
    uint32_t ch = jxl_sample_channels(fr->layout.format);
    uint32_t b = fr->layout.bits_per_sample;
    if (S == 0u || ch == 0u || !fr->data || fr->layout.width == 0u ||
        fr->layout.height == 0u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if ((S == 8u && (b < 1u || b > 8u)) || (S == 16u && (b < 9u || b > 16u))) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if ((ch == 4u && fr->layout.alpha != RFC_ALPHA_STRAIGHT) ||
        (ch != 4u && fr->layout.alpha != RFC_ALPHA_NONE)) {
        return RFC_ERR_UNSUPPORTED_ALPHA;
    }

    size_t elem_bytes = (size_t)(S / 8u);
    size_t row_samples = 0u;
    size_t min_row = 0u;
    size_t packed_len = 0u;
    if (!jxl_mul_sz((size_t)fr->layout.width, (size_t)ch, &row_samples) ||
        !jxl_mul_sz(row_samples, elem_bytes, &min_row) ||
        !jxl_mul_sz(min_row, (size_t)fr->layout.height, &packed_len) ||
        fr->layout.stride_bytes < min_row) {
        return RFC_ERR_OVERFLOW;
    }
    if (fr->data_len <
        (size_t)(fr->layout.height - 1u) * fr->layout.stride_bytes + min_row) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint32_t shift = S - b;
    uint16_t pad_mask = (shift > 0u) ? (uint16_t)((1u << shift) - 1u) : 0u;

    if (pad_mask != 0u) {
        for (uint32_t y = 0u; y < fr->layout.height; y++) {
            const uint8_t *row =
                fr->data + (size_t)y * fr->layout.stride_bytes;
            if (S == 8u) {
                for (size_t i = 0u; i < row_samples; i++) {
                    if ((row[i] & (uint8_t)pad_mask) != 0u) {
                        return RFC_ERR_UNSUPPORTED_SAMPLE;
                    }
                }
            } else {
                for (size_t i = 0u; i < row_samples; i++) {
                    uint16_t v = 0u;
                    memcpy(&v, row + i * 2u, 2u);
                    if ((v & pad_mask) != 0u) {
                        return RFC_ERR_UNSUPPORTED_SAMPLE;
                    }
                }
            }
        }
    }

    if (shift == 0u && fr->layout.stride_bytes == min_row) {
        *out_pix = fr->data;
        *out_pix_len = packed_len;
        return RFC_OK;
    }

    uint8_t *packed = (uint8_t *)alloc->alloc(alloc->ctx, packed_len);
    if (!packed) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    for (uint32_t y = 0u; y < fr->layout.height; y++) {
        const uint8_t *src_row =
            fr->data + (size_t)y * fr->layout.stride_bytes;
        uint8_t *dst_row = packed + (size_t)y * min_row;
        if (shift == 0u) {
            memcpy(dst_row, src_row, min_row);
        } else if (S == 8u) {
            for (size_t i = 0u; i < row_samples; i++) {
                dst_row[i] = (uint8_t)(src_row[i] >> shift);
            }
        } else {
            for (size_t i = 0u; i < row_samples; i++) {
                uint16_t v = 0u;
                memcpy(&v, src_row + i * 2u, 2u);
                v = (uint16_t)(v >> shift);
                memcpy(dst_row + i * 2u, &v, 2u);
            }
        }
    }
    *out_owned_buf = packed;
    *out_pix = packed;
    *out_pix_len = packed_len;
    return RFC_OK;
}

static rfc_status jxl_drain_encoder(JxlEncoder *enc,
                                    const void *session,
                                    const rfc_exec_context *exec,
                                    const rfc_allocator *alloc,
                                    uint8_t **out_buf,
                                    size_t *out_len) {
    size_t cap = 4096u;
    size_t len = 0u;
    uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    for (;;) {
        if (jxl_is_cancelled(session, exec)) {
            alloc->free(alloc->ctx, buf);
            return RFC_ERR_CANCELLED;
        }
        uint8_t *next_out = buf + len;
        size_t avail = cap - len;
        JxlEncoderStatus es = JxlEncoderProcessOutput(enc, &next_out, &avail);
        if (jxl_is_cancelled(session, exec)) {
            alloc->free(alloc->ctx, buf);
            return RFC_ERR_CANCELLED;
        }
        len = (size_t)(next_out - buf);
        if (es == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t next_cap = 0u;
            if (!jxl_mul_sz(cap, 2u, &next_cap)) {
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_OVERFLOW;
            }
            uint8_t *nbuf = (uint8_t *)alloc->alloc(alloc->ctx, next_cap);
            if (!nbuf) {
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_OUT_OF_MEMORY;
            }
            if (len > 0u) {
                memcpy(nbuf, buf, len);
            }
            alloc->free(alloc->ctx, buf);
            buf = nbuf;
            cap = next_cap;
        } else if (es == JXL_ENC_SUCCESS) {
            break;
        } else {
            alloc->free(alloc->ctx, buf);
            return jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                             : RFC_ERR_INVALID_ARGUMENT;
        }
    }
    *out_buf = buf;
    *out_len = len;
    return RFC_OK;
}

static rfc_status jxl_encode_session(void *session,
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
    if (!frames || frame_count == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count != 1u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (jxl_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_metadata_policy meta_policy = RFC_META_POLICY_PRESERVE;
    rfc_status st =
        jxl_parse_encode_settings(settings, setting_count, &meta_policy);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_allocator *alloc = jxl_alloc_of(exec);
    rfc_limits scratch_lim;
    const rfc_limits *limits = jxl_limits_of(exec, &scratch_lim);
    const rfc_frame *fr = &frames[0];

    st = rfc_limits_check_frames(limits, frames, frame_count, meta, 0u, 0);
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

    rfc_orientation bi_ori = RFC_ORIENTATION_NORMAL;
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
                bi_ori = exif_ori;
            } else if (eff_ori != RFC_ORIENTATION_UNSPECIFIED) {
                bi_ori = eff_ori;
            }
        } else if (eff_ori != RFC_ORIENTATION_UNSPECIFIED) {
            bi_ori = eff_ori;
        }
    }

    uint8_t *owned_pix = NULL;
    const uint8_t *pix_buf = NULL;
    size_t pix_len = 0u;
    st = jxl_validate_and_pack_input(fr, alloc, &owned_pix, &pix_buf, &pix_len);
    if (st != RFC_OK) {
        return st;
    }

    JxlMemoryManager mm;
    jxl_init_mm(exec, alloc, &mm);

    jxl_runner_ctx runner_ctx;
    st = jxl_runner_init(&runner_ctx, session, exec, &mm);
    if (st != RFC_OK) {
        if (owned_pix) {
            alloc->free(alloc->ctx, owned_pix);
        }
        return st;
    }

    size_t reserve_bytes = 0u;
    if (meta_policy == RFC_META_POLICY_PRESERVE) {
        reserve_bytes = eff_icc_len + eff_exif_len + eff_xmp_len;
    }
    jxl_budget_guard bg;
    st = jxl_reserve_budget(exec, reserve_bytes, &bg);
    if (st != RFC_OK) {
        jxl_runner_destroy(&runner_ctx);
        if (owned_pix) {
            alloc->free(alloc->ctx, owned_pix);
        }
        return st;
    }

    JxlEncoder *enc = JxlEncoderCreate(&mm);
    if (!enc) {
        jxl_release_budget(&bg);
        jxl_runner_destroy(&runner_ctx);
        if (owned_pix) {
            alloc->free(alloc->ctx, owned_pix);
        }
        return RFC_ERR_OUT_OF_MEMORY;
    }

    uint32_t ch = jxl_sample_channels(fr->layout.format);
    uint32_t S = jxl_sample_width_bits(fr->layout.format);
    uint32_t color_ch = (ch == 1u) ? 1u : 3u;
    int has_alpha = (ch == 4u) ? 1 : 0;

    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = fr->layout.width;
    bi.ysize = fr->layout.height;
    bi.bits_per_sample = fr->layout.bits_per_sample;
    bi.exponent_bits_per_sample = 0u;
    bi.uses_original_profile = JXL_TRUE;
    bi.num_color_channels = color_ch;
    bi.num_extra_channels = has_alpha ? 1u : 0u;
    bi.alpha_bits = has_alpha ? fr->layout.bits_per_sample : 0u;
    bi.alpha_exponent_bits = 0u;
    bi.alpha_premultiplied = JXL_FALSE;
    bi.orientation = (JxlOrientation)bi_ori;

    int add_exif = (meta_policy == RFC_META_POLICY_PRESERVE && eff_exif_len > 0u);
    int add_xmp = (meta_policy == RFC_META_POLICY_PRESERVE && eff_xmp_len > 0u);
    int use_boxes = (add_exif || add_xmp);

    if (JxlEncoderSetParallelRunner(enc, jxl_parallel_runner, &runner_ctx) !=
        JXL_ENC_SUCCESS) {
        st = jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                       : RFC_ERR_INVALID_ARGUMENT;
        goto fail_enc;
    }
    if (use_boxes && JxlEncoderUseBoxes(enc) != JXL_ENC_SUCCESS) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto fail_enc;
    }
    if (JxlEncoderSetBasicInfo(enc, &bi) != JXL_ENC_SUCCESS) {
        st = RFC_ERR_INVALID_ARGUMENT;
        goto fail_enc;
    }
    if (JxlEncoderGetRequiredCodestreamLevel(enc) > 5) {
        if (JxlEncoderSetCodestreamLevel(enc, 10) != JXL_ENC_SUCCESS) {
            st = RFC_ERR_INVALID_ARGUMENT;
            goto fail_enc;
        }
    }

    if (meta_policy == RFC_META_POLICY_PRESERVE && eff_icc_len > 0u) {
        if (color_ch == 1u && eff_icc_len >= 128u) {
            (void)JxlEncoderSetCms(enc, *JxlGetDefaultCms());
        }
        if (JxlEncoderSetICCProfile(enc, eff_icc, eff_icc_len) !=
            JXL_ENC_SUCCESS) {
            st = jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                           : RFC_ERR_INVALID_ARGUMENT;
            goto fail_enc;
        }
    } else {
        JxlColorEncoding ce;
        memset(&ce, 0, sizeof(ce));
        JxlColorEncodingSetToSRGB(&ce, (color_ch == 1u) ? JXL_TRUE : JXL_FALSE);
        if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) {
            st = RFC_ERR_INVALID_ARGUMENT;
            goto fail_enc;
        }
    }

    if (add_exif) {
        size_t box_len = 0u;
        if (!jxl_add_sz(4u, eff_exif_len, &box_len)) {
            st = RFC_ERR_OVERFLOW;
            goto fail_enc;
        }
        uint8_t *exif_box = (uint8_t *)alloc->alloc(alloc->ctx, box_len);
        if (!exif_box) {
            st = RFC_ERR_OUT_OF_MEMORY;
            goto fail_enc;
        }
        jxl_wr32be(exif_box, 0u);
        memcpy(exif_box + 4u, eff_exif, eff_exif_len);
        JxlBoxType btype = {'E', 'x', 'i', 'f'};
        JxlEncoderStatus bs =
            JxlEncoderAddBox(enc, btype, exif_box, box_len, JXL_FALSE);
        alloc->free(alloc->ctx, exif_box);
        if (bs != JXL_ENC_SUCCESS) {
            st = RFC_ERR_OUT_OF_MEMORY;
            goto fail_enc;
        }
    }
    if (add_xmp) {
        JxlBoxType btype = {'x', 'm', 'l', ' '};
        if (JxlEncoderAddBox(enc, btype, eff_xmp, eff_xmp_len, JXL_FALSE) !=
            JXL_ENC_SUCCESS) {
            st = RFC_ERR_OUT_OF_MEMORY;
            goto fail_enc;
        }
    }
    if (use_boxes) {
        JxlEncoderCloseBoxes(enc);
    }

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto fail_enc;
    }
    JxlBitDepth bd;
    bd.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
    bd.bits_per_sample = 0u;
    bd.exponent_bits_per_sample = 0u;
    JxlPixelFormat px;
    px.num_channels = ch;
    px.data_type = (S == 16u) ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8;
    px.endianness = JXL_NATIVE_ENDIAN;
    px.align = 0u;

    if (JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS ||
        JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_MODULAR,
                                         1) != JXL_ENC_SUCCESS ||
        JxlEncoderFrameSettingsSetOption(
            fs, JXL_ENC_FRAME_SETTING_KEEP_INVISIBLE, 1) != JXL_ENC_SUCCESS ||
        JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_STRIP_ALPHA,
                                         0) != JXL_ENC_SUCCESS ||
        JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, 7) !=
            JXL_ENC_SUCCESS ||
        JxlEncoderSetFrameBitDepth(fs, &bd) != JXL_ENC_SUCCESS ||
        JxlEncoderAddImageFrame(fs, &px, pix_buf, pix_len) != JXL_ENC_SUCCESS) {
        st = jxl_is_cancelled(session, exec)
                 ? RFC_ERR_CANCELLED
                 : (jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                              : RFC_ERR_INVALID_ARGUMENT);
        goto fail_enc;
    }
    if (owned_pix) {
        alloc->free(alloc->ctx, owned_pix);
        owned_pix = NULL;
    }
    JxlEncoderCloseFrames(enc);

    uint8_t *out_bytes = NULL;
    size_t out_len = 0u;
    st = jxl_drain_encoder(enc, session, exec, alloc, &out_bytes, &out_len);
    if (st != RFC_OK) {
        goto fail_enc;
    }

    JxlEncoderDestroy(enc);
    jxl_release_budget(&bg);
    jxl_runner_destroy(&runner_ctx);

    out_encoded->data = out_bytes;
    out_encoded->len = out_len;
    out_encoded->allocator = alloc;
    return RFC_OK;

fail_enc:
    JxlEncoderDestroy(enc);
    jxl_release_budget(&bg);
    jxl_runner_destroy(&runner_ctx);
    if (owned_pix) {
        alloc->free(alloc->ctx, owned_pix);
    }
    return st;
}

static void jxl_free_encoded(void *session, rfc_encoded *encoded) {
    (void)session;
    rfc_encoded_free(encoded);
}

/* ------------------------------------------------------------------------ */
/* Zero-alloc JPEG marker scanner & JPEG <-> JPEG XL lossless adapters      */
/* ------------------------------------------------------------------------ */

typedef struct jxl_icc_chunk_ref {
    size_t off;
    size_t len;
} jxl_icc_chunk_ref;

typedef struct jxl_jpeg_scan_info {
    uint32_t width;
    uint32_t height;
    uint32_t components;
    uint32_t bits_per_sample;
    rfc_orientation orientation;
    int has_exif;
    size_t exif_off;
    size_t exif_len;
    int has_xmp;
    size_t xmp_off;
    size_t xmp_len;
    uint8_t icc_total_chunks;
    int icc_invalid;
    jxl_icc_chunk_ref icc_chunks[256];
} jxl_jpeg_scan_info;

static rfc_status jxl_scan_jpeg_markers(const uint8_t *bytes,
                                        size_t size,
                                        size_t meta_cap,
                                        jxl_jpeg_scan_info *out) {
    memset(out, 0, sizeof(*out));
    if (!bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < 2u) {
        return (size == 0u || bytes[0] == 0xFFu) ? RFC_ERR_TRUNCATED_INPUT
                                                 : RFC_ERR_INVALID_ARGUMENT;
    }
    if (bytes[0] != 0xFFu || bytes[1] != 0xD8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    size_t pos = 2u;
    int seen_sof = 0;
    int seen_eoi = 0;
    while (pos < size) {
        if (bytes[pos] != 0xFFu) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        while (pos < size && bytes[pos] == 0xFFu) {
            pos++;
        }
        if (pos >= size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint8_t marker = bytes[pos++];
        if (marker == 0x00u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (marker == 0xD9u) {
            seen_eoi = 1;
            break;
        }
        if (marker == 0x01u || (marker >= 0xD0u && marker <= 0xD7u)) {
            continue;
        }
        if (size - pos < 2u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint16_t seg_len = jxl_rd16be(bytes + pos);
        if (seg_len < 2u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if ((size_t)seg_len > size - pos) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        const uint8_t *payload = bytes + pos + 2u;
        size_t plen = (size_t)seg_len - 2u;
        size_t payload_off = pos + 2u;
        pos += (size_t)seg_len;

        if (marker == 0xC0u || marker == 0xC1u || marker == 0xC2u) {
            if (plen < 6u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            uint8_t prec = payload[0];
            uint16_t h = jxl_rd16be(payload + 1u);
            uint16_t w = jxl_rd16be(payload + 3u);
            uint8_t ncomp = payload[5];
            if (plen != 6u + (size_t)ncomp * 3u || w == 0u || h == 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (prec != 8u || (ncomp != 1u && ncomp != 3u)) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
            if (!seen_sof) {
                out->width = w;
                out->height = h;
                out->components = ncomp;
                out->bits_per_sample = prec;
                seen_sof = 1;
            }
        } else if ((marker >= 0xC3u && marker <= 0xCFu) && marker != 0xC4u &&
                   marker != 0xC8u && marker != 0xCCu) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        } else if (marker == 0xE1u) {
            if (!out->has_exif && plen >= 6u &&
                memcmp(payload, "Exif\x00\x00", 6) == 0) {
                const uint8_t *tiff = payload + 6u;
                size_t tiff_len = plen - 6u;
                rfc_orientation ori = RFC_ORIENTATION_UNSPECIFIED;
                if (tiff_len > meta_cap) {
                    return RFC_ERR_LIMIT_EXCEEDED;
                }
                if (rfc_exif_parse_orientation(tiff, tiff_len, &ori) !=
                    RFC_OK) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                out->has_exif = 1;
                out->exif_off = payload_off + 6u;
                out->exif_len = tiff_len;
                out->orientation = (ori != RFC_ORIENTATION_UNSPECIFIED)
                                       ? ori
                                       : RFC_ORIENTATION_NORMAL;
            } else if (!out->has_xmp && plen >= 29u &&
                       memcmp(payload, "http://ns.adobe.com/xap/1.0/\x00",
                              29) == 0) {
                size_t xlen = plen - 29u;
                if (xlen > meta_cap) {
                    return RFC_ERR_LIMIT_EXCEEDED;
                }
                if (xlen > 0u) {
                    out->has_xmp = 1;
                    out->xmp_off = payload_off + 29u;
                    out->xmp_len = xlen;
                }
            }
        } else if (marker == 0xE2u) {
            if (plen >= 14u && memcmp(payload, "ICC_PROFILE\x00", 12) == 0) {
                uint8_t seq = payload[12];
                uint8_t tot = payload[13];
                if (seq == 0u || tot == 0u || seq > tot ||
                    (out->icc_total_chunks != 0u &&
                     out->icc_total_chunks != tot)) {
                    out->icc_invalid = 1;
                } else {
                    out->icc_total_chunks = tot;
                    if (out->icc_chunks[seq].len == 0u) {
                        out->icc_chunks[seq].off = payload_off + 14u;
                        out->icc_chunks[seq].len = plen - 14u;
                    } else {
                        out->icc_invalid = 1;
                    }
                }
            }
        } else if (marker == 0xDAu) {
            while (pos < size) {
                if (bytes[pos] != 0xFFu) {
                    pos++;
                    continue;
                }
                pos++;
                if (pos >= size) {
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                uint8_t b = bytes[pos];
                if (b == 0x00u || (b >= 0xD0u && b <= 0xD7u)) {
                    pos++;
                    continue;
                }
                if (b == 0xFFu) {
                    continue;
                }
                pos--;
                break;
            }
        }
    }

    if (!seen_eoi) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (!seen_sof) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

static rfc_status jxl_materialize_jpeg_meta(const jxl_jpeg_scan_info *info,
                                            const uint8_t *bytes,
                                            const rfc_allocator *alloc,
                                            size_t meta_cap,
                                            rfc_metadata *out_meta) {
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = info->has_exif ? info->orientation
                                           : RFC_ORIENTATION_NORMAL;

    if (info->icc_total_chunks > 0u && !info->icc_invalid) {
        size_t icc_sum = 0u;
        int complete = 1;
        for (uint8_t i = 1u; i <= info->icc_total_chunks; i++) {
            if (info->icc_chunks[i].len == 0u) {
                complete = 0;
                break;
            }
            if (!jxl_add_sz(icc_sum, info->icc_chunks[i].len, &icc_sum)) {
                return RFC_ERR_OVERFLOW;
            }
        }
        if (complete && icc_sum > 0u) {
            if (icc_sum > meta_cap) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            uint8_t *icc = (uint8_t *)alloc->alloc(alloc->ctx, icc_sum);
            if (!icc) {
                return RFC_ERR_OUT_OF_MEMORY;
            }
            size_t wp = 0u;
            for (uint8_t i = 1u; i <= info->icc_total_chunks; i++) {
                memcpy(icc + wp, bytes + info->icc_chunks[i].off,
                       info->icc_chunks[i].len);
                wp += info->icc_chunks[i].len;
            }
            out_meta->icc = icc;
            out_meta->icc_len = icc_sum;
        }
    }

    if (info->has_exif && info->exif_len > 0u) {
        if (info->exif_len > meta_cap) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        uint8_t *ex = (uint8_t *)alloc->alloc(alloc->ctx, info->exif_len);
        if (!ex) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(ex, bytes + info->exif_off, info->exif_len);
        out_meta->exif = ex;
        out_meta->exif_len = info->exif_len;
    }

    if (info->has_xmp && info->xmp_len > 0u) {
        if (info->xmp_len > meta_cap) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        uint8_t *xm = (uint8_t *)alloc->alloc(alloc->ctx, info->xmp_len);
        if (!xm) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(xm, bytes + info->xmp_off, info->xmp_len);
        out_meta->xmp = xm;
        out_meta->xmp_len = info->xmp_len;
    }
    return RFC_OK;
}

static rfc_status jxl_jpeg_probe(void *session,
                                 const rfc_exec_context *exec,
                                 const uint8_t *jpeg_bytes,
                                 size_t jpeg_size,
                                 rfc_image_probe *out_probe) {
    if (!out_probe || !jpeg_bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (jxl_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits scratch_lim;
    const rfc_limits *limits = jxl_limits_of(exec, &scratch_lim);
    const rfc_allocator *alloc = jxl_alloc_of(exec);
    size_t meta_cap = jxl_meta_cap_of(limits);

    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)jpeg_size);
    if (st != RFC_OK) {
        return st;
    }

    jxl_jpeg_scan_info sinfo;
    st = jxl_scan_jpeg_markers(jpeg_bytes, jpeg_size, meta_cap, &sinfo);
    if (st != RFC_OK) {
        return st;
    }

    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format =
        (sinfo.components == 1u) ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_RGB8;
    lay.alpha = RFC_ALPHA_NONE;
    lay.width = sinfo.width;
    lay.height = sinfo.height;
    lay.bits_per_sample = 8u;
    size_t rb = 0u;
    if (!jxl_mul_sz((size_t)lay.width, (size_t)sinfo.components, &rb)) {
        return RFC_ERR_OVERFLOW;
    }
    lay.stride_bytes = rb;

    out_probe->frame_count = 1u;
    out_probe->first_frame = lay;
    out_probe->total_pixels = (uint64_t)lay.width * (uint64_t)lay.height;
    out_probe->total_decoded_bytes =
        (uint64_t)lay.height * (uint64_t)lay.stride_bytes;

    st = rfc_limits_check_probe(limits, out_probe, (uint64_t)jpeg_size,
                                jpeg_size > 0u);
    if (st != RFC_OK) {
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }

    st = jxl_materialize_jpeg_meta(&sinfo, jpeg_bytes, alloc, meta_cap,
                                   &out_probe->container_meta);
    if (st != RFC_OK) {
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }
    st = rfc_limits_check_metadata(limits, &out_probe->container_meta);
    if (st != RFC_OK) {
        rfc_metadata_free(&out_probe->container_meta, alloc);
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }
    return RFC_OK;
}

static rfc_status jxl_jpeg_transcode(void *session,
                                     const rfc_exec_context *exec,
                                     const uint8_t *jpeg_bytes,
                                     size_t jpeg_size,
                                     const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_encoded *out_jxl,
                                     rfc_metadata *out_src_meta) {
    if (!out_jxl || !out_src_meta || !jpeg_bytes || jpeg_size == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_jxl, 0, sizeof(*out_jxl));
    memset(out_src_meta, 0, sizeof(*out_src_meta));
    if (jxl_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    if (settings && setting_count > 0u) {
        const char *reason = NULL;
        rfc_status vst = rfc_registry_validate_settings(
            RFC_FORMAT_JPEG_XL, settings, setting_count, &reason);
        if (vst != RFC_OK) {
            return vst;
        }
        for (size_t i = 0u; i < setting_count; i++) {
            if (strcmp(settings[i].id, "metadata") == 0 &&
                strcmp(settings[i].value, "strip") == 0) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
            if (strcmp(settings[i].id, "exactness") == 0 &&
                strcmp(settings[i].value, "original-byte-reconstruction") != 0) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
        }
    }

    const rfc_allocator *alloc = jxl_alloc_of(exec);
    rfc_limits scratch_lim;
    const rfc_limits *limits = jxl_limits_of(exec, &scratch_lim);
    size_t meta_cap = jxl_meta_cap_of(limits);

    rfc_image_probe pr;
    rfc_status st = jxl_jpeg_probe(session, exec, jpeg_bytes, jpeg_size, &pr);
    if (st != RFC_OK) {
        return st;
    }
    *out_src_meta = pr.container_meta;
    memset(&pr.container_meta, 0, sizeof(pr.container_meta));

    JxlMemoryManager mm;
    jxl_init_mm(exec, alloc, &mm);

    jxl_runner_ctx runner_ctx;
    st = jxl_runner_init(&runner_ctx, session, exec, &mm);
    if (st != RFC_OK) {
        rfc_metadata_free(out_src_meta, alloc);
        return st;
    }

    jxl_budget_guard bg;
    size_t res_sz = jpeg_size;
    if (out_src_meta->icc_len + out_src_meta->exif_len +
            out_src_meta->xmp_len <=
        meta_cap) {
        size_t msum = out_src_meta->icc_len + out_src_meta->exif_len +
                      out_src_meta->xmp_len;
        (void)jxl_add_sz(res_sz, msum, &res_sz);
    }
    st = jxl_reserve_budget(exec, res_sz, &bg);
    if (st != RFC_OK) {
        jxl_runner_destroy(&runner_ctx);
        rfc_metadata_free(out_src_meta, alloc);
        return st;
    }

    JxlEncoder *enc = JxlEncoderCreate(&mm);
    if (!enc) {
        jxl_release_budget(&bg);
        jxl_runner_destroy(&runner_ctx);
        rfc_metadata_free(out_src_meta, alloc);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    if (JxlEncoderSetParallelRunner(enc, jxl_parallel_runner, &runner_ctx) !=
            JXL_ENC_SUCCESS ||
        JxlEncoderUseContainer(enc, JXL_TRUE) != JXL_ENC_SUCCESS ||
        JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE) != JXL_ENC_SUCCESS) {
        st = jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                       : RFC_ERR_INVALID_ARGUMENT;
        goto fail_trans;
    }

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto fail_trans;
    }
    if (JxlEncoderAddJPEGFrame(fs, jpeg_bytes, jpeg_size) != JXL_ENC_SUCCESS) {
        st = jxl_is_cancelled(session, exec)
                 ? RFC_ERR_CANCELLED
                 : (jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                              : RFC_ERR_UNSUPPORTED_COMBINATION);
        goto fail_trans;
    }
    JxlEncoderCloseInput(enc);

    uint8_t *jxl_buf = NULL;
    size_t jxl_len = 0u;
    st = jxl_drain_encoder(enc, session, exec, alloc, &jxl_buf, &jxl_len);
    if (st != RFC_OK) {
        goto fail_trans;
    }

    JxlEncoderDestroy(enc);
    jxl_release_budget(&bg);
    jxl_runner_destroy(&runner_ctx);

    out_jxl->data = jxl_buf;
    out_jxl->len = jxl_len;
    out_jxl->allocator = alloc;
    return RFC_OK;

fail_trans:
    JxlEncoderDestroy(enc);
    jxl_release_budget(&bg);
    jxl_runner_destroy(&runner_ctx);
    rfc_metadata_free(out_src_meta, alloc);
    return st;
}

static rfc_status jxl_jpeg_reconstruct(void *session,
                                       const rfc_exec_context *exec,
                                       const uint8_t *jxl_bytes,
                                       size_t jxl_size,
                                       rfc_encoded *out_jpeg,
                                       rfc_metadata *out_dst_meta) {
    if (!out_jpeg || !out_dst_meta || !jxl_bytes || jxl_size == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_jpeg, 0, sizeof(*out_jpeg));
    memset(out_dst_meta, 0, sizeof(*out_dst_meta));
    if (jxl_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    const rfc_allocator *alloc = jxl_alloc_of(exec);
    rfc_limits scratch_lim;
    const rfc_limits *limits = jxl_limits_of(exec, &scratch_lim);
    size_t meta_cap = jxl_meta_cap_of(limits);

    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)jxl_size);
    if (st != RFC_OK) {
        return st;
    }
    st = jxl_preflight_stream(jxl_bytes, jxl_size);
    if (st != RFC_OK) {
        return st;
    }

    uint64_t max_rec_u64 = limits->max_compressed_bytes;
    if (limits->max_decoded_bytes > 0u &&
        (max_rec_u64 == 0u || limits->max_decoded_bytes < max_rec_u64)) {
        max_rec_u64 = limits->max_decoded_bytes;
    }
    if (max_rec_u64 == 0u || max_rec_u64 > SIZE_MAX) {
        max_rec_u64 = SIZE_MAX;
    }
    size_t max_rec = (size_t)max_rec_u64;

    JxlMemoryManager mm;
    jxl_init_mm(exec, alloc, &mm);

    jxl_runner_ctx runner_ctx;
    st = jxl_runner_init(&runner_ctx, session, exec, &mm);
    if (st != RFC_OK) {
        return st;
    }

    jxl_budget_guard bg;
    size_t reserve_est = (jxl_size < 65536u) ? jxl_size : 65536u;
    st = jxl_reserve_budget(exec, reserve_est, &bg);
    if (st != RFC_OK) {
        jxl_runner_destroy(&runner_ctx);
        return st;
    }

    JxlDecoder *dec = JxlDecoderCreate(&mm);
    if (!dec) {
        jxl_release_budget(&bg);
        jxl_runner_destroy(&runner_ctx);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    size_t jpeg_cap = (jxl_size > 4096u) ? jxl_size : 4096u;
    if (jpeg_cap > max_rec) {
        jpeg_cap = max_rec ? max_rec : 1u;
    }
    size_t jpeg_len = 0u;
    uint8_t *jpeg_buf = (uint8_t *)alloc->alloc(alloc->ctx, jpeg_cap);
    if (!jpeg_buf) {
        JxlDecoderDestroy(dec);
        jxl_release_budget(&bg);
        jxl_runner_destroy(&runner_ctx);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    jxl_box_capture box_cap;
    memset(&box_cap, 0, sizeof(box_cap));
    int seen_exif_box = 0;
    int seen_xmp_box = 0;
    int jpeg_buf_active = 0;
    int saw_jrec = 0;

    if (JxlDecoderSetParallelRunner(dec, jxl_parallel_runner, &runner_ctx) !=
            JXL_DEC_SUCCESS ||
        JxlDecoderSetDecompressBoxes(dec, JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(
            dec, JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_BASIC_INFO |
                     JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE | JXL_DEC_BOX |
                     JXL_DEC_BOX_COMPLETE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, jxl_bytes, jxl_size) != JXL_DEC_SUCCESS) {
        st = jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                       : RFC_ERR_INVALID_ARGUMENT;
        goto fail_rec;
    }
    JxlDecoderCloseInput(dec);

    for (;;) {
        if (jxl_is_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        JxlDecoderStatus dstat = JxlDecoderProcessInput(dec);
        if (jxl_is_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        if (dstat == JXL_DEC_SUCCESS) {
            st = jxl_box_discard_checked(dec, &box_cap, alloc, meta_cap);
            if (st != RFC_OK) {
                break;
            }
            if (jpeg_buf_active) {
                size_t rem = JxlDecoderReleaseJPEGBuffer(dec);
                jpeg_buf_active = 0;
                if (rem <= jpeg_cap) {
                    jpeg_len = jpeg_cap - rem;
                }
            }
            break;
        } else if (dstat == JXL_DEC_ERROR) {
            st = jxl_budget_rejected(exec) ? RFC_ERR_OUT_OF_MEMORY
                                           : RFC_ERR_INVALID_ARGUMENT;
            break;
        } else if (dstat == JXL_DEC_NEED_MORE_INPUT) {
            st = RFC_ERR_TRUNCATED_INPUT;
            break;
        } else if (dstat == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo bi;
            memset(&bi, 0, sizeof(bi));
            if (JxlDecoderGetBasicInfo(dec, &bi) != JXL_DEC_SUCCESS) {
                st = RFC_ERR_INVALID_ARGUMENT;
                break;
            }
            if (bi.have_animation) {
                st = RFC_ERR_UNSUPPORTED_COMBINATION;
                break;
            }
        } else if (dstat == JXL_DEC_BOX) {
            st = jxl_box_discard_checked(dec, &box_cap, alloc, meta_cap);
            if (st != RFC_OK) {
                break;
            }
            st = jxl_box_begin(dec, &box_cap, alloc, meta_cap, seen_exif_box,
                               seen_xmp_box);
            if (st != RFC_OK) {
                break;
            }
            if (box_cap.active) {
                if (box_cap.is_exif) {
                    seen_exif_box = 1;
                }
                if (box_cap.is_xmp) {
                    seen_xmp_box = 1;
                }
            }
        } else if (dstat == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
            st = jxl_box_grow(dec, &box_cap, alloc, meta_cap);
            if (st != RFC_OK) {
                break;
            }
        } else if (dstat == JXL_DEC_BOX_COMPLETE) {
            st = jxl_box_discard_checked(dec, &box_cap, alloc, meta_cap);
            if (st != RFC_OK) {
                break;
            }
        } else if (dstat == JXL_DEC_JPEG_RECONSTRUCTION) {
            saw_jrec = 1;
            if (!jpeg_buf_active) {
                if (JxlDecoderSetJPEGBuffer(dec, jpeg_buf, jpeg_cap) !=
                    JXL_DEC_SUCCESS) {
                    st = RFC_ERR_INVALID_ARGUMENT;
                    break;
                }
                jpeg_buf_active = 1;
            }
        } else if (dstat == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
            size_t rem = JxlDecoderReleaseJPEGBuffer(dec);
            jpeg_buf_active = 0;
            jpeg_len = (rem <= jpeg_cap) ? (jpeg_cap - rem) : jpeg_cap;
            if (jpeg_len >= max_rec || jpeg_cap >= max_rec) {
                st = RFC_ERR_LIMIT_EXCEEDED;
                break;
            }
            size_t next_cap =
                (jpeg_cap <= SIZE_MAX / 2u) ? (jpeg_cap * 2u) : max_rec;
            if (next_cap > max_rec) {
                next_cap = max_rec;
            }
            if (next_cap <= jpeg_len) {
                st = RFC_ERR_LIMIT_EXCEEDED;
                break;
            }
            uint8_t *nbuf = (uint8_t *)alloc->alloc(alloc->ctx, next_cap);
            if (!nbuf) {
                st = RFC_ERR_OUT_OF_MEMORY;
                break;
            }
            if (jpeg_len > 0u) {
                memcpy(nbuf, jpeg_buf, jpeg_len);
            }
            alloc->free(alloc->ctx, jpeg_buf);
            jpeg_buf = nbuf;
            jpeg_cap = next_cap;
            if (JxlDecoderSetJPEGBuffer(dec, jpeg_buf + jpeg_len,
                                        jpeg_cap - jpeg_len) !=
                JXL_DEC_SUCCESS) {
                st = RFC_ERR_INVALID_ARGUMENT;
                break;
            }
            jpeg_buf_active = 1;
        } else if (dstat == JXL_DEC_FRAME) {
            if (!saw_jrec) {
                st = RFC_ERR_UNSUPPORTED_COMBINATION;
                break;
            }
        } else if (dstat == JXL_DEC_FULL_IMAGE) {
            if (jpeg_buf_active) {
                size_t rem = JxlDecoderReleaseJPEGBuffer(dec);
                jpeg_buf_active = 0;
                if (rem <= jpeg_cap) {
                    jpeg_len = jpeg_cap - rem;
                }
            }
        } else {
            st = RFC_ERR_INVALID_ARGUMENT;
            break;
        }
    }

    if (st == RFC_OK && (!saw_jrec || jpeg_len == 0u)) {
        st = RFC_ERR_UNSUPPORTED_COMBINATION;
    }

    if (st == RFC_OK) {
        jxl_jpeg_scan_info sinfo;
        st = jxl_scan_jpeg_markers(jpeg_buf, jpeg_len, meta_cap, &sinfo);
        if (st == RFC_OK) {
            st = jxl_materialize_jpeg_meta(&sinfo, jpeg_buf, alloc, meta_cap,
                                           out_dst_meta);
        }
    }

    if (st == RFC_OK) {
        if (box_cap.active) {
            (void)JxlDecoderReleaseBoxBuffer(dec);
        }
        jxl_box_capture_reset(&box_cap, alloc);
        JxlDecoderDestroy(dec);
        jxl_release_budget(&bg);
        jxl_runner_destroy(&runner_ctx);
        out_jpeg->data = jpeg_buf;
        out_jpeg->len = jpeg_len;
        out_jpeg->allocator = alloc;
        return RFC_OK;
    }

fail_rec:
    if (box_cap.active) {
        (void)JxlDecoderReleaseBoxBuffer(dec);
    }
    jxl_box_capture_reset(&box_cap, alloc);
    if (jpeg_buf_active) {
        (void)JxlDecoderReleaseJPEGBuffer(dec);
    }
    alloc->free(alloc->ctx, jpeg_buf);
    JxlDecoderDestroy(dec);
    jxl_release_budget(&bg);
    jxl_runner_destroy(&runner_ctx);
    rfc_metadata_free(out_dst_meta, alloc);
    return st;
}

/* ------------------------------------------------------------------------ */
/* Public direct entry points & adapter registration                        */
/* ------------------------------------------------------------------------ */

rfc_status rfc_jxl_transcode_jpeg(const rfc_exec_context *exec,
                                  const uint8_t *jpeg_bytes,
                                  size_t jpeg_size,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_encoded *out_jxl,
                                  rfc_metadata *out_src_meta) {
    return jxl_jpeg_transcode(NULL, exec, jpeg_bytes, jpeg_size, settings,
                              setting_count, out_jxl, out_src_meta);
}

rfc_status rfc_jxl_reconstruct_jpeg(const rfc_exec_context *exec,
                                    const uint8_t *jxl_bytes,
                                    size_t jxl_size,
                                    rfc_encoded *out_jpeg,
                                    rfc_metadata *out_dst_meta) {
    return jxl_jpeg_reconstruct(NULL, exec, jxl_bytes, jxl_size, out_jpeg,
                                out_dst_meta);
}

static const rfc_session_adapter k_jxl_session_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_JPEG_XL,
    jxl_box_open_session,
    jxl_box_close_session,
    jxl_probe_session,
    jxl_encode_session,
    jxl_decode_session,
    jxl_box_cancel_session,
    jxl_free_encoded
};

static const rfc_jpeg_reconstruct_adapter k_jxl_jrec_adapter = {
    RFC_CODEC_ABI_VERSION,
    jxl_box_open_session,
    jxl_box_close_session,
    jxl_box_cancel_session,
    jxl_jpeg_probe,
    jxl_jpeg_transcode,
    jxl_jpeg_reconstruct
};

rfc_status rfc_codecs_register_jxl(void) {
    rfc_status st = rfc_registry_register_session(&k_jxl_session_adapter);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_registry_register_jpeg_reconstruct(&k_jxl_jrec_adapter);
    if (st != RFC_OK) {
        (void)rfc_registry_unregister_session(RFC_FORMAT_JPEG_XL);
        return st;
    }
    return RFC_OK;
}

rfc_status rfc_codecs_unregister_jxl(void) {
    rfc_status st_jrec = rfc_registry_unregister_jpeg_reconstruct();
    if (st_jrec != RFC_OK) {
        return st_jrec;
    }
    rfc_status st_sess = rfc_registry_unregister_session(RFC_FORMAT_JPEG_XL);
    if (st_sess != RFC_OK) {
        (void)rfc_registry_register_jpeg_reconstruct(&k_jxl_jrec_adapter);
        return st_sess;
    }
    return RFC_OK;
}
