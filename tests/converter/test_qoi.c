/* C11 codec & format adapter test suite + CLI harness for D10 T02 s8:
 *   RFC_FORMAT_QOI (bundled phoboslab/qoi 0.0.0-ffb2d2cb: RGB8/RGBA8 including
 *   A==0 RGB preservation, padded strides, sRGB/linear colorspace round-trip,
 *   metadata policy outcomes, budget/limits/cancel, malformed stream refusals,
 *   layout-free file jobs, rollback, and same-format byte-identical copy). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "rfc_codec.h"
#include "rfc_codecs.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#ifndef QOI_NO_STDIO
#define QOI_NO_STDIO
#endif
#include <qoi.h>

#include <stdatomic.h>
#include <stdint.h>
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
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

typedef struct tracked_alloc {
    size_t live;
    size_t calls;
} tracked_alloc;

static void *t_alloc(void *ctx, size_t sz) {
    tracked_alloc *st = (tracked_alloc *)ctx;
    void *p = malloc(sz ? sz : 1u);
    if (p && st) {
        st->live++;
        st->calls++;
    }
    return p;
}

static void t_free(void *ctx, void *p) {
    tracked_alloc *st = (tracked_alloc *)ctx;
    if (p) {
        if (st && st->live > 0u) {
            st->live--;
        }
        free(p);
    }
}

static uint64_t budget_used(const rfc_budget *bud) {
    rfc_budget_stats bs;
    memset(&bs, 0, sizeof(bs));
    rfc_budget_stats_get(bud, &bs);
    return bs.current_allocated_bytes;
}

static void init_exec(rfc_exec_context *e, tracked_alloc *st,
                      rfc_allocator *al, rfc_budget *bud) {
    memset(e, 0, sizeof(*e));
    memset(st, 0, sizeof(*st));
    al->alloc = t_alloc;
    al->free = t_free;
    al->ctx = st;
    rfc_limits_default(&e->limits);
    e->codec_threads = 1u;
    e->allocator = al;
    e->budget = bud;
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
    long len = ftell(fp);
    if (len < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc(len > 0 ? (size_t)len : 1u);
    if (!buf || (len > 0 && fread(buf, 1u, (size_t)len, fp) != (size_t)len)) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_len = (size_t)len;
    return buf;
}

static int write_file_all(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    int ok = (len == 0u || fwrite(data, 1u, len, fp) == len);
    return (fclose(fp) == 0) && ok;
}

static int file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
}

static char g_test_dir[256] = {0};

static void create_private_test_dir(void) {
    if (g_test_dir[0] != '\0') {
        return;
    }
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(base), base);
    CHECK(n > 0u && n < sizeof(base) - 64u);
    for (unsigned attempt = 0u; attempt < 256u; attempt++) {
        snprintf(g_test_dir, sizeof(g_test_dir), "%srfc_qoi_%lu_%lu_%u",
                 base, (unsigned long)GetCurrentProcessId(),
                 (unsigned long)GetTickCount(), attempt);
        if (CreateDirectoryA(g_test_dir, NULL)) {
            return;
        }
    }
    CHECK(0 && "failed to create exclusive Windows temp directory");
#else
    char tmpl[] = "/tmp/rfc_qoi_XXXXXX";
    char *created = mkdtemp(tmpl);
    CHECK(created != NULL);
    snprintf(g_test_dir, sizeof(g_test_dir), "%s", created);
#endif
}

static void remove_private_test_dir(void) {
    if (g_test_dir[0] == '\0') {
        return;
    }
#ifdef _WIN32
    (void)RemoveDirectoryA(g_test_dir);
#else
    (void)rmdir(g_test_dir);
#endif
    g_test_dir[0] = '\0';
}

static int dir_has_partial_files(const char *dir) {
#ifdef _WIN32
    char pat[320];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int found = 0;
    do {
        if (strstr(fd.cFileName, ".partial") != NULL ||
            strstr(fd.cFileName, ".rfc-partial") != NULL) {
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strstr(ent->d_name, ".partial") != NULL ||
            strstr(ent->d_name, ".rfc-partial") != NULL) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
#endif
}

static void temp_path(char *buf, size_t cap, const char *tag) {
    static unsigned counter = 0u;
    create_private_test_dir();
    counter++;
#ifdef _WIN32
    snprintf(buf, cap, "%s\\%u_%s", g_test_dir, counter, tag);
#else
    snprintf(buf, cap, "%s/%u_%s", g_test_dir, counter, tag);
#endif
}

static void fill_pattern_rgb8(uint8_t *buf, uint32_t w, uint32_t h,
                              size_t stride) {
    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *row = buf + (size_t)y * stride;
        for (uint32_t x = 0u; x < w; x++) {
            row[(size_t)x * 3u + 0u] = (uint8_t)((x * 37u + y * 17u) & 255u);
            row[(size_t)x * 3u + 1u] = (uint8_t)((x * 13u + y * 53u + 9u) & 255u);
            row[(size_t)x * 3u + 2u] = (uint8_t)((x * 71u ^ (y * 29u)) & 255u);
        }
        for (size_t p = (size_t)w * 3u; p < stride; p++) {
            row[p] = (uint8_t)(0xA5u ^ (uint8_t)(y + p));
        }
    }
}

static void fill_pattern_rgba8(uint8_t *buf, uint32_t w, uint32_t h,
                               size_t stride) {
    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *row = buf + (size_t)y * stride;
        for (uint32_t x = 0u; x < w; x++) {
            uint8_t *px = row + (size_t)x * 4u;
            if ((x + y) % 5u == 0u) {
                /* Fully transparent pixel with distinct non-zero RGB payload. */
                px[0] = (uint8_t)(17u + ((x * 23u) & 127u));
                px[1] = (uint8_t)(134u + ((y * 19u) & 63u));
                px[2] = (uint8_t)(241u - ((x + y) & 63u));
                px[3] = 0u;
            } else if ((x + y) % 7u == 0u) {
                px[0] = 255u;
                px[1] = 0u;
                px[2] = 128u;
                px[3] = 255u;
            } else {
                px[0] = (uint8_t)((x * 29u + y * 11u) & 255u);
                px[1] = (uint8_t)((x * 43u + y * 7u) & 255u);
                px[2] = (uint8_t)((x * 5u + y * 61u) & 255u);
                px[3] = (uint8_t)(1u + ((x * 13u + y * 31u) % 254u));
            }
        }
        for (size_t p = (size_t)w * 4u; p < stride; p++) {
            row[p] = (uint8_t)(0x5Au ^ (uint8_t)(y * 3u + p));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Suite 1: Registration, memory contract, geometry sniff                   */
/* ------------------------------------------------------------------------ */

static void test_registration_and_geometry(void) {
    const rfc_capability *cap = rfc_registry_find(RFC_FORMAT_QOI);
    CHECK(cap != NULL);
    CHECK(cap->availability == RFC_AVAILABILITY_USABLE);
    CHECK(cap->can_read == 1 && cap->can_write == 1);
    CHECK(cap->metadata_items == RFC_META_ITEM_NONE);
    CHECK(strcmp(cap->provider, "qoi (bundled, pinned by D10 T01 \xc2\xa7" "2)") == 0);

    CHECK(rfc_registry_select(RFC_FORMAT_QOI, RFC_MODE_READ) == RFC_ERR_NO_ADAPTER);
    CHECK(rfc_registry_select(RFC_FORMAT_QOI, RFC_MODE_WRITE) == RFC_ERR_NO_ADAPTER);

    CHECK(rfc_codecs_register_qoi() == RFC_OK);
    CHECK(rfc_codecs_register_qoi() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_select(RFC_FORMAT_QOI, RFC_MODE_READ) == RFC_OK);
    CHECK(rfc_registry_select(RFC_FORMAT_QOI, RFC_MODE_WRITE) == RFC_OK);

    const rfc_session_adapter *acq = rfc_registry_acquire_session(RFC_FORMAT_QOI);
    CHECK(acq != NULL);
    CHECK(rfc_codecs_unregister_qoi() == RFC_ERR_BUSY);
    rfc_registry_release_session(RFC_FORMAT_QOI);
    CHECK(rfc_codecs_unregister_qoi() == RFC_OK);
    CHECK(rfc_codecs_register_qoi() == RFC_OK);

    rfc_provider_memory_info mem;
    memset(&mem, 0, sizeof(mem));
    CHECK(rfc_query_provider_memory_contract(RFC_FORMAT_QOI, &mem) == RFC_OK);
    CHECK(mem.mode == RFC_MEM_RESERVATION_AND_LIMITS);
    CHECK(mem.working_set_multiplier == 5u);
    CHECK(mem.fixed_overhead_bytes == 256ull * 1024ull);

    /* Sniff both RGB8 and RGBA8 headers (format + dimensions + channel layout). */
    const uint8_t hdr_rgb[22] = {
        'q', 'o', 'i', 'f', 0, 0, 0, 4, 0, 0, 0, 4, 3, 0,
        0, 0, 0, 0, 0, 0, 0, 1
    };
    const uint8_t hdr_rgba[22] = {
        'q', 'o', 'i', 'f', 0, 0, 0, 4, 0, 0, 0, 4, 4, 1,
        0, 0, 0, 0, 0, 0, 0, 1
    };
    rfc_sniff_result sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(rfc_input_sniff_bytes(hdr_rgb, sizeof(hdr_rgb), "a.qoi", &sr) == RFC_OK);
    CHECK(sr.content_format == RFC_FORMAT_QOI && !sr.disagreement);
    memset(&sr, 0, sizeof(sr));
    CHECK(rfc_input_sniff_bytes(hdr_rgba, sizeof(hdr_rgba), "b.qoi", &sr) == RFC_OK);
    CHECK(sr.content_format == RFC_FORMAT_QOI && !sr.disagreement);

    /* Prove sniff_qoi_geometry extracts both dimensions and 3-vs-4 channel
     * sample format on layout-free scheduler submission: 4x4 RGB8 = 48 B fits
     * under max_decoded_bytes=50, while 4x4 RGBA8 = 64 B is refused at submit. */
    {
        rfc_scheduler_config scfg;
        rfc_scheduler_config_default(&scfg);
        rfc_scheduler *sched = rfc_scheduler_create(&scfg);
        CHECK(sched != NULL);
        rfc_limits tight_lim;
        rfc_limits_default(&tight_lim);
        tight_lim.max_decoded_bytes = 50u;
        rfc_job_spec sp;
        rfc_job *j = NULL;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = RFC_FORMAT_QOI;
        sp.target_format = RFC_FORMAT_QOI;
        sp.source_bytes = hdr_rgba;
        sp.source_size = sizeof(hdr_rgba);
        sp.limits_override = &tight_lim;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(j == NULL);

        /* Valid 4x4 RGB8 QOI (16 pixels = 48 B <= 50 B) is admitted by geometry sniff. */
        uint8_t rgb_ok[14 + 1 + 8] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 4, 0, 0, 0, 4, 3, 0,
            0xCF, /* QOI_OP_RUN(16) */
            0, 0, 0, 0, 0, 0, 0, 1
        };
        char p_geo[256];
        temp_path(p_geo, sizeof(p_geo), "geo_rgb.qoi");
        rfc_io_sink gso;
        rfc_io_sink_init_path(&gso, p_geo, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_bytes = rgb_ok;
        sp.source_size = sizeof(rgb_ok);
        sp.io_sink = &gso;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(j);
        remove(p_geo);
        rfc_scheduler_destroy(sched);
    }

    printf("qoi: registration_and_geometry ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 2: RGB8, RGBA8 (A==0), padded strides, sRGB and linear colorspace  */
/* ------------------------------------------------------------------------ */

static void test_rgb_rgba_stride_and_colorspace(void) {
    const rfc_session_adapter *adp = rfc_registry_session_adapter(RFC_FORMAT_QOI);
    CHECK(adp != NULL);

    tracked_alloc st;
    rfc_allocator al;
    rfc_budget *bud = rfc_budget_create(32ull * 1024ull * 1024ull);
    CHECK(bud != NULL);
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, bud);

    void *sess = NULL;
    CHECK(adp->open_session(&exec, &sess) == RFC_OK);

    /* 1. RGB8 & RGBA8 (including A==0 distinct RGB) with tight and padded strides. */
    const uint32_t w = 23u, h = 19u;
    const size_t pad_cases[2] = {0u, 13u};
    for (int is_rgba = 0; is_rgba < 2; is_rgba++) {
        for (int pi = 0; pi < 2; pi++) {
            size_t bpp = is_rgba ? 4u : 3u;
            size_t stride = (size_t)w * bpp + pad_cases[pi];
            rfc_layout lay;
            memset(&lay, 0, sizeof(lay));
            lay.format = is_rgba ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8;
            lay.alpha = is_rgba ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
            lay.width = w;
            lay.height = h;
            lay.stride_bytes = stride;
            lay.bits_per_sample = 8u;

            rfc_frame src;
            CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
            if (is_rgba) {
                fill_pattern_rgba8(src.data, w, h, stride);
            } else {
                fill_pattern_rgb8(src.data, w, h, stride);
            }

            rfc_encoded enc;
            CHECK(adp->encode_session(sess, &exec, &src, 1u, NULL, NULL, 0u, &enc) ==
                  RFC_OK);
            CHECK(enc.len >= 22u && enc.data[12] == (uint8_t)bpp &&
                  enc.data[13] == QOI_SRGB);
            CHECK(budget_used(bud) == 0u);

            rfc_image_probe pr;
            CHECK(adp->probe_session(sess, &exec, enc.data, enc.len, &pr) == RFC_OK);
            CHECK(pr.first_frame.width == w && pr.first_frame.height == h &&
                  pr.first_frame.format == lay.format && pr.frame_count == 1u &&
                  !rfc_colorimetry_has_any(&pr.container_meta.color));

            rfc_frame *dec = NULL;
            size_t dec_n = 0u;
            rfc_metadata dec_meta;
            CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u,
                                      &dec, &dec_n, &dec_meta) == RFC_OK);
            CHECK(dec_n == 1u && dec != NULL);
            CHECK(dec[0].layout.width == w && dec[0].layout.height == h &&
                  dec[0].layout.format == lay.format &&
                  dec[0].layout.alpha == lay.alpha &&
                  dec[0].layout.bits_per_sample == 8u);
            for (uint32_t y = 0u; y < h; y++) {
                CHECK(memcmp(src.data + (size_t)y * stride,
                             dec[0].data + (size_t)y * dec[0].layout.stride_bytes,
                             (size_t)w * bpp) == 0);
            }
            CHECK(budget_used(bud) == 0u);

            rfc_verify_report vrep;
            CHECK(rfc_verify_encoded_fresh(RFC_FORMAT_QOI, &enc, &src, 1u, NULL,
                                           RFC_META_POLICY_PRESERVE, 0u, NULL, 0u,
                                           &exec.limits, &al, &vrep) == RFC_OK);
            CHECK(vrep.valid == 1);

            rfc_frame_free(&dec[0]);
            al.free(al.ctx, dec);
            rfc_encoded_free(&enc);
            rfc_frame_free(&src);
        }
    }

    /* 2. QOI_LINEAR (colorspace=1) round-trip + emitted header inspection + mismatch refusal. */
    {
        rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 11u, 9u, 44u, 8u};
        rfc_metadata lin_meta;
        memset(&lin_meta, 0, sizeof(lin_meta));
        lin_meta.color.has_cicp = 1u;
        lin_meta.color.cicp[0] = 2u;
        lin_meta.color.cicp[1] = 8u;
        lin_meta.color.cicp[2] = 0u;
        lin_meta.color.cicp[3] = 1u;

        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, &lin_meta, &al) == RFC_OK);
        fill_pattern_rgba8(src.data, 11u, 9u, 44u);

        rfc_encoded enc_lin;
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &lin_meta, NULL, 0u,
                                  &enc_lin) == RFC_OK);
        CHECK(enc_lin.len >= 22u && enc_lin.data[13] == QOI_LINEAR);

        rfc_image_probe pr;
        CHECK(adp->probe_session(sess, &exec, enc_lin.data, enc_lin.len, &pr) ==
              RFC_OK);
        CHECK(pr.container_meta.color.has_cicp == 1u &&
              pr.container_meta.color.cicp[0] == 2u &&
              pr.container_meta.color.cicp[1] == 8u &&
              pr.container_meta.color.cicp[2] == 0u &&
              pr.container_meta.color.cicp[3] == 1u);

        rfc_verify_report vrep;
        CHECK(rfc_verify_encoded_fresh(RFC_FORMAT_QOI, &enc_lin, &src, 1u,
                                       &lin_meta, RFC_META_POLICY_PRESERVE, 0u,
                                       NULL, 0u, &exec.limits, &al, &vrep) ==
              RFC_OK);
        CHECK(vrep.valid == 1);
        CHECK(vrep.metadata.outcome == RFC_VERIFY_META_PRESERVED);
        CHECK((vrep.metadata.preserved_items & RFC_META_ITEM_ICC) != 0u);

        /* Flipping header byte 13 from linear (1) to sRGB (0) must fail verification! */
        enc_lin.data[13] = QOI_SRGB;
        CHECK(rfc_verify_encoded_fresh(RFC_FORMAT_QOI, &enc_lin, &src, 1u,
                                       &lin_meta, RFC_META_POLICY_PRESERVE, 0u,
                                       NULL, 0u, &exec.limits, &al, &vrep) ==
              RFC_ERR_VERIFY_MISMATCH);

        /* Flipping sRGB source's output header from 0 to 1 must also fail verification! */
        rfc_frame src_srgb;
        CHECK(rfc_frame_alloc(&src_srgb, &lay, NULL, &al) == RFC_OK);
        memcpy(src_srgb.data, src.data, src.data_len);
        enc_lin.data[13] = QOI_LINEAR;
        CHECK(rfc_verify_encoded_fresh(RFC_FORMAT_QOI, &enc_lin, &src_srgb, 1u,
                                       NULL, RFC_META_POLICY_PRESERVE, 0u,
                                       NULL, 0u, &exec.limits, &al, &vrep) ==
              RFC_ERR_VERIFY_MISMATCH);

        /* Explicit metadata=strip on linear source emits colorspace=0 (sRGB). */
        rfc_setting_value strip_sv[1] = {{"metadata", "strip"}};
        rfc_encoded enc_strip;
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &lin_meta, strip_sv,
                                  1u, &enc_strip) == RFC_OK);
        CHECK(enc_strip.data[13] == QOI_SRGB);
        CHECK(rfc_verify_encoded_fresh(RFC_FORMAT_QOI, &enc_strip, &src, 1u,
                                       &lin_meta, RFC_META_POLICY_STRIP, 0u,
                                       NULL, 0u, &exec.limits, &al, &vrep) ==
              RFC_OK);
        CHECK(vrep.metadata.outcome == RFC_VERIFY_META_STRIPPED_EXPLICITLY);

        rfc_encoded_free(&enc_strip);
        rfc_encoded_free(&enc_lin);
        rfc_frame_free(&src_srgb);
        rfc_frame_free(&src);
    }

    adp->close_session(sess);
    CHECK(st.live == 0u);
    CHECK(budget_used(bud) == 0u);
    rfc_budget_destroy(bud);
    printf("qoi: rgb_rgba_stride_and_colorspace ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 3: Refusals, named malformed fixtures (zero alloc), metadata       */
/* ------------------------------------------------------------------------ */

static void assert_malformed_refused(const rfc_session_adapter *adp,
                                     void *sess,
                                     const uint8_t *blob,
                                     size_t len,
                                     rfc_status expected) {
    const rfc_setting_value sv_preserve[1] = {{"metadata", "preserve"}};
    const rfc_setting_value sv_strip[1] = {{"metadata", "strip"}};
    const rfc_setting_value *modes[2] = {sv_preserve, sv_strip};
    for (int m = 0; m < 2; m++) {
        tracked_alloc st;
        rfc_allocator al;
        rfc_budget *bud = rfc_budget_create(32ull * 1024ull * 1024ull);
        CHECK(bud != NULL);
        rfc_exec_context exec;
        init_exec(&exec, &st, &al, bud);

        rfc_image_probe pr;
        CHECK(adp->probe_session(sess, &exec, blob, len, &pr) == expected);
        rfc_budget_stats bs;
        rfc_budget_stats_get(bud, &bs);
        CHECK(st.calls == 0u && st.live == 0u &&
              bs.peak_allocated_bytes == 0u && bs.allocation_count == 0u);

        rfc_frame *dec = NULL;
        size_t dec_n = 0u;
        CHECK(adp->decode_session(sess, &exec, blob, len, modes[m], 1u, &dec,
                                  &dec_n, NULL) == expected);
        rfc_budget_stats_get(bud, &bs);
        CHECK(dec == NULL && dec_n == 0u);
        CHECK(st.calls == 0u && st.live == 0u &&
              bs.peak_allocated_bytes == 0u && bs.allocation_count == 0u);
        rfc_budget_destroy(bud);
    }
}

static void test_refusals_malformed_and_metadata_policy(void) {
    const rfc_session_adapter *adp = rfc_registry_session_adapter(RFC_FORMAT_QOI);
    CHECK(adp != NULL);

    tracked_alloc st;
    rfc_allocator al;
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, NULL);
    void *sess = NULL;
    CHECK(adp->open_session(&exec, &sess) == RFC_OK);

    /* 1. Unsupported sample formats, bit depths, alpha, multi-frame. */
    {
        const rfc_sample_format bad_fmts[] = {
            RFC_SAMPLE_GRAY8, RFC_SAMPLE_GRAY16, RFC_SAMPLE_RGB16,
            RFC_SAMPLE_RGBA16, RFC_SAMPLE_RGB_HALF, RFC_SAMPLE_RGBA_FLOAT
        };
        for (size_t i = 0u; i < sizeof(bad_fmts) / sizeof(bad_fmts[0]); i++) {
            rfc_sample_format sf = bad_fmts[i];
            size_t bpp = rfc_sample_bytes(sf);
            uint32_t bps = (sf == RFC_SAMPLE_GRAY8) ? 8u
                         : (sf == RFC_SAMPLE_RGBA_FLOAT) ? 32u : 16u;
            rfc_alpha_mode am = (sf == RFC_SAMPLE_RGBA16 || sf == RFC_SAMPLE_RGBA_FLOAT)
                                    ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
            uint8_t px[64] = {0};
            rfc_frame fr;
            memset(&fr, 0, sizeof(fr));
            fr.layout.format = sf;
            fr.layout.alpha = am;
            fr.layout.width = 2u;
            fr.layout.height = 2u;
            fr.layout.stride_bytes = 2u * bpp;
            fr.layout.bits_per_sample = bps;
            fr.data = px;
            fr.data_len = sizeof(px);
            rfc_encoded out;
            st.calls = 0u;
            CHECK(adp->encode_session(sess, &exec, &fr, 1u, NULL, NULL, 0u,
                                      &out) == RFC_ERR_UNSUPPORTED_SAMPLE);
            CHECK(st.calls == 0u);
        }

        /* Premultiplied alpha refused before work. */
        uint8_t px4[16] = {10, 20, 30, 128};
        rfc_frame fr_pm;
        memset(&fr_pm, 0, sizeof(fr_pm));
        fr_pm.layout.format = RFC_SAMPLE_RGBA8;
        fr_pm.layout.alpha = RFC_ALPHA_PREMULTIPLIED;
        fr_pm.layout.width = 2u;
        fr_pm.layout.height = 2u;
        fr_pm.layout.stride_bytes = 8u;
        fr_pm.layout.bits_per_sample = 8u;
        fr_pm.data = px4;
        fr_pm.data_len = sizeof(px4);
        rfc_encoded out;
        st.calls = 0u;
        CHECK(adp->encode_session(sess, &exec, &fr_pm, 1u, NULL, NULL, 0u,
                                  &out) == RFC_ERR_UNSUPPORTED_ALPHA);
        CHECK(st.calls == 0u);

        /* Multi-frame refused before work. */
        rfc_frame two[2];
        memset(two, 0, sizeof(two));
        for (int k = 0; k < 2; k++) {
            two[k].layout.format = RFC_SAMPLE_RGB8;
            two[k].layout.alpha = RFC_ALPHA_NONE;
            two[k].layout.width = 2u;
            two[k].layout.height = 2u;
            two[k].layout.stride_bytes = 6u;
            two[k].layout.bits_per_sample = 8u;
            two[k].data = px4;
            two[k].data_len = 12u;
        }
        st.calls = 0u;
        CHECK(adp->encode_session(sess, &exec, two, 2u, NULL, NULL, 0u,
                                  &out) == RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(st.calls == 0u);
    }

    /* 2. Metadata policy refusals under preserve mode (ICC, sRGB/gAMA/other cICP,
     *    EXIF, XMP, orientation, frame timing). */
    {
        uint8_t px[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        rfc_frame fr;
        memset(&fr, 0, sizeof(fr));
        fr.layout.format = RFC_SAMPLE_RGB8;
        fr.layout.alpha = RFC_ALPHA_NONE;
        fr.layout.width = 2u;
        fr.layout.height = 2u;
        fr.layout.stride_bytes = 6u;
        fr.layout.bits_per_sample = 8u;
        fr.data = px;
        fr.data_len = sizeof(px);

        const uint8_t dummy_icc[4] = {'I', 'C', 'C', '!'};
        const uint8_t dummy_exif[8] = {'I', 'I', 0x2A, 0x00, 8, 0, 0, 0};
        const uint8_t dummy_xmp[6] = {'<', 'x', '/', '>', '\n', 0};

        rfc_metadata metas[7];
        memset(metas, 0, sizeof(metas));
        metas[0].icc = dummy_icc; metas[0].icc_len = sizeof(dummy_icc);
        metas[1].color.has_srgb = 1u; metas[1].color.srgb_intent = 0u;
        metas[2].color.has_gama = 1u; metas[2].color.gama_scaled = 45455u;
        metas[3].color.has_cicp = 1u;
        metas[3].color.cicp[0] = 1u; metas[3].color.cicp[1] = 13u;
        metas[3].color.cicp[2] = 0u; metas[3].color.cicp[3] = 1u;
        metas[4].exif = dummy_exif; metas[4].exif_len = sizeof(dummy_exif);
        metas[5].xmp = dummy_xmp; metas[5].xmp_len = sizeof(dummy_xmp);
        metas[6].orientation = RFC_ORIENTATION_ROTATE_90;

        for (size_t i = 0u; i < 7u; i++) {
            rfc_encoded out;
            st.calls = 0u;
            CHECK(adp->encode_session(sess, &exec, &fr, 1u, &metas[i], NULL, 0u,
                                      &out) == RFC_ERR_METADATA_POLICY);
            CHECK(st.calls == 0u);
        }

        /* Standalone encode_session refuses exactness=byte-identical before work. */
        rfc_setting_value bi_sv[1] = {{"exactness", "byte-identical"}};
        rfc_encoded bi_out;
        st.calls = 0u;
        CHECK(adp->encode_session(sess, &exec, &fr, 1u, NULL, bi_sv, 1u,
                                  &bi_out) == RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(st.calls == 0u);

        /* Malformed metadata descriptor (nonzero length with NULL pointer) refused. */
        rfc_metadata bad_desc;
        memset(&bad_desc, 0, sizeof(bad_desc));
        bad_desc.icc = NULL;
        bad_desc.icc_len = 4u;
        rfc_encoded bad_out;
        st.calls = 0u;
        CHECK(adp->encode_session(sess, &exec, &fr, 1u, &bad_desc, NULL, 0u,
                                  &bad_out) == RFC_ERR_INVALID_ARGUMENT);
        CHECK(st.calls == 0u);
    }

    /* 3. Valid control + named malformed QOI fixtures (all zero-alloc refused). */
    {
        /* Valid 2x1 RGB8 QOI control (2 pixels: QOI_OP_RGB + QOI_OP_RUN(1)). */
        const uint8_t valid_ctrl[] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 2, 0, 0, 0, 1, 3, 0,
            0xFE, 10, 20, 30, 0xC0,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        rfc_image_probe pr;
        CHECK(adp->probe_session(sess, &exec, valid_ctrl, sizeof(valid_ctrl),
                                 &pr) == RFC_OK);
        CHECK(pr.first_frame.width == 2u && pr.first_frame.height == 1u);

        /* Truncated header (<4 and <22). */
        assert_malformed_refused(adp, sess, valid_ctrl, 3u, RFC_ERR_TRUNCATED_INPUT);
        assert_malformed_refused(adp, sess, valid_ctrl, 14u, RFC_ERR_TRUNCATED_INPUT);

        /* Bad magic. */
        uint8_t bad_magic[sizeof(valid_ctrl)];
        memcpy(bad_magic, valid_ctrl, sizeof(valid_ctrl));
        bad_magic[3] = 'X';
        assert_malformed_refused(adp, sess, bad_magic, sizeof(bad_magic),
                                 RFC_ERR_INVALID_ARGUMENT);

        /* Zero width / zero height. */
        uint8_t zero_w[sizeof(valid_ctrl)];
        memcpy(zero_w, valid_ctrl, sizeof(valid_ctrl));
        wr32be(zero_w + 4u, 0u);
        assert_malformed_refused(adp, sess, zero_w, sizeof(zero_w),
                                 RFC_ERR_INVALID_ARGUMENT);
        uint8_t zero_h[sizeof(valid_ctrl)];
        memcpy(zero_h, valid_ctrl, sizeof(valid_ctrl));
        wr32be(zero_h + 8u, 0u);
        assert_malformed_refused(adp, sess, zero_h, sizeof(zero_h),
                                 RFC_ERR_INVALID_ARGUMENT);

        /* Invalid channels (2 and 5). */
        uint8_t bad_ch[sizeof(valid_ctrl)];
        memcpy(bad_ch, valid_ctrl, sizeof(valid_ctrl));
        bad_ch[12] = 2u;
        assert_malformed_refused(adp, sess, bad_ch, sizeof(bad_ch),
                                 RFC_ERR_INVALID_ARGUMENT);
        bad_ch[12] = 5u;
        assert_malformed_refused(adp, sess, bad_ch, sizeof(bad_ch),
                                 RFC_ERR_INVALID_ARGUMENT);

        /* Invalid colorspace (2 and 255). */
        uint8_t bad_cs[sizeof(valid_ctrl)];
        memcpy(bad_cs, valid_ctrl, sizeof(valid_ctrl));
        bad_cs[13] = 2u;
        assert_malformed_refused(adp, sess, bad_cs, sizeof(bad_cs),
                                 RFC_ERR_INVALID_ARGUMENT);
        bad_cs[13] = 255u;
        assert_malformed_refused(adp, sess, bad_cs, sizeof(bad_cs),
                                 RFC_ERR_INVALID_ARGUMENT);

        /* Missing / corrupt end marker. */
        uint8_t bad_end[sizeof(valid_ctrl)];
        memcpy(bad_end, valid_ctrl, sizeof(valid_ctrl));
        bad_end[sizeof(bad_end) - 1u] = 0u;
        assert_malformed_refused(adp, sess, bad_end, sizeof(bad_end),
                                 RFC_ERR_TRUNCATED_INPUT);

        /* Truncated QOI_OP_RGB (only 2 payload bytes before end marker). */
        const uint8_t trunc_rgb[] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0,
            0xFE, 10, 20,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        assert_malformed_refused(adp, sess, trunc_rgb, sizeof(trunc_rgb),
                                 RFC_ERR_TRUNCATED_INPUT);

        /* Truncated QOI_OP_RGBA (only 3 payload bytes before end marker). */
        const uint8_t trunc_rgba[] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 4, 0,
            0xFF, 10, 20, 30,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        assert_malformed_refused(adp, sess, trunc_rgba, sizeof(trunc_rgba),
                                 RFC_ERR_TRUNCATED_INPUT);

        /* Truncated QOI_OP_LUMA (0 payload bytes before end marker). */
        const uint8_t trunc_luma[] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0,
            0x80,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        assert_malformed_refused(adp, sess, trunc_luma, sizeof(trunc_luma),
                                 RFC_ERR_TRUNCATED_INPUT);

        /* Truncated chunk stream (only 1 pixel encoded for a 2x1 image). */
        const uint8_t short_stream[] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 2, 0, 0, 0, 1, 3, 0,
            0xC0,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        assert_malformed_refused(adp, sess, short_stream, sizeof(short_stream),
                                 RFC_ERR_TRUNCATED_INPUT);

        /* Excess QOI_OP_RUN (run of 3 pixels for a 2x1 image). */
        const uint8_t excess_run[] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 2, 0, 0, 0, 1, 3, 0,
            0xC2,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        assert_malformed_refused(adp, sess, excess_run, sizeof(excess_run),
                                 RFC_ERR_INVALID_ARGUMENT);

        /* Consecutive identical QOI_OP_INDEX chunks (forbidden by QOI spec). */
        const uint8_t consec_idx[] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 2, 0, 0, 0, 1, 3, 0,
            0x00, 0x00,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        assert_malformed_refused(adp, sess, consec_idx, sizeof(consec_idx),
                                 RFC_ERR_INVALID_ARGUMENT);

        /* Trailing garbage byte before end marker. */
        const uint8_t trailing[] = {
            'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0,
            0xC0, 0x00,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        assert_malformed_refused(adp, sess, trailing, sizeof(trailing),
                                 RFC_ERR_INVALID_ARGUMENT);

        /* Over-limit dimensions (decompression bomb). */
        const uint8_t bomb[] = {
            'q', 'o', 'i', 'f', 0x00, 0x02, 0x00, 0x00,
            0x00, 0x02, 0x00, 0x00, 4, 0,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        assert_malformed_refused(adp, sess, bomb, sizeof(bomb),
                                 RFC_ERR_LIMIT_EXCEEDED);
    }

    adp->close_session(sess);
    CHECK(st.live == 0u);
    printf("qoi: refusals_malformed_and_metadata_policy ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 4: Budget enforcement, cancel hook, layout-free file jobs, safety  */
/* ------------------------------------------------------------------------ */

typedef struct cancel_sess_ctx {
    const rfc_session_adapter *adp;
    void *sess;
    const rfc_budget *bud;
    unsigned skip_calls;
    unsigned calls;
    uint64_t observed_used;
    int fired;
} cancel_sess_ctx;

static void hook_cancel_session(void *ctx) {
    cancel_sess_ctx *c = (cancel_sess_ctx *)ctx;
    if (!c || c->fired) {
        return;
    }
    c->calls++;
    if (c->calls <= c->skip_calls) {
        return;
    }
    if (c->bud) {
        c->observed_used = budget_used(c->bud);
    }
    c->fired = 1;
    c->adp->cancel_session(c->sess);
}

typedef struct cancel_job_ctx {
#ifdef _WIN32
    CRITICAL_SECTION mu;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t mu;
    pthread_cond_t cv;
#endif
    rfc_job *job;
    int fired;
} cancel_job_ctx;

static void cancel_job_ctx_init(cancel_job_ctx *c) {
    memset(c, 0, sizeof(*c));
#ifdef _WIN32
    InitializeCriticalSection(&c->mu);
    InitializeConditionVariable(&c->cv);
#else
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
#endif
}

static void cancel_job_ctx_destroy(cancel_job_ctx *c) {
#ifdef _WIN32
    DeleteCriticalSection(&c->mu);
#else
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
#endif
}

static void cancel_job_ctx_publish(cancel_job_ctx *c, rfc_job *j) {
#ifdef _WIN32
    EnterCriticalSection(&c->mu);
    c->job = j;
    WakeAllConditionVariable(&c->cv);
    LeaveCriticalSection(&c->mu);
#else
    pthread_mutex_lock(&c->mu);
    c->job = j;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
#endif
}

static void hook_cancel_job(void *ctx) {
    cancel_job_ctx *c = (cancel_job_ctx *)ctx;
    if (!c) {
        return;
    }
    rfc_job *j = NULL;
#ifdef _WIN32
    EnterCriticalSection(&c->mu);
    if (c->fired) {
        LeaveCriticalSection(&c->mu);
        return;
    }
    while (!c->job) {
        SleepConditionVariableCS(&c->cv, &c->mu, INFINITE);
    }
    j = c->job;
    LeaveCriticalSection(&c->mu);
    (void)rfc_job_cancel(j);
    EnterCriticalSection(&c->mu);
    c->fired = 1;
    LeaveCriticalSection(&c->mu);
#else
    pthread_mutex_lock(&c->mu);
    if (c->fired) {
        pthread_mutex_unlock(&c->mu);
        return;
    }
    while (!c->job) {
        pthread_cond_wait(&c->cv, &c->mu);
    }
    j = c->job;
    pthread_mutex_unlock(&c->mu);
    (void)rfc_job_cancel(j);
    pthread_mutex_lock(&c->mu);
    c->fired = 1;
    pthread_mutex_unlock(&c->mu);
#endif
}

static rfc_status noop_verify_cb(rfc_job *job, const rfc_exec_context *exec,
                                 void *ctx) {
    (void)job;
    (void)exec;
    (void)ctx;
    return RFC_OK;
}

static rfc_status bi_encode_cb(rfc_job *job, const rfc_exec_context *exec,
                               void *ctx) {
    (void)ctx;
    size_t fc = 0u;
    const rfc_metadata *dm = NULL;
    const rfc_frame *fr = rfc_job_decoded_frames(job, &fc, &dm);
    if (!fr || fc != 1u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const rfc_session_adapter *adp = rfc_registry_session_adapter(RFC_FORMAT_QOI);
    if (!adp) {
        return RFC_ERR_NO_ADAPTER;
    }
    void *s = NULL;
    rfc_status st = adp->open_session(exec, &s);
    if (st != RFC_OK) {
        return st;
    }
    rfc_setting_value sv[1] = {{"exactness", "byte-identical"}};
    rfc_encoded enc;
    st = adp->encode_session(s, exec, fr, fc, dm, sv, 1u, &enc);
    if (st == RFC_OK) {
        rfc_encoded_free(&enc);
    }
    adp->close_session(s);
    return st;
}

static struct {
    int mode;
    uint32_t decode_calls;
    rfc_status frame1_check_st;
    rfc_status frame2_check_st;
    int frame2_alloc_attempted;
} g_qoi_probe_mock;

static rfc_status qoi_mock_open(const rfc_exec_context *exec, void **out_sess) {
    (void)exec;
    *out_sess = &g_qoi_probe_mock;
    return RFC_OK;
}

static void qoi_mock_close(void *session) { (void)session; }

static rfc_status qoi_mock_probe(void *session,
                                 const rfc_exec_context *exec,
                                 const uint8_t *bytes,
                                 size_t size,
                                 rfc_image_probe *out_probe) {
    (void)session;
    (void)exec;
    (void)bytes;
    (void)size;
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.width = 4u;
    out_probe->first_frame.height = 4u;
    out_probe->first_frame.bits_per_sample = 8u;
    if (g_qoi_probe_mock.mode == 0) {
        out_probe->first_frame.format = RFC_SAMPLE_RGB8;
        out_probe->first_frame.alpha = RFC_ALPHA_NONE;
        out_probe->first_frame.stride_bytes = 4u * 3u;
        out_probe->frame_count = 2u;
        out_probe->total_pixels = 4u * 4u * 2u;
        out_probe->total_decoded_bytes = 4u * 4u * 3u * 2u;
    } else if (g_qoi_probe_mock.mode == 1) {
        out_probe->first_frame.format = RFC_SAMPLE_RGBA8;
        out_probe->first_frame.alpha = RFC_ALPHA_PREMULTIPLIED;
        out_probe->first_frame.stride_bytes = 4u * 4u;
        out_probe->frame_count = 1u;
    } else if (g_qoi_probe_mock.mode == 2) {
        out_probe->first_frame.format = RFC_SAMPLE_RGB8;
        out_probe->first_frame.alpha = RFC_ALPHA_NONE;
        out_probe->first_frame.stride_bytes = 4u * 3u;
        out_probe->first_frame.bits_per_sample = 7u;
        out_probe->frame_count = 1u;
    } else {
        out_probe->first_frame.format = RFC_SAMPLE_RGB8;
        out_probe->first_frame.alpha = RFC_ALPHA_NONE;
        out_probe->first_frame.stride_bytes = 4u * 3u;
        out_probe->frame_count = 0u; /* unknown at probe time */
    }
    return RFC_OK;
}

static rfc_status qoi_mock_decode(void *session,
                                  const rfc_exec_context *exec,
                                  const uint8_t *bytes,
                                  size_t size,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_frame **out_frames,
                                  size_t *out_frame_count,
                                  rfc_metadata *out_meta) {
    (void)session;
    (void)bytes;
    (void)size;
    (void)settings;
    (void)setting_count;
    (void)out_frames;
    (void)out_frame_count;
    (void)out_meta;
    g_qoi_probe_mock.decode_calls++;
    if (g_qoi_probe_mock.mode == 3) {
        rfc_layout l0 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_status s1 = rfc_exec_check_next_frame(exec, &l0, NULL);
        g_qoi_probe_mock.frame1_check_st = s1;
        if (s1 != RFC_OK) {
            return s1;
        }
        rfc_frame f0;
        memset(&f0, 0, sizeof(f0));
        s1 = rfc_exec_alloc_frame(exec, &f0, &l0, NULL);
        if (s1 != RFC_OK) {
            return s1;
        }
        rfc_status s2 = rfc_exec_check_next_frame(exec, &l0, NULL);
        g_qoi_probe_mock.frame2_check_st = s2;
        if (s2 == RFC_OK) {
            g_qoi_probe_mock.frame2_alloc_attempted = 1;
        }
        rfc_frame_free(&f0);
        return (s2 != RFC_OK) ? s2 : RFC_ERR_TRUNCATED_INPUT;
    }
    return RFC_ERR_TRUNCATED_INPUT;
}

static rfc_status qoi_mock_encode(void *session,
                                  const rfc_exec_context *exec,
                                  const rfc_frame *frames,
                                  size_t frame_count,
                                  const rfc_metadata *meta,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_encoded *out_encoded) {
    (void)session;
    (void)exec;
    (void)frames;
    (void)frame_count;
    (void)meta;
    (void)settings;
    (void)setting_count;
    (void)out_encoded;
    return RFC_ERR_UNSUPPORTED_COMBINATION;
}

static void qoi_mock_cancel(void *session) {
    (void)session;
}

static void test_jobs_budget_cancel_and_file_safety(void) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    const rfc_session_adapter *adp = rfc_registry_session_adapter(RFC_FORMAT_QOI);
    CHECK(adp != NULL);

    /* 1. Insufficient budget refused on both encode and decode, reservation released. */
    {
        tracked_alloc st;
        rfc_allocator al;
        rfc_budget *tight = rfc_budget_create(64u);
        CHECK(tight != NULL);
        rfc_exec_context exec;
        init_exec(&exec, &st, &al, tight);

        void *sess = NULL;
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);

        rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 32u, 32u, 128u, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        fill_pattern_rgba8(src.data, 32u, 32u, 128u);

        rfc_encoded enc;
        CHECK(adp->encode_session(sess, &exec, &src, 1u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_OUT_OF_MEMORY);
        CHECK(budget_used(tight) == 0u);

        /* Encode with ample budget so we have valid QOI bytes for decode test. */
        rfc_budget *ample = rfc_budget_create(1024ull * 1024ull);
        CHECK(ample != NULL);
        exec.budget = ample;
        CHECK(adp->encode_session(sess, &exec, &src, 1u, NULL, NULL, 0u, &enc) ==
              RFC_OK);
        CHECK(budget_used(ample) == 0u);

        /* Decode with tight budget -> refused with RFC_ERR_OUT_OF_MEMORY, used==0. */
        exec.budget = tight;
        rfc_frame *dec = NULL;
        size_t dec_n = 0u;
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u,
                                  &dec, &dec_n, NULL) == RFC_ERR_OUT_OF_MEMORY);
        CHECK(dec == NULL && dec_n == 0u);
        CHECK(budget_used(tight) == 0u);

        /* Mid-decode cancellation AFTER qoi_reserve_budget + qoi_decode (skip_calls=1):
         * verify the decode reservation is live when the hook fires and released on cancel. */
        exec.budget = ample;
        cancel_sess_ctx cctx = {adp, sess, ample, 1u, 0u, 0u, 0};
        rfc_codecs_qoi_set_hook_for_testing(hook_cancel_session, &cctx);
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u,
                                  &dec, &dec_n, NULL) == RFC_ERR_CANCELLED);
        rfc_codecs_qoi_set_hook_for_testing(NULL, NULL);
        CHECK(cctx.fired == 1 && cctx.observed_used >= 32u * 32u * 4u &&
              dec == NULL && budget_used(ample) == 0u);

        void *sess2 = NULL;
        CHECK(adp->open_session(&exec, &sess2) == RFC_OK);
        cancel_sess_ctx cctx2 = {adp, sess2, ample, 0u, 0u, 0u, 0};
        rfc_codecs_qoi_set_hook_for_testing(hook_cancel_session, &cctx2);
        rfc_encoded enc2;
        CHECK(adp->encode_session(sess2, &exec, &src, 1u, NULL, NULL, 0u,
                                  &enc2) == RFC_ERR_CANCELLED);
        rfc_codecs_qoi_set_hook_for_testing(NULL, NULL);
        CHECK(cctx2.fired == 1 && cctx2.observed_used > 0u &&
              enc2.data == NULL && budget_used(ample) == 0u);
        adp->close_session(sess2);

        /* Padded encode budget-release boundary: release packing reservation after
         * free(packed) so a budget-backed final output allocation fits without
         * double-counting packed_bytes alongside final_len. */
        {
            const uint32_t pw = 32u, ph = 32u;
            const size_t packed_b = (size_t)pw * (size_t)ph * 4u;
            const size_t pad_stride = (size_t)pw * 4u + 16u;
            const size_t worst_qoi = (size_t)pw * (size_t)ph * 5u + 22u;
            rfc_layout play = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, pw, ph,
                               pad_stride, 8u};
            rfc_frame psrc;
            CHECK(rfc_frame_alloc(&psrc, &play, NULL, &al) == RFC_OK);
            fill_pattern_rgba8(psrc.data, pw, ph, pad_stride);

            /* Limit = worst_qoi + max(packed_b, enc.len) (strictly less than
             * worst_qoi + packed_b + enc.len, so keeping pack_guard would OOM). */
            size_t peak_needed =
                worst_qoi + (packed_b > enc.len ? packed_b : enc.len);
            CHECK(peak_needed < worst_qoi + packed_b + enc.len);
            rfc_budget *bbound = rfc_budget_create((uint64_t)peak_needed);
            CHECK(bbound != NULL);
            rfc_allocator bal;
            rfc_budget_allocator(bbound, &bal);
            rfc_exec_context bexec;
            memset(&bexec, 0, sizeof(bexec));
            rfc_limits_default(&bexec.limits);
            bexec.codec_threads = 1u;
            bexec.allocator = &bal;
            bexec.budget = bbound;

            void *bsess = NULL;
            CHECK(adp->open_session(&bexec, &bsess) == RFC_OK);
            rfc_encoded penc;
            CHECK(adp->encode_session(bsess, &bexec, &psrc, 1u, NULL, NULL, 0u,
                                      &penc) == RFC_OK);
            CHECK(penc.len == enc.len);
            rfc_encoded_free(&penc);
            CHECK(budget_used(bbound) == 0u);
            adp->close_session(bsess);
            rfc_budget_destroy(bbound);
            rfc_frame_free(&psrc);
        }

        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
        adp->close_session(sess);
        rfc_budget_destroy(ample);
        rfc_budget_destroy(tight);
        CHECK(st.live == 0u);
    }

    /* 2. Layout-free, estimate-free scheduler file jobs:
     *    QOI (sRGB + linear) -> PNG -> QOI, same-format byte-identical,
     *    metadata policy outcomes (preserve refusal, acknowledged loss, strip),
     *    active job cancellation, and occupied-destination rollback. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        /* Prepare an sRGB RGBA8 QOI file and a linear RGBA8 QOI file. */
        tracked_alloc st;
        rfc_allocator al;
        rfc_exec_context exec;
        init_exec(&exec, &st, &al, NULL);
        void *sess = NULL;
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);

        rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 17u, 13u, 68u, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        fill_pattern_rgba8(src.data, 17u, 13u, 68u);

        rfc_encoded qoi_srgb, qoi_lin;
        CHECK(adp->encode_session(sess, &exec, &src, 1u, NULL, NULL, 0u,
                                  &qoi_srgb) == RFC_OK);
        rfc_metadata lin_m;
        memset(&lin_m, 0, sizeof(lin_m));
        lin_m.color.has_cicp = 1u;
        lin_m.color.cicp[0] = 2u; lin_m.color.cicp[1] = 8u;
        lin_m.color.cicp[2] = 0u; lin_m.color.cicp[3] = 1u;
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &lin_m, NULL, 0u,
                                  &qoi_lin) == RFC_OK);
        CHECK(qoi_lin.data[13] == QOI_LINEAR);

        char p_qoi[256], p_png[256], p_qoi2[256], p_bi[256], p_lin_qoi[256],
             p_lin_png[256], p_lin_qoi2[256], p_icc_png[256], p_ack_qoi[256],
             p_strip_qoi[256], p_cancel[256], p_occ[256];
        temp_path(p_qoi, sizeof(p_qoi), "src.qoi");
        temp_path(p_png, sizeof(p_png), "mid.png");
        temp_path(p_qoi2, sizeof(p_qoi2), "back.qoi");
        temp_path(p_bi, sizeof(p_bi), "bi.qoi");
        temp_path(p_lin_qoi, sizeof(p_lin_qoi), "lin.qoi");
        temp_path(p_lin_png, sizeof(p_lin_png), "lin.png");
        temp_path(p_lin_qoi2, sizeof(p_lin_qoi2), "lin_back.qoi");
        temp_path(p_icc_png, sizeof(p_icc_png), "icc.png");
        temp_path(p_ack_qoi, sizeof(p_ack_qoi), "ack.qoi");
        temp_path(p_strip_qoi, sizeof(p_strip_qoi), "strip.qoi");
        temp_path(p_cancel, sizeof(p_cancel), "cancel.png");
        temp_path(p_occ, sizeof(p_occ), "occupied.png");

        CHECK(write_file_all(p_qoi, qoi_srgb.data, qoi_srgb.len));
        CHECK(write_file_all(p_lin_qoi, qoi_lin.data, qoi_lin.len));

        /* A. Layout-free, estimate-free QOI -> PNG -> QOI (sRGB). */
        rfc_io_source si;
        rfc_io_sink so;
        rfc_job_spec sp;
        rfc_job_snapshot snap;
        rfc_verify_report vrep;
        rfc_job *job = NULL;

        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_qoi, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_png, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_QOI;
        sp.target_format = RFC_FORMAT_PNG;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(job);

        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_png, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_qoi2, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_PNG;
        sp.target_format = RFC_FORMAT_QOI;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(job);

        /* Reopen QOI output and compare every pixel byte (including A==0 RGB). */
        size_t q2_len = 0u;
        uint8_t *q2_bytes = read_file_all(p_qoi2, &q2_len);
        CHECK(q2_bytes != NULL && q2_len >= 22u && q2_bytes[13] == QOI_SRGB);
        rfc_frame *q2_dec = NULL;
        size_t q2_n = 0u;
        CHECK(adp->decode_session(sess, &exec, q2_bytes, q2_len, NULL, 0u,
                                  &q2_dec, &q2_n, NULL) == RFC_OK);
        CHECK(q2_n == 1u && q2_dec[0].data_len == src.data_len &&
              memcmp(q2_dec[0].data, src.data, src.data_len) == 0);
        rfc_frame_free(&q2_dec[0]);
        al.free(al.ctx, q2_dec);
        free(q2_bytes);

        /* B. Linear QOI -> PNG -> QOI preserves colorspace=1 through PNG cICP. */
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_lin_qoi, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_lin_png, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_QOI;
        sp.target_format = RFC_FORMAT_PNG;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(job);

        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_lin_png, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_lin_qoi2, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_PNG;
        sp.target_format = RFC_FORMAT_QOI;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(job);
        size_t lq2_len = 0u;
        uint8_t *lq2 = read_file_all(p_lin_qoi2, &lq2_len);
        CHECK(lq2 != NULL && lq2_len >= 22u && lq2[13] == QOI_LINEAR);
        free(lq2);

        /* C. Same-format QOI -> QOI byte-identical copy + verification. */
        rfc_setting_value bi_sv[1] = {{"exactness", "byte-identical"}};
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_qoi, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_bi, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_QOI;
        sp.target_format = RFC_FORMAT_QOI;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.settings = bi_sv;
        sp.setting_count = 1u;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(job);
        size_t bi_len = 0u;
        uint8_t *bi_bytes = read_file_all(p_bi, &bi_len);
        CHECK(bi_bytes != NULL && bi_len == qoi_srgb.len &&
              memcmp(bi_bytes, qoi_srgb.data, bi_len) == 0);
        free(bi_bytes);

        /* C2. Compressible 4096x4096 RGBA8 solid QOI (~265 KiB compressed, 64 MiB
         * decoded) succeeds through layout-free byte-identical file job under
         * default scheduler sizing. */
        {
            const uint32_t bw = 4096u, bh = 4096u;
            const uint64_t total_px = (uint64_t)bw * (uint64_t)bh;
            const size_t full_runs = (size_t)(total_px / 62u);
            const size_t rem_px = (size_t)(total_px % 62u);
            const size_t big_len = 14u + full_runs + (rem_px ? 1u : 0u) + 8u;
            uint8_t *big_qoi = (uint8_t *)malloc(big_len);
            CHECK(big_qoi != NULL);
            memcpy(big_qoi, "qoif", 4u);
            wr32be(big_qoi + 4u, bw);
            wr32be(big_qoi + 8u, bh);
            big_qoi[12] = 4u;
            big_qoi[13] = QOI_SRGB;
            memset(big_qoi + 14u, 0xC0 | 61u, full_runs);
            size_t tail_off = 14u + full_runs;
            if (rem_px) {
                big_qoi[tail_off++] = (uint8_t)(0xC0u | (rem_px - 1u));
            }
            const uint8_t endm[8] = {0, 0, 0, 0, 0, 0, 0, 1};
            memcpy(big_qoi + tail_off, endm, 8u);

            char p_big_in[256], p_big_out[256];
            temp_path(p_big_in, sizeof(p_big_in), "big4k.qoi");
            temp_path(p_big_out, sizeof(p_big_out), "big4k_bi.qoi");
            CHECK(write_file_all(p_big_in, big_qoi, big_len));

            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_big_in, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_big_out, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_QOI;
            sp.target_format = RFC_FORMAT_QOI;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.settings = bi_sv;
            sp.setting_count = 1u;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
            rfc_job_release(job);

            size_t big_out_len = 0u;
            uint8_t *big_out = read_file_all(p_big_out, &big_out_len);
            CHECK(big_out != NULL && big_out_len == big_len &&
                  memcmp(big_out, big_qoi, big_len) == 0);
            free(big_out);
            free(big_qoi);
            remove(p_big_in);
            remove(p_big_out);
        }

        /* D. Tagged PNG (with ICC profile) -> QOI: preserve refusal, acknowledged loss, explicit strip. */
        const rfc_session_adapter *png_adp =
            rfc_registry_session_adapter(RFC_FORMAT_PNG);
        CHECK(png_adp != NULL);
        void *psess = NULL;
        CHECK(png_adp->open_session(&exec, &psess) == RFC_OK);
        uint8_t icc_blob[132];
        memset(icc_blob, 0, sizeof(icc_blob));
        wr32be(icc_blob, (uint32_t)sizeof(icc_blob));
        icc_blob[8] = 2u; icc_blob[9] = 0x10u;
        memcpy(icc_blob + 12u, "mntrRGB XYZ ", 12u);
        memcpy(icc_blob + 36u, "acsp", 4u);
        rfc_metadata icc_m;
        memset(&icc_m, 0, sizeof(icc_m));
        icc_m.icc = icc_blob;
        icc_m.icc_len = sizeof(icc_blob);
        rfc_encoded icc_png;
        CHECK(png_adp->encode_session(psess, &exec, &src, 1u, &icc_m, NULL, 0u,
                                      &icc_png) == RFC_OK);
        png_adp->close_session(psess);
        CHECK(write_file_all(p_icc_png, icc_png.data, icc_png.len));
        rfc_encoded_free(&icc_png);

        /* D1: Default preserve refuses with RFC_ERR_METADATA_POLICY BEFORE pixel
         * allocation (verified via a 256x256 RGBA8 = 262144 B ICC-tagged PNG
         * whose job budget peak stays far below one decoded pixel buffer). */
        {
            rfc_layout big_lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 256u,
                                  256u, 256u * 4u, 8u};
            rfc_frame big_src;
            CHECK(rfc_frame_alloc(&big_src, &big_lay, NULL, &al) == RFC_OK);
            fill_pattern_rgba8(big_src.data, 256u, 256u, 256u * 4u);
            void *psess2 = NULL;
            CHECK(png_adp->open_session(&exec, &psess2) == RFC_OK);
            rfc_encoded big_icc_png;
            CHECK(png_adp->encode_session(psess2, &exec, &big_src, 1u, &icc_m,
                                          NULL, 0u, &big_icc_png) == RFC_OK);
            png_adp->close_session(psess2);
            char p_big_icc[256];
            temp_path(p_big_icc, sizeof(p_big_icc), "big_icc.png");
            CHECK(write_file_all(p_big_icc, big_icc_png.data, big_icc_png.len));
            rfc_encoded_free(&big_icc_png);
            rfc_frame_free(&big_src);

            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_big_icc, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_ack_qoi, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_PNG;
            sp.target_format = RFC_FORMAT_QOI;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_METADATA_POLICY &&
                  snap.budget.peak_allocated_bytes < 256u * 256u * 4u &&
                  !file_exists(p_ack_qoi));
            rfc_job_release(job);

            /* Same ICC-tagged PNG -> QOI job with custom on_verify (on_encode == NULL)
             * also refuses before pixel allocation. */
            sp.has_stage_ops = 1;
            sp.stage_ops.on_verify = noop_verify_cb;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_METADATA_POLICY &&
                  snap.budget.peak_allocated_bytes < 256u * 256u * 4u &&
                  !file_exists(p_ack_qoi));
            rfc_job_release(job);
            remove(p_big_icc);

            /* Calling qoi_encode_session with exactness=byte-identical inside a
             * live job on_encode callback (non-NULL exec->job) is refused. */
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_qoi, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_ack_qoi, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_QOI;
            sp.target_format = RFC_FORMAT_QOI;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.has_stage_ops = 1;
            sp.stage_ops.on_encode = bi_encode_cb;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_UNSUPPORTED_COMBINATION &&
                  !file_exists(p_ack_qoi));
            rfc_job_release(job);

            /* Untagged 16-bit RGB16 PNG (256x256x6 = 393216 B decoded) submitted
             * to a built-in QOI file job under preserve mode is refused with
             * RFC_ERR_UNSUPPORTED_SAMPLE BEFORE pixel allocation. */
            rfc_layout lay16 = {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 256u, 256u,
                                256u * 6u, 16u};
            rfc_frame src16;
            CHECK(rfc_frame_alloc(&src16, &lay16, NULL, &al) == RFC_OK);
            memset(src16.data, 0x3Cu, src16.data_len);
            void *psess16 = NULL;
            CHECK(png_adp->open_session(&exec, &psess16) == RFC_OK);
            rfc_encoded png16;
            CHECK(png_adp->encode_session(psess16, &exec, &src16, 1u, NULL,
                                          NULL, 0u, &png16) == RFC_OK);
            png_adp->close_session(psess16);
            char p_rgb16[256];
            temp_path(p_rgb16, sizeof(p_rgb16), "rgb16.png");
            CHECK(write_file_all(p_rgb16, png16.data, png16.len));
            rfc_encoded_free(&png16);
            rfc_frame_free(&src16);

            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_rgb16, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_ack_qoi, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_PNG;
            sp.target_format = RFC_FORMAT_QOI;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_UNSUPPORTED_SAMPLE &&
                  snap.budget.peak_allocated_bytes < 256u * 256u * 6u &&
                  !file_exists(p_ack_qoi));
            rfc_job_release(job);
            remove(p_rgb16);

            /* Multi-frame (2 RGB8 frames, empty metadata) and premultiplied-alpha
             * (RGBA8, empty metadata) sources submitted to a built-in QOI file
             * job under preserve mode are refused BEFORE decode_session is called. */
            rfc_session_adapter msess;
            memset(&msess, 0, sizeof(msess));
            msess.abi_version = RFC_CODEC_ABI_VERSION;
            msess.format = RFC_FORMAT_JPEG_XL;
            msess.open_session = qoi_mock_open;
            msess.close_session = qoi_mock_close;
            msess.probe_session = qoi_mock_probe;
            msess.decode_session = qoi_mock_decode;
            msess.encode_session = qoi_mock_encode;
            msess.cancel_session = qoi_mock_cancel;
            CHECK(rfc_registry_register_session(&msess) == RFC_OK);

            char p_mock_src[256];
            temp_path(p_mock_src, sizeof(p_mock_src), "mock_mf.jxl");
            static const uint8_t mock_bytes[16] = {
                0xFFu, 0x0Au, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
                0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
            CHECK(write_file_all(p_mock_src, mock_bytes, sizeof(mock_bytes)));

            g_qoi_probe_mock.mode = 0;
            g_qoi_probe_mock.decode_calls = 0u;
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_mock_src, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_ack_qoi, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG_XL;
            sp.target_format = RFC_FORMAT_QOI;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.expected_working_set_bytes = 64u * 1024u;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_UNSUPPORTED_COMBINATION &&
                  g_qoi_probe_mock.decode_calls == 0u &&
                  !file_exists(p_ack_qoi));
            rfc_job_release(job);

            g_qoi_probe_mock.mode = 1;
            g_qoi_probe_mock.decode_calls = 0u;
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_mock_src, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_ack_qoi, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG_XL;
            sp.target_format = RFC_FORMAT_QOI;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.expected_working_set_bytes = 64u * 1024u;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_UNSUPPORTED_ALPHA &&
                  g_qoi_probe_mock.decode_calls == 0u &&
                  !file_exists(p_ack_qoi));
            rfc_job_release(job);

            g_qoi_probe_mock.mode = 2;
            g_qoi_probe_mock.decode_calls = 0u;
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_mock_src, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_ack_qoi, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG_XL;
            sp.target_format = RFC_FORMAT_QOI;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.expected_working_set_bytes = 64u * 1024u;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_UNSUPPORTED_SAMPLE &&
                  g_qoi_probe_mock.decode_calls == 0u &&
                  !file_exists(p_ack_qoi));
            rfc_job_release(job);

            /* Unknown probe frame_count == 0 with incremental frame discovery:
             * frame 1 succeeds, frame 2 is refused with
             * RFC_ERR_UNSUPPORTED_COMBINATION before allocation credit. */
            g_qoi_probe_mock.mode = 3;
            g_qoi_probe_mock.decode_calls = 0u;
            g_qoi_probe_mock.frame1_check_st = RFC_ERR_TRUNCATED_INPUT;
            g_qoi_probe_mock.frame2_check_st = RFC_OK;
            g_qoi_probe_mock.frame2_alloc_attempted = 0;
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_mock_src, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_ack_qoi, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG_XL;
            sp.target_format = RFC_FORMAT_QOI;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.expected_working_set_bytes = 64u * 1024u;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(g_qoi_probe_mock.decode_calls == 1u &&
                  g_qoi_probe_mock.frame1_check_st == RFC_OK &&
                  g_qoi_probe_mock.frame2_check_st ==
                      RFC_ERR_UNSUPPORTED_COMBINATION &&
                  g_qoi_probe_mock.frame2_alloc_attempted == 0 &&
                  snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_UNSUPPORTED_COMBINATION &&
                  !file_exists(p_ack_qoi));
            rfc_job_release(job);

            remove(p_mock_src);
            CHECK(rfc_registry_unregister_session(RFC_FORMAT_JPEG_XL) == RFC_OK);
        }

        /* D2: Acknowledged unsupported-item loss succeeds and reports UNSUPPORTED_ITEMS_NAMED. */
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_icc_png, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_ack_qoi, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_PNG;
        sp.target_format = RFC_FORMAT_QOI;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        sp.acknowledged_unsupported_metadata = RFC_META_ITEM_ICC;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        CHECK(rfc_job_verify_report_get(job, &vrep) == RFC_OK);
        CHECK(vrep.metadata.outcome ==
              RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED);
        CHECK((vrep.metadata.unsupported_items &
               RFC_META_ITEM_ICC) != 0u);
        rfc_job_release(job);
        CHECK(file_exists(p_ack_qoi));

        /* D3: Explicit metadata=strip succeeds and reports STRIPPED_EXPLICITLY. */
        rfc_setting_value strip_sv[1] = {{"metadata", "strip"}};
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_icc_png, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_strip_qoi, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_PNG;
        sp.target_format = RFC_FORMAT_QOI;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.settings = strip_sv;
        sp.setting_count = 1u;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        CHECK(rfc_job_verify_report_get(job, &vrep) == RFC_OK);
        CHECK(vrep.metadata.outcome ==
              RFC_VERIFY_META_STRIPPED_EXPLICITLY);
        rfc_job_release(job);
        size_t sq_len = 0u;
        uint8_t *sq = read_file_all(p_strip_qoi, &sq_len);
        CHECK(sq != NULL && sq_len >= 22u && sq[13] == QOI_SRGB);
        free(sq);

        /* E. Occupied destination rollback: destination and source bytes intact, no .partial. */
        const uint8_t sentinel[] = "DO_NOT_OVERWRITE_EXISTING_DESTINATION";
        CHECK(write_file_all(p_occ, sentinel, sizeof(sentinel)));
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_qoi, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_occ, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_QOI;
        sp.target_format = RFC_FORMAT_PNG;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        rfc_status sub_st = rfc_scheduler_submit(sched, NULL, &sp, &job);
        if (sub_st == RFC_OK) {
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_DESTINATION_EXISTS);
            rfc_job_release(job);
        } else {
            CHECK(sub_st == RFC_ERR_DESTINATION_EXISTS);
        }
        size_t occ_len = 0u;
        uint8_t *occ_after = read_file_all(p_occ, &occ_len);
        CHECK(occ_after != NULL && occ_len == sizeof(sentinel) &&
              memcmp(occ_after, sentinel, sizeof(sentinel)) == 0);
        free(occ_after);
        CHECK(!dir_has_partial_files(g_test_dir));

        /* F. Active job cancellation via synchronized hook: source intact,
         * no destination, and no .partial / .rfc-partial staging files. */
        cancel_job_ctx cj;
        cancel_job_ctx_init(&cj);
        rfc_codecs_qoi_set_hook_for_testing(hook_cancel_job, &cj);
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_qoi, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_cancel, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_QOI;
        sp.target_format = RFC_FORMAT_PNG;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        cancel_job_ctx_publish(&cj, job);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        rfc_codecs_qoi_set_hook_for_testing(NULL, NULL);
        CHECK(cj.fired == 1);
        cancel_job_ctx_destroy(&cj);
        CHECK(snap.state == RFC_JOB_CANCELLED &&
              snap.status == RFC_ERR_CANCELLED);
        rfc_job_release(job);
        CHECK(!file_exists(p_cancel) && !dir_has_partial_files(g_test_dir));
        size_t src_after_len = 0u;
        uint8_t *src_after = read_file_all(p_qoi, &src_after_len);
        CHECK(src_after != NULL && src_after_len == qoi_srgb.len &&
              memcmp(src_after, qoi_srgb.data, src_after_len) == 0);
        free(src_after);

        remove(p_qoi); remove(p_png); remove(p_qoi2); remove(p_bi);
        remove(p_lin_qoi); remove(p_lin_png); remove(p_lin_qoi2);
        remove(p_icc_png); remove(p_ack_qoi); remove(p_strip_qoi);
        remove(p_cancel); remove(p_occ);
        remove_private_test_dir();

        rfc_encoded_free(&qoi_srgb);
        rfc_encoded_free(&qoi_lin);
        rfc_frame_free(&src);
        adp->close_session(sess);
        rfc_scheduler_destroy(sched);
        CHECK(st.live == 0u);
    }

    CHECK(rfc_codecs_unregister_basic() == RFC_OK);
    printf("qoi: jobs_budget_cancel_and_file_safety ok\n");
}

/* ------------------------------------------------------------------------ */
/* CLI harness for tests/converter/test_qoi.py                              */
/* ------------------------------------------------------------------------ */

static int cli_encode(const char *in_path, const char *out_path,
                      const char *meta_mode) {
    CHECK(rfc_codecs_register_qoi() == RFC_OK);
    size_t blen = 0u;
    uint8_t *blob = read_file_all(in_path, &blen);
    if (!blob || blen < 36u || memcmp(blob, "RFQE", 4u) != 0) {
        free(blob);
        fprintf(stderr, "invalid RFQE input\n");
        return 2;
    }
    uint32_t w = rd32le(blob + 4u);
    uint32_t h = rd32le(blob + 8u);
    uint32_t sf = rd32le(blob + 12u);
    uint32_t am = rd32le(blob + 16u);
    uint32_t bps = rd32le(blob + 20u);
    uint32_t stride = rd32le(blob + 24u);
    uint32_t cs = rd32le(blob + 28u);
    uint32_t pay_len = rd32le(blob + 32u);
    if (36u + (size_t)pay_len > blen) {
        free(blob);
        return 2;
    }

    tracked_alloc st;
    rfc_allocator al;
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, NULL);
    const rfc_session_adapter *adp = rfc_registry_session_adapter(RFC_FORMAT_QOI);
    void *sess = NULL;
    rfc_status r = adp->open_session(&exec, &sess);
    if (r != RFC_OK) {
        free(blob);
        printf("status=%s\n", rfc_status_name(r));
        return 1;
    }

    rfc_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.layout.width = w;
    fr.layout.height = h;
    fr.layout.format = (rfc_sample_format)sf;
    fr.layout.alpha = (rfc_alpha_mode)am;
    fr.layout.bits_per_sample = bps;
    fr.layout.stride_bytes = stride;
    fr.data = blob + 36u;
    fr.data_len = pay_len;

    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    if (cs == 1u) {
        meta.color.has_cicp = 1u;
        meta.color.cicp[0] = 2u; meta.color.cicp[1] = 8u;
        meta.color.cicp[2] = 0u; meta.color.cicp[3] = 1u;
    } else if (cs == 2u) {
        meta.color.has_srgb = 1u;
        meta.color.srgb_intent = 0u;
    }

    rfc_setting_value sv[1];
    size_t sv_n = 0u;
    if (meta_mode && meta_mode[0]) {
        sv[0].id = "metadata";
        sv[0].value = meta_mode;
        sv_n = 1u;
    }

    rfc_encoded out;
    r = adp->encode_session(sess, &exec, &fr, 1u, &meta, sv, sv_n, &out);
    if (r == RFC_OK) {
        int ok = write_file_all(out_path, out.data, out.len);
        printf("status=ok bytes=%zu colorspace=%u\n", out.len,
               out.len >= 14u ? (unsigned)out.data[13] : 255u);
        rfc_encoded_free(&out);
        adp->close_session(sess);
        free(blob);
        (void)rfc_codecs_unregister_qoi();
        return ok ? 0 : 2;
    }
    printf("status=%s\n", rfc_status_name(r));
    adp->close_session(sess);
    free(blob);
    (void)rfc_codecs_unregister_qoi();
    return 1;
}

static int cli_decode(const char *in_path, const char *out_path,
                      const char *meta_mode) {
    CHECK(rfc_codecs_register_qoi() == RFC_OK);
    size_t blen = 0u;
    uint8_t *blob = read_file_all(in_path, &blen);
    if (!blob) {
        fprintf(stderr, "cannot read %s\n", in_path);
        return 2;
    }

    tracked_alloc st;
    rfc_allocator al;
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, NULL);
    const rfc_session_adapter *adp = rfc_registry_session_adapter(RFC_FORMAT_QOI);
    void *sess = NULL;
    rfc_status r = adp->open_session(&exec, &sess);
    if (r != RFC_OK) {
        free(blob);
        printf("status=%s\n", rfc_status_name(r));
        return 1;
    }

    rfc_setting_value sv[1];
    size_t sv_n = 0u;
    if (meta_mode && meta_mode[0]) {
        sv[0].id = "metadata";
        sv[0].value = meta_mode;
        sv_n = 1u;
    }
    rfc_frame *frames = NULL;
    size_t fn = 0u;
    rfc_metadata meta;
    r = adp->decode_session(sess, &exec, blob, blen, sv, sv_n, &frames, &fn,
                            &meta);
    if (r == RFC_OK && fn == 1u) {
        uint32_t cs = (meta.color.has_cicp && meta.color.cicp[0] == 2u &&
                       meta.color.cicp[1] == 8u && meta.color.cicp[2] == 0u &&
                       meta.color.cicp[3] == 1u)
                          ? 1u
                          : 0u;
        size_t total = 36u + frames[0].data_len;
        uint8_t *dump = (uint8_t *)malloc(total);
        CHECK(dump != NULL);
        memcpy(dump, "RFQD", 4u);
        wr32le(dump + 4u, frames[0].layout.width);
        wr32le(dump + 8u, frames[0].layout.height);
        wr32le(dump + 12u, (uint32_t)frames[0].layout.format);
        wr32le(dump + 16u, (uint32_t)frames[0].layout.alpha);
        wr32le(dump + 20u, frames[0].layout.bits_per_sample);
        wr32le(dump + 24u, (uint32_t)frames[0].layout.stride_bytes);
        wr32le(dump + 28u, cs);
        wr32le(dump + 32u, (uint32_t)frames[0].data_len);
        memcpy(dump + 36u, frames[0].data, frames[0].data_len);
        int ok = write_file_all(out_path, dump, total);
        free(dump);
        printf("status=ok width=%u height=%u sample=%s colorspace=%u bytes=%zu\n",
               frames[0].layout.width, frames[0].layout.height,
               rfc_sample_name(frames[0].layout.format), cs,
               frames[0].data_len);
        rfc_frame_free(&frames[0]);
        al.free(al.ctx, frames);
        adp->close_session(sess);
        free(blob);
        (void)rfc_codecs_unregister_qoi();
        return ok ? 0 : 2;
    }
    printf("status=%s alloc_calls=%zu\n", rfc_status_name(r), st.calls);
    adp->close_session(sess);
    free(blob);
    (void)rfc_codecs_unregister_qoi();
    return 1;
}

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "encode") == 0) {
        return cli_encode(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
    }
    if (argc >= 4 && strcmp(argv[1], "decode") == 0) {
        return cli_decode(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
    }

    test_registration_and_geometry();
    test_rgb_rgba_stride_and_colorspace();
    test_refusals_malformed_and_metadata_policy();
    test_jobs_budget_cancel_and_file_safety();

    CHECK(rfc_codecs_unregister_qoi() == RFC_OK);
    printf("qoi: all checks passed (%d checks)\n", g_checks);
    return 0;
}
