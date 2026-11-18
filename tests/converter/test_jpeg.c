/* C11 codec adapter test suite + CLI harness for D10 T02 s5:
 *   RFC_FORMAT_JPEG (libjpeg-turbo 3.2.1): pixel import under pinned decoder
 *   settings, ICC/EXIF/XMP/orientation, lossy-write refusal, coefficient-exact
 *   entropy optimization (selectors, progressive, arithmetic, markers,
 *   not-smaller fallback), the preserve/strip marker contract, effective
 *   default exactness, JPEG XL original-byte delegation, refusals, provider
 *   error boundary, limits, budget and cancellation. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#else
#include <process.h>
#endif

#include "rfc_codec.h"
#include "rfc_codecs.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

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
/* Helpers: allocator, exec, files, temp paths                               */
/* ------------------------------------------------------------------------ */

typedef struct tracked_alloc_state {
    size_t live;
} tracked_alloc_state;

static void *t_alloc(void *ctx, size_t sz) {
    tracked_alloc_state *st = (tracked_alloc_state *)ctx;
    void *p = malloc(sz ? sz : 1u);
    if (p && st) {
        st->live++;
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

static int g_fail_after = -1;

static void *fail_alloc(void *ctx, size_t sz) {
    if (g_fail_after == 0) {
        return NULL;
    }
    if (g_fail_after > 0) {
        g_fail_after--;
    }
    return t_alloc(ctx, sz);
}

static void init_exec(rfc_exec_context *exec, tracked_alloc_state *st,
                      rfc_allocator *alloc) {
    memset(exec, 0, sizeof(*exec));
    memset(st, 0, sizeof(*st));
    alloc->alloc = t_alloc;
    alloc->free = t_free;
    alloc->ctx = st;
    rfc_limits_default(&exec->limits);
    exec->codec_threads = 1u;
    exec->allocator = alloc;
}

static uint8_t *read_file_all(const char *path, size_t *out_len) {
    *out_len = 0u;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) {
        fclose(fp);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1u);
    if (!buf || (n > 0 && fread(buf, 1u, (size_t)n, fp) != (size_t)n)) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_len = (size_t)n;
    return buf;
}

static int write_file_all(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    int ok = (len == 0u || fwrite(data, 1u, len, fp) == len);
    ok = (fclose(fp) == 0) && ok;
    return ok;
}

static int file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
}

static void make_temp_path(char *buf, size_t cap, const char *tag) {
    static unsigned counter = 0u;
    counter++;
#ifdef _WIN32
    /* The isolated Windows run executes in its own temporary directory. */
    snprintf(buf, cap, "rfc_jpeg_%d_%u_%s", (int)_getpid(), counter, tag);
#else
    snprintf(buf, cap, "/tmp/rfc_jpeg_%ld_%u_%s", (long)getpid(), counter, tag);
#endif
    remove(buf);
}

/* ------------------------------------------------------------------------ */
/* Fixture writer through the libjpeg-turbo compressor                       */
/* ------------------------------------------------------------------------ */

typedef struct mk_marker {
    int marker;
    const uint8_t *data;
    unsigned len;
} mk_marker;

typedef struct mk_opts {
    uint32_t w, h;
    int ncomp;
    int hs[3], vs[3];
    int quality;
    int progressive;
    int arith;
    int restart;
    int optimize;
    int use_sel;
    int sel_dc[3], sel_ac[3];
    int multiscan;
    int rgb_adobe;
    int no_jfif;
    int flat;
    uint32_t seed;
    mk_marker extra[20];
    int extra_n;
} mk_opts;

static void mk_defaults(mk_opts *o, uint32_t w, uint32_t h, int ncomp) {
    memset(o, 0, sizeof(*o));
    o->w = w;
    o->h = h;
    o->ncomp = ncomp;
    for (int i = 0; i < 3; i++) {
        o->hs[i] = 1;
        o->vs[i] = 1;
    }
    if (ncomp == 3) {
        o->hs[0] = 2;
        o->vs[0] = 2; /* 4:2:0 */
    }
    o->quality = 85;
}

static uint8_t pattern_px(uint32_t x, uint32_t y, int c, uint32_t seed) {
    uint32_t v = (x * (7u + (uint32_t)c * 3u) + y * (5u + (uint32_t)c) +
                  ((x * y + seed) % (29u + (uint32_t)c))) ^ (seed * 13u);
    return (uint8_t)(v & 0xFFu);
}

static uint8_t *make_jpeg(const mk_opts *o, size_t *out_len) {
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr jerr;
    memset(&c, 0, sizeof(c));
    c.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&c);
    unsigned char *mem = NULL;
    unsigned long mem_len = 0u;
    jpeg_mem_dest(&c, &mem, &mem_len);
    c.image_width = o->w;
    c.image_height = o->h;
    c.input_components = o->ncomp;
    c.in_color_space = (o->ncomp == 1) ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&c);
    if (o->ncomp == 3 && o->rgb_adobe) {
        jpeg_set_colorspace(&c, JCS_RGB);
    }
    jpeg_set_quality(&c, o->quality, TRUE);
    for (int i = 0; i < o->ncomp; i++) {
        c.comp_info[i].h_samp_factor = o->hs[i];
        c.comp_info[i].v_samp_factor = o->vs[i];
        if (o->use_sel) {
            c.comp_info[i].dc_tbl_no = o->sel_dc[i];
            c.comp_info[i].ac_tbl_no = o->sel_ac[i];
        }
    }
    if (o->no_jfif) {
        c.write_JFIF_header = FALSE;
    }
    c.arith_code = o->arith ? TRUE : FALSE;
    c.optimize_coding = o->optimize ? TRUE : FALSE;
    c.restart_interval = (unsigned)o->restart;
    static jpeg_scan_info scans[3];
    if (o->progressive) {
        jpeg_simple_progression(&c);
    } else if (o->multiscan) {
        for (int i = 0; i < o->ncomp; i++) {
            memset(&scans[i], 0, sizeof(scans[i]));
            scans[i].comps_in_scan = 1;
            scans[i].component_index[0] = i;
            scans[i].Ss = 0;
            scans[i].Se = 63;
        }
        c.scan_info = scans;
        c.num_scans = o->ncomp;
    }
    jpeg_start_compress(&c, TRUE);
    for (int i = 0; i < o->extra_n; i++) {
        jpeg_write_marker(&c, o->extra[i].marker, o->extra[i].data,
                          o->extra[i].len);
    }
    size_t row_b = (size_t)o->w * (size_t)o->ncomp;
    uint8_t *row = (uint8_t *)malloc(row_b);
    CHECK(row != NULL);
    while (c.next_scanline < c.image_height) {
        for (uint32_t x = 0u; x < o->w; x++) {
            for (int k = 0; k < o->ncomp; k++) {
                row[(size_t)x * (size_t)o->ncomp + (size_t)k] =
                    o->flat ? (uint8_t)(100 + 20 * k)
                            : pattern_px(x, c.next_scanline, k, o->seed);
            }
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&c, &rp, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    uint8_t *out = (uint8_t *)malloc(mem_len);
    CHECK(out != NULL);
    memcpy(out, mem, mem_len);
    free(mem);
    *out_len = (size_t)mem_len;
    return out;
}

/* Reference decode with the pinned settings straight through libjpeg-turbo. */
static uint8_t *ref_decode(const uint8_t *b, size_t n, uint32_t *w, uint32_t *h,
                           int *comps) {
    struct jpeg_decompress_struct d;
    struct jpeg_error_mgr jerr;
    memset(&d, 0, sizeof(d));
    d.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, b, (unsigned long)n);
    CHECK(jpeg_read_header(&d, TRUE) == JPEG_HEADER_OK);
    d.out_color_space = d.num_components == 1 ? JCS_GRAYSCALE : JCS_RGB;
    d.dct_method = JDCT_ISLOW;
    d.do_fancy_upsampling = TRUE;
    d.do_block_smoothing = FALSE;
    jpeg_start_decompress(&d);
    *w = d.output_width;
    *h = d.output_height;
    *comps = d.output_components;
    size_t rb = (size_t)*w * (size_t)*comps;
    uint8_t *px = (uint8_t *)malloc(rb * (size_t)*h);
    CHECK(px != NULL);
    while (d.output_scanline < d.output_height) {
        JSAMPROW r = px + (size_t)d.output_scanline * rb;
        jpeg_read_scanlines(&d, &r, 1);
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    return px;
}

static size_t find_marker(const uint8_t *b, size_t n, uint8_t m, int nth) {
    int seen = 0;
    for (size_t i = 2u; i + 1u < n; i++) {
        if (b[i] == 0xFFu && b[i + 1u] == m) {
            if (seen == nth) {
                return i;
            }
            seen++;
        }
    }
    return 0u;
}

static uint8_t *splice(const uint8_t *b, size_t n, size_t at,
                       const uint8_t *ins, size_t ins_len, size_t *out_len) {
    uint8_t *o = (uint8_t *)malloc(n + ins_len);
    CHECK(o != NULL);
    memcpy(o, b, at);
    memcpy(o + at, ins, ins_len);
    memcpy(o + at + ins_len, b + at, n - at);
    *out_len = n + ins_len;
    return o;
}

/* Insert one APP/COM segment right after SOI. */
static uint8_t *with_segment(const uint8_t *b, size_t n, uint8_t marker,
                             const uint8_t *payload, size_t plen,
                             size_t *out_len) {
    uint8_t *seg = (uint8_t *)malloc(plen + 4u);
    CHECK(seg != NULL && plen + 2u <= 65535u);
    seg[0] = 0xFFu;
    seg[1] = marker;
    seg[2] = (uint8_t)((plen + 2u) >> 8);
    seg[3] = (uint8_t)((plen + 2u) & 0xFFu);
    memcpy(seg + 4u, payload, plen);
    uint8_t *o = splice(b, n, 2u, seg, plen + 4u, out_len);
    free(seg);
    return o;
}

/* ------------------------------------------------------------------------ */
/* Adapter and job drivers                                                   */
/* ------------------------------------------------------------------------ */

static rfc_status adapter_probe(const uint8_t *b, size_t n, const rfc_limits *lim,
                                rfc_image_probe *pr) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator al;
    init_exec(&exec, &st, &al);
    if (lim) {
        exec.limits = *lim;
    }
    const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_JPEG);
    CHECK(a != NULL);
    void *s = NULL;
    CHECK(a->open_session(&exec, &s) == RFC_OK);
    memset(pr, 0, sizeof(*pr));
    rfc_status r = a->probe_session(s, &exec, b, n, pr);
    a->close_session(s);
    rfc_registry_release_session(RFC_FORMAT_JPEG);
    rfc_metadata owned = pr->container_meta;
    pr->container_meta.icc = NULL;
    pr->container_meta.exif = NULL;
    pr->container_meta.xmp = NULL;
    rfc_metadata_free(&owned, &al);
    CHECK(st.live == 0u);
    return r;
}

static rfc_status adapter_decode(const uint8_t *b, size_t n, uint8_t **out_px,
                                 rfc_layout *out_lay, rfc_metadata *out_meta,
                                 rfc_budget *budget) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator al;
    init_exec(&exec, &st, &al);
    rfc_allocator ba;
    if (budget) {
        rfc_budget_allocator(budget, &ba);
        exec.allocator = &ba;
        exec.budget = budget;
    }
    const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_JPEG);
    CHECK(a != NULL);
    void *s = NULL;
    CHECK(a->open_session(&exec, &s) == RFC_OK);
    rfc_frame *fr = NULL;
    size_t fc = 0u;
    rfc_status r = a->decode_session(s, &exec, b, n, NULL, 0u, &fr, &fc, NULL);
    a->close_session(s);
    rfc_registry_release_session(RFC_FORMAT_JPEG);
    if (r == RFC_OK) {
        CHECK(fc == 1u);
        if (out_lay) {
            *out_lay = fr[0].layout;
        }
        if (out_meta) {
            memset(out_meta, 0, sizeof(*out_meta));
            out_meta->orientation = fr[0].metadata.orientation;
            out_meta->icc_len = fr[0].metadata.icc_len;
            out_meta->exif_len = fr[0].metadata.exif_len;
            out_meta->xmp_len = fr[0].metadata.xmp_len;
        }
        if (out_px) {
            size_t rb = (size_t)fr[0].layout.width *
                        (fr[0].layout.format == RFC_SAMPLE_GRAY8 ? 1u : 3u);
            uint8_t *px = (uint8_t *)malloc(rb * fr[0].layout.height);
            CHECK(px != NULL);
            for (uint32_t y = 0u; y < fr[0].layout.height; y++) {
                memcpy(px + (size_t)y * rb,
                       fr[0].data + (size_t)y * fr[0].layout.stride_bytes, rb);
            }
            *out_px = px;
        }
        rfc_frame_free(&fr[0]);
        exec.allocator->free(exec.allocator->ctx, fr);
    }
    if (!budget) {
        CHECK(st.live == 0u);
    }
    return r;
}

static rfc_status read_coeffs(const uint8_t *b, size_t n,
                              rfc_jpeg_coeff_image **out) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator al;
    init_exec(&exec, &st, &al);
    exec.allocator = NULL; /* stdlib */
    const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
    CHECK(t != NULL);
    void *s = NULL;
    CHECK(t->open_session(&exec, &s) == RFC_OK);
    rfc_metadata m;
    rfc_status r = t->read_coefficients(s, &exec, b, n, out, &m);
    if (r == RFC_OK) {
        rfc_metadata_free(&m, NULL);
    }
    t->close_session(s);
    rfc_registry_release_jpeg_transcode();
    return r;
}

static void free_coeffs(rfc_jpeg_coeff_image *img) {
    if (!img) {
        return;
    }
    for (int c = 0; c < 4; c++) {
        free((void *)img->components[c].dct_blocks);
    }
    free(img);
}

static rfc_status coeffs_equal(const uint8_t *a, size_t an, const uint8_t *b,
                               size_t bn) {
    rfc_jpeg_coeff_image *ca = NULL, *cb = NULL;
    CHECK(read_coeffs(a, an, &ca) == RFC_OK);
    CHECK(read_coeffs(b, bn, &cb) == RFC_OK);
    rfc_sample_mismatch mm;
    rfc_status r = rfc_verify_jpeg_coefficients(ca, cb, &mm);
    free_coeffs(ca);
    free_coeffs(cb);
    return r;
}

static rfc_status optimize_direct(const uint8_t *b, size_t n, const char *meta,
                                  rfc_budget *budget, uint8_t **out,
                                  size_t *out_len) {
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator al;
    init_exec(&exec, &st, &al);
    rfc_allocator ba;
    if (budget) {
        rfc_budget_allocator(budget, &ba);
        exec.allocator = &ba;
        exec.budget = budget;
    }
    const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
    CHECK(t != NULL);
    void *s = NULL;
    CHECK(t->open_session(&exec, &s) == RFC_OK);
    rfc_setting_value sv[1] = {{"metadata", meta}};
    rfc_encoded enc;
    memset(&enc, 0, sizeof(enc));
    rfc_status r = t->optimize_jpeg(s, &exec, b, n, sv, 1u, &enc);
    t->close_session(s);
    rfc_registry_release_jpeg_transcode();
    if (r == RFC_OK && out) {
        *out = (uint8_t *)malloc(enc.len);
        CHECK(*out != NULL);
        memcpy(*out, enc.data, enc.len);
        *out_len = enc.len;
    }
    rfc_encoded_free(&enc);
    if (!budget) {
        CHECK(st.live == 0u);
    }
    return r;
}

typedef struct job_result {
    rfc_status submit;
    rfc_status status;
    rfc_job_state state;
    rfc_job_stage failing;
    int has_rep;
    rfc_verify_report rep;
    char detail[RFC_JOB_DETAIL_MAX];
} job_result;

static void run_job(rfc_format_id sf, rfc_format_id df, const char *src,
                    const char *dst, const char *exactness, const char *meta,
                    uint32_t ack, uint64_t working_set, job_result *out) {
    memset(out, 0, sizeof(*out));
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);
    rfc_io_source src_io;
    rfc_io_sink dst_io;
    rfc_io_source_init_path(&src_io, src, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, dst, RFC_EXPORT_REFUSE_EXISTING);
    rfc_setting_value sv[2];
    size_t n = 0u;
    if (exactness) {
        sv[n].id = "exactness";
        sv[n].value = exactness;
        n++;
    }
    if (meta) {
        sv[n].id = "metadata";
        sv[n].value = meta;
        n++;
    }
    rfc_job_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_format = sf;
    sp.target_format = df;
    sp.io_source = &src_io;
    sp.io_sink = &dst_io;
    sp.settings = n ? sv : NULL;
    sp.setting_count = n;
    sp.acknowledged_unsupported_metadata = ack;
    sp.expected_working_set_bytes = working_set;
    rfc_job *j = NULL;
    out->submit = rfc_scheduler_submit(sched, NULL, &sp, &j);
    if (out->submit == RFC_OK) {
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        out->status = snap.status;
        out->state = snap.state;
        out->failing = snap.failing_stage;
        memcpy(out->detail, snap.detail, sizeof(out->detail));
        out->has_rep = (rfc_job_verify_report_get(j, &out->rep) == RFC_OK);
        rfc_job_release(j);
    }
    rfc_scheduler_destroy(sched);
}

static int has_app_segment(const uint8_t *b, size_t n, uint8_t marker,
                           const char *prefix) {
    size_t plen = strlen(prefix);
    for (size_t i = 2u; i + 4u + plen <= n; i++) {
        if (b[i] == 0xFFu && b[i + 1u] == marker &&
            memcmp(b + i + 4u, prefix, plen) == 0) {
            return 1;
        }
        if (b[i] == 0xFFu && b[i + 1u] == 0xDAu) {
            break;
        }
    }
    return 0;
}

static const uint8_t k_exif_ori6[] = {
    'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1,
    0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 6, 0, 0, 0, 0, 0, 0};
static const char k_xmp_hdr[] = "http://ns.adobe.com/xap/1.0/";

/* ------------------------------------------------------------------------ */
/* 1. Registration and upstream contract                                    */
/* ------------------------------------------------------------------------ */

static rfc_status dummy_open(const rfc_exec_context *e, void **s) {
    (void)e;
    *s = NULL;
    return RFC_OK;
}
static void dummy_close(void *s) { (void)s; }
static rfc_status dummy_pre(void *s, const rfc_exec_context *e, const uint8_t *b,
                            size_t n) {
    (void)s; (void)e; (void)b; (void)n;
    return RFC_ERR_INVALID_ARGUMENT;
}
static rfc_status dummy_opt(void *s, const rfc_exec_context *e, const uint8_t *b,
                            size_t n, const rfc_setting_value *v, size_t vc,
                            rfc_encoded *o) {
    (void)s; (void)e; (void)b; (void)n; (void)v; (void)vc; (void)o;
    return RFC_ERR_INVALID_ARGUMENT;
}
static rfc_status dummy_read(void *s, const rfc_exec_context *e, const uint8_t *b,
                             size_t n, struct rfc_jpeg_coeff_image **c,
                             rfc_metadata *m) {
    (void)s; (void)e; (void)b; (void)n; (void)c; (void)m;
    return RFC_ERR_INVALID_ARGUMENT;
}
static void dummy_free(void *s, const rfc_exec_context *e,
                       struct rfc_jpeg_coeff_image *c) {
    (void)s; (void)e; (void)c;
}
static rfc_status dummy_cmp(void *s, const rfc_exec_context *e, const uint8_t *a,
                            size_t an, const uint8_t *b, size_t bn, int strip,
                            const char **d) {
    (void)s; (void)e; (void)a; (void)an; (void)b; (void)bn; (void)strip; (void)d;
    return RFC_ERR_INVALID_ARGUMENT;
}

static void test_registration_and_upstream_contract(void) {
    const rfc_capability *cap = rfc_registry_find(RFC_FORMAT_JPEG);
    CHECK(cap != NULL && cap->availability == RFC_AVAILABILITY_USABLE);
    CHECK(cap->can_read && cap->can_write);
    CHECK(cap->sample_count == 2u && cap->samples[0] == RFC_SAMPLE_GRAY8 &&
          cap->samples[1] == RFC_SAMPLE_RGB8);
    CHECK(cap->metadata_items == (RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION |
                                  RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP));
    CHECK(strcmp(rfc_registry_setting_default(RFC_FORMAT_JPEG, "exactness"),
                 "coefficient-exact") == 0);
    CHECK(strcmp(rfc_registry_setting_default(RFC_FORMAT_PNG, "exactness"),
                 "pixel-exact") == 0);
    CHECK(rfc_registry_setting_default(RFC_FORMAT_JPEG, "frames") == NULL);

    /* Rollback: a foreign transcode adapter makes registration fail cleanly. */
    static const rfc_jpeg_transcode_adapter dummy = {
        RFC_CODEC_ABI_VERSION, dummy_open, dummy_close, NULL, dummy_pre,
        dummy_opt, dummy_read, dummy_free, dummy_cmp};
    CHECK(rfc_registry_register_jpeg_transcode(&dummy) == RFC_OK);
    CHECK(rfc_codecs_register_jpeg() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_JPEG) == NULL);
    CHECK(rfc_registry_unregister_jpeg_transcode() == RFC_OK);

    CHECK(rfc_codecs_register_jpeg() == RFC_OK);
    CHECK(rfc_codecs_register_jpeg() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_JPEG) != NULL);
    /* Busy unregister leaves both adapters registered. */
    const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
    CHECK(t != NULL);
    CHECK(rfc_codecs_unregister_jpeg() == RFC_ERR_BUSY);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_JPEG) != NULL);
    rfc_registry_release_jpeg_transcode();
    const rfc_session_adapter *sa = rfc_registry_acquire_session(RFC_FORMAT_JPEG);
    CHECK(sa != NULL);
    CHECK(rfc_codecs_unregister_jpeg() == RFC_ERR_BUSY);
    CHECK(rfc_registry_acquire_jpeg_transcode() != NULL);
    rfc_registry_release_jpeg_transcode();
    rfc_registry_release_session(RFC_FORMAT_JPEG);
    CHECK(rfc_codecs_unregister_jpeg() == RFC_OK);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_JPEG) == NULL);
    CHECK(rfc_codecs_register_jpeg() == RFC_OK);

    /* Pixel -> JPEG encode is always refused (lossy). */
    {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator al;
        init_exec(&exec, &st, &al);
        const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_JPEG);
        void *s = NULL;
        CHECK(a->open_session(&exec, &s) == RFC_OK);
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_frame fr;
        memset(&fr, 0, sizeof(fr));
        CHECK(rfc_frame_alloc(&fr, &lay, NULL, &al) == RFC_OK);
        rfc_encoded enc;
        for (int k = 0; k < 3; k++) {
            const char *ex[3] = {"pixel-exact", "byte-identical", "coefficient-exact"};
            rfc_setting_value sv[1] = {{"exactness", ex[k]}};
            CHECK(a->encode_session(s, &exec, &fr, 1u, NULL, sv, 1u, &enc) ==
                  RFC_ERR_UNSUPPORTED_COMBINATION);
            CHECK(enc.data == NULL);
        }
        rfc_frame_free(&fr);
        a->close_session(s);
        rfc_registry_release_session(RFC_FORMAT_JPEG);
        CHECK(st.live == 0u);
    }

    /* Upstream contract: adapter pixels equal libjpeg-turbo under the pinned
     * settings across sampling, coding modes and restart intervals. */
    const int samp[6][6] = {
        {1, 1, 1, 1, 1, 1}, {2, 2, 1, 1, 1, 1}, {2, 1, 1, 1, 1, 1},
        {1, 2, 1, 1, 1, 1}, {4, 1, 1, 1, 1, 1}, {2, 2, 1, 1, 1, 1}};
    for (int mode = 0; mode < 4; mode++) {
        for (int si = 0; si < 6; si++) {
            mk_opts o;
            mk_defaults(&o, 37u + (uint32_t)si, 29u + (uint32_t)mode, 3);
            for (int c = 0; c < 3; c++) {
                o.hs[c] = samp[si][c * 2];
                o.vs[c] = samp[si][c * 2 + 1];
            }
            o.progressive = (mode == 1);
            o.arith = (mode == 2);
            o.restart = (mode == 3) ? 2 : 0;
            o.seed = (uint32_t)(mode * 7 + si);
            size_t n = 0u;
            uint8_t *j = make_jpeg(&o, &n);
            uint32_t rw = 0u, rh = 0u;
            int rc = 0;
            uint8_t *ref = ref_decode(j, n, &rw, &rh, &rc);
            uint8_t *px = NULL;
            rfc_layout lay;
            CHECK(adapter_decode(j, n, &px, &lay, NULL, NULL) == RFC_OK);
            CHECK(lay.width == rw && lay.height == rh &&
                  lay.format == RFC_SAMPLE_RGB8 && rc == 3);
            CHECK(memcmp(px, ref, (size_t)rw * rh * 3u) == 0);
            free(px);
            free(ref);
            free(j);
        }
    }
    {
        mk_opts o;
        mk_defaults(&o, 33u, 21u, 1);
        size_t n = 0u;
        uint8_t *j = make_jpeg(&o, &n);
        uint32_t rw, rh;
        int rc;
        uint8_t *ref = ref_decode(j, n, &rw, &rh, &rc);
        uint8_t *px = NULL;
        rfc_layout lay;
        CHECK(adapter_decode(j, n, &px, &lay, NULL, NULL) == RFC_OK);
        CHECK(lay.format == RFC_SAMPLE_GRAY8 && rc == 1);
        CHECK(memcmp(px, ref, (size_t)rw * rh) == 0);
        free(px);
        free(ref);
        free(j);
    }
    printf("jpeg: registration_and_upstream_contract ok\n");
}

/* ------------------------------------------------------------------------ */
/* 2. Import, pinned decoder and metadata                                    */
/* ------------------------------------------------------------------------ */

static uint8_t *make_meta_jpeg(size_t icc_len, int with_exif, int with_xmp,
                               size_t *out_len, uint8_t **out_icc) {
    mk_opts o;
    mk_defaults(&o, 24u, 16u, 3);
    static uint8_t chunk_a[65519], chunk_b[65519], xmp_seg[200];
    uint8_t *icc = (uint8_t *)malloc(icc_len ? icc_len : 1u);
    CHECK(icc != NULL);
    for (size_t i = 0u; i < icc_len; i++) {
        icc[i] = (uint8_t)(i * 31u + 7u);
    }
    size_t first = icc_len > 65519u - 14u ? 65519u - 14u : icc_len;
    unsigned nchunks = icc_len > first ? 2u : 1u;
    if (icc_len > 0u) {
        memcpy(chunk_a, "ICC_PROFILE\0", 12);
        chunk_a[12] = 1u;
        chunk_a[13] = (uint8_t)nchunks;
        memcpy(chunk_a + 14, icc, first);
        o.extra[o.extra_n].marker = JPEG_APP0 + 2;
        o.extra[o.extra_n].data = chunk_a;
        o.extra[o.extra_n].len = (unsigned)(first + 14u);
        o.extra_n++;
        if (nchunks == 2u) {
            memcpy(chunk_b, "ICC_PROFILE\0", 12);
            chunk_b[12] = 2u;
            chunk_b[13] = 2u;
            memcpy(chunk_b + 14, icc + first, icc_len - first);
            o.extra[o.extra_n].marker = JPEG_APP0 + 2;
            o.extra[o.extra_n].data = chunk_b;
            o.extra[o.extra_n].len = (unsigned)(icc_len - first + 14u);
            o.extra_n++;
        }
    }
    if (with_exif) {
        o.extra[o.extra_n].marker = JPEG_APP0 + 1;
        o.extra[o.extra_n].data = k_exif_ori6;
        o.extra[o.extra_n].len = (unsigned)sizeof(k_exif_ori6);
        o.extra_n++;
    }
    if (with_xmp) {
        memcpy(xmp_seg, k_xmp_hdr, sizeof(k_xmp_hdr));
        memcpy(xmp_seg + sizeof(k_xmp_hdr), "<x:xmpmeta>rf</x:xmpmeta>", 25);
        o.extra[o.extra_n].marker = JPEG_APP0 + 1;
        o.extra[o.extra_n].data = xmp_seg;
        o.extra[o.extra_n].len = (unsigned)(sizeof(k_xmp_hdr) + 25u);
        o.extra_n++;
    }
    uint8_t *j = make_jpeg(&o, out_len);
    if (out_icc) {
        *out_icc = icc;
    } else {
        free(icc);
    }
    return j;
}

static void test_import_pinned_decoder_and_metadata(void) {
    size_t n = 0u;
    uint8_t *icc = NULL;
    uint8_t *j = make_meta_jpeg(70000u, 1, 1, &n, &icc);
    rfc_image_probe pr;
    CHECK(adapter_probe(j, n, NULL, &pr) == RFC_OK);
    CHECK(pr.first_frame.width == 24u && pr.first_frame.height == 16u);
    CHECK(pr.first_frame.format == RFC_SAMPLE_RGB8 && pr.frame_count == 1u);
    CHECK(pr.container_meta.icc_len == 70000u);
    CHECK(pr.container_meta.exif_len == sizeof(k_exif_ori6) - 6u);
    CHECK(pr.container_meta.xmp_len == 25u);
    CHECK(pr.container_meta.orientation == RFC_ORIENTATION_ROTATE_90);
    rfc_metadata dm;
    rfc_layout lay;
    uint8_t *px = NULL;
    CHECK(adapter_decode(j, n, &px, &lay, &dm, NULL) == RFC_OK);
    CHECK(dm.icc_len == 70000u && dm.exif_len == 26u && dm.xmp_len == 25u);
    CHECK(dm.orientation == RFC_ORIENTATION_ROTATE_90);
    free(px);

    /* JPEG -> PNG: pixel-exact by default; PNG carries ICC only, so EXIF,
     * XMP and orientation need an explicit acknowledgement. */
    char p_src[512], p_png[512], p_png2[512];
    make_temp_path(p_src, sizeof(p_src), "meta.jpg");
    make_temp_path(p_png, sizeof(p_png), "meta.png");
    make_temp_path(p_png2, sizeof(p_png2), "meta2.png");
    free(icc);
    free(j);
    /* The PNG encoder validates ICC profiles, so the policy job carries EXIF
     * and XMP only; the synthetic ICC above is proven by probe and decode. */
    j = make_meta_jpeg(0u, 1, 1, &n, &icc);
    CHECK(write_file_all(p_src, j, n));
    job_result r;
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_PNG, p_src, p_png, NULL, NULL, 0u,
            64u << 20, &r);
    CHECK(r.submit == RFC_OK && r.status == RFC_ERR_METADATA_POLICY);
    CHECK(!file_exists(p_png));
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_PNG, p_src, p_png2, NULL, NULL,
            RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP | RFC_META_ITEM_ORIENTATION,
            64u << 20, &r);
    CHECK(r.submit == RFC_OK && r.state == RFC_JOB_SUCCEEDED);
    CHECK(r.has_rep && r.rep.exactness == RFC_EXACT_PIXEL_EXACT);
    CHECK(file_exists(p_png2));
    remove(p_src);
    remove(p_png2);
    free(icc);
    free(j);

    /* EXIF without orientation reports normal; no EXIF reports normal. */
    {
        mk_opts o;
        mk_defaults(&o, 8u, 8u, 1);
        size_t m = 0u;
        uint8_t *g = make_jpeg(&o, &m);
        CHECK(adapter_probe(g, m, NULL, &pr) == RFC_OK);
        CHECK(pr.container_meta.orientation == RFC_ORIENTATION_NORMAL);
        CHECK(pr.first_frame.format == RFC_SAMPLE_GRAY8);
        free(g);
    }
    printf("jpeg: import_pinned_decoder_and_metadata ok\n");
}

/* ------------------------------------------------------------------------ */
/* 3. Coefficient optimization and the marker contract                       */
/* ------------------------------------------------------------------------ */

static void opt_job_roundtrip(const uint8_t *j, size_t n, const char *meta,
                              int expect_smaller, const char *tag,
                              uint8_t **out_bytes, size_t *out_len) {
    char p_src[512], p_dst[512];
    make_temp_path(p_src, sizeof(p_src), tag);
    make_temp_path(p_dst, sizeof(p_dst), "opt.jpg");
    CHECK(write_file_all(p_src, j, n));
    job_result r;
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, NULL, meta, 0u, 0u,
            &r);
    if (r.state != RFC_JOB_SUCCEEDED) {
        fprintf(stderr, "%s: submit=%s status=%s detail=%s\n", tag,
                rfc_status_name(r.submit), rfc_status_name(r.status), r.detail);
    }
    CHECK(r.submit == RFC_OK && r.state == RFC_JOB_SUCCEEDED);
    CHECK(r.has_rep && r.rep.exactness == RFC_EXACT_COEFFICIENT_EXACT);
    size_t on = 0u;
    uint8_t *o = read_file_all(p_dst, &on);
    CHECK(o != NULL && on > 0u);
    CHECK(coeffs_equal(j, n, o, on) == RFC_OK);
    if (expect_smaller > 0) {
        CHECK(on < n && (on != n || memcmp(o, j, n) != 0));
    } else if (expect_smaller < 0) {
        CHECK(on == n && memcmp(o, j, n) == 0); /* not-smaller fallback */
    }
    remove(p_src);
    remove(p_dst);
    if (out_bytes) {
        *out_bytes = o;
        *out_len = on;
    } else {
        free(o);
    }
}

static void test_coefficient_optimization_and_markers(void) {
    /* Unoptimized baseline 4:2:0 with restarts becomes strictly smaller; the
     * already-optimized result takes the not-smaller fallback. */
    mk_opts o;
    mk_defaults(&o, 96u, 72u, 3);
    o.restart = 1;
    size_t n = 0u;
    uint8_t *j = make_jpeg(&o, &n);
    uint8_t *opt = NULL;
    size_t opt_n = 0u;
    opt_job_roundtrip(j, n, NULL, 1, "base.jpg", &opt, &opt_n);
    opt_job_roundtrip(opt, opt_n, NULL, -1, "again.jpg", NULL, NULL);
    free(opt);
    free(j);

    /* Progressive, arithmetic, gray, 4:4:4, multiscan and nondefault
     * selectors keep their coefficients and coding mode. */
    for (int k = 0; k < 6; k++) {
        mk_defaults(&o, 61u, 45u, k == 2 ? 1 : 3);
        if (k == 0) o.progressive = 1;
        if (k == 1) o.arith = 1;
        if (k == 3) { o.hs[0] = 1; o.vs[0] = 1; }
        if (k == 4) o.multiscan = 1;
        if (k == 5) {
            o.use_sel = 1;
            o.sel_dc[0] = 1; o.sel_ac[0] = 1;
            o.sel_dc[1] = 0; o.sel_ac[1] = 0;
            o.sel_dc[2] = 0; o.sel_ac[2] = 0;
        }
        o.seed = (uint32_t)(40 + k);
        j = make_jpeg(&o, &n);
        uint8_t *out = NULL;
        size_t out_n = 0u;
        opt_job_roundtrip(j, n, NULL, 0, "mode.jpg", &out, &out_n);
        rfc_jpeg_coeff_image *ci = NULL;
        CHECK(read_coeffs(out, out_n, &ci) == RFC_OK);
        CHECK(ci->progressive == (k == 0));
        CHECK(ci->arithmetic_coding == (k == 1));
        if (k == 5) {
            CHECK(ci->components[0].dc_table_idx == 1u &&
                  ci->components[0].ac_table_idx == 1u &&
                  ci->components[1].dc_table_idx == 0u);
        }
        free_coeffs(ci);
        free(out);
        free(j);
    }

    /* Quantization slot redefined between scans: refused for optimization
     * under both policies, still importable as pixels. */
    {
        mk_defaults(&o, 32u, 32u, 3);
        o.multiscan = 1;
        j = make_jpeg(&o, &n);
        size_t sos2 = find_marker(j, n, 0xDAu, 1);
        CHECK(sos2 > 0u);
        uint8_t dqt[69];
        dqt[0] = 0xFFu;
        dqt[1] = 0xDBu;
        dqt[2] = 0u;
        dqt[3] = 67u;
        dqt[4] = 0u; /* 8-bit, slot 0 (luma, already latched) */
        for (int i = 0; i < 64; i++) {
            dqt[5 + i] = (uint8_t)(1 + i);
        }
        size_t rn = 0u;
        uint8_t *redef = splice(j, n, sos2, dqt, sizeof(dqt), &rn);
        uint8_t *px = NULL;
        CHECK(adapter_decode(redef, rn, &px, NULL, NULL, NULL) == RFC_OK);
        free(px);
        CHECK(optimize_direct(redef, rn, "preserve", NULL, NULL, NULL) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(optimize_direct(redef, rn, "strip", NULL, NULL, NULL) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        char p_src[512], p_dst[512];
        make_temp_path(p_src, sizeof(p_src), "redef.jpg");
        make_temp_path(p_dst, sizeof(p_dst), "redef_out.jpg");
        CHECK(write_file_all(p_src, redef, rn));
        job_result r;
        run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, NULL, NULL, 0u,
                0u, &r);
        CHECK(r.status == RFC_ERR_UNSUPPORTED_COMBINATION &&
              r.failing == RFC_STAGE_IMPORT && !file_exists(p_dst));
        run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, NULL, "strip",
                0u, 0u, &r);
        CHECK(r.status == RFC_ERR_UNSUPPORTED_COMBINATION &&
              r.failing == RFC_STAGE_IMPORT && !file_exists(p_dst));
        remove(p_src);
        free(redef);
        free(j);
    }

    /* Marker contract: JFIF thumbnail, COM and APP13 under preserve and strip. */
    {
        static uint8_t jfif[5 + 9 + 2 * 2 * 3];
        memcpy(jfif, "JFIF\0", 5);
        jfif[5] = 1; jfif[6] = 2; jfif[7] = 1;
        jfif[8] = 0; jfif[9] = 72; jfif[10] = 0; jfif[11] = 72;
        jfif[12] = 2; jfif[13] = 2; /* 2x2 RGB thumbnail */
        for (int i = 0; i < 12; i++) jfif[14 + i] = (uint8_t)(i * 20);
        static const uint8_t com[] = "RasterFold comment";
        static const uint8_t app13[] = "Photoshop 3.0\0" "8BIM\4\4\0\0\0\0\0\0";
        mk_defaults(&o, 48u, 40u, 3);
        o.no_jfif = 1;
        o.extra[0] = (mk_marker){JPEG_APP0, jfif, (unsigned)sizeof(jfif)};
        o.extra[1] = (mk_marker){JPEG_COM, com, (unsigned)(sizeof(com) - 1u)};
        o.extra[2] = (mk_marker){JPEG_APP0 + 13, app13, (unsigned)sizeof(app13)};
        o.extra_n = 3;
        j = make_jpeg(&o, &n);
        uint8_t *keep = NULL, *strip = NULL;
        size_t keep_n = 0u, strip_n = 0u;
        opt_job_roundtrip(j, n, "preserve", 0, "mk.jpg", &keep, &keep_n);
        opt_job_roundtrip(j, n, "strip", 0, "mk2.jpg", &strip, &strip_n);
        CHECK(has_app_segment(keep, keep_n, 0xFEu, "RasterFold"));
        CHECK(has_app_segment(keep, keep_n, 0xEDu, "Photoshop"));
        CHECK(!has_app_segment(strip, strip_n, 0xFEu, "RasterFold"));
        CHECK(!has_app_segment(strip, strip_n, 0xEDu, "Photoshop"));
        CHECK(has_app_segment(strip, strip_n, 0xE0u, "JFIF"));
        /* Fresh JFIF under strip has no thumbnail: segment length 16. */
        size_t a0 = find_marker(strip, strip_n, 0xE0u, 0);
        CHECK(a0 > 0u && strip[a0 + 2u] == 0u && strip[a0 + 3u] == 16u);
        uint8_t *p0 = NULL, *p1 = NULL, *p2 = NULL;
        CHECK(adapter_decode(j, n, &p0, NULL, NULL, NULL) == RFC_OK);
        CHECK(adapter_decode(keep, keep_n, &p1, NULL, NULL, NULL) == RFC_OK);
        CHECK(adapter_decode(strip, strip_n, &p2, NULL, NULL, NULL) == RFC_OK);
        CHECK(memcmp(p0, p1, 48u * 40u * 3u) == 0 &&
              memcmp(p0, p2, 48u * 40u * 3u) == 0);
        free(p0); free(p1); free(p2);

        /* compare_markers refuses an extra marker either way. */
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator al;
        init_exec(&exec, &st, &al);
        const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
        void *s = NULL;
        CHECK(t->open_session(&exec, &s) == RFC_OK);
        const char *det = NULL;
        CHECK(t->compare_markers(s, &exec, j, n, keep, keep_n, 0, &det) == RFC_OK);
        CHECK(t->compare_markers(s, &exec, j, n, strip, strip_n, 1, &det) == RFC_OK);
        size_t xn = 0u;
        uint8_t *extra = with_segment(keep, keep_n, 0xFEu, (const uint8_t *)"x", 1u, &xn);
        CHECK(t->compare_markers(s, &exec, j, n, extra, xn, 0, &det) ==
              RFC_ERR_VERIFY_MISMATCH && det != NULL);
        free(extra);
        extra = with_segment(strip, strip_n, 0xFEu, (const uint8_t *)"x", 1u, &xn);
        CHECK(t->compare_markers(s, &exec, j, n, extra, xn, 1, &det) ==
              RFC_ERR_VERIFY_MISMATCH);
        free(extra);
        CHECK(t->compare_markers(s, &exec, j, n, keep, keep_n, 1, &det) ==
              RFC_ERR_VERIFY_MISMATCH);
        t->close_session(s);
        rfc_registry_release_jpeg_transcode();
        free(keep);
        free(strip);
        free(j);
    }

    /* Adobe transform 0 RGB keeps its interpretation under both policies. */
    {
        mk_defaults(&o, 40u, 24u, 3);
        o.rgb_adobe = 1;
        o.hs[0] = 1; o.vs[0] = 1;
        j = make_jpeg(&o, &n);
        CHECK(has_app_segment(j, n, 0xEEu, "Adobe"));
        uint8_t *p0 = NULL;
        CHECK(adapter_decode(j, n, &p0, NULL, NULL, NULL) == RFC_OK);
        for (int pol = 0; pol < 2; pol++) {
            uint8_t *out = NULL;
            size_t out_n = 0u;
            opt_job_roundtrip(j, n, pol ? "strip" : "preserve", 0, "adobe.jpg",
                              &out, &out_n);
            CHECK(has_app_segment(out, out_n, 0xEEu, "Adobe"));
            uint8_t *p1 = NULL;
            CHECK(adapter_decode(out, out_n, &p1, NULL, NULL, NULL) == RFC_OK);
            CHECK(memcmp(p0, p1, 40u * 24u * 3u) == 0);
            free(p1);
            free(out);
        }
        free(p0);
        free(j);
    }
    /* Progressive selectors come from the scans that use them: libjpeg's
     * default chroma DC table 1 survives although AC scans write DC 0. */
    {
        mk_defaults(&o, 40u, 40u, 3);
        o.progressive = 1;
        j = make_jpeg(&o, &n);
        rfc_jpeg_coeff_image *ci = NULL;
        CHECK(read_coeffs(j, n, &ci) == RFC_OK);
        CHECK(ci->components[1].dc_table_idx == 1u &&
              ci->components[1].ac_table_idx == 1u &&
              ci->components[0].dc_table_idx == 0u);
        free_coeffs(ci);
        uint8_t *out = NULL;
        size_t out_n = 0u;
        opt_job_roundtrip(j, n, NULL, 0, "psel.jpg", &out, &out_n);
        CHECK(read_coeffs(out, out_n, &ci) == RFC_OK);
        CHECK(ci->components[1].dc_table_idx == 1u &&
              ci->components[2].dc_table_idx == 1u);
        free_coeffs(ci);
        free(out);
        free(j);
    }

    /* An unused quantization slot is not image data: optimization succeeds
     * and compares equal on the referenced tables. */
    {
        mk_defaults(&o, 24u, 24u, 3);
        j = make_jpeg(&o, &n);
        size_t sof = find_marker(j, n, 0xC0u, 0);
        uint8_t dqt[69] = {0xFFu, 0xDBu, 0u, 67u, 3u};
        for (int i = 0; i < 64; i++) dqt[5 + i] = (uint8_t)(2 + i);
        size_t xn = 0u;
        uint8_t *x = splice(j, n, sof, dqt, sizeof(dqt), &xn);
        uint8_t *out = NULL;
        size_t out_n = 0u;
        opt_job_roundtrip(x, xn, NULL, 0, "extradqt.jpg", &out, &out_n);
        rfc_jpeg_coeff_image *ca = NULL, *cb = NULL;
        CHECK(read_coeffs(x, xn, &ca) == RFC_OK);
        CHECK(read_coeffs(out, out_n, &cb) == RFC_OK);
        CHECK(ca->quant_tables[3].populated && cb->quant_tables[3].populated);
        CHECK(memcmp(ca->quant_tables[3].values, cb->quant_tables[3].values,
                     sizeof(ca->quant_tables[3].values)) == 0);
        free_coeffs(ca);
        free_coeffs(cb);
        free(out);
        free(x);
        free(j);
    }

    /* A JFIF/Adobe header after the first scan would move ahead of the
     * output's first scan and change the colour interpretation: refused at
     * IMPORT, while pixel import of the source still works. */
    {
        mk_defaults(&o, 32u, 32u, 3);
        o.progressive = 1;
        o.no_jfif = 1;
        j = make_jpeg(&o, &n);
        size_t sos2 = find_marker(j, n, 0xDAu, 1);
        static const uint8_t adobe[16] = {0xFFu, 0xEEu, 0u, 14u, 'A', 'd', 'o', 'b',
                                          'e', 0u, 100u, 0u, 0u, 0u, 0u, 0u};
        size_t xn = 0u;
        uint8_t *x = splice(j, n, sos2, adobe, sizeof(adobe), &xn);
        uint8_t *px = NULL, *ref = NULL;
        CHECK(adapter_decode(x, xn, &px, NULL, NULL, NULL) == RFC_OK);
        CHECK(adapter_decode(j, n, &ref, NULL, NULL, NULL) == RFC_OK);
        CHECK(memcmp(px, ref, 32u * 32u * 3u) == 0); /* still YCbCr */
        free(px);
        free(ref);
        char p_src[512], p_dst[512];
        make_temp_path(p_src, sizeof(p_src), "late.jpg");
        make_temp_path(p_dst, sizeof(p_dst), "late_out.jpg");
        CHECK(write_file_all(p_src, x, xn));
        job_result r;
        run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, NULL, NULL, 0u,
                0u, &r);
        CHECK(r.status == RFC_ERR_UNSUPPORTED_COMBINATION &&
              r.failing == RFC_STAGE_IMPORT && !file_exists(p_dst));
        remove(p_src);
        free(x);
        free(j);
    }

    /* Strip verification refuses a retained JFIF thumbnail. */
    {
        static uint8_t jfif_t[5 + 9 + 12];
        memcpy(jfif_t, "JFIF\0", 5);
        jfif_t[5] = 1; jfif_t[6] = 2; jfif_t[7] = 1; jfif_t[9] = 72; jfif_t[11] = 72;
        jfif_t[12] = 2; jfif_t[13] = 2;
        mk_defaults(&o, 24u, 16u, 3);
        o.no_jfif = 1;
        o.extra[0] = (mk_marker){JPEG_APP0, jfif_t, (unsigned)sizeof(jfif_t)};
        o.extra_n = 1;
        j = make_jpeg(&o, &n);
        uint8_t *keep = NULL;
        size_t keep_n = 0u;
        CHECK(optimize_direct(j, n, "preserve", NULL, &keep, &keep_n) == RFC_OK);
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator al;
        init_exec(&exec, &st, &al);
        const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
        void *ss = NULL;
        CHECK(t->open_session(&exec, &ss) == RFC_OK);
        const char *det = NULL;
        CHECK(t->compare_markers(ss, &exec, j, n, keep, keep_n, 1, &det) ==
              RFC_ERR_VERIFY_MISMATCH);
        t->cancel_session(ss);
        CHECK(t->compare_markers(ss, &exec, j, n, keep, keep_n, 0, &det) ==
              RFC_ERR_CANCELLED);
        t->close_session(ss);
        rfc_registry_release_jpeg_transcode();
        free(keep);
        free(j);
    }

    /* One component with different AC tables in different bands cannot be
     * re-encoded with its selectors: refused at IMPORT. */
    {
        mk_defaults(&o, 32u, 32u, 3);
        o.progressive = 1;
        j = make_jpeg(&o, &n);
        int patched = 0;
        for (int k = 0; k < 64 && !patched; k++) {
            size_t sp = find_marker(j, n, 0xDAu, k);
            if (sp == 0u) break;
            uint8_t ns = j[sp + 4u];
            uint8_t se = j[sp + 4u + 1u + 2u * ns + 1u];
            if (ns == 1u && j[sp + 5u] == 2u && se > 0u &&
                (j[sp + 6u] & 0x0Fu) == 1u) {
                j[sp + 6u] = (uint8_t)(j[sp + 6u] & 0xF0u); /* AC table 0 */
                patched = 1;
            }
        }
        CHECK(patched);
        char p_src[512], p_dst[512];
        make_temp_path(p_src, sizeof(p_src), "selconf.jpg");
        make_temp_path(p_dst, sizeof(p_dst), "selconf_out.jpg");
        CHECK(write_file_all(p_src, j, n));
        job_result r;
        run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, NULL, "strip",
                0u, 0u, &r);
        CHECK(r.status == RFC_ERR_UNSUPPORTED_COMBINATION &&
              r.failing == RFC_STAGE_IMPORT && !file_exists(p_dst));
        remove(p_src);
        free(j);
    }

    /* Strip verification requires the regenerated JFIF header with the
     * source's density: a missing header or a changed density fails. */
    {
        mk_defaults(&o, 24u, 24u, 1);
        j = make_jpeg(&o, &n);
        uint8_t *st_out = NULL;
        size_t st_n = 0u;
        CHECK(optimize_direct(j, n, "strip", NULL, &st_out, &st_n) == RFC_OK);
        size_t a0 = find_marker(st_out, st_n, 0xE0u, 0);
        CHECK(a0 > 0u);
        size_t seg = 2u + (((size_t)st_out[a0 + 2u] << 8) | st_out[a0 + 3u]);
        uint8_t *nojfif = (uint8_t *)malloc(st_n);
        CHECK(nojfif != NULL);
        memcpy(nojfif, st_out, a0);
        memcpy(nojfif + a0, st_out + a0 + seg, st_n - a0 - seg);
        uint8_t *dens = (uint8_t *)malloc(st_n);
        CHECK(dens != NULL);
        memcpy(dens, st_out, st_n);
        dens[a0 + 4u + 9u] ^= 0x01u; /* X density low byte */
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator al;
        init_exec(&exec, &st, &al);
        const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
        void *ss = NULL;
        CHECK(t->open_session(&exec, &ss) == RFC_OK);
        const char *det = NULL;
        CHECK(t->compare_markers(ss, &exec, j, n, st_out, st_n, 1, &det) == RFC_OK);
        CHECK(t->compare_markers(ss, &exec, j, n, nojfif, st_n - seg, 1, &det) ==
              RFC_ERR_VERIFY_MISMATCH);
        CHECK(t->compare_markers(ss, &exec, j, n, dens, st_n, 1, &det) ==
              RFC_ERR_VERIFY_MISMATCH);
        t->close_session(ss);
        rfc_registry_release_jpeg_transcode();
        free(nojfif);
        free(dens);
        free(st_out);
        free(j);
    }

    /* Not-smaller fallback when the re-encode outgrows the output buffer:
     * preserve publishes the source, strip refuses. */
    {
        mk_defaults(&o, 64u, 64u, 3);
        j = make_jpeg(&o, &n);
        rfc_codecs_jpeg_set_output_slack_for_testing(-(long long)(n / 2u));
        uint8_t *out = NULL;
        size_t out_n = 0u;
        CHECK(optimize_direct(j, n, "preserve", NULL, &out, &out_n) == RFC_OK);
        CHECK(out_n == n && memcmp(out, j, n) == 0);
        free(out);
        CHECK(optimize_direct(j, n, "strip", NULL, NULL, NULL) ==
              RFC_ERR_LIMIT_EXCEEDED);
        rfc_codecs_jpeg_set_output_slack_for_testing(-1);
        free(j);
    }
    printf("jpeg: coefficient_optimization_and_markers ok\n");
}

/* ------------------------------------------------------------------------ */
/* 4. Jobs: defaults, delegation to JPEG XL, file safety                     */
/* ------------------------------------------------------------------------ */

static void test_jobs_defaults_delegation_and_file_safety(void) {
    mk_opts o;
    mk_defaults(&o, 50u, 34u, 3);
    size_t n = 0u;
    uint8_t *j = make_jpeg(&o, &n);
    char p_src[512], p_dst[512], p_png[512], p_jxl[512];
    make_temp_path(p_src, sizeof(p_src), "def.jpg");
    make_temp_path(p_dst, sizeof(p_dst), "def_out.jpg");
    make_temp_path(p_png, sizeof(p_png), "def_out.png");
    make_temp_path(p_jxl, sizeof(p_jxl), "def_out.jxl");
    CHECK(write_file_all(p_src, j, n));
    job_result r;

    /* Explicit pixel-exact to JPEG and PNG to JPEG: refused at submit. */
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, "pixel-exact", NULL,
            0u, 0u, &r);
    CHECK(r.submit == RFC_ERR_UNSUPPORTED_COMBINATION && !file_exists(p_dst));
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_PNG, p_src, p_png, "coefficient-exact",
            NULL, 0u, 64u << 20, &r);
    CHECK(r.submit == RFC_ERR_UNSUPPORTED_COMBINATION && !file_exists(p_png));
    {
        char p_pin[512], p_pout[512];
        make_temp_path(p_pin, sizeof(p_pin), "in.png");
        make_temp_path(p_pout, sizeof(p_pout), "png_to.jpg");
        CHECK(write_file_all(p_pin, j, n)); /* content never read: refused first */
        run_job(RFC_FORMAT_PNG, RFC_FORMAT_JPEG, p_pin, p_pout, NULL, NULL, 0u,
                64u << 20, &r);
        CHECK(r.submit == RFC_ERR_UNSUPPORTED_COMBINATION ||
              r.submit == RFC_ERR_FORMAT_DISAGREEMENT);
        CHECK(!file_exists(p_pout));
        remove(p_pin);
    }

    /* The refusal reason reaches the caller before submission. */
    {
        const char *why = NULL;
        rfc_setting_value px[1] = {{"exactness", "pixel-exact"}};
        CHECK(rfc_job_validate_combination(RFC_FORMAT_PNG, RFC_FORMAT_JPEG, NULL,
                                           0u, &why) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(why && strstr(why, "lossy") && strstr(why, "coefficient-exact"));
        CHECK(rfc_job_validate_combination(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, px,
                                           1u, &why) ==
              RFC_ERR_UNSUPPORTED_COMBINATION && why != NULL);
        CHECK(rfc_job_validate_combination(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, NULL,
                                           0u, &why) == RFC_OK && why == NULL);
    }

    /* JPEG -> PNG with exactness omitted is pixel-exact, and a file job
     * needs no caller working-set estimate: the SOF sizes it. */
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_PNG, p_src, p_png, NULL, NULL, 0u,
            0u, &r);
    CHECK(r.state == RFC_JOB_SUCCEEDED && r.rep.exactness == RFC_EXACT_PIXEL_EXACT);
    remove(p_png);

    /* Byte-identical copy. */
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, "byte-identical",
            NULL, 0u, 0u, &r);
    CHECK(r.state == RFC_JOB_SUCCEEDED);
    size_t cn = 0u;
    uint8_t *copy = read_file_all(p_dst, &cn);
    CHECK(copy && cn == n && memcmp(copy, j, n) == 0);
    free(copy);

    /* An existing destination is never overwritten. */
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, NULL, NULL, 0u, 0u,
            &r);
    CHECK(r.submit == RFC_ERR_DESTINATION_EXISTS ||
          r.status == RFC_ERR_DESTINATION_EXISTS);
    copy = read_file_all(p_dst, &cn);
    CHECK(copy && cn == n && memcmp(copy, j, n) == 0);
    free(copy);
    remove(p_dst);

    /* Original-byte reconstruction is delegated to the JPEG XL adapter. */
    run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG_XL, p_src, p_jxl,
            "original-byte-reconstruction", NULL, 0u, 0u, &r);
    CHECK(r.state == RFC_JOB_SUCCEEDED);
    size_t xn = 0u;
    uint8_t *jx = read_file_all(p_jxl, &xn);
    CHECK(jx != NULL);
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator al;
    init_exec(&exec, &st, &al);
    rfc_encoded rec;
    rfc_metadata rm;
    CHECK(rfc_jxl_reconstruct_jpeg(&exec, jx, xn, &rec, &rm) == RFC_OK);
    CHECK(rec.len == n && memcmp(rec.data, j, n) == 0);
    rfc_encoded_free(&rec);
    rfc_metadata_free(&rm, &al);
    CHECK(st.live == 0u);
    free(jx);
    remove(p_jxl);

    /* Source bytes survive every job. */
    copy = read_file_all(p_src, &cn);
    CHECK(copy && cn == n && memcmp(copy, j, n) == 0);
    free(copy);
    remove(p_src);
    free(j);
    printf("jpeg: jobs_defaults_delegation_and_file_safety ok\n");
}

/* ------------------------------------------------------------------------ */
/* 5. Refusals, provider errors, limits, budget and cancellation             */
/* ------------------------------------------------------------------------ */

static void test_refusals_errors_limits_and_cancel(void) {
    mk_opts o;
    mk_defaults(&o, 40u, 32u, 3);
    size_t n = 0u;
    uint8_t *j = make_jpeg(&o, &n);
    rfc_image_probe pr;
    CHECK(adapter_probe(j, n, NULL, &pr) == RFC_OK);

    /* 12-bit precision and lossless SOF3 by patching the SOF. */
    size_t sof = find_marker(j, n, 0xC0u, 0);
    CHECK(sof > 0u);
    uint8_t *b = (uint8_t *)malloc(n);
    CHECK(b != NULL);
    memcpy(b, j, n);
    b[sof + 4u] = 12u;
    CHECK(adapter_probe(b, n, NULL, &pr) == RFC_ERR_UNSUPPORTED_SAMPLE);
    memcpy(b, j, n);
    b[sof + 1u] = 0xC3u;
    CHECK(adapter_probe(b, n, NULL, &pr) == RFC_ERR_UNSUPPORTED_COMBINATION);
    memcpy(b, j, n);
    b[sof + 1u] = 0xC5u;
    CHECK(adapter_probe(b, n, NULL, &pr) == RFC_ERR_UNSUPPORTED_COMBINATION);
    free(b);

    /* CMYK through libjpeg. */
    {
        struct jpeg_compress_struct c;
        struct jpeg_error_mgr jerr;
        memset(&c, 0, sizeof(c));
        c.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&c);
        unsigned char *mem = NULL;
        unsigned long ml = 0u;
        jpeg_mem_dest(&c, &mem, &ml);
        c.image_width = 8u;
        c.image_height = 8u;
        c.input_components = 4;
        c.in_color_space = JCS_CMYK;
        jpeg_set_defaults(&c);
        jpeg_start_compress(&c, TRUE);
        uint8_t row[32] = {0};
        while (c.next_scanline < 8u) {
            JSAMPROW rp = row;
            jpeg_write_scanlines(&c, &rp, 1);
        }
        jpeg_finish_compress(&c);
        jpeg_destroy_compress(&c);
        CHECK(adapter_probe(mem, ml, NULL, &pr) == RFC_ERR_UNSUPPORTED_SAMPLE);
        free(mem);
    }

    /* Metadata and multi-image refusals. */
    size_t m = 0u;
    uint8_t *x = with_segment(j, n, 0xE1u,
                              (const uint8_t *)"http://ns.adobe.com/xmp/extension/\0ABCDEF", 41u, &m);
    CHECK(adapter_probe(x, m, NULL, &pr) == RFC_ERR_UNSUPPORTED_COMBINATION);
    free(x);
    {
        size_t m1 = 0u;
        uint8_t *e1 = with_segment(j, n, 0xE1u, k_exif_ori6, sizeof(k_exif_ori6), &m1);
        x = with_segment(e1, m1, 0xE1u, k_exif_ori6, sizeof(k_exif_ori6), &m);
        CHECK(adapter_probe(e1, m1, NULL, &pr) == RFC_OK);
        CHECK(adapter_probe(x, m, NULL, &pr) == RFC_ERR_UNSUPPORTED_COMBINATION);
        free(x);
        free(e1);
    }
    x = with_segment(j, n, 0xE2u, (const uint8_t *)"MPF\0II*\0", 8u, &m);
    CHECK(adapter_probe(x, m, NULL, &pr) == RFC_ERR_UNSUPPORTED_COMBINATION);
    free(x);
    {
        uint8_t icc_bad[20];
        memcpy(icc_bad, "ICC_PROFILE\0", 12);
        icc_bad[12] = 1u;
        icc_bad[13] = 2u; /* claims two chunks, carries one */
        memset(icc_bad + 14, 7, 6);
        x = with_segment(j, n, 0xE2u, icc_bad, sizeof(icc_bad), &m);
        CHECK(adapter_probe(x, m, NULL, &pr) == RFC_ERR_UNSUPPORTED_COMBINATION);
        free(x);
    }
    {
        uint8_t *cat = (uint8_t *)malloc(2u * n);
        CHECK(cat != NULL);
        memcpy(cat, j, n);
        memcpy(cat + n, j, n);
        CHECK(adapter_probe(cat, 2u * n, NULL, &pr) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        memset(cat + n, 0, n);
        CHECK(adapter_probe(cat, 2u * n, NULL, &pr) == RFC_OK); /* zero padding */
        free(cat);
    }
    CHECK(adapter_probe(j, n / 2u, NULL, &pr) == RFC_ERR_TRUNCATED_INPUT);
    {
        uint8_t garbage[64];
        memset(garbage, 0x5A, sizeof(garbage));
        CHECK(adapter_probe(garbage, sizeof(garbage), NULL, &pr) ==
              RFC_ERR_INVALID_ARGUMENT);
    }
    /* Scan flood: more than 1000 SOS segments refused before libjpeg. */
    {
        size_t sos = find_marker(j, n, 0xDAu, 0);
        CHECK(sos > 0u);
        size_t sos_len = ((size_t)j[sos + 2u] << 8 | j[sos + 3u]) + 2u;
        size_t fl = sos + 1001u * sos_len + 2u;
        uint8_t *flood = (uint8_t *)malloc(fl);
        CHECK(flood != NULL);
        memcpy(flood, j, sos);
        for (size_t k = 0u; k < 1001u; k++) {
            memcpy(flood + sos + k * sos_len, j + sos, sos_len);
        }
        flood[fl - 2u] = 0xFFu;
        flood[fl - 1u] = 0xD9u;
        CHECK(adapter_probe(flood, fl, NULL, &pr) == RFC_ERR_LIMIT_EXCEEDED);
        free(flood);
    }

    /* Provider error boundary: damaged entropy data with a valid EOI fails
     * without publication; a valid job right after succeeds in-process. */
    {
        uint8_t *dam = (uint8_t *)malloc(n);
        CHECK(dam != NULL);
        memcpy(dam, j, n);
        size_t sos = find_marker(j, n, 0xDAu, 0);
        size_t data0 = sos + 2u + (((size_t)j[sos + 2u] << 8) | j[sos + 3u]);
        for (size_t k = data0 + 10u; k < data0 + 40u && k + 2u < n; k++) {
            dam[k] = (dam[k] == 0xFFu) ? 0xFFu : (uint8_t)(dam[k] ^ 0xA5u);
            if (dam[k] == 0xFFu) dam[k] = 0xFEu;
        }
        uint8_t *px = NULL;
        rfc_status ds = adapter_decode(dam, n, &px, NULL, NULL, NULL);
        CHECK(ds == RFC_ERR_INVALID_ARGUMENT || ds == RFC_ERR_TRUNCATED_INPUT);
        CHECK(px == NULL);
        char p_bad[512], p_out[512], p_good[512], p_out2[512];
        make_temp_path(p_bad, sizeof(p_bad), "dam.jpg");
        make_temp_path(p_out, sizeof(p_out), "dam.png");
        make_temp_path(p_good, sizeof(p_good), "good.jpg");
        make_temp_path(p_out2, sizeof(p_out2), "good.png");
        CHECK(write_file_all(p_bad, dam, n));
        CHECK(write_file_all(p_good, j, n));
        job_result r;
        run_job(RFC_FORMAT_JPEG, RFC_FORMAT_PNG, p_bad, p_out, NULL, NULL, 0u,
                64u << 20, &r);
        CHECK(r.submit == RFC_OK && r.state != RFC_JOB_SUCCEEDED &&
              !file_exists(p_out));
        run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_bad, p_out, NULL, NULL, 0u,
                0u, &r);
        CHECK(r.state != RFC_JOB_SUCCEEDED && !file_exists(p_out));
        run_job(RFC_FORMAT_JPEG, RFC_FORMAT_PNG, p_good, p_out2, NULL, NULL, 0u,
                64u << 20, &r);
        CHECK(r.state == RFC_JOB_SUCCEEDED && file_exists(p_out2));
        remove(p_bad);
        remove(p_good);
        remove(p_out2);
        free(dam);
    }

    /* Limits. */
    {
        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_width = 16u;
        CHECK(adapter_probe(j, n, &lim, &pr) == RFC_ERR_LIMIT_EXCEEDED);
        size_t mn = 0u;
        uint8_t *mj = make_meta_jpeg(4000u, 0, 1, &mn, NULL);
        rfc_limits_default(&lim);
        lim.max_metadata_bytes = 1000u;
        CHECK(adapter_probe(mj, mn, &lim, &pr) == RFC_ERR_LIMIT_EXCEEDED);
        free(mj);
    }

    /* Budget: tight budgets refuse and release everything; a calibrated one
     * succeeds. Large markers on a tiny image and a progressive image. */
    {
        static uint8_t big[65000];
        memset(big, 'm', sizeof(big));
        mk_opts lo;
        mk_defaults(&lo, 16u, 16u, 3);
        for (int k = 0; k < 14; k++) {
            lo.extra[k] = (mk_marker){JPEG_COM, big, (unsigned)sizeof(big)};
        }
        lo.extra_n = 14;
        size_t ln = 0u;
        uint8_t *lj = make_jpeg(&lo, &ln);
        CHECK(ln > 900000u);
        rfc_budget *bt = rfc_budget_create(256u * 1024u);
        CHECK(optimize_direct(lj, ln, "preserve", bt, NULL, NULL) ==
              RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_stats bs;
        rfc_budget_stats_get(bt, &bs);
        CHECK(bs.current_allocated_bytes == 0u);
        rfc_budget_destroy(bt);
        rfc_budget *bg = rfc_budget_create(16u * 1024u * 1024u);
        uint8_t *lo_out = NULL;
        size_t lo_n = 0u;
        CHECK(optimize_direct(lj, ln, "preserve", bg, &lo_out, &lo_n) == RFC_OK);
        rfc_budget_stats_get(bg, &bs);
        CHECK(bs.current_allocated_bytes == 0u);
        rfc_budget_destroy(bg);
        CHECK(coeffs_equal(lj, ln, lo_out, lo_n) == RFC_OK);
        free(lo_out);
        free(lj);

        mk_opts po;
        mk_defaults(&po, 512u, 512u, 3);
        po.progressive = 1;
        size_t pn = 0u;
        uint8_t *pj = make_jpeg(&po, &pn);
        bt = rfc_budget_create(1024u * 1024u);
        uint8_t *px = NULL;
        CHECK(adapter_decode(pj, pn, &px, NULL, NULL, bt) == RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_stats_get(bt, &bs);
        CHECK(bs.current_allocated_bytes == 0u && px == NULL);
        rfc_budget_destroy(bt);
        bg = rfc_budget_create(64u * 1024u * 1024u);
        CHECK(adapter_decode(pj, pn, &px, NULL, NULL, bg) == RFC_OK);
        rfc_budget_stats_get(bg, &bs);
        CHECK(bs.current_allocated_bytes == 0u);
        rfc_budget_destroy(bg);
        free(px);
        free(pj);
    }

    /* Truncated entropy data with an EOI present is truncation. */
    {
        size_t sos = find_marker(j, n, 0xDAu, 0);
        size_t data0 = sos + 2u + (((size_t)j[sos + 2u] << 8) | j[sos + 3u]);
        size_t cut = data0 + (n - data0) / 3u;
        uint8_t *tr = (uint8_t *)malloc(cut + 2u);
        CHECK(tr != NULL);
        memcpy(tr, j, cut);
        if (tr[cut - 1u] == 0xFFu) tr[cut - 1u] = 0x00u;
        tr[cut] = 0xFFu;
        tr[cut + 1u] = 0xD9u;
        CHECK(adapter_decode(tr, cut + 2u, NULL, NULL, NULL, NULL) ==
              RFC_ERR_TRUNCATED_INPUT);
        free(tr);
    }

    /* Every allocation failure is clean: no crash, no leak. */
    for (int op = 0; op < 3; op++) {
        for (int k = 0; k < 12; k++) {
            g_fail_after = k;
            rfc_exec_context exec;
            tracked_alloc_state st;
            rfc_allocator al;
            init_exec(&exec, &st, &al);
            al.alloc = fail_alloc;
            rfc_status fs = RFC_OK;
            if (op == 0) {
                const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
                void *ss = NULL;
                CHECK(t->open_session(&exec, &ss) == RFC_OK);
                struct rfc_jpeg_coeff_image *ci = NULL;
                rfc_metadata m;
                fs = t->read_coefficients(ss, &exec, j, n, &ci, &m);
                if (fs == RFC_OK) {
                    t->free_coefficients(ss, &exec, ci);
                    rfc_metadata_free(&m, &al);
                }
                t->close_session(ss);
                rfc_registry_release_jpeg_transcode();
            } else if (op == 1) {
                const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
                void *ss = NULL;
                CHECK(t->open_session(&exec, &ss) == RFC_OK);
                rfc_encoded enc;
                fs = t->optimize_jpeg(ss, &exec, j, n, NULL, 0u, &enc);
                if (fs == RFC_OK) rfc_encoded_free(&enc);
                t->close_session(ss);
                rfc_registry_release_jpeg_transcode();
            } else {
                const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_JPEG);
                void *ss = NULL;
                CHECK(a->open_session(&exec, &ss) == RFC_OK);
                rfc_frame *fr = NULL;
                size_t fc = 0u;
                fs = a->decode_session(ss, &exec, j, n, NULL, 0u, &fr, &fc, NULL);
                if (fs == RFC_OK) {
                    rfc_frame_free(&fr[0]);
                    al.free(al.ctx, fr);
                }
                a->close_session(ss);
                rfc_registry_release_session(RFC_FORMAT_JPEG);
            }
            CHECK(fs == RFC_OK || fs == RFC_ERR_OUT_OF_MEMORY);
            CHECK(st.live == 0u);
        }
        g_fail_after = -1;
    }

    /* A large flat image: the default reservation comes from the SOF, not the
     * tiny compressed size. */
    {
        mk_opts fo;
        mk_defaults(&fo, 4096u, 4096u, 1);
        fo.flat = 1;
        size_t fn = 0u;
        uint8_t *fj = make_jpeg(&fo, &fn);
        CHECK(fn < 2u * 1024u * 1024u);
        char p_src[512], p_dst[512];
        make_temp_path(p_src, sizeof(p_src), "flat.jpg");
        make_temp_path(p_dst, sizeof(p_dst), "flat_out.jpg");
        CHECK(write_file_all(p_src, fj, fn));
        job_result r;
        run_job(RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, p_src, p_dst, NULL, NULL, 0u,
                0u, &r);
        CHECK(r.submit == RFC_OK && r.state == RFC_JOB_SUCCEEDED);
        remove(p_src);
        remove(p_dst);
        free(fj);
    }

    /* Cancellation before work. */
    {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator al;
        init_exec(&exec, &st, &al);
        const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_JPEG);
        void *s = NULL;
        CHECK(a->open_session(&exec, &s) == RFC_OK);
        a->cancel_session(s);
        rfc_frame *fr = NULL;
        size_t fc = 0u;
        CHECK(a->decode_session(s, &exec, j, n, NULL, 0u, &fr, &fc, NULL) ==
              RFC_ERR_CANCELLED);
        a->close_session(s);
        rfc_registry_release_session(RFC_FORMAT_JPEG);
        const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
        CHECK(t->open_session(&exec, &s) == RFC_OK);
        t->cancel_session(s);
        rfc_encoded enc;
        CHECK(t->optimize_jpeg(s, &exec, j, n, NULL, 0u, &enc) == RFC_ERR_CANCELLED);
        t->close_session(s);
        rfc_registry_release_jpeg_transcode();
        CHECK(st.live == 0u);
    }
    free(j);
    printf("jpeg: refusals_errors_limits_and_cancel ok\n");
}

/* ------------------------------------------------------------------------ */
/* CLI harness for tests/converter/test_jpeg.py                              */
/* ------------------------------------------------------------------------ */

static void wr32(FILE *fp, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 24)};
    CHECK(fwrite(b, 1u, 4u, fp) == 4u);
}

static rfc_format_id parse_fmt(const char *s) {
    const rfc_capability *c = rfc_registry_find_name(s);
    return c ? c->id : (rfc_format_id)0;
}

static int cli_main(int argc, char **argv) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_OK);
    CHECK(rfc_codecs_register_jpeg() == RFC_OK);
    const char *sub = argv[2];
    if (strcmp(sub, "probe") == 0 && argc == 4) {
        size_t n = 0u;
        uint8_t *b = read_file_all(argv[3], &n);
        if (!b) b = (uint8_t *)calloc(1u, 1u);
        rfc_image_probe pr;
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator al;
        init_exec(&exec, &st, &al);
        const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_JPEG);
        void *s = NULL;
        CHECK(a->open_session(&exec, &s) == RFC_OK);
        memset(&pr, 0, sizeof(pr));
        rfc_status r = a->probe_session(s, &exec, b, n, &pr);
        a->close_session(s);
        rfc_registry_release_session(RFC_FORMAT_JPEG);
        printf("STATUS=%s WIDTH=%u HEIGHT=%u SAMPLE=%u ICC_LEN=%zu EXIF_LEN=%zu "
               "XMP_LEN=%zu ORIENTATION=%u\n",
               rfc_status_name(r), pr.first_frame.width, pr.first_frame.height,
               (unsigned)pr.first_frame.format, pr.container_meta.icc_len,
               pr.container_meta.exif_len, pr.container_meta.xmp_len,
               (unsigned)pr.container_meta.orientation);
        rfc_metadata_free(&pr.container_meta, &al);
        CHECK(st.live == 0u);
        free(b);
        return r == RFC_OK ? 0 : 2;
    }
    if (strcmp(sub, "dump") == 0 && argc == 5) {
        size_t n = 0u;
        uint8_t *b = read_file_all(argv[3], &n);
        if (!b) b = (uint8_t *)calloc(1u, 1u);
        uint8_t *px = NULL;
        rfc_layout lay;
        memset(&lay, 0, sizeof(lay));
        rfc_status r = adapter_decode(b, n, &px, &lay, NULL, NULL);
        free(b);
        if (r == RFC_OK) {
            size_t ch = lay.format == RFC_SAMPLE_GRAY8 ? 1u : 3u;
            CHECK(write_file_all(argv[4], px, (size_t)lay.width * lay.height * ch));
            free(px);
        }
        printf("STATUS=%s WIDTH=%u HEIGHT=%u SAMPLE=%u\n", rfc_status_name(r),
               lay.width, lay.height, (unsigned)lay.format);
        return r == RFC_OK ? 0 : 2;
    }
    if (strcmp(sub, "coeffs") == 0 && argc == 5) {
        size_t n = 0u;
        uint8_t *b = read_file_all(argv[3], &n);
        if (!b) b = (uint8_t *)calloc(1u, 1u);
        rfc_jpeg_coeff_image *ci = NULL;
        rfc_status r = read_coeffs(b, n, &ci);
        free(b);
        if (r == RFC_OK) {
            FILE *fp = fopen(argv[4], "wb");
            CHECK(fp != NULL);
            CHECK(fwrite("RFJC", 1u, 4u, fp) == 4u);
            wr32(fp, ci->width);
            wr32(fp, ci->height);
            wr32(fp, (uint32_t)ci->progressive);
            wr32(fp, (uint32_t)ci->arithmetic_coding);
            wr32(fp, (uint32_t)ci->baseline);
            wr32(fp, ci->component_count);
            for (int t = 0; t < 4; t++) {
                wr32(fp, (uint32_t)ci->quant_tables[t].populated);
                for (int k = 0; k < 64; k++) wr32(fp, ci->quant_tables[t].values[k]);
            }
            for (uint32_t c = 0u; c < ci->component_count; c++) {
                const rfc_jpeg_component_spec *cs = &ci->components[c];
                wr32(fp, cs->component_id);
                wr32(fp, cs->h_samp_factor);
                wr32(fp, cs->v_samp_factor);
                wr32(fp, cs->quant_table_idx);
                wr32(fp, cs->dc_table_idx);
                wr32(fp, cs->ac_table_idx);
                wr32(fp, cs->blocks_wide);
                wr32(fp, cs->blocks_high);
                size_t cnt = (size_t)cs->blocks_wide * cs->blocks_high * 64u;
                for (size_t k = 0u; k < cnt; k++) {
                    int16_t v = cs->dct_blocks[k];
                    uint8_t vb[2] = {(uint8_t)((uint16_t)v & 0xFFu),
                                     (uint8_t)((uint16_t)v >> 8)};
                    CHECK(fwrite(vb, 1u, 2u, fp) == 2u);
                }
            }
            CHECK(fclose(fp) == 0);
            free_coeffs(ci);
        }
        printf("STATUS=%s\n", rfc_status_name(r));
        return r == RFC_OK ? 0 : 2;
    }
    if (strcmp(sub, "convert") == 0 && argc == 10) {
        rfc_format_id sf = parse_fmt(argv[3]);
        rfc_format_id df = parse_fmt(argv[4]);
        if (!sf || !df) return 2;
        const char *ex = strcmp(argv[7], "-") == 0 ? NULL : argv[7];
        const char *meta = strcmp(argv[8], "-") == 0 ? NULL : argv[8];
        uint32_t ack = (uint32_t)strtoul(argv[9], NULL, 0);
        job_result r;
        uint64_t ws = (sf == RFC_FORMAT_JPEG) ? 0u : 256u << 20;
        run_job(sf, df, argv[5], argv[6], ex, meta, ack, ws, &r);
        if (r.submit != RFC_OK) {
            rfc_setting_value sv[2];
            size_t svn = 0u;
            if (ex) { sv[svn].id = "exactness"; sv[svn].value = ex; svn++; }
            if (meta) { sv[svn].id = "metadata"; sv[svn].value = meta; svn++; }
            const char *why = NULL;
            (void)rfc_job_validate_combination(sf, df, svn ? sv : NULL, svn, &why);
            fprintf(stderr, "REASON=%s\n", why ? why : "none");
        }
        printf("SUBMIT=%s STATUS=%s STATE=%u EXACTNESS=%s META_OUTCOME=%s "
               "STAGE=%u\n",
               rfc_status_name(r.submit), rfc_status_name(r.status),
               (unsigned)r.state,
               r.has_rep ? rfc_exactness_class_name(r.rep.exactness) : "none",
               r.has_rep ? rfc_verify_meta_outcome_name(r.rep.meta_outcome) : "none",
               (unsigned)r.failing);
        return (r.submit == RFC_OK && r.state == RFC_JOB_SUCCEEDED) ? 0 : 2;
    }
    if (strcmp(sub, "make") == 0 && argc == 14) {
        /* make <out> <w> <h> <ncomp> <hv e.g. 22,11,11> <quality> <prog> <arith>
         *      <restart> <optimize> <seed> */
        mk_opts o;
        mk_defaults(&o, (uint32_t)strtoul(argv[4], NULL, 10),
                    (uint32_t)strtoul(argv[5], NULL, 10), atoi(argv[6]));
        const char *hv = argv[7];
        for (int c = 0; c < o.ncomp && hv[0] && hv[1]; c++) {
            o.hs[c] = hv[0] - '0';
            o.vs[c] = hv[1] - '0';
            hv += 2;
            if (*hv == ',') hv++;
        }
        o.quality = atoi(argv[8]);
        o.progressive = atoi(argv[9]);
        o.arith = atoi(argv[10]);
        o.restart = atoi(argv[11]);
        o.optimize = atoi(argv[12]);
        o.seed = (uint32_t)strtoul(argv[13], NULL, 10);
        size_t n = 0u;
        uint8_t *j = make_jpeg(&o, &n);
        CHECK(write_file_all(argv[3], j, n));
        free(j);
        printf("STATUS=ok BYTES=%zu\n", n);
        return 0;
    }
    if (strcmp(sub, "budget-peak") == 0 && argc == 6) {
        /* budget-peak <decode|optimize|coeffs> <file> <budget-bytes> */
        size_t n = 0u;
        uint8_t *b = read_file_all(argv[4], &n);
        CHECK(b != NULL);
        rfc_budget *bud = rfc_budget_create(strtoull(argv[5], NULL, 10));
        CHECK(bud != NULL);
        rfc_status r;
        if (strcmp(argv[3], "noop") == 0) {
            r = RFC_OK; /* the CLI's own footprint, no adapter call */
        } else if (strcmp(argv[3], "decode") == 0) {
            uint8_t *px = NULL;
            r = adapter_decode(b, n, &px, NULL, NULL, bud);
            free(px);
        } else if (strcmp(argv[3], "optimize") == 0) {
            uint8_t *out = NULL;
            size_t on = 0u;
            r = optimize_direct(b, n, "preserve", bud, &out, &on);
            free(out);
        } else {
            rfc_exec_context exec;
            tracked_alloc_state st;
            rfc_allocator al;
            init_exec(&exec, &st, &al);
            rfc_allocator ba;
            rfc_budget_allocator(bud, &ba);
            exec.allocator = &ba;
            exec.budget = bud;
            const rfc_jpeg_transcode_adapter *t = rfc_registry_acquire_jpeg_transcode();
            void *s = NULL;
            CHECK(t->open_session(&exec, &s) == RFC_OK);
            struct rfc_jpeg_coeff_image *ci = NULL;
            rfc_metadata m;
            r = t->read_coefficients(s, &exec, b, n, &ci, &m);
            if (r == RFC_OK) {
                t->free_coefficients(s, &exec, ci);
                rfc_metadata_free(&m, &ba);
            }
            t->close_session(s);
            rfc_registry_release_jpeg_transcode();
        }
        rfc_budget_stats bs;
        rfc_budget_stats_get(bud, &bs);
        printf("STATUS=%s PEAK_BUDGET=%llu CURR_ALLOC=%llu\n", rfc_status_name(r),
               (unsigned long long)bs.peak_allocated_bytes,
               (unsigned long long)bs.current_allocated_bytes);
        rfc_budget_destroy(bud);
        free(b);
        return r == RFC_OK ? 0 : 2;
    }
    return 2;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "cli") == 0) {
        return cli_main(argc, argv);
    }
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_OK);
    test_registration_and_upstream_contract();
    test_import_pinned_decoder_and_metadata();
    test_coefficient_optimization_and_markers();
    test_jobs_defaults_delegation_and_file_safety();
    test_refusals_errors_limits_and_cancel();
    CHECK(rfc_codecs_unregister_jpeg() == RFC_OK);
    printf("jpeg: all checks passed (%d checks)\n", g_checks);
    return 0;
}
