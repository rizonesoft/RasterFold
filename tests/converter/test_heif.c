/* C11 suite + CLI harness for D10 T02 s7: import-only HEIF and HEIC through
 * the Windows Imaging Component.
 *   - every build: registration as one transaction, the registry rows, write
 *     refusal, and the shared ISOBMFF reader on hand-built container
 *     structures (one picture, grid, second image, thumbnail, derived,
 *     sequence, alpha and other auxiliaries, depth, colour boxes, Exif/XMP,
 *     bounds, reader failures);
 *   - non-Windows builds: the named refusal through the adapter and a job;
 *   - Windows: fixtures written by WIC's HEIF encoder in this harness only,
 *     decoded by the adapter and compared with a second fresh WIC decode,
 *     orientation 1..8 against every transform of the pattern, ICC, Exif and
 *     XMP under both metadata policies, a second image, the missing-component
 *     paths, HEIC to PNG jobs without hints, limits, budget, cancellation. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#else
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <psapi.h>
#include <process.h>
#include <wincodec.h>
#endif

#include "rfc_codec.h"
#include "rfc_codecs.h"
#include "rfc_heif_scan.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <math.h>
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
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct tracked {
    size_t live;
} tracked;

static void *t_alloc(void *ctx, size_t sz) {
    tracked *st = (tracked *)ctx;
    void *p = malloc(sz ? sz : 1u);
    if (p && st) st->live++;
    return p;
}

static void t_free(void *ctx, void *p) {
    tracked *st = (tracked *)ctx;
    if (p) {
        if (st && st->live) st->live--;
        free(p);
    }
}

static void init_exec(rfc_exec_context *e, tracked *st, rfc_allocator *al) {
    memset(e, 0, sizeof(*e));
    memset(st, 0, sizeof(*st));
    al->alloc = t_alloc;
    al->free = t_free;
    al->ctx = st;
    rfc_limits_default(&e->limits);
    e->codec_threads = 1u;
    e->allocator = al;
}

static uint8_t *read_file_all(const char *path, size_t *n) {
    *n = 0u;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(len > 0 ? (size_t)len : 1u);
    if (!b || (len > 0 && fread(b, 1u, (size_t)len, fp) != (size_t)len)) {
        free(b);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *n = (size_t)len;
    return b;
}

static int write_file_all(const char *path, const uint8_t *d, size_t n) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    int ok = (n == 0u || fwrite(d, 1u, n, fp) == n);
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

static void temp_path(char *buf, size_t cap, const char *tag) {
    static unsigned counter = 0u;
    counter++;
#ifdef _WIN32
    snprintf(buf, cap, "rfc_heif_%d_%u_%s", (int)_getpid(), counter, tag);
#else
    snprintf(buf, cap, "/tmp/rfc_heif_%ld_%u_%s", (long)getpid(), counter, tag);
#endif
    remove(buf);
}

typedef struct job_result {
    rfc_status submit, status;
    rfc_job_state state;
    rfc_job_stage failing;
    uint64_t budget_limit, budget_peak;
    char detail[RFC_JOB_DETAIL_MAX];
} job_result;

static void run_job_full(rfc_format_id sf, rfc_format_id df, const char *src,
                         const char *dst, const char *meta,
                         const rfc_limits *lim, job_result *out) {
    memset(out, 0, sizeof(*out));
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);
    rfc_io_source si;
    rfc_io_sink so;
    rfc_io_source_init_path(&si, src, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&so, dst, RFC_EXPORT_REFUSE_EXISTING);
    rfc_setting_value sv[1];
    size_t n = 0u;
    if (meta) {
        sv[0].id = "metadata";
        sv[0].value = meta;
        n = 1u;
    }
    rfc_job_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_format = sf;
    sp.target_format = df;
    sp.io_source = &si;
    sp.io_sink = &so;
    sp.settings = n ? sv : NULL;
    sp.setting_count = n;
    sp.limits_override = lim;
    rfc_job *j = NULL;
    out->submit = rfc_scheduler_submit(sched, NULL, &sp, &j);
    if (out->submit == RFC_OK) {
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        out->status = snap.status;
        out->state = snap.state;
        out->failing = snap.failing_stage;
        out->budget_limit = snap.budget.limit_bytes;
        out->budget_peak = snap.budget.peak_allocated_bytes;
        memcpy(out->detail, snap.detail, sizeof(out->detail));
        rfc_job_release(j);
    }
    rfc_scheduler_destroy(sched);
}

#define CHECK_JOB_OK(r)                                                        \
    do {                                                                       \
        if ((r).submit != RFC_OK || (r).state != RFC_JOB_SUCCEEDED) {          \
            fprintf(stderr, "job: submit=%s status=%s stage=%u detail=%s\n",   \
                    rfc_status_name((r).submit), rfc_status_name((r).status),  \
                    (unsigned)(r).failing, (r).detail);                        \
        }                                                                      \
        CHECK((r).submit == RFC_OK && (r).state == RFC_JOB_SUCCEEDED);         \
    } while (0)

/* Decode through the registered adapter; px is packed RGB. */
static rfc_status adapter_decode(rfc_format_id f, const uint8_t *b, size_t n,
                                 const char *meta, const rfc_limits *lim,
                                 rfc_budget *bud, rfc_frame *info,
                                 uint8_t **out_px) {
    rfc_exec_context e;
    tracked st;
    rfc_allocator al;
    init_exec(&e, &st, &al);
    if (lim) e.limits = *lim;
    rfc_allocator ba;
    if (bud) {
        rfc_budget_allocator(bud, &ba);
        e.allocator = &ba;
        e.budget = bud;
    }
    const rfc_session_adapter *a = rfc_registry_acquire_session(f);
    CHECK(a != NULL);
    void *s = NULL;
    CHECK(a->open_session(&e, &s) == RFC_OK);
    rfc_setting_value sv[1] = {{"metadata", meta ? meta : "preserve"}};
    rfc_frame *fr = NULL;
    size_t fc = 0u;
    rfc_status r = a->decode_session(s, &e, b, n, sv, 1u, &fr, &fc, NULL);
    a->close_session(s);
    rfc_registry_release_session(f);
    if (r == RFC_OK) {
        CHECK(fc == 1u);
        if (info) {
            *info = fr[0];
            info->data = NULL;
            info->metadata.icc = NULL;
            info->metadata.exif = NULL;
            info->metadata.xmp = NULL;
        }
        if (out_px) {
            size_t rb = (size_t)fr[0].layout.width * 3u;
            uint8_t *px = (uint8_t *)malloc(rb * fr[0].layout.height + 1u);
            CHECK(px != NULL);
            for (uint32_t y = 0u; y < fr[0].layout.height; y++) {
                memcpy(px + (size_t)y * rb, fr[0].data + (size_t)y * fr[0].layout.stride_bytes, rb);
            }
            *out_px = px;
        }
        rfc_frame_free(&fr[0]);
        e.allocator->free(e.allocator->ctx, fr);
    }
    if (!bud) CHECK(st.live == 0u);
    return r;
}

/* ------------------------------------------------------------------------ */
/* Hand-built ISOBMFF structures for the shared reader                       */
/* ------------------------------------------------------------------------ */

typedef struct buf {
    uint8_t d[262144];
    size_t n;
} buf;

static void put8(buf *b, uint32_t v) { b->d[b->n++] = (uint8_t)v; }
static void put16(buf *b, uint32_t v) { put8(b, v >> 8); put8(b, v); }
static void put32(buf *b, uint32_t v) { put16(b, v >> 16); put16(b, v); }
static void put4cc(buf *b, const char *t) { memcpy(b->d + b->n, t, 4); b->n += 4u; }
static void putbytes(buf *b, const void *p, size_t n) { memcpy(b->d + b->n, p, n); b->n += n; }

static size_t box_begin(buf *b, const char *type) {
    size_t at = b->n;
    put32(b, 0u);
    put4cc(b, type);
    return at;
}

static size_t fullbox_begin(buf *b, const char *type, uint32_t version, uint32_t flags) {
    size_t at = box_begin(b, type);
    put8(b, version);
    put8(b, flags >> 16);
    put8(b, flags >> 8);
    put8(b, flags);
    return at;
}

static void box_end(buf *b, size_t at) {
    uint32_t sz = (uint32_t)(b->n - at);
    b->d[at] = (uint8_t)(sz >> 24);
    b->d[at + 1] = (uint8_t)(sz >> 16);
    b->d[at + 2] = (uint8_t)(sz >> 8);
    b->d[at + 3] = (uint8_t)sz;
}

typedef struct hspec {
    const char *primary_type; /* "hvc1", "grid", "iden", "av01" */
    int grid_tiles;
    int second_image;
    int thumbnail;
    int alpha_aux, depth_aux;
    int pixi_bits;   /* 0: no pixi */
    int hvcc_depth;  /* 0: no hvcC */
    uint32_t w, h;
    int irot, imir;
    int icc_len;     /* 0: none */
    int nclx_primaries, nclx_transfer; /* 0: none */
    int exif, xmp;
    int moov;
    int extra_items; /* plain items to exceed bounds */
    int ftyp_late;
    uint32_t clap_w, clap_h, clap_d; /* clean aperture N/D; 0: none */
    int32_t clap_hon, clap_von;      /* offsets, over clap_od */
    uint32_t clap_od;                /* 0: 1 */
    int ipma_v1;        /* ipma version 1 with flags 1 (32-bit IDs, 16-bit entries) */
    int split_ipma;     /* the ispe association in a second ipma box */
    int nested_iprp;    /* ipco and ipma one iprp deeper (not permitted) */
    int pixi_c1;        /* pixi second-channel depth (0: as pixi_bits) */
    int hvcc_chroma;    /* hvcC chroma depth (0: as hvcc_depth) */
    int grid_mixed;     /* the last grid tile is av01 */
    int grid_deep_tile; /* the last grid tile has its own 10-bit hvcC */
    int moov_only;      /* ftyp and moov, no meta */
    int unterminated;   /* the primary's item name has no terminator */
    uint32_t tile_w, tile_h; /* grid tile size (0: half the grid each way) */
    int tile_mismatch;  /* the last tile has another size */
    int meta_to_eof;    /* meta is the last box, its size 0 (to the end) */
} hspec;

static void hspec_default(hspec *s) {
    memset(s, 0, sizeof(*s));
    s->primary_type = "hvc1";
    s->w = 64u;
    s->h = 48u;
    s->hvcc_depth = 8;
}

typedef struct prop_ref {
    uint32_t item;
    uint32_t index;
} prop_ref;

static size_t build_heif(const hspec *s, uint8_t *out) {
    buf *b = (buf *)calloc(1u, sizeof(buf));
    CHECK(b != NULL);
    if (s->ftyp_late) {
        size_t f = box_begin(b, "free");
        box_end(b, f);
    }
    size_t ft = box_begin(b, "ftyp");
    put4cc(b, "heic");
    put32(b, 0u);
    put4cc(b, "mif1");
    put4cc(b, "heic");
    box_end(b, ft);
    if (s->moov_only) {
        size_t mv = box_begin(b, "moov");
        box_end(b, mv);
        size_t n0 = b->n;
        memcpy(out, b->d, n0);
        free(b);
        return n0;
    }
    size_t meta = fullbox_begin(b, "meta", 0u, 0u);
    if (s->meta_to_eof) {
        /* no mdat after it; the size field is set to 0 below */
    }
    size_t hdlr = fullbox_begin(b, "hdlr", 0u, 0u);
    put32(b, 0u);
    put4cc(b, "pict");
    put32(b, 0u); put32(b, 0u); put32(b, 0u);
    put8(b, 0u);
    box_end(b, hdlr);
    size_t pitm = fullbox_begin(b, "pitm", 0u, 0u);
    put16(b, 1u);
    box_end(b, pitm);
    /* items */
    typedef struct it { uint32_t id; const char *type; int mime_xmp; } it;
    it items[4300];
    int ni = 0;
    items[ni++] = (it){1u, s->primary_type, 0};
    for (int t = 0; t < s->grid_tiles; t++)
        items[ni++] = (it){10u + (uint32_t)t, (s->grid_mixed && t == s->grid_tiles - 1) ? "av01" : "hvc1", 0};
    if (s->second_image) items[ni++] = (it){2u, "hvc1", 0};
    if (s->thumbnail) items[ni++] = (it){3u, "hvc1", 0};
    if (s->alpha_aux) items[ni++] = (it){4u, "hvc1", 0};
    if (s->depth_aux) items[ni++] = (it){5u, "hvc1", 0};
    if (s->exif) items[ni++] = (it){6u, "Exif", 0};
    if (s->xmp) items[ni++] = (it){7u, "mime", 1};
    if (!strcmp(s->primary_type, "iden")) items[ni++] = (it){8u, "hvc1", 0};
    for (int k = 0; k < s->extra_items; k++) items[ni++] = (it){100u + (uint32_t)k, "Exif", 0};
    size_t iinf = fullbox_begin(b, "iinf", 0u, 0u);
    put16(b, (uint32_t)ni);
    for (int k = 0; k < ni; k++) {
        size_t infe = fullbox_begin(b, "infe", 2u, 0u);
        put16(b, items[k].id);
        put16(b, 0u);
        put4cc(b, items[k].type);
        if (s->unterminated && k == ni - 1) {
            put8(b, 'x'); /* name runs to the end of the box */
            box_end(b, infe);
            continue;
        }
        put8(b, 0u); /* name */
        if (items[k].mime_xmp) {
            putbytes(b, "application/rdf+xml", 20u);
        }
        box_end(b, infe);
    }
    box_end(b, iinf);
    /* references */
    size_t iref = fullbox_begin(b, "iref", 0u, 0u);
    if (s->grid_tiles) {
        size_t r = box_begin(b, "dimg");
        put16(b, 1u);
        put16(b, (uint32_t)s->grid_tiles);
        for (int t = 0; t < s->grid_tiles; t++) put16(b, 10u + (uint32_t)t);
        box_end(b, r);
    }
    if (!strcmp(s->primary_type, "iden")) {
        size_t r = box_begin(b, "dimg");
        put16(b, 1u); put16(b, 1u); put16(b, 8u);
        box_end(b, r);
    }
    if (s->thumbnail) {
        size_t r = box_begin(b, "thmb");
        put16(b, 3u); put16(b, 1u); put16(b, 1u);
        box_end(b, r);
    }
    if (s->alpha_aux) {
        size_t r = box_begin(b, "auxl");
        put16(b, 4u); put16(b, 1u); put16(b, 1u);
        box_end(b, r);
    }
    if (s->depth_aux) {
        size_t r = box_begin(b, "auxl");
        put16(b, 5u); put16(b, 1u); put16(b, 1u);
        box_end(b, r);
    }
    if (s->exif) {
        size_t r = box_begin(b, "cdsc");
        put16(b, 6u); put16(b, 1u); put16(b, 1u);
        box_end(b, r);
    }
    box_end(b, iref);
    /* properties */
    size_t iprp = box_begin(b, "iprp");
    size_t iprp_inner = s->nested_iprp ? box_begin(b, "iprp") : 0u;
    size_t ipco = box_begin(b, "ipco");
    uint32_t np = 0u;
    prop_ref refs[64];
    int nr = 0;
    /* 1: ispe of the primary */
    size_t p = fullbox_begin(b, "ispe", 0u, 0u);
    put32(b, s->w); put32(b, s->h);
    box_end(b, p);
    np++;
    refs[nr++] = (prop_ref){1u, np};
    if (s->grid_tiles) {
        uint32_t tw = s->tile_w ? s->tile_w : (s->w + 1u) / 2u;
        uint32_t th = s->tile_h ? s->tile_h : (s->h + 1u) / 2u;
        p = fullbox_begin(b, "ispe", 0u, 0u);
        put32(b, tw); put32(b, th);
        box_end(b, p);
        np++;
        for (int t = 0; t < s->grid_tiles; t++) {
            if (s->tile_mismatch && t == s->grid_tiles - 1) continue;
            refs[nr++] = (prop_ref){10u + (uint32_t)t, np};
        }
        if (s->tile_mismatch) {
            p = fullbox_begin(b, "ispe", 0u, 0u);
            put32(b, tw + 2u); put32(b, th);
            box_end(b, p);
            np++;
            refs[nr++] = (prop_ref){10u + (uint32_t)(s->grid_tiles - 1), np};
        }
    }
    if (s->hvcc_depth) {
        p = box_begin(b, "hvcC");
        uint8_t cfg[23] = {1};
        cfg[17] = (uint8_t)(0xF8u | ((uint32_t)(s->hvcc_depth - 8) & 7u));
        cfg[18] = (uint8_t)(0xF8u | ((uint32_t)((s->hvcc_chroma ? s->hvcc_chroma : s->hvcc_depth) - 8) & 7u));
        putbytes(b, cfg, sizeof(cfg));
        box_end(b, p);
        np++;
        refs[nr++] = (prop_ref){1u, np};
        for (int t = 0; t < s->grid_tiles; t++) {
            if (s->grid_deep_tile && t == s->grid_tiles - 1) continue;
            refs[nr++] = (prop_ref){10u + (uint32_t)t, np};
        }
        if (s->grid_deep_tile && s->grid_tiles) {
            p = box_begin(b, "hvcC");
            uint8_t deep[23] = {1};
            deep[17] = 0xFAu; /* luma 10 */
            deep[18] = 0xFAu;
            putbytes(b, deep, sizeof(deep));
            box_end(b, p);
            np++;
            refs[nr++] = (prop_ref){10u + (uint32_t)(s->grid_tiles - 1), np};
        }
    }
    if (s->pixi_bits) {
        p = fullbox_begin(b, "pixi", 0u, 0u);
        put8(b, 3u); put8(b, (uint32_t)s->pixi_bits);
        put8(b, (uint32_t)(s->pixi_c1 ? s->pixi_c1 : s->pixi_bits)); put8(b, (uint32_t)s->pixi_bits);
        box_end(b, p);
        np++;
        refs[nr++] = (prop_ref){1u, np};
    }
    if (s->clap_w) {
        p = box_begin(b, "clap");
        put32(b, s->clap_w); put32(b, s->clap_d);
        put32(b, s->clap_h); put32(b, s->clap_d);
        put32(b, (uint32_t)s->clap_hon); put32(b, s->clap_od ? s->clap_od : 1u);
        put32(b, (uint32_t)s->clap_von); put32(b, s->clap_od ? s->clap_od : 1u);
        box_end(b, p);
        np++;
        refs[nr++] = (prop_ref){1u, np};
    }
    if (s->irot) {
        p = box_begin(b, "irot");
        put8(b, (uint32_t)s->irot);
        box_end(b, p);
        np++;
        refs[nr++] = (prop_ref){1u, np};
    }
    if (s->imir) {
        p = box_begin(b, "imir");
        put8(b, 1u);
        box_end(b, p);
        np++;
        refs[nr++] = (prop_ref){1u, np};
    }
    if (s->icc_len) {
        p = box_begin(b, "colr");
        put4cc(b, "prof");
        for (int k = 0; k < s->icc_len; k++) put8(b, (uint32_t)(k * 7 + 3));
        box_end(b, p);
        np++;
        refs[nr++] = (prop_ref){1u, np};
    }
    if (s->nclx_primaries) {
        p = box_begin(b, "colr");
        put4cc(b, "nclx");
        put16(b, (uint32_t)s->nclx_primaries);
        put16(b, (uint32_t)s->nclx_transfer);
        put16(b, 6u);
        put8(b, 0x80u);
        box_end(b, p);
        np++;
        refs[nr++] = (prop_ref){1u, np};
    }
    if (s->alpha_aux || s->depth_aux) {
        p = fullbox_begin(b, "auxC", 0u, 0u);
        putbytes(b, "urn:mpeg:hevc:2015:auxid:1", 27u);
        box_end(b, p);
        np++;
        if (s->alpha_aux) refs[nr++] = (prop_ref){4u, np};
        p = fullbox_begin(b, "auxC", 0u, 0u);
        putbytes(b, "urn:mpeg:hevc:2015:auxid:2", 27u);
        box_end(b, p);
        np++;
        if (s->depth_aux) refs[nr++] = (prop_ref){5u, np};
    }
    box_end(b, ipco);
    /* one entry per distinct item, in first-seen order; with split_ipma
     * the first association (the primary's ispe) goes in its own ipma */
    for (int pass = 0; pass < (s->split_ipma ? 2 : 1); pass++) {
        int lo = s->split_ipma ? (pass == 0 ? 0 : 1) : 0;
        int hi = s->split_ipma ? (pass == 0 ? 1 : nr) : nr;
        size_t ipma = fullbox_begin(b, "ipma", s->ipma_v1 ? 1u : 0u, s->ipma_v1 ? 1u : 0u);
        uint32_t ids[64];
        int nid = 0;
        for (int k = lo; k < hi; k++) {
            int seen = 0;
            for (int q = 0; q < nid; q++) seen |= ids[q] == refs[k].item;
            if (!seen) ids[nid++] = refs[k].item;
        }
        put32(b, (uint32_t)nid);
        for (int q = 0; q < nid; q++) {
            if (s->ipma_v1) put32(b, ids[q]); else put16(b, ids[q]);
            uint32_t cnt = 0u;
            for (int k = lo; k < hi; k++) cnt += refs[k].item == ids[q];
            put8(b, cnt);
            for (int k = lo; k < hi; k++)
                if (refs[k].item == ids[q]) {
                    if (s->ipma_v1) put16(b, 0x8000u | refs[k].index);
                    else put8(b, 0x80u | refs[k].index);
                }
        }
        box_end(b, ipma);
    }
    if (s->nested_iprp) box_end(b, iprp_inner);
    box_end(b, iprp);
    box_end(b, meta);
    if (s->meta_to_eof) {
        b->d[meta] = b->d[meta + 1] = b->d[meta + 2] = b->d[meta + 3] = 0u;
        size_t n0 = b->n;
        memcpy(out, b->d, n0);
        free(b);
        return n0;
    }
    if (s->moov) {
        size_t m = box_begin(b, "moov");
        box_end(b, m);
    }
    size_t md = box_begin(b, "mdat");
    put32(b, 0u);
    box_end(b, md);
    size_t n = b->n;
    memcpy(out, b->d, n);
    free(b);
    return n;
}

typedef struct mem_ctx {
    const uint8_t *d;
    size_t n;
    int fail_at;   /* fail the Nth read (0: never) */
    int reads;
} mem_ctx;

static rfc_status mem_read(void *ctx, uint64_t off, uint8_t *dst, size_t n, size_t *got) {
    mem_ctx *m = (mem_ctx *)ctx;
    *got = 0u;
    if (m->fail_at && ++m->reads == m->fail_at) return RFC_ERR_IO;
    if (off >= m->n) return RFC_OK;
    size_t k = m->n - (size_t)off < n ? m->n - (size_t)off : n;
    memcpy(dst, m->d + off, k);
    *got = k;
    return RFC_OK;
}

static rfc_status scan(const hspec *s, rfc_heif_info *info, const char **detail) {
    static uint8_t bytes[262144];
    size_t n = build_heif(s, bytes);
    mem_ctx m = {bytes, n, 0, 0};
    int rf = 0;
    rfc_status st = rfc_heif_scan(mem_read, &m, n, NULL, info, detail, &rf);
    CHECK(!rf);
    /* the same verdict with the size unknown (read to the end) */
    rfc_heif_info info2;
    mem_ctx m2 = {bytes, n, 0, 0};
    rfc_status st2 = rfc_heif_scan(mem_read, &m2, RFC_HEIF_SIZE_UNKNOWN, NULL, &info2, NULL, &rf);
    CHECK(st2 == st && info2.width == info->width && info2.height == info->height);
    return st;
}

static void test_registration_and_scanner(void) {
    const rfc_capability *hf = rfc_registry_find(RFC_FORMAT_HEIF);
    const rfc_capability *hc = rfc_registry_find(RFC_FORMAT_HEIC);
    CHECK(hf && hc);
    CHECK(hf->availability == RFC_AVAILABILITY_USABLE && hc->availability == RFC_AVAILABILITY_USABLE);
    CHECK(hf->can_read && !hf->can_write && hc->can_read && !hc->can_write);
    CHECK(hf->metadata_items == RFC_META_ITEM_ICC && hc->metadata_items == RFC_META_ITEM_ICC);
    CHECK(hf->sample_count == 1u && hf->samples[0] == RFC_SAMPLE_RGB8);
    CHECK(strstr(hc->provider, "Windows Imaging Component") != NULL);

    CHECK(rfc_codecs_register_heif() == RFC_OK);
    CHECK(rfc_codecs_register_heif() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_HEIF) != NULL &&
          rfc_registry_session_adapter(RFC_FORMAT_HEIC) != NULL);
    const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_HEIC);
    CHECK(a != NULL);
    CHECK(rfc_codecs_unregister_heif() == RFC_ERR_BUSY);
    rfc_registry_release_session(RFC_FORMAT_HEIC);
    CHECK(rfc_codecs_unregister_heif() == RFC_OK);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_HEIF) == NULL &&
          rfc_registry_session_adapter(RFC_FORMAT_HEIC) == NULL);
    CHECK(rfc_codecs_register_heif() == RFC_OK);

    /* encode always refuses */
    {
        rfc_exec_context e;
        tracked st;
        rfc_allocator al;
        init_exec(&e, &st, &al);
        const rfc_session_adapter *ad = rfc_registry_acquire_session(RFC_FORMAT_HEIC);
        void *s = NULL;
        CHECK(ad->open_session(&e, &s) == RFC_OK);
        uint8_t px[12] = {0};
        rfc_frame fr;
        memset(&fr, 0, sizeof(fr));
        fr.layout = (rfc_layout){RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 2u, 2u, 6u, 8u};
        fr.data = px;
        fr.data_len = sizeof(px);
        rfc_encoded enc;
        memset(&enc, 0, sizeof(enc));
        CHECK(ad->encode_session(s, &e, &fr, 1u, NULL, NULL, 0u, &enc) == RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(enc.data == NULL);
        ad->close_session(s);
        rfc_registry_release_session(RFC_FORMAT_HEIC);
    }

    /* the shared reader on hand-built structures */
    rfc_heif_info info;
    const char *d = NULL;
    hspec s;
    hspec_default(&s);
    CHECK(scan(&s, &info, &d) == RFC_OK);
    CHECK(info.width == 64u && info.height == 48u && info.coding == RFC_HEIF_CODING_HEVC && info.bit_depth == 8u);
    CHECK(info.layout.format == RFC_SAMPLE_RGB8 && !info.has_icc && !info.has_exif);
    hspec_default(&s); s.irot = 1;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 48u && info.height == 64u);
    hspec_default(&s); s.clap_w = 30u; s.clap_h = 20u; s.clap_d = 1u; s.irot = 3;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 20u && info.height == 30u); /* crop, then turn */
    hspec_default(&s); s.clap_w = 61u; s.clap_h = 20u; s.clap_d = 2u;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_COMBINATION); /* fractional */
    hspec_default(&s); s.imir = 1;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 64u && info.has_imir);
    hspec_default(&s); s.irot = 2; s.imir = 1;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_COMBINATION && d != NULL); /* mirror + rotation */
    hspec_default(&s); s.primary_type = "grid"; s.grid_tiles = 4; s.hvcc_depth = 8;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.coding == RFC_HEIF_CODING_HEVC && info.primary_type == 0x67726964u);
    hspec_default(&s); s.thumbnail = 1;
    CHECK(scan(&s, &info, &d) == RFC_OK);
    struct {
        const char *what;
        rfc_status want;
    } cases[] = {
        {"second", RFC_ERR_UNSUPPORTED_COMBINATION}, {"iden", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"moov", RFC_ERR_UNSUPPORTED_COMBINATION},   {"alpha", RFC_ERR_UNSUPPORTED_ALPHA},
        {"depth", RFC_ERR_UNSUPPORTED_COMBINATION},  {"pixi10", RFC_ERR_UNSUPPORTED_SAMPLE},
        {"hvcc10", RFC_ERR_UNSUPPORTED_SAMPLE},      {"pq", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"hlg", RFC_ERR_UNSUPPORTED_COMBINATION},    {"items", RFC_ERR_LIMIT_EXCEEDED},
        {"ftyp_late", RFC_ERR_INVALID_ARGUMENT},
    };
    for (size_t k = 0u; k < sizeof(cases) / sizeof(cases[0]); k++) {
        hspec_default(&s);
        const char *w = cases[k].what;
        if (!strcmp(w, "second")) s.second_image = 1;
        if (!strcmp(w, "iden")) s.primary_type = "iden";
        if (!strcmp(w, "moov")) s.moov = 1;
        if (!strcmp(w, "alpha")) s.alpha_aux = 1;
        if (!strcmp(w, "depth")) s.depth_aux = 1;
        if (!strcmp(w, "pixi10")) s.pixi_bits = 10;
        if (!strcmp(w, "hvcc10")) s.hvcc_depth = 10;
        if (!strcmp(w, "pq")) { s.nclx_primaries = 9; s.nclx_transfer = 16; }
        if (!strcmp(w, "hlg")) { s.nclx_primaries = 9; s.nclx_transfer = 18; }
        if (!strcmp(w, "items")) s.extra_items = 4100;
        if (!strcmp(w, "ftyp_late")) s.ftyp_late = 1;
        rfc_status st = scan(&s, &info, &d);
        if (st != cases[k].want) fprintf(stderr, "scan %s: got %s\n", w, rfc_status_name(st));
        CHECK(st == cases[k].want);
        if (st != RFC_ERR_LIMIT_EXCEEDED && st != RFC_ERR_INVALID_ARGUMENT && strcmp(w, "moov") != 0) {
            CHECK(d != NULL && info.dims_known && info.width == 64u);
        }
    }
    /* table forms and bounds the redesign covers */
    hspec_default(&s); s.ipma_v1 = 1;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 64u);
    hspec_default(&s); s.split_ipma = 1; s.icc_len = 40;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 64u && info.has_icc);
    hspec_default(&s); s.nested_iprp = 1;
    CHECK(scan(&s, &info, &d) == RFC_ERR_INVALID_ARGUMENT); /* no ispe at its level */
    hspec_default(&s); s.pixi_bits = 8; s.pixi_c1 = 10;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_SAMPLE);
    hspec_default(&s); s.hvcc_chroma = 10;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_SAMPLE);
    hspec_default(&s); s.primary_type = "grid"; s.grid_tiles = 4; s.grid_mixed = 1;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_COMBINATION);
    hspec_default(&s); s.primary_type = "grid"; s.grid_tiles = 4; s.grid_deep_tile = 1;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_SAMPLE);
    hspec_default(&s); s.moov_only = 1;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_COMBINATION && d != NULL);
    hspec_default(&s); s.unterminated = 1;
    CHECK(scan(&s, &info, &d) == RFC_ERR_INVALID_ARGUMENT);
    /* completion gaps: a meta box running to the end with the size unknown;
     * a grid's coded picture is its tile mosaic; AV1 is refused, named;
     * clean-aperture extremes stay defined (UBSan) */
    hspec_default(&s); s.meta_to_eof = 1;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 64u && info.height == 48u);
    hspec_default(&s); s.primary_type = "grid"; s.grid_tiles = 4; s.w = 60u; s.h = 60u; s.tile_w = 32u; s.tile_h = 32u;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 60u && info.coded_width == 64u &&
          info.coded_height == 64u && info.tile_width == 32u);
    hspec_default(&s); s.primary_type = "grid"; s.grid_tiles = 4; s.tile_mismatch = 1;
    CHECK(scan(&s, &info, &d) == RFC_ERR_INVALID_ARGUMENT);
    hspec_default(&s); s.primary_type = "grid"; s.grid_tiles = 3;
    CHECK(scan(&s, &info, &d) == RFC_ERR_INVALID_ARGUMENT); /* 3 tiles cannot cover 2x2 */
    hspec_default(&s); s.primary_type = "av01"; s.hvcc_depth = 0;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_COMBINATION && d != NULL && strstr(d, "AV1") != NULL);
    hspec_default(&s); s.w = 0xFFFFFFFFu; s.h = 0xFFFFFFFFu; s.clap_w = 1u; s.clap_h = 1u; s.clap_d = 1u;
    s.clap_hon = 0x7FFFFFFF; s.clap_od = 0xFFFFFFFFu;
    {
        rfc_status cst = scan(&s, &info, &d);
        CHECK(cst == RFC_ERR_LIMIT_EXCEEDED || cst == RFC_ERR_UNSUPPORTED_COMBINATION);
    }
    /* coded and displayed geometry: a 30x20 aperture of a 64x48 picture
     * turned a quarter, and apertures off the pixel grid */
    hspec_default(&s); s.clap_w = 30u; s.clap_h = 20u; s.clap_d = 1u; s.irot = 1;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 20u && info.height == 30u &&
          info.coded_width == 48u && info.coded_height == 64u);
    hspec_default(&s); s.clap_w = 31u; s.clap_h = 20u; s.clap_d = 1u; /* (64-31)/2 not whole */
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_COMBINATION);
    hspec_default(&s); s.clap_w = 31u; s.clap_h = 20u; s.clap_d = 1u; s.clap_hon = -1; s.clap_od = 2u;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.width == 31u); /* 16.5 - 0.5 = 16 */
    hspec_default(&s); s.clap_w = 30u; s.clap_h = 20u; s.clap_d = 1u; s.clap_hon = 40;
    CHECK(scan(&s, &info, &d) == RFC_ERR_UNSUPPORTED_COMBINATION); /* off the picture */
    /* colour boxes and metadata items */
    hspec_default(&s); s.icc_len = 300;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.has_icc && info.icc_len == 300u && !info.has_nclx);
    hspec_default(&s); s.icc_len = 200; s.nclx_primaries = 1; s.nclx_transfer = 13;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.has_icc && !info.has_nclx); /* ICC wins */
    hspec_default(&s); s.nclx_primaries = 12; s.nclx_transfer = 13;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.has_nclx && info.primaries == 12u &&
          info.transfer == 13u && info.matrix == 6u && info.full_range == 1u);
    hspec_default(&s); s.exif = 1; s.xmp = 1;
    CHECK(scan(&s, &info, &d) == RFC_OK && info.has_exif && info.has_xmp);
    /* truncation at every length refuses cleanly; a reader failure is told
     * apart from a verdict */
    {
        static uint8_t bytes[262144];
        hspec_default(&s);
        s.icc_len = 64;
        size_t n = build_heif(&s, bytes);
        for (size_t cut = 0u; cut < n; cut += 7u) {
            mem_ctx m = {bytes, cut, 0, 0};
            int rf = 0;
            rfc_status st = rfc_heif_scan(mem_read, &m, cut, NULL, &info, &d, &rf);
            CHECK(st != RFC_OK || cut >= n - 16u); /* mdat payload is not needed */
            CHECK(!rf);
        }
        /* the reader is read a handful of times (top-level headers, then the
         * meta box once); a failure at any of those reads is the reader's */
        mem_ctx count = {bytes, n, 1000000, 0};
        int rf0 = 0;
        CHECK(rfc_heif_scan(mem_read, &count, n, NULL, &info, &d, &rf0) == RFC_OK && !rf0);
        CHECK(count.reads >= 3 && count.reads <= 8);
        for (int fail = 1; fail <= count.reads; fail++) {
            mem_ctx m = {bytes, n, fail, 0};
            int rf = 0;
            rfc_status st = rfc_heif_scan(mem_read, &m, n, NULL, &info, &d, &rf);
            CHECK(rf && st == RFC_ERR_IO);
        }
    }
    printf("heif: registration_and_scanner ok\n");
}

/* ------------------------------------------------------------------------ */
/* Non-Windows: the named refusal                                            */
/* ------------------------------------------------------------------------ */

#ifndef _WIN32
static void test_platform_refusal(void) {
    static uint8_t bytes[262144];
    hspec s;
    hspec_default(&s);
    size_t n = build_heif(&s, bytes);
    CHECK(adapter_decode(RFC_FORMAT_HEIC, bytes, n, NULL, NULL, NULL, NULL, NULL) == RFC_ERR_NO_ADAPTER);
    char src[512], dst[512];
    temp_path(src, sizeof(src), "x.heic");
    temp_path(dst, sizeof(dst), "x.png");
    CHECK(write_file_all(src, bytes, n));
    job_result r;
    run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, dst, NULL, NULL, &r);
    /* admitted without hints (sniffed geometry), refused at IMPORT, named */
    CHECK(r.submit == RFC_OK && r.status == RFC_ERR_NO_ADAPTER && r.failing == RFC_STAGE_IMPORT);
    CHECK(strstr(r.detail, "Windows Imaging Component") != NULL && !file_exists(dst));
    remove(src);
    printf("heif: platform_refusal ok\n");
}
#endif

/* ------------------------------------------------------------------------ */
/* Windows: WIC fixtures                                                     */
/* ------------------------------------------------------------------------ */

#ifdef _WIN32

typedef struct heic_opts {
    uint32_t w, h;
    int orientation; /* 0: not written */
    int icc_len;
    int exif, xmp;
    int frames;
} heic_opts;

static uint8_t pattern(uint32_t x, uint32_t y, uint32_t c, uint32_t w, uint32_t h) {
    if (c == 0u) return (uint8_t)(x * 255u / (w > 1u ? w - 1u : 1u));
    if (c == 1u) return (uint8_t)(y * 255u / (h > 1u ? h - 1u : 1u));
    return (uint8_t)(((x / 8u) + (y / 8u)) % 2u ? 200u : 60u);
}

static IWICImagingFactory *wic(void) {
    static IWICImagingFactory *f = NULL;
    if (!f) {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        (void)hr;
        CHECK(SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                         &IID_IWICImagingFactory, (void **)&f)));
    }
    return f;
}

static uint8_t g_icc[2048];

static uint8_t *make_heic(const heic_opts *o, size_t *out_n) {
    char path[512];
    temp_path(path, sizeof(path), "mk.heic");
    wchar_t wpath[512];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);
    IWICImagingFactory *f = wic();
    IWICBitmapEncoder *e = NULL;
    CHECK(SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatHeif, NULL, &e)));
    IWICStream *st = NULL;
    CHECK(SUCCEEDED(IWICImagingFactory_CreateStream(f, &st)));
    CHECK(SUCCEEDED(IWICStream_InitializeFromFilename(st, wpath, GENERIC_WRITE)));
    CHECK(SUCCEEDED(IWICBitmapEncoder_Initialize(e, (IStream *)st, WICBitmapEncoderNoCache)));
    for (int k = 0; k < (o->frames ? o->frames : 1); k++) {
        IWICBitmapFrameEncode *fr = NULL;
        IPropertyBag2 *pb = NULL;
        CHECK(SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(e, &fr, &pb)));
        CHECK(SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, pb)));
        CHECK(SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, o->w, o->h)));
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGR;
        CHECK(SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &pf)));
        CHECK(IsEqualGUID(&pf, &GUID_WICPixelFormat32bppBGR));
        if (o->icc_len) {
            IWICColorContext *cc = NULL;
            CHECK(SUCCEEDED(IWICImagingFactory_CreateColorContext(f, &cc)));
            for (int i = 0; i < o->icc_len; i++) g_icc[i] = (uint8_t)(i * 13 + 1);
            memset(g_icc, 0, 4);
            g_icc[2] = (uint8_t)(o->icc_len >> 8);
            g_icc[3] = (uint8_t)o->icc_len;
            memcpy(g_icc + 36, "acsp", 4);
            CHECK(SUCCEEDED(IWICColorContext_InitializeFromMemory(cc, g_icc, (UINT)o->icc_len)));
            CHECK(SUCCEEDED(IWICBitmapFrameEncode_SetColorContexts(fr, 1, &cc)));
            IWICColorContext_Release(cc);
        }
        IWICMetadataQueryWriter *qw = NULL;
        if (SUCCEEDED(IWICBitmapFrameEncode_GetMetadataQueryWriter(fr, &qw)) && qw) {
            PROPVARIANT v;
            if (o->orientation) {
                PropVariantInit(&v);
                v.vt = VT_UI2;
                v.uiVal = (USHORT)o->orientation;
                CHECK(SUCCEEDED(IWICMetadataQueryWriter_SetMetadataByName(qw, L"/heifProps/Orientation", &v)));
            }
            if (o->exif) {
                PropVariantInit(&v);
                v.vt = VT_LPSTR;
                v.pszVal = (LPSTR) "RasterFold";
                CHECK(SUCCEEDED(IWICMetadataQueryWriter_SetMetadataByName(qw, L"/ifd/{ushort=271}", &v)));
            }
            if (o->xmp) {
                PropVariantInit(&v);
                v.vt = VT_LPWSTR;
                v.pwszVal = (LPWSTR)L"RasterFold";
                CHECK(SUCCEEDED(IWICMetadataQueryWriter_SetMetadataByName(qw, L"/xmp/dc:creator", &v)));
            }
            IWICMetadataQueryWriter_Release(qw);
        }
        BYTE *px = (BYTE *)malloc((size_t)o->w * o->h * 4u);
        CHECK(px != NULL);
        for (uint32_t y = 0u; y < o->h; y++)
            for (uint32_t x = 0u; x < o->w; x++) {
                BYTE *q = px + ((size_t)y * o->w + x) * 4u;
                q[0] = pattern(x, y, 2u, o->w, o->h); /* B */
                q[1] = pattern(x, y, 1u, o->w, o->h); /* G */
                q[2] = pattern(x, y, 0u, o->w, o->h); /* R */
                q[3] = 255u;
            }
        CHECK(SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, o->h, o->w * 4u, o->w * o->h * 4u, px)));
        free(px);
        CHECK(SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)));
        IWICBitmapFrameEncode_Release(fr);
        if (pb) IPropertyBag2_Release(pb);
    }
    CHECK(SUCCEEDED(IWICBitmapEncoder_Commit(e)));
    IWICBitmapEncoder_Release(e);
    IWICStream_Release(st);
    uint8_t *b = read_file_all(path, out_n);
    CHECK(b != NULL);
    remove(path);
    return b;
}

/* Rebrand a WIC-written HEIC as generic HEIF: its ftyp brands heic -> mif1. */
static void as_heif(uint8_t *b, size_t n) {
    uint32_t ftyp = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    for (size_t i = 8u; i + 4u <= ftyp && i + 4u <= n; i += 4u) {
        if (memcmp(b + i, "heic", 4) == 0 || memcmp(b + i, "heix", 4) == 0) memcpy(b + i, "mif1", 4);
    }
}

/* Rewrite the primary's 7-byte ICC colr box in place as an nclx box of the
 * same size (BT.2020 primaries, sRGB transfer). */
static void icc7_to_nclx(uint8_t *b, size_t n) {
    for (size_t i = 0u; i + 19u <= n; i++) {
        if (memcmp(b + i + 4u, "colrprof", 8) == 0 && b[i + 3u] == 19u) {
            static const uint8_t nclx[11] = {'n', 'c', 'l', 'x', 0, 9, 0, 13, 0, 6, 0x80};
            memcpy(b + i + 8u, nclx, sizeof(nclx));
            return;
        }
    }
    CHECK(0 && "no 7-byte colr prof box");
}

/* A second, fresh WIC decode (shared provider), packed RGB. */
static uint8_t *wic_decode_rgb(const uint8_t *b, size_t n, uint32_t *w, uint32_t *h) {
    IWICImagingFactory *f = wic();
    IWICStream *st = NULL;
    CHECK(SUCCEEDED(IWICImagingFactory_CreateStream(f, &st)));
    CHECK(SUCCEEDED(IWICStream_InitializeFromMemory(st, (BYTE *)b, (DWORD)n)));
    IWICBitmapDecoder *d = NULL;
    CHECK(SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)st, NULL,
                                                               WICDecodeMetadataCacheOnDemand, &d)));
    IWICBitmapFrameDecode *fd = NULL;
    CHECK(SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fd)));
    CHECK(SUCCEEDED(IWICBitmapFrameDecode_GetSize(fd, w, h)));
    WICPixelFormatGUID pf;
    CHECK(SUCCEEDED(IWICBitmapFrameDecode_GetPixelFormat(fd, &pf)));
    CHECK(IsEqualGUID(&pf, &GUID_WICPixelFormat32bppBGR));
    BYTE *tmp = (BYTE *)malloc((size_t)*w * *h * 4u);
    uint8_t *rgb = (uint8_t *)malloc((size_t)*w * *h * 3u);
    CHECK(tmp && rgb);
    WICRect rc = {0, 0, (INT)*w, (INT)*h};
    CHECK(SUCCEEDED(IWICBitmapFrameDecode_CopyPixels(fd, &rc, *w * 4u, *w * *h * 4u, tmp)));
    for (size_t i = 0u; i < (size_t)*w * *h; i++) {
        rgb[i * 3u + 0u] = tmp[i * 4u + 2u];
        rgb[i * 3u + 1u] = tmp[i * 4u + 1u];
        rgb[i * 3u + 2u] = tmp[i * 4u + 0u];
    }
    free(tmp);
    IWICBitmapFrameDecode_Release(fd);
    IWICBitmapDecoder_Release(d);
    IWICStream_Release(st);
    return rgb;
}

/* Source coordinates of displayed pixel (dx, dy) under EXIF orientation o
 * for a w x h source. */
static void orient_src(int o, uint32_t w, uint32_t h, uint32_t dx, uint32_t dy, uint32_t *sx,
                       uint32_t *sy) {
    switch (o) {
        case 2: *sx = w - 1u - dx; *sy = dy; break;
        case 3: *sx = w - 1u - dx; *sy = h - 1u - dy; break;
        case 4: *sx = dx; *sy = h - 1u - dy; break;
        case 5: *sx = dy; *sy = dx; break;
        case 6: *sx = dy; *sy = h - 1u - dx; break;
        case 7: *sx = w - 1u - dy; *sy = h - 1u - dx; break;
        case 8: *sx = w - 1u - dy; *sy = dx; break;
        default: *sx = dx; *sy = dy; break;
    }
}

static double orient_error(const uint8_t *px, uint32_t dw, uint32_t dh, int o, uint32_t w, uint32_t h) {
    if ((o >= 5) ? (dw != h || dh != w) : (dw != w || dh != h)) return 1e9;
    double sum = 0.0;
    for (uint32_t y = 0u; y < dh; y++)
        for (uint32_t x = 0u; x < dw; x++) {
            uint32_t sx, sy;
            orient_src(o, w, h, x, y, &sx, &sy);
            for (uint32_t c = 0u; c < 3u; c++)
                sum += fabs((double)px[((size_t)y * dw + x) * 3u + c] - (double)pattern(sx, sy, c, w, h));
        }
    return sum / ((double)dw * dh * 3.0);
}

/* Mean luma error against the pattern (orientation 1): HEVC's 4:2:0 chroma
 * subsampling of a pattern with sharp chroma edges raises the overall error
 * of a small image; a wrong colour conversion breaks luma too. */
/* The system's encoder and decoder place an odd-sized clean aperture
 * (padded to an even coded size) up to one pixel off, so the best fit over
 * shifts of at most one pixel is taken and the shift reported. */
static double luma_error(const uint8_t *px, uint32_t w, uint32_t h, int *sx_out, int *sy_out) {
    double best = 1e18;
    for (int sy = -1; sy <= 1; sy++)
        for (int sx = -1; sx <= 1; sx++) {
            double sum = 0.0;
            size_t cnt = 0u;
            for (uint32_t y = 0u; y < h; y++)
                for (uint32_t x = 0u; x < w; x++) {
                    int px_ = (int)x + sx, py_ = (int)y + sy;
                    if (px_ < 0 || py_ < 0 || px_ >= (int)w || py_ >= (int)h) continue;
                    const uint8_t *q = px + ((size_t)y * w + x) * 3u;
                    double a = 0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2];
                    double b = 0.299 * pattern((uint32_t)px_, (uint32_t)py_, 0u, w, h) +
                               0.587 * pattern((uint32_t)px_, (uint32_t)py_, 1u, w, h) +
                               0.114 * pattern((uint32_t)px_, (uint32_t)py_, 2u, w, h);
                    sum += fabs(a - b);
                    cnt++;
                }
            double e = sum / (double)cnt;
            if (e < best) {
                best = e;
                *sx_out = sx;
                *sy_out = sy;
            }
        }
    return best;
}

typedef struct cancel_ctx {
    const rfc_session_adapter *a;
    void *sess;
} cancel_ctx;

static rfc_job *volatile g_cancel_job = NULL;

static void job_cancel_hook(void *ctx) {
    (void)ctx;
    if (g_cancel_job) rfc_job_cancel(g_cancel_job);
}

static void cancel_hook(void *ctx) {
    cancel_ctx *c = (cancel_ctx *)ctx;
    c->a->cancel_session(c->sess);
}

static void test_wic_decode_matrix(void) {
    /* decode equals a second fresh WIC decode, and stays close to the pattern */
    static const uint32_t sizes[4][2] = {{64u, 48u}, {33u, 17u}, {256u, 130u}, {8u, 8u}};
    for (int k = 0; k < 4; k++) {
        heic_opts o = {sizes[k][0], sizes[k][1], 0, 0, 0, 0, 1};
        size_t n = 0u;
        uint8_t *b = make_heic(&o, &n);
        uint8_t *px = NULL;
        rfc_frame info;
        CHECK(adapter_decode(RFC_FORMAT_HEIC, b, n, NULL, NULL, NULL, &info, &px) == RFC_OK);
        CHECK(info.layout.format == RFC_SAMPLE_RGB8 && info.layout.width == o.w && info.layout.height == o.h);
        CHECK(info.metadata.orientation == RFC_ORIENTATION_NORMAL && info.metadata.icc_len == 0u);
        uint32_t w2 = 0u, h2 = 0u;
        uint8_t *ref = wic_decode_rgb(b, n, &w2, &h2);
        CHECK(w2 == o.w && h2 == o.h && memcmp(px, ref, (size_t)o.w * o.h * 3u) == 0);
        if (o.w > 8u) {
            int sx = 0, sy = 0;
            double lerr = luma_error(px, o.w, o.h, &sx, &sy);
            printf("heif: %ux%u luma error %.2f against the pattern (best shift %d,%d)\n", o.w, o.h,
                   lerr, sx, sy);
            CHECK(lerr < 8.0);
            if ((o.w % 2u) == 0u && (o.h % 2u) == 0u) CHECK(sx == 0 && sy == 0);
        }
        free(ref);
        free(px);
        free(b);
    }
    /* a file the system decoder itself cannot open (WIC writes a 1x1 image
     * it then fails to decode) is refused cleanly, nothing left allocated */
    {
        heic_opts o = {1u, 1u, 0, 0, 0, 0, 1};
        size_t n = 0u;
        uint8_t *b = make_heic(&o, &n);
        CHECK(adapter_decode(RFC_FORMAT_HEIC, b, n, NULL, NULL, NULL, NULL, NULL) == RFC_ERR_INVALID_ARGUMENT);
        free(b);
    }
    /* orientation 1..8: the system applies a rotation or a mirror, and the
     * closest transform of the pattern is the one written; a mirror combined
     * with a rotation (5 and 7, which the system's encoder and decoder
     * disagree on) is refused, named */
    for (int ori = 1; ori <= 8; ori++) {
        heic_opts o = {48u, 32u, ori, 0, 0, 0, 1};
        size_t n = 0u;
        uint8_t *b = make_heic(&o, &n);
        uint8_t *px = NULL;
        rfc_frame info;
        rfc_status ost = adapter_decode(RFC_FORMAT_HEIC, b, n, NULL, NULL, NULL, &info, &px);
        if (ori == 5 || ori == 7) {
            CHECK(ost == RFC_ERR_UNSUPPORTED_COMBINATION);
            free(b);
            continue;
        }
        if (ost != RFC_OK) fprintf(stderr, "orientation %d: %s\n", ori, rfc_status_name(ost));
        CHECK(ost == RFC_OK);
        CHECK(info.metadata.orientation == RFC_ORIENTATION_NORMAL);
        int best = 0;
        double best_err = 1e18;
        for (int t = 1; t <= 8; t++) {
            double e = orient_error(px, info.layout.width, info.layout.height, t, o.w, o.h);
            if (e < best_err) {
                best_err = e;
                best = t;
            }
        }
        if (best != ori) fprintf(stderr, "orientation %d decoded as %d (err %.2f)\n", ori, best, best_err);
        CHECK(best == ori);
        free(px);
        free(b);
    }
    printf("heif: wic_decode_matrix ok\n");
}

static void test_wic_metadata_and_refusals(void) {
    size_t n = 0u;
    /* ICC bytes come from the file */
    {
        heic_opts o = {40u, 30u, 0, 600, 0, 0, 1};
        uint8_t *b = make_heic(&o, &n);
        rfc_exec_context e;
        tracked st;
        rfc_allocator al;
        init_exec(&e, &st, &al);
        const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_HEIC);
        void *s = NULL;
        CHECK(a->open_session(&e, &s) == RFC_OK);
        rfc_frame *fr = NULL;
        size_t fc = 0u;
        CHECK(a->decode_session(s, &e, b, n, NULL, 0u, &fr, &fc, NULL) == RFC_OK);
        CHECK(fr[0].metadata.icc_len == 600u && memcmp(fr[0].metadata.icc, g_icc, 600u) == 0);
        rfc_frame_free(&fr[0]);
        al.free(al.ctx, fr);
        a->close_session(s);
        rfc_registry_release_session(RFC_FORMAT_HEIC);
        CHECK(st.live == 0u);
        free(b);
    }
    /* the probe reports the file's ICC */
    {
        heic_opts o = {40u, 30u, 0, 600, 0, 0, 1};
        uint8_t *b = make_heic(&o, &n);
        rfc_exec_context e;
        tracked st;
        rfc_allocator al;
        init_exec(&e, &st, &al);
        const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_HEIC);
        void *s = NULL;
        CHECK(a->open_session(&e, &s) == RFC_OK);
        rfc_image_probe pr;
        CHECK(a->probe_session(s, &e, b, n, &pr) == RFC_OK);
        CHECK(pr.first_frame.width == 40u && pr.container_meta.icc_len == 600u &&
              memcmp(pr.container_meta.icc, g_icc, 600u) == 0);
        al.free(al.ctx, (void *)pr.container_meta.icc);
        a->close_session(s);
        rfc_registry_release_session(RFC_FORMAT_HEIC);
        CHECK(st.live == 0u);
        free(b);
    }
    /* nclx reaches the frame as CICP: primaries and transfer, matrix 0 and
     * full range for the RGB the system returns */
    {
        heic_opts o = {40u, 30u, 0, 7, 0, 0, 1};
        uint8_t *b = make_heic(&o, &n);
        icc7_to_nclx(b, n);
        rfc_frame info;
        CHECK(adapter_decode(RFC_FORMAT_HEIC, b, n, NULL, NULL, NULL, &info, NULL) == RFC_OK);
        CHECK(info.metadata.icc_len == 0u && info.metadata.color.has_cicp &&
              info.metadata.color.cicp[0] == 9u && info.metadata.color.cicp[1] == 13u &&
              info.metadata.color.cicp[2] == 0u && info.metadata.color.cicp[3] == 1u);
        free(b);
    }
    /* the HEIF row: a generic HEIF (mif1) file through its own sniff,
     * selection and decode, and a job */
    {
        heic_opts o = {48u, 32u, 0, 0, 0, 0, 1};
        uint8_t *b = make_heic(&o, &n);
        as_heif(b, n);
        rfc_sniff_result sr;
        CHECK(rfc_input_sniff_bytes(b, n, "x.heif", &sr) == RFC_OK && sr.content_format == RFC_FORMAT_HEIF);
        uint8_t *px = NULL;
        CHECK(adapter_decode(RFC_FORMAT_HEIF, b, n, NULL, NULL, NULL, NULL, &px) == RFC_OK);
        uint32_t w2 = 0u, h2 = 0u;
        uint8_t *ref = wic_decode_rgb(b, n, &w2, &h2);
        CHECK(w2 == 48u && h2 == 32u && memcmp(px, ref, 48u * 32u * 3u) == 0);
        char src[512], dst[512];
        temp_path(src, sizeof(src), "g.heif");
        temp_path(dst, sizeof(dst), "g.png");
        CHECK(write_file_all(src, b, n));
        job_result r;
        run_job_full(RFC_FORMAT_HEIF, RFC_FORMAT_PNG, src, dst, NULL, NULL, &r);
        CHECK_JOB_OK(r);
        remove(dst);
        remove(src);
        free(ref);
        free(px);
        free(b);
    }
    /* Exif and XMP: refused under preserve, imported under strip; through a
     * job, strip publishes a PNG holding the same pixels and neither item */
    for (int k = 0; k < 2; k++) {
        heic_opts o = {40u, 30u, 0, 0, k == 0, k == 1, 1};
        uint8_t *b = make_heic(&o, &n);
        CHECK(adapter_decode(RFC_FORMAT_HEIC, b, n, "preserve", NULL, NULL, NULL, NULL) == RFC_ERR_METADATA_POLICY);
        uint8_t *px = NULL;
        CHECK(adapter_decode(RFC_FORMAT_HEIC, b, n, "strip", NULL, NULL, NULL, &px) == RFC_OK);
        char src[512], dst[512];
        temp_path(src, sizeof(src), "m.heic");
        temp_path(dst, sizeof(dst), "m.png");
        CHECK(write_file_all(src, b, n));
        job_result r;
        run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, dst, "preserve", NULL, &r);
        CHECK(r.submit == RFC_OK && r.status == RFC_ERR_METADATA_POLICY && !file_exists(dst));
        CHECK(strstr(r.detail, "Exif or XMP") != NULL);
        run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, dst, "strip", NULL, &r);
        CHECK_JOB_OK(r);
        size_t pn = 0u;
        uint8_t *pb = read_file_all(dst, &pn);
        uint8_t *back = NULL;
        rfc_exec_context e;
        tracked st;
        rfc_allocator al;
        init_exec(&e, &st, &al);
        const rfc_session_adapter *pa = rfc_registry_acquire_session(RFC_FORMAT_PNG);
        void *ps = NULL;
        CHECK(pa->open_session(&e, &ps) == RFC_OK);
        rfc_frame *pf = NULL;
        size_t pc = 0u;
        CHECK(pa->decode_session(ps, &e, pb, pn, NULL, 0u, &pf, &pc, NULL) == RFC_OK);
        back = (uint8_t *)malloc(40u * 30u * 3u);
        for (uint32_t y = 0u; y < 30u; y++) memcpy(back + y * 120u, pf[0].data + (size_t)y * pf[0].layout.stride_bytes, 120u);
        CHECK(memcmp(back, px, 40u * 30u * 3u) == 0);
        CHECK(pf[0].metadata.exif_len == 0u && pf[0].metadata.xmp_len == 0u);
        rfc_frame_free(&pf[0]);
        al.free(al.ctx, pf);
        pa->close_session(ps);
        rfc_registry_release_session(RFC_FORMAT_PNG);
        free(back);
        free(pb);
        remove(dst);
        remove(src);
        free(px);
        free(b);
    }
    /* a second image */
    {
        heic_opts o = {40u, 30u, 0, 0, 0, 0, 2};
        uint8_t *b = make_heic(&o, &n);
        CHECK(adapter_decode(RFC_FORMAT_HEIC, b, n, NULL, NULL, NULL, NULL, NULL) == RFC_ERR_UNSUPPORTED_COMBINATION);
        free(b);
    }
    /* missing components, through a job without layout or estimate */
    {
        heic_opts o = {40u, 30u, 0, 0, 0, 0, 1};
        uint8_t *b = make_heic(&o, &n);
        char src[512], dst[512];
        temp_path(src, sizeof(src), "c.heic");
        temp_path(dst, sizeof(dst), "c.png");
        CHECK(write_file_all(src, b, n));
        for (int k = 0; k < 2; k++) {
            rfc_codecs_heif_set_missing_for_testing(k == 0, k == 1);
            job_result r;
            run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, dst, NULL, NULL, &r);
            rfc_codecs_heif_set_missing_for_testing(0, 0);
            CHECK(r.submit == RFC_OK && r.status == RFC_ERR_NO_ADAPTER && !file_exists(dst));
            CHECK(strstr(r.detail, k == 0 ? "HEIF Image Extensions" : "HEVC Video Extensions") != NULL);
        }
        /* a resource failure creating the WIC decoder is reported as such,
         * never as a missing extension */
        rfc_codecs_heif_set_create_hr_for_testing((long)E_OUTOFMEMORY);
        {
            job_result r;
            run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, dst, NULL, NULL, &r);
            rfc_codecs_heif_set_create_hr_for_testing(0);
            CHECK(r.submit == RFC_OK && r.status == RFC_ERR_OUT_OF_MEMORY && !file_exists(dst));
            CHECK(strstr(r.detail, "Extensions") == NULL);
        }
        /* and with the components, the same submission publishes a PNG */
        job_result r;
        run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, dst, NULL, NULL, &r);
        CHECK_JOB_OK(r);
        CHECK(file_exists(dst));
        remove(dst);
        /* writing HEIC is refused, with no output */
        char png[512], heic[512];
        temp_path(png, sizeof(png), "w.png");
        temp_path(heic, sizeof(heic), "w.heic");
        run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, png, NULL, NULL, &r);
        CHECK_JOB_OK(r);
        run_job_full(RFC_FORMAT_PNG, RFC_FORMAT_HEIC, png, heic, NULL, NULL, &r);
        CHECK((r.submit != RFC_OK || r.status != RFC_OK) && !file_exists(heic));
        remove(png);
        /* limits: refused at submit from the sniffed geometry, and by the adapter */
        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_width = 39u;
        run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, dst, NULL, &lim, &r);
        CHECK(r.submit == RFC_ERR_LIMIT_EXCEEDED && !file_exists(dst));
        CHECK(adapter_decode(RFC_FORMAT_HEIC, b, n, NULL, &lim, NULL, NULL, NULL) == RFC_ERR_LIMIT_EXCEEDED);
        /* budget: below the reservation refuses with nothing left */
        rfc_budget *bud = rfc_budget_create(1u << 20);
        CHECK(adapter_decode(RFC_FORMAT_HEIC, b, n, NULL, NULL, bud, NULL, NULL) == RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_stats bs;
        rfc_budget_stats_get(bud, &bs);
        CHECK(bs.current_allocated_bytes == 0u);
        rfc_budget_destroy(bud);
        remove(src);
        free(b);
    }
    /* cancellation between bands releases everything */
    {
        heic_opts o = {64u, 300u, 0, 0, 0, 0, 1};
        uint8_t *b = make_heic(&o, &n);
        rfc_budget *bud = rfc_budget_create(256u << 20);
        rfc_allocator ba;
        rfc_budget_allocator(bud, &ba);
        rfc_exec_context e;
        tracked st;
        rfc_allocator al;
        init_exec(&e, &st, &al);
        e.allocator = &ba;
        e.budget = bud;
        const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_HEIC);
        void *s = NULL;
        CHECK(a->open_session(&e, &s) == RFC_OK);
        cancel_ctx cc = {a, s};
        rfc_codecs_heif_set_band_hook_for_testing(cancel_hook, &cc);
        rfc_frame *fr = NULL;
        size_t fc = 0u;
        CHECK(a->decode_session(s, &e, b, n, NULL, 0u, &fr, &fc, NULL) == RFC_ERR_CANCELLED);
        rfc_codecs_heif_set_band_hook_for_testing(NULL, NULL);
        CHECK(fr == NULL);
        a->close_session(s);
        rfc_registry_release_session(RFC_FORMAT_HEIC);
        rfc_budget_stats bs;
        rfc_budget_stats_get(bud, &bs);
        CHECK(bs.current_allocated_bytes == 0u);
        rfc_budget_destroy(bud);
        free(b);
    }
    /* a scheduled file job cancelled mid-decode leaves the source unchanged
     * and no destination or partial file; an existing destination is never
     * replaced */
    {
        heic_opts o = {64u, 640u, 0, 0, 0, 0, 1};
        uint8_t *b = make_heic(&o, &n);
        char src[512], dst[512], partial[600];
        temp_path(src, sizeof(src), "fj.heic");
        temp_path(dst, sizeof(dst), "fj.png");
        CHECK(write_file_all(src, b, n));
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        rfc_io_source si;
        rfc_io_sink so;
        rfc_io_source_init_path(&si, src, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, dst, RFC_EXPORT_REFUSE_EXISTING);
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = RFC_FORMAT_HEIC;
        sp.target_format = RFC_FORMAT_PNG;
        sp.io_source = &si;
        sp.io_sink = &so;
        g_cancel_job = NULL;
        rfc_codecs_heif_set_band_hook_for_testing(job_cancel_hook, NULL);
        rfc_job *j = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        g_cancel_job = j;
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_codecs_heif_set_band_hook_for_testing(NULL, NULL);
        g_cancel_job = NULL;
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_CANCELLED);
        rfc_job_release(j);
        rfc_scheduler_destroy(sched);
        snprintf(partial, sizeof(partial), "%s.partial", dst);
        CHECK(!file_exists(dst) && !file_exists(partial));
        size_t sn = 0u;
        uint8_t *sb = read_file_all(src, &sn);
        CHECK(sb && sn == n && memcmp(sb, b, n) == 0);
        free(sb);
        static const uint8_t keep[5] = {'k', 'e', 'e', 'p', '!'};
        CHECK(write_file_all(dst, keep, sizeof(keep)));
        job_result r;
        run_job_full(RFC_FORMAT_HEIC, RFC_FORMAT_PNG, src, dst, NULL, NULL, &r);
        CHECK(r.submit == RFC_ERR_DESTINATION_EXISTS || r.status == RFC_ERR_DESTINATION_EXISTS);
        uint8_t *kb = read_file_all(dst, &sn);
        CHECK(kb && sn == sizeof(keep) && memcmp(kb, keep, sizeof(keep)) == 0);
        free(kb);
        remove(dst);
        remove(src);
        free(b);
    }
    printf("heif: wic_metadata_refusals_limits_and_cancel ok\n");
}
#endif /* _WIN32 */

/* ------------------------------------------------------------------------ */
/* CLI harness for tests/converter/test_heif.py                              */
/* ------------------------------------------------------------------------ */

static int cli_main(int argc, char **argv) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_heif() == RFC_OK);
    const char *sub = argv[2];
    if (strcmp(sub, "dump") == 0 && (argc == 5 || argc == 6)) {
        /* dump <in> <raw> [heif|heic] */
        rfc_format_id f = (argc == 6 && strcmp(argv[5], "heif") == 0) ? RFC_FORMAT_HEIF : RFC_FORMAT_HEIC;
        size_t n = 0u;
        uint8_t *b = read_file_all(argv[3], &n);
        if (!b) b = (uint8_t *)calloc(1u, 1u);
        uint8_t *px = NULL;
        rfc_frame info;
        memset(&info, 0, sizeof(info));
        rfc_status r = adapter_decode(f, b, n, "strip", NULL, NULL, &info, &px);
        free(b);
        if (r == RFC_OK) {
            CHECK(write_file_all(argv[4], px, (size_t)info.layout.width * info.layout.height * 3u));
            free(px);
        }
        printf("STATUS=%s WIDTH=%u HEIGHT=%u\n", rfc_status_name(r), info.layout.width, info.layout.height);
        return r == RFC_OK ? 0 : 2;
    }
#ifdef _WIN32
    if (strcmp(sub, "make-heic") == 0 && argc == 7) {
        /* make-heic <out> <w> <h> <orientation> <frames> */
        heic_opts o = {(uint32_t)atoi(argv[4]), (uint32_t)atoi(argv[5]), atoi(argv[6]), 0, 0, 0, 1};
        size_t n = 0u;
        uint8_t *b = make_heic(&o, &n);
        CHECK(write_file_all(argv[3], b, n));
        free(b);
        printf("STATUS=ok BYTES=%zu\n", n);
        return 0;
    }
    if (strcmp(sub, "wic-dump") == 0 && argc == 5) {
        size_t n = 0u;
        uint8_t *b = read_file_all(argv[3], &n);
        CHECK(b != NULL);
        uint32_t w = 0u, h = 0u;
        uint8_t *px = wic_decode_rgb(b, n, &w, &h);
        CHECK(write_file_all(argv[4], px, (size_t)w * h * 3u));
        printf("STATUS=ok WIDTH=%u HEIGHT=%u\n", w, h);
        free(px);
        free(b);
        return 0;
    }
    if (strcmp(sub, "heif-peak") == 0 && argc == 5) {
        /* heif-peak <noop|decode> <file>: peak private bytes of this process */
        size_t n = 0u;
        uint8_t *b = read_file_all(argv[4], &n);
        CHECK(b != NULL);
        rfc_status r = RFC_OK;
        uint64_t reserve = 0u;
        if (strcmp(argv[3], "decode") == 0) {
            /* decode under a budget: its peak is what the adapter actually
             * reserved and allocated through the job allocator */
            rfc_budget *bud = rfc_budget_create(1ull << 32);
            CHECK(bud != NULL);
            r = adapter_decode(RFC_FORMAT_HEIC, b, n, "strip", NULL, bud, NULL, NULL);
            rfc_budget_stats bs;
            rfc_budget_stats_get(bud, &bs);
            reserve = bs.peak_allocated_bytes;
            CHECK(bs.current_allocated_bytes == 0u);
            rfc_budget_destroy(bud);
        }
        PROCESS_MEMORY_COUNTERS_EX pmc;
        memset(&pmc, 0, sizeof(pmc));
        pmc.cb = sizeof(pmc);
        CHECK(K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc)));
        printf("STATUS=%s PEAK=%llu RESERVE=%llu\n", rfc_status_name(r),
               (unsigned long long)pmc.PeakPagefileUsage, (unsigned long long)reserve);
        free(b);
        return r == RFC_OK ? 0 : 2;
    }
#endif
    return 2;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 3 && strcmp(argv[1], "cli") == 0) {
        return cli_main(argc, argv);
    }
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    test_registration_and_scanner();
#ifndef _WIN32
    test_platform_refusal();
#else
    test_wic_decode_matrix();
    test_wic_metadata_and_refusals();
#endif
    CHECK(rfc_codecs_unregister_heif() == RFC_OK);
    printf("heif: all checks passed (%d checks)\n", g_checks);
    return 0;
}
