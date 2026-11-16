/* C11 codec & format adapter test suite + CLI harness for D10 T02 §1:
 *   RFC_FORMAT_RASTERFOLD, RFC_FORMAT_BMP, RFC_FORMAT_PNG, RFC_FORMAT_PNM,
 *   RFC_FORMAT_PAM. */
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
#include "rfc_verify.h"
#include "rf_shared_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* Portable helpers: endian, tracked allocator, files, temp dirs, PNG/BMP   */
/* ------------------------------------------------------------------------ */

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

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
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
    int ok = (wrote == len) && (fclose(fp) == 0);
    return ok;
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

static void fill_test_pattern(rfc_frame *f, uint32_t seed) {
    uint32_t w = f->layout.width;
    uint32_t h = f->layout.height;
    uint32_t bps = f->layout.bits_per_sample;
    uint32_t ch = 0u;
    uint32_t sbits = 8u;
    switch (f->layout.format) {
        case RFC_SAMPLE_GRAY8:
            ch = 1u;
            sbits = 8u;
            break;
        case RFC_SAMPLE_GRAY16:
            ch = 1u;
            sbits = 16u;
            break;
        case RFC_SAMPLE_RGB8:
            ch = 3u;
            sbits = 8u;
            break;
        case RFC_SAMPLE_RGB16:
            ch = 3u;
            sbits = 16u;
            break;
        case RFC_SAMPLE_RGBA8:
            ch = 4u;
            sbits = 8u;
            break;
        case RFC_SAMPLE_RGBA16:
            ch = 4u;
            sbits = 16u;
            break;
        default:
            CHECK(0);
            break;
    }
    uint32_t maxv = (1u << bps) - 1u;
    uint32_t shift = sbits - bps;
    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *row = f->data + (size_t)y * f->layout.stride_bytes;
        for (uint32_t x = 0u; x < w; x++) {
            for (uint32_t c = 0u; c < ch; c++) {
                uint32_t v = ((x * 29u + y * 53u + c * 97u + seed * 17u) %
                              (maxv + 1u));
                if (ch == 4u && x == 1u && y == 1u) {
                    /* Explicit A==0 transparent pixel with non-zero RGB */
                    if (c == 3u) {
                        v = 0u;
                    } else {
                        v = (0x31u + c * 0x25u) & maxv;
                    }
                }
                if (sbits == 8u) {
                    row[x * ch + c] = (uint8_t)(v << shift);
                } else {
                    uint16_t u16 = (uint16_t)(v << shift);
                    memcpy(row + (x * ch + c) * 2u, &u16, 2u);
                }
            }
        }
    }
}

static void roundtrip_session(rfc_format_id fmt,
                              const rfc_frame *src,
                              const rfc_metadata *src_meta,
                              const rfc_setting_value *enc_settings,
                              size_t enc_scount,
                              rfc_frame *out_dec,
                              rfc_metadata *out_dec_meta,
                              const rfc_exec_context *exec) {
    const rfc_session_adapter *adp = rfc_registry_acquire_session(fmt);
    CHECK(adp != NULL);
    void *esess = NULL;
    CHECK(adp->open_session(exec, &esess) == RFC_OK);
    rfc_encoded enc = {0};
    CHECK(adp->encode_session(esess, exec, src, 1u, src_meta, enc_settings,
                              enc_scount, &enc) == RFC_OK);
    CHECK(enc.data != NULL && enc.len > 0u);
    adp->close_session(esess);

    void *dsess = NULL;
    CHECK(adp->open_session(exec, &dsess) == RFC_OK);
    rfc_image_probe probe;
    CHECK(adp->probe_session(dsess, exec, enc.data, enc.len, &probe) ==
          RFC_OK);
    CHECK(probe.frame_count == 1u);
    if (probe.container_meta.icc) {
        exec->allocator->free(exec->allocator->ctx,
                              (void *)probe.container_meta.icc);
    }

    rfc_frame *dec_frames = NULL;
    size_t dec_cnt = 0u;
    rfc_metadata dm;
    CHECK(adp->decode_session(dsess, exec, enc.data, enc.len, NULL, 0u,
                              &dec_frames, &dec_cnt, &dm) == RFC_OK);
    CHECK(dec_cnt == 1u && dec_frames != NULL);
    *out_dec = dec_frames[0];
    exec->allocator->free(exec->allocator->ctx, dec_frames);
    if (out_dec_meta) {
        *out_dec_meta = dm;
    }

    adp->close_session(dsess);
    if (adp->free_encoded) {
        adp->free_encoded(NULL, &enc);
    } else {
        rfc_encoded_free(&enc);
    }
    rfc_registry_release_session(fmt);
}

/* Build valid minimal .fold v2 or v3 RAW buffer using rf_crc32 only (0 heap allocs). */
static uint8_t *build_raw_fold(uint32_t version,
                               uint32_t w,
                               uint32_t h,
                               const uint8_t *rgb,
                               size_t *out_len) {
    size_t plen = (size_t)w * (size_t)h * 3u;
    size_t total = 32u + plen;
    uint8_t *b = (uint8_t *)malloc(total);
    CHECK(b != NULL);
    memset(b, 0, 32u);
    b[0] = 'R';
    b[1] = 'F';
    b[2] = 'I';
    b[3] = (uint8_t)version;
    wr16le(b + 4, (uint16_t)version);
    wr16le(b + 6, 32u);
    wr32le(b + 8, w);
    wr32le(b + 12, h);
    b[16] = RF_METHOD_RAW;
    b[17] = 0u;
    wr16le(b + 18, RF_FORMAT_RGB8);
    wr32le(b + 20, (uint32_t)plen);
    memcpy(b + 32, rgb, plen);
    wr32le(b + 24, rf_crc32(b + 32, plen));
    wr32le(b + 28, rf_crc32(b, 28u));
    *out_len = total;
    return b;
}

/* Insert an arbitrary PNG chunk immediately before IDAT with valid CRC-32. */
static uint8_t *insert_png_chunk_before_idat(const uint8_t *png,
                                             size_t plen,
                                             const char tag[4],
                                             const uint8_t *cdata,
                                             uint32_t clen,
                                             size_t *out_len) {
    size_t pos = 8u;
    size_t idat_pos = 0u;
    while (pos + 12u <= plen) {
        uint32_t l = ((uint32_t)png[pos] << 24) | ((uint32_t)png[pos + 1] << 16) |
                     ((uint32_t)png[pos + 2] << 8) | (uint32_t)png[pos + 3];
        if (memcmp(png + pos + 4u, "IDAT", 4) == 0) {
            idat_pos = pos;
            break;
        }
        pos += 12u + (size_t)l;
    }
    CHECK(idat_pos >= 8u + 25u);
    size_t add = 12u + (size_t)clen;
    uint8_t *out = (uint8_t *)malloc(plen + add);
    CHECK(out != NULL);
    memcpy(out, png, idat_pos);
    wr32be(out + idat_pos, clen);
    memcpy(out + idat_pos + 4u, tag, 4u);
    if (clen > 0u && cdata) {
        memcpy(out + idat_pos + 8u, cdata, clen);
    }
    uint32_t crc = rf_crc32(out + idat_pos + 4u, 4u + (size_t)clen);
    wr32be(out + idat_pos + 8u + clen, crc);
    memcpy(out + idat_pos + add, png + idat_pos, plen - idat_pos);
    *out_len = plen + add;
    return out;
}

static size_t build_exif_orientation_payload(uint16_t orientation,
                                             uint8_t out[28]) {
    /* Little-endian TIFF header + 1 IFD0 entry (tag 0x0112 SHORT=3 count=1) */
    memset(out, 0, 28u);
    out[0] = 'I';
    out[1] = 'I';
    wr16le(out + 2, 0x002Au);
    wr32le(out + 4, 8u);
    wr16le(out + 8, 1u); /* count */
    wr16le(out + 10, 0x0112u);
    wr16le(out + 12, 3u); /* SHORT */
    wr32le(out + 14, 1u); /* count 1 */
    wr16le(out + 18, orientation);
    wr32le(out + 22, 0u);
    return 26u;
}

/* Dummy adapter used to prove registration rollback on duplicate slot. */
static rfc_status dummy_open(const rfc_exec_context *e, void **s) {
    (void)e;
    *s = (void *)0x1;
    return RFC_OK;
}
static void dummy_close(void *s) { (void)s; }
static rfc_status dummy_probe(void *s,
                              const rfc_exec_context *e,
                              const uint8_t *b,
                              size_t n,
                              rfc_image_probe *p) {
    (void)s;
    (void)e;
    (void)b;
    (void)n;
    (void)p;
    return RFC_ERR_NO_ADAPTER;
}
static rfc_status dummy_enc(void *s,
                            const rfc_exec_context *e,
                            const rfc_frame *f,
                            size_t c,
                            const rfc_metadata *m,
                            const rfc_setting_value *sv,
                            size_t sc,
                            rfc_encoded *out) {
    (void)s;
    (void)e;
    (void)f;
    (void)c;
    (void)m;
    (void)sv;
    (void)sc;
    (void)out;
    return RFC_ERR_NO_ADAPTER;
}
static rfc_status dummy_dec(void *s,
                            const rfc_exec_context *e,
                            const uint8_t *b,
                            size_t n,
                            const rfc_setting_value *sv,
                            size_t sc,
                            rfc_frame **f,
                            size_t *c,
                            rfc_metadata *m) {
    (void)s;
    (void)e;
    (void)b;
    (void)n;
    (void)sv;
    (void)sc;
    (void)f;
    (void)c;
    (void)m;
    return RFC_ERR_NO_ADAPTER;
}

static const rfc_session_adapter k_dummy_bmp = {
    RFC_CODEC_ABI_VERSION, RFC_FORMAT_BMP, dummy_open, dummy_close,
    dummy_probe, dummy_enc, dummy_dec, NULL, NULL
};

static rfc_status stage_export_noop(rfc_job *j,
                                    const rfc_exec_context *e,
                                    void *u) {
    (void)j;
    (void)e;
    (void)u;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Suite 1: Registration, Rollback, Fold v2/v3, limits_override zero-alloc  */
/* ------------------------------------------------------------------------ */

static void test_registration_and_fold_v2_v3(void) {
    /* Before registration: usable formats report RFC_ERR_NO_ADAPTER. */
    CHECK(rfc_registry_select(RFC_FORMAT_PNG, RFC_MODE_READ) ==
          RFC_ERR_NO_ADAPTER);
    CHECK(rfc_registry_select(RFC_FORMAT_WEBP, RFC_MODE_READ) ==
          RFC_ERR_PLANNED);

    /* Rollback check on duplicate conflict. */
    CHECK(rfc_registry_register_session(&k_dummy_bmp) == RFC_OK);
    CHECK(rfc_codecs_register_basic() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_RASTERFOLD) == NULL);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_BMP) == &k_dummy_bmp);
    CHECK(rfc_registry_unregister_session(RFC_FORMAT_BMP) == RFC_OK);

    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_basic() == RFC_ERR_DUPLICATE);
    CHECK(rfc_codecs_unregister_basic() == RFC_OK);
    CHECK(rfc_codecs_register_basic() == RFC_OK);

    /* IMPORTANT: Test valid .fold v3 fixture (128x64, well inside RF_MAX_DIM/
     * RF_MAX_PIXELS) against tighter job limits_override BEFORE any core
     * rf_encode_image/rf_decode_image runs, proving rf_internal_alloc_peak
     * stays 0! */
    uint8_t *raw128 = (uint8_t *)malloc(128u * 64u * 3u);
    CHECK(raw128 != NULL);
    for (size_t i = 0u; i < 128u * 64u * 3u; i++) {
        raw128[i] = (uint8_t)(i * 13u + 7u);
    }
    size_t fold128_len = 0u;
    uint8_t *fold128 = build_raw_fold(3u, 128u, 64u, raw128, &fold128_len);
    free(raw128);

    uint64_t peak0 = rf_internal_alloc_peak;
    CHECK(peak0 == 0ull);

    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);
    exec.limits.max_width = 64u;

    const rfc_session_adapter *rf_adp =
        rfc_registry_acquire_session(RFC_FORMAT_RASTERFOLD);
    CHECK(rf_adp != NULL);
    void *sess = NULL;
    CHECK(rf_adp->open_session(&exec, &sess) == RFC_OK);
    rfc_image_probe pr;
    CHECK(rf_adp->probe_session(sess, &exec, fold128, fold128_len, &pr) ==
          RFC_ERR_LIMIT_EXCEEDED);
    rfc_frame *df = NULL;
    size_t dc = 0u;
    CHECK(rf_adp->decode_session(sess, &exec, fold128, fold128_len, NULL, 0u,
                                 &df, &dc, NULL) == RFC_ERR_LIMIT_EXCEEDED);
    CHECK(df == NULL && dc == 0u);
    rf_adp->close_session(sess);
    rfc_registry_release_session(RFC_FORMAT_RASTERFOLD);
    CHECK(st.count == 0u && st.live == 0u);
    CHECK(rf_internal_alloc_peak == 0ull);

    /* Same zero-alloc rejection through full rfc_scheduler + max_decoded_bytes. */
    rfc_scheduler_config scfg;
    rfc_scheduler_config_default(&scfg);
    rfc_scheduler *sched = rfc_scheduler_create(&scfg);
    CHECK(sched != NULL);
    rfc_limits lim_tight;
    rfc_limits_default(&lim_tight);
    lim_tight.max_decoded_bytes = 4096ull;
    rfc_job_spec jspec;
    memset(&jspec, 0, sizeof(jspec));
    jspec.source_format = RFC_FORMAT_RASTERFOLD;
    jspec.target_format = RFC_FORMAT_BMP;
    jspec.source_bytes = fold128;
    jspec.source_size = fold128_len;
    jspec.limits_override = &lim_tight;
    jspec.expected_working_set_bytes = 256u * 1024u;
    jspec.has_stage_ops = 1;
    jspec.stage_ops.on_export = stage_export_noop;
    rfc_job *job = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &jspec, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    rfc_job_snapshot snap;
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_FAILED);
    CHECK(snap.status == RFC_ERR_LIMIT_EXCEEDED);
    CHECK(rf_internal_alloc_peak == 0ull);
    rfc_job_release(job);
    rfc_scheduler_destroy(sched);
    free(fold128);

    /* Now run full .fold v3 round-trip (threads=1 vs 4 byte identity) and v2 decode. */
    rfc_exec_context ex1, ex4;
    tracked_alloc_state st1, st4;
    rfc_allocator a1, a4;
    init_exec(&ex1, &st1, &a1, NULL, 1u);
    init_exec(&ex4, &st4, &a4, NULL, 4u);

    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 32u, 24u, 96u, 8u};
    rfc_frame src = {0};
    CHECK(test_alloc_frame(&src, &lay, NULL, &a1) == RFC_OK);
    fill_test_pattern(&src, 1u);

    CHECK(rf_adp = rfc_registry_acquire_session(RFC_FORMAT_RASTERFOLD));
    void *s1 = NULL, *s4 = NULL;
    CHECK(rf_adp->open_session(&ex1, &s1) == RFC_OK);
    CHECK(rf_adp->open_session(&ex4, &s4) == RFC_OK);
    rfc_encoded e1 = {0}, e4 = {0};
    CHECK(rf_adp->encode_session(s1, &ex1, &src, 1u, NULL, NULL, 0u, &e1) ==
          RFC_OK);
    CHECK(rf_adp->encode_session(s4, &ex4, &src, 1u, NULL, NULL, 0u, &e4) ==
          RFC_OK);
    CHECK(e1.len == e4.len && e1.len >= 32u);
    CHECK(memcmp(e1.data, e4.data, e1.len) == 0);
    CHECK(e1.data[0] == 'R' && e1.data[1] == 'F' && e1.data[2] == 'I' &&
          e1.data[3] == 3u);

    rfc_frame dec_fold = {0};
    roundtrip_session(RFC_FORMAT_RASTERFOLD, &src, NULL, NULL, 0u, &dec_fold,
                      NULL, &ex1);
    rfc_sample_mismatch mm;
    CHECK(rfc_verify_frames_equal(&src, 1u, &dec_fold, 1u, &mm) == RFC_OK);
    rfc_frame_free(&dec_fold);

    /* .fold v2 raw fixture decode */
    size_t v2_len = 0u;
    uint8_t *v2 = build_raw_fold(2u, src.layout.width, src.layout.height,
                                 src.data, &v2_len);
    rfc_image_probe v2_probe;
    CHECK(rf_adp->probe_session(s1, &ex1, v2, v2_len, &v2_probe) == RFC_OK);
    CHECK(v2_probe.first_frame.width == src.layout.width &&
          v2_probe.first_frame.height == src.layout.height);
    rfc_frame *v2_df = NULL;
    size_t v2_fc = 0u;
    CHECK(rf_adp->decode_session(s1, &ex1, v2, v2_len, NULL, 0u, &v2_df,
                                 &v2_fc, NULL) == RFC_OK);
    CHECK(v2_fc == 1u);
    CHECK(rfc_verify_frames_equal(&src, 1u, v2_df, 1u, &mm) == RFC_OK);
    rfc_frame_free(&v2_df[0]);
    a1.free(a1.ctx, v2_df);
    free(v2);

    /* Refusals: bad magic, truncated <32, corrupt header CRC, version 4, RGBA8 encode */
    uint8_t bad_magic[32] = {'N', 'O', 'P', 3};
    CHECK(rf_adp->probe_session(s1, &ex1, bad_magic, sizeof(bad_magic), &pr) ==
          RFC_ERR_INVALID_ARGUMENT);
    CHECK(rf_adp->probe_session(s1, &ex1, e1.data, 16u, &pr) ==
          RFC_ERR_TRUNCATED_INPUT);
    uint8_t *bad_crc = (uint8_t *)malloc(e1.len);
    CHECK(bad_crc != NULL);
    memcpy(bad_crc, e1.data, e1.len);
    bad_crc[31] ^= 0x01u;
    CHECK(rf_adp->probe_session(s1, &ex1, bad_crc, e1.len, &pr) ==
          RFC_ERR_INVALID_ARGUMENT);
    free(bad_crc);

    uint8_t v4[32] = {'R', 'F', 'I', 4, 4, 0, 32, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    wr16le(v4 + 18, RF_FORMAT_RGB8);
    wr32le(v4 + 28, rf_crc32(v4, 28u));
    CHECK(rf_adp->probe_session(s1, &ex1, v4, sizeof(v4), &pr) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);

    rfc_layout rgba_lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 4u, 4u, 16u,
                           8u};
    rfc_frame rgba_fr = {0};
    CHECK(test_alloc_frame(&rgba_fr, &rgba_lay, NULL, &a1) == RFC_OK);
    fill_test_pattern(&rgba_fr, 2u);
    rfc_encoded bad_enc = {0};
    CHECK(rf_adp->encode_session(s1, &ex1, &rgba_fr, 1u, NULL, NULL, 0u,
                                 &bad_enc) == RFC_ERR_UNSUPPORTED_SAMPLE);
    rfc_frame_free(&rgba_fr);

    rf_adp->close_session(s1);
    rf_adp->close_session(s4);
    rfc_encoded_free(&e1);
    rfc_encoded_free(&e4);
    rfc_frame_free(&src);
    rfc_registry_release_session(RFC_FORMAT_RASTERFOLD);
    CHECK(st1.live == 0u && st4.live == 0u);
    printf("basic: registration_and_fold_v2_v3 ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 2: BMP depths (GRAY8, RGB8, RGBA8 A=0 RGB), top-down, V5 ICC      */
/* ------------------------------------------------------------------------ */

static void test_bmp_depths_alpha_and_icc(void) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    rfc_sample_format fmts[3] = {RFC_SAMPLE_GRAY8, RFC_SAMPLE_RGB8,
                                 RFC_SAMPLE_RGBA8};
    for (int i = 0; i < 3; i++) {
        rfc_alpha_mode am =
            (fmts[i] == RFC_SAMPLE_RGBA8) ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
        rfc_layout lay = {fmts[i], am, 13u, 9u, 0u, 8u};
        rfc_frame src = {0}, dec = {0};
        CHECK(test_alloc_frame(&src, &lay, NULL, &alloc) == RFC_OK);
        fill_test_pattern(&src, (uint32_t)(10 + i));
        roundtrip_session(RFC_FORMAT_BMP, &src, NULL, NULL, 0u, &dec, NULL,
                          &exec);
        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&src, 1u, &dec, 1u, &mm) == RFC_OK);
        rfc_frame_free(&src);
        rfc_frame_free(&dec);
    }

    /* V5 embedded ICC in BMP triggers RFC_ERR_METADATA_POLICY on BMP->BMP preserve,
     * succeeds on BMP->PNG preserve and BMP->BMP with ack or strip. */
    uint8_t valid_icc[132];
    memset(valid_icc, 0, sizeof(valid_icc));
    wr32be(valid_icc + 0u, (uint32_t)sizeof(valid_icc));
    wr32be(valid_icc + 8u, 0x02100000u);
    memcpy(valid_icc + 12u, "mntrRGB XYZ ", 12u);
    valid_icc[24u] = 0x07u; valid_icc[25u] = 0xEAu; /* year 2026 */
    valid_icc[27u] = 11u;   valid_icc[29u] = 16u;   /* month 11, day 16 */
    memcpy(valid_icc + 36u, "acsp", 4u);
    wr32be(valid_icc + 68u, 0x0000F6D6u); /* D50 X */
    wr32be(valid_icc + 72u, 0x00010000u); /* D50 Y */
    wr32be(valid_icc + 76u, 0x0000D32Du); /* D50 Z */

    uint32_t w = 4u, h = 4u;
    size_t row = 12u;
    size_t off_bits = 14u + 124u + sizeof(valid_icc);
    size_t fsz = off_bits + row * h;
    uint8_t *bmp_icc = (uint8_t *)calloc(1u, fsz);
    CHECK(bmp_icc != NULL);
    bmp_icc[0] = 'B';
    bmp_icc[1] = 'M';
    wr32le(bmp_icc + 2, (uint32_t)fsz);
    wr32le(bmp_icc + 10, (uint32_t)off_bits);
    wr32le(bmp_icc + 14, 124u);
    wr32le(bmp_icc + 18, w);
    wr32le(bmp_icc + 22, h);
    wr16le(bmp_icc + 26, 1u);
    wr16le(bmp_icc + 28, 24u);
    wr32le(bmp_icc + 30, 0u);
    wr32le(bmp_icc + 14u + 56u, 0x4D424544u); /* PROFILE_EMBEDDED */
    wr32le(bmp_icc + 14u + 112u, 124u);
    wr32le(bmp_icc + 14u + 116u, (uint32_t)sizeof(valid_icc));
    memcpy(bmp_icc + 14u + 124u, valid_icc, sizeof(valid_icc));

    rfc_scheduler_config scfg;
    rfc_scheduler_config_default(&scfg);
    rfc_scheduler *sched = rfc_scheduler_create(&scfg);
    CHECK(sched != NULL);

    rfc_setting_value s_pres[1] = {{"metadata", "preserve"}};
    rfc_setting_value s_strip[1] = {{"metadata", "strip"}};
    rfc_job_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_format = RFC_FORMAT_BMP;
    sp.target_format = RFC_FORMAT_BMP;
    sp.source_bytes = bmp_icc;
    sp.source_size = fsz;
    sp.settings = s_pres;
    sp.setting_count = 1u;
    sp.expected_working_set_bytes = 256u * 1024u;
    sp.has_stage_ops = 1;
    sp.stage_ops.on_export = stage_export_noop;

    rfc_job *job = NULL;
    rfc_job_snapshot snap;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_FAILED &&
          snap.status == RFC_ERR_METADATA_POLICY);
    rfc_verify_report rep;
    CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
    CHECK((rep.metadata.unsupported_items & RFC_META_ITEM_ICC) != 0u);
    rfc_job_release(job);

    sp.acknowledged_unsupported_metadata = RFC_META_ITEM_ICC;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED);
    rfc_job_release(job);

    sp.acknowledged_unsupported_metadata = 0u;
    sp.settings = s_strip;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED);
    rfc_job_release(job);

    /* BMP -> PNG preserve carries ICC */
    sp.target_format = RFC_FORMAT_PNG;
    sp.settings = s_pres;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED);
    CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
    CHECK(rep.meta_outcome == RFC_VERIFY_META_PRESERVED);
    rfc_job_release(job);

    /* Refusals: colour palette 8-bpp, 16-bpp, RLE8, premultiplied alpha */
    const rfc_session_adapter *bmp_adp =
        rfc_registry_acquire_session(RFC_FORMAT_BMP);
    CHECK(bmp_adp != NULL);
    void *bs = NULL;
    CHECK(bmp_adp->open_session(&exec, &bs) == RFC_OK);
    rfc_image_probe pr;

    uint8_t color8[54 + 1024 + 4] = {'B', 'M'};
    wr32le(color8 + 2, sizeof(color8));
    wr32le(color8 + 10, 54u + 1024u);
    wr32le(color8 + 14, 40u);
    wr32le(color8 + 18, 1u);
    wr32le(color8 + 22, 1u);
    wr16le(color8 + 26, 1u);
    wr16le(color8 + 28, 8u);
    color8[54] = 200u;
    color8[55] = 10u;
    color8[56] = 10u; /* B!=G!=R */
    CHECK(bmp_adp->probe_session(bs, &exec, color8, sizeof(color8), &pr) ==
          RFC_ERR_UNSUPPORTED_SAMPLE);

    uint8_t bmp16[58] = {'B', 'M'};
    wr32le(bmp16 + 2, sizeof(bmp16));
    wr32le(bmp16 + 10, 54u);
    wr32le(bmp16 + 14, 40u);
    wr32le(bmp16 + 18, 1u);
    wr32le(bmp16 + 22, 1u);
    wr16le(bmp16 + 26, 1u);
    wr16le(bmp16 + 28, 16u);
    CHECK(bmp_adp->probe_session(bs, &exec, bmp16, sizeof(bmp16), &pr) ==
          RFC_ERR_UNSUPPORTED_SAMPLE);

    uint8_t rle8[54 + 1024 + 4] = {'B', 'M'};
    wr32le(rle8 + 2, sizeof(rle8));
    wr32le(rle8 + 10, 54u + 1024u);
    wr32le(rle8 + 14, 40u);
    wr32le(rle8 + 18, 1u);
    wr32le(rle8 + 22, 1u);
    wr16le(rle8 + 26, 1u);
    wr16le(rle8 + 28, 8u);
    wr32le(rle8 + 30, 1u); /* BI_RLE8 */
    CHECK(bmp_adp->probe_session(bs, &exec, rle8, sizeof(rle8), &pr) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);

    rfc_layout pm_lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_PREMULTIPLIED, 4u, 4u,
                         16u, 8u};
    rfc_frame pm_fr = {0};
    CHECK(test_alloc_frame(&pm_fr, &pm_lay, NULL, &alloc) == RFC_OK);
    memset(pm_fr.data, 0, pm_fr.data_len);
    rfc_encoded pm_enc = {0};
    CHECK(bmp_adp->encode_session(bs, &exec, &pm_fr, 1u, NULL, NULL, 0u,
                                  &pm_enc) == RFC_ERR_UNSUPPORTED_ALPHA);
    rfc_frame_free(&pm_fr);

    bmp_adp->close_session(bs);
    rfc_registry_release_session(RFC_FORMAT_BMP);
    rfc_scheduler_destroy(sched);
    free(bmp_icc);
    CHECK(st.live == 0u);
    printf("basic: bmp_depths_alpha_and_icc ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 3: PNG depths, dual routes & OOM budget, 4 eXIf outcomes, refusals */
/* ------------------------------------------------------------------------ */

static void test_png_depths_routes_budget_and_exif(void) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    const rfc_sample_format fmts[6] = {
        RFC_SAMPLE_GRAY8, RFC_SAMPLE_GRAY16, RFC_SAMPLE_RGB8,
        RFC_SAMPLE_RGBA8, RFC_SAMPLE_RGB16,  RFC_SAMPLE_RGBA16
    };
    for (int i = 0; i < 6; i++) {
        rfc_sample_format sf = fmts[i];
        uint32_t bps = (sf == RFC_SAMPLE_GRAY16 || sf == RFC_SAMPLE_RGB16 ||
                        sf == RFC_SAMPLE_RGBA16)
                           ? 16u
                           : 8u;
        rfc_alpha_mode am =
            (sf == RFC_SAMPLE_RGBA8 || sf == RFC_SAMPLE_RGBA16)
                ? RFC_ALPHA_STRAIGHT
                : RFC_ALPHA_NONE;
        rfc_layout lay = {sf, am, 17u, 11u, 0u, bps};
        rfc_frame src = {0}, dec = {0};
        CHECK(test_alloc_frame(&src, &lay, NULL, &alloc) == RFC_OK);
        fill_test_pattern(&src, (uint32_t)(30 + i));
        roundtrip_session(RFC_FORMAT_PNG, &src, NULL, NULL, 0u, &dec, NULL,
                          &exec);
        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&src, 1u, &dec, 1u, &mm) == RFC_OK);
        rfc_frame_free(&src);
        rfc_frame_free(&dec);
    }

    /* Low-budget RFC_ERR_OUT_OF_MEMORY tests on BOTH PNG routes. */
    const rfc_session_adapter *png_adp =
        rfc_registry_acquire_session(RFC_FORMAT_PNG);
    CHECK(png_adp != NULL);

    /* Route A (shared rf_png_load baseline RGB8): 64x64 frame = 12288 bytes,
     * working set = 4096 + 64*(1+192) + 12288 = 28736 bytes -> budget 20480
     * admits rfc_frame (12288 + ~128) then rejects shared ws_guard before
     * rf_png_load runs! */
    rfc_layout l64 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 64u, 64u, 0u, 8u};
    rfc_frame f64 = {0};
    CHECK(test_alloc_frame(&f64, &l64, NULL, &alloc) == RFC_OK);
    fill_test_pattern(&f64, 71u);
    void *es = NULL;
    CHECK(png_adp->open_session(&exec, &es) == RFC_OK);
    rfc_encoded png_rgb8 = {0};
    CHECK(png_adp->encode_session(es, &exec, &f64, 1u, NULL, NULL, 0u,
                                  &png_rgb8) == RFC_OK);
    png_adp->close_session(es);
    rfc_frame_free(&f64);

    rfc_budget *b_shared = rfc_budget_create(20480ull);
    CHECK(b_shared != NULL);
    rfc_allocator ba_shared;
    rfc_budget_allocator(b_shared, &ba_shared);
    rfc_exec_context ex_shared;
    memset(&ex_shared, 0, sizeof(ex_shared));
    rfc_limits_default(&ex_shared.limits);
    ex_shared.codec_threads = 1u;
    ex_shared.allocator = &ba_shared;
    ex_shared.budget = b_shared;
    void *ds1 = NULL;
    CHECK(png_adp->open_session(&ex_shared, &ds1) == RFC_OK);
    rfc_frame *df1 = NULL;
    size_t dc1 = 0u;
    CHECK(png_adp->decode_session(ds1, &ex_shared, png_rgb8.data, png_rgb8.len,
                                  NULL, 0u, &df1, &dc1, NULL) ==
          RFC_ERR_OUT_OF_MEMORY);
    CHECK(df1 == NULL && dc1 == 0u);
    png_adp->close_session(ds1);
    rfc_budget_stats bs_shared;
    rfc_budget_stats_get(b_shared, &bs_shared);
    CHECK(bs_shared.rejected_count > 0ull);
    CHECK(bs_shared.current_allocated_bytes == 0ull);
    rfc_budget_destroy(b_shared);
    rfc_encoded_free(&png_rgb8);

    /* Route B (libpng 1.6.59): RGB16 64x64 decode (24576B frame + libpng
     * structs > 26624B limit -> hits basic_png_malloc OOM) + encode under 16KiB. */
    rfc_layout l64_16 = {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 64u, 64u, 0u, 16u};
    rfc_frame f64_16 = {0};
    CHECK(test_alloc_frame(&f64_16, &l64_16, NULL, &alloc) == RFC_OK);
    fill_test_pattern(&f64_16, 72u);
    CHECK(png_adp->open_session(&exec, &es) == RFC_OK);
    rfc_encoded png_rgb16 = {0};
    CHECK(png_adp->encode_session(es, &exec, &f64_16, 1u, NULL, NULL, 0u,
                                  &png_rgb16) == RFC_OK);
    png_adp->close_session(es);

    /* Tighten encode budget -> RFC_ERR_OUT_OF_MEMORY from libpng write hooks */
    rfc_budget *b_enc = rfc_budget_create(16384ull);
    CHECK(b_enc != NULL);
    rfc_allocator ba_enc;
    rfc_budget_allocator(b_enc, &ba_enc);
    rfc_exec_context ex_enc;
    memset(&ex_enc, 0, sizeof(ex_enc));
    rfc_limits_default(&ex_enc.limits);
    ex_enc.codec_threads = 1u;
    ex_enc.allocator = &ba_enc;
    ex_enc.budget = b_enc;
    void *es_oom = NULL;
    CHECK(png_adp->open_session(&ex_enc, &es_oom) == RFC_OK);
    rfc_encoded oom_enc = {0};
    CHECK(png_adp->encode_session(es_oom, &ex_enc, &f64_16, 1u, NULL, NULL, 0u,
                                  &oom_enc) == RFC_ERR_OUT_OF_MEMORY);
    png_adp->close_session(es_oom);
    rfc_budget_stats bs_enc;
    rfc_budget_stats_get(b_enc, &bs_enc);
    CHECK(bs_enc.rejected_count > 0ull && bs_enc.current_allocated_bytes == 0ull);
    rfc_budget_destroy(b_enc);
    rfc_frame_free(&f64_16);

    /* Tighten decode budget on RGB16 libpng read -> RFC_ERR_OUT_OF_MEMORY */
    rfc_budget *b_dec = rfc_budget_create(28000ull);
    CHECK(b_dec != NULL);
    rfc_allocator ba_dec;
    rfc_budget_allocator(b_dec, &ba_dec);
    rfc_exec_context ex_dec;
    memset(&ex_dec, 0, sizeof(ex_dec));
    rfc_limits_default(&ex_dec.limits);
    ex_dec.codec_threads = 1u;
    ex_dec.allocator = &ba_dec;
    ex_dec.budget = b_dec;
    void *ds2 = NULL;
    CHECK(png_adp->open_session(&ex_dec, &ds2) == RFC_OK);
    rfc_frame *df2 = NULL;
    size_t dc2 = 0u;
    CHECK(png_adp->decode_session(ds2, &ex_dec, png_rgb16.data, png_rgb16.len,
                                  NULL, 0u, &df2, &dc2, NULL) ==
          RFC_ERR_OUT_OF_MEMORY);
    CHECK(df2 == NULL && dc2 == 0u);
    png_adp->close_session(ds2);
    rfc_budget_stats bs_dec;
    rfc_budget_stats_get(b_dec, &bs_dec);
    CHECK(bs_dec.rejected_count > 0ull && bs_dec.current_allocated_bytes == 0ull);
    rfc_budget_destroy(b_dec);
    rfc_encoded_free(&png_rgb16);

    /* Four explicit eXIf RGB8 PNG orientation outcomes via rfc_scheduler. */
    rfc_layout l8 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 0u, 8u};
    rfc_frame f8 = {0};
    CHECK(test_alloc_frame(&f8, &l8, NULL, &alloc) == RFC_OK);
    fill_test_pattern(&f8, 88u);
    CHECK(png_adp->open_session(&exec, &es) == RFC_OK);
    rfc_encoded base_png = {0};
    CHECK(png_adp->encode_session(es, &exec, &f8, 1u, NULL, NULL, 0u,
                                  &base_png) == RFC_OK);
    png_adp->close_session(es);
    rfc_frame_free(&f8);

    uint8_t ex1_pay[28], ex6_pay[28];
    size_t ex1_n = build_exif_orientation_payload(1u, ex1_pay);
    size_t ex6_n = build_exif_orientation_payload(6u, ex6_pay);
    size_t png_o1_len = 0u, png_o6_len = 0u;
    uint8_t *png_o1 = insert_png_chunk_before_idat(
        base_png.data, base_png.len, "eXIf", ex1_pay, (uint32_t)ex1_n,
        &png_o1_len);
    uint8_t *png_o6 = insert_png_chunk_before_idat(
        base_png.data, base_png.len, "eXIf", ex6_pay, (uint32_t)ex6_n,
        &png_o6_len);

    rfc_scheduler_config scfg;
    rfc_scheduler_config_default(&scfg);
    rfc_scheduler *sched = rfc_scheduler_create(&scfg);
    CHECK(sched != NULL);
    rfc_setting_value s_pres[1] = {{"metadata", "preserve"}};
    rfc_setting_value s_strip[1] = {{"metadata", "strip"}};

    /* 1) Orientation=1 under preserve -> SUCCEEDED + PRESERVED */
    rfc_job_spec jsp;
    memset(&jsp, 0, sizeof(jsp));
    jsp.source_format = RFC_FORMAT_PNG;
    jsp.target_format = RFC_FORMAT_PNG;
    jsp.source_bytes = png_o1;
    jsp.source_size = png_o1_len;
    jsp.settings = s_pres;
    jsp.setting_count = 1u;
    jsp.expected_working_set_bytes = 256u * 1024u;
    jsp.has_stage_ops = 1;
    jsp.stage_ops.on_export = stage_export_noop;
    rfc_job *job = NULL;
    rfc_job_snapshot snap;
    rfc_verify_report rep;
    CHECK(rfc_scheduler_submit(sched, NULL, &jsp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED);
    CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
    CHECK(rep.meta_outcome == RFC_VERIFY_META_PRESERVED);
    rfc_job_release(job);

    /* 2) Orientation=6 under preserve, ack=0 -> RFC_ERR_METADATA_POLICY */
    jsp.source_bytes = png_o6;
    jsp.source_size = png_o6_len;
    CHECK(rfc_scheduler_submit(sched, NULL, &jsp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_FAILED &&
          snap.status == RFC_ERR_METADATA_POLICY);
    CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
    CHECK(rep.meta_outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED);
    CHECK((rep.metadata.unsupported_items & RFC_META_ITEM_ORIENTATION) != 0u);
    rfc_job_release(job);

    /* 3) Orientation=6 under preserve + ack -> SUCCEEDED + UNSUPPORTED_ITEMS_NAMED */
    jsp.acknowledged_unsupported_metadata = RFC_META_ITEM_ORIENTATION;
    CHECK(rfc_scheduler_submit(sched, NULL, &jsp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED);
    CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
    CHECK(rep.meta_outcome == RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED);
    CHECK((rep.metadata.unsupported_items & RFC_META_ITEM_ORIENTATION) != 0u);
    rfc_job_release(job);

    /* 4) Orientation=6 under strip -> SUCCEEDED + STRIPPED_EXPLICITLY */
    jsp.acknowledged_unsupported_metadata = 0u;
    jsp.settings = s_strip;
    CHECK(rfc_scheduler_submit(sched, NULL, &jsp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED);
    CHECK(rfc_job_verify_report_get(job, &rep) == RFC_OK);
    CHECK(rep.meta_outcome == RFC_VERIFY_META_STRIPPED_EXPLICITLY);
    rfc_job_release(job);

    /* Refusals: acTL animated PNG, sBIT reduction, tRNS, sub-8 PNG encode */
    uint8_t actl[8] = {0, 0, 0, 2, 0, 0, 0, 0};
    size_t apng_len = 0u;
    uint8_t *apng_bytes = insert_png_chunk_before_idat(
        base_png.data, base_png.len, "acTL", actl, 8u, &apng_len);
    void *ps = NULL;
    CHECK(png_adp->open_session(&exec, &ps) == RFC_OK);
    rfc_image_probe pr;
    CHECK(png_adp->probe_session(ps, &exec, apng_bytes, apng_len, &pr) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    free(apng_bytes);

    uint8_t sbit_data[3] = {5u, 6u, 5u};
    size_t sbit_len = 0u;
    uint8_t *sbit_png = insert_png_chunk_before_idat(
        base_png.data, base_png.len, "sBIT", sbit_data, 3u, &sbit_len);
    CHECK(png_adp->probe_session(ps, &exec, sbit_png, sbit_len, &pr) ==
          RFC_ERR_UNSUPPORTED_SAMPLE);
    free(sbit_png);

    uint8_t trns_data[6] = {0u, 1u, 0u, 2u, 0u, 3u};
    size_t trns_len = 0u;
    uint8_t *trns_png = insert_png_chunk_before_idat(
        base_png.data, base_png.len, "tRNS", trns_data, 6u, &trns_len);
    CHECK(png_adp->probe_session(ps, &exec, trns_png, trns_len, &pr) ==
          RFC_ERR_UNSUPPORTED_SAMPLE);
    free(trns_png);

    /* Sub-8 bps on PNG encode refused */
    rfc_layout sub4_lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 0u, 4u};
    rfc_frame sub4_fr = {0};
    CHECK(test_alloc_frame(&sub4_fr, &sub4_lay, NULL, &alloc) == RFC_OK);
    fill_test_pattern(&sub4_fr, 99u);
    rfc_encoded sub4_enc = {0};
    CHECK(png_adp->encode_session(ps, &exec, &sub4_fr, 1u, NULL, NULL, 0u,
                                  &sub4_enc) == RFC_ERR_UNSUPPORTED_SAMPLE);
    rfc_frame_free(&sub4_fr);

    png_adp->close_session(ps);
    rfc_scheduler_destroy(sched);
    free(png_o1);
    free(png_o6);
    rfc_encoded_free(&base_png);
    rfc_registry_release_session(RFC_FORMAT_PNG);
    CHECK(st.live == 0u);
    printf("basic: png_depths_routes_budget_and_exif ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 4: PNM (P1..P6) and PAM (P7) full matrix & bit-depth/padding       */
/* ------------------------------------------------------------------------ */

static void test_pnm_and_pam_matrix(void) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    /* PNM supported sample & bps combinations */
    struct {
        rfc_sample_format sf;
        uint32_t bps;
    } pnm_cases[] = {
        {RFC_SAMPLE_GRAY8, 1u},
        {RFC_SAMPLE_GRAY8, 4u},
        {RFC_SAMPLE_GRAY8, 8u},
        {RFC_SAMPLE_GRAY16, 12u},
        {RFC_SAMPLE_GRAY16, 16u},
        {RFC_SAMPLE_RGB8, 4u},
        {RFC_SAMPLE_RGB8, 8u},
        {RFC_SAMPLE_RGB16, 10u},
        {RFC_SAMPLE_RGB16, 16u},
    };
    for (size_t i = 0u; i < sizeof(pnm_cases) / sizeof(pnm_cases[0]); i++) {
        rfc_layout lay = {pnm_cases[i].sf, RFC_ALPHA_NONE, 11u, 5u, 0u,
                          pnm_cases[i].bps};
        rfc_frame src = {0}, dec = {0};
        CHECK(test_alloc_frame(&src, &lay, NULL, &alloc) == RFC_OK);
        fill_test_pattern(&src, (uint32_t)(100 + i));
        roundtrip_session(RFC_FORMAT_PNM, &src, NULL, NULL, 0u, &dec, NULL,
                          &exec);
        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&src, 1u, &dec, 1u, &mm) == RFC_OK);
        rfc_frame_free(&src);
        rfc_frame_free(&dec);
    }

    /* Non-zero low padding bits in sub-8/sub-16 must fail on encode! */
    const rfc_session_adapter *pnm_adp =
        rfc_registry_acquire_session(RFC_FORMAT_PNM);
    CHECK(pnm_adp != NULL);
    void *ps = NULL;
    CHECK(pnm_adp->open_session(&exec, &ps) == RFC_OK);
    rfc_layout pad_lay = {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 4u, 4u, 0u, 4u};
    rfc_frame pad_fr = {0};
    CHECK(test_alloc_frame(&pad_fr, &pad_lay, NULL, &alloc) == RFC_OK);
    fill_test_pattern(&pad_fr, 200u);
    pad_fr.data[3] |= 0x01u; /* non-zero low bit in 4-bit left-aligned sample */
    rfc_encoded pad_enc = {0};
    CHECK(pnm_adp->encode_session(ps, &exec, &pad_fr, 1u, NULL, NULL, 0u,
                                  &pad_enc) == RFC_ERR_INVALID_ARGUMENT);
    rfc_frame_free(&pad_fr);

    /* Non-(2^bits - 1) maxval and trailing byte refusals */
    const char *bad_max = "P5\n2 2\n100\n\x01\x02\x03\x04";
    rfc_image_probe pr;
    CHECK(pnm_adp->probe_session(ps, &exec, (const uint8_t *)bad_max, 15u,
                                 &pr) == RFC_ERR_UNSUPPORTED_SAMPLE);
    const char *p6_extra = "P6\n1 1\n255\n\x10\x20\x30\x99";
    CHECK(pnm_adp->probe_session(ps, &exec, (const uint8_t *)p6_extra, 15u,
                                 &pr) == RFC_ERR_INVALID_ARGUMENT);
    pnm_adp->close_session(ps);
    rfc_registry_release_session(RFC_FORMAT_PNM);

    /* PAM full matrix including BLACKANDWHITE, GRAYSCALE 16, RGB, RGB_ALPHA 8/16 */
    struct {
        rfc_sample_format sf;
        rfc_alpha_mode am;
        uint32_t bps;
    } pam_cases[] = {
        {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 1u},
        {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 8u},
        {RFC_SAMPLE_GRAY16, RFC_ALPHA_NONE, 16u},
        {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u},
        {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 12u},
        {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 8u},
        {RFC_SAMPLE_RGBA16, RFC_ALPHA_STRAIGHT, 16u},
    };
    for (size_t i = 0u; i < sizeof(pam_cases) / sizeof(pam_cases[0]); i++) {
        rfc_layout lay = {pam_cases[i].sf, pam_cases[i].am, 7u, 5u, 0u,
                          pam_cases[i].bps};
        rfc_frame src = {0}, dec = {0};
        CHECK(test_alloc_frame(&src, &lay, NULL, &alloc) == RFC_OK);
        fill_test_pattern(&src, (uint32_t)(250 + i));
        roundtrip_session(RFC_FORMAT_PAM, &src, NULL, NULL, 0u, &dec, NULL,
                          &exec);
        rfc_sample_mismatch mm;
        CHECK(rfc_verify_frames_equal(&src, 1u, &dec, 1u, &mm) == RFC_OK);
        rfc_frame_free(&src);
        rfc_frame_free(&dec);
    }

    /* PAM rejects GRAYSCALE_ALPHA (DEPTH 2) and P6 mismatch */
    const rfc_session_adapter *pam_adp =
        rfc_registry_acquire_session(RFC_FORMAT_PAM);
    CHECK(pam_adp != NULL);
    CHECK(pam_adp->open_session(&exec, &ps) == RFC_OK);
    const char *ga_pam =
        "P7\nWIDTH 1\nHEIGHT 1\nDEPTH 2\nMAXVAL 255\nTUPLTYPE "
        "GRAYSCALE_ALPHA\nENDHDR\n\x10\x20";
    CHECK(pam_adp->probe_session(ps, &exec, (const uint8_t *)ga_pam,
                                 strlen(ga_pam), &pr) ==
          RFC_ERR_UNSUPPORTED_SAMPLE);
    const char *p6_in = "P6\n1 1\n255\n\x01\x02\x03";
    CHECK(pam_adp->probe_session(ps, &exec, (const uint8_t *)p6_in, 14u,
                                 &pr) == RFC_ERR_FORMAT_DISAGREEMENT);
    pam_adp->close_session(ps);
    rfc_registry_release_session(RFC_FORMAT_PAM);
    CHECK(st.live == 0u);
    printf("basic: pnm_and_pam_matrix ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 5: End-to-end scheduler file safety & adapter cancellation          */
/* ------------------------------------------------------------------------ */

static void test_jobs_file_safety_and_cancel(void) {
    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "basic_fs");
    char bmp_p[1024], fold_p[1024], png_p[1024], pam_p[1024], pnm_p[1024];
    char exist_p[1024];
    join_path(bmp_p, sizeof(bmp_p), tmp, "src.bmp");
    join_path(fold_p, sizeof(fold_p), tmp, "mid.fold");
    join_path(png_p, sizeof(png_p), tmp, "mid.png");
    join_path(pam_p, sizeof(pam_p), tmp, "mid.pam");
    join_path(pnm_p, sizeof(pnm_p), tmp, "out.ppm");
    join_path(exist_p, sizeof(exist_p), tmp, "keep.fold");

    /* Write a 12x10 RGB8 BMP via adapter */
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 12u, 10u, 0u, 8u};
    rfc_frame src = {0};
    CHECK(test_alloc_frame(&src, &lay, NULL, &alloc) == RFC_OK);
    fill_test_pattern(&src, 313u);
    const rfc_session_adapter *bmp_adp =
        rfc_registry_acquire_session(RFC_FORMAT_BMP);
    void *bs = NULL;
    CHECK(bmp_adp->open_session(&exec, &bs) == RFC_OK);
    rfc_encoded bmp_bytes = {0};
    CHECK(bmp_adp->encode_session(bs, &exec, &src, 1u, NULL, NULL, 0u,
                                  &bmp_bytes) == RFC_OK);
    CHECK(write_file_all(bmp_p, bmp_bytes.data, bmp_bytes.len));
    bmp_adp->close_session(bs);
    rfc_encoded_free(&bmp_bytes);
    rfc_registry_release_session(RFC_FORMAT_BMP);
    rfc_frame_free(&src);

    const uint8_t keep_sentinel[10] = {'K', 'E', 'E', 'P', '!', 'S', 'E', 'N', 'T', '\n'};
    CHECK(write_file_all(exist_p, keep_sentinel, sizeof(keep_sentinel)));

    rfc_scheduler_config scfg;
    rfc_scheduler_config_default(&scfg);
    rfc_scheduler *sched = rfc_scheduler_create(&scfg);
    CHECK(sched != NULL);

    struct {
        rfc_format_id sfmt;
        rfc_format_id dfmt;
        const char *in_p;
        const char *out_p;
    } chain[4] = {
        {RFC_FORMAT_BMP, RFC_FORMAT_RASTERFOLD, bmp_p, fold_p},
        {RFC_FORMAT_RASTERFOLD, RFC_FORMAT_PNG, fold_p, png_p},
        {RFC_FORMAT_PNG, RFC_FORMAT_PAM, png_p, pam_p},
        {RFC_FORMAT_PAM, RFC_FORMAT_PNM, pam_p, pnm_p},
    };
    for (int i = 0; i < 4; i++) {
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_path(&src_io, chain[i].in_p, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&dst_io, chain[i].out_p,
                              RFC_EXPORT_REFUSE_EXISTING);
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = chain[i].sfmt;
        sp.target_format = chain[i].dfmt;
        sp.io_source = &src_io;
        sp.io_sink = &dst_io;
        sp.expected_working_set_bytes = 256u * 1024u;
        rfc_job *j = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED);
        rfc_job_release(j);
    }

    /* Verify attempting export over exist_p leaves sentinel intact */
    rfc_io_source src_io;
    rfc_io_sink exist_io;
    rfc_io_source_init_path(&src_io, bmp_p, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&exist_io, exist_p, RFC_EXPORT_REFUSE_EXISTING);
    rfc_job_spec ex_sp;
    memset(&ex_sp, 0, sizeof(ex_sp));
    ex_sp.source_format = RFC_FORMAT_BMP;
    ex_sp.target_format = RFC_FORMAT_RASTERFOLD;
    ex_sp.io_source = &src_io;
    ex_sp.io_sink = &exist_io;
    ex_sp.expected_working_set_bytes = 256u * 1024u;
    rfc_job *ej = NULL;
    rfc_status ex_sub = rfc_scheduler_submit(sched, NULL, &ex_sp, &ej);
    CHECK(ex_sub == RFC_ERR_DESTINATION_EXISTS && ej == NULL);

    size_t klen = 0u;
    uint8_t *kbuf = read_file_all(exist_p, &klen);
    CHECK(kbuf && klen == sizeof(keep_sentinel) &&
          memcmp(kbuf, keep_sentinel, sizeof(keep_sentinel)) == 0);
    free(kbuf);

    /* Session-level cancel check across PNG and RASTERFOLD */
    const rfc_session_adapter *png_adp =
        rfc_registry_acquire_session(RFC_FORMAT_PNG);
    size_t png_sz = 0u;
    uint8_t *png_b = read_file_all(png_p, &png_sz);
    CHECK(png_b != NULL);
    void *cs = NULL;
    CHECK(png_adp->open_session(&exec, &cs) == RFC_OK);
    png_adp->cancel_session(cs);
    rfc_image_probe pr;
    CHECK(png_adp->probe_session(cs, &exec, png_b, png_sz, &pr) ==
          RFC_ERR_CANCELLED);
    rfc_frame *cf = NULL;
    size_t cc = 0u;
    CHECK(png_adp->decode_session(cs, &exec, png_b, png_sz, NULL, 0u, &cf, &cc,
                                  NULL) == RFC_ERR_CANCELLED);
    png_adp->close_session(cs);
    rfc_registry_release_session(RFC_FORMAT_PNG);
    free(png_b);

    rfc_scheduler_destroy(sched);
    rm_tree(tmp);
    CHECK(st.live == 0u);
    printf("basic: jobs_file_safety_and_cancel ok\n");
}

/* ------------------------------------------------------------------------ */
/* CLI subcommands driven from tests/converter/test_basic.py                */
/* ------------------------------------------------------------------------ */

static int parse_fmt_cli(const char *s, rfc_format_id *out) {
    const rfc_capability *c = rfc_registry_find_name(s);
    if (!c) {
        return 0;
    }
    *out = c->id;
    return 1;
}

static int cli_main(int argc, char **argv) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    const char *sub = argv[2];

    if (strcmp(sub, "dump") == 0 && (argc == 6 || argc == 8)) {
        rfc_format_id fmt = 0;
        if (!parse_fmt_cli(argv[3], &fmt)) {
            return 2;
        }
        size_t in_len = 0u;
        uint8_t *in_b = read_file_all(argv[4], &in_len);
        if (!in_b && in_len == 0u) {
            /* could be 0-byte empty file */
            in_b = (uint8_t *)malloc(1u);
        }
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        if (argc == 8) {
            uint32_t mw = (uint32_t)strtoul(argv[6], NULL, 10);
            uint64_t md = (uint64_t)strtoull(argv[7], NULL, 10);
            if (mw > 0u) {
                exec.limits.max_width = mw;
            }
            if (md > 0ull) {
                exec.limits.max_decoded_bytes = md;
            }
        }
        const rfc_session_adapter *adp = rfc_registry_acquire_session(fmt);
        if (!adp) {
            free(in_b);
            printf("STATUS=%s PEAK_ALLOC=%llu\n",
                   rfc_status_name(RFC_ERR_NO_ADAPTER),
                   (unsigned long long)rf_internal_alloc_peak);
            return 2;
        }
        void *sess = NULL;
        rfc_status r = adp->open_session(&exec, &sess);
        rfc_image_probe pr;
        memset(&pr, 0, sizeof(pr));
        if (r == RFC_OK) {
            r = adp->probe_session(sess, &exec, in_b, in_len, &pr);
        }
        if (pr.container_meta.icc) {
            alloc.free(alloc.ctx, (void *)pr.container_meta.icc);
            pr.container_meta.icc = NULL;
        }
        rfc_frame *frames = NULL;
        size_t fcnt = 0u;
        rfc_metadata dm;
        memset(&dm, 0, sizeof(dm));
        if (r == RFC_OK) {
            r = adp->decode_session(sess, &exec, in_b, in_len, NULL, 0u,
                                    &frames, &fcnt, &dm);
        }
        adp->close_session(sess);
        rfc_registry_release_session(fmt);
        free(in_b);
        if (r != RFC_OK) {
            printf("STATUS=%s PEAK_ALLOC=%llu\n", rfc_status_name(r),
                   (unsigned long long)rf_internal_alloc_peak);
            return 2;
        }

        size_t row_b = (size_t)frames[0].layout.width *
                       ((size_t)(frames[0].layout.format == RFC_SAMPLE_GRAY8 ||
                                 frames[0].layout.format == RFC_SAMPLE_GRAY16
                                     ? 1u
                                     : (frames[0].layout.format == RFC_SAMPLE_RGB8 ||
                                                frames[0].layout.format ==
                                                    RFC_SAMPLE_RGB16
                                            ? 3u
                                            : 4u))) *
                       ((frames[0].layout.format == RFC_SAMPLE_GRAY16 ||
                         frames[0].layout.format == RFC_SAMPLE_RGB16 ||
                         frames[0].layout.format == RFC_SAMPLE_RGBA16)
                            ? 2u
                            : 1u);
        size_t pay_len = row_b * (size_t)frames[0].layout.height;
        size_t icc_len = frames[0].metadata.icc_len;
        uint8_t hdr[40] = {'R', 'F', 'B', 'D'};
        wr32le(hdr + 4, frames[0].layout.width);
        wr32le(hdr + 8, frames[0].layout.height);
        wr32le(hdr + 12, (uint32_t)frames[0].layout.format);
        wr32le(hdr + 16, (uint32_t)frames[0].layout.alpha);
        wr32le(hdr + 20, frames[0].layout.bits_per_sample);
        wr32le(hdr + 24, (uint32_t)row_b);
        wr32le(hdr + 28, (uint32_t)frames[0].metadata.orientation);
        wr32le(hdr + 32, (uint32_t)icc_len);
        wr32le(hdr + 36, (uint32_t)pay_len);
        FILE *fp = fopen(argv[5], "wb");
        CHECK(fp != NULL);
        CHECK(fwrite(hdr, 1u, sizeof(hdr), fp) == sizeof(hdr));
        if (icc_len > 0u) {
            CHECK(fwrite(frames[0].metadata.icc, 1u, icc_len, fp) == icc_len);
        }
        for (uint32_t y = 0u; y < frames[0].layout.height; y++) {
            CHECK(fwrite(frames[0].data +
                             (size_t)y * frames[0].layout.stride_bytes,
                         1u, row_b, fp) == row_b);
        }
        fclose(fp);
        rfc_frame_free(&frames[0]);
        alloc.free(alloc.ctx, frames);
        printf("STATUS=ok PEAK_ALLOC=%llu LIVE=%zu\n",
               (unsigned long long)rf_internal_alloc_peak, st.live);
        return 0;
    }

    if (strcmp(sub, "encode") == 0 && argc == 7) {
        rfc_format_id fmt = 0;
        if (!parse_fmt_cli(argv[3], &fmt)) {
            return 2;
        }
        size_t raw_len = 0u;
        uint8_t *raw = read_file_all(argv[4], &raw_len);
        CHECK(raw != NULL && raw_len >= 40u &&
              memcmp(raw, "RFBE", 4) == 0);
        uint32_t w = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) |
                     ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
        uint32_t h = (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) |
                     ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
        uint32_t sf = (uint32_t)raw[12] | ((uint32_t)raw[13] << 8) |
                      ((uint32_t)raw[14] << 16) | ((uint32_t)raw[15] << 24);
        uint32_t am = (uint32_t)raw[16] | ((uint32_t)raw[17] << 8) |
                      ((uint32_t)raw[18] << 16) | ((uint32_t)raw[19] << 24);
        uint32_t bps = (uint32_t)raw[20] | ((uint32_t)raw[21] << 8) |
                       ((uint32_t)raw[22] << 16) | ((uint32_t)raw[23] << 24);
        uint32_t ori = (uint32_t)raw[24] | ((uint32_t)raw[25] << 8) |
                       ((uint32_t)raw[26] << 16) | ((uint32_t)raw[27] << 24);
        uint32_t icc_len =
            (uint32_t)raw[28] | ((uint32_t)raw[29] << 8) |
            ((uint32_t)raw[30] << 16) | ((uint32_t)raw[31] << 24);
        uint32_t pay_len =
            (uint32_t)raw[32] | ((uint32_t)raw[33] << 8) |
            ((uint32_t)raw[34] << 16) | ((uint32_t)raw[35] << 24);
        CHECK(40u + (size_t)icc_len + (size_t)pay_len <= raw_len);

        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        rfc_layout lay = {(rfc_sample_format)sf, (rfc_alpha_mode)am, w, h, 0u,
                          (uint8_t)bps};
        rfc_metadata meta = {icc_len ? (raw + 40u) : NULL, icc_len,
                             (rfc_orientation)ori, 0u};
        rfc_frame fr = {0};
        CHECK(test_alloc_frame(&fr, &lay, &meta, &alloc) == RFC_OK);
        memcpy(fr.data, raw + 40u + icc_len, pay_len);

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
        printf("STATUS=%s OUT_BYTES=%zu\n", rfc_status_name(r), elen);
        return r == RFC_OK ? 0 : 2;
    }

    if (strcmp(sub, "convert") == 0 && argc == 11) {
        rfc_format_id sfmt = 0, dfmt = 0;
        if (!parse_fmt_cli(argv[3], &sfmt) || !parse_fmt_cli(argv[4], &dfmt)) {
            return 2;
        }
        const char *in_p = argv[5];
        const char *out_p = argv[6];
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
        rfc_io_source_init_path(&src_io, in_p, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&dst_io, out_p, RFC_EXPORT_REFUSE_EXISTING);
        rfc_limits lim;
        rfc_limits_default(&lim);
        if (max_w > 0u) {
            lim.max_width = max_w;
        }
        if (max_dec > 0ull) {
            lim.max_decoded_bytes = max_dec;
        }
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
        sp.expected_working_set_bytes = 256u * 1024u;
        rfc_job *j = NULL;
        rfc_status sub_r = rfc_scheduler_submit(sched, NULL, &sp, &j);
        if (sub_r != RFC_OK) {
            printf("STATUS=%s PEAK_ALLOC=%llu\n", rfc_status_name(sub_r),
                   (unsigned long long)rf_internal_alloc_peak);
            rfc_scheduler_destroy(sched);
            return 2;
        }
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        rfc_verify_report rep;
        int has_rep = (rfc_job_verify_report_get(j, &rep) == RFC_OK);
        printf("STATUS=%s META_OUTCOME=%s PEAK_ALLOC=%llu\n",
               rfc_status_name(snap.status),
               has_rep ? rfc_verify_meta_outcome_name(rep.meta_outcome) : "none",
               (unsigned long long)rf_internal_alloc_peak);
        rfc_job_release(j);
        rfc_scheduler_destroy(sched);
        return snap.state == RFC_JOB_SUCCEEDED ? 0 : 2;
    }

    if (strcmp(sub, "budget-png-decode") == 0 && argc == 5) {
        size_t plen = 0u;
        uint8_t *pb = read_file_all(argv[3], &plen);
        CHECK(pb != NULL);
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
            rfc_registry_acquire_session(RFC_FORMAT_PNG);
        CHECK(adp != NULL);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_status r = adp->decode_session(ds, &exec, pb, plen, NULL, 0u, &df,
                                           &dc, NULL);
        if (df) {
            rfc_frame_free(&df[0]);
            ba.free(ba.ctx, df);
        }
        adp->close_session(ds);
        rfc_registry_release_session(RFC_FORMAT_PNG);
        rfc_budget_stats bs;
        rfc_budget_stats_get(b, &bs);
        printf("STATUS=%s REJECTED=%llu CURR_ALLOC=%llu\n", rfc_status_name(r),
               (unsigned long long)bs.rejected_count,
               (unsigned long long)bs.current_allocated_bytes);
        rfc_budget_destroy(b);
        free(pb);
        return r == RFC_OK ? 0 : 2;
    }

    return 2;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "cli") == 0) {
        return cli_main(argc, argv);
    }
    test_registration_and_fold_v2_v3();
    test_bmp_depths_alpha_and_icc();
    test_png_depths_routes_budget_and_exif();
    test_pnm_and_pam_matrix();
    test_jobs_file_safety_and_cancel();
    printf("basic: all checks passed (%d checks)\n", g_checks);
    return 0;
}
