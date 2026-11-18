/* Shipped C11 codec adapters for D10 T02 s5 (bundled libjpeg-turbo 3.2.1):
 *   - RFC_FORMAT_JPEG session adapter: zero-allocation marker preflight
 *     (jpeg_scan.c), pixel import to GRAY8/RGB8 under pinned decoder settings
 *     (JDCT_ISLOW, fancy upsampling, no block smoothing), ICC/EXIF/XMP and
 *     EXIF orientation, and refusal of every pixel -> JPEG encode (every
 *     exactness class is lossless; baseline JPEG from pixels is lossy).
 *   - rfc_jpeg_transcode_adapter: coefficient-exact entropy optimization
 *     (jpeg_read_coefficients -> optimized jpeg_write_coefficients, Huffman
 *     selectors and coding mode kept), coefficient extraction for the
 *     engine's coefficient-domain verifier, and the preserve/strip marker
 *     contract. Every provider call runs under a per-call longjmp error
 *     manager that turns warnings into failures. */

#include "jpeg_scan.h"
#include "rfc_codecs.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <limits.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jerror.h>
#include <jpeglib.h>

#define JPG_MAX_SCANS 1000u
#define JPG_OUTPUT_SLACK (64u * 1024u)

/* Test seam: output buffer = source size + slack. Negative values shrink it
 * to exercise the destination-full fallback; -1 restores the default. */
static _Atomic long long g_jpg_output_slack = (long long)JPG_OUTPUT_SLACK;

void rfc_codecs_jpeg_set_output_slack_for_testing(long long slack) {
    atomic_store(&g_jpg_output_slack,
                 slack == -1 ? (long long)JPG_OUTPUT_SLACK : slack);
}
#define JPG_FIXED_OVERHEAD (1024u * 1024u)

/* ------------------------------------------------------------------------ */
/* Allocator, limits, cancellation, budget, arithmetic                       */
/* ------------------------------------------------------------------------ */

static void *jpg_stdlib_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void jpg_stdlib_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const rfc_allocator k_jpg_stdlib_alloc = {jpg_stdlib_alloc,
                                                 jpg_stdlib_free, NULL};

static const rfc_allocator *jpg_alloc_of(const rfc_exec_context *exec) {
    const rfc_allocator *a = rfc_exec_allocator(exec);
    return (a && a->alloc && a->free) ? a : &k_jpg_stdlib_alloc;
}

static const rfc_limits *jpg_limits_of(const rfc_exec_context *exec,
                                       rfc_limits *scratch) {
    const rfc_limits *l = rfc_exec_limits(exec);
    if (l) {
        return l;
    }
    rfc_limits_default(scratch);
    return scratch;
}

static size_t jpg_meta_cap_of(const rfc_limits *limits) {
    uint64_t cap = RFC_MAX_ICC_BYTES;
    if (limits && limits->max_metadata_bytes > 0u &&
        limits->max_metadata_bytes < cap) {
        cap = limits->max_metadata_bytes;
    }
    return (size_t)cap;
}

typedef struct jpg_session {
    atomic_int cancelled;
} jpg_session;

static int jpg_is_cancelled(const void *session, const rfc_exec_context *exec) {
    const jpg_session *s = (const jpg_session *)session;
    if (s && atomic_load_explicit(&s->cancelled, memory_order_acquire) != 0) {
        return 1;
    }
    return exec ? rfc_exec_is_cancelled(exec) : 0;
}

typedef struct jpg_budget_guard {
    rfc_budget *budget;
    size_t bytes;
    int active;
} jpg_budget_guard;

static rfc_status jpg_reserve(const rfc_exec_context *exec, size_t bytes,
                              jpg_budget_guard *g) {
    memset(g, 0, sizeof(*g));
    if (!exec || !exec->budget) {
        return RFC_OK;
    }
    rfc_status st = rfc_budget_reserve(exec->budget, bytes);
    if (st != RFC_OK) {
        return st;
    }
    g->budget = exec->budget;
    g->bytes = bytes;
    g->active = 1;
    return RFC_OK;
}

static void jpg_release(jpg_budget_guard *g) {
    if (g && g->active && g->budget) {
        rfc_budget_release(g->budget, g->bytes);
    }
    if (g) {
        memset(g, 0, sizeof(*g));
    }
}

static int jpg_mul(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0u && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static int jpg_add(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Preflight: structure policy, layout, limits, provider working set        */
/* ------------------------------------------------------------------------ */

typedef struct jpg_preflight {
    rfc_jpeg_scan_info info;
    rfc_image_probe probe;  /* metadata lengths only; pointers stay NULL */
    uint32_t hmax;
    uint32_t vmax;
    uint64_t coef_bytes;    /* whole-image JBLOCK arrays, MCU padded */
    uint64_t row_bytes;     /* provider row and context buffers */
} jpg_preflight;

static rfc_status jpg_preflight_run(const uint8_t *bytes, size_t size,
                                    const rfc_limits *limits,
                                    jpg_preflight *out) {
    memset(out, 0, sizeof(*out));
    if (!bytes || size == 0u) {
        return size == 0u ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }
    if ((uint64_t)size > (uint64_t)ULONG_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED; /* jpeg_mem_src takes unsigned long */
    }
    rfc_jpeg_scan_info *si = &out->info;
    st = rfc_jpeg_scan(bytes, size, jpg_meta_cap_of(limits),
                       RFC_JPEG_SCAN_ALLOW_ARITHMETIC, si);
    if (st != RFC_OK) {
        return st;
    }
    if (si->sof_count != 1u || si->sos_count == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (si->sos_count > JPG_MAX_SCANS) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (si->sos_malformed > 0u || si->quant_missing) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    /* Metadata that cannot be carried exactly is refused, never dropped. */
    if (si->exif_count > 1u || si->xmp_count > 1u || si->xmp_ext_count > 0u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (si->icc_total_chunks > 0u) {
        if (si->icc_invalid) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
        for (uint32_t i = 1u; i <= (uint32_t)si->icc_total_chunks; i++) {
            if (!si->icc_seen[i] || si->icc_chunks[i].len == 0u) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
        }
    } else if (si->icc_invalid) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    /* One still image per file: MPF pictures and data after EOI. */
    if (si->mpf_count > 0u || si->trailing_nonpad > 0u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (si->components == 3u && si->header_adobe && !si->header_jfif &&
        si->header_adobe_transform != 0 && si->header_adobe_transform != 1) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    uint32_t hmax = 1u, vmax = 1u;
    for (uint32_t c = 0u; c < si->components; c++) {
        if (si->h_samp[c] < 1u || si->h_samp[c] > 4u || si->v_samp[c] < 1u ||
            si->v_samp[c] > 4u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (si->h_samp[c] > hmax) hmax = si->h_samp[c];
        if (si->v_samp[c] > vmax) vmax = si->v_samp[c];
    }
    for (uint32_t c = 0u; c < si->components; c++) {
        if (hmax % si->h_samp[c] != 0u || vmax % si->v_samp[c] != 0u) {
            return RFC_ERR_UNSUPPORTED_COMBINATION; /* fractional sampling */
        }
    }
    out->hmax = hmax;
    out->vmax = vmax;

    rfc_layout *lay = &out->probe.first_frame;
    lay->format = (si->components == 1u) ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_RGB8;
    lay->alpha = RFC_ALPHA_NONE;
    lay->width = si->width;
    lay->height = si->height;
    lay->bits_per_sample = 8u;
    lay->stride_bytes = (size_t)si->width * (size_t)si->components;
    out->probe.frame_count = 1u;
    out->probe.total_pixels = (uint64_t)si->width * (uint64_t)si->height;
    out->probe.total_decoded_bytes =
        (uint64_t)si->height * (uint64_t)lay->stride_bytes;
    out->probe.container_meta.orientation =
        si->has_exif ? si->orientation : RFC_ORIENTATION_NORMAL;
    out->probe.container_meta.exif_len = si->exif_len;
    out->probe.container_meta.xmp_len = si->xmp_len;
    size_t icc_len = 0u;
    for (uint32_t i = 1u; i <= (uint32_t)si->icc_total_chunks; i++) {
        icc_len += si->icc_chunks[i].len;
    }
    out->probe.container_meta.icc_len = icc_len;
    st = rfc_limits_check_probe(limits, &out->probe, (uint64_t)size, 1);
    if (st != RFC_OK) {
        return st;
    }

    uint64_t mcux = ((uint64_t)si->width + 8u * hmax - 1u) / (8u * hmax);
    uint64_t mcuy = ((uint64_t)si->height + 8u * vmax - 1u) / (8u * vmax);
    uint64_t blocks = 0u;
    for (uint32_t c = 0u; c < si->components; c++) {
        uint64_t b = 0u;
        if (!jpg_mul(mcux * si->h_samp[c], mcuy * si->v_samp[c], &b) ||
            !jpg_add(blocks, b, &blocks)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }
    uint64_t padded_w = mcux * 8u * hmax;
    if (!jpg_mul(blocks, (uint64_t)sizeof(JBLOCK), &out->coef_bytes) ||
        !jpg_mul(padded_w * si->components, (uint64_t)vmax * 8u * 6u,
                 &out->row_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return RFC_OK;
}

/* Provider-internal bytes (libjpeg pools; not routed through rfc_alloc). */
static rfc_status jpg_provider_bytes(const jpg_preflight *pf, int whole_image,
                                     int save_markers, size_t *out) {
    uint64_t t = JPG_FIXED_OVERHEAD;
    if ((whole_image && !jpg_add(t, pf->coef_bytes, &t)) ||
        !jpg_add(t, pf->row_bytes, &t) ||
        (save_markers && !jpg_add(t, (uint64_t)pf->info.marker_bytes +
                                         (uint64_t)pf->info.marker_count * 64u,
                                  &t)) ||
        t > (uint64_t)LONG_MAX || t > (uint64_t)SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    *out = (size_t)t;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Per-call provider error boundary                                          */
/* ------------------------------------------------------------------------ */

typedef struct jpg_guard {
    jmp_buf jb;
    rfc_status st;
    const void *session;
    const rfc_exec_context *exec;
} jpg_guard;

typedef struct jpg_err {
    struct jpeg_error_mgr pub;
    jpg_guard *guard;
} jpg_err;

typedef struct jpg_progress {
    struct jpeg_progress_mgr pub;
    jpg_guard *guard;
} jpg_progress;

static rfc_status jpg_map_error(int code) {
    switch (code) {
        case JERR_OUT_OF_MEMORY:
            return RFC_ERR_OUT_OF_MEMORY;
        case JERR_NO_BACKING_STORE:
        case JERR_WIDTH_OVERFLOW:
        case JERR_IMAGE_TOO_BIG:
            return RFC_ERR_LIMIT_EXCEEDED;
        case JERR_MISMATCHED_QUANT_TABLE:
        case JERR_NOTIMPL:
        case JERR_FRACT_SAMPLE_NOTIMPL:
        case JERR_CCIR601_NOTIMPL:
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        case JERR_INPUT_EMPTY:
        case JERR_INPUT_EOF:
            return RFC_ERR_TRUNCATED_INPUT;
        default:
            return RFC_ERR_INVALID_ARGUMENT;
    }
}

static void jpg_on_error(j_common_ptr c) {
    jpg_err *e = (jpg_err *)(void *)c->err;
    if (e->guard->st == RFC_OK) {
        e->guard->st = jpg_map_error(c->err->msg_code);
    }
    longjmp(e->guard->jb, 1);
}

static void jpg_on_emit(j_common_ptr c, int level) {
    if (level >= 0) {
        return; /* trace messages */
    }
    /* A warning means libjpeg recovered from damaged data (corrupt entropy
     * data, bad restart markers, extraneous bytes, premature end): fail
     * instead of accepting recovered output. */
    jpg_err *e = (jpg_err *)(void *)c->err;
    if (e->guard->st == RFC_OK) {
        /* JWRN_JPEG_EOF: input ended; JWRN_HIT_MARKER: coded data ended
         * at a marker before the scan was complete. Both are truncation. */
        e->guard->st = (c->err->msg_code == JWRN_JPEG_EOF ||
                        c->err->msg_code == JWRN_HIT_MARKER)
                           ? RFC_ERR_TRUNCATED_INPUT
                           : RFC_ERR_INVALID_ARGUMENT;
    }
    longjmp(e->guard->jb, 1);
}

static void jpg_on_output(j_common_ptr c) {
    (void)c; /* never print */
}

static void jpg_on_progress(j_common_ptr c) {
    jpg_progress *p = (jpg_progress *)(void *)c->progress;
    if (jpg_is_cancelled(p->guard->session, p->guard->exec)) {
        p->guard->st = RFC_ERR_CANCELLED;
        longjmp(p->guard->jb, 1);
    }
}

static void jpg_err_init(jpg_err *e, jpg_guard *g) {
    jpeg_std_error(&e->pub);
    e->pub.error_exit = jpg_on_error;
    e->pub.emit_message = jpg_on_emit;
    e->pub.output_message = jpg_on_output;
    e->guard = g;
}

static void jpg_progress_init(jpg_progress *p, jpg_guard *g) {
    memset(p, 0, sizeof(*p));
    p->pub.progress_monitor = jpg_on_progress;
    p->guard = g;
}

/* Growing-free output destination over one pre-sized rfc_alloc buffer. */
typedef struct jpg_dest {
    struct jpeg_destination_mgr pub;
    jpg_guard *guard;
    uint8_t *buf;
    size_t cap;
    size_t len;
    int full;
} jpg_dest;

static void jpg_dest_init(j_compress_ptr c) {
    jpg_dest *d = (jpg_dest *)(void *)c->dest;
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = d->cap;
}

static boolean jpg_dest_empty(j_compress_ptr c) {
    jpg_dest *d = (jpg_dest *)(void *)c->dest;
    d->full = 1;
    d->guard->st = RFC_ERR_LIMIT_EXCEEDED; /* output outgrew source + slack */
    longjmp(d->guard->jb, 1);
    return FALSE;
}

static void jpg_dest_term(j_compress_ptr c) {
    jpg_dest *d = (jpg_dest *)(void *)c->dest;
    d->len = d->cap - d->pub.free_in_buffer;
}

/* ------------------------------------------------------------------------ */
/* Session lifecycle and probe                                               */
/* ------------------------------------------------------------------------ */

static rfc_status jpg_open(const rfc_exec_context *exec, void **out_session) {
    (void)exec;
    if (!out_session) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    jpg_session *s = (jpg_session *)malloc(sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&s->cancelled, 0);
    *out_session = s;
    return RFC_OK;
}

static void jpg_close(void *session) {
    free(session);
}

static void jpg_cancel(void *session) {
    jpg_session *s = (jpg_session *)session;
    if (s) {
        atomic_store_explicit(&s->cancelled, 1, memory_order_release);
    }
}

static rfc_status jpg_probe_meta(const rfc_exec_context *exec,
                                 const uint8_t *bytes, size_t size,
                                 jpg_preflight *pf, rfc_metadata *out_meta) {
    rfc_limits scratch;
    const rfc_limits *limits = jpg_limits_of(exec, &scratch);
    rfc_status st = jpg_preflight_run(bytes, size, limits, pf);
    if (st != RFC_OK) {
        return st;
    }
    const rfc_allocator *alloc = jpg_alloc_of(exec);
    st = rfc_jpeg_scan_materialize_meta(&pf->info, bytes, alloc,
                                        jpg_meta_cap_of(limits), out_meta);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_limits_check_metadata(limits, out_meta);
    if (st != RFC_OK) {
        rfc_metadata_free(out_meta, alloc);
        memset(out_meta, 0, sizeof(*out_meta));
    }
    return st;
}

static rfc_status jpg_probe(void *session, const rfc_exec_context *exec,
                            const uint8_t *bytes, size_t size,
                            rfc_image_probe *out_probe) {
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (jpg_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    jpg_preflight pf;
    rfc_metadata meta;
    rfc_status st = jpg_probe_meta(exec, bytes, size, &pf, &meta);
    if (st != RFC_OK) {
        return st;
    }
    *out_probe = pf.probe;
    out_probe->container_meta = meta;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Pixel decode                                                             */
/* ------------------------------------------------------------------------ */

typedef struct jpg_decode_ctx {
    jpg_guard guard;
    jpg_err err;
    jpg_progress prog;
    struct jpeg_decompress_struct d;
    const uint8_t *bytes;
    size_t size;
    uint32_t components;
    long mem_limit;
    rfc_frame *frame;
} jpg_decode_ctx;

static rfc_status jpg_decode_run(jpg_decode_ctx *x) {
    memset(&x->d, 0, sizeof(x->d));
    if (setjmp(x->guard.jb)) {
        jpeg_destroy_decompress(&x->d);
        return x->guard.st != RFC_OK ? x->guard.st : RFC_ERR_INVALID_ARGUMENT;
    }
    x->d.err = &x->err.pub;
    jpeg_create_decompress(&x->d);
    x->d.mem->max_memory_to_use = x->mem_limit;
    x->d.progress = &x->prog.pub;
    jpeg_mem_src(&x->d, x->bytes, (unsigned long)x->size);
    if (jpeg_read_header(&x->d, TRUE) != JPEG_HEADER_OK) {
        x->guard.st = RFC_ERR_INVALID_ARGUMENT;
        longjmp(x->guard.jb, 1);
    }
    const rfc_layout *lay = &x->frame->layout;
    if (x->d.image_width != lay->width || x->d.image_height != lay->height ||
        (uint32_t)x->d.num_components != x->components) {
        x->guard.st = RFC_ERR_INVALID_ARGUMENT;
        longjmp(x->guard.jb, 1);
    }
    x->d.out_color_space = (x->components == 1u) ? JCS_GRAYSCALE : JCS_RGB;
    x->d.dct_method = JDCT_ISLOW;
    x->d.do_fancy_upsampling = TRUE;
    x->d.do_block_smoothing = FALSE;
    x->d.quantize_colors = FALSE;
    jpeg_start_decompress(&x->d);
    if (x->d.output_width != lay->width || x->d.output_height != lay->height ||
        (uint32_t)x->d.output_components != x->components) {
        x->guard.st = RFC_ERR_INVALID_ARGUMENT;
        longjmp(x->guard.jb, 1);
    }
    while (x->d.output_scanline < x->d.output_height) {
        if ((x->d.output_scanline & 15u) == 0u &&
            jpg_is_cancelled(x->guard.session, x->guard.exec)) {
            x->guard.st = RFC_ERR_CANCELLED;
            longjmp(x->guard.jb, 1);
        }
        JSAMPROW row = x->frame->data +
                       (size_t)x->d.output_scanline * lay->stride_bytes;
        if (jpeg_read_scanlines(&x->d, &row, 1) != 1) {
            x->guard.st = RFC_ERR_TRUNCATED_INPUT;
            longjmp(x->guard.jb, 1);
        }
    }
    jpeg_finish_decompress(&x->d);
    jpeg_destroy_decompress(&x->d);
    return RFC_OK;
}

static rfc_status jpg_decode(void *session, const rfc_exec_context *exec,
                             const uint8_t *bytes, size_t size,
                             const rfc_setting_value *settings,
                             size_t setting_count, rfc_frame **out_frames,
                             size_t *out_frame_count, rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (setting_count > 0u) {
        rfc_status vst = rfc_registry_validate_settings(RFC_FORMAT_JPEG,
                                                        settings,
                                                        setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (jpg_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    const rfc_allocator *alloc = jpg_alloc_of(exec);
    jpg_preflight pf;
    rfc_metadata meta;
    rfc_status st = jpg_probe_meta(exec, bytes, size, &pf, &meta);
    if (st != RFC_OK) {
        return st;
    }
    int whole_image = (pf.info.sos_count > 1u || pf.info.sof_marker == 0xC2u ||
                       pf.info.sof_marker == 0xCAu);
    size_t prov = 0u;
    st = jpg_provider_bytes(&pf, whole_image, 0, &prov);
    jpg_budget_guard g;
    memset(&g, 0, sizeof(g));
    if (st == RFC_OK) {
        st = jpg_reserve(exec, prov, &g);
    }
    rfc_frame *frames = NULL;
    if (st == RFC_OK) {
        frames = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(rfc_frame));
        if (!frames) {
            st = RFC_ERR_OUT_OF_MEMORY;
        } else {
            memset(frames, 0, sizeof(*frames));
            st = exec ? rfc_exec_alloc_frame(exec, &frames[0],
                                             &pf.probe.first_frame, &meta)
                      : rfc_frame_alloc(&frames[0], &pf.probe.first_frame,
                                        &meta, alloc);
            if (st != RFC_OK) {
                alloc->free(alloc->ctx, frames);
                frames = NULL;
            }
        }
    }
    rfc_metadata_free(&meta, alloc);
    if (st != RFC_OK) {
        jpg_release(&g);
        return st;
    }

    jpg_decode_ctx *x = (jpg_decode_ctx *)alloc->alloc(alloc->ctx, sizeof(*x));
    if (!x) {
        rfc_frame_free(&frames[0]);
        alloc->free(alloc->ctx, frames);
        jpg_release(&g);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(x, 0, sizeof(*x));
    x->guard.session = session;
    x->guard.exec = exec;
    jpg_err_init(&x->err, &x->guard);
    jpg_progress_init(&x->prog, &x->guard);
    x->bytes = bytes;
    x->size = size;
    x->components = pf.info.components;
    x->mem_limit = (long)prov;
    x->frame = &frames[0];
    st = jpg_decode_run(x);
    alloc->free(alloc->ctx, x);
    jpg_release(&g);
    if (st == RFC_OK && jpg_is_cancelled(session, exec)) {
        st = RFC_ERR_CANCELLED;
    }
    if (st != RFC_OK) {
        rfc_frame_free(&frames[0]);
        alloc->free(alloc->ctx, frames);
        return st;
    }
    *out_frames = frames;
    *out_frame_count = 1u;
    if (out_meta) {
        *out_meta = frames[0].metadata;
    }
    return RFC_OK;
}

static rfc_status jpg_encode(void *session, const rfc_exec_context *exec,
                             const rfc_frame *frames, size_t frame_count,
                             const rfc_metadata *meta,
                             const rfc_setting_value *settings,
                             size_t setting_count, rfc_encoded *out_encoded) {
    (void)session;
    (void)exec;
    (void)frames;
    (void)frame_count;
    (void)meta;
    if (!out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    rfc_status st = rfc_registry_validate_settings(
        RFC_FORMAT_JPEG, setting_count > 0u ? settings : NULL, setting_count,
        NULL);
    if (st != RFC_OK) {
        return st;
    }
    /* Every exactness class is lossless; encoding pixels as baseline JPEG is
     * lossy, so it is refused rather than approximated. JPEG output exists
     * only as coefficient-exact optimization or a byte-identical copy. */
    return RFC_ERR_UNSUPPORTED_COMBINATION;
}

/* ------------------------------------------------------------------------ */
/* Coefficient-exact entropy optimization                                    */
/* ------------------------------------------------------------------------ */

static rfc_status jpg_parse_policy(const rfc_setting_value *settings,
                                   size_t setting_count, int *out_strip) {
    *out_strip = 0;
    if (setting_count > 0u) {
        rfc_status st = rfc_registry_validate_settings(RFC_FORMAT_JPEG, settings,
                                                       setting_count, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }
    for (size_t i = 0u; i < setting_count; i++) {
        if (strcmp(settings[i].id, "metadata") == 0) {
            rfc_metadata_policy pol = RFC_META_POLICY_PRESERVE;
            rfc_status st = rfc_metadata_policy_parse(settings[i].value, &pol);
            if (st != RFC_OK) {
                return st;
            }
            *out_strip = (pol == RFC_META_POLICY_STRIP) ? 1 : 0;
        } else if (strcmp(settings[i].id, "exactness") == 0 &&
                   strcmp(settings[i].value, "coefficient-exact") != 0) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
    }
    return RFC_OK;
}

/* Coefficient-exact optimization preconditions beyond import: the IJG encoder
 * cannot reproduce a quantization slot redefined between scans, and every
 * saved marker is written before the first output scan, so a JFIF or Adobe
 * header after the first source SOS would change the colour interpretation. */
static rfc_status jpg_optimize_preflight(const uint8_t *bytes, size_t size,
                                         const rfc_limits *limits,
                                         jpg_preflight *pf) {
    rfc_status st = jpg_preflight_run(bytes, size, limits, pf);
    if (st != RFC_OK) {
        return st;
    }
    /* The IJG encoder holds one DC and one AC selector per component, so a
     * source whose scans give one component different tables cannot be
     * re-encoded with its selectors intact. */
    if (pf->info.quant_redefined || pf->info.late_structural > 0u ||
        pf->info.selector_conflict) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    return RFC_OK;
}

static rfc_status jpg_check_optimizable(void *session,
                                        const rfc_exec_context *exec,
                                        const uint8_t *bytes, size_t size) {
    if (jpg_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_limits scratch;
    jpg_preflight pf;
    return jpg_optimize_preflight(bytes, size, jpg_limits_of(exec, &scratch),
                                  &pf);
}

/* Natural-order index of each zigzag position (ITU-T T.81 Figure A.6). */
static const uint8_t k_jpg_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* libjpeg's frame writer emits only the tables components reference, so
 * every other populated slot is written back verbatim as its own DQT. */
static void jpg_emit_unused_dqt(j_decompress_ptr d, j_compress_ptr c) {
    int referenced[NUM_QUANT_TBLS] = {0};
    for (int ci = 0; ci < d->num_components; ci++) {
        int t = d->comp_info[ci].quant_tbl_no;
        if (t >= 0 && t < NUM_QUANT_TBLS) {
            referenced[t] = 1;
        }
    }
    for (int t = 0; t < NUM_QUANT_TBLS; t++) {
        const JQUANT_TBL *q = d->quant_tbl_ptrs[t];
        if (!q || referenced[t]) {
            continue;
        }
        int wide = 0;
        for (int k = 0; k < DCTSIZE2; k++) {
            if (q->quantval[k] > 255u) {
                wide = 1;
            }
        }
        JOCTET seg[1 + 128];
        unsigned len = 0u;
        seg[len++] = (JOCTET)((wide ? 0x10 : 0x00) | t);
        for (int k = 0; k < DCTSIZE2; k++) {
            UINT16 v = q->quantval[k_jpg_zigzag[k]];
            if (wide) {
                seg[len++] = (JOCTET)(v >> 8);
            }
            seg[len++] = (JOCTET)(v & 0xFFu);
        }
        jpeg_write_marker(c, 0xDB /* DQT */, seg, len);
    }
}

typedef struct jpg_transcode_ctx {
    jpg_guard guard;
    jpg_err src_err;
    jpg_err dst_err;
    jpg_progress src_prog;
    jpg_progress dst_prog;
    jpg_dest dest;
    struct jpeg_decompress_struct d;
    struct jpeg_compress_struct c;
    const uint8_t *bytes;
    size_t size;
    int strip;
    long mem_limit;
    uint8_t dc_sel[4];
    uint8_t ac_sel[4];
} jpg_transcode_ctx;

static rfc_status jpg_transcode_run(jpg_transcode_ctx *x) {
    memset(&x->d, 0, sizeof(x->d));
    memset(&x->c, 0, sizeof(x->c));
    if (setjmp(x->guard.jb)) {
        jpeg_destroy_compress(&x->c);
        jpeg_destroy_decompress(&x->d);
        return x->guard.st != RFC_OK ? x->guard.st : RFC_ERR_INVALID_ARGUMENT;
    }
    x->d.err = &x->src_err.pub;
    jpeg_create_decompress(&x->d);
    x->d.mem->max_memory_to_use = x->mem_limit;
    x->d.progress = &x->src_prog.pub;
    jpeg_mem_src(&x->d, x->bytes, (unsigned long)x->size);
    if (!x->strip) {
        jpeg_save_markers(&x->d, JPEG_COM, 0xFFFF);
        for (int m = 0; m < 16; m++) {
            jpeg_save_markers(&x->d, JPEG_APP0 + m, 0xFFFF);
        }
    }
    if (jpeg_read_header(&x->d, TRUE) != JPEG_HEADER_OK) {
        x->guard.st = RFC_ERR_INVALID_ARGUMENT;
        longjmp(x->guard.jb, 1);
    }
    jvirt_barray_ptr *coefs = jpeg_read_coefficients(&x->d);

    x->c.err = &x->dst_err.pub;
    jpeg_create_compress(&x->c);
    x->c.mem->max_memory_to_use = x->mem_limit;
    x->c.progress = &x->dst_prog.pub;
    x->c.dest = &x->dest.pub;
    jpeg_copy_critical_parameters(&x->d, &x->c);
    /* jpeg_copy_critical_parameters does not copy Huffman selectors, and
     * libjpeg overwrites both selector fields at every SOS (an AC scan writes
     * DC selector 0), so the selectors come from the scans that use them. */
    for (int ci = 0; ci < x->c.num_components && ci < 4; ci++) {
        x->c.comp_info[ci].dc_tbl_no = x->dc_sel[ci];
        x->c.comp_info[ci].ac_tbl_no = x->ac_sel[ci];
    }
    x->c.arith_code = x->d.arith_code;
    x->c.optimize_coding = x->d.arith_code ? FALSE : TRUE;
    if (x->d.progressive_mode) {
        jpeg_simple_progression(&x->c);
    }
    x->c.restart_interval = 0;
    x->c.restart_in_rows = 0;
    if (x->strip) {
        x->c.write_JFIF_header = x->d.saw_JFIF_marker;
        x->c.write_Adobe_marker = x->d.saw_Adobe_marker;
    } else {
        x->c.write_JFIF_header = FALSE;
        x->c.write_Adobe_marker = FALSE;
    }
    jpeg_write_coefficients(&x->c, coefs);
    jpg_emit_unused_dqt(&x->d, &x->c);
    if (!x->strip) {
        for (jpeg_saved_marker_ptr m = x->d.marker_list; m; m = m->next) {
            if (m->original_length != m->data_length) {
                x->guard.st = RFC_ERR_INVALID_ARGUMENT;
                longjmp(x->guard.jb, 1);
            }
            jpeg_write_marker(&x->c, m->marker, m->data, m->data_length);
        }
    }
    jpeg_finish_compress(&x->c);
    jpeg_finish_decompress(&x->d);
    jpeg_destroy_compress(&x->c);
    jpeg_destroy_decompress(&x->d);
    return RFC_OK;
}

static rfc_status jpg_optimize(void *session, const rfc_exec_context *exec,
                               const uint8_t *bytes, size_t size,
                               const rfc_setting_value *settings,
                               size_t setting_count, rfc_encoded *out_jpeg) {
    if (!out_jpeg) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_jpeg, 0, sizeof(*out_jpeg));
    int strip = 0;
    rfc_status st = jpg_parse_policy(settings, setting_count, &strip);
    if (st != RFC_OK) {
        return st;
    }
    if (jpg_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_limits scratch;
    const rfc_limits *limits = jpg_limits_of(exec, &scratch);
    jpg_preflight pf;
    st = jpg_optimize_preflight(bytes, size, limits, &pf);
    if (st != RFC_OK) {
        return st;
    }
    size_t prov = 0u;
    st = jpg_provider_bytes(&pf, 1, !strip, &prov);
    if (st != RFC_OK) {
        return st;
    }
    long long slack = atomic_load(&g_jpg_output_slack);
    if (size > SIZE_MAX - JPG_OUTPUT_SLACK) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    size_t cap = (slack >= 0) ? size + (size_t)slack
                 : ((size_t)(-slack) < size ? size - (size_t)(-slack) : 1u);
    jpg_budget_guard g;
    st = jpg_reserve(exec, prov, &g);
    if (st != RFC_OK) {
        return st;
    }
    const rfc_allocator *alloc = jpg_alloc_of(exec);
    uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, cap);
    jpg_transcode_ctx *x =
        buf ? (jpg_transcode_ctx *)alloc->alloc(alloc->ctx, sizeof(*x)) : NULL;
    if (!buf || !x) {
        if (buf) {
            alloc->free(alloc->ctx, buf);
        }
        jpg_release(&g);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(x, 0, sizeof(*x));
    x->guard.session = session;
    x->guard.exec = exec;
    jpg_err_init(&x->src_err, &x->guard);
    jpg_err_init(&x->dst_err, &x->guard);
    jpg_progress_init(&x->src_prog, &x->guard);
    jpg_progress_init(&x->dst_prog, &x->guard);
    x->dest.pub.init_destination = jpg_dest_init;
    x->dest.pub.empty_output_buffer = jpg_dest_empty;
    x->dest.pub.term_destination = jpg_dest_term;
    x->dest.guard = &x->guard;
    x->dest.buf = buf;
    x->dest.cap = cap;
    x->bytes = bytes;
    x->size = size;
    x->strip = strip;
    x->mem_limit = (long)prov;
    memcpy(x->dc_sel, pf.info.dc_sel, sizeof(x->dc_sel));
    memcpy(x->ac_sel, pf.info.ac_sel, sizeof(x->ac_sel));
    st = jpg_transcode_run(x);
    size_t out_len = x->dest.len;
    int dest_full = x->dest.full;
    alloc->free(alloc->ctx, x);
    jpg_release(&g);
    if (st == RFC_ERR_LIMIT_EXCEEDED && dest_full && !strip) {
        /* The re-encode outgrew source + 64 KiB, so it cannot be smaller:
         * take the not-smaller fallback below. */
        st = RFC_OK;
        out_len = size;
    }
    if (st == RFC_OK && jpg_is_cancelled(session, exec)) {
        st = RFC_ERR_CANCELLED;
    }
    if (st != RFC_OK || out_len == 0u) {
        alloc->free(alloc->ctx, buf);
        return st != RFC_OK ? st : RFC_ERR_INVALID_ARGUMENT;
    }
    if (!strip && out_len >= size) {
        /* Optimization never grows a file: publish the source unchanged. */
        if (cap < size) {
            uint8_t *nb = (uint8_t *)alloc->alloc(alloc->ctx, size);
            if (!nb) {
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_OUT_OF_MEMORY;
            }
            alloc->free(alloc->ctx, buf);
            buf = nb;
        }
        memcpy(buf, bytes, size);
        out_len = size;
    }
    out_jpeg->data = buf;
    out_jpeg->len = out_len;
    out_jpeg->allocator = alloc;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Coefficient extraction                                                    */
/* ------------------------------------------------------------------------ */

static void jpg_free_coeffs_with(const rfc_allocator *alloc,
                                 rfc_jpeg_coeff_image *img) {
    if (!img) {
        return;
    }
    for (uint32_t c = 0u; c < 4u; c++) {
        if (img->components[c].dct_blocks) {
            alloc->free(alloc->ctx, (void *)img->components[c].dct_blocks);
        }
    }
    alloc->free(alloc->ctx, img);
}

typedef struct jpg_coeff_ctx {
    jpg_guard guard;
    jpg_err err;
    jpg_progress prog;
    struct jpeg_decompress_struct d;
    const uint8_t *bytes;
    size_t size;
    long mem_limit;
    const rfc_allocator *alloc;
    rfc_jpeg_coeff_image *img;
    uint8_t dc_sel[4];
    uint8_t ac_sel[4];
} jpg_coeff_ctx;

static rfc_status jpg_coeff_run(jpg_coeff_ctx *x) {
    memset(&x->d, 0, sizeof(x->d));
    if (setjmp(x->guard.jb)) {
        jpeg_destroy_decompress(&x->d);
        return x->guard.st != RFC_OK ? x->guard.st : RFC_ERR_INVALID_ARGUMENT;
    }
    x->d.err = &x->err.pub;
    jpeg_create_decompress(&x->d);
    x->d.mem->max_memory_to_use = x->mem_limit;
    x->d.progress = &x->prog.pub;
    jpeg_mem_src(&x->d, x->bytes, (unsigned long)x->size);
    if (jpeg_read_header(&x->d, TRUE) != JPEG_HEADER_OK) {
        x->guard.st = RFC_ERR_INVALID_ARGUMENT;
        longjmp(x->guard.jb, 1);
    }
    jvirt_barray_ptr *coefs = jpeg_read_coefficients(&x->d);
    rfc_jpeg_coeff_image *img = x->img;
    img->width = x->d.image_width;
    img->height = x->d.image_height;
    img->progressive = x->d.progressive_mode ? 1 : 0;
    img->arithmetic_coding = x->d.arith_code ? 1 : 0;
    img->component_count = (uint32_t)x->d.num_components;
    int baseline = (!x->d.progressive_mode && !x->d.arith_code) ? 1 : 0;
    /* Every populated slot is part of the contract (optimization writes the
     * unused ones back verbatim); baseline compatibility depends only on the
     * tables components reference. */
    int referenced[4] = {0, 0, 0, 0};
    for (int ci = 0; ci < x->d.num_components && ci < 4; ci++) {
        if (x->d.comp_info[ci].quant_tbl_no >= 0 &&
            x->d.comp_info[ci].quant_tbl_no < 4) {
            referenced[x->d.comp_info[ci].quant_tbl_no] = 1;
        }
    }
    for (int t = 0; t < NUM_QUANT_TBLS && t < 4; t++) {
        if (x->d.quant_tbl_ptrs[t]) {
            img->quant_tables[t].populated = 1;
            for (int k = 0; k < DCTSIZE2; k++) {
                img->quant_tables[t].values[k] =
                    (uint16_t)x->d.quant_tbl_ptrs[t]->quantval[k];
                if (referenced[t] && x->d.quant_tbl_ptrs[t]->quantval[k] > 255u) {
                    baseline = 0;
                }
            }
        }
    }
    for (int ci = 0; ci < x->d.num_components && ci < 4; ci++) {
        jpeg_component_info *cp = &x->d.comp_info[ci];
        rfc_jpeg_component_spec *cs = &img->components[ci];
        cs->component_id = (uint8_t)cp->component_id;
        cs->h_samp_factor = (uint8_t)cp->h_samp_factor;
        cs->v_samp_factor = (uint8_t)cp->v_samp_factor;
        cs->quant_table_idx = (uint8_t)cp->quant_tbl_no;
        cs->dc_table_idx = x->dc_sel[ci];
        cs->ac_table_idx = x->ac_sel[ci];
        if (x->dc_sel[ci] > 1u || x->ac_sel[ci] > 1u) {
            baseline = 0;
        }
        cs->blocks_wide = cp->width_in_blocks;
        cs->blocks_high = cp->height_in_blocks;
        size_t row_coefs = (size_t)cp->width_in_blocks * (size_t)DCTSIZE2;
        size_t bytes = row_coefs * (size_t)cp->height_in_blocks * sizeof(int16_t);
        int16_t *blk = (int16_t *)x->alloc->alloc(x->alloc->ctx, bytes ? bytes : 1u);
        if (!blk) {
            x->guard.st = RFC_ERR_OUT_OF_MEMORY;
            longjmp(x->guard.jb, 1);
        }
        cs->dct_blocks = blk;
        for (JDIMENSION by = 0; by < cp->height_in_blocks; by++) {
            if ((by & 15u) == 0u &&
                jpg_is_cancelled(x->guard.session, x->guard.exec)) {
                x->guard.st = RFC_ERR_CANCELLED;
                longjmp(x->guard.jb, 1);
            }
            JBLOCKARRAY rows = (*x->d.mem->access_virt_barray)(
                (j_common_ptr)&x->d, coefs[ci], by, 1, FALSE);
            for (JDIMENSION bx = 0; bx < cp->width_in_blocks; bx++) {
                for (int k = 0; k < DCTSIZE2; k++) {
                    blk[(size_t)by * row_coefs + (size_t)bx * DCTSIZE2 + (size_t)k] =
                        (int16_t)rows[0][bx][k];
                }
            }
        }
    }
    /* "baseline" means baseline-compatible coding: sequential Huffman with
     * 8-bit quantization tables and table selectors 0 and 1. */
    img->baseline = baseline;
    jpeg_finish_decompress(&x->d);
    jpeg_destroy_decompress(&x->d);
    return RFC_OK;
}

static rfc_status jpg_read_coeffs(void *session, const rfc_exec_context *exec,
                                  const uint8_t *bytes, size_t size,
                                  struct rfc_jpeg_coeff_image **out_coeffs,
                                  rfc_metadata *out_meta) {
    if (!out_coeffs || !out_meta) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_coeffs = NULL;
    memset(out_meta, 0, sizeof(*out_meta));
    if (jpg_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    const rfc_allocator *alloc = jpg_alloc_of(exec);
    jpg_preflight pf;
    rfc_metadata meta;
    rfc_status st = jpg_probe_meta(exec, bytes, size, &pf, &meta);
    if (st != RFC_OK) {
        return st;
    }
    size_t prov = 0u;
    st = jpg_provider_bytes(&pf, 1, 0, &prov);
    jpg_budget_guard g;
    memset(&g, 0, sizeof(g));
    if (st == RFC_OK) {
        st = jpg_reserve(exec, prov, &g);
    }
    rfc_jpeg_coeff_image *img = NULL;
    jpg_coeff_ctx *x = NULL;
    if (st == RFC_OK) {
        img = (rfc_jpeg_coeff_image *)alloc->alloc(alloc->ctx, sizeof(*img));
        if (img) {
            memset(img, 0, sizeof(*img)); /* before any failure path frees it */
        }
        x = img ? (jpg_coeff_ctx *)alloc->alloc(alloc->ctx, sizeof(*x)) : NULL;
        if (!img || !x) {
            st = RFC_ERR_OUT_OF_MEMORY;
        }
    }
    if (st == RFC_OK) {
        memset(x, 0, sizeof(*x));
        x->guard.session = session;
        x->guard.exec = exec;
        jpg_err_init(&x->err, &x->guard);
        jpg_progress_init(&x->prog, &x->guard);
        x->bytes = bytes;
        x->size = size;
        x->mem_limit = (long)prov;
        x->alloc = alloc;
        x->img = img;
        memcpy(x->dc_sel, pf.info.dc_sel, sizeof(x->dc_sel));
        memcpy(x->ac_sel, pf.info.ac_sel, sizeof(x->ac_sel));
        st = jpg_coeff_run(x);
    }
    if (x) {
        alloc->free(alloc->ctx, x);
    }
    jpg_release(&g);
    if (st != RFC_OK) {
        jpg_free_coeffs_with(alloc, img);
        rfc_metadata_free(&meta, alloc);
        return st;
    }
    *out_coeffs = img;
    *out_meta = meta;
    return RFC_OK;
}

static void jpg_free_coeffs(void *session, const rfc_exec_context *exec,
                            struct rfc_jpeg_coeff_image *coeffs) {
    (void)session;
    jpg_free_coeffs_with(jpg_alloc_of(exec), coeffs);
}

/* ------------------------------------------------------------------------ */
/* Marker contract                                                           */
/* ------------------------------------------------------------------------ */

/* The colour interpretation libjpeg fixes at jpeg_read_header, from the
 * structural headers seen before the first SOS (jdapimin.c
 * default_decompress_parms). */
static int jpg_color_space(const rfc_jpeg_scan_info *si) {
    if (si->components == 1u) {
        return 1; /* gray */
    }
    if (si->header_jfif) {
        return 2; /* YCbCr */
    }
    if (si->header_adobe) {
        return si->header_adobe_transform == 0 ? 3 : 2;
    }
    if (si->component_ids[0] == 82u && si->component_ids[1] == 71u &&
        si->component_ids[2] == 66u) {
        return 3; /* RGB by component ids */
    }
    return 2;
}

/* The headers libjpeg's compressor regenerates under metadata=strip: a
 * 14-byte JFIF APP0 with a 0x0 thumbnail and a 12-byte Adobe APP14. */
static int jpg_is_fresh_jfif(uint8_t m, const uint8_t *p, size_t n) {
    return m == 0xE0u && n == 14u && memcmp(p, "JFIF\x00", 5) == 0 &&
           p[12] == 0u && p[13] == 0u;
}

static int jpg_is_fresh_adobe(uint8_t m, const uint8_t *p, size_t n) {
    return m == 0xEEu && n == 12u && memcmp(p, "Adobe", 5) == 0;
}

static rfc_status jpg_compare_markers(void *session,
                                      const rfc_exec_context *exec,
                                      const uint8_t *src, size_t src_size,
                                      const uint8_t *dst, size_t dst_size,
                                      int strip, const char **out_detail) {
    const char *dummy = NULL;
    const char **detail = out_detail ? out_detail : &dummy;
    *detail = NULL;
    rfc_limits scratch;
    const rfc_limits *limits = jpg_limits_of(exec, &scratch);
    rfc_jpeg_scan_info *si = (rfc_jpeg_scan_info *)malloc(2u * sizeof(*si));
    if (!si) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_status st = jpg_is_cancelled(session, exec) ? RFC_ERR_CANCELLED : RFC_OK;
    if (st == RFC_OK) {
        st = rfc_jpeg_scan(src, src_size, jpg_meta_cap_of(limits),
                           RFC_JPEG_SCAN_ALLOW_ARITHMETIC, &si[0]);
    }
    if (st == RFC_OK && jpg_is_cancelled(session, exec)) {
        st = RFC_ERR_CANCELLED;
    }
    if (st == RFC_OK) {
        st = rfc_jpeg_scan(dst, dst_size, jpg_meta_cap_of(limits),
                           RFC_JPEG_SCAN_ALLOW_ARITHMETIC, &si[1]);
    }
    if (st == RFC_OK && jpg_is_cancelled(session, exec)) {
        st = RFC_ERR_CANCELLED;
    }
    if (st != RFC_OK) {
        free(si);
        return st;
    }
    if (jpg_color_space(&si[0]) != jpg_color_space(&si[1])) {
        free(si);
        *detail = "JPEG colour interpretation (JFIF, Adobe transform or "
                  "component ids) differs";
        return RFC_ERR_VERIFY_MISMATCH;
    }
    size_t ps = 2u, pd = 2u;
    uint8_t ms = 0u, md = 0u;
    const uint8_t *as = NULL, *ad = NULL;
    size_t ls = 0u, ld = 0u;
    if (!strip) {
        for (uint32_t n = 0u;; n++) {
            if ((n & 255u) == 255u && jpg_is_cancelled(session, exec)) {
                free(si);
                return RFC_ERR_CANCELLED;
            }
            int hs = rfc_jpeg_next_app_segment(src, src_size, &ps, &ms, &as, &ls);
            int hd = rfc_jpeg_next_app_segment(dst, dst_size, &pd, &md, &ad, &ld);
            if (!hs && !hd) {
                break;
            }
            if (hs != hd || ms != md || ls != ld ||
                (ls > 0u && memcmp(as, ad, ls) != 0)) {
                free(si);
                *detail = "ordered APPn/COM marker list differs under "
                          "metadata=preserve";
                return RFC_ERR_VERIFY_MISMATCH;
            }
        }
    } else {
        /* Regenerated headers must sit before the output's first scan, and
         * a source header after its first scan is never reproducible. */
        if (si[0].late_structural > 0u || si[1].late_structural > 0u ||
            si[1].header_jfif != si[0].header_jfif ||
            si[1].header_adobe != si[0].header_adobe) {
            free(si);
            *detail = "metadata=strip structural headers are missing, extra, "
                      "or placed after the first scan";
            return RFC_ERR_VERIFY_MISMATCH;
        }
        /* The reference is the last JFIF and Adobe header before the first
         * scan: libjpeg's examine_app0/app14 overwrite density and transform
         * at each one, and jpeg_copy_critical_parameters copies that state. */
        const uint8_t *sj = NULL, *sa = NULL;
        for (uint32_t n = 0u;
             rfc_jpeg_next_app_segment(src, src_size, &ps, &ms, &as, &ls); n++) {
            if ((n & 255u) == 255u && jpg_is_cancelled(session, exec)) {
                free(si);
                return RFC_ERR_CANCELLED;
            }
            if (ms == 0xE0u && ls >= 14u && memcmp(as, "JFIF\x00", 5) == 0) {
                sj = as;
            } else if (ms == 0xEEu && ls >= 12u && memcmp(as, "Adobe", 5) == 0) {
                sa = as;
            }
        }
        uint32_t jfif = 0u, adobe = 0u;
        while (rfc_jpeg_next_app_segment(dst, dst_size, &pd, &md, &ad, &ld)) {
            if (jpg_is_fresh_jfif(md, ad, ld) && si[0].header_jfif && sj &&
                jfif == 0u) {
                /* Version is copied from a 1.x source (else libjpeg's 1.01);
                 * density units and values are always copied. */
                int ver_ok = (sj[5] == 1u) ? (ad[5] == sj[5] && ad[6] == sj[6])
                                           : (ad[5] == 1u && ad[6] == 1u);
                if (!ver_ok || memcmp(ad + 7, sj + 7, 5) != 0) {
                    free(si);
                    *detail = "metadata=strip JFIF header changes version or "
                              "density";
                    return RFC_ERR_VERIFY_MISMATCH;
                }
                jfif++;
            } else if (jpg_is_fresh_adobe(md, ad, ld) && si[0].header_adobe &&
                       sa && adobe == 0u) {
                if (ad[11] != sa[11]) {
                    free(si);
                    *detail = "metadata=strip Adobe header changes the "
                              "colour transform";
                    return RFC_ERR_VERIFY_MISMATCH;
                }
                adobe++;
            } else {
                free(si);
                *detail = "metadata=strip output keeps a non-structural or "
                          "altered APPn/COM marker";
                return RFC_ERR_VERIFY_MISMATCH;
            }
        }
        if (jfif != (si[0].header_jfif ? 1u : 0u) ||
            adobe != (si[0].header_adobe ? 1u : 0u)) {
            free(si);
            *detail = "metadata=strip output is missing a required JFIF or "
                      "Adobe structural header";
            return RFC_ERR_VERIFY_MISMATCH;
        }
    }
    free(si);
    /* A cancellation during the walks never ends in a success verdict. */
    return jpg_is_cancelled(session, exec) ? RFC_ERR_CANCELLED : RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static const rfc_session_adapter k_jpeg_session_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_JPEG,
    jpg_open,
    jpg_close,
    jpg_probe,
    jpg_encode,
    jpg_decode,
    jpg_cancel,
    NULL
};

static const rfc_jpeg_transcode_adapter k_jpeg_transcode_adapter = {
    RFC_CODEC_ABI_VERSION,
    jpg_open,
    jpg_close,
    jpg_cancel,
    jpg_check_optimizable,
    jpg_optimize,
    jpg_read_coeffs,
    jpg_free_coeffs,
    jpg_compare_markers
};

rfc_status rfc_codecs_register_jpeg(void) {
    return rfc_registry_register_jpeg_pair(&k_jpeg_session_adapter,
                                           &k_jpeg_transcode_adapter);
}

rfc_status rfc_codecs_unregister_jpeg(void) {
    return rfc_registry_unregister_jpeg_pair();
}
