#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "rasterfold.h"
#include "rfc_codec.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK ex_mutex_t;
typedef CONDITION_VARIABLE ex_cond_t;
static void ex_mutex_init(ex_mutex_t *m) { InitializeSRWLock(m); }
static void ex_mutex_destroy(ex_mutex_t *m) { (void)m; }
static void ex_mutex_lock(ex_mutex_t *m) { AcquireSRWLockExclusive(m); }
static void ex_mutex_unlock(ex_mutex_t *m) { ReleaseSRWLockExclusive(m); }
static void ex_cond_init(ex_cond_t *c) { InitializeConditionVariable(c); }
static void ex_cond_destroy(ex_cond_t *c) { (void)c; }
static void ex_cond_wait(ex_cond_t *c, ex_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static void ex_cond_broadcast(ex_cond_t *c) { WakeAllConditionVariable(c); }
#else
#include <pthread.h>
typedef pthread_mutex_t ex_mutex_t;
typedef pthread_cond_t ex_cond_t;
static void ex_mutex_init(ex_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void ex_mutex_destroy(ex_mutex_t *m) { pthread_mutex_destroy(m); }
static void ex_mutex_lock(ex_mutex_t *m) { pthread_mutex_lock(m); }
static void ex_mutex_unlock(ex_mutex_t *m) { pthread_mutex_unlock(m); }
static void ex_cond_init(ex_cond_t *c) { pthread_cond_init(c, NULL); }
static void ex_cond_destroy(ex_cond_t *c) { pthread_cond_destroy(c); }
static void ex_cond_wait(ex_cond_t *c, ex_mutex_t *m) {
    pthread_cond_wait(c, m);
}
static void ex_cond_broadcast(ex_cond_t *c) { pthread_cond_broadcast(c); }
#endif

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond); \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

typedef struct alloc_tracker {
    size_t live;
    size_t calls;
    size_t fail_at;
} alloc_tracker;

static void *tracked_alloc(void *ctx, size_t size) {
    alloc_tracker *t = (alloc_tracker *)ctx;
    if (t) {
        t->calls++;
        if (t->fail_at > 0u && t->calls == t->fail_at) {
            return NULL;
        }
    }
    void *p = malloc(size ? size : 1u);
    if (p && t) {
        t->live++;
    }
    return p;
}

static void tracked_free(void *ctx, void *ptr) {
    alloc_tracker *t = (alloc_tracker *)ctx;
    if (ptr) {
        if (t && t->live > 0u) {
            t->live--;
        }
        free(ptr);
    }
}

static void init_tracked_alloc(alloc_tracker *t, rfc_allocator *a) {
    t->live = 0u;
    t->calls = 0u;
    t->fail_at = 0u;
    a->alloc = tracked_alloc;
    a->free = tracked_free;
    a->ctx = t;
}

static uint16_t read_u16(const uint8_t *p) {
    uint16_t v = 0u;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void write_u16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, sizeof(v));
}

static uint32_t read_u32(const uint8_t *p) {
    uint32_t v = 0u;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void write_u32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

static uint32_t channel_count_of(rfc_sample_format fmt) {
    switch (fmt) {
        case RFC_SAMPLE_GRAY8:
        case RFC_SAMPLE_GRAY16:
            return 1u;
        case RFC_SAMPLE_RGB8:
        case RFC_SAMPLE_RGB16:
        case RFC_SAMPLE_RGB_HALF:
        case RFC_SAMPLE_RGB_FLOAT:
            return 3u;
        case RFC_SAMPLE_RGBA8:
        case RFC_SAMPLE_RGBA16:
        case RFC_SAMPLE_RGBA_HALF:
        case RFC_SAMPLE_RGBA_FLOAT:
            return 4u;
        default:
            return 0u;
    }
}

static uint32_t default_bps_of(rfc_sample_format fmt) {
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

static rfc_alpha_mode default_alpha_of(rfc_sample_format fmt) {
    return channel_count_of(fmt) == 4u ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
}

static void fill_deterministic_pixels(rfc_frame *f) {
    CHECK(f && f->data);
    memset(f->data, 0xA5, f->data_len);

    rfc_sample_format fmt = f->layout.format;
    uint32_t w = f->layout.width;
    uint32_t h = f->layout.height;
    uint32_t bps = f->layout.bits_per_sample;
    size_t bpp = rfc_sample_bytes(fmt);
    CHECK(bpp > 0u);
    uint32_t ch = channel_count_of(fmt);
    size_t ch_b = bpp / ch;
    int is_flt = (fmt == RFC_SAMPLE_RGB_HALF || fmt == RFC_SAMPLE_RGBA_HALF ||
                  fmt == RFC_SAMPLE_RGB_FLOAT || fmt == RFC_SAMPLE_RGBA_FLOAT);
    uint32_t shift = (!is_flt && ch_b * 8u > bps) ? (ch_b * 8u - bps) : 0u;

    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = f->data + (size_t)y * f->layout.stride_bytes;
        for (uint32_t x = 0; x < w; x++) {
            for (uint32_t c = 0; c < ch; c++) {
                uint8_t *p = row + ((size_t)x * ch + c) * ch_b;
                if (ch_b == 1u) {
                    p[0] = (uint8_t)((x * 37u + y * 19u + c * 53u) & 0xffu);
                } else if (fmt == RFC_SAMPLE_RGB_HALF ||
                           fmt == RFC_SAMPLE_RGBA_HALF) {
                    uint16_t hv = (uint16_t)(0x3800u +
                                             ((x * 17u + y * 29u + c * 7u) &
                                              0x03ffu));
                    write_u16(p, hv);
                } else if (ch_b == 2u) {
                    uint32_t maxn = (1u << bps) - 1u;
                    uint16_t v =
                        (uint16_t)(((x * 131u + y * 257u + c * 97u) % (maxn + 1u))
                                   << shift);
                    write_u16(p, v);
                } else {
                    uint32_t fv =
                        0x3f000000u +
                        ((uint32_t)((x * 41u + y * 83u + c * 17u) & 0xffffu));
                    write_u32(p, fv);
                }
            }
        }
    }
}

static void fill_deterministic_frame(rfc_frame *f,
                                     uint32_t w,
                                     uint32_t h,
                                     rfc_sample_format fmt,
                                     rfc_alpha_mode alpha,
                                     uint32_t bps,
                                     size_t extra_stride,
                                     uint32_t duration_ms,
                                     const rfc_allocator *alloc) {
    size_t bpp = rfc_sample_bytes(fmt);
    CHECK(bpp > 0u);
    rfc_layout lay = {fmt, alpha, w, h, (size_t)w * bpp + extra_stride, bps};
    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, duration_ms, {0},
                         NULL, 0u, NULL, 0u};
    CHECK(rfc_frame_alloc(f, &lay, &meta, alloc) == RFC_OK);
    fill_deterministic_pixels(f);
}

/* ----------------------------------------------------------------
 * 1. Test-only RasterFold session adapter backed directly by librasterfold.a
 *    (Phase 10 D10 T02 s1 builds the production adapter later).
 * ---------------------------------------------------------------- */

typedef struct rf_test_sess {
    const rfc_exec_context *exec;
    int cancelled;
} rf_test_sess;

static ex_mutex_t g_rf_stats_lock;
static uint32_t g_rf_open_calls = 0u;
static uint32_t g_rf_close_calls = 0u;

static rfc_status rf_sess_open(const rfc_exec_context *exec, void **out_sess) {
    rf_test_sess *s = (rf_test_sess *)exec->allocator->alloc(
        exec->allocator->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    s->exec = exec;
    s->cancelled = 0;
    ex_mutex_lock(&g_rf_stats_lock);
    g_rf_open_calls++;
    ex_mutex_unlock(&g_rf_stats_lock);
    *out_sess = s;
    return RFC_OK;
}

static void rf_sess_close(void *raw) {
    rf_test_sess *s = (rf_test_sess *)raw;
    if (s) {
        const rfc_allocator *a = s->exec->allocator;
        a->free(a->ctx, s);
        ex_mutex_lock(&g_rf_stats_lock);
        g_rf_close_calls++;
        ex_mutex_unlock(&g_rf_stats_lock);
    }
}

static void rf_sess_cancel(void *raw) {
    rf_test_sess *s = (rf_test_sess *)raw;
    if (s) {
        s->cancelled = 1;
    }
}

static rfc_status rf_sess_probe(void *raw,
                                const rfc_exec_context *exec,
                                const uint8_t *bytes,
                                size_t size,
                                rfc_image_probe *out_probe) {
    (void)raw;
    (void)exec;
    if (!bytes || size < 32u || !out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (bytes[0] != 'R' || bytes[1] != 'F' || bytes[2] != 'I') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    uint32_t w = read_u32(bytes + 8u);
    uint32_t h = read_u32(bytes + 12u);
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.stride_bytes = (size_t)w * 3u;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)w * (uint64_t)h;
    out_probe->total_decoded_bytes = out_probe->total_pixels * 3ull;
    out_probe->container_meta.orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

static rfc_status rf_sess_encode(void *raw,
                                 const rfc_exec_context *exec,
                                 const rfc_frame *frames,
                                 size_t frame_count,
                                 const rfc_metadata *meta,
                                 const rfc_setting_value *settings,
                                 size_t setting_count,
                                 rfc_encoded *out_encoded) {
    (void)raw;
    (void)meta;
    (void)settings;
    (void)setting_count;
    if (!frames || frame_count != 1u ||
        frames[0].layout.format != RFC_SAMPLE_RGB8 || !out_encoded) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    rf_pixels px;
    px.width = frames[0].layout.width;
    px.height = frames[0].layout.height;
    px.stride = frames[0].layout.stride_bytes;
    px.layout = RF_LAYOUT_RGB8;
    px.data = frames[0].data;
    px.size = frames[0].data_len;

    rf_options opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = (uint32_t)sizeof(opt);
    opt.threads = rfc_exec_threads(exec);
    if (opt.threads > 9u) {
        opt.threads = 9u;
    }

    rf_buffer buf = {NULL, 0u};
    rf_error err = {0, NULL};
    if (rf_encode_image(&px, &opt, &buf, &err) != RF_OK) {
        rf_free(buf.data);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint8_t *copy = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx,
                                                      buf.size);
    if (!copy) {
        rf_free(buf.data);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, buf.data, buf.size);
    size_t sz = buf.size;
    rf_free(buf.data);

    out_encoded->data = copy;
    out_encoded->len = sz;
    out_encoded->allocator = exec->allocator;
    return RFC_OK;
}

static rfc_status rf_sess_decode(void *raw,
                                 const rfc_exec_context *exec,
                                 const uint8_t *bytes,
                                 size_t size,
                                 const rfc_setting_value *settings,
                                 size_t setting_count,
                                 rfc_frame **out_frames,
                                 size_t *out_frame_count,
                                 rfc_metadata *out_meta) {
    (void)raw;
    (void)settings;
    (void)setting_count;
    rf_options opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = (uint32_t)sizeof(opt);
    opt.threads = rfc_exec_threads(exec);
    if (opt.threads > 9u) {
        opt.threads = 9u;
    }

    rf_pixels px;
    memset(&px, 0, sizeof(px));
    rf_error err = {0, NULL};
    if (rf_decode_image(bytes, size, &opt, &px, &err) != RF_OK) {
        rf_free(px.data);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_frame *arr = (rfc_frame *)exec->allocator->alloc(exec->allocator->ctx,
                                                         sizeof(rfc_frame));
    if (!arr) {
        rf_free(px.data);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, sizeof(*arr));
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, px.width, px.height,
                      px.stride, 8u};
    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                         NULL, 0u, NULL, 0u};
    rfc_status st = rfc_exec_alloc_frame(exec, &arr[0], &lay, &meta);
    if (st != RFC_OK) {
        rf_free(px.data);
        exec->allocator->free(exec->allocator->ctx, arr);
        return st;
    }
    for (uint32_t y = 0; y < px.height; y++) {
        memcpy(arr[0].data + (size_t)y * arr[0].layout.stride_bytes,
               px.data + (size_t)y * px.stride, (size_t)px.width * 3u);
    }
    rf_free(px.data);

    *out_frames = arr;
    *out_frame_count = 1u;
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

/* Synthetic test codec harness configurable across BMP/PNG/TIFF/WEBP/JPEG/JXL. */
typedef struct mock_codec_ctrl {
    ex_mutex_t lock;
    ex_cond_t cond;
    uint32_t open_count;
    uint32_t close_count;
    uint32_t decode_calls;
    uint32_t encode_calls;
    int corrupt_next_encode;
    int flip_next_encoded_byte;
    int drop_icc_on_encode;
    int alter_orientation_on_encode;
    int retain_forbidden_meta_on_encode;
    rfc_status fail_next_probe_status;
    rfc_status fail_next_decode_status;
    int fail_on_verify_decode_only;
    int block_on_verify_decode;
    int verify_decode_entered;
    int release_verify_decode;
    int force_tiny_ratio_bomb;
    uint32_t stream_unknown_frames;
    uint32_t stream_alloc_successes;
    rfc_sample_format sample_fmt;
    rfc_alpha_mode alpha_mode;
    uint32_t width;
    uint32_t height;
    size_t extra_stride;
    rfc_orientation orientation;
    uint32_t duration_ms;
    const uint8_t *icc;
    size_t icc_len;
    rfc_jpeg_coeff_image *src_coeffs;
    rfc_jpeg_coeff_image *dst_coeffs;
    const rfc_metadata *coeff_meta;
    const uint8_t *recon_bytes;
    size_t recon_len;
    const rfc_metadata *recon_meta;
    int supply_coeffs;
    int supply_recon;
    uint32_t export_calls;
    uint32_t fail_first_export;
} mock_codec_ctrl;

static mock_codec_ctrl g_ctrl;

typedef struct mock_sess_obj {
    const rfc_exec_context *exec;
    rfc_format_id fmt;
    int cancelled;
} mock_sess_obj;

static rfc_status mock_open(const rfc_exec_context *exec,
                            rfc_format_id fmt,
                            void **out_sess) {
    mock_sess_obj *s = (mock_sess_obj *)exec->allocator->alloc(
        exec->allocator->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    s->exec = exec;
    s->fmt = fmt;
    s->cancelled = 0;
    ex_mutex_lock(&g_ctrl.lock);
    g_ctrl.open_count++;
    ex_mutex_unlock(&g_ctrl.lock);
    *out_sess = s;
    return RFC_OK;
}

static void mock_close(void *raw) {
    mock_sess_obj *s = (mock_sess_obj *)raw;
    if (s) {
        const rfc_allocator *a = s->exec->allocator;
        a->free(a->ctx, s);
        ex_mutex_lock(&g_ctrl.lock);
        g_ctrl.close_count++;
        ex_mutex_unlock(&g_ctrl.lock);
    }
}

static void mock_cancel(void *raw) {
    mock_sess_obj *s = (mock_sess_obj *)raw;
    ex_mutex_lock(&g_ctrl.lock);
    if (s) {
        s->cancelled = 1;
    }
    g_ctrl.release_verify_decode = 1;
    ex_cond_broadcast(&g_ctrl.cond);
    ex_mutex_unlock(&g_ctrl.lock);
}

static rfc_status mock_probe(void *raw,
                             const rfc_exec_context *exec,
                             const uint8_t *bytes,
                             size_t size,
                             rfc_image_probe *out_probe) {
    mock_sess_obj *s = (mock_sess_obj *)raw;
    ex_mutex_lock(&g_ctrl.lock);
    uint32_t call_idx = g_ctrl.decode_calls;
    rfc_status fst = g_ctrl.fail_next_probe_status;
    int only_ver = g_ctrl.fail_on_verify_decode_only;
    int ratio_bomb = g_ctrl.force_tiny_ratio_bomb && (call_idx >= 1u);
    int unknown_stream = (g_ctrl.stream_unknown_frames > 0u);
    ex_mutex_unlock(&g_ctrl.lock);
    (void)s;

    if (fst != RFC_OK && (!only_ver || call_idx >= 1u)) {
        return fst;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (!ratio_bomb && bytes && size >= 32u && bytes[0] == 'M' &&
        bytes[1] == 'C' && bytes[2] == 'K') {
        uint32_t w = read_u32(bytes + 4u);
        uint32_t h = read_u32(bytes + 8u);
        rfc_sample_format sf = (rfc_sample_format)read_u32(bytes + 12u);
        rfc_alpha_mode am = (rfc_alpha_mode)read_u32(bytes + 16u);
        uint32_t bps = read_u32(bytes + 20u);
        rfc_orientation ori = (rfc_orientation)read_u32(bytes + 24u);
        size_t icc_len = (size_t)read_u32(bytes + 28u);
        uint32_t dur = (uint32_t)bytes[3];
        size_t row_bytes = (size_t)w * rfc_sample_bytes(sf);
        out_probe->first_frame.format = sf;
        out_probe->first_frame.alpha = am;
        out_probe->first_frame.width = w;
        out_probe->first_frame.height = h;
        out_probe->first_frame.stride_bytes = row_bytes;
        out_probe->first_frame.bits_per_sample = bps;
        out_probe->frame_count = 1u;
        out_probe->total_pixels = (uint64_t)w * (uint64_t)h;
        out_probe->total_decoded_bytes = (uint64_t)row_bytes * (uint64_t)h;
        if (icc_len > 0u && 32u + icc_len <= size) {
            uint8_t *cp = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx,
                                                            icc_len);
            if (!cp) {
                return RFC_ERR_OUT_OF_MEMORY;
            }
            memcpy(cp, bytes + 32u, icc_len);
            out_probe->container_meta.icc = cp;
            out_probe->container_meta.icc_len = icc_len;
        }
        out_probe->container_meta.orientation = ori;
        out_probe->container_meta.duration_ms = dur;
        return RFC_OK;
    }

    uint32_t w = ratio_bomb ? 256u : g_ctrl.width;
    uint32_t h = ratio_bomb ? 256u : g_ctrl.height;
    rfc_sample_format sf = g_ctrl.sample_fmt;
    size_t row_bytes = (size_t)w * rfc_sample_bytes(sf) +
                       (ratio_bomb ? 0u : g_ctrl.extra_stride);
    out_probe->first_frame.format = sf;
    out_probe->first_frame.alpha = g_ctrl.alpha_mode;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.stride_bytes = row_bytes;
    out_probe->first_frame.bits_per_sample = default_bps_of(sf);
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)w * (uint64_t)h;
    out_probe->total_decoded_bytes = (uint64_t)row_bytes * (uint64_t)h;
    if (ratio_bomb || unknown_stream) {
        /* Omit exact aggregate totals so streaming check_next_frame enforces limits. */
        out_probe->frame_count = 0u;
        out_probe->total_pixels = 0u;
        out_probe->total_decoded_bytes = 0u;
    }
    if (g_ctrl.icc && g_ctrl.icc_len > 0u) {
        uint8_t *cp = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx,
                                                        g_ctrl.icc_len);
        if (!cp) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(cp, g_ctrl.icc, g_ctrl.icc_len);
        out_probe->container_meta.icc = cp;
        out_probe->container_meta.icc_len = g_ctrl.icc_len;
    }
    out_probe->container_meta.orientation = g_ctrl.orientation;
    out_probe->container_meta.duration_ms = g_ctrl.duration_ms;
    return RFC_OK;
}

static rfc_status mock_encode(void *raw,
                              const rfc_exec_context *exec,
                              const rfc_frame *frames,
                              size_t frame_count,
                              const rfc_metadata *meta,
                              const rfc_setting_value *settings,
                              size_t setting_count,
                              rfc_encoded *out_encoded) {
    mock_sess_obj *s = (mock_sess_obj *)raw;
    if (!frames || frame_count != 1u || !out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_metadata_policy pol = RFC_META_POLICY_PRESERVE;
    for (size_t i = 0u; i < setting_count; i++) {
        if (strcmp(settings[i].id, "metadata") == 0) {
            rfc_metadata_policy_parse(settings[i].value, &pol);
        }
    }

    ex_mutex_lock(&g_ctrl.lock);
    g_ctrl.encode_calls++;
    int corrupt = g_ctrl.corrupt_next_encode;
    int flip_byte = g_ctrl.flip_next_encoded_byte;
    int drop_icc = g_ctrl.drop_icc_on_encode;
    int alter_ori = g_ctrl.alter_orientation_on_encode;
    int retain_forbidden = g_ctrl.retain_forbidden_meta_on_encode;
    int tiny_bomb = g_ctrl.force_tiny_ratio_bomb;
    ex_mutex_unlock(&g_ctrl.lock);

    const rfc_frame *f = &frames[0];
    size_t row_bytes =
        (size_t)f->layout.width * rfc_sample_bytes(f->layout.format);
    size_t pix_bytes = row_bytes * (size_t)f->layout.height;

    const rfc_metadata *eff_m = meta ? meta : &f->metadata;
    uint32_t cap_mask = rfc_format_metadata_caps(s->fmt);

    uint8_t store_icc[256];
    size_t store_icc_len = 0u;
    rfc_orientation store_ori = RFC_ORIENTATION_NORMAL;
    uint32_t store_dur = 0u;

    if (pol == RFC_META_POLICY_PRESERVE) {
        if ((cap_mask & RFC_META_ITEM_ICC) != 0u && eff_m && eff_m->icc &&
            eff_m->icc_len > 0u && !drop_icc) {
            store_icc_len = eff_m->icc_len <= sizeof(store_icc)
                                ? eff_m->icc_len
                                : sizeof(store_icc);
            memcpy(store_icc, eff_m->icc, store_icc_len);
        }
        if ((cap_mask & RFC_META_ITEM_ORIENTATION) != 0u && eff_m &&
            eff_m->orientation != RFC_ORIENTATION_UNSPECIFIED) {
            store_ori = eff_m->orientation;
        }
        if (alter_ori) {
            store_ori = RFC_ORIENTATION_ROTATE_180;
        }
        if ((cap_mask & RFC_META_ITEM_FRAME_TIMING) != 0u && eff_m) {
            store_dur = eff_m->duration_ms;
        }
    } else if (retain_forbidden) {
        if (eff_m && eff_m->icc && eff_m->icc_len > 0u) {
            store_icc_len = eff_m->icc_len <= sizeof(store_icc)
                                ? eff_m->icc_len
                                : sizeof(store_icc);
            memcpy(store_icc, eff_m->icc, store_icc_len);
        } else {
            store_icc[0] = 0x55u;
            store_icc_len = 1u;
        }
        store_ori = RFC_ORIENTATION_ROTATE_90;
        store_dur = eff_m ? eff_m->duration_ms : 0u;
    }

    /* Simple mock container: 32-byte header + icc + packed pixels. */
    size_t total = tiny_bomb ? 16u : (32u + store_icc_len + pix_bytes);
    uint8_t *buf = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx,
                                                     total);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(buf, 0, total);
    buf[0] = 'M';
    buf[1] = 'C';
    buf[2] = 'K';
    buf[3] = 1u;
    if (!tiny_bomb) {
        write_u32(buf + 4u, f->layout.width);
        write_u32(buf + 8u, f->layout.height);
        write_u32(buf + 12u, (uint32_t)f->layout.format);
        write_u32(buf + 16u, (uint32_t)f->layout.alpha);
        write_u32(buf + 20u, f->layout.bits_per_sample);
        write_u32(buf + 24u, (uint32_t)store_ori);
        write_u32(buf + 28u, (uint32_t)store_icc_len);
        if (store_icc_len > 0u) {
            memcpy(buf + 32u, store_icc, store_icc_len);
        }
        uint8_t *dst = buf + 32u + store_icc_len;
        for (uint32_t y = 0; y < f->layout.height; y++) {
            memcpy(dst + (size_t)y * row_bytes,
                   f->data + (size_t)y * f->layout.stride_bytes, row_bytes);
        }
        if (corrupt && pix_bytes > 0u) {
            dst[0] ^= 0x01u;
        }
        if (flip_byte && total > 32u) {
            buf[32u] ^= 0x80u;
        }
        /* Stash duration cleanly in reserved byte 31 if fitting. */
        buf[3] = (uint8_t)(store_dur & 0xffu);
    }

    out_encoded->data = buf;
    out_encoded->len = total;
    out_encoded->allocator = exec->allocator;
    return RFC_OK;
}

static rfc_status mock_decode(void *raw,
                              const rfc_exec_context *exec,
                              const uint8_t *bytes,
                              size_t size,
                              const rfc_setting_value *settings,
                              size_t setting_count,
                              rfc_frame **out_frames,
                              size_t *out_frame_count,
                              rfc_metadata *out_meta) {
    mock_sess_obj *s = (mock_sess_obj *)raw;
    (void)settings;
    (void)setting_count;

    ex_mutex_lock(&g_ctrl.lock);
    uint32_t call_idx = g_ctrl.decode_calls++;
    int is_verify_call = (call_idx >= 1u);
    rfc_status fdec = g_ctrl.fail_next_decode_status;
    int only_ver = g_ctrl.fail_on_verify_decode_only;
    int block_ver = g_ctrl.block_on_verify_decode;
    int tiny_bomb = g_ctrl.force_tiny_ratio_bomb && is_verify_call;
    uint32_t unknown_frames = g_ctrl.stream_unknown_frames;
    if (is_verify_call && block_ver) {
        g_ctrl.verify_decode_entered = 1;
        ex_cond_broadcast(&g_ctrl.cond);
        while (!g_ctrl.release_verify_decode && !s->cancelled &&
               !rfc_exec_is_cancelled(exec)) {
            ex_cond_wait(&g_ctrl.cond, &g_ctrl.lock);
        }
    }
    ex_mutex_unlock(&g_ctrl.lock);

    if (rfc_exec_is_cancelled(exec) || s->cancelled) {
        return RFC_ERR_CANCELLED;
    }
    if (fdec != RFC_OK && (!only_ver || is_verify_call)) {
        return fdec;
    }

    if (tiny_bomb) {
        rfc_layout big = {g_ctrl.sample_fmt, g_ctrl.alpha_mode, 256u, 256u,
                          (size_t)256u * rfc_sample_bytes(g_ctrl.sample_fmt),
                          default_bps_of(g_ctrl.sample_fmt)};
        rfc_status cs = rfc_exec_check_next_frame(exec, &big, NULL);
        return cs == RFC_OK ? RFC_ERR_LIMIT_EXCEEDED : cs;
    }

    if (unknown_frames > 0u) {
        rfc_frame *uarr = (rfc_frame *)exec->allocator->alloc(
            exec->allocator->ctx, (size_t)unknown_frames * sizeof(rfc_frame));
        if (!uarr) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memset(uarr, 0, (size_t)unknown_frames * sizeof(rfc_frame));
        rfc_layout ulay = {
            g_ctrl.sample_fmt, g_ctrl.alpha_mode, g_ctrl.width, g_ctrl.height,
            (size_t)g_ctrl.width * rfc_sample_bytes(g_ctrl.sample_fmt),
            default_bps_of(g_ctrl.sample_fmt)};
        rfc_metadata umeta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                              NULL, 0u, NULL, 0u};
        for (uint32_t fi = 0u; fi < unknown_frames; fi++) {
            rfc_status ast =
                rfc_exec_alloc_frame(exec, &uarr[fi], &ulay, &umeta);
            if (ast != RFC_OK) {
                for (uint32_t k = 0u; k < fi; k++) {
                    rfc_frame_free(&uarr[k]);
                }
                exec->allocator->free(exec->allocator->ctx, uarr);
                return ast;
            }
            fill_deterministic_pixels(&uarr[fi]);
            ex_mutex_lock(&g_ctrl.lock);
            g_ctrl.stream_alloc_successes++;
            ex_mutex_unlock(&g_ctrl.lock);
        }
        *out_frames = uarr;
        *out_frame_count = (size_t)unknown_frames;
        memset(out_meta, 0, sizeof(*out_meta));
        out_meta->orientation = RFC_ORIENTATION_NORMAL;
        return RFC_OK;
    }

    rfc_frame *arr = (rfc_frame *)exec->allocator->alloc(exec->allocator->ctx,
                                                         sizeof(rfc_frame));
    if (!arr) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, sizeof(*arr));

    if (size >= 32u && bytes[0] == 'M' && bytes[1] == 'C' && bytes[2] == 'K') {
        uint32_t w = read_u32(bytes + 4u);
        uint32_t h = read_u32(bytes + 8u);
        rfc_sample_format sf = (rfc_sample_format)read_u32(bytes + 12u);
        rfc_alpha_mode am = (rfc_alpha_mode)read_u32(bytes + 16u);
        uint32_t bps = read_u32(bytes + 20u);
        rfc_orientation ori = (rfc_orientation)read_u32(bytes + 24u);
        size_t icc_len = (size_t)read_u32(bytes + 28u);
        uint32_t dur = (uint32_t)bytes[3];
        const uint8_t *icc = (icc_len > 0u && 32u + icc_len <= size)
                                 ? (bytes + 32u)
                                 : NULL;
        rfc_layout lay = {sf, am, w, h, (size_t)w * rfc_sample_bytes(sf), bps};
        rfc_metadata meta = {icc, icc_len, ori, dur, {0}, NULL, 0u, NULL, 0u};
        rfc_status st = rfc_exec_alloc_frame(exec, &arr[0], &lay, &meta);
        if (st != RFC_OK) {
            exec->allocator->free(exec->allocator->ctx, arr);
            return st;
        }
        size_t row_bytes = (size_t)w * rfc_sample_bytes(sf);
        memcpy(arr[0].data, bytes + 32u + icc_len, row_bytes * (size_t)h);
        *out_frames = arr;
        *out_frame_count = 1u;
        memset(out_meta, 0, sizeof(*out_meta));
        out_meta->icc = arr[0].metadata.icc;
        out_meta->icc_len = arr[0].metadata.icc_len;
        out_meta->orientation = ori;
        out_meta->duration_ms = dur;
        return RFC_OK;
    }

    rfc_layout lay = {
        g_ctrl.sample_fmt,
        g_ctrl.alpha_mode,
        g_ctrl.width,
        g_ctrl.height,
        (size_t)g_ctrl.width * rfc_sample_bytes(g_ctrl.sample_fmt) +
            g_ctrl.extra_stride,
        default_bps_of(g_ctrl.sample_fmt)};
    rfc_metadata meta = {g_ctrl.icc, g_ctrl.icc_len, g_ctrl.orientation,
                         g_ctrl.duration_ms, {0}, NULL, 0u, NULL, 0u};
    rfc_status st = rfc_exec_alloc_frame(exec, &arr[0], &lay, &meta);
    if (st != RFC_OK) {
        exec->allocator->free(exec->allocator->ctx, arr);
        return st;
    }
    fill_deterministic_pixels(&arr[0]);

    *out_frames = arr;
    *out_frame_count = 1u;
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->icc = arr[0].metadata.icc;
    out_meta->icc_len = arr[0].metadata.icc_len;
    out_meta->orientation = g_ctrl.orientation;
    out_meta->duration_ms = g_ctrl.duration_ms;
    return RFC_OK;
}

#define DEFINE_MOCK_FORMAT(fn_prefix, fmt_enum)                                \
    static rfc_status fn_prefix##_open(const rfc_exec_context *e, void **o) {  \
        return mock_open(e, fmt_enum, o);                                      \
    }                                                                          \
    static rfc_session_adapter fn_prefix##_adp = {                             \
        RFC_CODEC_ABI_VERSION,                                                 \
        fmt_enum,                                                              \
        fn_prefix##_open,                                                      \
        mock_close,                                                            \
        mock_probe,                                                            \
        mock_encode,                                                           \
        mock_decode,                                                           \
        mock_cancel,                                                           \
        NULL,                                                                  \
    };

DEFINE_MOCK_FORMAT(m_bmp, RFC_FORMAT_BMP)
DEFINE_MOCK_FORMAT(m_png, RFC_FORMAT_PNG)
DEFINE_MOCK_FORMAT(m_webp, RFC_FORMAT_WEBP)
DEFINE_MOCK_FORMAT(m_tiff, RFC_FORMAT_TIFF)
DEFINE_MOCK_FORMAT(m_jpeg, RFC_FORMAT_JPEG)
DEFINE_MOCK_FORMAT(m_jxl, RFC_FORMAT_JPEG_XL)

static rfc_session_adapter g_rf_adp = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_RASTERFOLD,
    rf_sess_open,
    rf_sess_close,
    rf_sess_probe,
    rf_sess_encode,
    rf_sess_decode,
    rf_sess_cancel,
    NULL,
};

static rfc_status stage_export_count(rfc_job *job,
                                     const rfc_exec_context *exec,
                                     void *user_ctx) {
    (void)job;
    (void)exec;
    (void)user_ctx;
    ex_mutex_lock(&g_ctrl.lock);
    if (g_ctrl.fail_first_export) {
        g_ctrl.fail_first_export = 0;
        ex_mutex_unlock(&g_ctrl.lock);
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    g_ctrl.export_calls++;
    ex_mutex_unlock(&g_ctrl.lock);
    return RFC_OK;
}

static rfc_status stage_encode_with_verify_artifacts(
    rfc_job *job,
    const rfc_exec_context *exec,
    void *user_ctx) {
    (void)user_ctx;
    static const uint8_t jxl_stub[12] = {0xff, 0x0a, 'J', 'X', 'L', 0x42,
                                         0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t *buf = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx,
                                                     sizeof(jxl_stub));
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(buf, jxl_stub, sizeof(jxl_stub));
    rfc_encoded enc = {buf, sizeof(jxl_stub), exec->allocator};
    rfc_status st = rfc_job_set_encoded_output(job, &enc);
    if (st != RFC_OK) {
        rfc_encoded_free(&enc);
        return st;
    }
    if (g_ctrl.supply_coeffs && g_ctrl.src_coeffs && g_ctrl.dst_coeffs) {
        st = rfc_job_set_verified_jpeg_coeffs(
            job, g_ctrl.src_coeffs, g_ctrl.dst_coeffs, g_ctrl.coeff_meta);
        if (st != RFC_OK) {
            return st;
        }
    }
    if (g_ctrl.supply_recon && g_ctrl.recon_bytes && g_ctrl.recon_len > 0u) {
        st = rfc_job_set_verified_reconstructed_bytes(
            job, g_ctrl.recon_bytes, g_ctrl.recon_len, g_ctrl.recon_meta);
        if (st != RFC_OK) {
            return st;
        }
    }
    return RFC_OK;
}

static void reset_ctrl(rfc_sample_format fmt, uint32_t w, uint32_t h) {
    memset(&g_ctrl, 0, sizeof(g_ctrl));
    ex_mutex_init(&g_ctrl.lock);
    ex_cond_init(&g_ctrl.cond);
    g_ctrl.sample_fmt = fmt;
    g_ctrl.alpha_mode = default_alpha_of(fmt);
    g_ctrl.width = w;
    g_ctrl.height = h;
    ex_mutex_lock(&g_rf_stats_lock);
    g_rf_open_calls = 0u;
    g_rf_close_calls = 0u;
    ex_mutex_unlock(&g_rf_stats_lock);
}

static void cleanup_ctrl(void) {
    ex_cond_destroy(&g_ctrl.cond);
    ex_mutex_destroy(&g_ctrl.lock);
}

static void register_all_test_adapters(void) {
    CHECK(rfc_registry_register_session(&g_rf_adp) == RFC_OK);
    CHECK(rfc_registry_register_session(&m_bmp_adp) == RFC_OK);
    CHECK(rfc_registry_register_session(&m_png_adp) == RFC_OK);
    CHECK(rfc_registry_register_session(&m_webp_adp) == RFC_OK);
    CHECK(rfc_registry_register_session(&m_tiff_adp) == RFC_OK);
    CHECK(rfc_registry_register_session(&m_jpeg_adp) == RFC_OK);
    CHECK(rfc_registry_register_session(&m_jxl_adp) == RFC_OK);
}

static void unregister_all_test_adapters(void) {
    rfc_registry_unregister_session(RFC_FORMAT_RASTERFOLD);
    rfc_registry_unregister_session(RFC_FORMAT_BMP);
    rfc_registry_unregister_session(RFC_FORMAT_PNG);
    rfc_registry_unregister_session(RFC_FORMAT_WEBP);
    rfc_registry_unregister_session(RFC_FORMAT_TIFF);
    rfc_registry_unregister_session(RFC_FORMAT_JPEG);
    rfc_registry_unregister_session(RFC_FORMAT_JPEG_XL);
}

/* ----------------------------------------------------------------
 * 2. Helper: write .fold file and RFC-FRAMES-V1 hash for Python cross-check
 * ---------------------------------------------------------------- */

typedef struct fold_capture {
    uint8_t *bytes;
    size_t len;
} fold_capture;

static rfc_status export_capture_fold(rfc_job *job,
                                      const rfc_exec_context *exec,
                                      void *user_ctx) {
    (void)exec;
    fold_capture *cap = (fold_capture *)user_ctx;
    const rfc_encoded *enc = rfc_job_encoded_output(job);
    CHECK(enc != NULL && enc->data != NULL && enc->len > 32u);
    cap->bytes = (uint8_t *)malloc(enc->len);
    CHECK(cap->bytes != NULL);
    memcpy(cap->bytes, enc->data, enc->len);
    cap->len = enc->len;
    return RFC_OK;
}

static void run_fold_roundtrip_export(const char *out_fold_path,
                                      const char *out_hash_path) {
    reset_ctrl(RFC_SAMPLE_RGB8, 17u, 13u);
    g_ctrl.extra_stride = 13u; /* Non-tight input stride verifies padding strip */

    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    uint8_t src_stub[16] = {1, 2, 3, 4};
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 17u, 13u,
                      17u * 3u + 13u, 8u};
    rfc_metadata smeta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                          NULL, 0u, NULL, 0u};
    fold_capture cap = {NULL, 0u};

    rfc_job_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.source_format = RFC_FORMAT_BMP;
    spec.target_format = RFC_FORMAT_RASTERFOLD;
    spec.source_bytes = src_stub;
    spec.source_size = sizeof(src_stub);
    spec.source_layout = &lay;
    spec.source_meta = &smeta;
    spec.expected_working_set_bytes = 256u * 1024u;
    spec.has_stage_ops = 1;
    spec.stage_ops.on_export = export_capture_fold;
    spec.stage_ops.user_ctx = &cap;

    rfc_job *job = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);

    rfc_job_snapshot snap;
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED);
    CHECK(snap.status == RFC_OK);
    CHECK(snap.budget.current_allocated_bytes == 0u);

    rfc_verify_report rep;
    CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
    CHECK(rep.valid == 1);
    CHECK(rep.exactness == RFC_EXACT_PIXEL_EXACT);
    CHECK(memcmp(rep.source_hash, rep.verified_hash, 32u) == 0);
    CHECK(g_rf_open_calls == 2u && g_rf_close_calls == 2u);

    if (out_fold_path && out_hash_path) {
        FILE *ff = fopen(out_fold_path, "wb");
        CHECK(ff != NULL);
        CHECK(fwrite(cap.bytes, 1u, cap.len, ff) == cap.len);
        fclose(ff);

        FILE *hf = fopen(out_hash_path, "wb");
        CHECK(hf != NULL);
        for (size_t i = 0u; i < 32u; i++) {
            fprintf(hf, "%02x", rep.verified_hash[i]);
        }
        fputc('\n', hf);
        fclose(hf);
    }

    free(cap.bytes);
    rfc_job_release(job);
    rfc_scheduler_destroy(sched);
    cleanup_ctrl();
}

/* ----------------------------------------------------------------
 * 3. Tests
 * ---------------------------------------------------------------- */

static void test_sha256_and_digest(void) {
    static const uint8_t h_empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
        0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
        0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    static const uint8_t h_abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    static const uint8_t h_448[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
        0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
        0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1};

    uint8_t out[32];
    rfc_bytes_hash_sha256(NULL, 0u, out);
    CHECK(memcmp(out, h_empty, 32u) == 0);
    rfc_bytes_hash_sha256((const uint8_t *)"abc", 3u, out);
    CHECK(memcmp(out, h_abc, 32u) == 0);
    const char *m448 =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    rfc_bytes_hash_sha256((const uint8_t *)m448, strlen(m448), out);
    CHECK(memcmp(out, h_448, 32u) == 0);

    /* Stride invariance & junk row-tail rejection isolation. */
    alloc_tracker tr;
    rfc_allocator al;
    init_tracked_alloc(&tr, &al);
    rfc_frame f1, f2;
    fill_deterministic_frame(&f1, 7u, 5u, RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 12u,
                             0u, 0u, &al);
    fill_deterministic_frame(&f2, 7u, 5u, RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 12u,
                             24u, 0u, &al);
    uint8_t d1[32], d2[32];
    CHECK(rfc_frames_hash_sha256(&f1, 1u, d1) == RFC_OK);
    CHECK(rfc_frames_hash_sha256(&f2, 1u, d2) == RFC_OK);
    CHECK(memcmp(d1, d2, 32u) == 0);
    CHECK(rfc_verify_frames_equal(&f1, 1u, &f2, 1u, NULL) == RFC_OK);

    /* Nonzero low padding bit must fail both hash and frame comparison. */
    uint16_t corrupted = read_u16(f2.data + 2u) | 0x0003u;
    write_u16(f2.data + 2u, corrupted);
    CHECK(rfc_frames_hash_sha256(&f2, 1u, d2) == RFC_ERR_VERIFY_MISMATCH);
    rfc_sample_mismatch mm;
    CHECK(rfc_verify_frames_equal(&f1, 1u, &f2, 1u, &mm) ==
          RFC_ERR_VERIFY_MISMATCH);
    CHECK(mm.kind == RFC_MISMATCH_LOW_PADDING_BITS);
    CHECK(mm.frame_index == 0u && mm.x == 0u && mm.y == 0u &&
          mm.channel == 1u);

    rfc_frame_free(&f1);
    rfc_frame_free(&f2);
    CHECK(tr.live == 0u);
    printf("exactness: sha256_and_digest ok\n");
}

static void test_sample_formats_and_mutations(void) {
    alloc_tracker tr;
    rfc_allocator al;
    init_tracked_alloc(&tr, &al);

    static const rfc_sample_format all_fmts[10] = {
        RFC_SAMPLE_GRAY8,     RFC_SAMPLE_GRAY16,    RFC_SAMPLE_RGB8,
        RFC_SAMPLE_RGBA8,     RFC_SAMPLE_RGB16,     RFC_SAMPLE_RGBA16,
        RFC_SAMPLE_RGB_HALF,  RFC_SAMPLE_RGBA_HALF, RFC_SAMPLE_RGB_FLOAT,
        RFC_SAMPLE_RGBA_FLOAT};

    for (size_t i = 0u; i < 10u; i++) {
        rfc_sample_format sf = all_fmts[i];
        uint32_t bps = default_bps_of(sf);
        rfc_alpha_mode am = default_alpha_of(sf);
        rfc_frame a, b;
        fill_deterministic_frame(&a, 6u, 5u, sf, am, bps, 0u, 0u, &al);
        fill_deterministic_frame(&b, 6u, 5u, sf, am, bps, 17u, 0u, &al);
        CHECK(rfc_verify_frames_equal(&a, 1u, &b, 1u, NULL) == RFC_OK);

        /* Flip 1 LSB at (2, 1, ch 0). */
        size_t bpp = rfc_sample_bytes(sf);
        uint32_t ch = channel_count_of(sf);
        size_t ch_b = bpp / ch;
        uint8_t *px = b.data + (size_t)1u * b.layout.stride_bytes + 2u * bpp;
        px[0] ^= 0x01u;
        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&a, 1u, &b, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        int is_flt = (sf == RFC_SAMPLE_RGB_HALF || sf == RFC_SAMPLE_RGBA_HALF ||
                      sf == RFC_SAMPLE_RGB_FLOAT ||
                      sf == RFC_SAMPLE_RGBA_FLOAT);
        CHECK(mm.kind == (is_flt ? RFC_MISMATCH_FLOAT_BIT_PATTERN
                                 : RFC_MISMATCH_SAMPLE_VALUE));
        CHECK(mm.x == 2u && mm.y == 1u && mm.channel == 0u);
        (void)ch_b;

        rfc_frame_free(&a);
        rfc_frame_free(&b);
    }

    /* Transparent RGB zeroing checks on RGBA8 and RGBA_FLOAT. */
    {
        rfc_frame a, b;
        fill_deterministic_frame(&a, 3u, 2u, RFC_SAMPLE_RGBA8,
                                 RFC_ALPHA_STRAIGHT, 8u, 0u, 0u, &al);
        fill_deterministic_frame(&b, 3u, 2u, RFC_SAMPLE_RGBA8,
                                 RFC_ALPHA_STRAIGHT, 8u, 0u, 0u, &al);
        uint8_t *pa = a.data + 4u;
        uint8_t *pb = b.data + 4u;
        pa[0] = 120u;
        pa[1] = 45u;
        pa[2] = 200u;
        pa[3] = 0u;
        pb[0] = 0u;
        pb[1] = 0u;
        pb[2] = 0u;
        pb[3] = 0u;
        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&a, 1u, &b, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_TRANSPARENT_RGB);
        CHECK(mm.x == 1u && mm.y == 0u && mm.channel == 0u);
        rfc_frame_free(&a);
        rfc_frame_free(&b);

        fill_deterministic_frame(&a, 2u, 2u, RFC_SAMPLE_RGBA_FLOAT,
                                 RFC_ALPHA_STRAIGHT, 32u, 0u, 0u, &al);
        fill_deterministic_frame(&b, 2u, 2u, RFC_SAMPLE_RGBA_FLOAT,
                                 RFC_ALPHA_STRAIGHT, 32u, 0u, 0u, &al);
        write_u32(a.data + 0u, 0x3f800000u);
        write_u32(a.data + 4u, 0x3f000000u);
        write_u32(a.data + 8u, 0x3e800000u);
        write_u32(a.data + 12u, 0x00000000u);
        write_u32(b.data + 0u, 0x00000000u);
        write_u32(b.data + 4u, 0x00000000u);
        write_u32(b.data + 8u, 0x00000000u);
        write_u32(b.data + 12u, 0x00000000u);
        CHECK(rfc_verify_frames_equal(&a, 1u, &b, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_TRANSPARENT_RGB);
        rfc_frame_free(&a);
        rfc_frame_free(&b);
    }

    /* Multi-frame duration mismatch on frame 2 (index 1). */
    {
        rfc_frame seq1[2], seq2[2];
        fill_deterministic_frame(&seq1[0], 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 40u, &al);
        fill_deterministic_frame(&seq1[1], 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 80u, &al);
        fill_deterministic_frame(&seq2[0], 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 40u, &al);
        fill_deterministic_frame(&seq2[1], 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 90u, &al);
        CHECK(rfc_verify_frames_equal(seq1, 2u, seq1, 2u, NULL) == RFC_OK);
        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(seq1, 2u, seq2, 2u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_DURATION_MS && mm.frame_index == 1u);
        CHECK(mm.expected_bits == 80u && mm.actual_bits == 90u);
        for (int i = 0; i < 2; i++) {
            rfc_frame_free(&seq1[i]);
            rfc_frame_free(&seq2[i]);
        }
    }

    /* OpenEXR -0.0 vs +0.0 and distinct NaN payloads on HALF and FLOAT. */
    {
        rfc_frame ha, hb;
        fill_deterministic_frame(&ha, 2u, 2u, RFC_SAMPLE_RGB_HALF,
                                 RFC_ALPHA_NONE, 16u, 0u, 0u, &al);
        fill_deterministic_frame(&hb, 2u, 2u, RFC_SAMPLE_RGB_HALF,
                                 RFC_ALPHA_NONE, 16u, 0u, 0u, &al);
        write_u16(ha.data + 2u, 0x8000u); /* -0.0 */
        write_u16(hb.data + 2u, 0x0000u); /* +0.0 */
        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&ha, 1u, &hb, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_FLOAT_BIT_PATTERN);
        CHECK(mm.expected_bits == 0x8000u && mm.actual_bits == 0x0000u);

        write_u16(ha.data + 2u, 0x7e01u); /* NaN payload 1 */
        write_u16(hb.data + 2u, 0x7e02u); /* NaN payload 2 */
        CHECK(rfc_verify_frames_equal(&ha, 1u, &hb, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_FLOAT_BIT_PATTERN);
        CHECK(mm.expected_bits == 0x7e01u && mm.actual_bits == 0x7e02u);
        rfc_frame_free(&ha);
        rfc_frame_free(&hb);

        rfc_frame fa, fb;
        fill_deterministic_frame(&fa, 2u, 2u, RFC_SAMPLE_RGBA_FLOAT,
                                 RFC_ALPHA_STRAIGHT, 32u, 0u, 0u, &al);
        fill_deterministic_frame(&fb, 2u, 2u, RFC_SAMPLE_RGBA_FLOAT,
                                 RFC_ALPHA_STRAIGHT, 32u, 0u, 0u, &al);
        write_u32(fa.data + 4u, 0x80000000u);
        write_u32(fb.data + 4u, 0x00000000u);
        CHECK(rfc_verify_frames_equal(&fa, 1u, &fb, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_FLOAT_BIT_PATTERN);

        write_u32(fa.data + 4u, 0x7fc00001u);
        write_u32(fb.data + 4u, 0x7fc00002u);
        CHECK(rfc_verify_frames_equal(&fa, 1u, &fb, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_FLOAT_BIT_PATTERN);
        rfc_frame_free(&fa);
        rfc_frame_free(&fb);
    }

    CHECK(tr.live == 0u);
    printf("exactness: sample_formats_and_mutations ok\n");
}

static void init_sample_jpeg_coeffs(rfc_jpeg_coeff_image *img,
                                    int16_t *y_blocks,
                                    int16_t *cb_blocks,
                                    int16_t *cr_blocks) {
    memset(img, 0, sizeof(*img));
    img->width = 16u;
    img->height = 16u;
    img->progressive = 1;
    img->arithmetic_coding = 0;
    img->baseline = 0;
    img->component_count = 3u;

    for (size_t i = 0u; i < 4u * 64u; i++) {
        y_blocks[i] = (int16_t)((int)(i % 31u) - 15);
    }
    for (size_t i = 0u; i < 64u; i++) {
        cb_blocks[i] = (int16_t)((int)(i % 13u) - 6);
        cr_blocks[i] = (int16_t)((int)(i % 17u) - 8);
    }

    img->components[0] = (rfc_jpeg_component_spec){1u, 2u, 2u, 0u, 0u, 0u, 2u,
                                                   2u, y_blocks};
    img->components[1] = (rfc_jpeg_component_spec){2u, 1u, 1u, 1u, 1u, 1u, 1u,
                                                   1u, cb_blocks};
    img->components[2] = (rfc_jpeg_component_spec){3u, 1u, 1u, 1u, 1u, 1u, 1u,
                                                   1u, cr_blocks};
    img->quant_tables[0].populated = 1;
    img->quant_tables[1].populated = 1;
    for (size_t i = 0u; i < 64u; i++) {
        img->quant_tables[0].values[i] = (uint16_t)(8u + i);
        img->quant_tables[1].values[i] = (uint16_t)(16u + i);
    }
}

static void test_jpeg_coefficients(void) {
    int16_t y1[4 * 64], cb1[64], cr1[64];
    int16_t y2[4 * 64], cb2[64], cr2[64];
    rfc_jpeg_coeff_image a, b;
    init_sample_jpeg_coeffs(&a, y1, cb1, cr1);
    init_sample_jpeg_coeffs(&b, y2, cb2, cr2);
    CHECK(rfc_verify_jpeg_coefficients(&a, &b, NULL) == RFC_OK);

    rfc_sample_mismatch mm;
    /* Identical DCT blocks, mutated dimensions. */
    b.width = 15u;
    CHECK(rfc_verify_jpeg_coefficients(&a, &b, &mm) == RFC_ERR_VERIFY_MISMATCH);
    CHECK(mm.kind == RFC_MISMATCH_JPEG_DIMENSIONS);
    b.width = 16u;

    /* Identical DCT blocks, mutated coding flags. */
    b.progressive = 0;
    CHECK(rfc_verify_jpeg_coefficients(&a, &b, &mm) == RFC_ERR_VERIFY_MISMATCH);
    CHECK(mm.kind == RFC_MISMATCH_JPEG_CODING_PARAMS);
    b.progressive = 1;

    /* Identical DCT blocks, mutated sampling factor. */
    b.components[0].h_samp_factor = 1u;
    CHECK(rfc_verify_jpeg_coefficients(&a, &b, &mm) == RFC_ERR_VERIFY_MISMATCH);
    CHECK(mm.kind == RFC_MISMATCH_JPEG_COMPONENT_SPEC);
    b.components[0].h_samp_factor = 2u;

    /* Identical DCT blocks, mutated quant table entry. */
    b.quant_tables[1].values[12] += 1u;
    CHECK(rfc_verify_jpeg_coefficients(&a, &b, &mm) == RFC_ERR_VERIFY_MISMATCH);
    CHECK(mm.kind == RFC_MISMATCH_JPEG_QUANT_TABLE);
    b.quant_tables[1].values[12] -= 1u;

    /* Single DCT coefficient difference. */
    y2[2 * 64 + 19] += 1;
    CHECK(rfc_verify_jpeg_coefficients(&a, &b, &mm) == RFC_ERR_VERIFY_MISMATCH);
    CHECK(mm.kind == RFC_MISMATCH_JPEG_DCT_COEFF);
    CHECK(mm.channel == 0u && mm.frame_index == 2u && mm.x == 19u &&
          mm.y == 0u && mm.actual_bits != mm.expected_bits);

    printf("exactness: jpeg_coefficients ok\n");
}

static void test_stage_verify_modes(void) {
    /* 1. Pixel-exact with real RasterFold round-trip + corrupted target encode refusal. */
    run_fold_roundtrip_export(NULL, NULL);
    {
        reset_ctrl(RFC_SAMPLE_RGB8, 8u, 8u);
        g_ctrl.corrupt_next_encode = 1;

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        uint8_t src[16] = {9, 8, 7, 6};
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 24u, 8u};
        rfc_metadata sm = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                           NULL, 0u, NULL, 0u};
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_PNG;
        spec.target_format = RFC_FORMAT_PNG;
        spec.source_bytes = src;
        spec.source_size = sizeof(src);
        spec.source_layout = &lay;
        spec.source_meta = &sm;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_export = stage_export_count;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED);
        CHECK(snap.status == RFC_ERR_VERIFY_MISMATCH);
        CHECK(snap.failing_stage == RFC_STAGE_VERIFY);
        CHECK(g_ctrl.export_calls == 0u);

        rfc_verify_report rep;
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.valid == 1 && rep.mismatch.kind == RFC_MISMATCH_SAMPLE_VALUE);
        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        cleanup_ctrl();
    }

    /* 2. Byte-identical pass + 1-byte mutation fail. */
    {
        reset_ctrl(RFC_SAMPLE_RGB8, 4u, 4u);
        uint8_t src_bytes[32 + 48];
        memset(src_bytes, 0, sizeof(src_bytes));
        src_bytes[0] = 'M';
        src_bytes[1] = 'C';
        src_bytes[2] = 'K';
        src_bytes[3] = 0u;
        write_u32(src_bytes + 4u, 4u);
        write_u32(src_bytes + 8u, 4u);
        write_u32(src_bytes + 12u, (uint32_t)RFC_SAMPLE_RGB8);
        write_u32(src_bytes + 16u, (uint32_t)RFC_ALPHA_NONE);
        write_u32(src_bytes + 20u, 8u);
        write_u32(src_bytes + 24u, (uint32_t)RFC_ORIENTATION_NORMAL);
        write_u32(src_bytes + 28u, 0u);
        for (size_t i = 0u; i < 48u; i++) {
            src_bytes[32u + i] = (uint8_t)(i * 7u + 3u);
        }

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_metadata sm = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                           NULL, 0u, NULL, 0u};
        rfc_setting_value s_byte[1] = {{"exactness", "byte-identical"}};
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_PNG;
        spec.target_format = RFC_FORMAT_PNG;
        spec.source_bytes = src_bytes;
        spec.source_size = sizeof(src_bytes);
        spec.source_layout = &lay;
        spec.source_meta = &sm;
        spec.settings = s_byte;
        spec.setting_count = 1u;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_export = stage_export_count;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED);
        CHECK(g_ctrl.export_calls == 1u);
        rfc_verify_report rep;
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.exactness == RFC_EXACT_BYTE_IDENTICAL &&
              memcmp(rep.source_hash, rep.verified_hash, 32u) == 0);
        rfc_job_release(job);

        /* Flip 1 encoded byte -> refuse with RFC_MISMATCH_BYTE_HASH. */
        g_ctrl.flip_next_encoded_byte = 1;
        g_ctrl.export_calls = 0u;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED);
        CHECK(snap.status == RFC_ERR_VERIFY_MISMATCH);
        CHECK(snap.failing_stage == RFC_STAGE_VERIFY);
        CHECK(g_ctrl.export_calls == 0u);
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.mismatch.kind == RFC_MISMATCH_BYTE_HASH);
        rfc_job_release(job);

        rfc_scheduler_destroy(sched);
        cleanup_ctrl();
    }

    /* 3. Coefficient-exact via Stage 2 evidence -> missing / pass / quant mismatch. */
    {
        reset_ctrl(RFC_SAMPLE_RGB8, 16u, 16u);
        int16_t y1[4 * 64], cb1[64], cr1[64];
        int16_t y2[4 * 64], cb2[64], cr2[64];
        rfc_jpeg_coeff_image ca, cb;
        init_sample_jpeg_coeffs(&ca, y1, cb1, cr1);
        init_sample_jpeg_coeffs(&cb, y2, cb2, cr2);
        g_ctrl.src_coeffs = &ca;
        g_ctrl.dst_coeffs = &cb;

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        uint8_t jpg_src[24] = {0xff, 0xd8, 0xff, 0xe0, 1, 2, 3, 4};
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 16u, 16u, 48u, 8u};
        rfc_metadata sm = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                           NULL, 0u, NULL, 0u};
        rfc_setting_value s_coeff[1] = {{"exactness", "coefficient-exact"}};
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_JPEG;
        spec.target_format = RFC_FORMAT_JPEG_XL;
        spec.source_bytes = jpg_src;
        spec.source_size = sizeof(jpg_src);
        spec.source_layout = &lay;
        spec.source_meta = &sm;
        spec.settings = s_coeff;
        spec.setting_count = 1u;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_encode = stage_encode_with_verify_artifacts;
        spec.stage_ops.on_export = stage_export_count;

        rfc_job *job = NULL;
        g_ctrl.supply_coeffs = 0;
        g_ctrl.coeff_meta = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_NO_ADAPTER &&
              snap.failing_stage == RFC_STAGE_VERIFY);
        CHECK(g_ctrl.export_calls == 0u);
        rfc_job_release(job);

        /* Coeffs supplied without verified output metadata -> RFC_ERR_NO_ADAPTER. */
        g_ctrl.supply_coeffs = 1;
        g_ctrl.coeff_meta = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_NO_ADAPTER &&
              snap.failing_stage == RFC_STAGE_VERIFY);
        CHECK(g_ctrl.export_calls == 0u);
        rfc_job_release(job);

        g_ctrl.coeff_meta = &sm;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && g_ctrl.export_calls == 1u);
        rfc_job_release(job);

        /* Dropped output ICC under preserve must fail; stripped output under strip must succeed. */
        {
            static const uint8_t c_icc[4] = {0xa1, 0xb2, 0xc3, 0xd4};
            rfc_metadata sm_icc = {c_icc, sizeof(c_icc),
                                   RFC_ORIENTATION_ROTATE_90, 0u, {0},
                                   NULL, 0u, NULL, 0u};
            rfc_metadata sm_drop = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u,
                                    {0}, NULL, 0u, NULL, 0u};
            rfc_metadata sm_stripped = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u,
                                        {0}, NULL, 0u, NULL, 0u};
            g_ctrl.icc = c_icc;
            g_ctrl.icc_len = sizeof(c_icc);
            g_ctrl.orientation = RFC_ORIENTATION_ROTATE_90;

            rfc_job_spec mspec = spec;
            mspec.source_meta = &sm_icc;
            g_ctrl.export_calls = 0u;
            g_ctrl.coeff_meta = &sm_drop;
            CHECK(rfc_scheduler_submit(sched, NULL, &mspec, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.status == RFC_ERR_VERIFY_MISMATCH &&
                  snap.failing_stage == RFC_STAGE_VERIFY &&
                  g_ctrl.export_calls == 0u);
            rfc_verify_report mrep;
            CHECK(rfc_job_verify_report_get(job, &mrep) == RFC_OK);
            CHECK(mrep.mismatch.kind == RFC_MISMATCH_METADATA_LOST);
            rfc_job_release(job);

            rfc_setting_value s_coeff_strip[2] = {
                {"exactness", "coefficient-exact"}, {"metadata", "strip"}};
            mspec.settings = s_coeff_strip;
            mspec.setting_count = 2u;
            g_ctrl.coeff_meta = &sm_stripped;
            CHECK(rfc_scheduler_submit(sched, NULL, &mspec, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_SUCCEEDED && g_ctrl.export_calls == 1u);
            CHECK(rfc_job_verify_report_get(job, &mrep) == RFC_OK);
            CHECK(mrep.meta_outcome == RFC_VERIFY_META_STRIPPED_EXPLICITLY);
            rfc_job_release(job);

            g_ctrl.icc = NULL;
            g_ctrl.icc_len = 0u;
            g_ctrl.orientation = RFC_ORIENTATION_NORMAL;
            g_ctrl.coeff_meta = &sm;
        }

        g_ctrl.export_calls = 0u;
        cb.quant_tables[0].values[3] += 2u;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_VERIFY_MISMATCH &&
              snap.failing_stage == RFC_STAGE_VERIFY && g_ctrl.export_calls == 0u);
        rfc_verify_report rep;
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.mismatch.kind == RFC_MISMATCH_JPEG_QUANT_TABLE);
        rfc_job_release(job);

        rfc_scheduler_destroy(sched);
        cleanup_ctrl();
    }

    /* 4. Original-byte-reconstruction (JPEG -> JXL): compares reconstructed JPEG, never .jxl. */
    {
        reset_ctrl(RFC_SAMPLE_RGB8, 16u, 16u);
        uint8_t orig_jpeg[20] = {0xff, 0xd8, 0xff, 0xdb, 0x10, 0x20, 0x30,
                                 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0,
                                 0xb0, 0xc0, 0xd0, 0xe0, 0xff, 0xd9};
        uint8_t wrong_jxl[12] = {0xff, 0x0a, 'J', 'X', 'L', 0x42,
                                 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        uint8_t good_recon[20];
        memcpy(good_recon, orig_jpeg, sizeof(orig_jpeg));

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 16u, 16u, 48u, 8u};
        rfc_metadata sm = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                           NULL, 0u, NULL, 0u};
        rfc_setting_value s_rec[1] = {
            {"exactness", "original-byte-reconstruction"}};
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_JPEG;
        spec.target_format = RFC_FORMAT_JPEG_XL;
        spec.source_bytes = orig_jpeg;
        spec.source_size = sizeof(orig_jpeg);
        spec.source_layout = &lay;
        spec.source_meta = &sm;
        spec.settings = s_rec;
        spec.setting_count = 1u;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_encode = stage_encode_with_verify_artifacts;
        spec.stage_ops.on_export = stage_export_count;

        /* Omitted reconstruction bytes -> RFC_ERR_NO_ADAPTER. */
        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_NO_ADAPTER &&
              snap.failing_stage == RFC_STAGE_VERIFY);
        rfc_job_release(job);

        /* Reconstruction bytes supplied without output metadata -> RFC_ERR_NO_ADAPTER. */
        g_ctrl.supply_recon = 1;
        g_ctrl.recon_bytes = good_recon;
        g_ctrl.recon_len = sizeof(good_recon);
        g_ctrl.recon_meta = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_NO_ADAPTER &&
              snap.failing_stage == RFC_STAGE_VERIFY &&
              g_ctrl.export_calls == 0u);
        rfc_job_release(job);

        /* Passing .jxl output bytes instead of reconstructed JPEG must fail. */
        g_ctrl.recon_bytes = wrong_jxl;
        g_ctrl.recon_len = sizeof(wrong_jxl);
        g_ctrl.recon_meta = &sm;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_VERIFY_MISMATCH &&
              g_ctrl.export_calls == 0u);
        rfc_verify_report rep;
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.mismatch.kind == RFC_MISMATCH_BYTE_HASH);
        rfc_job_release(job);

        /* Passing reconstructed JPEG matching orig_jpeg succeeds even though encoded output is .jxl. */
        g_ctrl.recon_bytes = good_recon;
        g_ctrl.recon_len = sizeof(good_recon);
        g_ctrl.recon_meta = &sm;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && g_ctrl.export_calls == 1u);
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.exactness == RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION &&
              memcmp(rep.source_hash, rep.verified_hash, 32u) == 0);
        rfc_job_release(job);

        /* Non-JPEG source with coefficient-exact / original-byte-reconstruction fails preflight with 0 allocs. */
        rfc_scheduler_stats pst_a, pst_b;
        rfc_scheduler_stats_get(sched, &pst_a);
        rfc_job_spec bad_s = spec;
        bad_s.source_format = RFC_FORMAT_PNG;
        rfc_job *bad_j = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &bad_s, &bad_j) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        rfc_setting_value s_coeff_only[1] = {
            {"exactness", "coefficient-exact"}};
        bad_s.settings = s_coeff_only;
        CHECK(rfc_scheduler_submit(sched, NULL, &bad_s, &bad_j) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(bad_j == NULL);
        rfc_scheduler_stats_get(sched, &pst_b);
        CHECK(pst_b.submission_alloc_count == pst_a.submission_alloc_count);

        rfc_scheduler_destroy(sched);
        cleanup_ctrl();
    }

    /* 5. Direct rfc_verify_encoded_fresh API contract checks. */
    {
        alloc_tracker tr;
        rfc_allocator al;
        init_tracked_alloc(&tr, &al);
        rfc_limits lim;
        rfc_limits_default(&lim);

        rfc_frame exp_f;
        fill_deterministic_frame(&exp_f, 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 0u, &al);

        uint8_t mck[32 + 48];
        memset(mck, 0, sizeof(mck));
        mck[0] = 'M';
        mck[1] = 'C';
        mck[2] = 'K';
        write_u32(mck + 4u, 4u);
        write_u32(mck + 8u, 4u);
        write_u32(mck + 12u, (uint32_t)RFC_SAMPLE_RGB8);
        write_u32(mck + 16u, (uint32_t)RFC_ALPHA_NONE);
        write_u32(mck + 20u, 8u);
        write_u32(mck + 24u, (uint32_t)RFC_ORIENTATION_NORMAL);
        memcpy(mck + 32u, exp_f.data, 48u);

        rfc_verify_report frep;
        rfc_encoded unregistered = {mck, sizeof(mck), NULL};
        CHECK(rfc_verify_encoded_fresh(
                  RFC_FORMAT_QOI, &unregistered, &exp_f, 1u, NULL,
                  RFC_META_POLICY_PRESERVE, 0u, NULL, 0u, &lim, &al, &frep) ==
              RFC_ERR_NO_ADAPTER);

        reset_ctrl(RFC_SAMPLE_RGB8, 4u, 4u);
        rfc_encoded png_enc = {mck, sizeof(mck), NULL};
        CHECK(rfc_verify_encoded_fresh(
                  RFC_FORMAT_PNG, &png_enc, &exp_f, 1u, NULL,
                  RFC_META_POLICY_PRESERVE, 0u, NULL, 0u, &lim, &al, &frep) ==
              RFC_OK);
        CHECK(frep.valid == 1 && frep.independent_decoder == 0);
        CHECK(strcmp(frep.decoder_provenance, "same-provider-fresh-session") ==
              0);
        CHECK(memcmp(frep.source_hash, frep.verified_hash, 32u) == 0);

        mck[32u] ^= 0x01u;
        CHECK(rfc_verify_encoded_fresh(
                  RFC_FORMAT_PNG, &png_enc, &exp_f, 1u, NULL,
                  RFC_META_POLICY_PRESERVE, 0u, NULL, 0u, &lim, &al, &frep) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(frep.mismatch.kind == RFC_MISMATCH_SAMPLE_VALUE);
        cleanup_ctrl();

        /* Unknown-length multi-frame stream in standalone rfc_verify_encoded_fresh stops before over-limit frame allocation. */
        {
            uint8_t anim[16] = {'A', 'N', 'I', 'M'};
            rfc_encoded anim_enc = {anim, sizeof(anim), NULL};
            rfc_limits sl;
            rfc_limits_default(&sl);
            sl.max_frames = 2u;

            reset_ctrl(RFC_SAMPLE_RGB8, 4u, 4u);
            g_ctrl.stream_unknown_frames = 4u;
            CHECK(rfc_verify_encoded_fresh(
                      RFC_FORMAT_PNG, &anim_enc, &exp_f, 1u, NULL,
                      RFC_META_POLICY_PRESERVE, 0u, NULL, 0u, &sl, &al,
                      &frep) == RFC_ERR_LIMIT_EXCEEDED);
            CHECK(g_ctrl.stream_alloc_successes == 2u);
            CHECK(tr.live == 1u);
            cleanup_ctrl();

            rfc_limits_default(&sl);
            sl.max_frames = 16u;
            sl.max_decoded_bytes = 2u * 48u;
            reset_ctrl(RFC_SAMPLE_RGB8, 4u, 4u);
            g_ctrl.stream_unknown_frames = 4u;
            CHECK(rfc_verify_encoded_fresh(
                      RFC_FORMAT_PNG, &anim_enc, &exp_f, 1u, NULL,
                      RFC_META_POLICY_PRESERVE, 0u, NULL, 0u, &sl, &al,
                      &frep) == RFC_ERR_LIMIT_EXCEEDED);
            CHECK(g_ctrl.stream_alloc_successes == 2u);
            CHECK(tr.live == 1u);
            cleanup_ctrl();
        }

        rfc_frame_free(&exp_f);
        CHECK(tr.live == 0u);
    }

    printf("exactness: stage_verify_modes ok (pixel=fold byte=sha256 "
           "coeff=dct+quant recon=byte-hash fresh_api=same-provider-fresh-session/no-adapter)\n");
}

static void test_metadata_policies(void) {
    static const uint8_t icc_bytes[8] = {0x11, 0x22, 0x33, 0x44,
                                         0x55, 0x66, 0x77, 0x88};
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;

    /* A. WEBP -> TIFF preserve success & metadata-lost mismatch refusal. */
    {
        reset_ctrl(RFC_SAMPLE_RGB8, 4u, 4u);
        g_ctrl.icc = icc_bytes;
        g_ctrl.icc_len = sizeof(icc_bytes);
        g_ctrl.orientation = RFC_ORIENTATION_ROTATE_90;

        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        uint8_t src[16] = {1, 2, 3, 4};
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_metadata sm = {icc_bytes, sizeof(icc_bytes),
                           RFC_ORIENTATION_ROTATE_90, 0u, {0},
                           NULL, 0u, NULL, 0u};
        rfc_setting_value s_pres[1] = {{"metadata", "preserve"}};
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_WEBP;
        spec.target_format = RFC_FORMAT_TIFF;
        spec.source_bytes = src;
        spec.source_size = sizeof(src);
        spec.source_layout = &lay;
        spec.source_meta = &sm;
        spec.settings = s_pres;
        spec.setting_count = 1u;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_export = stage_export_count;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && g_ctrl.export_calls == 1u);
        rfc_verify_report rep;
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.meta_outcome == RFC_VERIFY_META_PRESERVED);
        rfc_job_release(job);

        /* Drop ICC during encode -> RFC_ERR_VERIFY_MISMATCH (RFC_MISMATCH_METADATA_LOST). */
        g_ctrl.export_calls = 0u;
        g_ctrl.drop_icc_on_encode = 1;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_VERIFY_MISMATCH &&
              snap.failing_stage == RFC_STAGE_VERIFY &&
              g_ctrl.export_calls == 0u);
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.mismatch.kind == RFC_MISMATCH_METADATA_LOST);
        rfc_job_release(job);

        /* B. Strip policy: explicit stripped pass & retained-metadata refusal. */
        g_ctrl.drop_icc_on_encode = 0;
        g_ctrl.export_calls = 0u;
        rfc_setting_value s_strip[1] = {{"metadata", "strip"}};
        spec.settings = s_strip;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && g_ctrl.export_calls == 1u);
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.meta_outcome == RFC_VERIFY_META_STRIPPED_EXPLICITLY);
        rfc_job_release(job);

        g_ctrl.export_calls = 0u;
        g_ctrl.retain_forbidden_meta_on_encode = 1;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_VERIFY_MISMATCH &&
              g_ctrl.export_calls == 0u);
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.mismatch.kind == RFC_MISMATCH_METADATA_RETAINED);
        rfc_job_release(job);

        rfc_scheduler_destroy(sched);
        cleanup_ctrl();
    }

    /* C. TIFF -> RASTERFOLD (no ICC/orientation cap): refuse unless acknowledged, preserve ack across retry. */
    {
        reset_ctrl(RFC_SAMPLE_RGB8, 4u, 4u);
        g_ctrl.icc = icc_bytes;
        g_ctrl.icc_len = sizeof(icc_bytes);
        g_ctrl.orientation = RFC_ORIENTATION_ROTATE_90;

        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        uint8_t src[16] = {1, 2, 3, 4};
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_metadata sm = {icc_bytes, sizeof(icc_bytes),
                           RFC_ORIENTATION_ROTATE_90, 0u, {0},
                           NULL, 0u, NULL, 0u};
        rfc_setting_value s_pres[1] = {{"metadata", "preserve"}};
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_TIFF;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.source_bytes = src;
        spec.source_size = sizeof(src);
        spec.source_layout = &lay;
        spec.source_meta = &sm;
        spec.settings = s_pres;
        spec.setting_count = 1u;
        spec.expected_working_set_bytes = 256u * 1024u;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_export = stage_export_count;

        rfc_scheduler_stats st_before, st_after;
        rfc_scheduler_stats_get(sched, &st_before);
        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(job == NULL);
        rfc_scheduler_stats_get(sched, &st_after);
        CHECK(st_after.submission_alloc_count ==
              st_before.submission_alloc_count);

        /* Direct rfc_metadata_verify check also names unsupported items. */
        rfc_metadata_report mrep;
        rfc_sample_mismatch mmm;
        rfc_metadata dm = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                           NULL, 0u, NULL, 0u};
        CHECK(rfc_metadata_verify(RFC_FORMAT_RASTERFOLD,
                                  RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &sm, NULL, 0u, &dm,
                                  NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(mrep.outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED);
        CHECK(mrep.unsupported_items ==
              (RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION));
        CHECK(strstr(mrep.detail, "icc, orientation") != NULL);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_UNSUPPORTED);

        /* With explicit acknowledgement on a retryable job slot, fails export then retries cleanly. */
        rfc_job_slot *slot = rfc_job_slot_create();
        CHECK(slot != NULL);
        spec.acknowledged_unsupported_metadata =
            RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION;
        g_ctrl.fail_first_export = 1;
        CHECK(rfc_scheduler_submit(sched, slot, &spec, &job) == RFC_OK);
        CHECK(rfc_job_acknowledged_metadata(job) ==
              (RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION));
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);

        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED &&
              snap.failing_stage == RFC_STAGE_EXPORT);
        rfc_verify_report rep;
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.meta_outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED);
        CHECK(strstr(rep.metadata.detail, "icc, orientation") != NULL);

        rfc_job *job2 = NULL;
        CHECK(rfc_scheduler_retry(sched, slot, &job2) == RFC_OK);
        CHECK(rfc_job_acknowledged_metadata(job2) ==
              (RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION));
        CHECK(rfc_job_wait(job2, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job2, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && g_ctrl.export_calls == 1u);
        CHECK(rfc_job_verify_report_get(job2, &rep) == RFC_OK);
        CHECK(rep.meta_outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED &&
              rep.metadata.unsupported_items ==
                  (RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION));

        rfc_job_release(job);
        rfc_job_release(job2);

        /* Retry whose next attempt fails BEFORE VERIFY must not retain stale verify_report. */
        g_ctrl.fail_first_export = 1;
        CHECK(rfc_scheduler_submit(sched, slot, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED && snap.failing_stage == RFC_STAGE_EXPORT);
        CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
        CHECK(rep.valid == 1);

        g_ctrl.fail_next_probe_status = RFC_ERR_LIMIT_EXCEEDED;
        g_ctrl.fail_on_verify_decode_only = 0;
        rfc_job *job3 = NULL;
        CHECK(rfc_scheduler_retry(sched, slot, &job3) == RFC_OK);
        CHECK(rfc_job_wait(job3, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job3, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED && snap.failing_stage == RFC_STAGE_IMPORT);
        CHECK(rfc_job_verify_report_get(job3, &rep) == RFC_ERR_INVALID_ARGUMENT);
        CHECK(rep.valid == 0);

        rfc_job_release(job);
        rfc_job_release(job3);
        rfc_job_slot_destroy(slot);
        rfc_scheduler_destroy(sched);
        cleanup_ctrl();
    }

    /* D. Per-frame multi-page metadata (later frame drop/orientation) & default-to-nondefault mutation. */
    {
        alloc_tracker tr;
        rfc_allocator al;
        init_tracked_alloc(&tr, &al);

        rfc_frame sf[2], df[2];
        fill_deterministic_frame(&sf[0], 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 0u, &al);
        fill_deterministic_frame(&sf[1], 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 0u, &al);
        fill_deterministic_frame(&df[0], 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 0u, &al);
        fill_deterministic_frame(&df[1], 4u, 4u, RFC_SAMPLE_RGB8,
                                 RFC_ALPHA_NONE, 8u, 0u, 0u, &al);

        sf[0].metadata.icc = icc_bytes;
        sf[0].metadata.icc_len = sizeof(icc_bytes);
        sf[0].metadata.orientation = RFC_ORIENTATION_ROTATE_90;
        sf[1].metadata.icc = icc_bytes;
        sf[1].metadata.icc_len = sizeof(icc_bytes);
        sf[1].metadata.orientation = RFC_ORIENTATION_ROTATE_180;

        df[0].metadata.icc = icc_bytes;
        df[0].metadata.icc_len = sizeof(icc_bytes);
        df[0].metadata.orientation = RFC_ORIENTATION_ROTATE_90;
        df[1].metadata.icc = icc_bytes;
        df[1].metadata.icc_len = sizeof(icc_bytes);
        df[1].metadata.orientation = RFC_ORIENTATION_ROTATE_180;

        rfc_metadata cont = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                             NULL, 0u, NULL, 0u};
        rfc_metadata_report mrep;
        rfc_sample_mismatch mmm;
        CHECK(rfc_metadata_verify(RFC_FORMAT_JPEG_XL, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &cont, sf, 2u, &cont,
                                  df, 2u, 0u, &mrep, &mmm) == RFC_OK);

        /* Paired native-orientation vs EXIF-carrier regression (D10 T02 s3):
         * orientation-only single-frame JXL (exif == NULL, ori = ROTATE_90)
         * passes with dlen == 0 when destination orientation matches, while
         * equivalent WebP (exif == NULL, ori = ROTATE_90) still fails dlen == 0
         * and requires its 26-byte canonical TIFF EXIF carrier; mismatched JXL
         * orientation still fails. */
        {
            sf[0].metadata.icc = NULL;
            sf[0].metadata.icc_len = 0u;
            sf[0].metadata.exif = NULL;
            sf[0].metadata.exif_len = 0u;
            sf[0].metadata.orientation = RFC_ORIENTATION_ROTATE_90;
            df[0].metadata.icc = NULL;
            df[0].metadata.icc_len = 0u;
            df[0].metadata.exif = NULL;
            df[0].metadata.exif_len = 0u;
            df[0].metadata.orientation = RFC_ORIENTATION_ROTATE_90;

            CHECK(rfc_metadata_verify(
                      RFC_FORMAT_JPEG_XL, RFC_META_POLICY_PRESERVE,
                      RFC_EXACT_PIXEL_EXACT, &cont, &sf[0], 1u, &cont, &df[0],
                      1u, 0u, &mrep, &mmm) == RFC_OK);
            CHECK(rfc_metadata_verify(
                      RFC_FORMAT_WEBP, RFC_META_POLICY_PRESERVE,
                      RFC_EXACT_PIXEL_EXACT, &cont, &sf[0], 1u, &cont, &df[0],
                      1u, 0u, &mrep, &mmm) == RFC_ERR_VERIFY_MISMATCH);
            CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
                  (mrep.lost_supported_items & RFC_META_ITEM_EXIF) != 0u);

            df[0].metadata.orientation = RFC_ORIENTATION_NORMAL;
            CHECK(rfc_metadata_verify(
                      RFC_FORMAT_JPEG_XL, RFC_META_POLICY_PRESERVE,
                      RFC_EXACT_PIXEL_EXACT, &cont, &sf[0], 1u, &cont, &df[0],
                      1u, 0u, &mrep, &mmm) == RFC_ERR_VERIFY_MISMATCH);
            CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
                  (mrep.lost_supported_items & RFC_META_ITEM_ORIENTATION) !=
                      0u);

            sf[0].metadata.icc = icc_bytes;
            sf[0].metadata.icc_len = sizeof(icc_bytes);
            sf[0].metadata.orientation = RFC_ORIENTATION_ROTATE_90;
            df[0].metadata.icc = icc_bytes;
            df[0].metadata.icc_len = sizeof(icc_bytes);
            df[0].metadata.orientation = RFC_ORIENTATION_ROTATE_90;
        }

        /* Frame 0 identical, frame 1 loses ICC -> rejected. */
        df[1].metadata.icc = NULL;
        df[1].metadata.icc_len = 0u;
        CHECK(rfc_metadata_verify(RFC_FORMAT_JPEG_XL, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &cont, sf, 2u, &cont,
                                  df, 2u, 0u, &mrep, &mmm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
              (mrep.lost_supported_items & RFC_META_ITEM_ICC) != 0u);

        /* Frame 1 restores ICC, alters orientation -> rejected. */
        df[1].metadata.icc = icc_bytes;
        df[1].metadata.icc_len = sizeof(icc_bytes);
        df[1].metadata.orientation = RFC_ORIENTATION_NORMAL;
        CHECK(rfc_metadata_verify(RFC_FORMAT_JPEG_XL, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &cont, sf, 2u, &cont,
                                  df, 2u, 0u, &mrep, &mmm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
              (mrep.lost_supported_items & RFC_META_ITEM_ORIENTATION) != 0u);

        /* Single-frame NORMAL source mutated to rotated destination (and spurious ICC) -> rejected. */
        sf[0].metadata.icc = NULL;
        sf[0].metadata.icc_len = 0u;
        sf[0].metadata.orientation = RFC_ORIENTATION_NORMAL;
        df[0].metadata.icc = NULL;
        df[0].metadata.icc_len = 0u;
        df[0].metadata.orientation = RFC_ORIENTATION_ROTATE_90;
        CHECK(rfc_metadata_verify(RFC_FORMAT_TIFF, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &cont, &sf[0], 1u,
                                  &cont, &df[0], 1u, 0u, &mrep, &mmm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
              (mrep.lost_supported_items & RFC_META_ITEM_ORIENTATION) != 0u);

        df[0].metadata.orientation = RFC_ORIENTATION_NORMAL;
        df[0].metadata.icc = icc_bytes;
        df[0].metadata.icc_len = sizeof(icc_bytes);
        CHECK(rfc_metadata_verify(RFC_FORMAT_TIFF, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &cont, &sf[0], 1u,
                                  &cont, &df[0], 1u, 0u, &mrep, &mmm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
              (mrep.lost_supported_items & RFC_META_ITEM_ICC) != 0u);

        /* Clear borrowed stack pointers before rfc_frame_free. */
        for (int i = 0; i < 2; i++) {
            sf[i].metadata.icc = NULL;
            sf[i].metadata.icc_len = 0u;
            df[i].metadata.icc = NULL;
            df[i].metadata.icc_len = 0u;
            rfc_frame_free(&sf[i]);
            rfc_frame_free(&df[i]);
        }
        CHECK(tr.live == 0u);
    }

    /* E. Non-ICC colourimetry (sRGB/gAMA/cHRM/cICP/mDCV/cLLI) detection, validation, and policy enforcement. */
    {
        alloc_tracker tr;
        rfc_allocator al;
        init_tracked_alloc(&tr, &al);

        rfc_layout bad_lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 2u, 2u, 6u, 8u};
        rfc_frame bad_f = {0};
        rfc_metadata bad_gama = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                                 NULL, 0u, NULL, 0u};
        bad_gama.color.has_gama = 1u;
        bad_gama.color.gama_scaled = 0u;
        CHECK(rfc_frame_alloc(&bad_f, &bad_lay, &bad_gama, &al) ==
              RFC_ERR_INVALID_ARGUMENT);

        rfc_metadata bad_srgb = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                                 NULL, 0u, NULL, 0u};
        bad_srgb.color.has_srgb = 1u;
        bad_srgb.color.srgb_intent = 4u;
        CHECK(rfc_frame_alloc(&bad_f, &bad_lay, &bad_srgb, &al) ==
              RFC_ERR_INVALID_ARGUMENT);

        rfc_metadata bad_cicp = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                                 NULL, 0u, NULL, 0u};
        bad_cicp.color.has_cicp = 1u;
        bad_cicp.color.cicp[0] = 9u;
        bad_cicp.color.cicp[1] = 16u;
        bad_cicp.color.cicp[2] = 1u;
        bad_cicp.color.cicp[3] = 1u;
        CHECK(rfc_frame_alloc(&bad_f, &bad_lay, &bad_cicp, &al) ==
              RFC_ERR_INVALID_ARGUMENT);

        rfc_metadata src_m = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                              NULL, 0u, NULL, 0u};
        src_m.color.has_gama = 1u;
        src_m.color.gama_scaled = 45455u;
        CHECK((rfc_metadata_source_items(&src_m, NULL, 0u) & RFC_META_ITEM_ICC) !=
              0u);

        rfc_metadata dst_empty = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                                  NULL, 0u, NULL, 0u};
        rfc_metadata_report mrep;
        rfc_sample_mismatch mmm;
        CHECK(rfc_metadata_verify(RFC_FORMAT_PNG, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_m, NULL, 0u,
                                  &dst_empty, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
              (mrep.lost_supported_items & RFC_META_ITEM_ICC) != 0u);

        rfc_metadata src_pair = src_m;
        src_pair.color.has_chrm = 1u;
        src_pair.color.chrm_xy[0] = 31270u;
        src_pair.color.chrm_xy[1] = 32900u;
        rfc_metadata dst_pair = src_pair;
        CHECK(rfc_metadata_verify(RFC_FORMAT_PNG, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_pair, NULL, 0u,
                                  &dst_pair, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_OK);
        CHECK(mrep.outcome == RFC_VERIFY_META_PRESERVED);

        CHECK(rfc_metadata_verify(RFC_FORMAT_PNG, RFC_META_POLICY_STRIP,
                                  RFC_EXACT_PIXEL_EXACT, &src_m, NULL, 0u,
                                  &src_m, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_RETAINED &&
              (mrep.retained_removable_items & RFC_META_ITEM_ICC) != 0u);

        CHECK(rfc_metadata_verify(RFC_FORMAT_BMP, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_m, NULL, 0u,
                                  &dst_empty, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(mrep.outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED &&
              (mrep.unsupported_items & RFC_META_ITEM_ICC) != 0u);

        /* Cross-format non-ICC PNG colourimetry targeting WebP (which supports ICC,
         * not PNG chunks) must refuse pre-encode unless acknowledged. */
        CHECK(rfc_metadata_verify(RFC_FORMAT_WEBP, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_m, NULL, 0u,
                                  &dst_empty, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(mrep.outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED &&
              (mrep.unsupported_items & RFC_META_ITEM_ICC) != 0u);
        CHECK(rfc_metadata_verify(RFC_FORMAT_WEBP, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_m, NULL, 0u,
                                  &dst_empty, NULL, 0u, RFC_META_ITEM_ICC,
                                  &mrep, &mmm) == RFC_OK);
        CHECK(mrep.outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED &&
              (mrep.unsupported_items & RFC_META_ITEM_ICC) != 0u);
        CHECK(tr.live == 0u);
    }

    /* F. EXIF/XMP deep-copy ownership, partial-OOM rollback, synthesized carrier, and strip/preserve verification. */
    {
        alloc_tracker tr;
        rfc_allocator al;
        init_tracked_alloc(&tr, &al);

        uint8_t icc_p[4] = {0x11, 0x22, 0x33, 0x44};
        uint8_t exif_p[26] = {
            'I', 'I', 0x2Au, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u,
            0x12u, 0x01u, 0x03u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x06u, 0x00u,
            0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
        };
        uint8_t xmp_p[14] = "<x:xmpmeta/>";
        rfc_layout fl = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 2u, 2u, 6u, 8u};
        rfc_metadata pm = {icc_p, sizeof(icc_p), RFC_ORIENTATION_ROTATE_90, 0u,
                           {0}, exif_p, sizeof(exif_p), xmp_p, sizeof(xmp_p)};

        /* Validation before alloc: NULL pointer with nonzero len returns 0 allocs. */
        rfc_frame rf = {0};
        rfc_metadata bad_ptr = pm;
        bad_ptr.xmp = NULL;
        CHECK(rfc_frame_alloc(&rf, &fl, &bad_ptr, &al) ==
              RFC_ERR_INVALID_ARGUMENT);
        CHECK(tr.calls == 0u && tr.live == 0u);

        /* Fail on 2nd (icc), 3rd (exif), 4th (xmp) allocation: zero leaks. */
        for (size_t fail_idx = 2u; fail_idx <= 4u; fail_idx++) {
            tr.calls = 0u;
            tr.fail_at = fail_idx;
            CHECK(rfc_frame_alloc(&rf, &fl, &pm, &al) ==
                  RFC_ERR_OUT_OF_MEMORY);
            CHECK(tr.live == 0u);
        }

        /* Succeed, mutate caller buffers, verify deep copies, free cleanly. */
        tr.calls = 0u;
        tr.fail_at = 0u;
        CHECK(rfc_frame_alloc(&rf, &fl, &pm, &al) == RFC_OK);
        CHECK(rf.metadata.icc && rf.metadata.icc != icc_p &&
              rf.metadata.exif && rf.metadata.exif != exif_p &&
              rf.metadata.xmp && rf.metadata.xmp != xmp_p);
        uint8_t exif_orig[26];
        memcpy(exif_orig, exif_p, sizeof(exif_orig));
        uint8_t xmp_orig[14];
        memcpy(xmp_orig, xmp_p, sizeof(xmp_orig));
        memset(icc_p, 0xAA, sizeof(icc_p));
        memset(exif_p, 0xBB, sizeof(exif_p));
        memset(xmp_p, 0xCC, sizeof(xmp_p));
        CHECK(memcmp(rf.metadata.exif, exif_orig, sizeof(exif_orig)) == 0);
        CHECK(memcmp(rf.metadata.xmp, xmp_orig, sizeof(xmp_orig)) == 0);

        rfc_metadata dst_ok = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                               exif_orig, sizeof(exif_orig),
                               xmp_orig, sizeof(xmp_orig)};
        rfc_metadata src_nx = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                               exif_orig, sizeof(exif_orig),
                               xmp_orig, sizeof(xmp_orig)};
        rfc_metadata_report mrep;
        rfc_sample_mismatch mmm;
        CHECK(rfc_metadata_verify(RFC_FORMAT_WEBP, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_nx, NULL, 0u,
                                  &dst_ok, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_OK);
        CHECK(mrep.outcome == RFC_VERIFY_META_PRESERVED);

        /* PNG orientation-only source (exif==NULL, ori==6) -> WebP synthesized 26B EXIF passes. */
        rfc_metadata src_ori_only = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u,
                                     {0}, NULL, 0u, NULL, 0u};
        rfc_metadata dst_synth = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u,
                                  {0}, exif_orig, sizeof(exif_orig),
                                  NULL, 0u};
        CHECK(rfc_metadata_verify(RFC_FORMAT_WEBP, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_ori_only, NULL,
                                  0u, &dst_synth, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_OK);
        CHECK(mrep.outcome == RFC_VERIFY_META_PRESERVED);

        /* Strip retains XMP/EXIF -> RFC_ERR_VERIFY_MISMATCH naming retained bit. */
        CHECK(rfc_metadata_verify(RFC_FORMAT_WEBP, RFC_META_POLICY_STRIP,
                                  RFC_EXACT_PIXEL_EXACT, &src_nx, NULL, 0u,
                                  &dst_ok, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_RETAINED &&
              (mrep.retained_removable_items &
               (RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP)) ==
                  (RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP));

        /* Dropped XMP under PRESERVE -> RFC_MISMATCH_METADATA_LOST. */
        dst_ok.xmp = NULL;
        dst_ok.xmp_len = 0u;
        CHECK(rfc_metadata_verify(RFC_FORMAT_WEBP, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_nx, NULL, 0u,
                                  &dst_ok, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
              (mrep.lost_supported_items & RFC_META_ITEM_XMP) != 0u);

        /* PNG refuses unacknowledged EXIF+XMP with RFC_ERR_METADATA_POLICY. */
        CHECK(rfc_metadata_verify(RFC_FORMAT_PNG, RFC_META_POLICY_PRESERVE,
                                  RFC_EXACT_PIXEL_EXACT, &src_nx, NULL, 0u,
                                  &dst_synth, NULL, 0u, 0u, &mrep, &mmm) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(mrep.outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED &&
              (mrep.unsupported_items &
               (RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP)) ==
                  (RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP));

        {
            /* Finding #2: EXIF orientation with UNSPECIFIED field resolves
             * both RFC_META_ITEM_ORIENTATION and RFC_META_ITEM_EXIF. */
            const uint8_t exif_rot90[26] = {
                'I', 'I', 0x2Au, 0u, 8u, 0u, 0u, 0u,
                1u, 0u,
                0x12u, 0x01u, 3u, 0u, 1u, 0u, 0u, 0u, 6u, 0u, 0u, 0u,
                0u, 0u, 0u, 0u
            };
            rfc_metadata src_exif_unspec = {
                NULL, 0u, RFC_ORIENTATION_UNSPECIFIED, 0u, {0},
                exif_rot90, sizeof(exif_rot90), NULL, 0u
            };
            rfc_metadata dst_exif_rot90 = {
                NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                exif_rot90, sizeof(exif_rot90), NULL, 0u
            };
            uint32_t src_items =
                rfc_metadata_source_items(&src_exif_unspec, NULL, 0u);
            CHECK((src_items & (RFC_META_ITEM_ORIENTATION | RFC_META_ITEM_EXIF)) ==
                  (RFC_META_ITEM_ORIENTATION | RFC_META_ITEM_EXIF));
            CHECK(rfc_metadata_verify(RFC_FORMAT_WEBP, RFC_META_POLICY_PRESERVE,
                                      RFC_EXACT_PIXEL_EXACT, &src_exif_unspec,
                                      NULL, 0u, &dst_exif_rot90, NULL, 0u, 0u,
                                      &mrep, &mmm) == RFC_OK);
            CHECK(mrep.outcome == RFC_VERIFY_META_PRESERVED);
            CHECK(rfc_metadata_verify(RFC_FORMAT_PNG, RFC_META_POLICY_PRESERVE,
                                      RFC_EXACT_PIXEL_EXACT, &src_exif_unspec,
                                      NULL, 0u, &dst_exif_rot90, NULL, 0u, 0u,
                                      &mrep, &mmm) == RFC_ERR_METADATA_POLICY);
            CHECK(mrep.outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED &&
                  (mrep.unsupported_items &
                   (RFC_META_ITEM_ORIENTATION | RFC_META_ITEM_EXIF)) ==
                      (RFC_META_ITEM_ORIENTATION | RFC_META_ITEM_EXIF));

            /* Finding #5: orientation-only source synthesized into EXIF
             * requires the exact 26-byte canonical TIFF blob. */
            const uint8_t noncanon_exif[38] = {
                'I', 'I', 0x2Au, 0u, 8u, 0u, 0u, 0u,
                2u, 0u,
                0x0Eu, 0x01u, 2u, 0u, 3u, 0u, 0u, 0u, 'E', 'X', 0u, 0u,
                0x12u, 0x01u, 3u, 0u, 1u, 0u, 0u, 0u, 6u, 0u, 0u, 0u,
                0u, 0u, 0u, 0u
            };
            rfc_metadata src_ori_only = {
                NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                NULL, 0u, NULL, 0u
            };
            rfc_metadata dst_noncanon = {
                NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                noncanon_exif, sizeof(noncanon_exif), NULL, 0u
            };
            CHECK(rfc_metadata_verify(RFC_FORMAT_WEBP, RFC_META_POLICY_PRESERVE,
                                      RFC_EXACT_PIXEL_EXACT, &src_ori_only,
                                      NULL, 0u, &dst_noncanon, NULL, 0u, 0u,
                                      &mrep, &mmm) == RFC_ERR_VERIFY_MISMATCH);
            CHECK(mmm.kind == RFC_MISMATCH_METADATA_LOST &&
                  (mrep.lost_supported_items & RFC_META_ITEM_EXIF) != 0u);
        }

        rfc_frame_free(&rf);
        CHECK(tr.live == 0u);
    }

    printf("exactness: metadata_policies ok\n");
}

static void test_preview_isolation(void) {
    alloc_tracker tr;
    rfc_allocator al;
    init_tracked_alloc(&tr, &al);

    /* Case 1: RGB16 0x8000 vs 0x8001 collide in 8-bit preview, distinct in verifier. */
    {
        rfc_frame a, b;
        fill_deterministic_frame(&a, 4u, 4u, RFC_SAMPLE_RGB16, RFC_ALPHA_NONE,
                                 16u, 0u, 0u, &al);
        fill_deterministic_frame(&b, 4u, 4u, RFC_SAMPLE_RGB16, RFC_ALPHA_NONE,
                                 16u, 0u, 0u, &al);
        write_u16(a.data + 0u, 0x8000u);
        write_u16(b.data + 0u, 0x8001u);

        rfc_preview_frame pa, pb;
        CHECK(rfc_preview_render_srgb8(&a, 128u, 128u, 128u, 0, &al, &pa) ==
              RFC_OK);
        CHECK(rfc_preview_render_srgb8(&b, 128u, 128u, 128u, 0, &al, &pb) ==
              RFC_OK);
        CHECK(pa.not_for_verification == 1 && pb.not_for_verification == 1);
        CHECK(pa.data_len == pb.data_len &&
              memcmp(pa.rgba8_srgb, pb.rgba8_srgb, pa.data_len) == 0);

        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&a, 1u, &b, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_SAMPLE_VALUE);

        rfc_preview_free(&pa);
        rfc_preview_free(&pb);
        rfc_frame_free(&a);
        rfc_frame_free(&b);
    }

    /* Case 2: RGBA8 straight alpha 0 (200,10,50,0) vs (0,0,0,0) collide in matte preview. */
    {
        rfc_frame a, b;
        fill_deterministic_frame(&a, 4u, 4u, RFC_SAMPLE_RGBA8,
                                 RFC_ALPHA_STRAIGHT, 8u, 0u, 0u, &al);
        fill_deterministic_frame(&b, 4u, 4u, RFC_SAMPLE_RGBA8,
                                 RFC_ALPHA_STRAIGHT, 8u, 0u, 0u, &al);
        a.data[0] = 200u;
        a.data[1] = 10u;
        a.data[2] = 50u;
        a.data[3] = 0u;
        b.data[0] = 0u;
        b.data[1] = 0u;
        b.data[2] = 0u;
        b.data[3] = 0u;

        rfc_preview_frame pa, pb;
        CHECK(rfc_preview_render_srgb8(&a, 32u, 64u, 96u, 0, &al, &pa) ==
              RFC_OK);
        CHECK(rfc_preview_render_srgb8(&b, 32u, 64u, 96u, 0, &al, &pb) ==
              RFC_OK);
        CHECK(pa.not_for_verification == 1 &&
              memcmp(pa.rgba8_srgb, pb.rgba8_srgb, pa.data_len) == 0);

        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&a, 1u, &b, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_TRANSPARENT_RGB);

        rfc_preview_free(&pa);
        rfc_preview_free(&pb);
        rfc_frame_free(&a);
        rfc_frame_free(&b);
    }

    /* Case 3: Linear float & half straight vs premultiplied equivalence in preview (and verifier rejection). */
    {
        rfc_frame fs, fp;
        rfc_layout ls = {RFC_SAMPLE_RGBA_FLOAT, RFC_ALPHA_STRAIGHT, 1u, 1u,
                         16u, 32u};
        rfc_layout lp = {RFC_SAMPLE_RGBA_FLOAT, RFC_ALPHA_PREMULTIPLIED, 1u, 1u,
                         16u, 32u};
        rfc_metadata fm = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                           NULL, 0u, NULL, 0u};
        CHECK(rfc_frame_alloc(&fs, &ls, &fm, &al) == RFC_OK);
        CHECK(rfc_frame_alloc(&fp, &lp, &fm, &al) == RFC_OK);

        /* Straight (1.0, 1.0, 1.0, 0.5) vs premult (0.5, 0.5, 0.5, 0.5) over black -> ~188 in both. */
        write_u32(fs.data + 0u, 0x3f800000u);
        write_u32(fs.data + 4u, 0x3f800000u);
        write_u32(fs.data + 8u, 0x3f800000u);
        write_u32(fs.data + 12u, 0x3f000000u);

        write_u32(fp.data + 0u, 0x3f000000u);
        write_u32(fp.data + 4u, 0x3f000000u);
        write_u32(fp.data + 8u, 0x3f000000u);
        write_u32(fp.data + 12u, 0x3f000000u);

        rfc_preview_frame pa, pb;
        CHECK(rfc_preview_render_srgb8(&fs, 0u, 0u, 0u, 0, &al, &pa) == RFC_OK);
        CHECK(rfc_preview_render_srgb8(&fp, 0u, 0u, 0u, 0, &al, &pb) == RFC_OK);
        CHECK(pa.rgba8_srgb[0] >= 187u && pa.rgba8_srgb[0] <= 189u);
        CHECK(pa.data_len == pb.data_len &&
              memcmp(pa.rgba8_srgb, pb.rgba8_srgb, pa.data_len) == 0);
        rfc_preview_free(&pa);
        rfc_preview_free(&pb);

        /* Straight vs premult float over colored matte (40, 120, 200) match byte-for-byte. */
        write_u32(fs.data + 4u, 0x3e800000u); /* 0.25 */
        write_u32(fs.data + 8u, 0x00000000u); /* 0.0 */
        write_u32(fp.data + 4u, 0x3e000000u); /* 0.125 */
        write_u32(fp.data + 8u, 0x00000000u); /* 0.0 */
        CHECK(rfc_preview_render_srgb8(&fs, 40u, 120u, 200u, 0, &al, &pa) ==
              RFC_OK);
        CHECK(rfc_preview_render_srgb8(&fp, 40u, 120u, 200u, 0, &al, &pb) ==
              RFC_OK);
        CHECK(pa.data_len == pb.data_len &&
              memcmp(pa.rgba8_srgb, pb.rgba8_srgb, pa.data_len) == 0);

        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&fs, 1u, &fp, 1u, &mm) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(mm.kind == RFC_MISMATCH_ALPHA_MODE);
        rfc_preview_free(&pa);
        rfc_preview_free(&pb);

        /* Fully transparent straight float renders exact sRGB matte color. */
        write_u32(fs.data + 12u, 0x00000000u);
        CHECK(rfc_preview_render_srgb8(&fs, 40u, 120u, 200u, 0, &al, &pa) ==
              RFC_OK);
        CHECK(pa.rgba8_srgb[0] == 40u && pa.rgba8_srgb[1] == 120u &&
              pa.rgba8_srgb[2] == 200u);
        rfc_preview_free(&pa);
        rfc_frame_free(&fs);
        rfc_frame_free(&fp);

        /* Half straight (1.0, 0.25, 0.0, 0.5) vs premult (0.5, 0.125, 0.0, 0.5). */
        rfc_layout lhs = {RFC_SAMPLE_RGBA_HALF, RFC_ALPHA_STRAIGHT, 1u, 1u, 8u,
                          16u};
        rfc_layout lhp = {RFC_SAMPLE_RGBA_HALF, RFC_ALPHA_PREMULTIPLIED, 1u,
                          1u, 8u, 16u};
        CHECK(rfc_frame_alloc(&fs, &lhs, &fm, &al) == RFC_OK);
        CHECK(rfc_frame_alloc(&fp, &lhp, &fm, &al) == RFC_OK);
        write_u16(fs.data + 0u, 0x3c00u); /* 1.0 */
        write_u16(fs.data + 2u, 0x3400u); /* 0.25 */
        write_u16(fs.data + 4u, 0x0000u); /* 0.0 */
        write_u16(fs.data + 6u, 0x3800u); /* 0.5 */
        write_u16(fp.data + 0u, 0x3800u); /* 0.5 */
        write_u16(fp.data + 2u, 0x3000u); /* 0.125 */
        write_u16(fp.data + 4u, 0x0000u); /* 0.0 */
        write_u16(fp.data + 6u, 0x3800u); /* 0.5 */
        CHECK(rfc_preview_render_srgb8(&fs, 0u, 0u, 0u, 0, &al, &pa) == RFC_OK);
        CHECK(rfc_preview_render_srgb8(&fp, 0u, 0u, 0u, 0, &al, &pb) == RFC_OK);
        CHECK(pa.rgba8_srgb[0] >= 187u && pa.rgba8_srgb[0] <= 189u);
        CHECK(memcmp(pa.rgba8_srgb, pb.rgba8_srgb, pa.data_len) == 0);
        rfc_preview_free(&pa);
        rfc_preview_free(&pb);
        rfc_frame_free(&fs);
        rfc_frame_free(&fp);
    }

    CHECK(tr.live == 0u);
    printf("exactness: preview_isolation ok\n");
}

static void test_cancel_and_faults(void) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;

    /* 1. Mid-VERIFY decode cancellation barrier. */
    {
        reset_ctrl(RFC_SAMPLE_RGB8, 8u, 8u);
        g_ctrl.block_on_verify_decode = 1;

        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);
        uint8_t src[16] = {1, 2, 3, 4};
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 24u, 8u};
        rfc_metadata sm = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                           NULL, 0u, NULL, 0u};
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_PNG;
        spec.target_format = RFC_FORMAT_PNG;
        spec.source_bytes = src;
        spec.source_size = sizeof(src);
        spec.source_layout = &lay;
        spec.source_meta = &sm;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_export = stage_export_count;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);

        ex_mutex_lock(&g_ctrl.lock);
        while (!g_ctrl.verify_decode_entered) {
            ex_cond_wait(&g_ctrl.cond, &g_ctrl.lock);
        }
        ex_mutex_unlock(&g_ctrl.lock);

        CHECK(rfc_job_cancel(job) == RFC_OK);
        ex_mutex_lock(&g_ctrl.lock);
        g_ctrl.release_verify_decode = 1;
        ex_cond_broadcast(&g_ctrl.cond);
        ex_mutex_unlock(&g_ctrl.lock);

        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_CANCELLED && snap.status == RFC_ERR_CANCELLED);
        CHECK(g_ctrl.export_calls == 0u);
        CHECK(g_ctrl.open_count == g_ctrl.close_count);
        CHECK(snap.budget.current_allocated_bytes == 0u);
        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        cleanup_ctrl();
    }

    /* 2. Target probe error, decode OOM, decompression-ratio bomb, and tight budget OOM in VERIFY. */
    {
        for (int tc = 0; tc < 4; tc++) {
            reset_ctrl(RFC_SAMPLE_RGB8, tc == 3 ? 16u : 8u, tc == 3 ? 16u : 8u);
            if (tc == 0) {
                g_ctrl.fail_next_probe_status = RFC_ERR_LIMIT_EXCEEDED;
                g_ctrl.fail_on_verify_decode_only = 1;
            } else if (tc == 1) {
                g_ctrl.fail_next_decode_status = RFC_ERR_OUT_OF_MEMORY;
                g_ctrl.fail_on_verify_decode_only = 1;
            } else if (tc == 2) {
                g_ctrl.force_tiny_ratio_bomb = 1;
            }

            rfc_limits lim;
            rfc_limits_default(&lim);
            if (tc == 2) {
                lim.max_expansion_ratio = 4u;
            }

            rfc_scheduler *sched = rfc_scheduler_create(&cfg);
            CHECK(sched != NULL);
            uint8_t src[512];
            memset(src, 0, sizeof(src));
            uint32_t dim = (tc == 3) ? 16u : 8u;
            rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, dim, dim,
                              (size_t)dim * 3u, 8u};
            rfc_metadata sm = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                           NULL, 0u, NULL, 0u};
            rfc_job_spec spec;
            memset(&spec, 0, sizeof(spec));
            spec.source_format = RFC_FORMAT_PNG;
            spec.target_format = RFC_FORMAT_PNG;
            spec.source_bytes = src;
            spec.source_size = sizeof(src);
            spec.source_layout = &lay;
            spec.source_meta = &sm;
            spec.limits_override = &lim;
            /* 16x16x3=768B: fits decoded frame (840B) + encoded MCK (800B) = ~1.7KB,
             * then overflows 2048B job limit when VERIFY allocates a fresh 768B frame. */
            spec.expected_working_set_bytes =
                (tc == 3) ? 2048u : (64u * 1024u);
            spec.job_memory_limit_bytes = (tc == 3) ? 2048u : 0u;
            spec.has_stage_ops = 1;
            spec.stage_ops.on_export = stage_export_count;

            rfc_job *job = NULL;
            CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            rfc_job_snapshot snap;
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED);
            CHECK(snap.failing_stage == RFC_STAGE_VERIFY);
            if (tc == 3) {
                CHECK(snap.status == RFC_ERR_OUT_OF_MEMORY);
            }
            CHECK(g_ctrl.export_calls == 0u);
            CHECK(g_ctrl.open_count == g_ctrl.close_count);
            CHECK(snap.budget.current_allocated_bytes == 0u);
            rfc_job_release(job);
            rfc_scheduler_destroy(sched);
            cleanup_ctrl();
        }
    }

    printf("exactness: cancel_and_faults ok\n");
}

int main(int argc, char **argv) {
    ex_mutex_init(&g_rf_stats_lock);
    register_all_test_adapters();

    if (argc == 4 && strcmp(argv[1], "--fold-roundtrip") == 0) {
        run_fold_roundtrip_export(argv[2], argv[3]);
        unregister_all_test_adapters();
        ex_mutex_destroy(&g_rf_stats_lock);
        return 0;
    }

    test_sha256_and_digest();
    test_sample_formats_and_mutations();
    test_jpeg_coefficients();
    test_stage_verify_modes();
    test_metadata_policies();
    test_preview_isolation();
    test_cancel_and_faults();

    unregister_all_test_adapters();
    ex_mutex_destroy(&g_rf_stats_lock);
    printf("exactness: all checks passed\n");
    return 0;
}
