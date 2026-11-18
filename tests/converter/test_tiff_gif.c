/* C11 codec adapter test suite + CLI harness for D10 T02 s6:
 *   RFC_FORMAT_TIFF (libtiff 4.7.2) and RFC_FORMAT_GIF (giflib 6.1.3):
 *   registration as one transaction, submission geometry, the TIFF import
 *   matrix and refusals, lossless TIFF export with ICC/orientation/XMP, GIF
 *   import with exact LZW length checks, exact-palette GIF export with frame
 *   timing, jobs (byte-identical, refusals, cross-format round trips, file
 *   safety), limits, budget and deterministic mid-decode cancellation. */
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

#include <tiffio.h>

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
    snprintf(buf, cap, "rfc_tg_%d_%u_%s", (int)_getpid(), counter, tag);
#else
    snprintf(buf, cap, "/tmp/rfc_tg_%ld_%u_%s", (long)getpid(), counter, tag);
#endif
    remove(buf);
}

static uint8_t pat(uint32_t x, uint32_t y, uint32_t c, uint32_t seed) {
    return (uint8_t)((x * (3u + c) + y * (7u + 2u * c) + seed * 11u +
                      ((x ^ y) * (c + 1u))) & 0xFFu);
}

/* Decode through the registered session adapter. */
static rfc_status adapter_decode(rfc_format_id f, const uint8_t *b, size_t n,
                                 const rfc_limits *lim, rfc_budget *bud,
                                 rfc_frame *out_copy, uint8_t **out_px) {
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
    rfc_frame *fr = NULL;
    size_t fc = 0u;
    rfc_status r = a->decode_session(s, &e, b, n, NULL, 0u, &fr, &fc, NULL);
    a->close_session(s);
    rfc_registry_release_session(f);
    if (r == RFC_OK) {
        CHECK(fc == 1u);
        if (out_copy) {
            *out_copy = fr[0];
            out_copy->data = NULL;
            out_copy->metadata.icc = NULL;
            out_copy->metadata.xmp = NULL;
            out_copy->metadata.exif = NULL;
        }
        if (out_px) {
            size_t rb = (size_t)fr[0].layout.width *
                        rfc_sample_bytes(fr[0].layout.format);
            uint8_t *px = (uint8_t *)malloc(rb * fr[0].layout.height);
            CHECK(px != NULL);
            for (uint32_t y = 0u; y < fr[0].layout.height; y++) {
                memcpy(px + (size_t)y * rb,
                       fr[0].data + (size_t)y * fr[0].layout.stride_bytes, rb);
            }
            *out_px = px;
        }
        rfc_frame_free(&fr[0]);
        e.allocator->free(e.allocator->ctx, fr);
    }
    if (!bud) CHECK(st.live == 0u);
    return r;
}

static rfc_status adapter_encode(rfc_format_id f, const rfc_layout *lay,
                                 const uint8_t *px, const rfc_metadata *meta,
                                 const char *metadata, uint8_t **out,
                                 size_t *out_n) {
    rfc_exec_context e;
    tracked st;
    rfc_allocator al;
    init_exec(&e, &st, &al);
    rfc_frame fr;
    memset(&fr, 0, sizeof(fr));
    rfc_layout norm;
    if (rfc_layout_normalize_stride(lay, &norm) != RFC_OK) {
        return RFC_ERR_UNSUPPORTED_SAMPLE; /* e.g. a 12-bit rgb16 layout */
    }
    lay = &norm;
    CHECK(rfc_frame_alloc(&fr, lay, meta, &al) == RFC_OK);
    size_t rb = (size_t)lay->width * rfc_sample_bytes(lay->format);
    for (uint32_t y = 0u; y < lay->height; y++) {
        memcpy(fr.data + (size_t)y * fr.layout.stride_bytes, px + (size_t)y * rb, rb);
    }
    const rfc_session_adapter *a = rfc_registry_acquire_session(f);
    CHECK(a != NULL);
    void *s = NULL;
    CHECK(a->open_session(&e, &s) == RFC_OK);
    rfc_setting_value sv[1] = {{"metadata", metadata ? metadata : "preserve"}};
    rfc_encoded enc;
    memset(&enc, 0, sizeof(enc));
    rfc_status r = a->encode_session(s, &e, &fr, 1u, meta, sv, 1u, &enc);
    a->close_session(s);
    rfc_registry_release_session(f);
    if (r == RFC_OK && out) {
        *out = (uint8_t *)malloc(enc.len);
        CHECK(*out != NULL);
        memcpy(*out, enc.data, enc.len);
        *out_n = enc.len;
    }
    rfc_encoded_free(&enc);
    rfc_frame_free(&fr);
    CHECK(st.live == 0u);
    return r;
}

typedef struct job_result {
    rfc_status submit, status;
    rfc_job_state state;
    rfc_job_stage failing;
    int has_rep;
    rfc_verify_report rep;
    uint64_t budget_limit, budget_peak;
    char detail[RFC_JOB_DETAIL_MAX];
} job_result;

static void run_job_full(rfc_format_id sf, rfc_format_id df, const char *src,
                         const uint8_t *src_bytes, size_t src_size,
                         const char *dst, const char *exactness,
                         const char *meta, const char *frames,
                         const rfc_limits *lim, uint64_t working_set,
                         job_result *out) {
    memset(out, 0, sizeof(*out));
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);
    rfc_io_source si;
    rfc_io_sink so;
    if (src) rfc_io_source_init_path(&si, src, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&so, dst, RFC_EXPORT_REFUSE_EXISTING);
    rfc_setting_value sv[3];
    size_t n = 0u;
    if (exactness) { sv[n].id = "exactness"; sv[n].value = exactness; n++; }
    if (meta) { sv[n].id = "metadata"; sv[n].value = meta; n++; }
    if (frames) { sv[n].id = "frames"; sv[n].value = frames; n++; }
    rfc_job_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_format = sf;
    sp.target_format = df;
    if (src) {
        sp.io_source = &si;
    } else {
        sp.source_bytes = src_bytes;
        sp.source_size = src_size;
    }
    sp.io_sink = &so;
    sp.settings = n ? sv : NULL;
    sp.setting_count = n;
    sp.limits_override = lim;
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
        out->budget_limit = snap.budget.limit_bytes;
        out->budget_peak = snap.budget.peak_allocated_bytes;
        memcpy(out->detail, snap.detail, sizeof(out->detail));
        out->has_rep = rfc_job_verify_report_get(j, &out->rep) == RFC_OK;
        rfc_job_release(j);
    }
    rfc_scheduler_destroy(sched);
}

/* A job expected to succeed; on failure the statuses and stage are printed. */
#define CHECK_JOB_OK(r)                                                        \
    do {                                                                       \
        if ((r).submit != RFC_OK || (r).state != RFC_JOB_SUCCEEDED) {          \
            fprintf(stderr, "job: submit=%s status=%s stage=%u\n",             \
                    rfc_status_name((r).submit), rfc_status_name((r).status),  \
                    (unsigned)(r).failing);                                    \
        }                                                                      \
        CHECK((r).submit == RFC_OK && (r).state == RFC_JOB_SUCCEEDED);         \
    } while (0)

static void run_job(rfc_format_id sf, rfc_format_id df, const char *src,
                    const char *dst, const char *exactness, const char *meta,
                    job_result *out) {
    run_job_full(sf, df, src, NULL, 0u, dst, exactness, meta, NULL, NULL, 0u, out);
}

/* ------------------------------------------------------------------------ */
/* TIFF fixtures through libtiff                                             */
/* ------------------------------------------------------------------------ */

typedef struct tmk {
    uint32_t w, h;
    uint16_t spp, bps, photometric, compression, predictor, planar;
    int alpha;          /* 1 unassociated, 2 associated */
    int tiled;
    uint16_t orientation;
    const uint8_t *icc;
    uint32_t icc_len;
    const char *xmp;
    int pages;          /* > 1 writes more IFDs */
    int subifd, exif, whitepoint;
    uint16_t sampleformat;
    uint32_t seed;
    int flat;
    int bigtiff;
    uint32_t tile; /* tile edge when tiled (16 when 0) */
} tmk;

static void tmk_defaults(tmk *t, uint32_t w, uint32_t h, uint16_t spp,
                         uint16_t bps) {
    memset(t, 0, sizeof(*t));
    t->w = w;
    t->h = h;
    t->spp = spp;
    t->bps = bps;
    t->photometric = spp >= 3u ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK;
    t->compression = COMPRESSION_NONE;
    t->predictor = 1u;
    t->planar = PLANARCONFIG_CONTIG;
    t->sampleformat = SAMPLEFORMAT_UINT;
    t->alpha = spp == 4u ? 1 : 0;
}

/* Expected native pixels (little-endian 16-bit) for a fixture. */
static uint8_t *tmk_pixels(const tmk *t) {
    size_t bpsb = t->bps / 8u;
    size_t n = (size_t)t->w * t->h * t->spp * bpsb;
    uint8_t *px = (uint8_t *)malloc(n);
    CHECK(px != NULL);
    size_t o = 0u;
    for (uint32_t y = 0u; y < t->h; y++)
        for (uint32_t x = 0u; x < t->w; x++)
            for (uint32_t c = 0u; c < t->spp; c++) {
                uint8_t v = t->flat ? (uint8_t)(40u + c) : pat(x, y, c, t->seed);
                if (bpsb == 1u) {
                    px[o++] = v;
                } else {
                    uint16_t w16 = (uint16_t)(v * 257u ^ (x * 13u + c));
                    px[o++] = (uint8_t)w16;
                    px[o++] = (uint8_t)(w16 >> 8);
                }
            }
    return px;
}

static uint8_t *make_tiff(const tmk *t, size_t *out_n) {
    char path[512];
    temp_path(path, sizeof(path), "mk.tif");
    TIFF *tif = TIFFOpen(path, t->bigtiff ? "w8" : "w");
    CHECK(tif != NULL);
    uint8_t *px = tmk_pixels(t);
    size_t bpsb = t->bps / 8u;
    for (int page = 0; page < (t->pages ? t->pages : 1); page++) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, t->w);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, t->h);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, t->spp);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, t->bps);
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, t->sampleformat);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, t->photometric);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, t->planar);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, t->compression);
        if (t->compression == COMPRESSION_WEBP) {
            TIFFSetField(tif, TIFFTAG_WEBP_LOSSLESS, 1);
        }
        if (t->predictor > 1u) TIFFSetField(tif, TIFFTAG_PREDICTOR, t->predictor);
        if (t->alpha) {
            uint16_t ex = t->alpha == 2 ? EXTRASAMPLE_ASSOCALPHA : EXTRASAMPLE_UNASSALPHA;
            TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &ex);
        }
        if (t->photometric == PHOTOMETRIC_PALETTE) {
            static uint16_t cmap[3][256];
            for (int i = 0; i < 256; i++) {
                cmap[0][i] = (uint16_t)(i * 257);
                cmap[1][i] = (uint16_t)((255 - i) * 257);
                cmap[2][i] = (uint16_t)((i * 7 & 255) * 257);
            }
            TIFFSetField(tif, TIFFTAG_COLORMAP, cmap[0], cmap[1], cmap[2]);
        }
        if (t->orientation) TIFFSetField(tif, TIFFTAG_ORIENTATION, t->orientation);
        if (t->icc) TIFFSetField(tif, TIFFTAG_ICCPROFILE, t->icc_len, t->icc);
        if (t->xmp) TIFFSetField(tif, TIFFTAG_XMLPACKET, (uint32_t)strlen(t->xmp), t->xmp);
        if (t->whitepoint) {
            float wp[2] = {0.3127f, 0.329f};
            TIFFSetField(tif, TIFFTAG_WHITEPOINT, wp);
        }
        if (t->subifd) {
            uint64_t off[1] = {0u};
            TIFFSetField(tif, TIFFTAG_SUBIFD, 1, off);
        }
        uint32_t ch = t->planar == PLANARCONFIG_SEPARATE ? 1u : t->spp;
        uint32_t planes = t->planar == PLANARCONFIG_SEPARATE ? t->spp : 1u;
        if (t->tiled) {
            uint32_t T = t->tile ? t->tile : 16u;
            TIFFSetField(tif, TIFFTAG_TILEWIDTH, T);
            TIFFSetField(tif, TIFFTAG_TILELENGTH, T);
            uint8_t *tile = (uint8_t *)calloc((size_t)T * T * ch * bpsb, 1u);
            CHECK(tile != NULL);
            for (uint32_t p = 0u; p < planes; p++)
                for (uint32_t ty = 0u; ty < t->h; ty += T)
                    for (uint32_t tx = 0u; tx < t->w; tx += T) {
                        memset(tile, 0, (size_t)T * T * ch * bpsb);
                        for (uint32_t r = 0u; r < T && ty + r < t->h; r++)
                            for (uint32_t x = 0u; x < T && tx + x < t->w; x++)
                                for (uint32_t c = 0u; c < ch; c++) {
                                    uint32_t sc = planes > 1u ? p : c;
                                    memcpy(tile + ((size_t)(r * T + x) * ch + c) * bpsb,
                                           px + (((size_t)(ty + r) * t->w + tx + x) * t->spp + sc) * bpsb,
                                           bpsb);
                                }
                        CHECK(TIFFWriteTile(tif, tile, tx, ty, 0, (uint16_t)p) >= 0);
                    }
            free(tile);
        } else {
            /* JPEG strips hold whole 8-row (16 for YCbCr 2x2) MCU rows */
            TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP,
                         t->compression == COMPRESSION_JPEG ? 16u : 7u);
            if (t->compression == COMPRESSION_JPEG &&
                t->photometric == PHOTOMETRIC_YCBCR) {
                TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
            }
            uint8_t *row = (uint8_t *)malloc((size_t)t->w * ch * bpsb);
            CHECK(row != NULL);
            for (uint32_t p = 0u; p < planes; p++)
                for (uint32_t y = 0u; y < t->h; y++) {
                    for (uint32_t x = 0u; x < t->w; x++)
                        for (uint32_t c = 0u; c < ch; c++) {
                            uint32_t sc = planes > 1u ? p : c;
                            memcpy(row + ((size_t)x * ch + c) * bpsb,
                                   px + (((size_t)y * t->w + x) * t->spp + sc) * bpsb, bpsb);
                        }
                    CHECK(TIFFWriteScanline(tif, row, y, (uint16_t)p) >= 0);
                }
            free(row);
        }
        if (t->exif) {
            uint64_t dir_off = 0u;
            CHECK(TIFFWriteDirectory(tif));
            CHECK(TIFFCreateEXIFDirectory(tif) == 0);
            TIFFSetField(tif, EXIFTAG_EXPOSURETIME, 0.01);
            CHECK(TIFFWriteCustomDirectory(tif, &dir_off));
            CHECK(TIFFSetDirectory(tif, 0));
            TIFFSetField(tif, TIFFTAG_EXIFIFD, dir_off);
            CHECK(TIFFRewriteDirectory(tif));
        } else {
            CHECK(TIFFWriteDirectory(tif));
        }
    }
    TIFFClose(tif);
    free(px);
    uint8_t *b = read_file_all(path, out_n);
    CHECK(b != NULL);
    remove(path);
    return b;
}

/* ------------------------------------------------------------------------ */
/* GIF fixtures by hand (LZW written here so lengths can be wrong)          */
/* ------------------------------------------------------------------------ */

typedef struct bits {
    uint8_t *buf;
    size_t n;
    uint32_t acc, nacc;
} bits;

static void bits_put(bits *b, uint32_t code, uint32_t width) {
    b->acc |= code << b->nacc;
    b->nacc += width;
    while (b->nacc >= 8u) {
        b->buf[b->n++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->nacc -= 8u;
    }
}

/* Literal-only GIF LZW with min code size 2: a clear before every pair of
 * literals keeps the code width at 3. extra/short_by alter the pixel count;
 * eoi controls the end code. */
static size_t gif_lzw(const uint8_t *idx, size_t count, int eoi, uint8_t *out) {
    bits *b = (bits *)calloc(1u, sizeof(bits));
    CHECK(b != NULL);
    b->buf = (uint8_t *)malloc(count + 16u); /* <= 9 bits per index */
    CHECK(b->buf != NULL);
    for (size_t i = 0u; i < count; i += 2u) {
        bits_put(b, 4u, 3u);
        bits_put(b, idx[i], 3u);
        if (i + 1u < count) bits_put(b, idx[i + 1u], 3u);
    }
    if (eoi) bits_put(b, 5u, 3u);
    if (b->nacc) b->buf[b->n++] = (uint8_t)b->acc;
    size_t o = 0u;
    for (size_t p = 0u; p < b->n; p += 255u) {
        size_t k = b->n - p < 255u ? b->n - p : 255u;
        out[o++] = (uint8_t)k;
        memcpy(out + o, b->buf + p, k);
        o += k;
    }
    out[o++] = 0u;
    free(b->buf);
    free(b);
    return o;
}

typedef struct gmk {
    uint32_t w, h;
    int transparent;     /* -1 none, else index */
    uint16_t delay_cs;
    int images;
    int partial;
    int interlace;
    int pixel_delta;     /* +1 excess, -1 short */
    int no_eoi;
    int bad_index;
    int no_trailer;
    int plain_text;      /* a Plain Text Extension before the image */
    int gce_shape;       /* 1: zero-length GCE, 2: GCE with an extra block */
    uint32_t comment_blocks; /* a Comment Extension of this many 1-byte blocks */
} gmk;

static const uint8_t k_gct[12] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};

static uint8_t gmk_index(uint32_t x, uint32_t y) {
    return (uint8_t)((x + 2u * y) & 3u);
}

static uint8_t *make_gif(const gmk *g, size_t *out_n) {
    size_t cap = 64u + 2u * (size_t)g->w * g->h + 4096u + 2u * (size_t)g->comment_blocks;
    uint8_t *o = (uint8_t *)malloc(cap * (g->images > 1 ? (size_t)g->images : 1u));
    CHECK(o != NULL);
    size_t n = 0u;
    memcpy(o, "GIF89a", 6);
    n = 6u;
    o[n++] = (uint8_t)g->w; o[n++] = (uint8_t)(g->w >> 8);
    o[n++] = (uint8_t)g->h; o[n++] = (uint8_t)(g->h >> 8);
    o[n++] = 0x81u; /* GCT, 4 entries */
    o[n++] = 0u;
    o[n++] = 0u;
    memcpy(o + n, k_gct, sizeof(k_gct));
    n += sizeof(k_gct);
    if (g->comment_blocks) {
        o[n++] = 0x21u;
        o[n++] = 0xFEu;
        for (uint32_t k = 0u; k < g->comment_blocks; k++) {
            o[n++] = 1u;
            o[n++] = (uint8_t)('a' + k % 26u);
        }
        o[n++] = 0u;
    }
    if (g->plain_text) {
        static const uint8_t pte[19] = {0x21u, 0x01u, 12u, 0, 0, 0, 0, 8, 0, 8, 0,
                                        8, 8, 1, 0, 2u, 'h', 'i', 0u};
        memcpy(o + n, pte, sizeof(pte));
        n += sizeof(pte);
    }
    if (g->gce_shape == 1) {
        static const uint8_t z[3] = {0x21u, 0xF9u, 0u};
        memcpy(o + n, z, sizeof(z));
        n += sizeof(z);
    } else if (g->gce_shape == 2) {
        static const uint8_t x[10] = {0x21u, 0xF9u, 4u, 0, 0, 0, 0, 1u, 0xAAu, 0u};
        memcpy(o + n, x, sizeof(x));
        n += sizeof(x);
    }
    for (int im = 0; im < (g->images ? g->images : 1); im++) {
        if (g->transparent >= 0 || g->delay_cs) {
            uint8_t gce[8] = {0x21u, 0xF9u, 4u,
                              (uint8_t)(g->transparent >= 0 ? 1u : 0u),
                              (uint8_t)g->delay_cs, (uint8_t)(g->delay_cs >> 8),
                              (uint8_t)(g->transparent >= 0 ? g->transparent : 0), 0u};
            memcpy(o + n, gce, sizeof(gce));
            n += sizeof(gce);
        }
        uint32_t w = g->partial ? g->w - 1u : g->w;
        o[n++] = 0x2Cu;
        o[n++] = 0u; o[n++] = 0u; o[n++] = 0u; o[n++] = 0u;
        o[n++] = (uint8_t)w; o[n++] = (uint8_t)(w >> 8);
        o[n++] = (uint8_t)g->h; o[n++] = (uint8_t)(g->h >> 8);
        o[n++] = g->interlace ? 0x40u : 0u;
        o[n++] = 2u; /* min code size */
        size_t count = (size_t)w * g->h;
        uint8_t *idx = (uint8_t *)malloc(count + 2u);
        CHECK(idx != NULL);
        static const uint32_t start[4] = {0u, 4u, 2u, 1u}, step[4] = {8u, 8u, 4u, 2u};
        size_t k = 0u;
        for (uint32_t pass = 0u; pass < (g->interlace ? 4u : 1u); pass++)
            for (uint32_t y = g->interlace ? start[pass] : 0u; y < g->h;
                 y += g->interlace ? step[pass] : 1u)
                for (uint32_t x = 0u; x < w; x++) idx[k++] = gmk_index(x, y);
        if (g->bad_index) idx[0] = 6u; /* beyond a 4-entry table: code 6 */
        if (g->pixel_delta > 0) idx[count++] = 1u;
        if (g->pixel_delta < 0) count--;
        n += gif_lzw(idx, count, !g->no_eoi, o + n);
        free(idx);
    }
    if (!g->no_trailer) o[n++] = 0x3Bu;
    *out_n = n;
    return o;
}

static void gmk_defaults(gmk *g, uint32_t w, uint32_t h) {
    memset(g, 0, sizeof(*g));
    g->w = w;
    g->h = h;
    g->transparent = -1;
}

/* ------------------------------------------------------------------------ */
/* 1. Registration and submission geometry                                   */
/* ------------------------------------------------------------------------ */

static void test_registration_and_geometry(void) {
    const rfc_capability *t = rfc_registry_find(RFC_FORMAT_TIFF);
    const rfc_capability *g = rfc_registry_find(RFC_FORMAT_GIF);
    CHECK(t && g && t->availability == RFC_AVAILABILITY_USABLE &&
          g->availability == RFC_AVAILABILITY_USABLE);
    CHECK(t->metadata_items == (RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION |
                                RFC_META_ITEM_XMP));
    CHECK(g->metadata_items == RFC_META_ITEM_FRAME_TIMING);
    CHECK(strstr(g->provider, "giflib") != NULL);

    /* One transaction: a foreign GIF adapter blocks both. */
    static const rfc_session_adapter foreign = {
        RFC_CODEC_ABI_VERSION, RFC_FORMAT_GIF, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    (void)foreign;
    CHECK(rfc_codecs_register_tiff_gif() == RFC_OK);
    CHECK(rfc_codecs_register_tiff_gif() == RFC_ERR_DUPLICATE);
    const rfc_session_adapter *ga = rfc_registry_acquire_session(RFC_FORMAT_GIF);
    CHECK(ga != NULL);
    CHECK(rfc_codecs_unregister_tiff_gif() == RFC_ERR_BUSY);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_TIFF) != NULL);
    rfc_registry_release_session(RFC_FORMAT_GIF);
    CHECK(rfc_codecs_unregister_tiff_gif() == RFC_OK);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_TIFF) == NULL &&
          rfc_registry_session_adapter(RFC_FORMAT_GIF) == NULL);
    /* GIF already taken: the pair refuses and TIFF stays unregistered. */
    const rfc_session_adapter *tiff_a = NULL;
    CHECK(rfc_codecs_register_tiff_gif() == RFC_OK);
    tiff_a = rfc_registry_session_adapter(RFC_FORMAT_TIFF);
    const rfc_session_adapter *gif_a = rfc_registry_session_adapter(RFC_FORMAT_GIF);
    CHECK(rfc_codecs_unregister_tiff_gif() == RFC_OK);
    CHECK(rfc_registry_register_session(gif_a) == RFC_OK);
    CHECK(rfc_codecs_register_tiff_gif() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_TIFF) == NULL);
    CHECK(rfc_registry_unregister_session(RFC_FORMAT_GIF) == RFC_OK);
    CHECK(rfc_codecs_register_tiff_gif() == RFC_OK);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_TIFF) == tiff_a);

    tmk m;
    gmk gm;
    size_t n = 0u;
    uint8_t *tb = NULL, *gb = NULL;
    /* Default submissions (no layout, no estimate) of large compressible
     * images succeed by path and by bytes; a lower max_width refuses. */
    tmk_defaults(&m, 4096u, 4096u, 3u, 8u);
    m.flat = 1;
    m.compression = COMPRESSION_ADOBE_DEFLATE;
    tb = make_tiff(&m, &n);
    CHECK(n < 4u * 1024u * 1024u);
    char p_src[512], p_dst[512];
    temp_path(p_src, sizeof(p_src), "big.tif");
    temp_path(p_dst, sizeof(p_dst), "big_out.tif");
    CHECK(write_file_all(p_src, tb, n));
    job_result r;
    run_job(RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, p_src, p_dst, NULL, NULL, &r);
    CHECK_JOB_OK(r);
    remove(p_dst);
    run_job_full(RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, NULL, tb, n, p_dst, NULL, NULL,
                 NULL, NULL, 0u, &r);
    CHECK_JOB_OK(r);
    remove(p_dst);
    rfc_limits lim;
    rfc_limits_default(&lim);
    lim.max_width = 1024u;
    run_job_full(RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, p_src, NULL, 0u, p_dst, NULL,
                 NULL, NULL, &lim, 0u, &r);
    CHECK(r.submit == RFC_ERR_LIMIT_EXCEEDED && !file_exists(p_dst));
    remove(p_src);
    free(tb);
    gmk_defaults(&gm, 4096u, 1024u);
    gb = make_gif(&gm, &n);
    temp_path(p_src, sizeof(p_src), "big.gif");
    temp_path(p_dst, sizeof(p_dst), "big_out.png");
    CHECK(write_file_all(p_src, gb, n));
    run_job(RFC_FORMAT_GIF, RFC_FORMAT_PNG, p_src, p_dst, NULL, NULL, &r);
    CHECK_JOB_OK(r);
    remove(p_src);
    remove(p_dst);
    free(gb);
    printf("tiff_gif: registration_and_geometry ok\n");
}

/* ------------------------------------------------------------------------ */
/* 2. TIFF import matrix and refusals                                        */
/* ------------------------------------------------------------------------ */

static void tiff_roundtrip_check(const tmk *m) {
    size_t n = 0u;
    uint8_t *b = make_tiff(m, &n);
    uint8_t *px = NULL;
    rfc_frame info;
    rfc_status st = adapter_decode(RFC_FORMAT_TIFF, b, n, NULL, NULL, &info, &px);
    if (st != RFC_OK) {
        fprintf(stderr, "tiff decode %s: spp=%u bps=%u comp=%u tiled=%d planar=%u\n",
                rfc_status_name(st), m->spp, m->bps, m->compression, m->tiled, m->planar);
    }
    CHECK(st == RFC_OK);
    uint8_t *want = tmk_pixels(m);
    CHECK(memcmp(px, want, (size_t)m->w * m->h * m->spp * (m->bps / 8u)) == 0);
    free(want);
    free(px);
    free(b);
}

static void test_tiff_import_matrix_and_refusals(void) {
    static const uint16_t comps[] = {COMPRESSION_NONE, COMPRESSION_LZW,
                                     COMPRESSION_ADOBE_DEFLATE, COMPRESSION_PACKBITS};
    static const uint16_t spps[] = {1u, 3u, 4u};
    for (size_t ci = 0u; ci < 4u; ci++)
        for (size_t si = 0u; si < 3u; si++)
            for (uint16_t bps = 8u; bps <= 16u; bps = (uint16_t)(bps + 8u))
                for (int tiled = 0; tiled < 2; tiled++)
                    for (int sep = 0; sep < 2; sep++) {
                        tmk m;
                        tmk_defaults(&m, 37u, 29u, spps[si], bps);
                        m.compression = comps[ci];
                        m.tiled = tiled;
                        m.planar = sep ? PLANARCONFIG_SEPARATE : PLANARCONFIG_CONTIG;
                        m.predictor = (comps[ci] == COMPRESSION_LZW ||
                                       comps[ci] == COMPRESSION_ADOBE_DEFLATE) &&
                                              !sep
                                          ? 2u
                                          : 1u;
                        m.seed = (uint32_t)(ci * 31u + si * 7u + bps + (unsigned)tiled * 3u);
                        tiff_roundtrip_check(&m);
                    }
    /* Metadata reported, orientation not applied. */
    {
        static uint8_t icc[300];
        for (int i = 0; i < 300; i++) icc[i] = (uint8_t)(i * 7);
        tmk m;
        tmk_defaults(&m, 10u, 6u, 3u, 8u);
        m.icc = icc;
        m.icc_len = sizeof(icc);
        m.xmp = "<x:xmpmeta>tiff</x:xmpmeta>";
        m.orientation = 6u;
        size_t n = 0u;
        uint8_t *b = make_tiff(&m, &n);
        rfc_frame info;
        CHECK(adapter_decode(RFC_FORMAT_TIFF, b, n, NULL, NULL, &info, NULL) == RFC_OK);
        CHECK(info.metadata.icc_len == 300u && info.metadata.xmp_len == 27u &&
              info.metadata.orientation == RFC_ORIENTATION_ROTATE_90);
        free(b);
    }
    /* Refusals before pixel allocation. */
    struct {
        const char *what;
        rfc_status want;
    } cases[] = {
        {"pages", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"subifd", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"exif", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"whitepoint", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"assoc", RFC_ERR_UNSUPPORTED_ALPHA},
        {"palette", RFC_ERR_UNSUPPORTED_SAMPLE},
        {"miniswhite", RFC_ERR_UNSUPPORTED_SAMPLE},
        {"float", RFC_ERR_UNSUPPORTED_SAMPLE},
        {"bps4", RFC_ERR_UNSUPPORTED_SAMPLE},
    };
    for (size_t k = 0u; k < sizeof(cases) / sizeof(cases[0]); k++) {
        tmk m;
        tmk_defaults(&m, 12u, 9u, 3u, 8u);
        const char *w = cases[k].what;
        if (!strcmp(w, "pages")) m.pages = 2;
        if (!strcmp(w, "subifd")) m.subifd = 1;
        if (!strcmp(w, "exif")) m.exif = 1;
        if (!strcmp(w, "whitepoint")) m.whitepoint = 1;
        if (!strcmp(w, "assoc")) { m.spp = 4u; m.alpha = 2; }
        if (!strcmp(w, "palette")) { m.spp = 1u; m.photometric = PHOTOMETRIC_PALETTE; }
        if (!strcmp(w, "miniswhite")) { m.spp = 1u; m.photometric = PHOTOMETRIC_MINISWHITE; }
        if (!strcmp(w, "float")) { m.bps = 16u; m.sampleformat = SAMPLEFORMAT_IEEEFP; }
        if (!strcmp(w, "bps4")) { m.spp = 1u; m.bps = 8u; }
        size_t n = 0u;
        uint8_t *b = NULL;
        if (!strcmp(w, "bps4")) {
            /* libtiff writes 4-bit gray; build it then patch nothing else */
            m.bps = 8u;
            b = make_tiff(&m, &n);
            /* rewrite BitsPerSample entry value 8 -> 4 in the first IFD */
            int le = b[0] == 'I';
            uint32_t ifd = le ? (uint32_t)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24)
                              : (uint32_t)((uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]);
            uint32_t cnt = le ? (uint32_t)(b[ifd] | b[ifd + 1] << 8) : (uint32_t)(b[ifd] << 8 | b[ifd + 1]);
            for (uint32_t e = 0u; e < cnt; e++) {
                uint8_t *ent = b + ifd + 2u + 12u * e;
                uint32_t tag = le ? (uint32_t)(ent[0] | ent[1] << 8) : (uint32_t)(ent[0] << 8 | ent[1]);
                if (tag == 258u) { if (le) ent[8] = 4u; else ent[9] = 4u; }
            }
        } else {
            b = make_tiff(&m, &n);
        }
        rfc_status st = adapter_decode(RFC_FORMAT_TIFF, b, n, NULL, NULL, NULL, NULL);
        if (st != cases[k].want) {
            fprintf(stderr, "tiff refusal %s: got %s\n", w, rfc_status_name(st));
        }
        CHECK(st == cases[k].want);
        free(b);
    }
    /* Truncation and garbage. */
    {
        tmk m;
        tmk_defaults(&m, 40u, 40u, 3u, 8u);
        size_t n = 0u;
        uint8_t *b = make_tiff(&m, &n);
        rfc_status st = adapter_decode(RFC_FORMAT_TIFF, b, n / 2u, NULL, NULL, NULL, NULL);
        CHECK(st == RFC_ERR_TRUNCATED_INPUT || st == RFC_ERR_INVALID_ARGUMENT);
        uint8_t junk[64];
        memset(junk, 0x49, sizeof(junk));
        CHECK(adapter_decode(RFC_FORMAT_TIFF, junk, sizeof(junk), NULL, NULL, NULL, NULL) != RFC_OK);
        free(b);
    }
    printf("tiff_gif: tiff_import_matrix_and_refusals ok\n");
}

/* ------------------------------------------------------------------------ */
/* 3. TIFF export and metadata                                               */
/* ------------------------------------------------------------------------ */

static void test_tiff_export_and_metadata(void) {
    static const rfc_sample_format fmts[] = {RFC_SAMPLE_GRAY8, RFC_SAMPLE_GRAY16,
                                             RFC_SAMPLE_RGB8, RFC_SAMPLE_RGB16,
                                             RFC_SAMPLE_RGBA8, RFC_SAMPLE_RGBA16};
    for (size_t k = 0u; k < 6u; k++) {
        rfc_layout lay;
        memset(&lay, 0, sizeof(lay));
        lay.format = fmts[k];
        lay.alpha = (fmts[k] == RFC_SAMPLE_RGBA8 || fmts[k] == RFC_SAMPLE_RGBA16)
                        ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
        lay.width = 41u;
        lay.height = 23u;
        lay.bits_per_sample = (fmts[k] == RFC_SAMPLE_GRAY16 || fmts[k] == RFC_SAMPLE_RGB16 ||
                               fmts[k] == RFC_SAMPLE_RGBA16) ? 16u : 8u;
        size_t sb = rfc_sample_bytes(fmts[k]);
        size_t total = sb * lay.width * lay.height;
        uint8_t *px = (uint8_t *)malloc(total);
        CHECK(px != NULL);
        for (size_t i = 0u; i < total; i++) px[i] = (uint8_t)(i * 131u + k);
        static uint8_t icc[64] = {1, 2, 3};
        rfc_metadata meta = {icc, sizeof(icc), RFC_ORIENTATION_FLIP_HORIZONTAL, 0u, {0},
                             NULL, 0u, (const uint8_t *)"<xmp/>", 6u};
        for (int strip = 0; strip < 2; strip++) {
            uint8_t *out = NULL;
            size_t on = 0u;
            CHECK(adapter_encode(RFC_FORMAT_TIFF, &lay, px, &meta,
                                 strip ? "strip" : "preserve", &out, &on) == RFC_OK);
            uint8_t *back = NULL;
            rfc_frame info;
            CHECK(adapter_decode(RFC_FORMAT_TIFF, out, on, NULL, NULL, &info, &back) == RFC_OK);
            CHECK(info.layout.format == fmts[k] && memcmp(back, px, total) == 0);
            if (strip) {
                CHECK(info.metadata.icc_len == 0u && info.metadata.xmp_len == 0u);
            } else {
                CHECK(info.metadata.icc_len == sizeof(icc) && info.metadata.xmp_len == 6u &&
                      info.metadata.orientation == RFC_ORIENTATION_FLIP_HORIZONTAL);
            }
            free(back);
            free(out);
        }
        free(px);
    }
    /* Reduced depths are refused, not widened; standalone EXIF is policy. */
    {
        rfc_layout lay = {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 4u, 4u, 0u, 12u};
        uint8_t px[4 * 4 * 6] = {0};
        CHECK(adapter_encode(RFC_FORMAT_TIFF, &lay, px, NULL, "preserve", NULL, NULL) ==
              RFC_ERR_UNSUPPORTED_SAMPLE);
        rfc_layout l8 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 0u, 8u};
        static const uint8_t exif[26] = {'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 1, 0x12, 0, 3, 0, 0, 0, 1, 0, 1};
        rfc_metadata m = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0}, exif, sizeof(exif), NULL, 0u};
        CHECK(adapter_encode(RFC_FORMAT_TIFF, &l8, px, &m, "preserve", NULL, NULL) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(adapter_encode(RFC_FORMAT_TIFF, &l8, px, &m, "strip", NULL, NULL) == RFC_OK);
    }
    printf("tiff_gif: tiff_export_and_metadata ok\n");
}

/* ------------------------------------------------------------------------ */
/* 4. GIF import, LZW lengths and refusals                                   */
/* ------------------------------------------------------------------------ */

static void test_gif_import_lzw_and_refusals(void) {
    for (int inter = 0; inter < 2; inter++)
        for (int tr = -1; tr < 2; tr++) {
            gmk g;
            gmk_defaults(&g, 23u, 17u);
            g.interlace = inter;
            g.transparent = tr;
            g.delay_cs = tr == 1 ? 12u : 0u;
            size_t n = 0u;
            uint8_t *b = make_gif(&g, &n);
            uint8_t *px = NULL;
            rfc_frame info;
            CHECK(adapter_decode(RFC_FORMAT_GIF, b, n, NULL, NULL, &info, &px) == RFC_OK);
            uint32_t ch = tr >= 0 ? 4u : 3u;
            CHECK(info.layout.format == (tr >= 0 ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8));
            CHECK(info.metadata.duration_ms == (uint32_t)g.delay_cs * 10u);
            for (uint32_t y = 0u; y < g.h; y++)
                for (uint32_t x = 0u; x < g.w; x++) {
                    uint8_t idx = gmk_index(x, y);
                    const uint8_t *p = px + ((size_t)y * g.w + x) * ch;
                    CHECK(p[0] == k_gct[idx * 3u] && p[1] == k_gct[idx * 3u + 1u] &&
                          p[2] == k_gct[idx * 3u + 2u]);
                    if (ch == 4u) CHECK(p[3] == (idx == (uint8_t)tr ? 0u : 255u));
                }
            free(px);
            free(b);
        }
    struct {
        const char *what;
        rfc_status want;
    } cases[] = {
        {"excess", RFC_ERR_INVALID_ARGUMENT},     {"short", RFC_ERR_INVALID_ARGUMENT},
        {"no_eoi", RFC_ERR_INVALID_ARGUMENT},     {"bad_index", RFC_ERR_INVALID_ARGUMENT},
        {"two_images", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"partial", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"no_trailer", RFC_ERR_TRUNCATED_INPUT},
        {"plain_text", RFC_ERR_UNSUPPORTED_COMBINATION},
        {"gce_zero", RFC_ERR_INVALID_ARGUMENT},
        {"gce_extra", RFC_ERR_INVALID_ARGUMENT},
    };
    for (size_t k = 0u; k < sizeof(cases) / sizeof(cases[0]); k++) {
        gmk g;
        gmk_defaults(&g, 9u, 5u);
        const char *w = cases[k].what;
        if (!strcmp(w, "excess")) g.pixel_delta = 1;
        if (!strcmp(w, "short")) g.pixel_delta = -1;
        if (!strcmp(w, "no_eoi")) g.no_eoi = 1;
        if (!strcmp(w, "bad_index")) g.bad_index = 1;
        if (!strcmp(w, "two_images")) g.images = 2;
        if (!strcmp(w, "partial")) g.partial = 1;
        if (!strcmp(w, "no_trailer")) g.no_trailer = 1;
        if (!strcmp(w, "plain_text")) g.plain_text = 1;
        if (!strcmp(w, "gce_zero")) g.gce_shape = 1;
        if (!strcmp(w, "gce_extra")) g.gce_shape = 2;
        size_t n = 0u;
        uint8_t *b = make_gif(&g, &n);
        rfc_status st = adapter_decode(RFC_FORMAT_GIF, b, n, NULL, NULL, NULL, NULL);
        if (st != cases[k].want) fprintf(stderr, "gif %s: got %s\n", w, rfc_status_name(st));
        CHECK(st == cases[k].want);
        free(b);
    }
    /* Too many extensions and over-limit extension bytes. */
    {
        gmk g;
        gmk_defaults(&g, 4u, 4u);
        size_t n = 0u;
        uint8_t *b = make_gif(&g, &n);
        size_t big = n + 70u * 6u;
        uint8_t *x = (uint8_t *)malloc(big);
        CHECK(x != NULL);
        memcpy(x, b, 25u); /* header, LSD, GCT */
        size_t o = 25u;
        for (int e = 0; e < 70; e++) {
            uint8_t ce[6] = {0x21u, 0xFEu, 2u, 'h', 'i', 0u};
            memcpy(x + o, ce, 6u);
            o += 6u;
        }
        memcpy(x + o, b + 25u, n - 25u);
        o += n - 25u;
        CHECK(adapter_decode(RFC_FORMAT_GIF, x, o, NULL, NULL, NULL, NULL) ==
              RFC_ERR_LIMIT_EXCEEDED);
        free(x);
        free(b);
    }
    printf("tiff_gif: gif_import_lzw_and_refusals ok\n");
}

/* ------------------------------------------------------------------------ */
/* 5. GIF export: exact palettes and timing                                  */
/* ------------------------------------------------------------------------ */

static rfc_status gif_export_colours(uint32_t ncolours, int rgba, int transparent,
                                     int transp_rgb_shared, uint32_t dur_ms,
                                     int partial_alpha, int differing) {
    uint32_t w = 32u, h = 9u;
    rfc_layout lay = {rgba ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8,
                      rgba ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE, w, h, 0u, 8u};
    uint32_t ch = rgba ? 4u : 3u;
    uint8_t *px = (uint8_t *)calloc((size_t)w * h * ch, 1u);
    CHECK(px != NULL);
    for (uint32_t i = 0u; i < w * h; i++) {
        uint32_t c = i % ncolours;
        uint8_t *p = px + (size_t)i * ch;
        p[0] = (uint8_t)c;
        p[1] = (uint8_t)(c >> 8);
        p[2] = 7u;
        if (rgba) p[3] = 255u;
    }
    if (rgba && transparent) {
        for (uint32_t i = 0u; i < 5u; i++) {
            uint8_t *p = px + (size_t)(i * 3u) * ch;
            p[3] = partial_alpha && i == 0u ? 128u : 0u;
            if (transp_rgb_shared) {
                /* the RGB of an opaque colour (pixel 1) */
                p[0] = px[ch + 0u]; p[1] = px[ch + 1u]; p[2] = px[ch + 2u];
            } else {
                p[0] = 250u; p[1] = 1u; p[2] = 2u;
            }
            if (differing && i == 1u) p[0] = 251u;
        }
    }
    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, dur_ms, {0}, NULL, 0u, NULL, 0u};
    uint8_t *out = NULL;
    size_t on = 0u;
    rfc_status st = adapter_encode(RFC_FORMAT_GIF, &lay, px, &meta, "preserve", &out, &on);
    if (st == RFC_OK) {
        uint8_t *back = NULL;
        rfc_frame info;
        CHECK(adapter_decode(RFC_FORMAT_GIF, out, on, NULL, NULL, &info, &back) == RFC_OK);
        CHECK(info.layout.format == lay.format);
        CHECK(memcmp(back, px, (size_t)w * h * ch) == 0);
        CHECK(info.metadata.duration_ms == dur_ms);
        free(back);
        free(out);
    }
    free(px);
    return st;
}

static void test_gif_export_palette_timing(void) {
    CHECK(gif_export_colours(256u, 0, 0, 0, 0u, 0, 0) == RFC_OK);
    CHECK(gif_export_colours(257u, 0, 0, 0, 0u, 0, 0) == RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(gif_export_colours(255u, 1, 0, 0, 0u, 0, 0) == RFC_OK);   /* opaque rgba8 */
    CHECK(gif_export_colours(256u, 1, 0, 0, 0u, 0, 0) == RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(gif_export_colours(100u, 1, 1, 1, 0u, 0, 0) == RFC_OK);   /* shared RGB */
    CHECK(gif_export_colours(100u, 1, 1, 0, 0u, 0, 0) == RFC_OK);
    CHECK(gif_export_colours(100u, 1, 1, 0, 0u, 0, 1) == RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(gif_export_colours(100u, 1, 1, 0, 0u, 1, 0) == RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(gif_export_colours(16u, 0, 0, 0, 120u, 0, 0) == RFC_OK);
    CHECK(gif_export_colours(16u, 0, 0, 0, 125u, 0, 0) == RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(gif_export_colours(16u, 0, 0, 0, 655360u, 0, 0) == RFC_ERR_UNSUPPORTED_COMBINATION);
    {
        rfc_layout lay = {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 2u, 2u, 0u, 16u};
        uint8_t px[24] = {0};
        CHECK(adapter_encode(RFC_FORMAT_GIF, &lay, px, NULL, "preserve", NULL, NULL) ==
              RFC_ERR_UNSUPPORTED_SAMPLE);
    }
    printf("tiff_gif: gif_export_palette_timing ok\n");
}

/* ------------------------------------------------------------------------ */
/* 6. Jobs, limits, budget and cancellation                                  */
/* ------------------------------------------------------------------------ */

typedef struct cancel_ctx {
    const rfc_session_adapter *a;
    void *sess;
    int after;
} cancel_ctx;

static void cancel_hook(void *ctx) {
    cancel_ctx *c = (cancel_ctx *)ctx;
    if (c->after-- == 0) c->a->cancel_session(c->sess);
}

static void test_jobs_limits_budget_and_cancel(void) {
    char p_src[512], p_dst[512], p_png[512], p_back[512];
    job_result r;
    /* TIFF rgb16 -> PNG -> TIFF returns the original native samples. */
    tmk m;
    tmk_defaults(&m, 31u, 17u, 3u, 16u);
    m.compression = COMPRESSION_LZW;
    size_t n = 0u;
    uint8_t *tb = make_tiff(&m, &n);
    temp_path(p_src, sizeof(p_src), "x.tif");
    temp_path(p_png, sizeof(p_png), "x.png");
    temp_path(p_back, sizeof(p_back), "x_back.tif");
    CHECK(write_file_all(p_src, tb, n));
    run_job(RFC_FORMAT_TIFF, RFC_FORMAT_PNG, p_src, p_png, NULL, NULL, &r);
    CHECK_JOB_OK(r);
    run_job(RFC_FORMAT_PNG, RFC_FORMAT_TIFF, p_png, p_back, NULL, NULL, &r);
    CHECK_JOB_OK(r);
    size_t bn = 0u;
    uint8_t *bb = read_file_all(p_back, &bn);
    uint8_t *a = NULL, *b = NULL;
    CHECK(adapter_decode(RFC_FORMAT_TIFF, tb, n, NULL, NULL, NULL, &a) == RFC_OK);
    CHECK(adapter_decode(RFC_FORMAT_TIFF, bb, bn, NULL, NULL, NULL, &b) == RFC_OK);
    CHECK(memcmp(a, b, (size_t)31u * 17u * 6u) == 0);
    free(a); free(b); free(bb);
    remove(p_png);
    remove(p_back);
    /* Byte-identical copy, and the refusals at submit. */
    temp_path(p_dst, sizeof(p_dst), "copy.tif");
    run_job(RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, p_src, p_dst, "byte-identical", NULL, &r);
    CHECK_JOB_OK(r);
    bb = read_file_all(p_dst, &bn);
    CHECK(bb && bn == n && memcmp(bb, tb, n) == 0);
    free(bb);
    /* Existing destination: never overwritten. */
    run_job(RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, p_src, p_dst, NULL, NULL, &r);
    CHECK(r.submit == RFC_ERR_DESTINATION_EXISTS || r.status == RFC_ERR_DESTINATION_EXISTS);
    bb = read_file_all(p_dst, &bn);
    CHECK(bb && bn == n && memcmp(bb, tb, n) == 0);
    free(bb);
    remove(p_dst);
    run_job(RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, p_src, p_dst, "byte-identical", "strip", &r);
    CHECK(r.submit == RFC_ERR_UNSUPPORTED_COMBINATION && !file_exists(p_dst));
    run_job_full(RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, p_src, NULL, 0u, p_dst, NULL, NULL,
                 "first-only", NULL, 0u, &r);
    CHECK(r.submit == RFC_ERR_UNSUPPORTED_COMBINATION && !file_exists(p_dst));
    /* Source bytes survive. */
    bb = read_file_all(p_src, &bn);
    CHECK(bb && bn == n && memcmp(bb, tb, n) == 0);
    free(bb);
    remove(p_src);

    /* GIF -> PNG -> GIF returns the original samples and timing. */
    gmk g;
    gmk_defaults(&g, 21u, 11u);
    g.transparent = 2;
    g.delay_cs = 7u;
    size_t gn = 0u;
    uint8_t *gb = make_gif(&g, &gn);
    temp_path(p_src, sizeof(p_src), "x.gif");
    temp_path(p_png, sizeof(p_png), "xg.png");
    temp_path(p_back, sizeof(p_back), "x_back.gif");
    CHECK(write_file_all(p_src, gb, gn));
    /* PNG cannot carry frame timing, which pixel-exact verification
     * compares: refused as a metadata-policy failure under both policies
     * (strip cannot remove timing; it is never a verify mismatch) after
     * IMPORT and before any ENCODE, with the verify report published and
     * no output left behind. */
    {
        rfc_metadata_report mr;
        for (int pol = 0; pol < 2; pol++) {
            rfc_metadata_policy mp = pol ? RFC_META_POLICY_STRIP : RFC_META_POLICY_PRESERVE;
            /* neither strip nor an acknowledgement drops timing */
            CHECK(rfc_metadata_check_preflight(RFC_FORMAT_PNG, mp, RFC_EXACT_PIXEL_EXACT,
                                               RFC_META_ITEM_FRAME_TIMING,
                                               RFC_META_ITEM_FRAME_TIMING, &mr) ==
                  RFC_ERR_METADATA_POLICY);
            CHECK(mr.unsupported_items == RFC_META_ITEM_FRAME_TIMING);
            CHECK(rfc_metadata_check_preflight(RFC_FORMAT_GIF, mp, RFC_EXACT_PIXEL_EXACT,
                                               RFC_META_ITEM_FRAME_TIMING, 0u, &mr) == RFC_OK);
        }
    }
    for (int pol = 0; pol < 2; pol++) {
        run_job(RFC_FORMAT_GIF, RFC_FORMAT_PNG, p_src, p_png, NULL,
                pol ? "strip" : "preserve", &r);
        CHECK(r.submit == RFC_OK && r.state == RFC_JOB_FAILED &&
              r.status == RFC_ERR_METADATA_POLICY &&
              r.failing == RFC_STAGE_IMPORT);
        CHECK(r.has_rep && r.rep.mismatch.kind == RFC_MISMATCH_METADATA_UNSUPPORTED);
        CHECK(!file_exists(p_png));
    }
    temp_path(p_dst, sizeof(p_dst), "copy.gif");
    run_job(RFC_FORMAT_GIF, RFC_FORMAT_GIF, p_src, p_dst, "byte-identical", NULL, &r);
    CHECK_JOB_OK(r);
    bb = read_file_all(p_dst, &bn);
    CHECK(bb && bn == gn && memcmp(bb, gb, gn) == 0);
    free(bb);
    remove(p_dst);
    run_job(RFC_FORMAT_GIF, RFC_FORMAT_GIF, p_src, p_back, NULL, NULL, &r);
    CHECK_JOB_OK(r);
    bb = read_file_all(p_back, &bn);
    rfc_frame i1, i2;
    a = b = NULL;
    CHECK(adapter_decode(RFC_FORMAT_GIF, gb, gn, NULL, NULL, &i1, &a) == RFC_OK);
    CHECK(adapter_decode(RFC_FORMAT_GIF, bb, bn, NULL, NULL, &i2, &b) == RFC_OK);
    CHECK(memcmp(a, b, (size_t)21u * 11u * 4u) == 0 &&
          i1.metadata.duration_ms == 70u && i2.metadata.duration_ms == 70u);
    free(a); free(b); free(bb);
    remove(p_back);
    remove(p_src);

    /* Mid-decode cancellation through the row hook; nothing left reserved. */
    for (int fmt = 0; fmt < 2; fmt++) {
        rfc_format_id f = fmt ? RFC_FORMAT_GIF : RFC_FORMAT_TIFF;
        const uint8_t *src = fmt ? gb : tb;
        size_t sn = fmt ? gn : n;
        rfc_budget *bud = rfc_budget_create(256u << 20);
        rfc_allocator ba;
        rfc_budget_allocator(bud, &ba);
        rfc_exec_context e;
        tracked st;
        rfc_allocator al;
        init_exec(&e, &st, &al);
        e.allocator = &ba;
        e.budget = bud;
        const rfc_session_adapter *ad = rfc_registry_acquire_session(f);
        void *s = NULL;
        CHECK(ad->open_session(&e, &s) == RFC_OK);
        cancel_ctx cc = {ad, s, 1};
        rfc_codecs_tiff_gif_set_row_hook_for_testing(cancel_hook, &cc);
        rfc_frame *fr = NULL;
        size_t fc = 0u;
        CHECK(ad->decode_session(s, &e, src, sn, NULL, 0u, &fr, &fc, NULL) ==
              RFC_ERR_CANCELLED);
        rfc_codecs_tiff_gif_set_row_hook_for_testing(NULL, NULL);
        ad->close_session(s);
        rfc_registry_release_session(f);
        rfc_budget_stats bs;
        rfc_budget_stats_get(bud, &bs);
        CHECK(bs.current_allocated_bytes == 0u && fr == NULL);
        rfc_budget_destroy(bud);
        /* A tight budget refuses with nothing left allocated. */
        bud = rfc_budget_create(4096u);
        CHECK(adapter_decode(f, src, sn, NULL, bud, NULL, NULL) == RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_stats_get(bud, &bs);
        CHECK(bs.current_allocated_bytes == 0u);
        rfc_budget_destroy(bud);
        /* Limits. */
        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_width = 8u;
        CHECK(adapter_decode(f, src, sn, &lim, NULL, NULL, NULL) == RFC_ERR_LIMIT_EXCEEDED);
    }
    free(gb);
    free(tb);
    printf("tiff_gif: jobs_limits_budget_and_cancel ok\n");
}

/* ------------------------------------------------------------------------ */
/* 7. Round-1 review regressions                                             */
/* ------------------------------------------------------------------------ */

static rfc_job *volatile g_cancel_job = NULL;

static void job_cancel_hook(void *ctx) {
    int *after = (int *)ctx;
    if (g_cancel_job && (*after)-- == 0) {
        rfc_job_cancel(g_cancel_job);
    }
}

typedef struct fail_alloc {
    size_t calls, fail_at;
} fail_alloc;

static void *fa_alloc(void *ctx, size_t sz) {
    fail_alloc *f = (fail_alloc *)ctx;
    if (++f->calls == f->fail_at) return NULL;
    return malloc(sz ? sz : 1u);
}

static void fa_free(void *ctx, void *p) {
    (void)ctx;
    free(p);
}

static void test_review_round1(void) {
    char p_src[512], p_dst[512], p_png[512];
    job_result r;
    size_t n = 0u;

    /* JPEG sniff fields survive beside the generic geometry. */
    {
        static const uint8_t j[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 8, 0x00, 0x20, 0x00, 0x30, 3,
                                    1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1, 0xFF, 0xDA};
        rfc_sniff_result sr;
        CHECK(rfc_input_sniff_bytes(j, sizeof(j), "a.jpg", &sr) == RFC_OK);
        CHECK(sr.has_jpeg_geometry && sr.jpeg_width == 48u && sr.jpeg_height == 32u &&
              sr.jpeg_components == 3u && sr.jpeg_h_samp[0] == 2u && sr.jpeg_v_samp[0] == 2u &&
              sr.jpeg_h_samp[1] == 1u);
    }
    /* BigTIFF: decode, and a default job sized from its sniffed geometry. */
    {
        tmk m;
        tmk_defaults(&m, 29u, 13u, 3u, 16u);
        m.bigtiff = 1;
        m.compression = COMPRESSION_ADOBE_DEFLATE;
        uint8_t *b = make_tiff(&m, &n);
        CHECK(b[2] == 0x2Bu || b[3] == 0x2Bu);
        uint8_t *px = NULL;
        CHECK(adapter_decode(RFC_FORMAT_TIFF, b, n, NULL, NULL, NULL, &px) == RFC_OK);
        uint8_t *want = tmk_pixels(&m);
        CHECK(memcmp(px, want, 29u * 13u * 6u) == 0);
        free(want);
        free(px);
        temp_path(p_src, sizeof(p_src), "big8.tif");
        temp_path(p_dst, sizeof(p_dst), "big8_out.png");
        CHECK(write_file_all(p_src, b, n));
        run_job(RFC_FORMAT_TIFF, RFC_FORMAT_PNG, p_src, p_dst, NULL, NULL, &r);
        CHECK_JOB_OK(r);
        remove(p_src);
        remove(p_dst);
        free(b);
    }
    /* A tile far larger than its frame is refused before allocation. */
    {
        tmk m;
        tmk_defaults(&m, 3u, 2u, 4u, 16u);
        m.tiled = 1;
        m.tile = 2048u;
        m.compression = COMPRESSION_ADOBE_DEFLATE;
        uint8_t *b = make_tiff(&m, &n);
        CHECK(adapter_decode(RFC_FORMAT_TIFF, b, n, NULL, NULL, NULL, NULL) == RFC_ERR_LIMIT_EXCEEDED);
        free(b);
    }
    /* An IFD offset past the end of the input is refused cleanly. */
    {
        tmk m;
        tmk_defaults(&m, 8u, 8u, 1u, 8u);
        uint8_t *b = make_tiff(&m, &n);
        uint32_t off = (uint32_t)n + 4096u;
        if (b[0] == 'I') { b[4] = (uint8_t)off; b[5] = (uint8_t)(off >> 8); b[6] = (uint8_t)(off >> 16); b[7] = (uint8_t)(off >> 24); }
        else { b[7] = (uint8_t)off; b[6] = (uint8_t)(off >> 8); b[5] = (uint8_t)(off >> 16); b[4] = (uint8_t)(off >> 24); }
        CHECK(adapter_decode(RFC_FORMAT_TIFF, b, n, NULL, NULL, NULL, NULL) != RFC_OK);
        free(b);
    }
    /* A small TIFF carrying a 10 MiB XMP packet converts with default
     * settings (TIFF's source-proportional reservation terms). */
    {
        size_t xn = 10u * 1024u * 1024u;
        char *xmp = (char *)malloc(xn + 1u);
        CHECK(xmp != NULL);
        memset(xmp, 'x', xn);
        memcpy(xmp, "<x:xmpmeta>", 11);
        xmp[xn] = '\0';
        tmk m;
        tmk_defaults(&m, 16u, 16u, 3u, 8u);
        m.xmp = xmp;
        uint8_t *b = make_tiff(&m, &n);
        temp_path(p_src, sizeof(p_src), "xmp.tif");
        temp_path(p_dst, sizeof(p_dst), "xmp_out.tif");
        CHECK(write_file_all(p_src, b, n));
        run_job(RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, p_src, p_dst, NULL, NULL, &r);
        CHECK_JOB_OK(r);
        size_t on = 0u;
        uint8_t *ob = read_file_all(p_dst, &on);
        rfc_frame info;
        CHECK(ob && adapter_decode(RFC_FORMAT_TIFF, ob, on, NULL, NULL, &info, NULL) == RFC_OK);
        CHECK(info.metadata.xmp_len == xn);
        free(ob);
        remove(p_src);
        remove(p_dst);
        free(b);
        free(xmp);
    }
    /* Encoders validate the buffer and honour a packed (zero) stride. */
    {
        const rfc_session_adapter *ads[2] = {rfc_registry_acquire_session(RFC_FORMAT_TIFF),
                                             rfc_registry_acquire_session(RFC_FORMAT_GIF)};
        rfc_format_id fs[2] = {RFC_FORMAT_TIFF, RFC_FORMAT_GIF};
        for (int k = 0; k < 2; k++) {
            rfc_exec_context e;
            tracked st;
            rfc_allocator al;
            init_exec(&e, &st, &al);
            void *sess = NULL;
            CHECK(ads[k]->open_session(&e, &sess) == RFC_OK);
            uint8_t px[4 * 3 * 3];
            for (size_t i = 0u; i < sizeof(px); i++) px[i] = (uint8_t)(i * 9u);
            rfc_frame fr;
            memset(&fr, 0, sizeof(fr));
            fr.layout = (rfc_layout){RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 3u, 0u, 8u};
            fr.data = px;
            fr.data_len = sizeof(px) - 1u; /* one byte short */
            fr.metadata.orientation = RFC_ORIENTATION_NORMAL;
            rfc_encoded enc;
            memset(&enc, 0, sizeof(enc));
            CHECK(ads[k]->encode_session(sess, &e, &fr, 1u, NULL, NULL, 0u, &enc) ==
                  RFC_ERR_INVALID_ARGUMENT);
            fr.data_len = sizeof(px);
            CHECK(ads[k]->encode_session(sess, &e, &fr, 1u, NULL, NULL, 0u, &enc) == RFC_OK);
            uint8_t *back = NULL;
            CHECK(adapter_decode(fs[k], enc.data, enc.len, NULL, NULL, NULL, &back) == RFC_OK);
            CHECK(memcmp(back, px, sizeof(px)) == 0); /* rows not repeated */
            free(back);
            rfc_encoded_free(&enc);
            ads[k]->close_session(sess);
            rfc_registry_release_session(fs[k]);
        }
    }
    /* GIF encode never returns success with a truncated output: every
     * allocation failure point either refuses or yields a decodable file. */
    {
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 64u, 64u, 0u, 8u};
        uint8_t *px = (uint8_t *)malloc(64u * 64u * 3u);
        CHECK(px != NULL);
        for (size_t i = 0u; i < 64u * 64u * 3u; i++) px[i] = (uint8_t)((i / 3u) % 7u * 30u);
        int refused = 0, ok = 0;
        for (size_t fail_at = 1u; fail_at < 40u; fail_at++) {
            fail_alloc fa = {0u, fail_at};
            rfc_allocator fal = {fa_alloc, fa_free, &fa};
            rfc_exec_context e;
            memset(&e, 0, sizeof(e));
            rfc_limits_default(&e.limits);
            e.codec_threads = 1u;
            e.allocator = &fal;
            rfc_frame fr;
            memset(&fr, 0, sizeof(fr));
            fr.layout = lay;
            fr.layout.stride_bytes = 64u * 3u;
            fr.data = px;
            fr.data_len = 64u * 64u * 3u;
            fr.metadata.orientation = RFC_ORIENTATION_NORMAL;
            const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_GIF);
            void *sess = NULL;
            CHECK(a->open_session(&e, &sess) == RFC_OK);
            rfc_encoded enc;
            memset(&enc, 0, sizeof(enc));
            rfc_status est = a->encode_session(sess, &e, &fr, 1u, NULL, NULL, 0u, &enc);
            a->close_session(sess);
            rfc_registry_release_session(RFC_FORMAT_GIF);
            if (est == RFC_OK) {
                ok++;
                uint8_t *back = NULL;
                CHECK(adapter_decode(RFC_FORMAT_GIF, enc.data, enc.len, NULL, NULL, NULL, &back) == RFC_OK);
                CHECK(memcmp(back, px, 64u * 64u * 3u) == 0);
                free(back);
                rfc_encoded_free(&enc);
            } else {
                refused++;
                if (est != RFC_ERR_OUT_OF_MEMORY) {
                    fprintf(stderr, "gif fail_at=%zu: %s\n", fail_at, rfc_status_name(est));
                }
                CHECK(est == RFC_ERR_OUT_OF_MEMORY && enc.data == NULL);
            }
        }
        CHECK(refused > 0 && ok > 0);
        free(px);
    }
    /* An opaque GIF is limited as the rgb8 it decodes to. */
    {
        gmk g;
        gmk_defaults(&g, 40u, 30u);
        uint8_t *b = make_gif(&g, &n);
        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_decoded_bytes = 40u * 30u * 3u;
        CHECK(adapter_decode(RFC_FORMAT_GIF, b, n, &lim, NULL, NULL, NULL) == RFC_OK);
        lim.max_decoded_bytes = 40u * 30u * 3u - 1u;
        CHECK(adapter_decode(RFC_FORMAT_GIF, b, n, &lim, NULL, NULL, NULL) == RFC_ERR_LIMIT_EXCEEDED);
        free(b);
    }
    /* The timing refusal names every unsupported item. */
    {
        rfc_metadata_report mr;
        uint32_t present = RFC_META_ITEM_FRAME_TIMING | RFC_META_ITEM_ICC | RFC_META_ITEM_XMP;
        CHECK(rfc_metadata_check_preflight(RFC_FORMAT_BMP, RFC_META_POLICY_PRESERVE,
                                           RFC_EXACT_PIXEL_EXACT, present, 0u, &mr) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(mr.unsupported_items == present);
    }
    /* Cancellation inside the GIF LZW pre-scan (probe) and palette pass. */
    {
        gmk g;
        gmk_defaults(&g, 2048u, 512u);
        uint8_t *b = make_gif(&g, &n);
        const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_GIF);
        rfc_exec_context e;
        tracked st;
        rfc_allocator al;
        init_exec(&e, &st, &al);
        void *sess = NULL;
        CHECK(a->open_session(&e, &sess) == RFC_OK);
        cancel_ctx cc = {a, sess, 0};
        rfc_codecs_tiff_gif_set_row_hook_for_testing(cancel_hook, &cc);
        rfc_image_probe pr;
        CHECK(a->probe_session(sess, &e, b, n, &pr) == RFC_ERR_CANCELLED);
        rfc_codecs_tiff_gif_set_row_hook_for_testing(NULL, NULL);
        a->close_session(sess);
        CHECK(a->open_session(&e, &sess) == RFC_OK);
        uint8_t *px = (uint8_t *)calloc(256u * 64u * 3u, 1u);
        CHECK(px != NULL);
        rfc_frame fr;
        memset(&fr, 0, sizeof(fr));
        fr.layout = (rfc_layout){RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 256u, 64u, 256u * 3u, 8u};
        fr.data = px;
        fr.data_len = 256u * 64u * 3u;
        fr.metadata.orientation = RFC_ORIENTATION_NORMAL;
        cc.sess = sess;
        cc.after = 3;
        rfc_codecs_tiff_gif_set_row_hook_for_testing(cancel_hook, &cc);
        rfc_encoded enc;
        memset(&enc, 0, sizeof(enc));
        CHECK(a->encode_session(sess, &e, &fr, 1u, NULL, NULL, 0u, &enc) == RFC_ERR_CANCELLED);
        CHECK(enc.data == NULL);
        rfc_codecs_tiff_gif_set_row_hook_for_testing(NULL, NULL);
        a->close_session(sess);
        rfc_registry_release_session(RFC_FORMAT_GIF);
        CHECK(st.live == 0u);
        free(px);
        free(b);
    }
    /* GIF -> PNG -> GIF returns the original samples (untimed). */
    {
        gmk g;
        gmk_defaults(&g, 33u, 21u);
        g.transparent = 1;
        uint8_t *gb = make_gif(&g, &n);
        temp_path(p_src, sizeof(p_src), "rt.gif");
        temp_path(p_png, sizeof(p_png), "rt.png");
        temp_path(p_dst, sizeof(p_dst), "rt_back.gif");
        CHECK(write_file_all(p_src, gb, n));
        run_job(RFC_FORMAT_GIF, RFC_FORMAT_PNG, p_src, p_png, NULL, NULL, &r);
        CHECK_JOB_OK(r);
        run_job(RFC_FORMAT_PNG, RFC_FORMAT_GIF, p_png, p_dst, NULL, NULL, &r);
        CHECK_JOB_OK(r);
        size_t bn = 0u;
        uint8_t *bb = read_file_all(p_dst, &bn);
        uint8_t *a1 = NULL, *a2 = NULL;
        CHECK(adapter_decode(RFC_FORMAT_GIF, gb, n, NULL, NULL, NULL, &a1) == RFC_OK);
        CHECK(bb && adapter_decode(RFC_FORMAT_GIF, bb, bn, NULL, NULL, NULL, &a2) == RFC_OK);
        CHECK(memcmp(a1, a2, 33u * 21u * 4u) == 0);
        free(a1); free(a2); free(bb);
        remove(p_src); remove(p_png); remove(p_dst);
        free(gb);
    }
    /* A scheduled file job cancelled mid-decode leaves the source intact and
     * no destination or partial file. */
    for (int fmt = 0; fmt < 2; fmt++) {
        uint8_t *b = NULL;
        if (fmt == 0) {
            tmk m;
            tmk_defaults(&m, 512u, 512u, 3u, 8u);
            b = make_tiff(&m, &n);
        } else {
            gmk g;
            gmk_defaults(&g, 512u, 512u);
            b = make_gif(&g, &n);
        }
        temp_path(p_src, sizeof(p_src), fmt ? "c.gif" : "c.tif");
        temp_path(p_dst, sizeof(p_dst), "c_out.png");
        CHECK(write_file_all(p_src, b, n));
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);
        rfc_io_source si;
        rfc_io_sink so;
        rfc_io_source_init_path(&si, p_src, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_dst, RFC_EXPORT_REFUSE_EXISTING);
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = fmt ? RFC_FORMAT_GIF : RFC_FORMAT_TIFF;
        sp.target_format = RFC_FORMAT_PNG;
        sp.io_source = &si;
        sp.io_sink = &so;
        int after = 4;
        g_cancel_job = NULL;
        rfc_codecs_tiff_gif_set_row_hook_for_testing(job_cancel_hook, &after);
        rfc_job *j = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        g_cancel_job = j;
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_codecs_tiff_gif_set_row_hook_for_testing(NULL, NULL);
        g_cancel_job = NULL;
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        CHECK(snap.status == RFC_ERR_CANCELLED);
        rfc_job_release(j);
        rfc_scheduler_destroy(sched);
        char partial[600];
        snprintf(partial, sizeof(partial), "%s.partial", p_dst);
        CHECK(!file_exists(p_dst) && !file_exists(partial));
        size_t sn = 0u;
        uint8_t *sb = read_file_all(p_src, &sn);
        CHECK(sb && sn == n && memcmp(sb, b, n) == 0);
        free(sb);
        remove(p_src);
        free(b);
    }
    /* GIF default submissions: path and bytes for a compressible 4096x4096
     * screen, and a submit-time refusal under a lower max_width. */
    {
        gmk g;
        gmk_defaults(&g, 4096u, 4096u);
        uint8_t *gb = make_gif(&g, &n);
        temp_path(p_src, sizeof(p_src), "g4k.gif");
        temp_path(p_dst, sizeof(p_dst), "g4k.gif.out.gif");
        CHECK(write_file_all(p_src, gb, n));
        run_job(RFC_FORMAT_GIF, RFC_FORMAT_GIF, p_src, p_dst, NULL, NULL, &r);
        CHECK_JOB_OK(r);
        remove(p_dst);
        run_job_full(RFC_FORMAT_GIF, RFC_FORMAT_GIF, NULL, gb, n, p_dst, NULL, NULL, NULL, NULL, 0u, &r);
        CHECK_JOB_OK(r);
        remove(p_dst);
        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_width = 1024u;
        run_job_full(RFC_FORMAT_GIF, RFC_FORMAT_GIF, p_src, NULL, 0u, p_dst, NULL, NULL, NULL, &lim, 0u, &r);
        CHECK(r.submit == RFC_ERR_LIMIT_EXCEEDED && !file_exists(p_dst));
        remove(p_src);
        free(gb);
    }
    printf("tiff_gif: review_round1_regressions ok\n");
}

/* ------------------------------------------------------------------------ */
/* 8. Completion-review gap regressions                                      */
/* ------------------------------------------------------------------------ */

static void png_file(const char *path, const rfc_layout *lay, const uint8_t *px) {
    uint8_t *out = NULL;
    size_t on = 0u;
    CHECK(adapter_encode(RFC_FORMAT_PNG, lay, px, NULL, "preserve", &out, &on) == RFC_OK);
    CHECK(write_file_all(path, out, on));
    free(out);
}

static void test_completion_gaps(void) {
    char p_src[512], p_dst[512];
    job_result r;
    size_t n = 0u;

    /* 1. Volumetric TIFF (ImageDepth 2, distinct slices) is refused. */
    {
        char path[512];
        temp_path(path, sizeof(path), "vol.tif");
        TIFF *tif = TIFFOpen(path, "w");
        CHECK(tif != NULL);
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 16u);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 16u);
        TIFFSetField(tif, TIFFTAG_IMAGEDEPTH, 2u);
        TIFFSetField(tif, TIFFTAG_TILEWIDTH, 16u);
        TIFFSetField(tif, TIFFTAG_TILELENGTH, 16u);
        TIFFSetField(tif, TIFFTAG_TILEDEPTH, 1u);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        uint8_t tile[256];
        for (uint32_t z = 0u; z < 2u; z++) {
            memset(tile, (int)(z ? 200 : 20), sizeof(tile));
            CHECK(TIFFWriteTile(tif, tile, 0, 0, z, 0) >= 0);
        }
        CHECK(TIFFWriteDirectory(tif));
        TIFFClose(tif);
        uint8_t *b = read_file_all(path, &n);
        CHECK(b != NULL);
        CHECK(adapter_decode(RFC_FORMAT_TIFF, b, n, NULL, NULL, NULL, NULL) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        temp_path(p_dst, sizeof(p_dst), "vol_out.png");
        run_job(RFC_FORMAT_TIFF, RFC_FORMAT_PNG, path, p_dst, NULL, NULL, &r);
        CHECK(r.status == RFC_ERR_UNSUPPORTED_COMBINATION && !file_exists(p_dst));
        CHECK(strstr(r.detail, "volumetric") != NULL);
        remove(path);
        free(b);
    }
    /* 2. Class regression: admission and the adapter decide alike. Each
     * fixture is admitted with max_decoded_bytes at exactly its decoded size,
     * and refused at submit one byte below and one pixel narrower or shorter,
     * which pins width, height and layout (the GIF sniff runs the adapter's
     * own walk under the job's limits). */
    {
        typedef struct fx {
            const char *name, *ext;
            rfc_format_id fmt;
            int exact; /* sniffed geometry equals the decoded layout */
        } fx;
        static const fx fxs[] = {
            {"gif opaque", "gif", RFC_FORMAT_GIF, 1},
            {"gif transparent", "gif", RFC_FORMAT_GIF, 1},
            {"gif 10000-block comment", "gif", RFC_FORMAT_GIF, 1},
            {"png rgb8", "png", RFC_FORMAT_PNG, 1},
            {"png rgba8", "png", RFC_FORMAT_PNG, 1},
            {"png gray16", "png", RFC_FORMAT_PNG, 1},
            {"tiff rgb16", "tif", RFC_FORMAT_TIFF, 1},
            {"tiff rgba8 tiled", "tif", RFC_FORMAT_TIFF, 1},
            {"bigtiff rgb16", "tif", RFC_FORMAT_TIFF, 1},
        };
        for (size_t k = 0u; k < sizeof(fxs) / sizeof(fxs[0]); k++) {
            uint8_t *b = NULL;
            uint32_t w = 29u, h = 13u;
            if (fxs[k].fmt == RFC_FORMAT_GIF) {
                gmk g;
                gmk_defaults(&g, w, h);
                if (k == 1u) g.transparent = 2;
                if (k == 2u) g.comment_blocks = 10000u;
                b = make_gif(&g, &n);
            } else if (fxs[k].fmt == RFC_FORMAT_PNG) {
                rfc_sample_format f = k == 3u ? RFC_SAMPLE_RGB8 : k == 4u ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_GRAY16;
                rfc_layout lay = {f, f == RFC_SAMPLE_RGBA8 ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE, w, h, 0u,
                                  f == RFC_SAMPLE_GRAY16 ? 16u : 8u};
                size_t bytes = (size_t)w * h * rfc_sample_bytes(f);
                uint8_t *px = (uint8_t *)malloc(bytes);
                CHECK(px != NULL);
                for (size_t i = 0u; i < bytes; i++) px[i] = (uint8_t)(i * 37u + 11u);
                if (f == RFC_SAMPLE_RGBA8) for (size_t i = 3u; i < bytes; i += 4u) px[i] = 255u;
                temp_path(p_src, sizeof(p_src), "cls.png");
                png_file(p_src, &lay, px);
                b = read_file_all(p_src, &n);
                remove(p_src);
                free(px);
            } else {
                tmk m;
                tmk_defaults(&m, w, h, k == 7u ? 4u : 3u, k == 7u ? 8u : 16u);
                m.tiled = k == 7u;
                m.bigtiff = k == 8u;
                m.compression = COMPRESSION_ADOBE_DEFLATE;
                b = make_tiff(&m, &n);
            }
            CHECK(b != NULL);
            rfc_frame info;
            CHECK(adapter_decode(fxs[k].fmt, b, n, NULL, NULL, &info, NULL) == RFC_OK);
            uint64_t dbytes = (uint64_t)info.layout.width * info.layout.height *
                              rfc_sample_bytes(info.layout.format);
            char ext_name[32];
            snprintf(ext_name, sizeof(ext_name), "cls_%zu.%s", k, fxs[k].ext);
            temp_path(p_src, sizeof(p_src), ext_name);
            snprintf(ext_name, sizeof(ext_name), "cls_%zu_out.%s", k, fxs[k].ext);
            temp_path(p_dst, sizeof(p_dst), ext_name);
            CHECK(write_file_all(p_src, b, n));
            rfc_limits lim;
            rfc_limits_default(&lim);
            lim.max_decoded_bytes = dbytes;
            run_job_full(fxs[k].fmt, fxs[k].fmt, p_src, NULL, 0u, p_dst, NULL, NULL, NULL, &lim, 0u, &r);
            if (r.submit != RFC_OK || r.state != RFC_JOB_SUCCEEDED) {
                fprintf(stderr, "class %s at %llu bytes\n", fxs[k].name, (unsigned long long)dbytes);
            }
            CHECK_JOB_OK(r);
            remove(p_dst);
            lim.max_decoded_bytes = dbytes - 1u;
            run_job_full(fxs[k].fmt, fxs[k].fmt, p_src, NULL, 0u, p_dst, NULL, NULL, NULL, &lim, 0u, &r);
            CHECK((r.submit == RFC_ERR_LIMIT_EXCEEDED ||
                   (r.submit == RFC_OK && r.status == RFC_ERR_LIMIT_EXCEEDED)) &&
                  !file_exists(p_dst));
            if (fxs[k].exact) {
                CHECK(r.submit == RFC_ERR_LIMIT_EXCEEDED);
                rfc_limits_default(&lim);
                lim.max_width = w - 1u;
                run_job_full(fxs[k].fmt, fxs[k].fmt, p_src, NULL, 0u, p_dst, NULL, NULL, NULL, &lim, 0u, &r);
                CHECK(r.submit == RFC_ERR_LIMIT_EXCEEDED);
                rfc_limits_default(&lim);
                lim.max_height = h - 1u;
                run_job_full(fxs[k].fmt, fxs[k].fmt, p_src, NULL, 0u, p_dst, NULL, NULL, NULL, &lim, 0u, &r);
                CHECK(r.submit == RFC_ERR_LIMIT_EXCEEDED);
            }
            remove(p_src);
            free(b);
        }
    }
    /* 2b. max_metadata_bytes applies as written, 0 included: a GIF with a
     * comment is refused under 0, one with no extension decodes; a job under
     * 0 is refused with the adapter's own status (the walk refuses it at
     * submit too, so admission falls back to the lower bound and leaves the
     * refusal to the adapter). */
    {
        gmk gj;
        gmk_defaults(&gj, 40u, 30u);
        gj.comment_blocks = 5u;
        uint8_t *jb = make_gif(&gj, &n);
        temp_path(p_src, sizeof(p_src), "m0.gif");
        temp_path(p_dst, sizeof(p_dst), "m0_out.gif");
        CHECK(write_file_all(p_src, jb, n));
        rfc_limits l0;
        rfc_limits_default(&l0);
        l0.max_metadata_bytes = 0u;
        run_job_full(RFC_FORMAT_GIF, RFC_FORMAT_GIF, p_src, NULL, 0u, p_dst, NULL, NULL, NULL, &l0, 0u, &r);
        CHECK(r.submit == RFC_OK && r.status == RFC_ERR_LIMIT_EXCEEDED &&
              r.failing == RFC_STAGE_IMPORT && !file_exists(p_dst));
        remove(p_src);
        free(jb);
    }
    {
        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_metadata_bytes = 0u;
        gmk g;
        gmk_defaults(&g, 8u, 8u);
        uint8_t *gb = make_gif(&g, &n);
        CHECK(adapter_decode(RFC_FORMAT_GIF, gb, n, &lim, NULL, NULL, NULL) == RFC_OK);
        free(gb);
        g.comment_blocks = 3u;
        gb = make_gif(&g, &n);
        CHECK(adapter_decode(RFC_FORMAT_GIF, gb, n, &lim, NULL, NULL, NULL) == RFC_ERR_LIMIT_EXCEEDED);
        free(gb);
    }
    /* 3. A small TIFF with a 10 MiB XMP packet converts to JPEG XL and AVIF
     * with default settings (the TIFF terms in both admission paths). */
    {
        size_t xn = 10u * 1024u * 1024u;
        char *xmp = (char *)malloc(xn + 1u);
        CHECK(xmp != NULL);
        memset(xmp, 'x', xn);
        memcpy(xmp, "<x:xmpmeta>", 11);
        xmp[xn] = '\0';
        tmk m;
        tmk_defaults(&m, 16u, 16u, 3u, 8u);
        m.xmp = xmp;
        uint8_t *b = make_tiff(&m, &n);
        temp_path(p_src, sizeof(p_src), "xmp2.tif");
        CHECK(write_file_all(p_src, b, n));
        rfc_format_id tg[2] = {RFC_FORMAT_JPEG_XL, RFC_FORMAT_AVIF};
        for (int k = 0; k < 2; k++) {
            temp_path(p_dst, sizeof(p_dst), k ? "xmp2.avif" : "xmp2.jxl");
            run_job(RFC_FORMAT_TIFF, tg[k], p_src, p_dst, NULL, NULL, &r);
            CHECK_JOB_OK(r);
            remove(p_dst);
        }
        remove(p_src);
        free(b);
        free(xmp);
    }
    /* 4. Standalone TIFF export applies the metadata byte limit. */
    {
        const rfc_session_adapter *a = rfc_registry_acquire_session(RFC_FORMAT_TIFF);
        rfc_exec_context e;
        tracked st;
        rfc_allocator al;
        init_exec(&e, &st, &al);
        e.limits.max_metadata_bytes = 1024u;
        void *sess = NULL;
        CHECK(a->open_session(&e, &sess) == RFC_OK);
        static uint8_t xmp2k[2048];
        memset(xmp2k, 'm', sizeof(xmp2k));
        uint8_t px[4 * 4 * 3] = {0};
        rfc_frame fr;
        memset(&fr, 0, sizeof(fr));
        fr.layout = (rfc_layout){RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        fr.data = px;
        fr.data_len = sizeof(px);
        fr.metadata.orientation = RFC_ORIENTATION_NORMAL;
        fr.metadata.xmp = xmp2k;
        fr.metadata.xmp_len = sizeof(xmp2k);
        rfc_encoded enc;
        memset(&enc, 0, sizeof(enc));
        rfc_setting_value sv[1] = {{"metadata", "preserve"}};
        CHECK(a->encode_session(sess, &e, &fr, 1u, NULL, sv, 1u, &enc) == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(enc.data == NULL);
        sv[0].value = "strip";
        CHECK(a->encode_session(sess, &e, &fr, 1u, NULL, sv, 1u, &enc) == RFC_OK);
        rfc_encoded_free(&enc);
        a->close_session(sess);
        rfc_registry_release_session(RFC_FORMAT_TIFF);
        CHECK(st.live == 0u);
    }
    /* 5. GIF export names why it refuses, in the job's detail. */
    {
        struct {
            const char *what, *want;
        } cases[] = {
            {"palette", "palette reduction"},
            {"partial", "partial alpha"},
            {"differing", "differing RGB"},
        };
        for (size_t k = 0u; k < 3u; k++) {
            int rgba = k > 0u;
            rfc_layout lay = {rgba ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8,
                              rgba ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE, 32u, 16u, 0u, 8u};
            uint32_t ch = rgba ? 4u : 3u;
            uint8_t *px = (uint8_t *)calloc(32u * 16u * ch, 1u);
            CHECK(px != NULL);
            for (uint32_t i = 0u; i < 32u * 16u; i++) {
                uint8_t *q = px + (size_t)i * ch;
                q[0] = (uint8_t)i;
                q[1] = (uint8_t)(i >> 8);
                q[2] = k == 0u ? 1u : 9u; /* 512 colours for the palette case */
                if (rgba) q[3] = 255u;
            }
            if (k > 0u) {
                for (uint32_t i = 0u; i < 4u; i++) {
                    uint8_t *q = px + (size_t)i * ch;
                    q[3] = k == 1u ? 128u : 0u;
                }
                for (uint32_t i = 4u; i < 32u * 16u; i++) {
                    px[(size_t)i * ch + 0u] = (uint8_t)(i & 7u); /* few colours */
                    px[(size_t)i * ch + 1u] = 0u;
                }
            }
            temp_path(p_src, sizeof(p_src), "detail.png");
            temp_path(p_dst, sizeof(p_dst), "detail.gif");
            png_file(p_src, &lay, px);
            run_job(RFC_FORMAT_PNG, RFC_FORMAT_GIF, p_src, p_dst, NULL, NULL, &r);
            CHECK(r.status == RFC_ERR_UNSUPPORTED_COMBINATION && !file_exists(p_dst));
            if (strstr(r.detail, cases[k].want) == NULL) {
                fprintf(stderr, "gif detail %s: '%s'\n", cases[k].what, r.detail);
            }
            CHECK(strstr(r.detail, cases[k].want) != NULL);
            remove(p_src);
            free(px);
        }
    }
    /* 6. A timed two-frame animation is refused as animation, not as a
     * malformed file. */
    {
        gmk g;
        gmk_defaults(&g, 8u, 6u);
        g.images = 2;
        g.delay_cs = 5u;
        uint8_t *gb = make_gif(&g, &n);
        CHECK(adapter_decode(RFC_FORMAT_GIF, gb, n, NULL, NULL, NULL, NULL) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        temp_path(p_src, sizeof(p_src), "anim.gif");
        temp_path(p_dst, sizeof(p_dst), "anim_out.png");
        CHECK(write_file_all(p_src, gb, n));
        run_job(RFC_FORMAT_GIF, RFC_FORMAT_PNG, p_src, p_dst, NULL, NULL, &r);
        CHECK(r.status == RFC_ERR_UNSUPPORTED_COMBINATION && strstr(r.detail, "animated") != NULL);
        remove(p_src);
        free(gb);
    }
    printf("tiff_gif: completion_gap_regressions ok\n");
}

/* ------------------------------------------------------------------------ */
/* CLI harness for tests/converter/test_tiff_gif.py                          */
/* ------------------------------------------------------------------------ */

static rfc_format_id parse_fmt(const char *s) {
    const rfc_capability *c = rfc_registry_find_name(s);
    return c ? c->id : (rfc_format_id)0;
}

static int cli_main(int argc, char **argv) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_OK);
    CHECK(rfc_codecs_register_avif() == RFC_OK);
    CHECK(rfc_codecs_register_tiff_gif() == RFC_OK);
    const char *sub = argv[2];
    if (strcmp(sub, "dump") == 0 && argc == 6) {
        rfc_format_id f = parse_fmt(argv[3]);
        size_t n = 0u;
        uint8_t *b = read_file_all(argv[4], &n);
        if (!b) b = (uint8_t *)calloc(1u, 1u);
        uint8_t *px = NULL;
        rfc_frame info;
        memset(&info, 0, sizeof(info));
        rfc_status r = adapter_decode(f, b, n, NULL, NULL, &info, &px);
        free(b);
        if (r == RFC_OK) {
            size_t len = (size_t)info.layout.width * info.layout.height *
                         rfc_sample_bytes(info.layout.format);
            CHECK(write_file_all(argv[5], px, len));
            free(px);
        }
        printf("STATUS=%s WIDTH=%u HEIGHT=%u SAMPLE=%u BPS=%u ICC_LEN=%zu XMP_LEN=%zu "
               "ORIENTATION=%u DURATION=%u\n",
               rfc_status_name(r), info.layout.width, info.layout.height,
               (unsigned)info.layout.format, (unsigned)info.layout.bits_per_sample,
               info.metadata.icc_len, info.metadata.xmp_len,
               (unsigned)info.metadata.orientation, info.metadata.duration_ms);
        return r == RFC_OK ? 0 : 2;
    }
    if (strcmp(sub, "convert") == 0 && (argc == 8 || argc == 9)) {
        /* convert <sfmt> <dfmt> <src> <dst> <metadata|-> [working-set bytes] */
        rfc_format_id sf = parse_fmt(argv[3]), df = parse_fmt(argv[4]);
        if (!sf || !df) return 2;
        const char *meta = strcmp(argv[7], "-") == 0 ? NULL : argv[7];
        uint64_t ws = argc == 9 ? strtoull(argv[8], NULL, 10) : 0u;
        job_result r;
        run_job_full(sf, df, argv[5], NULL, 0u, argv[6], NULL, meta, NULL, NULL, ws, &r);
        printf("SUBMIT=%s STATUS=%s STATE=%u STAGE=%u BUDGET_LIMIT=%llu BUDGET_PEAK=%llu\n",
               rfc_status_name(r.submit), rfc_status_name(r.status), (unsigned)r.state,
               (unsigned)r.failing,
               (unsigned long long)r.budget_limit, (unsigned long long)r.budget_peak);
        return (r.submit == RFC_OK && r.state == RFC_JOB_SUCCEEDED) ? 0 : 2;
    }
    if (strcmp(sub, "make-matrix") == 0 && argc == 4) {
        /* make-matrix <dir>: every compression x spp x depth x tiling x
         * planar fixture written as <dir>/mk_<c>_<spp>_<bps>_<t>_<p>.tif with
         * its adapter decode as the matching .raw, in one process. */
        static const uint16_t comps[4] = {COMPRESSION_NONE, COMPRESSION_PACKBITS,
                                          COMPRESSION_LZW, COMPRESSION_ADOBE_DEFLATE};
        static const uint16_t spps[3] = {1u, 3u, 4u};
        int made = 0;
        for (int ci = 0; ci < 4; ci++)
            for (int si = 0; si < 3; si++)
                for (uint16_t bps = 8u; bps <= 16u; bps = (uint16_t)(bps + 8u))
                    for (int tiled = 0; tiled < 2; tiled++)
                        for (int sep = 0; sep < 2; sep++) {
                            tmk m;
                            tmk_defaults(&m, 45u, 27u, spps[si], bps);
                            m.compression = comps[ci];
                            m.tiled = tiled;
                            m.planar = sep ? PLANARCONFIG_SEPARATE : PLANARCONFIG_CONTIG;
                            m.predictor = (comps[ci] == COMPRESSION_LZW ||
                                           comps[ci] == COMPRESSION_ADOBE_DEFLATE) && !sep ? 2u : 1u;
                            size_t n = 0u;
                            uint8_t *b = make_tiff(&m, &n);
                            char path[1024];
                            snprintf(path, sizeof(path), "%s/mk_%u_%u_%u_%d_%d.tif", argv[3],
                                     (unsigned)comps[ci], (unsigned)spps[si], (unsigned)bps, tiled, sep);
                            CHECK(write_file_all(path, b, n));
                            uint8_t *px = NULL;
                            rfc_frame info;
                            CHECK(adapter_decode(RFC_FORMAT_TIFF, b, n, NULL, NULL, &info, &px) == RFC_OK);
                            size_t len = (size_t)info.layout.width * info.layout.height *
                                         rfc_sample_bytes(info.layout.format);
                            path[strlen(path) - 3] = '\0';
                            strcat(path, "raw");
                            CHECK(write_file_all(path, px, len));
                            printf("FIX name=mk_%u_%u_%u_%d_%d WIDTH=%u HEIGHT=%u BPS=%u\n",
                                   (unsigned)comps[ci], (unsigned)spps[si], (unsigned)bps, tiled,
                                   sep, info.layout.width, info.layout.height,
                                   (unsigned)info.layout.bits_per_sample);
                            free(px);
                            free(b);
                            made++;
                        }
        printf("STATUS=ok MADE=%d\n", made);
        return 0;
    }
    if (strcmp(sub, "make-tiff") == 0 && (argc == 12 || argc == 13)) {
        /* make-tiff <out> <w> <h> <spp> <bps> <compression> <predictor> <tiled> <separate> [photometric] */
        tmk m;
        tmk_defaults(&m, (uint32_t)atoi(argv[4]), (uint32_t)atoi(argv[5]),
                     (uint16_t)atoi(argv[6]), (uint16_t)atoi(argv[7]));
        m.compression = (uint16_t)atoi(argv[8]);
        m.predictor = (uint16_t)atoi(argv[9]);
        m.tiled = atoi(argv[10]);
        m.planar = atoi(argv[11]) ? PLANARCONFIG_SEPARATE : PLANARCONFIG_CONTIG;
        if (argc == 13) {
            m.photometric = (uint16_t)atoi(argv[12]);
        }
        size_t n = 0u;
        uint8_t *b = make_tiff(&m, &n);
        CHECK(write_file_all(argv[3], b, n));
        free(b);
        printf("STATUS=ok BYTES=%zu\n", n);
        return 0;
    }
    return 2;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* progress survives a crash */
    TIFFSetErrorHandler(NULL);
    TIFFSetWarningHandler(NULL);
    if (argc >= 3 && strcmp(argv[1], "cli") == 0) {
        return cli_main(argc, argv);
    }
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_OK);
    CHECK(rfc_codecs_register_avif() == RFC_OK);
    test_registration_and_geometry();
    test_tiff_import_matrix_and_refusals();
    test_tiff_export_and_metadata();
    test_gif_import_lzw_and_refusals();
    test_gif_export_palette_timing();
    test_jobs_limits_budget_and_cancel();
    test_review_round1();
    test_completion_gaps();
    CHECK(rfc_codecs_unregister_tiff_gif() == RFC_OK);
    printf("tiff_gif: all checks passed (%d checks)\n", g_checks);
    return 0;
}
