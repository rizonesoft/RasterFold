/* C11 codec & format adapter test suite + CLI harness for D10 T02 s2:
 *   RFC_FORMAT_WEBP (libwebp lossless exact-RGB VP8L, lossy VP8 import,
 *   ICCP/EXIF/XMP mux/demux, policy/limits/budget/cancel, malformed RIFF). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "rasterfold.h"
#include "rfc_codec.h"
#include "rfc_codecs.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_report.h"
#include "rfc_verify.h"
#include "rf_shared_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK FAILED (%s:%d): %s\n", __FILE__, __LINE__,  \
                    #cond);                                                    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Portable helpers: endian, CRC32, tracked allocator, files, frames        */
/* ------------------------------------------------------------------------ */

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t png_crc32(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0u; i < len; i++) {
        c ^= (uint32_t)data[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return ~c;
}

static uint8_t *insert_png_chunk_before_idat(const uint8_t *png,
                                             size_t png_len,
                                             const char tag[4],
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             size_t *out_len) {
    CHECK(png != NULL && png_len > 12u);
    size_t pos = 8u;
    size_t idat_off = 0u;
    while (pos + 12u <= png_len) {
        uint32_t clen = rd32be(png + pos);
        if (memcmp(png + pos + 4u, "IDAT", 4u) == 0) {
            idat_off = pos;
            break;
        }
        pos += 12u + (size_t)clen;
    }
    CHECK(idat_off > 8u);
    size_t chunk_total = 12u + payload_len;
    size_t total = png_len + chunk_total;
    uint8_t *buf = (uint8_t *)malloc(total);
    CHECK(buf != NULL);
    memcpy(buf, png, idat_off);
    wr32be(buf + idat_off, (uint32_t)payload_len);
    memcpy(buf + idat_off + 4u, tag, 4u);
    if (payload_len > 0u) {
        memcpy(buf + idat_off + 8u, payload, payload_len);
    }
    uint32_t crc = png_crc32(buf + idat_off + 4u, 4u + payload_len);
    wr32be(buf + idat_off + 8u + payload_len, crc);
    memcpy(buf + idat_off + chunk_total, png + idat_off, png_len - idat_off);
    *out_len = total;
    return buf;
}

typedef struct tracked_alloc_state {
    size_t live;
    size_t count;
} tracked_alloc_state;

static void *t_alloc(void *ctx, size_t sz) {
    tracked_alloc_state *st = (tracked_alloc_state *)ctx;
    void *p = malloc(sz ? sz : 1u);
    if (p && st) {
        st->live++;
        st->count++;
    }
    return p;
}

static void t_free(void *ctx, void *ptr) {
    tracked_alloc_state *st = (tracked_alloc_state *)ctx;
    if (ptr) {
        if (st && st->live > 0u) {
            st->live--;
        }
        free(ptr);
    }
}

typedef struct cancel_alloc_state {
    tracked_alloc_state track;
    size_t skip_allocs;
    const rfc_session_adapter *adp;
    void *sess;
    int triggered;
} cancel_alloc_state;

static void *ca_alloc(void *ctx, size_t sz) {
    cancel_alloc_state *c = (cancel_alloc_state *)ctx;
    if (!c->triggered && c->adp && c->sess) {
        if (c->skip_allocs > 0u) {
            c->skip_allocs--;
        } else {
            c->triggered = 1;
            c->adp->cancel_session(c->sess);
        }
    }
    return t_alloc(&c->track, sz);
}

static void ca_free(void *ctx, void *ptr) {
    cancel_alloc_state *c = (cancel_alloc_state *)ctx;
    t_free(&c->track, ptr);
}

static void init_exec(rfc_exec_context *exec,
                      tracked_alloc_state *st,
                      rfc_allocator *alloc,
                      rfc_budget *budget,
                      uint32_t threads) {
    memset(exec, 0, sizeof(*exec));
    memset(st, 0, sizeof(*st));
    alloc->alloc = t_alloc;
    alloc->free = t_free;
    alloc->ctx = st;
    rfc_limits_default(&exec->limits);
    exec->codec_threads = threads;
    exec->allocator = alloc;
    exec->budget = budget;
}

static void fill_minimal_srgb_icc(uint8_t valid_icc[132]) {
    memset(valid_icc, 0, 132u);
    wr32be(valid_icc + 0u, 132u);
    wr32be(valid_icc + 8u, 0x02100000u);
    memcpy(valid_icc + 12u, "mntrRGB XYZ ", 12u);
    valid_icc[24u] = 0x07u;
    valid_icc[25u] = 0xEAu;
    valid_icc[27u] = 11u;
    valid_icc[29u] = 16u;
    memcpy(valid_icc + 36u, "acsp", 4u);
    wr32be(valid_icc + 68u, 0x0000F6D6u);
    wr32be(valid_icc + 72u, 0x00010000u);
    wr32be(valid_icc + 76u, 0x0000D32Du);
}

static void fill_tiff_exif_be_no_ori(uint8_t out_tiff[26]) {
    /* Valid big-endian TIFF carrying ImageDescription (0x010E) instead of 0x0112 */
    static const uint8_t k_tmpl[26] = {
        'M',  'M',  0x00u, 0x2Au, 0x00u, 0x00u, 0x00u, 0x08u,
        0x00u, 0x01u,
        0x01u, 0x0Eu, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x02u,
        'O',  'K',  0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u
    };
    memcpy(out_tiff, k_tmpl, 26u);
}

static void fill_tiff_exif_with_ori_be(uint8_t out_tiff[26], uint16_t ori) {
    static const uint8_t k_tmpl[26] = {
        'M',  'M',  0x00u, 0x2Au, 0x00u, 0x00u, 0x00u, 0x08u,
        0x00u, 0x01u,
        0x01u, 0x12u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u, 0x01u,
        0x00u, 0x01u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u
    };
    memcpy(out_tiff, k_tmpl, 26u);
    out_tiff[18] = (uint8_t)((ori >> 8) & 0xffu);
    out_tiff[19] = (uint8_t)(ori & 0xffu);
}

static uint8_t *read_file_all(const char *path, size_t *out_len) {
    *out_len = 0u;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc(sz ? (size_t)sz : 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = (sz > 0) ? fread(buf, 1u, (size_t)sz, fp) : 0u;
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)sz;
    return buf;
}

static int write_file_all(const char *path, const uint8_t *buf, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    size_t wrote = len ? fwrite(buf, 1u, len, fp) : 0u;
    return (wrote == len) && (fclose(fp) == 0);
}

static void make_temp_dir(char *out, size_t cap, const char *tag) {
    static unsigned seq = 0u;
    seq++;
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(base), base);
    CHECK(n > 0 && n < sizeof(base));
    snprintf(out, cap, "%srfc_%s_%lu_%u", base, tag,
             (unsigned long)GetCurrentProcessId(), seq);
    CHECK(CreateDirectoryA(out, NULL) != 0);
#else
    snprintf(out, cap, "/tmp/rfc_%s_%lu_%u", tag, (unsigned long)getpid(), seq);
    CHECK(mkdir(out, 0700) == 0);
#endif
}

static void rm_tree(const char *dir) {
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA ffd;
    HANDLE h = FindFirstFileA(pat, &ffd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(ffd.cFileName, ".") == 0 ||
                strcmp(ffd.cFileName, "..") == 0) {
                continue;
            }
            char sub[1024];
            snprintf(sub, sizeof(sub), "%s\\%s", dir, ffd.cFileName);
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                rm_tree(sub);
            } else {
                DeleteFileA(sub);
            }
        } while (FindNextFileA(h, &ffd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0) {
                continue;
            }
            char sub[1024];
            snprintf(sub, sizeof(sub), "%s/%s", dir, de->d_name);
            struct stat st;
            if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
                rm_tree(sub);
            } else {
                unlink(sub);
            }
        }
        closedir(d);
    }
    rmdir(dir);
#endif
}

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
#ifdef _WIN32
    snprintf(out, cap, "%s\\%s", dir, name);
#else
    snprintf(out, cap, "%s/%s", dir, name);
#endif
}

static rfc_status test_alloc_frame(rfc_frame *f,
                                   const rfc_layout *lay,
                                   const rfc_metadata *meta,
                                   const rfc_allocator *alloc) {
    rfc_layout norm;
    rfc_status st = rfc_layout_normalize_stride(lay, &norm);
    if (st != RFC_OK) {
        return st;
    }
    return rfc_frame_alloc(f, &norm, meta, alloc);
}

static void fill_webp_pattern(rfc_frame *f, uint32_t seed, int force_opaque) {
    uint32_t w = f->layout.width;
    uint32_t h = f->layout.height;
    int is_rgba = (f->layout.format == RFC_SAMPLE_RGBA8) ? 1 : 0;
    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *row = f->data + (size_t)y * f->layout.stride_bytes;
        for (uint32_t x = 0u; x < w; x++) {
            uint32_t hash = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
            uint8_t r = (uint8_t)((hash >> 0) & 0xFFu);
            uint8_t g = (uint8_t)((hash >> 8) & 0xFFu);
            uint8_t b = (uint8_t)((hash >> 16) & 0xFFu);
            if (is_rgba) {
                uint8_t *px = row + (size_t)x * 4u;
                uint8_t a = 255u;
                if (!force_opaque) {
                    if (((x + y) & 3u) == 0u) {
                        a = 0u;
                        r = (uint8_t)(0xA1u ^ (x * 13u));
                        g = (uint8_t)(0x5Cu ^ (y * 17u));
                        b = (uint8_t)(0x3Eu ^ ((x ^ y) * 29u));
                    } else if (((x + y) & 3u) == 1u) {
                        a = (uint8_t)(17u + ((hash >> 24) & 0xDEu));
                    }
                }
                px[0] = r;
                px[1] = g;
                px[2] = b;
                px[3] = a;
            } else {
                uint8_t *px = row + (size_t)x * 3u;
                px[0] = r;
                px[1] = g;
                px[2] = b;
            }
        }
    }
}

static void roundtrip_webp_session(const rfc_frame *src,
                                   const rfc_metadata *meta,
                                   const rfc_setting_value *settings,
                                   size_t setting_count,
                                   rfc_frame *out_dec,
                                   rfc_image_probe *out_probe,
                                   const rfc_exec_context *exec,
                                   rfc_encoded *out_keep_encoded) {
    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_WEBP);
    CHECK(adp != NULL);

    void *es = NULL;
    CHECK(adp->open_session(exec, &es) == RFC_OK);
    rfc_encoded enc = {0};
    CHECK(adp->encode_session(es, exec, src, 1u, meta, settings, setting_count,
                              &enc) == RFC_OK);
    adp->close_session(es);
    CHECK(enc.data != NULL && enc.len >= 20u);

    void *ds = NULL;
    CHECK(adp->open_session(exec, &ds) == RFC_OK);
    rfc_image_probe pr;
    CHECK(adp->probe_session(ds, exec, enc.data, enc.len, &pr) == RFC_OK);
    if (out_probe) {
        *out_probe = pr;
    } else {
        rfc_metadata_free(&pr.container_meta, rfc_exec_allocator(exec));
    }

    rfc_frame *frames = NULL;
    size_t fcnt = 0u;
    CHECK(adp->decode_session(ds, exec, enc.data, enc.len, NULL, 0u, &frames,
                              &fcnt, NULL) == RFC_OK);
    CHECK(fcnt == 1u && frames != NULL);
    *out_dec = frames[0];
    const rfc_allocator *al = rfc_exec_allocator(exec);
    al->free(al->ctx, frames);
    adp->close_session(ds);

    if (out_keep_encoded) {
        *out_keep_encoded = enc;
    } else {
        rfc_encoded_free(&enc);
    }
    rfc_registry_release_session(RFC_FORMAT_WEBP);
}

static rfc_status stage_export_noop(rfc_job *job,
                                    const rfc_exec_context *exec,
                                    void *user_ctx) {
    (void)job;
    (void)exec;
    (void)user_ctx;
    return RFC_OK;
}

static void submit_and_wait_job(rfc_scheduler *sched,
                                const rfc_job_spec *sp,
                                rfc_job_snapshot *out_snap,
                                rfc_verify_report *out_rep) {
    rfc_job *job = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, out_snap) == RFC_OK);
    if (out_rep) {
        memset(out_rep, 0, sizeof(*out_rep));
        if (rfc_job_verify_report_get(job, out_rep) != RFC_OK) {
            out_rep->valid = 0;
        }
    }
    rfc_job_release(job);
}

/* ------------------------------------------------------------------------ */
/* Suite 1: Registration, lossless exact-RGB contract, lossy VP8, threads   */
/* ------------------------------------------------------------------------ */

static void test_registration_and_upstream_contract(void) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_webp() == RFC_OK);
    CHECK(rfc_codecs_register_webp() == RFC_ERR_DUPLICATE);

    const rfc_capability *cap = rfc_registry_find(RFC_FORMAT_WEBP);
    CHECK(cap != NULL && cap->availability == RFC_AVAILABILITY_USABLE);
    CHECK(cap->can_read == 1 && cap->can_write == 1);
    uint32_t expected_caps = RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION |
                             RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP;
    CHECK(rfc_format_metadata_caps(RFC_FORMAT_WEBP) == expected_caps);

    /* Unregister + re-register cleanly when idle */
    CHECK(rfc_codecs_unregister_webp() == RFC_OK);
    CHECK(rfc_registry_acquire_session(RFC_FORMAT_WEBP) == NULL);
    CHECK(rfc_codecs_register_webp() == RFC_OK);

    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    /* 1a. Lossless RGB8 round-trip */
    rfc_layout rgb_lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 19u, 11u, 0u, 8u};
    rfc_frame rgb_src = {0}, rgb_dec = {0};
    CHECK(test_alloc_frame(&rgb_src, &rgb_lay, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&rgb_src, 1u, 0);
    rfc_encoded rgb_enc = {0};
    roundtrip_webp_session(&rgb_src, NULL, NULL, 0u, &rgb_dec, NULL, &exec,
                           &rgb_enc);
    rfc_sample_mismatch mm;
    CHECK(rfc_verify_frames_equal(&rgb_src, 1u, &rgb_dec, 1u, &mm) == RFC_OK);
    int uw = 0, uh = 0;
    uint8_t *u_rgb = WebPDecodeRGB(rgb_enc.data, rgb_enc.len, &uw, &uh);
    CHECK(u_rgb != NULL && uw == 19 && uh == 11);
    CHECK(memcmp(u_rgb, rgb_src.data, (size_t)19u * 11u * 3u) == 0);
    WebPFree(u_rgb);
    rfc_encoded_free(&rgb_enc);
    rfc_frame_free(&rgb_src);
    rfc_frame_free(&rgb_dec);

    /* 1b. Lossless RGBA8 with A == 0 exact RGB + contrast with cfg.exact = 0 */
    rfc_layout rgba_lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 19u, 11u, 0u,
                           8u};
    rfc_frame rgba_src = {0}, rgba_dec = {0};
    CHECK(test_alloc_frame(&rgba_src, &rgba_lay, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&rgba_src, 2u, 0);
    rfc_encoded rgba_enc = {0};
    roundtrip_webp_session(&rgba_src, NULL, NULL, 0u, &rgba_dec, NULL, &exec,
                           &rgba_enc);
    CHECK(rfc_verify_frames_equal(&rgba_src, 1u, &rgba_dec, 1u, &mm) == RFC_OK);
    uint8_t *u_rgba = WebPDecodeRGBA(rgba_enc.data, rgba_enc.len, &uw, &uh);
    CHECK(u_rgba != NULL && uw == 19 && uh == 11);
    CHECK(memcmp(u_rgba, rgba_src.data, (size_t)19u * 11u * 4u) == 0);
    WebPFree(u_rgba);

    /* Contrast against libwebp default exact = 0: transparent RGB zeroes out */
    uint8_t *lossy_tr_buf = NULL;
    size_t lossy_tr_len =
        WebPEncodeLosslessRGBA(rgba_src.data, 19, 11, 19 * 4, &lossy_tr_buf);
    CHECK(lossy_tr_len > 0u && lossy_tr_buf != NULL);
    uint8_t *dec_exact0 =
        WebPDecodeRGBA(lossy_tr_buf, lossy_tr_len, &uw, &uh);
    CHECK(dec_exact0 != NULL);
    CHECK(memcmp(dec_exact0, rgba_src.data, (size_t)19u * 11u * 4u) != 0);
    WebPFree(dec_exact0);
    WebPFree(lossy_tr_buf);
    rfc_encoded_free(&rgba_enc);
    rfc_frame_free(&rgba_src);
    rfc_frame_free(&rgba_dec);

    /* 1c. All-opaque RGBA8 (all A == 255) preserves RGBA8 + RFC_ALPHA_STRAIGHT */
    rfc_frame op_src = {0}, op_dec = {0};
    CHECK(test_alloc_frame(&op_src, &rgba_lay, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&op_src, 3u, 1);
    rfc_image_probe op_pr;
    roundtrip_webp_session(&op_src, NULL, NULL, 0u, &op_dec, &op_pr, &exec,
                           NULL);
    CHECK(op_pr.first_frame.format == RFC_SAMPLE_RGBA8);
    CHECK(op_pr.first_frame.alpha == RFC_ALPHA_STRAIGHT);
    CHECK(rfc_verify_frames_equal(&op_src, 1u, &op_dec, 1u, &mm) == RFC_OK);
    rfc_metadata_free(&op_pr.container_meta, &alloc);
    rfc_frame_free(&op_src);
    rfc_frame_free(&op_dec);

    /* 1d. Lossy VP8 (RGB8) and lossy VP8X + ALPH + VP8 (RGBA8) import */
    uint8_t rgb_in[16u * 12u * 3u];
    uint8_t rgba_in[16u * 12u * 4u];
    for (uint32_t i = 0u; i < 16u * 12u; i++) {
        rgb_in[i * 3u + 0u] = (uint8_t)((i * 37u) & 0xFFu);
        rgb_in[i * 3u + 1u] = (uint8_t)((i * 73u + 19u) & 0xFFu);
        rgb_in[i * 3u + 2u] = (uint8_t)((i * 19u + 91u) & 0xFFu);
        rgba_in[i * 4u + 0u] = rgb_in[i * 3u + 0u];
        rgba_in[i * 4u + 1u] = rgb_in[i * 3u + 1u];
        rgba_in[i * 4u + 2u] = rgb_in[i * 3u + 2u];
        rgba_in[i * 4u + 3u] = (i & 1u) ? 200u : 255u;
    }
    uint8_t *vp8_bytes = NULL;
    size_t vp8_len = WebPEncodeRGB(rgb_in, 16, 12, 16 * 3, 75.0f, &vp8_bytes);
    CHECK(vp8_len > 0u && vp8_bytes != NULL);

    uint8_t *vp8a_bytes = NULL;
    size_t vp8a_len =
        WebPEncodeRGBA(rgba_in, 16, 12, 16 * 4, 75.0f, &vp8a_bytes);
    CHECK(vp8a_len > 0u && vp8a_bytes != NULL);

    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_WEBP);
    void *ds = NULL;
    CHECK(adp->open_session(&exec, &ds) == RFC_OK);
    rfc_image_probe lpr;
    CHECK(adp->probe_session(ds, &exec, vp8_bytes, vp8_len, &lpr) == RFC_OK);
    CHECK(lpr.first_frame.format == RFC_SAMPLE_RGB8 &&
          lpr.first_frame.alpha == RFC_ALPHA_NONE &&
          lpr.first_frame.width == 16u && lpr.first_frame.height == 12u);
    rfc_frame *lf = NULL;
    size_t lcnt = 0u;
    CHECK(adp->decode_session(ds, &exec, vp8_bytes, vp8_len, NULL, 0u, &lf,
                              &lcnt, NULL) == RFC_OK);
    CHECK(lcnt == 1u && lf != NULL);
    uint8_t *ref_rgb = WebPDecodeRGB(vp8_bytes, vp8_len, &uw, &uh);
    CHECK(ref_rgb && uw == 16 && uh == 12);
    CHECK(memcmp(lf[0].data, ref_rgb, (size_t)16u * 12u * 3u) == 0);
    WebPFree(ref_rgb);
    rfc_frame_free(&lf[0]);
    alloc.free(alloc.ctx, lf);

    CHECK(adp->probe_session(ds, &exec, vp8a_bytes, vp8a_len, &lpr) == RFC_OK);
    CHECK(lpr.first_frame.format == RFC_SAMPLE_RGBA8 &&
          lpr.first_frame.alpha == RFC_ALPHA_STRAIGHT);
    CHECK(adp->decode_session(ds, &exec, vp8a_bytes, vp8a_len, NULL, 0u, &lf,
                              &lcnt, NULL) == RFC_OK);
    CHECK(lcnt == 1u && lf != NULL);
    uint8_t *ref_rgba = WebPDecodeRGBA(vp8a_bytes, vp8a_len, &uw, &uh);
    CHECK(ref_rgba && uw == 16 && uh == 12);
    CHECK(memcmp(lf[0].data, ref_rgba, (size_t)16u * 12u * 4u) == 0);
    WebPFree(ref_rgba);
    rfc_frame_free(&lf[0]);
    alloc.free(alloc.ctx, lf);
    adp->close_session(ds);
    WebPFree(vp8_bytes);
    WebPFree(vp8a_bytes);

    /* 1e. Thread determinism: codec_threads=1 vs codec_threads=4 identical bytes */
    rfc_layout det_lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 32u, 24u, 0u,
                          8u};
    rfc_frame det_src = {0};
    CHECK(test_alloc_frame(&det_src, &det_lay, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&det_src, 77u, 0);

    rfc_exec_context ex4;
    tracked_alloc_state st4;
    rfc_allocator a4;
    init_exec(&ex4, &st4, &a4, NULL, 4u);

    void *es1 = NULL, *es4 = NULL;
    CHECK(adp->open_session(&exec, &es1) == RFC_OK);
    CHECK(adp->open_session(&ex4, &es4) == RFC_OK);
    rfc_encoded e1 = {0}, e4 = {0};
    CHECK(adp->encode_session(es1, &exec, &det_src, 1u, NULL, NULL, 0u, &e1) ==
          RFC_OK);
    CHECK(adp->encode_session(es4, &ex4, &det_src, 1u, NULL, NULL, 0u, &e4) ==
          RFC_OK);
    CHECK(e1.len == e4.len && memcmp(e1.data, e4.data, e1.len) == 0);
    adp->close_session(es1);
    adp->close_session(es4);
    rfc_encoded_free(&e1);
    rfc_encoded_free(&e4);
    rfc_frame_free(&det_src);
    rfc_registry_release_session(RFC_FORMAT_WEBP);

    CHECK(st.live == 0u && st4.live == 0u);
    printf("webp: registration_and_upstream_contract ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 2: ICCP/EXIF/XMP mux/demux, orientation rules, cross-format jobs   */
/* ------------------------------------------------------------------------ */

static void test_mux_icc_exif_xmp_and_cross_format(void) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    uint8_t valid_icc[132];
    fill_minimal_srgb_icc(valid_icc);
    uint8_t exif_ori6_be[26];
    fill_tiff_exif_with_ori_be(exif_ori6_be, 6u);
    static const uint8_t xmp_bytes[] =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>";
    size_t xmp_len = sizeof(xmp_bytes) - 1u;

    rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 12u, 8u, 0u, 8u};
    rfc_metadata src_m = {0};
    src_m.icc = valid_icc;
    src_m.icc_len = sizeof(valid_icc);
    src_m.orientation = RFC_ORIENTATION_ROTATE_90;
    src_m.exif = exif_ori6_be;
    src_m.exif_len = sizeof(exif_ori6_be);
    src_m.xmp = xmp_bytes;
    src_m.xmp_len = xmp_len;

    rfc_frame src = {0};
    CHECK(test_alloc_frame(&src, &lay, &src_m, &alloc) == RFC_OK);
    fill_webp_pattern(&src, 9u, 0);

    /* 2a. Muxed ICCP + EXIF + XMP round trip checked with libwebp WebPDemux */
    rfc_frame dec = {0};
    rfc_image_probe pr;
    rfc_encoded mux_enc = {0};
    roundtrip_webp_session(&src, &src_m, NULL, 0u, &dec, &pr, &exec, &mux_enc);
    CHECK(pr.container_meta.orientation == RFC_ORIENTATION_ROTATE_90);
    CHECK(pr.container_meta.icc_len == sizeof(valid_icc) &&
          memcmp(pr.container_meta.icc, valid_icc, sizeof(valid_icc)) == 0);
    CHECK(pr.container_meta.exif_len == sizeof(exif_ori6_be) &&
          memcmp(pr.container_meta.exif, exif_ori6_be, sizeof(exif_ori6_be)) ==
              0);
    CHECK(pr.container_meta.xmp_len == xmp_len &&
          memcmp(pr.container_meta.xmp, xmp_bytes, xmp_len) == 0);
    CHECK(dec.metadata.orientation == RFC_ORIENTATION_ROTATE_90);
    CHECK(dec.metadata.icc_len == sizeof(valid_icc) &&
          memcmp(dec.metadata.icc, valid_icc, sizeof(valid_icc)) == 0);
    CHECK(dec.metadata.exif_len == sizeof(exif_ori6_be) &&
          memcmp(dec.metadata.exif, exif_ori6_be, sizeof(exif_ori6_be)) == 0);
    CHECK(dec.metadata.xmp_len == xmp_len &&
          memcmp(dec.metadata.xmp, xmp_bytes, xmp_len) == 0);

    WebPData wd = {mux_enc.data, mux_enc.len};
    WebPDemuxer *dm = WebPDemux(&wd);
    CHECK(dm != NULL);
    uint32_t dflags = WebPDemuxGetI(dm, WEBP_FF_FORMAT_FLAGS);
    CHECK((dflags & ICCP_FLAG) != 0u && (dflags & EXIF_FLAG) != 0u &&
          (dflags & XMP_FLAG) != 0u && (dflags & ALPHA_FLAG) != 0u);
    WebPChunkIterator ci;
    CHECK(WebPDemuxGetChunk(dm, "ICCP", 1, &ci) != 0);
    CHECK(ci.chunk.size == sizeof(valid_icc) &&
          memcmp(ci.chunk.bytes, valid_icc, sizeof(valid_icc)) == 0);
    WebPDemuxReleaseChunkIterator(&ci);
    CHECK(WebPDemuxGetChunk(dm, "EXIF", 1, &ci) != 0);
    CHECK(ci.chunk.size == sizeof(exif_ori6_be) &&
          memcmp(ci.chunk.bytes, exif_ori6_be, sizeof(exif_ori6_be)) == 0);
    WebPDemuxReleaseChunkIterator(&ci);
    CHECK(WebPDemuxGetChunk(dm, "XMP ", 1, &ci) != 0);
    CHECK(ci.chunk.size == xmp_len &&
          memcmp(ci.chunk.bytes, xmp_bytes, xmp_len) == 0);
    WebPDemuxReleaseChunkIterator(&ci);
    WebPDemuxDelete(dm);
    rfc_metadata_free(&pr.container_meta, &alloc);
    rfc_encoded_free(&mux_enc);
    rfc_frame_free(&dec);

    /* 2b. metadata=strip emits bare VP8L without VP8X / ICCP / EXIF / XMP */
    rfc_setting_value sv_strip[1] = {{"metadata", "strip"}};
    rfc_encoded strip_enc = {0};
    roundtrip_webp_session(&src, &src_m, sv_strip, 1u, &dec, &pr, &exec,
                           &strip_enc);
    CHECK(strip_enc.len >= 20u && memcmp(strip_enc.data + 12u, "VP8L", 4u) == 0);
    CHECK(pr.container_meta.icc_len == 0u &&
          pr.container_meta.exif_len == 0u &&
          pr.container_meta.xmp_len == 0u &&
          pr.container_meta.orientation == RFC_ORIENTATION_UNSPECIFIED);
    rfc_metadata_free(&pr.container_meta, &alloc);
    rfc_encoded_free(&strip_enc);
    rfc_frame_free(&dec);
    rfc_frame_free(&src);

    /* 2c. All 5 Plan-Review #3 EXIF / orientation precedence & consistency rules */
    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_WEBP);
    void *es = NULL;
    CHECK(adp->open_session(&exec, &es) == RFC_OK);

    /* Case 1: Frame metadata overrides container metadata */
    uint8_t exif_ori3_le[26];
    CHECK(rfc_exif_build_orientation_tiff26(RFC_ORIENTATION_ROTATE_180,
                                            exif_ori3_le) == RFC_OK);
    rfc_metadata f_over = {0};
    f_over.orientation = RFC_ORIENTATION_ROTATE_180;
    f_over.exif = exif_ori3_le;
    f_over.exif_len = sizeof(exif_ori3_le);
    rfc_metadata c_base = {0};
    c_base.orientation = RFC_ORIENTATION_ROTATE_90;
    c_base.exif = exif_ori6_be;
    c_base.exif_len = sizeof(exif_ori6_be);

    rfc_frame f_fr = {0};
    CHECK(test_alloc_frame(&f_fr, &lay, &f_over, &alloc) == RFC_OK);
    fill_webp_pattern(&f_fr, 11u, 0);
    roundtrip_webp_session(&f_fr, &c_base, NULL, 0u, &dec, &pr, &exec, NULL);
    CHECK(pr.container_meta.orientation == RFC_ORIENTATION_ROTATE_180 &&
          pr.container_meta.exif_len == sizeof(exif_ori3_le) &&
          memcmp(pr.container_meta.exif, exif_ori3_le, sizeof(exif_ori3_le)) ==
              0);
    rfc_metadata_free(&pr.container_meta, &alloc);
    rfc_frame_free(&dec);
    rfc_frame_free(&f_fr);

    /* Case 2: "Exif\0\0" prefix accepted; EXIF(ori=6) vs eff_ori=1 refused */
    uint8_t prefixed_exif[32];
    memcpy(prefixed_exif, "Exif\x00\x00", 6u);
    memcpy(prefixed_exif + 6u, exif_ori6_be, sizeof(exif_ori6_be));
    rfc_metadata m_pref = {0};
    m_pref.orientation = RFC_ORIENTATION_ROTATE_90;
    m_pref.exif = prefixed_exif;
    m_pref.exif_len = sizeof(prefixed_exif);
    rfc_frame m_fr = {0};
    CHECK(test_alloc_frame(&m_fr, &lay, &m_pref, &alloc) == RFC_OK);
    fill_webp_pattern(&m_fr, 12u, 0);
    roundtrip_webp_session(&m_fr, NULL, NULL, 0u, &dec, &pr, &exec, NULL);
    CHECK(pr.container_meta.orientation == RFC_ORIENTATION_ROTATE_90 &&
          pr.container_meta.exif_len == sizeof(prefixed_exif));
    rfc_metadata_free(&pr.container_meta, &alloc);
    rfc_frame_free(&dec);

    m_fr.metadata.orientation = RFC_ORIENTATION_NORMAL;
    rfc_encoded bad_enc = {0};
    CHECK(adp->encode_session(es, &exec, &m_fr, 1u, NULL, NULL, 0u, &bad_enc) ==
          RFC_ERR_INVALID_ARGUMENT);
    rfc_frame_free(&m_fr);

    /* Case 3: EXIF lacking tag 0x0112 allowed with Normal, refused with Rotate90 */
    uint8_t exif_no_ori[26];
    fill_tiff_exif_be_no_ori(exif_no_ori);
    rfc_metadata m_no = {0};
    m_no.orientation = RFC_ORIENTATION_NORMAL;
    m_no.exif = exif_no_ori;
    m_no.exif_len = sizeof(exif_no_ori);
    rfc_frame fr_no = {0};
    CHECK(test_alloc_frame(&fr_no, &lay, &m_no, &alloc) == RFC_OK);
    fill_webp_pattern(&fr_no, 13u, 0);
    roundtrip_webp_session(&fr_no, NULL, NULL, 0u, &dec, &pr, &exec, NULL);
    CHECK(pr.container_meta.orientation == RFC_ORIENTATION_UNSPECIFIED &&
          pr.container_meta.exif_len == sizeof(exif_no_ori) &&
          memcmp(pr.container_meta.exif, exif_no_ori, sizeof(exif_no_ori)) == 0);
    rfc_metadata_free(&pr.container_meta, &alloc);
    rfc_frame_free(&dec);

    fr_no.metadata.orientation = RFC_ORIENTATION_ROTATE_90;
    CHECK(adp->encode_session(es, &exec, &fr_no, 1u, NULL, NULL, 0u,
                              &bad_enc) == RFC_ERR_INVALID_ARGUMENT);
    rfc_frame_free(&fr_no);

    /* Case 4 & 5: Absent EXIF + Rotate90 synthesizes 26-byte TIFF; Normal omits */
    rfc_metadata m_syn = {0};
    m_syn.orientation = RFC_ORIENTATION_ROTATE_90;
    rfc_frame fr_syn = {0};
    CHECK(test_alloc_frame(&fr_syn, &lay, &m_syn, &alloc) == RFC_OK);
    fill_webp_pattern(&fr_syn, 14u, 0);
    uint8_t expected_tiff26[26];
    CHECK(rfc_exif_build_orientation_tiff26(RFC_ORIENTATION_ROTATE_90,
                                            expected_tiff26) == RFC_OK);
    roundtrip_webp_session(&fr_syn, NULL, NULL, 0u, &dec, &pr, &exec, NULL);
    CHECK(pr.container_meta.orientation == RFC_ORIENTATION_ROTATE_90 &&
          pr.container_meta.exif_len == 26u &&
          memcmp(pr.container_meta.exif, expected_tiff26, 26u) == 0);
    rfc_metadata_free(&pr.container_meta, &alloc);
    rfc_frame_free(&dec);

    fr_syn.metadata.orientation = RFC_ORIENTATION_NORMAL;
    roundtrip_webp_session(&fr_syn, NULL, NULL, 0u, &dec, &pr, &exec, NULL);
    CHECK(pr.container_meta.exif_len == 0u &&
          pr.container_meta.orientation == RFC_ORIENTATION_UNSPECIFIED);
    rfc_metadata_free(&pr.container_meta, &alloc);
    rfc_frame_free(&dec);
    rfc_frame_free(&fr_syn);

    /* Finding #2: EXIF orientation=6 with UNSPECIFIED struct field roundtrips
     * to ROTATE_90 and preserves end-to-end across WebP -> WebP scheduler. */
    rfc_encoded unspec_wp = {0};
    {
        rfc_metadata m_unspec = {0};
        m_unspec.orientation = RFC_ORIENTATION_UNSPECIFIED;
        m_unspec.exif = expected_tiff26;
        m_unspec.exif_len = sizeof(expected_tiff26);
        rfc_frame fr_unspec = {0};
        CHECK(test_alloc_frame(&fr_unspec, &lay, &m_unspec, &alloc) == RFC_OK);
        fill_webp_pattern(&fr_unspec, 15u, 0);
        roundtrip_webp_session(&fr_unspec, NULL, NULL, 0u, &dec, &pr, &exec,
                               NULL);
        CHECK(pr.container_meta.orientation == RFC_ORIENTATION_ROTATE_90 &&
              pr.container_meta.exif_len == 26u &&
              memcmp(pr.container_meta.exif, expected_tiff26, 26u) == 0 &&
              dec.metadata.orientation == RFC_ORIENTATION_ROTATE_90);
        rfc_metadata_free(&pr.container_meta, &alloc);
        rfc_frame_free(&dec);
        CHECK(adp->encode_session(es, &exec, &fr_unspec, 1u, &m_unspec, NULL,
                                  0u, &unspec_wp) == RFC_OK);
        rfc_frame_free(&fr_unspec);
    }

    adp->close_session(es);
    rfc_registry_release_session(RFC_FORMAT_WEBP);

    /* 2d. Cross-format scheduler jobs: PNG <-> WebP and WebP -> PNG/BMP/FOLD */
    rfc_scheduler_config scfg;
    rfc_scheduler_config_default(&scfg);
    rfc_scheduler *sched = rfc_scheduler_create(&scfg);
    CHECK(sched != NULL);

    /* Build PNG with iCCP + injected eXIf Ori=6 */
    rfc_layout png_lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 0u, 8u};
    rfc_metadata png_m = {0};
    png_m.icc = valid_icc;
    png_m.icc_len = sizeof(valid_icc);
    png_m.orientation = RFC_ORIENTATION_NORMAL;
    rfc_frame png_fr = {0};
    CHECK(test_alloc_frame(&png_fr, &png_lay, &png_m, &alloc) == RFC_OK);
    fill_webp_pattern(&png_fr, 20u, 0);

    const rfc_session_adapter *png_adp =
        rfc_registry_acquire_session(RFC_FORMAT_PNG);
    void *pes = NULL;
    CHECK(png_adp->open_session(&exec, &pes) == RFC_OK);
    rfc_encoded png_icc_base = {0};
    CHECK(png_adp->encode_session(pes, &exec, &png_fr, 1u, &png_m, NULL, 0u,
                                  &png_icc_base) == RFC_OK);
    size_t png_icc_ori_len = 0u;
    uint8_t *png_icc_ori = insert_png_chunk_before_idat(
        png_icc_base.data, png_icc_base.len, "eXIf", exif_ori6_be,
        sizeof(exif_ori6_be), &png_icc_ori_len);
    rfc_encoded_free(&png_icc_base);

    /* Bare PNG for gAMA insertion */
    rfc_encoded png_bare = {0};
    CHECK(png_adp->encode_session(pes, &exec, &png_fr, 1u, NULL,
                                  sv_strip, 1u, &png_bare) == RFC_OK);
    png_adp->close_session(pes);
    rfc_registry_release_session(RFC_FORMAT_PNG);
    rfc_frame_free(&png_fr);

    uint8_t gama_pl[4];
    wr32be(gama_pl, 45455u);
    size_t png_gama_len = 0u;
    uint8_t *png_gama = insert_png_chunk_before_idat(
        png_bare.data, png_bare.len, "gAMA", gama_pl, 4u, &png_gama_len);
    rfc_encoded_free(&png_bare);

    rfc_setting_value sv_pres[1] = {{"metadata", "preserve"}};
    rfc_job_spec sp;
    rfc_job_snapshot snap;
    rfc_verify_report rep;

    /* PNG(iCCP + eXIf Ori=6) -> WebP preserve -> SUCCEEDED */
    memset(&sp, 0, sizeof(sp));
    sp.source_format = RFC_FORMAT_PNG;
    sp.target_format = RFC_FORMAT_WEBP;
    sp.source_bytes = png_icc_ori;
    sp.source_size = png_icc_ori_len;
    sp.settings = sv_pres;
    sp.setting_count = 1u;
    sp.expected_working_set_bytes = 8u * 1024u * 1024u;
    sp.has_stage_ops = 1;
    sp.stage_ops.on_export = stage_export_noop;
    submit_and_wait_job(sched, &sp, &snap, &rep);
    CHECK(snap.state == RFC_JOB_SUCCEEDED &&
          rep.meta_outcome == RFC_VERIFY_META_PRESERVED);
    free(png_icc_ori);

    /* Finding #2: WebP (EXIF ori=6 only) -> WebP preserve via scheduler */
    sp.source_format = RFC_FORMAT_WEBP;
    sp.source_bytes = unspec_wp.data;
    sp.source_size = unspec_wp.len;
    submit_and_wait_job(sched, &sp, &snap, &rep);
    CHECK(snap.state == RFC_JOB_SUCCEEDED &&
          rep.meta_outcome == RFC_VERIFY_META_PRESERVED);
    rfc_encoded_free(&unspec_wp);
    sp.source_format = RFC_FORMAT_PNG;

    /* PNG(gAMA) -> WebP preserve ack=0 -> RFC_ERR_METADATA_POLICY */
    sp.source_bytes = png_gama;
    sp.source_size = png_gama_len;
    submit_and_wait_job(sched, &sp, &snap, &rep);
    CHECK(snap.state == RFC_JOB_FAILED &&
          snap.status == RFC_ERR_METADATA_POLICY &&
          rep.meta_outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED &&
          (rep.metadata.unsupported_items & RFC_META_ITEM_ICC) != 0u);

    /* PNG(gAMA) -> WebP preserve + ack=ICC -> SUCCEEDED */
    sp.acknowledged_unsupported_metadata = RFC_META_ITEM_ICC;
    submit_and_wait_job(sched, &sp, &snap, &rep);
    CHECK(snap.state == RFC_JOB_SUCCEEDED &&
          rep.meta_outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED);

    /* PNG(gAMA) -> WebP strip -> SUCCEEDED + STRIPPED_EXPLICITLY */
    sp.acknowledged_unsupported_metadata = 0u;
    sp.settings = sv_strip;
    submit_and_wait_job(sched, &sp, &snap, &rep);
    CHECK(snap.state == RFC_JOB_SUCCEEDED &&
          rep.meta_outcome == RFC_VERIFY_META_STRIPPED_EXPLICITLY);
    free(png_gama);

    /* Encode RGB8 WebP(ICCP + EXIF Ori=6 + XMP) and test WebP -> PNG/BMP/FOLD */
    rfc_metadata wsrc_m = {valid_icc, sizeof(valid_icc), RFC_ORIENTATION_ROTATE_90,
                           0u, {0}, exif_ori6_be, sizeof(exif_ori6_be),
                           xmp_bytes, xmp_len};
    rfc_frame wsrc_fr = {0}, wdec_tmp = {0};
    CHECK(test_alloc_frame(&wsrc_fr, &png_lay, &wsrc_m, &alloc) == RFC_OK);
    fill_webp_pattern(&wsrc_fr, 21u, 0);
    rfc_encoded webp_full = {0};
    roundtrip_webp_session(&wsrc_fr, &wsrc_m, NULL, 0u, &wdec_tmp, NULL, &exec,
                           &webp_full);
    rfc_frame_free(&wdec_tmp);
    rfc_frame_free(&wsrc_fr);

    memset(&sp, 0, sizeof(sp));
    sp.source_format = RFC_FORMAT_WEBP;
    sp.target_format = RFC_FORMAT_PNG;
    sp.source_bytes = webp_full.data;
    sp.source_size = webp_full.len;
    sp.settings = sv_pres;
    sp.setting_count = 1u;
    sp.expected_working_set_bytes = 8u * 1024u * 1024u;
    sp.has_stage_ops = 1;
    sp.stage_ops.on_export = stage_export_noop;

    /* WebP(ICCP+EXIF+XMP) -> PNG refuses Ori+EXIF+XMP (26), partial ack=XMP
     * still refuses Ori+EXIF, full ack (26) succeeds */
    uint32_t png_unsup =
        RFC_META_ITEM_ORIENTATION | RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP;
    submit_and_wait_job(sched, &sp, &snap, &rep);
    CHECK(snap.state == RFC_JOB_FAILED &&
          snap.status == RFC_ERR_METADATA_POLICY &&
          rep.metadata.unsupported_items == png_unsup);
    sp.acknowledged_unsupported_metadata = RFC_META_ITEM_XMP;
    submit_and_wait_job(sched, &sp, &snap, &rep);
    CHECK(snap.state == RFC_JOB_FAILED &&
          snap.status == RFC_ERR_METADATA_POLICY);
    sp.acknowledged_unsupported_metadata = png_unsup;
    submit_and_wait_job(sched, &sp, &snap, &rep);
    CHECK(snap.state == RFC_JOB_SUCCEEDED &&
          rep.meta_outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED);

    /* WebP(ICCP+EXIF+XMP) -> BMP & RASTERFOLD refuse preserve ack=0, pass strip */
    rfc_format_id no_meta_dsts[2] = {RFC_FORMAT_BMP, RFC_FORMAT_RASTERFOLD};
    for (int di = 0; di < 2; di++) {
        sp.target_format = no_meta_dsts[di];
        sp.settings = sv_pres;
        sp.acknowledged_unsupported_metadata = 0u;
        submit_and_wait_job(sched, &sp, &snap, &rep);
        CHECK(snap.state == RFC_JOB_FAILED &&
              snap.status == RFC_ERR_METADATA_POLICY &&
              rep.metadata.unsupported_items ==
                  rfc_format_metadata_caps(RFC_FORMAT_WEBP));
        sp.settings = sv_strip;
        submit_and_wait_job(sched, &sp, &snap, &rep);
        CHECK(snap.state == RFC_JOB_SUCCEEDED &&
              rep.meta_outcome == RFC_VERIFY_META_STRIPPED_EXPLICITLY);
    }

    rfc_encoded_free(&webp_full);
    rfc_scheduler_destroy(sched);
    CHECK(st.live == 0u);
    printf("webp: mux_icc_exif_xmp_and_cross_format ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 3: Unsupported formats/alpha, animation, limits, budget, compare   */
/* ------------------------------------------------------------------------ */

static void test_refusals_limits_and_budget(void) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_WEBP);
    void *es = NULL;
    CHECK(adp->open_session(&exec, &es) == RFC_OK);

    /* 3a. Unsupported sample formats & bits_per_sample */
    rfc_sample_format bad_samples[6] = {
        RFC_SAMPLE_GRAY8, RFC_SAMPLE_GRAY16, RFC_SAMPLE_RGB16,
        RFC_SAMPLE_RGBA16, RFC_SAMPLE_RGB_HALF, RFC_SAMPLE_RGB_FLOAT
    };
    uint8_t bad_bps[6] = {8u, 16u, 16u, 16u, 16u, 32u};
    for (int i = 0; i < 6; i++) {
        rfc_layout bl = {bad_samples[i],
                         bad_samples[i] == RFC_SAMPLE_RGBA16 ? RFC_ALPHA_STRAIGHT
                                                             : RFC_ALPHA_NONE,
                         4u, 4u, 0u, bad_bps[i]};
        rfc_frame bf = {0};
        CHECK(test_alloc_frame(&bf, &bl, NULL, &alloc) == RFC_OK);
        rfc_encoded out = {0};
        CHECK(adp->encode_session(es, &exec, &bf, 1u, NULL, NULL, 0u, &out) ==
              RFC_ERR_UNSUPPORTED_SAMPLE);
        rfc_frame_free(&bf);
    }

    /* Non-8 bps on RGB8 */
    rfc_layout bps5 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 0u, 5u};
    rfc_frame f_bps5 = {0};
    CHECK(test_alloc_frame(&f_bps5, &bps5, NULL, &alloc) == RFC_OK);
    rfc_encoded out = {0};
    CHECK(adp->encode_session(es, &exec, &f_bps5, 1u, NULL, NULL, 0u, &out) ==
          RFC_ERR_UNSUPPORTED_SAMPLE);
    rfc_frame_free(&f_bps5);

    /* Unsupported alpha modes: RFC_ALPHA_OPAQUE on RGB8/RGBA8 & PREMULTIPLIED */
    rfc_layout bad_a_lays[3] = {
        {RFC_SAMPLE_RGB8, RFC_ALPHA_OPAQUE, 4u, 4u, 0u, 8u},
        {RFC_SAMPLE_RGBA8, RFC_ALPHA_OPAQUE, 4u, 4u, 0u, 8u},
        {RFC_SAMPLE_RGBA8, RFC_ALPHA_PREMULTIPLIED, 4u, 4u, 0u, 8u}
    };
    for (int i = 0; i < 3; i++) {
        rfc_frame af = {0};
        CHECK(test_alloc_frame(&af, &bad_a_lays[i], NULL, &alloc) == RFC_OK);
        CHECK(adp->encode_session(es, &exec, &af, 1u, NULL, NULL, 0u, &out) ==
              RFC_ERR_UNSUPPORTED_ALPHA);
        rfc_frame_free(&af);
    }

    /* Frame counts 0 & 2 + rule conflict strip + byte-identical */
    rfc_layout ok_lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 0u, 8u};
    rfc_frame pair[2];
    memset(pair, 0, sizeof(pair));
    CHECK(test_alloc_frame(&pair[0], &ok_lay, NULL, &alloc) == RFC_OK);
    CHECK(test_alloc_frame(&pair[1], &ok_lay, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&pair[0], 30u, 0);
    fill_webp_pattern(&pair[1], 31u, 0);
    CHECK(adp->encode_session(es, &exec, NULL, 0u, NULL, NULL, 0u, &out) ==
          RFC_ERR_INVALID_ARGUMENT);
    CHECK(adp->encode_session(es, &exec, pair, 2u, NULL, NULL, 0u, &out) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);

    rfc_setting_value sv_bad[2] = {
        {"metadata", "strip"}, {"exactness", "byte-identical"}
    };
    CHECK(adp->encode_session(es, &exec, &pair[0], 1u, NULL, sv_bad, 2u,
                              &out) == RFC_ERR_UNSUPPORTED_COMBINATION);

    /* Dimension > 16383 limit exceeded */
    static uint8_t dummy_row[16384u * 3u];
    rfc_frame wide_fr = {0};
    wide_fr.layout.format = RFC_SAMPLE_RGB8;
    wide_fr.layout.alpha = RFC_ALPHA_NONE;
    wide_fr.layout.width = 16384u;
    wide_fr.layout.height = 1u;
    wide_fr.layout.stride_bytes = sizeof(dummy_row);
    wide_fr.layout.bits_per_sample = 8u;
    wide_fr.data = dummy_row;
    wide_fr.data_len = sizeof(dummy_row);
    CHECK(adp->encode_session(es, &exec, &wide_fr, 1u, NULL, NULL, 0u, &out) ==
          RFC_ERR_LIMIT_EXCEEDED);

    /* Build a valid small WebP and real 2-frame animated WebP via WebPMux */
    rfc_encoded small_enc = {0};
    uint8_t valid_icc[132];
    fill_minimal_srgb_icc(valid_icc);
    rfc_metadata sm_m = {valid_icc, sizeof(valid_icc), RFC_ORIENTATION_NORMAL,
                         0u, {0}, NULL, 0u, NULL, 0u};
    CHECK(adp->encode_session(es, &exec, &pair[0], 1u, &sm_m, NULL, 0u,
                              &small_enc) == RFC_OK);

    WebPMux *amux = WebPMuxNew();
    CHECK(amux != NULL);
    WebPMuxFrameInfo mfi;
    memset(&mfi, 0, sizeof(mfi));
    mfi.bitstream.bytes = small_enc.data;
    mfi.bitstream.size = small_enc.len;
    mfi.duration = 50;
    mfi.id = WEBP_CHUNK_ANMF;
    CHECK(WebPMuxPushFrame(amux, &mfi, 0) == WEBP_MUX_OK);
    CHECK(WebPMuxPushFrame(amux, &mfi, 0) == WEBP_MUX_OK);
    WebPMuxAnimParams ap = {0, 0};
    CHECK(WebPMuxSetAnimationParams(amux, &ap) == WEBP_MUX_OK);
    WebPData anim_data;
    WebPDataInit(&anim_data);
    CHECK(WebPMuxAssemble(amux, &anim_data) == WEBP_MUX_OK);
    WebPMuxDelete(amux);

    /* Reset alloc tracker & verify zero allocs on animation and limit refusals */
    rfc_frame_free(&pair[0]);
    rfc_frame_free(&pair[1]);
    size_t live_base = st.live;
    CHECK(live_base == 1u);
    st.count = 0u;

    rfc_image_probe apr;
    rfc_frame *dec_f = NULL;
    size_t dec_c = 0u;
    CHECK(adp->probe_session(es, &exec, anim_data.bytes, anim_data.size, &apr) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(adp->decode_session(es, &exec, anim_data.bytes, anim_data.size, NULL,
                              0u, &dec_f, &dec_c, NULL) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(st.count == 0u && st.live == live_base);
    WebPDataClear(&anim_data);

    /* limits_override: max_width, max_decoded_bytes, max_metadata_bytes (sum) */
    rfc_exec_context lim_ex = exec;
    lim_ex.limits.max_width = 4u;
    CHECK(adp->probe_session(es, &lim_ex, small_enc.data, small_enc.len,
                             &apr) == RFC_ERR_LIMIT_EXCEEDED);
    CHECK(adp->decode_session(es, &lim_ex, small_enc.data, small_enc.len, NULL,
                              0u, &dec_f, &dec_c, NULL) ==
          RFC_ERR_LIMIT_EXCEEDED);
    lim_ex = exec;
    lim_ex.limits.max_decoded_bytes = 64u;
    CHECK(adp->probe_session(es, &lim_ex, small_enc.data, small_enc.len,
                             &apr) == RFC_ERR_LIMIT_EXCEEDED);
    lim_ex = exec;
    lim_ex.limits.max_metadata_bytes = 64u;
    CHECK(adp->probe_session(es, &lim_ex, small_enc.data, small_enc.len,
                             &apr) == RFC_ERR_LIMIT_EXCEEDED);
    CHECK(st.count == 0u && st.live == live_base);

    /* Combined ICCP + EXIF + XMP sum exceeding max_metadata_bytes */
    uint8_t chunk60[60];
    memset(chunk60, 'A', sizeof(chunk60));
    uint8_t exif40[40];
    uint8_t t26[26];
    fill_tiff_exif_be_no_ori(t26);
    memset(exif40, 0, sizeof(exif40));
    memcpy(exif40, t26, 26u);
    rfc_metadata comb_m = {0};
    comb_m.icc = valid_icc;
    comb_m.icc_len = sizeof(valid_icc);
    comb_m.exif = exif40;
    comb_m.exif_len = sizeof(exif40);
    comb_m.xmp = chunk60;
    comb_m.xmp_len = sizeof(chunk60);
    rfc_frame comb_fr = {0};
    CHECK(test_alloc_frame(&comb_fr, &ok_lay, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&comb_fr, 32u, 0);
    rfc_encoded comb_enc = {0};
    CHECK(adp->encode_session(es, &exec, &comb_fr, 1u, &comb_m, NULL, 0u,
                              &comb_enc) == RFC_OK);
    lim_ex = exec;
    lim_ex.limits.max_metadata_bytes = 200u; /* 132 + 40 + 60 = 232 > 200 */
    live_base = st.live;
    st.count = 0u;
    CHECK(adp->probe_session(es, &lim_ex, comb_enc.data, comb_enc.len, &apr) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(adp->encode_session(es, &lim_ex, &comb_fr, 1u, &comb_m, NULL, 0u,
                              &out) == RFC_ERR_LIMIT_EXCEEDED);
    CHECK(st.count == 0u && st.live == live_base);
    rfc_encoded_free(&comb_enc);

    /* Finding #3: synthesized EXIF for non-Normal orientation counts toward
     * max_metadata_bytes under preserve policy at zero allocations. */
    {
        rfc_metadata m_syn_lim = {0};
        m_syn_lim.orientation = RFC_ORIENTATION_ROTATE_90;
        rfc_frame fr_syn_lim = {0};
        CHECK(test_alloc_frame(&fr_syn_lim, &ok_lay, &m_syn_lim, &alloc) ==
              RFC_OK);
        fill_webp_pattern(&fr_syn_lim, 33u, 0);
        lim_ex = exec;
        lim_ex.limits.max_metadata_bytes = 25u;
        live_base = st.live;
        st.count = 0u;
        CHECK(adp->encode_session(es, &lim_ex, &fr_syn_lim, 1u, &m_syn_lim,
                                  NULL, 0u, &out) == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(st.count == 0u && st.live == live_base);

        lim_ex.limits.max_metadata_bytes = 26u;
        rfc_encoded syn_ok = {0};
        CHECK(adp->encode_session(es, &lim_ex, &fr_syn_lim, 1u, &m_syn_lim,
                                  NULL, 0u, &syn_ok) == RFC_OK);
        CHECK(adp->probe_session(es, &lim_ex, syn_ok.data, syn_ok.len, &apr) ==
              RFC_OK);
        rfc_metadata_free(&apr.container_meta, &alloc);
        lim_ex.limits.max_metadata_bytes = 25u;
        live_base = st.live;
        st.count = 0u;
        CHECK(adp->probe_session(es, &lim_ex, syn_ok.data, syn_ok.len, &apr) ==
              RFC_ERR_LIMIT_EXCEEDED);
        CHECK(st.count == 0u && st.live == live_base);
        rfc_encoded_free(&syn_ok);

        /* ICC(132) + XMP(40) + Rotate90 synth EXIF(26) = 198 > 180 */
        rfc_metadata comb_syn_m = {0};
        comb_syn_m.icc = valid_icc;
        comb_syn_m.icc_len = sizeof(valid_icc);
        comb_syn_m.orientation = RFC_ORIENTATION_ROTATE_90;
        comb_syn_m.xmp = chunk60;
        comb_syn_m.xmp_len = 40u;
        lim_ex.limits.max_metadata_bytes = 180u;
        live_base = st.live;
        st.count = 0u;
        CHECK(adp->encode_session(es, &lim_ex, &fr_syn_lim, 1u, &comb_syn_m,
                                  NULL, 0u, &out) == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(st.count == 0u && st.live == live_base);
        rfc_frame_free(&fr_syn_lim);
    }

    /* 3b. rfc_budget enforcement on decode, encode enc_guard, encode mux_guard */
    rfc_budget *dec_b = rfc_budget_create(32u * 1024u);
    CHECK(dec_b != NULL);
    rfc_allocator dba;
    rfc_budget_allocator(dec_b, &dba);
    rfc_exec_context b_exec;
    memset(&b_exec, 0, sizeof(b_exec));
    rfc_limits_default(&b_exec.limits);
    b_exec.codec_threads = 1u;
    b_exec.allocator = &dba;
    b_exec.budget = dec_b;
    CHECK(adp->decode_session(es, &b_exec, small_enc.data, small_enc.len, NULL,
                              0u, &dec_f, &dec_c, NULL) ==
          RFC_ERR_OUT_OF_MEMORY);
    rfc_budget_stats bs;
    rfc_budget_stats_get(dec_b, &bs);
    CHECK(bs.rejected_count > 0u && bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(dec_b);

    /* Finding #1: decode scratch boundary (~263 KiB fails at 260000, passes 320 KiB) */
    dec_b = rfc_budget_create(260000u);
    CHECK(dec_b != NULL);
    rfc_budget_allocator(dec_b, &dba);
    b_exec.allocator = &dba;
    b_exec.budget = dec_b;
    CHECK(adp->decode_session(es, &b_exec, small_enc.data, small_enc.len, NULL,
                              0u, &dec_f, &dec_c, NULL) ==
          RFC_ERR_OUT_OF_MEMORY);
    rfc_budget_stats_get(dec_b, &bs);
    CHECK(bs.rejected_count > 0u && bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(dec_b);

    dec_b = rfc_budget_create(320u * 1024u);
    CHECK(dec_b != NULL);
    rfc_budget_allocator(dec_b, &dba);
    b_exec.allocator = &dba;
    b_exec.budget = dec_b;
    CHECK(adp->decode_session(es, &b_exec, small_enc.data, small_enc.len, NULL,
                              0u, &dec_f, &dec_c, NULL) == RFC_OK);
    rfc_budget_stats_get(dec_b, &bs);
    CHECK(bs.peak_allocated_bytes >= 262144u && dec_c == 1u && dec_f != NULL);
    rfc_frame_free(&dec_f[0]);
    dba.free(dba.ctx, dec_f);
    dec_f = NULL;
    dec_c = 0u;
    rfc_budget_stats_get(dec_b, &bs);
    CHECK(bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(dec_b);

    /* Encode enc_guard budget fail (tiny and tight 1-thread HASH_SIZE boundary) */
    rfc_budget *enc_b = rfc_budget_create(64u * 1024u);
    CHECK(enc_b != NULL);
    rfc_allocator eba;
    rfc_budget_allocator(enc_b, &eba);
    b_exec.allocator = &eba;
    b_exec.budget = enc_b;
    CHECK(adp->encode_session(es, &b_exec, &comb_fr, 1u, &comb_m, NULL, 0u,
                              &out) == RFC_ERR_OUT_OF_MEMORY);
    rfc_budget_stats_get(enc_b, &bs);
    CHECK(bs.rejected_count > 0u && bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(enc_b);

    enc_b = rfc_budget_create(1126223u);
    CHECK(enc_b != NULL);
    rfc_budget_allocator(enc_b, &eba);
    b_exec.allocator = &eba;
    b_exec.budget = enc_b;
    b_exec.codec_threads = 1u;
    CHECK(adp->encode_session(es, &b_exec, &comb_fr, 1u, NULL, NULL, 0u,
                              &out) == RFC_ERR_OUT_OF_MEMORY);
    rfc_budget_stats_get(enc_b, &bs);
    CHECK(bs.rejected_count > 0u && bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(enc_b);

    enc_b = rfc_budget_create(1200u * 1024u);
    CHECK(enc_b != NULL);
    rfc_budget_allocator(enc_b, &eba);
    b_exec.allocator = &eba;
    b_exec.budget = enc_b;
    b_exec.codec_threads = 1u;
    CHECK(adp->encode_session(es, &b_exec, &comb_fr, 1u, NULL, NULL, 0u,
                              &out) == RFC_OK);
    rfc_budget_stats_get(enc_b, &bs);
    CHECK(bs.peak_allocated_bytes >= 1126224u);
    rfc_encoded_free(&out);
    rfc_budget_stats_get(enc_b, &bs);
    CHECK(bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(enc_b);

    /* Finding #1: codec_threads = 4 doubles worker reservation (2x 1126224) */
    enc_b = rfc_budget_create(1280u * 1024u);
    CHECK(enc_b != NULL);
    rfc_budget_allocator(enc_b, &eba);
    b_exec.allocator = &eba;
    b_exec.budget = enc_b;
    b_exec.codec_threads = 4u;
    CHECK(adp->encode_session(es, &b_exec, &comb_fr, 1u, NULL, NULL, 0u,
                              &out) == RFC_ERR_OUT_OF_MEMORY);
    rfc_budget_stats_get(enc_b, &bs);
    CHECK(bs.rejected_count > 0u && bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(enc_b);

    enc_b = rfc_budget_create(2560u * 1024u);
    CHECK(enc_b != NULL);
    rfc_budget_allocator(enc_b, &eba);
    b_exec.allocator = &eba;
    b_exec.budget = enc_b;
    b_exec.codec_threads = 4u;
    CHECK(adp->encode_session(es, &b_exec, &comb_fr, 1u, NULL, NULL, 0u,
                              &out) == RFC_OK);
    rfc_budget_stats_get(enc_b, &bs);
    CHECK(bs.peak_allocated_bytes >= 2u * 1126224u);
    rfc_encoded_free(&out);
    rfc_budget_stats_get(enc_b, &bs);
    CHECK(bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(enc_b);
    b_exec.codec_threads = 1u;

    /* Encode mux_guard budget fail: allow enc_guard (~1126224 B for 8x8)
     * plus argb (256 B) + wbuf (~1 KiB) <= 1200 KiB, then fail mux_guard on 1400 KiB ICC */
    size_t big_icc_len = 1400u * 1024u;
    uint8_t *big_icc = (uint8_t *)calloc(1u, big_icc_len);
    CHECK(big_icc != NULL);
    memset(big_icc, 0x5Au, big_icc_len);
    wr32be(big_icc, (uint32_t)big_icc_len);
    rfc_metadata mux_m = {big_icc, big_icc_len, RFC_ORIENTATION_NORMAL, 0u,
                          {0}, NULL, 0u, NULL, 0u};
    rfc_budget *mux_b = rfc_budget_create(1200u * 1024u);
    CHECK(mux_b != NULL);
    rfc_allocator mba;
    rfc_budget_allocator(mux_b, &mba);
    b_exec.allocator = &mba;
    b_exec.budget = mux_b;
    CHECK(adp->encode_session(es, &b_exec, &comb_fr, 1u, &mux_m, NULL, 0u,
                              &out) == RFC_ERR_OUT_OF_MEMORY);
    rfc_budget_stats_get(mux_b, &bs);
    CHECK(bs.rejected_count > 0u && bs.current_allocated_bytes == 0u);
    rfc_budget_destroy(mux_b);
    free(big_icc);

    rfc_frame_free(&comb_fr);
    adp->close_session(es);
    rfc_registry_release_session(RFC_FORMAT_WEBP);

    /* 3c. rfc_run_comparison covering PNG -> WebP and WebP -> PNG/FOLD/WEBP */
    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "webp_cmp");

    rfc_frame cmp_fr = {0};
    rfc_layout cmp_lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 16u, 16u, 0u, 8u};
    CHECK(test_alloc_frame(&cmp_fr, &cmp_lay, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&cmp_fr, 44u, 0);
    const rfc_session_adapter *png_adp =
        rfc_registry_acquire_session(RFC_FORMAT_PNG);
    void *ps = NULL;
    CHECK(png_adp->open_session(&exec, &ps) == RFC_OK);
    rfc_encoded cmp_png = {0};
    CHECK(png_adp->encode_session(ps, &exec, &cmp_fr, 1u, NULL, NULL, 0u,
                                  &cmp_png) == RFC_OK);
    png_adp->close_session(ps);
    rfc_registry_release_session(RFC_FORMAT_PNG);
    rfc_frame_free(&cmp_fr);

    char c_webp[1024], c_png[1024], c_fold[1024], c_webp2[1024];
    join_path(c_webp, sizeof(c_webp), tmp, "out_a.webp");
    rfc_compare_candidate_spec cand_pw[1] = {{0}};
    cand_pw[0].target_format = RFC_FORMAT_WEBP;
    cand_pw[0].dest_path_utf8 = c_webp;

    rfc_compare_spec csp;
    memset(&csp, 0, sizeof(csp));
    csp.source_bytes = cmp_png.data;
    csp.source_size = cmp_png.len;
    csp.source_format = RFC_FORMAT_PNG;
    csp.staging_dir_utf8 = tmp;
    csp.requested_threads = 1u;
    csp.metadata_policy = RFC_META_POLICY_PRESERVE;
    csp.warmup_iterations = 0u;
    csp.measured_iterations = 1u;
    csp.candidates = cand_pw;
    csp.candidate_count = 1u;

    rfc_comparison_report *creport =
        (rfc_comparison_report *)calloc(1u, sizeof(*creport));
    CHECK(creport != NULL);
    CHECK(rfc_compare_run(&csp, creport) == RFC_OK);
    CHECK(creport->completed_count == 1u &&
          creport->candidates[0].outcome == RFC_CANDIDATE_COMPLETED &&
          creport->candidates[0].verify_report.valid == 1 &&
          creport->candidates[0].size.produced_file_bytes > 0u);
    free(creport);
    rfc_encoded_free(&cmp_png);

    join_path(c_png, sizeof(c_png), tmp, "out_b.png");
    join_path(c_fold, sizeof(c_fold), tmp, "out_b.fold");
    join_path(c_webp2, sizeof(c_webp2), tmp, "out_b.webp");
    rfc_compare_candidate_spec cand_wp[3];
    memset(cand_wp, 0, sizeof(cand_wp));
    cand_wp[0].target_format = RFC_FORMAT_PNG;
    cand_wp[0].dest_path_utf8 = c_png;
    cand_wp[1].target_format = RFC_FORMAT_RASTERFOLD;
    cand_wp[1].dest_path_utf8 = c_fold;
    cand_wp[2].target_format = RFC_FORMAT_WEBP;
    cand_wp[2].dest_path_utf8 = c_webp2;

    memset(&csp, 0, sizeof(csp));
    csp.source_bytes = small_enc.data;
    csp.source_size = small_enc.len;
    csp.source_format = RFC_FORMAT_WEBP;
    csp.staging_dir_utf8 = tmp;
    csp.requested_threads = 1u;
    csp.metadata_policy = RFC_META_POLICY_PRESERVE;
    csp.acknowledged_unsupported_metadata = RFC_META_ITEM_ICC;
    csp.warmup_iterations = 0u;
    csp.measured_iterations = 1u;
    csp.candidates = cand_wp;
    csp.candidate_count = 3u;

    creport = (rfc_comparison_report *)calloc(1u, sizeof(*creport));
    CHECK(creport != NULL);
    CHECK(rfc_compare_run(&csp, creport) == RFC_OK);
    CHECK(creport->completed_count == 3u && creport->ranking_complete == 1);
    for (size_t ci = 0u; ci < 3u; ci++) {
        CHECK(creport->candidates[ci].outcome == RFC_CANDIDATE_COMPLETED &&
              creport->candidates[ci].verify_report.valid == 1);
    }
    free(creport);
    rfc_encoded_free(&small_enc);

    /* Completion gap #1: default-budget scheduler + rfc_compare_run at 512x512
     * and 2048x2048 RGB8 with 1 and 3 admitted threads. */
    {
        static const uint32_t dims[2] = {512u, 2048u};
        static const uint32_t thr[2] = {1u, 3u};
        rfc_scheduler_config wcfg;
        rfc_scheduler_config_default(&wcfg);
        rfc_scheduler *wsched = rfc_scheduler_create(&wcfg);
        CHECK(wsched != NULL);

        for (size_t di = 0u; di < 2u; di++) {
            uint32_t dim = dims[di];
            char hdr_buf[64];
            int hlen = snprintf(hdr_buf, sizeof(hdr_buf), "P6\n%u %u\n255\n",
                                dim, dim);
            CHECK(hlen > 0 && (size_t)hlen < sizeof(hdr_buf));
            size_t pix_bytes = (size_t)dim * (size_t)dim * 3u;
            size_t ppm_len = (size_t)hlen + pix_bytes;
            uint8_t *ppm = (uint8_t *)malloc(ppm_len);
            CHECK(ppm != NULL);
            memcpy(ppm, hdr_buf, (size_t)hlen);
            uint8_t *ppix = ppm + (size_t)hlen;
            for (uint32_t y = 0u; y < dim; y++) {
                uint8_t *row = ppix + (size_t)y * (size_t)dim * 3u;
                uint32_t pr = 0x9E3779B9u ^ (y * 2654435761u);
                for (uint32_t x = 0u; x < dim; x++) {
                    if ((x & 3u) == 0u) {
                        pr ^= pr << 13;
                        pr ^= pr >> 17;
                        pr ^= pr << 5;
                    }
                    uint32_t v = (pr >> ((x & 3u) * 8u)) & 0x3Fu;
                    row[(size_t)x * 3u + 0u] = (uint8_t)(((x >> 1) + y + v) & 0xFFu);
                    row[(size_t)x * 3u + 1u] = (uint8_t)(((x * 3u + (y >> 1)) ^ v) & 0xFFu);
                    row[(size_t)x * 3u + 2u] = (uint8_t)((y * 5u + (x >> 2) + (v << 1)) & 0xFFu);
                }
            }

            rfc_layout wlay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, dim, dim, 0u,
                               8u};
            for (size_t ti = 0u; ti < 2u; ti++) {
                uint32_t req_t = thr[ti];
                rfc_job_spec wsp;
                memset(&wsp, 0, sizeof(wsp));
                wsp.source_format = RFC_FORMAT_PNM;
                wsp.target_format = RFC_FORMAT_WEBP;
                wsp.source_bytes = ppm;
                wsp.source_size = ppm_len;
                wsp.source_layout = &wlay;
                wsp.requested_codec_threads = req_t;
                wsp.expected_working_set_bytes = 0u;
                wsp.has_stage_ops = 1;
                wsp.stage_ops.on_export = stage_export_noop;

                rfc_job_snapshot wsnap;
                rfc_verify_report wrep;
                submit_and_wait_job(wsched, &wsp, &wsnap, &wrep);
                if (wsnap.state != RFC_JOB_SUCCEEDED ||
                    wsnap.status != RFC_OK ||
                    wsnap.admitted_threads != req_t ||
                    wsnap.budget.rejected_count != 0u || wrep.valid != 1) {
                    fprintf(stderr,
                            "sched fail dim=%u req_t=%u state=%d status=%s "
                            "admitted=%u rej=%llu peak=%llu lim=%llu valid=%d "
                            "detail=%s\n",
                            dim, req_t, (int)wsnap.state,
                            rfc_status_name(wsnap.status),
                            wsnap.admitted_threads,
                            (unsigned long long)wsnap.budget.rejected_count,
                            (unsigned long long)wsnap.budget.peak_allocated_bytes,
                            (unsigned long long)wsnap.budget.limit_bytes,
                            wrep.valid, wsnap.detail);
                }
                CHECK(wsnap.state == RFC_JOB_SUCCEEDED);
                CHECK(wsnap.status == RFC_OK);
                CHECK(wsnap.admitted_threads == req_t);
                CHECK(wsnap.budget.rejected_count == 0u);
                CHECK(wrep.valid == 1);
                if (dim == 512u && req_t == 3u) {
                    CHECK(wsnap.budget.peak_allocated_bytes >= 39993344ull);
                } else if (dim == 2048u && req_t == 1u) {
                    CHECK(wsnap.budget.peak_allocated_bytes >= 145850368ull);
                } else if (dim == 2048u && req_t == 3u) {
                    CHECK(wsnap.budget.peak_allocated_bytes >= 291700736ull);
                }

                char cpath[1024];
                char fname[64];
                snprintf(fname, sizeof(fname), "cmp_%u_t%u.webp", dim, req_t);
                join_path(cpath, sizeof(cpath), tmp, fname);
                rfc_compare_candidate_spec wcand[1] = {{0}};
                wcand[0].target_format = RFC_FORMAT_WEBP;
                wcand[0].dest_path_utf8 = cpath;

                rfc_compare_spec wcsp;
                memset(&wcsp, 0, sizeof(wcsp));
                wcsp.source_bytes = ppm;
                wcsp.source_size = ppm_len;
                wcsp.source_format = RFC_FORMAT_PNM;
                wcsp.staging_dir_utf8 = tmp;
                wcsp.requested_threads = req_t;
                wcsp.metadata_policy = RFC_META_POLICY_PRESERVE;
                wcsp.warmup_iterations = 0u;
                wcsp.measured_iterations = 1u;
                wcsp.candidates = wcand;
                wcsp.candidate_count = 1u;

                rfc_comparison_report *wcreport =
                    (rfc_comparison_report *)calloc(1u, sizeof(*wcreport));
                CHECK(wcreport != NULL);
                CHECK(rfc_compare_run(&wcsp, wcreport) == RFC_OK);
                CHECK(wcreport->completed_count == 1u &&
                      wcreport->effective_threads == req_t &&
                      wcreport->candidates[0].outcome ==
                          RFC_CANDIDATE_COMPLETED &&
                      wcreport->candidates[0].effective_threads == req_t &&
                      wcreport->candidates[0].verify_report.valid == 1 &&
                      wcreport->candidates[0].size.produced_file_bytes > 0u);
                free(wcreport);
            }
            free(ppm);
        }
        rfc_scheduler_destroy(wsched);
    }

    rm_tree(tmp);
    CHECK(st.live == 0u);
    printf("webp: refusals_limits_and_budget ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 4: Malformed RIFF/VP8X matrix & mid-encode cancellation + safety   */
/* ------------------------------------------------------------------------ */

static void test_malformed_riff_and_cancellation(void) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_WEBP);
    void *ds = NULL;
    CHECK(adp->open_session(&exec, &ds) == RFC_OK);

    /* Create a baseline muxed WebP (VP8X + ICCP + VP8L) for mutation fixtures */
    uint8_t valid_icc[132];
    fill_minimal_srgb_icc(valid_icc);
    rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 8u, 8u, 0u, 8u};
    rfc_metadata vm = {valid_icc, sizeof(valid_icc), RFC_ORIENTATION_NORMAL, 0u,
                       {0}, NULL, 0u, NULL, 0u};
    rfc_frame vfr = {0};
    CHECK(test_alloc_frame(&vfr, &lay, &vm, &alloc) == RFC_OK);
    fill_webp_pattern(&vfr, 50u, 0);
    rfc_encoded base = {0};
    CHECK(adp->encode_session(ds, &exec, &vfr, 1u, &vm, NULL, 0u, &base) ==
          RFC_OK);
    rfc_frame_free(&vfr);
    CHECK(base.len >= 40u && memcmp(base.data + 12u, "VP8X", 4u) == 0);

    rfc_image_probe pr;
    rfc_frame *df = NULL;
    size_t dc = 0u;

    /* 4a. Truncation -> RFC_ERR_TRUNCATED_INPUT */
    size_t trunc_lens[7] = {0u, 4u, 8u, 11u, 12u, 20u, base.len - 1u};
    for (int i = 0; i < 7; i++) {
        size_t tl = trunc_lens[i];
        CHECK(adp->probe_session(ds, &exec, base.data, tl, &pr) ==
              RFC_ERR_TRUNCATED_INPUT);
        CHECK(adp->decode_session(ds, &exec, base.data, tl, NULL, 0u, &df, &dc,
                                  NULL) == RFC_ERR_TRUNCATED_INPUT);
    }

    /* 4b. Bad headers, trailing garbage, VP8X reserved bits, flag mismatch,
     * mismatched dimensions, tiny image chunks */
    uint8_t *mut = (uint8_t *)malloc(base.len + 16u);
    CHECK(mut != NULL);

    /* Bad RIFF / WEBP magic */
    memcpy(mut, base.data, base.len);
    mut[0] = 'X';
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);
    memcpy(mut, base.data, base.len);
    mut[8] = 'X';
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* Trailing garbage after declared RIFF size */
    memcpy(mut, base.data, base.len);
    mut[base.len] = 0u;
    mut[base.len + 1u] = 0u;
    CHECK(adp->probe_session(ds, &exec, mut, base.len + 2u, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* Reserved bits / reserved bytes in VP8X */
    memcpy(mut, base.data, base.len);
    mut[20] |= 0x40u;
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);
    memcpy(mut, base.data, base.len);
    mut[21] = 1u;
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* VP8X ANIMATION_FLAG -> RFC_ERR_UNSUPPORTED_COMBINATION */
    memcpy(mut, base.data, base.len);
    mut[20] |= 0x02u;
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);

    /* VP8X ICCP_FLAG cleared while ICCP chunk present -> RFC_ERR_INVALID_ARGUMENT */
    memcpy(mut, base.data, base.len);
    mut[20] &= (uint8_t)~0x20u;
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* VP8X EXIF_FLAG set with no EXIF chunk -> RFC_ERR_INVALID_ARGUMENT */
    memcpy(mut, base.data, base.len);
    mut[20] |= 0x08u;
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* VP8X ALPHA_FLAG cleared when VP8L has alpha -> RFC_ERR_INVALID_ARGUMENT */
    memcpy(mut, base.data, base.len);
    mut[20] &= (uint8_t)~0x10u;
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* VP8X canvas dimension mismatch vs VP8L (width 9 != 8) */
    memcpy(mut, base.data, base.len);
    mut[24] = 8u; /* width = 1 + 8 = 9 */
    CHECK(adp->probe_session(ds, &exec, mut, base.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);
    free(mut);

    /* VP8X followed by tiny 2-byte VP8L chunk (clen < 5) */
    uint8_t tiny_vp8l[40];
    memcpy(tiny_vp8l, "RIFF", 4u);
    wr32le(tiny_vp8l + 4u, 32u);
    memcpy(tiny_vp8l + 8u, "WEBPVP8L", 8u);
    wr32le(tiny_vp8l + 16u, 2u);
    tiny_vp8l[20] = 0x2Fu;
    tiny_vp8l[21] = 0x00u;
    /* Without VP8X (22 bytes) */
    wr32le(tiny_vp8l + 4u, 14u);
    CHECK(adp->probe_session(ds, &exec, tiny_vp8l, 22u, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    uint8_t vp8x_tiny[40];
    memcpy(vp8x_tiny, "RIFF", 4u);
    wr32le(vp8x_tiny + 4u, 32u);
    memcpy(vp8x_tiny + 8u, "WEBPVP8X", 8u);
    wr32le(vp8x_tiny + 16u, 10u);
    memset(vp8x_tiny + 20u, 0, 10u);
    memcpy(vp8x_tiny + 30u, "VP8L", 4u);
    wr32le(vp8x_tiny + 34u, 2u);
    vp8x_tiny[38] = 0x2Fu;
    vp8x_tiny[39] = 0x00u;
    CHECK(adp->probe_session(ds, &exec, vp8x_tiny, 40u, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* ICCP after VP8L inside RIFF */
    uint8_t iccp_after[64];
    memcpy(iccp_after, "RIFF", 4u);
    wr32le(iccp_after + 4u, 52u);
    memcpy(iccp_after + 8u, "WEBPVP8X", 8u);
    wr32le(iccp_after + 16u, 10u);
    memset(iccp_after + 20u, 0, 10u);
    iccp_after[20] = 0x20u; /* ICCP_FLAG */
    memcpy(iccp_after + 30u, "VP8L", 4u);
    wr32le(iccp_after + 34u, 6u);
    iccp_after[38] = 0x2Fu;
    memset(iccp_after + 39u, 0, 5u);
    memcpy(iccp_after + 44u, "ICCP", 4u);
    wr32le(iccp_after + 48u, 8u);
    memset(iccp_after + 52u, 0x11, 8u);
    CHECK(adp->probe_session(ds, &exec, iccp_after, 60u, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* Corrupt EXIF chunk inside VP8X */
    uint8_t bad_exif_webp[64];
    memcpy(bad_exif_webp, "RIFF", 4u);
    wr32le(bad_exif_webp + 4u, 52u);
    memcpy(bad_exif_webp + 8u, "WEBPVP8X", 8u);
    wr32le(bad_exif_webp + 16u, 10u);
    memset(bad_exif_webp + 20u, 0, 10u);
    bad_exif_webp[20] = 0x08u; /* EXIF_FLAG */
    memcpy(bad_exif_webp + 30u, "EXIF", 4u);
    wr32le(bad_exif_webp + 34u, 10u);
    memcpy(bad_exif_webp + 38u, "BADTIFF000", 10u);
    memcpy(bad_exif_webp + 48u, "VP8L", 4u);
    wr32le(bad_exif_webp + 52u, 6u);
    bad_exif_webp[56] = 0x2Fu;
    memset(bad_exif_webp + 57u, 0, 5u);
    CHECK(adp->probe_session(ds, &exec, bad_exif_webp, 62u, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);

    rfc_encoded_free(&base);
    adp->close_session(ds);

    /* 4c. Mid-encode cancellation on noisy 512x512 RGBA8 via progress callback */
    rfc_layout big_lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 512u, 512u, 0u,
                          8u};
    rfc_frame big_fr = {0};
    CHECK(test_alloc_frame(&big_fr, &big_lay, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&big_fr, 99u, 0);

    cancel_alloc_state cast;
    memset(&cast, 0, sizeof(cast));
    cast.adp = adp;
    rfc_allocator calloc_fn = {ca_alloc, ca_free, &cast};
    rfc_exec_context cexec;
    memset(&cexec, 0, sizeof(cexec));
    rfc_limits_default(&cexec.limits);
    cexec.codec_threads = 1u;
    cexec.allocator = &calloc_fn;

    void *cs = NULL;
    CHECK(adp->open_session(&cexec, &cs) == RFC_OK);
    cast.sess = cs;
    /* Skip alloc #1 (argb buffer), trigger cancel on writer buffer allocation
     * inside WebPEncode so webp_progress_cb aborts cleanly with zero leaks. */
    cast.skip_allocs = 1u;
    rfc_encoded c_out = {0};
    rfc_status cst =
        adp->encode_session(cs, &cexec, &big_fr, 1u, NULL, NULL, 0u, &c_out);
    CHECK(cst == RFC_ERR_CANCELLED && cast.triggered == 1);
    CHECK(adp->probe_session(cs, &cexec, (const uint8_t *)"RIFF", 4u, &pr) ==
          RFC_ERR_CANCELLED);
    CHECK(adp->decode_session(cs, &cexec, (const uint8_t *)"RIFF", 4u, NULL,
                              0u, &df, &dc, NULL) == RFC_ERR_CANCELLED);
    CHECK(cast.track.live == 0u);
    adp->close_session(cs);
    rfc_frame_free(&big_fr);
    rfc_registry_release_session(RFC_FORMAT_WEBP);

    /* File-safety: existing destination preserved, no .partial left on refuse */
    char tmp[1024], exist_p[1024], src_p[1024];
    make_temp_dir(tmp, sizeof(tmp), "webp_fs");
    join_path(src_p, sizeof(src_p), tmp, "in.webp");
    join_path(exist_p, sizeof(exist_p), tmp, "dest.png");

    rfc_layout sl = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 0u, 8u};
    rfc_frame sf = {0}, sd = {0};
    CHECK(test_alloc_frame(&sf, &sl, NULL, &alloc) == RFC_OK);
    fill_webp_pattern(&sf, 60u, 0);
    rfc_encoded se = {0};
    roundtrip_webp_session(&sf, NULL, NULL, 0u, &sd, NULL, &exec, &se);
    CHECK(write_file_all(src_p, se.data, se.len));
    rfc_frame_free(&sd);
    rfc_frame_free(&sf);
    rfc_encoded_free(&se);

    static const uint8_t keep[8] = {'K', 'E', 'E', 'P', 'W', 'E', 'B', 'P'};
    CHECK(write_file_all(exist_p, keep, sizeof(keep)));
    rfc_scheduler_config scfg;
    rfc_scheduler_config_default(&scfg);
    rfc_scheduler *sched = rfc_scheduler_create(&scfg);
    CHECK(sched != NULL);
    rfc_io_source src_io;
    rfc_io_sink dst_io;
    rfc_io_source_init_path(&src_io, src_p, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, exist_p, RFC_EXPORT_REFUSE_EXISTING);
    rfc_job_spec jsp;
    memset(&jsp, 0, sizeof(jsp));
    jsp.source_format = RFC_FORMAT_WEBP;
    jsp.target_format = RFC_FORMAT_PNG;
    jsp.io_source = &src_io;
    jsp.io_sink = &dst_io;
    jsp.expected_working_set_bytes = 8u * 1024u * 1024u;
    rfc_job *job = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &jsp, &job) ==
          RFC_ERR_DESTINATION_EXISTS &&
          job == NULL);
    size_t klen = 0u;
    uint8_t *kbuf = read_file_all(exist_p, &klen);
    CHECK(kbuf && klen == sizeof(keep) && memcmp(kbuf, keep, sizeof(keep)) == 0);
    free(kbuf);
    rfc_scheduler_destroy(sched);
    rm_tree(tmp);

    CHECK(st.live == 0u);
    printf("webp: malformed_riff_and_cancellation ok\n");
}

/* ------------------------------------------------------------------------ */
/* CLI subcommands driven by tests/converter/test_webp.py                   */
/* ------------------------------------------------------------------------ */

static int parse_fmt_cli(const char *s, rfc_format_id *out) {
    const rfc_capability *c = rfc_registry_find_name(s);
    if (!c) {
        return 0;
    }
    *out = c->id;
    return 1;
}

static int read_rfwe(const char *path,
                     rfc_layout *out_lay,
                     rfc_metadata *out_meta,
                     uint8_t **out_raw,
                     uint8_t **out_pix,
                     size_t *out_pay_len) {
    size_t raw_len = 0u;
    uint8_t *raw = read_file_all(path, &raw_len);
    if (!raw || raw_len < 48u || memcmp(raw, "RFWE", 4u) != 0) {
        free(raw);
        return 0;
    }
    uint32_t w = rd32le(raw + 4u);
    uint32_t h = rd32le(raw + 8u);
    uint32_t sf = rd32le(raw + 12u);
    uint32_t am = rd32le(raw + 16u);
    uint32_t bps = rd32le(raw + 20u);
    uint32_t row_b = rd32le(raw + 24u);
    uint32_t ori = rd32le(raw + 28u);
    uint32_t icc_len = rd32le(raw + 32u);
    uint32_t exif_len = rd32le(raw + 36u);
    uint32_t xmp_len = rd32le(raw + 40u);
    uint32_t pay_len = rd32le(raw + 44u);
    uint64_t need = 48ull + (uint64_t)icc_len + (uint64_t)exif_len +
                    (uint64_t)xmp_len + (uint64_t)pay_len;
    if (need > (uint64_t)raw_len) {
        free(raw);
        return 0;
    }
    memset(out_lay, 0, sizeof(*out_lay));
    out_lay->format = (rfc_sample_format)sf;
    out_lay->alpha = (rfc_alpha_mode)am;
    out_lay->width = w;
    out_lay->height = h;
    out_lay->stride_bytes = row_b;
    out_lay->bits_per_sample = (uint8_t)bps;

    size_t off = 48u;
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = (rfc_orientation)ori;
    if (icc_len > 0u) {
        out_meta->icc = raw + off;
        out_meta->icc_len = icc_len;
        off += icc_len;
    }
    if (exif_len > 0u) {
        out_meta->exif = raw + off;
        out_meta->exif_len = exif_len;
        off += exif_len;
    }
    if (xmp_len > 0u) {
        out_meta->xmp = raw + off;
        out_meta->xmp_len = xmp_len;
        off += xmp_len;
    }
    *out_raw = raw;
    *out_pix = raw + off;
    *out_pay_len = pay_len;
    return 1;
}

static int cli_main(int argc, char **argv) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_webp() == RFC_OK);
    const char *sub = argv[2];

    if (strcmp(sub, "probe") == 0 && (argc == 5 || argc == 7)) {
        rfc_format_id fmt = 0;
        if (!parse_fmt_cli(argv[3], &fmt)) {
            return 2;
        }
        size_t in_len = 0u;
        uint8_t *in_b = read_file_all(argv[4], &in_len);
        if (!in_b && in_len == 0u) {
            in_b = (uint8_t *)malloc(1u);
        }
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        if (argc == 7) {
            uint32_t mw = (uint32_t)strtoul(argv[5], NULL, 10);
            size_t mm = (size_t)strtoull(argv[6], NULL, 10);
            if (mw > 0u) exec.limits.max_width = mw;
            if (mm > 0u) exec.limits.max_metadata_bytes = mm;
        }
        const rfc_session_adapter *adp = rfc_registry_acquire_session(fmt);
        if (!adp) {
            free(in_b);
            printf("STATUS=%s\n", rfc_status_name(RFC_ERR_NO_ADAPTER));
            return 2;
        }
        void *sess = NULL;
        rfc_status r = adp->open_session(&exec, &sess);
        rfc_image_probe pr;
        memset(&pr, 0, sizeof(pr));
        if (r == RFC_OK) {
            r = adp->probe_session(sess, &exec, in_b, in_len, &pr);
        }
        if (sess) adp->close_session(sess);
        rfc_registry_release_session(fmt);
        free(in_b);
        size_t oicc = pr.container_meta.icc_len;
        size_t oex = pr.container_meta.exif_len;
        size_t oxm = pr.container_meta.xmp_len;
        unsigned oori = (unsigned)pr.container_meta.orientation;
        rfc_metadata_free(&pr.container_meta, &alloc);
        printf("STATUS=%s WIDTH=%u HEIGHT=%u SAMPLE=%u ALPHA=%u BPS=%u "
               "ICC_LEN=%zu EXIF_LEN=%zu XMP_LEN=%zu ORIENTATION=%u LIVE=%zu\n",
               rfc_status_name(r), pr.first_frame.width,
               pr.first_frame.height, (unsigned)pr.first_frame.format,
               (unsigned)pr.first_frame.alpha,
               (unsigned)pr.first_frame.bits_per_sample,
               oicc, oex, oxm, oori, st.live);
        CHECK(st.live == 0u);
        return r == RFC_OK ? 0 : 2;
    }

    if (strcmp(sub, "dump") == 0 && (argc == 6 || argc == 8)) {
        rfc_format_id fmt = 0;
        if (!parse_fmt_cli(argv[3], &fmt)) {
            return 2;
        }
        size_t in_len = 0u;
        uint8_t *in_b = read_file_all(argv[4], &in_len);
        if (!in_b && in_len == 0u) {
            in_b = (uint8_t *)malloc(1u);
        }
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        if (argc == 8) {
            uint32_t mw = (uint32_t)strtoul(argv[6], NULL, 10);
            uint64_t md = (uint64_t)strtoull(argv[7], NULL, 10);
            if (mw > 0u) exec.limits.max_width = mw;
            if (md > 0ull) exec.limits.max_decoded_bytes = md;
        }
        const rfc_session_adapter *adp = rfc_registry_acquire_session(fmt);
        if (!adp) {
            free(in_b);
            printf("STATUS=%s\n", rfc_status_name(RFC_ERR_NO_ADAPTER));
            return 2;
        }
        void *sess = NULL;
        rfc_status r = adp->open_session(&exec, &sess);
        rfc_image_probe pr;
        memset(&pr, 0, sizeof(pr));
        if (r == RFC_OK) {
            r = adp->probe_session(sess, &exec, in_b, in_len, &pr);
        }
        rfc_metadata_free(&pr.container_meta, &alloc);
        rfc_frame *frames = NULL;
        size_t fcnt = 0u;
        if (r == RFC_OK) {
            r = adp->decode_session(sess, &exec, in_b, in_len, NULL, 0u,
                                    &frames, &fcnt, NULL);
        }
        if (sess) adp->close_session(sess);
        rfc_registry_release_session(fmt);
        free(in_b);
        if (r != RFC_OK) {
            CHECK(st.live == 0u);
            printf("STATUS=%s\n", rfc_status_name(r));
            return 2;
        }

        size_t row_b = (size_t)frames[0].layout.width *
                       (frames[0].layout.format == RFC_SAMPLE_RGBA8 ? 4u : 3u);
        size_t pay_len = row_b * (size_t)frames[0].layout.height;
        const rfc_metadata *fm = &frames[0].metadata;
        uint8_t hdr[48] = {'R', 'F', 'W', 'D'};
        wr32le(hdr + 4u, frames[0].layout.width);
        wr32le(hdr + 8u, frames[0].layout.height);
        wr32le(hdr + 12u, (uint32_t)frames[0].layout.format);
        wr32le(hdr + 16u, (uint32_t)frames[0].layout.alpha);
        wr32le(hdr + 20u, frames[0].layout.bits_per_sample);
        wr32le(hdr + 24u, (uint32_t)row_b);
        wr32le(hdr + 28u, (uint32_t)fm->orientation);
        wr32le(hdr + 32u, (uint32_t)fm->icc_len);
        wr32le(hdr + 36u, (uint32_t)fm->exif_len);
        wr32le(hdr + 40u, (uint32_t)fm->xmp_len);
        wr32le(hdr + 44u, (uint32_t)pay_len);
        FILE *fp = fopen(argv[5], "wb");
        CHECK(fp != NULL);
        CHECK(fwrite(hdr, 1u, sizeof(hdr), fp) == sizeof(hdr));
        if (fm->icc_len > 0u) CHECK(fwrite(fm->icc, 1u, fm->icc_len, fp) == fm->icc_len);
        if (fm->exif_len > 0u) CHECK(fwrite(fm->exif, 1u, fm->exif_len, fp) == fm->exif_len);
        if (fm->xmp_len > 0u) CHECK(fwrite(fm->xmp, 1u, fm->xmp_len, fp) == fm->xmp_len);
        for (uint32_t y = 0u; y < frames[0].layout.height; y++) {
            CHECK(fwrite(frames[0].data +
                             (size_t)y * frames[0].layout.stride_bytes,
                         1u, row_b, fp) == row_b);
        }
        fclose(fp);
        uint32_t ow = frames[0].layout.width, oh = frames[0].layout.height;
        uint32_t osf = (uint32_t)frames[0].layout.format;
        uint32_t oori = (uint32_t)fm->orientation;
        size_t oicc = fm->icc_len, oex = fm->exif_len, oxm = fm->xmp_len;
        rfc_frame_free(&frames[0]);
        alloc.free(alloc.ctx, frames);
        CHECK(st.live == 0u);
        printf("STATUS=ok WIDTH=%u HEIGHT=%u SAMPLE=%u ORIENTATION=%u "
               "ICC_LEN=%zu EXIF_LEN=%zu XMP_LEN=%zu LIVE=%zu\n",
               ow, oh, osf, oori, oicc, oex, oxm, st.live);
        return 0;
    }

    if (strcmp(sub, "encode") == 0 && (argc == 7 || argc == 8)) {
        rfc_format_id fmt = 0;
        if (!parse_fmt_cli(argv[3], &fmt)) {
            return 2;
        }
        rfc_layout lay;
        rfc_metadata meta;
        uint8_t *raw = NULL, *pix = NULL;
        size_t pay_len = 0u;
        CHECK(read_rfwe(argv[4], &lay, &meta, &raw, &pix, &pay_len));
        uint32_t threads = (argc == 8) ? (uint32_t)strtoul(argv[7], NULL, 10) : 1u;

        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, threads ? threads : 1u);
        rfc_frame fr = {0};
        CHECK(test_alloc_frame(&fr, &lay, &meta, &alloc) == RFC_OK);
        memcpy(fr.data, pix, pay_len);

        rfc_setting_value sv[1] = {{"metadata", argv[6]}};
        const rfc_session_adapter *adp = rfc_registry_acquire_session(fmt);
        CHECK(adp != NULL);
        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);
        rfc_encoded enc = {0};
        rfc_status r =
            adp->encode_session(es, &exec, &fr, 1u, &meta, sv, 1u, &enc);
        adp->close_session(es);
        rfc_registry_release_session(fmt);
        if (r == RFC_OK) {
            CHECK(write_file_all(argv[5], enc.data, enc.len));
        }
        size_t elen = enc.len;
        rfc_encoded_free(&enc);
        rfc_frame_free(&fr);
        free(raw);
        CHECK(st.live == 0u);
        printf("STATUS=%s OUT_BYTES=%zu\n", rfc_status_name(r), elen);
        return r == RFC_OK ? 0 : 2;
    }

    if (strcmp(sub, "convert") == 0 && argc == 11) {
        rfc_format_id sfmt = 0, dfmt = 0;
        if (!parse_fmt_cli(argv[3], &sfmt) || !parse_fmt_cli(argv[4], &dfmt)) {
            return 2;
        }
        rfc_setting_value sv[1] = {{"metadata", argv[7]}};
        uint32_t ack = (uint32_t)strtoul(argv[8], NULL, 0);
        uint32_t max_w = (uint32_t)strtoul(argv[9], NULL, 10);
        uint64_t max_dec = (uint64_t)strtoull(argv[10], NULL, 10);

        rfc_scheduler_config scfg;
        rfc_scheduler_config_default(&scfg);
        rfc_scheduler *sched = rfc_scheduler_create(&scfg);
        CHECK(sched != NULL);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_path(&src_io, argv[5], RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&dst_io, argv[6], RFC_EXPORT_REFUSE_EXISTING);
        rfc_limits lim;
        rfc_limits_default(&lim);
        if (max_w > 0u) lim.max_width = max_w;
        if (max_dec > 0ull) lim.max_decoded_bytes = max_dec;
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = sfmt;
        sp.target_format = dfmt;
        sp.io_source = &src_io;
        sp.io_sink = &dst_io;
        sp.settings = sv;
        sp.setting_count = 1u;
        sp.acknowledged_unsupported_metadata = ack;
        sp.limits_override = (max_w > 0u || max_dec > 0ull) ? &lim : NULL;
        sp.expected_working_set_bytes = 32u * 1024u * 1024u;
        rfc_job *j = NULL;
        rfc_status sub_r = rfc_scheduler_submit(sched, NULL, &sp, &j);
        if (sub_r != RFC_OK) {
            printf("STATUS=%s\n", rfc_status_name(sub_r));
            rfc_scheduler_destroy(sched);
            return 2;
        }
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        rfc_verify_report rep;
        int has_rep = (rfc_job_verify_report_get(j, &rep) == RFC_OK);
        printf("STATUS=%s META_OUTCOME=%s UNSUPPORTED_ITEMS=%u\n",
               rfc_status_name(snap.status),
               has_rep ? rfc_verify_meta_outcome_name(rep.meta_outcome) : "none",
               has_rep ? rep.metadata.unsupported_items : 0u);
        rfc_job_release(j);
        rfc_scheduler_destroy(sched);
        return snap.state == RFC_JOB_SUCCEEDED ? 0 : 2;
    }

    if (strcmp(sub, "budget-decode") == 0 && argc == 5) {
        size_t wlen = 0u;
        uint8_t *wb = read_file_all(argv[3], &wlen);
        CHECK(wb != NULL);
        uint64_t blim = (uint64_t)strtoull(argv[4], NULL, 10);
        rfc_budget *b = rfc_budget_create(blim);
        CHECK(b != NULL);
        rfc_allocator ba;
        rfc_budget_allocator(b, &ba);
        rfc_exec_context exec;
        memset(&exec, 0, sizeof(exec));
        rfc_limits_default(&exec.limits);
        exec.codec_threads = 1u;
        exec.allocator = &ba;
        exec.budget = b;

        const rfc_session_adapter *adp =
            rfc_registry_acquire_session(RFC_FORMAT_WEBP);
        CHECK(adp != NULL);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_status r = adp->decode_session(ds, &exec, wb, wlen, NULL, 0u, &df,
                                           &dc, NULL);
        if (df) {
            rfc_frame_free(&df[0]);
            ba.free(ba.ctx, df);
        }
        adp->close_session(ds);
        rfc_registry_release_session(RFC_FORMAT_WEBP);
        rfc_budget_stats bs;
        rfc_budget_stats_get(b, &bs);
        printf("STATUS=%s REJECTED=%llu CURR_ALLOC=%llu\n", rfc_status_name(r),
               (unsigned long long)bs.rejected_count,
               (unsigned long long)bs.current_allocated_bytes);
        rfc_budget_destroy(b);
        free(wb);
        return r == RFC_OK ? 0 : 2;
    }

    if (strcmp(sub, "budget-peak") == 0 && argc == 7) {
        uint32_t threads = (uint32_t)strtoul(argv[6], NULL, 10);
        if (threads == 0u) threads = 1u;
        rfc_budget *b = rfc_budget_create(256ull * 1024ull * 1024ull);
        CHECK(b != NULL);
        rfc_allocator ba;
        rfc_budget_allocator(b, &ba);
        rfc_exec_context b_exec;
        memset(&b_exec, 0, sizeof(b_exec));
        rfc_limits_default(&b_exec.limits);
        b_exec.codec_threads = threads;
        b_exec.allocator = &ba;
        b_exec.budget = b;
        const rfc_session_adapter *adp =
            rfc_registry_acquire_session(RFC_FORMAT_WEBP);
        CHECK(adp != NULL);
        void *sess = NULL;
        CHECK(adp->open_session(&b_exec, &sess) == RFC_OK);

        if (strcmp(argv[3], "encode") == 0) {
            rfc_layout lay;
            rfc_metadata meta;
            uint8_t *raw = NULL, *pix = NULL;
            size_t pay_len = 0u;
            CHECK(read_rfwe(argv[4], &lay, &meta, &raw, &pix, &pay_len));
            rfc_exec_context fr_ex;
            tracked_alloc_state fr_st;
            rfc_allocator fr_alloc;
            init_exec(&fr_ex, &fr_st, &fr_alloc, NULL, 1u);
            rfc_frame fr = {0};
            CHECK(test_alloc_frame(&fr, &lay, &meta, &fr_alloc) == RFC_OK);
            memcpy(fr.data, pix, pay_len);
            free(raw);

            rfc_encoded enc = {0};
            rfc_status r = adp->encode_session(sess, &b_exec, &fr, 1u, &meta,
                                               NULL, 0u, &enc);
            if (r == RFC_OK && strcmp(argv[5], "-") != 0) {
                CHECK(write_file_all(argv[5], enc.data, enc.len));
            }
            size_t elen = enc.len;
            rfc_encoded_free(&enc);
            rfc_frame_free(&fr);
            CHECK(fr_st.live == 0u);
            adp->close_session(sess);
            rfc_registry_release_session(RFC_FORMAT_WEBP);
            rfc_budget_stats bs;
            rfc_budget_stats_get(b, &bs);
            printf("STATUS=%s PEAK_BUDGET=%llu CURR_ALLOC=%llu OUT_BYTES=%zu\n",
                   rfc_status_name(r),
                   (unsigned long long)bs.peak_allocated_bytes,
                   (unsigned long long)bs.current_allocated_bytes, elen);
            rfc_budget_destroy(b);
            return r == RFC_OK ? 0 : 2;
        }
        if (strcmp(argv[3], "decode") == 0) {
            size_t wlen = 0u;
            uint8_t *wb = read_file_all(argv[4], &wlen);
            CHECK(wb != NULL);
            rfc_frame *df = NULL;
            size_t dc = 0u;
            rfc_status r = adp->decode_session(sess, &b_exec, wb, wlen, NULL,
                                               0u, &df, &dc, NULL);
            if (df) {
                rfc_frame_free(&df[0]);
                ba.free(ba.ctx, df);
            }
            free(wb);
            adp->close_session(sess);
            rfc_registry_release_session(RFC_FORMAT_WEBP);
            rfc_budget_stats bs;
            rfc_budget_stats_get(b, &bs);
            printf("STATUS=%s PEAK_BUDGET=%llu CURR_ALLOC=%llu OUT_BYTES=%zu\n",
                   rfc_status_name(r),
                   (unsigned long long)bs.peak_allocated_bytes,
                   (unsigned long long)bs.current_allocated_bytes, dc);
            rfc_budget_destroy(b);
            return r == RFC_OK ? 0 : 2;
        }
        adp->close_session(sess);
        rfc_registry_release_session(RFC_FORMAT_WEBP);
        rfc_budget_destroy(b);
        return 2;
    }

    return 2;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "cli") == 0) {
        return cli_main(argc, argv);
    }
    test_registration_and_upstream_contract();
    test_mux_icc_exif_xmp_and_cross_format();
    test_refusals_limits_and_budget();
    test_malformed_riff_and_cancellation();
    printf("webp: all checks passed (%d checks)\n", g_checks);
    return 0;
}
