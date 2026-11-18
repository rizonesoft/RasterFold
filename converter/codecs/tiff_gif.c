/* Shipped C11 codec adapters for D10 T02 s6:
 *   - RFC_FORMAT_TIFF (bundled libtiff 4.7.2): single-IFD still TIFF import
 *     (8/16-bit gray, RGB, straight-alpha RGBA; strips or tiles; contiguous or
 *     separate planes; none/LZW/Deflate/PackBits/JPEG/WebP; horizontal
 *     predictor) with ICC, orientation and XMP, refusal of multi-image and
 *     uncarriable content, and lossless Adobe-Deflate + predictor export.
 *   - RFC_FORMAT_GIF (bundled giflib 6.1.3): single-image GIF import after a
 *     zero-allocation structural pre-scan (descriptor, extensions, colour
 *     tables, exact LZW length and end code), and exact-palette export only.
 * Both adapters run libraries under per-call error capture, bound memory
 * through rfc_budget reservations (and libtiff's cumulated-allocation cap),
 * and check cancellation per strip, tile and row.  Encoded output is proved
 * by the engine's VERIFY stage in a fresh session, not re-decoded here. */

#include "rfc_codecs.h"
#include "rfc_gif_scan.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gif_lib.h>
#include <tiffio.h>

#define TG_FIXED_OVERHEAD (8u * 1024u * 1024u) /* libtiff and giflib bookkeeping */

/* Test seam: called before each decoded strip, tile or row so tests can
 * raise cancellation mid-decode deterministically. */
static void (*g_tg_row_hook)(void *) = NULL;
static void *g_tg_row_hook_ctx = NULL;

void rfc_codecs_tiff_gif_set_row_hook_for_testing(void (*hook)(void *),
                                                  void *ctx) {
    g_tg_row_hook = hook;
    g_tg_row_hook_ctx = ctx;
}

static void tg_row_hook(void) {
    if (g_tg_row_hook) {
        g_tg_row_hook(g_tg_row_hook_ctx);
    }
}
#define TG_SEG_SLACK (8u * 1024u * 1024u) /* tile overhang beyond the frame */
#define TG_GIF_STATE_BYTES (1024u * 1024u) /* giflib LZW tables, colour maps */

/* ------------------------------------------------------------------------ */
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------ */

static void *tg_stdlib_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void tg_stdlib_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const rfc_allocator k_tg_stdlib_alloc = {tg_stdlib_alloc,
                                                tg_stdlib_free, NULL};

static const rfc_allocator *tg_alloc_of(const rfc_exec_context *exec) {
    const rfc_allocator *a = rfc_exec_allocator(exec);
    return (a && a->alloc && a->free) ? a : &k_tg_stdlib_alloc;
}

static const rfc_limits *tg_limits_of(const rfc_exec_context *exec,
                                      rfc_limits *scratch) {
    const rfc_limits *l = rfc_exec_limits(exec);
    if (l) {
        return l;
    }
    rfc_limits_default(scratch);
    return scratch;
}

typedef struct tg_session {
    atomic_int cancelled;
} tg_session;

static int tg_cancelled(const void *session, const rfc_exec_context *exec) {
    const tg_session *s = (const tg_session *)session;
    if (s && atomic_load_explicit(&s->cancelled, memory_order_acquire) != 0) {
        return 1;
    }
    return exec ? rfc_exec_is_cancelled(exec) : 0;
}

static rfc_status tg_open(const rfc_exec_context *exec, void **out) {
    (void)exec;
    if (!out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    tg_session *s = (tg_session *)malloc(sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&s->cancelled, 0);
    *out = s;
    return RFC_OK;
}

static void tg_close(void *session) {
    free(session);
}

static void tg_cancel(void *session) {
    tg_session *s = (tg_session *)session;
    if (s) {
        atomic_store_explicit(&s->cancelled, 1, memory_order_release);
    }
}

typedef struct tg_guard {
    rfc_budget *budget;
    size_t bytes;
    int active;
} tg_guard;

static rfc_status tg_reserve(const rfc_exec_context *exec, size_t bytes,
                             tg_guard *g) {
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

static void tg_release(tg_guard *g) {
    if (g->active && g->budget) {
        rfc_budget_release(g->budget, g->bytes);
    }
    memset(g, 0, sizeof(*g));
}

/* The frame an encoder reads: its layout with a zero (packed) stride
 * replaced by the normalized one, refused when the buffer is shorter than
 * that layout needs. */
static rfc_status tg_frame_layout(const rfc_frame *fr, rfc_layout *out) {
    rfc_layout norm;
    rfc_status st = rfc_layout_normalize_stride(&fr->layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    *out = fr->layout;
    if (out->stride_bytes == 0u) {
        out->stride_bytes = norm.stride_bytes;
    }
    size_t need = 0u;
    st = rfc_layout_validate(out, &need);
    if (st != RFC_OK) {
        return st;
    }
    return fr->data_len < need ? RFC_ERR_INVALID_ARGUMENT : RFC_OK;
}

/* Settings shared by both formats: metadata policy and exactness. */
static rfc_status tg_parse_settings(rfc_format_id fmt,
                                    const rfc_setting_value *settings,
                                    size_t count, int *out_strip) {
    *out_strip = 0;
    if (count > 0u) {
        rfc_status st = rfc_registry_validate_settings(fmt, settings, count, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }
    for (size_t i = 0u; i < count; i++) {
        if (strcmp(settings[i].id, "metadata") == 0) {
            *out_strip = strcmp(settings[i].value, "strip") == 0;
        } else if (strcmp(settings[i].id, "exactness") == 0 &&
                   strcmp(settings[i].value, "pixel-exact") != 0) {
            /* byte-identical is a same-format engine copy, never an encode */
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
    }
    return RFC_OK;
}

/* Growable output buffer through the job allocator (budget-counted). */
typedef struct tg_outbuf {
    const rfc_allocator *alloc;
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t pos; /* TIFF writes seek */
    int oom;
} tg_outbuf;

static int tg_out_reserve(tg_outbuf *o, size_t need) {
    if (need <= o->cap) {
        return 1;
    }
    size_t nc = o->cap ? o->cap : 4096u;
    while (nc < need) {
        if (nc > SIZE_MAX / 2u) {
            nc = need;
            break;
        }
        nc *= 2u;
    }
    uint8_t *nb = (uint8_t *)o->alloc->alloc(o->alloc->ctx, nc);
    if (!nb) {
        o->oom = 1;
        return 0;
    }
    if (o->data) {
        memcpy(nb, o->data, o->len);
        o->alloc->free(o->alloc->ctx, o->data);
    }
    memset(nb + o->len, 0, nc - o->len);
    o->data = nb;
    o->cap = nc;
    return 1;
}

static int tg_out_write_at(tg_outbuf *o, size_t at, const void *src, size_t n) {
    if (at > SIZE_MAX - n || !tg_out_reserve(o, at + n)) {
        return 0;
    }
    memcpy(o->data + at, src, n);
    if (at + n > o->len) {
        o->len = at + n;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* TIFF                                                                      */
/* ------------------------------------------------------------------------ */

typedef struct tif_mem {
    const uint8_t *data; /* read */
    size_t size;
    size_t pos;
    tg_outbuf *out;      /* write */
} tif_mem;

typedef struct tif_err {
    int errors;
    int short_read;
} tif_err;

static tmsize_t tif_read(thandle_t h, void *buf, tmsize_t n) {
    tif_mem *m = (tif_mem *)h;
    if (n < 0 || !m->data) {
        return 0;
    }
    if (m->pos >= m->size) {
        return 0; /* at or past the end: no pointer is formed */
    }
    size_t avail = m->size - m->pos;
    size_t k = (size_t)n < avail ? (size_t)n : avail;
    memcpy(buf, m->data + m->pos, k);
    m->pos += k;
    return (tmsize_t)k;
}

static tmsize_t tif_write(thandle_t h, void *buf, tmsize_t n) {
    tif_mem *m = (tif_mem *)h;
    if (n < 0 || !m->out || (!buf && n > 0)) {
        return 0;
    }
    if (!tg_out_write_at(m->out, m->pos, buf, (size_t)n)) {
        return 0;
    }
    m->pos += (size_t)n;
    return n;
}

static toff_t tif_seek(thandle_t h, toff_t off, int whence) {
    tif_mem *m = (tif_mem *)h;
    size_t end = m->out ? m->out->len : m->size;
    uint64_t base = (whence == SEEK_SET) ? 0u
                    : (whence == SEEK_CUR) ? (uint64_t)m->pos
                                           : (uint64_t)end;
    uint64_t np = base + (uint64_t)off;
    if (np > (uint64_t)SIZE_MAX) {
        return (toff_t)-1;
    }
    m->pos = (size_t)np;
    return (toff_t)np;
}

static int tif_close(thandle_t h) {
    (void)h;
    return 0;
}

static toff_t tif_size(thandle_t h) {
    tif_mem *m = (tif_mem *)h;
    return (toff_t)(m->out ? m->out->len : m->size);
}

static int tif_on_error(TIFF *tif, void *user, const char *module,
                        const char *fmt, va_list ap) {
    (void)tif;
    (void)module;
    (void)ap;
    tif_err *e = (tif_err *)user;
    e->errors++;
    if (fmt && (strstr(fmt, "Read error") || strstr(fmt, "short") ||
                strstr(fmt, "Seek error") || strstr(fmt, "Not enough data"))) {
        e->short_read = 1;
    }
    return 1; /* handled: never printed */
}

static int tif_on_warning(TIFF *tif, void *user, const char *module,
                          const char *fmt, va_list ap) {
    (void)tif;
    (void)user;
    (void)module;
    (void)fmt;
    (void)ap;
    return 1; /* unknown or private tags are not failures */
}

static TIFF *tif_open(const char *mode, tif_mem *m, tif_err *e,
                      size_t mem_cap) {
    TIFFOpenOptions *opts = TIFFOpenOptionsAlloc();
    if (!opts) {
        return NULL;
    }
    TIFFOpenOptionsSetErrorHandlerExtR(opts, tif_on_error, e);
    TIFFOpenOptionsSetWarningHandlerExtR(opts, tif_on_warning, e);
    TIFFOpenOptionsSetMaxSingleMemAlloc(opts, (tmsize_t)mem_cap);
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts, (tmsize_t)mem_cap);
    TIFF *tif = TIFFClientOpenExt("rfc", mode, (thandle_t)m, tif_read,
                                  tif_write, tif_seek, tif_close, tif_size,
                                  NULL, NULL, opts);
    TIFFOpenOptionsFree(opts);
    return tif;
}

/* libtiff's per-handle heap cap: bookkeeping plus twice `bytes` (reading:
 * the raw strip buffer and the strip offset/count arrays, each bounded by the
 * file; writing: one strip plus the carried ICC and XMP). */
static size_t tif_mem_cap(size_t bytes) {
    uint64_t cap = (uint64_t)TG_FIXED_OVERHEAD + 2u * (uint64_t)bytes;
    if (cap > (uint64_t)LONG_MAX) {
        cap = (uint64_t)LONG_MAX;
    }
    return (size_t)cap;
}

typedef struct tif_info {
    uint32_t w, h;
    uint16_t spp, bps, photometric, planar, compression, sampleformat;
    int alpha;
    rfc_layout layout;
    const uint8_t *icc;
    uint32_t icc_len;
    const uint8_t *xmp;
    uint32_t xmp_len;
    uint16_t orientation;
} tif_info;

/* Inspect the open TIFF's first and only IFD and decide what it decodes to. */
static rfc_status tif_inspect(TIFF *tif, const rfc_exec_context *exec,
                              const rfc_limits *limits, tif_info *ti) {
    memset(ti, 0, sizeof(*ti));
    /* a nonzero next-IFD pointer is enough to refuse: no chain walk */
    if (!TIFFLastDirectory(tif)) {
        rfc_exec_set_detail(exec, "tiff: more than one image (multi-page TIFF is not supported yet)");
        return RFC_ERR_UNSUPPORTED_COMBINATION; /* multi-page: D10 T02 s12 */
    }
    uint16_t nsub = 0u;
    void *subs = NULL;
    uint64_t exif_off = 0u, gps_off = 0u;
    if (TIFFGetField(tif, TIFFTAG_SUBIFD, &nsub, &subs) && nsub > 0u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (TIFFGetField(tif, TIFFTAG_EXIFIFD, &exif_off) ||
        TIFFGetField(tif, TIFFTAG_GPSIFD, &gps_off)) {
        rfc_exec_set_detail(exec, "tiff: an EXIF or GPS IFD cannot be carried yet");
        return RFC_ERR_UNSUPPORTED_COMBINATION; /* backlog B-007 */
    }
    float *fp = NULL;
    uint16_t *tf[3] = {NULL, NULL, NULL};
    /* TransferRange (342) has no libtiff symbol: an anonymous field exists
     * only when the tag was present in this IFD. */
    if (TIFFGetField(tif, TIFFTAG_WHITEPOINT, &fp) ||
        TIFFGetField(tif, TIFFTAG_PRIMARYCHROMATICITIES, &fp) ||
        TIFFGetField(tif, TIFFTAG_TRANSFERFUNCTION, &tf[0], &tf[1], &tf[2]) ||
        TIFFFindField(tif, 342, TIFF_ANY) != NULL) {
        rfc_exec_set_detail(exec, "tiff: WhitePoint, PrimaryChromaticities, TransferFunction or TransferRange cannot be carried");
        return RFC_ERR_UNSUPPORTED_COMBINATION; /* colour tags: backlog B-007 */
    }
    if (!TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &ti->w) ||
        !TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &ti->h) || ti->w == 0u ||
        ti->h == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t depth = 1u;
    TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGEDEPTH, &depth);
    if (depth != 1u) {
        /* each z slice is its own image: refused rather than flattened */
        rfc_exec_set_detail(exec, "tiff: a volumetric image (ImageDepth above 1)");
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &ti->spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &ti->bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &ti->planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &ti->compression);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &ti->sampleformat);
    if (!TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &ti->photometric)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    switch (ti->compression) {
        case COMPRESSION_NONE:
        case COMPRESSION_LZW:
        case COMPRESSION_ADOBE_DEFLATE:
        case COMPRESSION_DEFLATE:
        case COMPRESSION_PACKBITS:
        case COMPRESSION_JPEG:
        case COMPRESSION_WEBP:
            break;
        default:
            return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (ti->sampleformat != SAMPLEFORMAT_UINT ||
        (ti->bps != 8u && ti->bps != 16u)) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    uint16_t nextra = 0u;
    uint16_t *extra = NULL;
    TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &nextra, &extra);
    int ycbcr_jpeg = (ti->photometric == PHOTOMETRIC_YCBCR &&
                      ti->compression == COMPRESSION_JPEG && ti->spp == 3u &&
                      ti->bps == 8u);
    if (ti->photometric == PHOTOMETRIC_MINISBLACK && ti->spp == 1u &&
        nextra == 0u) {
        ti->layout.format = ti->bps == 8u ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_GRAY16;
    } else if ((ti->photometric == PHOTOMETRIC_RGB || ycbcr_jpeg) &&
               ti->spp == 3u && nextra == 0u) {
        ti->layout.format = ti->bps == 8u ? RFC_SAMPLE_RGB8 : RFC_SAMPLE_RGB16;
    } else if (ti->photometric == PHOTOMETRIC_RGB && ti->spp == 4u &&
               nextra == 1u && extra) {
        if (extra[0] == EXTRASAMPLE_ASSOCALPHA) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
        if (extra[0] != EXTRASAMPLE_UNASSALPHA) {
            return RFC_ERR_UNSUPPORTED_SAMPLE;
        }
        ti->layout.format = ti->bps == 8u ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGBA16;
        ti->alpha = 1;
    } else {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (ycbcr_jpeg) {
        TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
    }
    ti->layout.alpha = ti->alpha ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
    ti->layout.width = ti->w;
    ti->layout.height = ti->h;
    ti->layout.bits_per_sample = (uint8_t)ti->bps;
    ti->layout.stride_bytes = 0u;
    rfc_layout norm;
    rfc_status st = rfc_layout_normalize_stride(&ti->layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    ti->layout = norm;
    st = rfc_limits_check_image(limits, &ti->layout, 1u);
    if (st != RFC_OK) {
        return st;
    }
    void *icc = NULL, *xmp = NULL;
    if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &ti->icc_len, &icc) && icc &&
        ti->icc_len > 0u) {
        ti->icc = (const uint8_t *)icc;
    }
    if (TIFFGetField(tif, TIFFTAG_XMLPACKET, &ti->xmp_len, &xmp) && xmp &&
        ti->xmp_len > 0u) {
        ti->xmp = (const uint8_t *)xmp;
    }
    ti->orientation = ORIENTATION_TOPLEFT;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &ti->orientation);
    if (ti->orientation < 1u || ti->orientation > 8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_metadata m = {0};
    m.icc_len = ti->icc_len;
    m.xmp_len = ti->xmp_len;
    return rfc_limits_check_metadata(limits, &m);
}

/* Copy one decoded segment (a strip or tile of one plane, or all planes)
 * into the frame. seg_ch is the channel count stored in the segment. */
static void tif_place(rfc_frame *fr, const uint8_t *seg, uint32_t seg_w,
                      uint32_t seg_h, uint32_t x0, uint32_t y0, uint32_t ch,
                      uint32_t seg_ch, uint32_t plane, uint32_t bpsb) {
    const rfc_layout *L = &fr->layout;
    uint32_t cols = (x0 + seg_w > L->width) ? L->width - x0 : seg_w;
    for (uint32_t r = 0u; r < seg_h && y0 + r < L->height; r++) {
        const uint8_t *src = seg + (size_t)r * seg_w * seg_ch * bpsb;
        uint8_t *dst = fr->data + (size_t)(y0 + r) * L->stride_bytes +
                       (size_t)x0 * ch * bpsb;
        if (seg_ch == ch) {
            memcpy(dst, src, (size_t)cols * ch * bpsb);
        } else {
            for (uint32_t x = 0u; x < cols; x++) {
                memcpy(dst + ((size_t)x * ch + plane) * bpsb,
                       src + (size_t)x * bpsb, bpsb);
            }
        }
    }
}

static rfc_status tif_read_pixels(TIFF *tif, const tif_info *ti,
                                  const void *session,
                                  const rfc_exec_context *exec,
                                  const rfc_allocator *alloc, rfc_frame *fr,
                                  tif_err *e) {
    uint32_t ch = (uint32_t)rfc_sample_bytes(ti->layout.format) /
                  (ti->bps / 8u);
    uint32_t bpsb = ti->bps / 8u;
    int separate = (ti->planar == PLANARCONFIG_SEPARATE && ch > 1u);
    uint32_t planes = separate ? ch : 1u;
    uint32_t seg_ch = separate ? 1u : ch;
    int tiled = TIFFIsTiled(tif);
    tmsize_t seg_size = tiled ? TIFFTileSize(tif) : TIFFStripSize(tif);
    if (seg_size <= 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    /* A tile may overhang the image, but its decoded size is bounded before
     * it is allocated: at most the frame plus TG_SEG_SLACK (the provider
     * contract's fixed overhead covers the slack). */
    if ((uint64_t)seg_size >
        (uint64_t)fr->layout.stride_bytes * fr->layout.height + TG_SEG_SLACK) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint8_t *seg = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)seg_size);
    if (!seg) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_status st = RFC_OK;
    if (tiled) {
        uint32_t tw = 0u, th = 0u;
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
        if (tw == 0u || th == 0u ||
            (uint64_t)tw * th * seg_ch * bpsb > (uint64_t)seg_size) {
            st = RFC_ERR_INVALID_ARGUMENT;
        }
        for (uint32_t p = 0u; p < planes && st == RFC_OK; p++) {
            for (uint32_t y = 0u; y < ti->h && st == RFC_OK; y += th) {
                for (uint32_t x = 0u; x < ti->w && st == RFC_OK; x += tw) {
                    tg_row_hook();
                    if (tg_cancelled(session, exec)) {
                        st = RFC_ERR_CANCELLED;
                        break;
                    }
                    uint32_t t = TIFFComputeTile(tif, x, y, 0, (uint16_t)p);
                    if (TIFFReadEncodedTile(tif, t, seg, seg_size) < 0 ||
                        e->errors) {
                        st = e->short_read ? RFC_ERR_TRUNCATED_INPUT
                                           : RFC_ERR_INVALID_ARGUMENT;
                        break;
                    }
                    tif_place(fr, seg, tw, th, x, y, ch, seg_ch, p, bpsb);
                }
            }
        }
    } else {
        uint32_t rps = ti->h;
        TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rps);
        if (rps == 0u || rps > ti->h) {
            rps = ti->h;
        }
        uint32_t per_plane = (ti->h + rps - 1u) / rps;
        if (TIFFNumberOfStrips(tif) < per_plane * planes) {
            st = RFC_ERR_INVALID_ARGUMENT;
        }
        for (uint32_t p = 0u; p < planes && st == RFC_OK; p++) {
            for (uint32_t s = 0u; s < per_plane; s++) {
                tg_row_hook();
                if (tg_cancelled(session, exec)) {
                    st = RFC_ERR_CANCELLED;
                    break;
                }
                uint32_t rows = (s + 1u) * rps > ti->h ? ti->h - s * rps : rps;
                tmsize_t need = (tmsize_t)((uint64_t)rows * ti->w * seg_ch * bpsb);
                tmsize_t got = TIFFReadEncodedStrip(tif, p * per_plane + s, seg,
                                                    seg_size);
                if (got < need || e->errors) {
                    st = (got >= 0 && got < need) || e->short_read
                             ? RFC_ERR_TRUNCATED_INPUT
                             : RFC_ERR_INVALID_ARGUMENT;
                    break;
                }
                tif_place(fr, seg, ti->w, rows, 0u, s * rps, ch, seg_ch, p, bpsb);
            }
        }
    }
    alloc->free(alloc->ctx, seg);
    return st;
}

static rfc_status tif_decode_bytes(const void *session,
                                   const rfc_exec_context *exec,
                                   const uint8_t *bytes, size_t size,
                                   rfc_image_probe *out_probe,
                                   rfc_frame **out_frames) {
    rfc_limits scratch;
    const rfc_limits *limits = tg_limits_of(exec, &scratch);
    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }
    if (size < 8u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    size_t cap = tif_mem_cap(size);
    tg_guard g;
    st = tg_reserve(exec, cap, &g);
    if (st != RFC_OK) {
        return st;
    }
    tif_mem m = {bytes, size, 0u, NULL};
    tif_err e = {0, 0};
    TIFF *tif = tif_open("rm", &m, &e, cap);
    if (!tif) {
        tg_release(&g);
        return e.short_read ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_INVALID_ARGUMENT;
    }
    tif_info ti;
    st = tif_inspect(tif, exec, limits, &ti);
    const rfc_allocator *alloc = tg_alloc_of(exec);
    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    if (st == RFC_OK) {
        meta.icc = ti.icc;
        meta.icc_len = ti.icc_len;
        meta.xmp = ti.xmp;
        meta.xmp_len = ti.xmp_len;
        meta.orientation = (rfc_orientation)ti.orientation;
        if (out_probe) {
            memset(out_probe, 0, sizeof(*out_probe));
            out_probe->first_frame = ti.layout;
            out_probe->frame_count = 1u;
            out_probe->total_pixels = (uint64_t)ti.w * ti.h;
            out_probe->total_decoded_bytes =
                (uint64_t)ti.layout.stride_bytes * ti.h;
            st = rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
            if (st == RFC_OK) {
                /* copy metadata out through the exec allocator */
                rfc_frame tmp;
                memset(&tmp, 0, sizeof(tmp));
                rfc_layout one = {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 1u, 1u, 1u, 8u};
                st = rfc_frame_alloc(&tmp, &one, &meta, alloc);
                if (st == RFC_OK) {
                    out_probe->container_meta = tmp.metadata;
                    tmp.metadata.icc = NULL;
                    tmp.metadata.xmp = NULL;
                    tmp.metadata.exif = NULL;
                    tmp.metadata.icc_len = tmp.metadata.xmp_len = 0u;
                    rfc_frame_free(&tmp);
                }
            }
        }
    }
    if (st == RFC_OK && out_frames) {
        rfc_frame *frames =
            (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(rfc_frame));
        if (!frames) {
            st = RFC_ERR_OUT_OF_MEMORY;
        } else {
            memset(frames, 0, sizeof(*frames));
            st = exec ? rfc_exec_alloc_frame(exec, &frames[0], &ti.layout, &meta)
                      : rfc_frame_alloc(&frames[0], &ti.layout, &meta, alloc);
            if (st == RFC_OK) {
                st = tif_read_pixels(tif, &ti, session, exec, alloc,
                                     &frames[0], &e);
            }
            if (st != RFC_OK) {
                if (frames[0].data) {
                    rfc_frame_free(&frames[0]);
                }
                alloc->free(alloc->ctx, frames);
            } else {
                *out_frames = frames;
            }
        }
    }
    TIFFClose(tif);
    tg_release(&g);
    return st;
}

static rfc_status tif_probe(void *session, const rfc_exec_context *exec,
                            const uint8_t *bytes, size_t size,
                            rfc_image_probe *out_probe) {
    if (!out_probe || !bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (tg_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    return tif_decode_bytes(session, exec, bytes, size, out_probe, NULL);
}

static rfc_status tif_decode(void *session, const rfc_exec_context *exec,
                             const uint8_t *bytes, size_t size,
                             const rfc_setting_value *settings,
                             size_t setting_count, rfc_frame **out_frames,
                             size_t *out_frame_count, rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count || !bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (setting_count > 0u) {
        rfc_status vst = rfc_registry_validate_settings(RFC_FORMAT_TIFF, settings,
                                                        setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (tg_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_frame *frames = NULL;
    rfc_status st = tif_decode_bytes(session, exec, bytes, size, NULL, &frames);
    if (st != RFC_OK) {
        return st;
    }
    *out_frames = frames;
    *out_frame_count = 1u;
    if (out_meta) {
        *out_meta = frames[0].metadata;
    }
    return RFC_OK;
}

static rfc_status tif_encode(void *session, const rfc_exec_context *exec,
                             const rfc_frame *frames, size_t frame_count,
                             const rfc_metadata *meta,
                             const rfc_setting_value *settings,
                             size_t setting_count, rfc_encoded *out) {
    if (!out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    int strip = 0;
    rfc_status st = tg_parse_settings(RFC_FORMAT_TIFF, settings, setting_count,
                                      &strip);
    if (st != RFC_OK) {
        return st;
    }
    if (!frames || frame_count == 0u || !frames[0].data) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count > 1u) {
        rfc_exec_set_detail(exec, "tiff: more than one image (multi-page TIFF is not supported yet)");
        return RFC_ERR_UNSUPPORTED_COMBINATION; /* multi-page: D10 T02 s12 */
    }
    const rfc_frame *fr = &frames[0];
    rfc_layout lay_eff;
    st = tg_frame_layout(fr, &lay_eff);
    if (st != RFC_OK) {
        return st;
    }
    const rfc_layout *L = &lay_eff;
    uint16_t spp = 0u, bps = 0u, photometric = PHOTOMETRIC_RGB;
    int alpha = 0;
    switch (L->format) {
        case RFC_SAMPLE_GRAY8: spp = 1u; bps = 8u; photometric = PHOTOMETRIC_MINISBLACK; break;
        case RFC_SAMPLE_GRAY16: spp = 1u; bps = 16u; photometric = PHOTOMETRIC_MINISBLACK; break;
        case RFC_SAMPLE_RGB8: spp = 3u; bps = 8u; break;
        case RFC_SAMPLE_RGB16: spp = 3u; bps = 16u; break;
        case RFC_SAMPLE_RGBA8: spp = 4u; bps = 8u; alpha = 1; break;
        case RFC_SAMPLE_RGBA16: spp = 4u; bps = 16u; alpha = 1; break;
        default: return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (L->bits_per_sample != bps) {
        return RFC_ERR_UNSUPPORTED_SAMPLE; /* reduced depths are not widened */
    }
    if (alpha ? L->alpha != RFC_ALPHA_STRAIGHT : L->alpha != RFC_ALPHA_NONE) {
        return RFC_ERR_UNSUPPORTED_ALPHA;
    }
    rfc_limits scratch;
    const rfc_limits *limits = tg_limits_of(exec, &scratch);
    st = rfc_limits_check_image(limits, L, 1u);
    if (st != RFC_OK) {
        return st;
    }
    rfc_metadata eff = fr->metadata;
    if (meta) {
        if (eff.icc_len == 0u) { eff.icc = meta->icc; eff.icc_len = meta->icc_len; }
        if (eff.xmp_len == 0u) { eff.xmp = meta->xmp; eff.xmp_len = meta->xmp_len; }
        if (eff.orientation == RFC_ORIENTATION_UNSPECIFIED) eff.orientation = meta->orientation;
    }
    /* EXIF and non-ICC colorimetry are not carried (backlog B-007): inside a
     * job the engine has already refused them or recorded the user's
     * acknowledgement; a standalone call checks the policy here. */
    if (!exec || !exec->job) {
        st = rfc_metadata_check_source_preflight(
            RFC_FORMAT_TIFF, strip ? RFC_META_POLICY_STRIP : RFC_META_POLICY_PRESERVE,
            RFC_EXACT_PIXEL_EXACT, meta, frames, frame_count, 0u, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }
    if (tg_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    /* Strips of about 64 KiB: libtiff's heap then holds two strips. */
    size_t row_b = (size_t)L->width * spp * (bps / 8u);
    uint32_t rps = (uint32_t)(65536u / row_b);
    if (rps == 0u) {
        rps = 1u;
    }
    if (rps > L->height) {
        rps = L->height;
    }
    if (!strip) {
        /* the caller's metadata byte limit applies to what is carried */
        rfc_metadata carried_meta;
        memset(&carried_meta, 0, sizeof(carried_meta));
        carried_meta.icc_len = eff.icc_len;
        carried_meta.xmp_len = eff.xmp_len;
        st = rfc_limits_check_metadata(limits, &carried_meta);
        if (st != RFC_OK) {
            return st;
        }
    }
    /* libtiff copies the carried ICC and XMP into its directory and stages
     * them again while writing it. */
    size_t carried = strip ? 0u : eff.icc_len + eff.xmp_len;
    size_t cap = tif_mem_cap(row_b * rps + carried);
    tg_guard g;
    st = tg_reserve(exec, cap, &g);
    if (st != RFC_OK) {
        return st;
    }
    const rfc_allocator *alloc = tg_alloc_of(exec);
    tg_outbuf ob = {alloc, NULL, 0u, 0u, 0u, 0};
    tif_mem m = {NULL, 0u, 0u, &ob};
    tif_err e = {0, 0};
    TIFF *tif = tif_open("wm", &m, &e, cap);
    if (!tif) {
        tg_release(&g);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, L->width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, L->height);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spp);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photometric);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    if (alpha) {
        uint16_t ex = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &ex);
    }
    if (!strip) {
        if (eff.icc_len > 0u) {
            TIFFSetField(tif, TIFFTAG_ICCPROFILE, (uint32_t)eff.icc_len, eff.icc);
        }
        if (eff.xmp_len > 0u) {
            TIFFSetField(tif, TIFFTAG_XMLPACKET, (uint32_t)eff.xmp_len, eff.xmp);
        }
        if (eff.orientation > RFC_ORIENTATION_NORMAL) {
            TIFFSetField(tif, TIFFTAG_ORIENTATION, (uint16_t)eff.orientation);
        }
    }
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rps);
    uint8_t *strip_buf = (uint8_t *)alloc->alloc(alloc->ctx, row_b * rps);
    if (!strip_buf) {
        st = RFC_ERR_OUT_OF_MEMORY;
    }
    for (uint32_t s = 0u; st == RFC_OK && s * rps < L->height; s++) {
        if (tg_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        uint32_t rows = (s + 1u) * rps > L->height ? L->height - s * rps : rps;
        for (uint32_t r = 0u; r < rows; r++) {
            memcpy(strip_buf + (size_t)r * row_b,
                   fr->data + (size_t)(s * rps + r) * L->stride_bytes, row_b);
        }
        if (TIFFWriteEncodedStrip(tif, s, strip_buf, (tmsize_t)(row_b * rows)) < 0 ||
            e.errors || ob.oom) {
            st = ob.oom ? RFC_ERR_OUT_OF_MEMORY : RFC_ERR_INVALID_ARGUMENT;
        }
    }
    if (strip_buf) {
        alloc->free(alloc->ctx, strip_buf);
    }
    if (st == RFC_OK && (!TIFFWriteDirectory(tif) || e.errors || ob.oom)) {
        st = ob.oom ? RFC_ERR_OUT_OF_MEMORY : RFC_ERR_INVALID_ARGUMENT;
    }
    if (st != RFC_OK) {
        /* The output is discarded: detach the sink so TIFFClose's flush of an
         * unfinished directory writes nothing (libtiff may hand the write
         * callback a NULL buffer on that path). */
        m.out = NULL;
    }
    TIFFClose(tif);
    tg_release(&g);
    if (st == RFC_OK && (ob.oom || ob.len == 0u)) {
        st = RFC_ERR_OUT_OF_MEMORY;
    }
    if (st != RFC_OK) {
        if (ob.data) {
            alloc->free(alloc->ctx, ob.data);
        }
        return st;
    }
    out->data = ob.data;
    out->len = ob.len;
    out->allocator = alloc;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* GIF: zero-allocation structural pre-scan                                  */
/* ------------------------------------------------------------------------ */

typedef struct gif_scan {
    uint32_t sw, sh;
    uint32_t colors;      /* active colour table size */
    int interlaced;
    int has_transparent;
    uint8_t transparent;
    uint32_t delay_cs;
} gif_scan;

/* Length-only walk of the LZW code stream in the image's sub-blocks: codes
 * stay within the table, exactly `pixels` indices are produced, and the
 * end-of-information code is present (GIF89a appendix F). */
static rfc_status gif_check_lzw(const void *session,
                                const rfc_exec_context *exec,
                                const uint8_t *b, size_t size, size_t *pos,
                                uint32_t min_code, uint64_t pixels) {
    if (min_code < 2u || min_code > 8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint16_t lens[4096];
    uint32_t clear = 1u << min_code, eoi = clear + 1u;
    uint32_t next = clear + 2u, width = min_code + 1u;
    int have_prev = 0;
    uint32_t prev_len = 0u;
    uint64_t produced = 0u;
    uint32_t acc = 0u, nacc = 0u;
    int ended = 0;
    for (uint32_t i = 0u; i < clear; i++) {
        lens[i] = 1u;
    }
    size_t p = *pos;
    uint32_t blocks = 0u;
    for (;;) {
        if ((++blocks & 255u) == 0u) { /* every 256 sub-blocks (64 KiB) */
            tg_row_hook();
            if (tg_cancelled(session, exec)) {
                return RFC_ERR_CANCELLED;
            }
        }
        if (p >= size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint8_t n = b[p++];
        if (n == 0u) {
            break;
        }
        if ((size_t)n > size - p) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        for (uint8_t k = 0u; k < n && !ended; k++) {
            acc |= (uint32_t)b[p + k] << nacc;
            nacc += 8u;
            while (nacc >= width && !ended) {
                uint32_t code = acc & ((1u << width) - 1u);
                acc >>= width;
                nacc -= width;
                if (code == clear) {
                    next = clear + 2u;
                    width = min_code + 1u;
                    have_prev = 0;
                    continue;
                }
                if (code == eoi) {
                    ended = 1;
                    break;
                }
                uint32_t len;
                if (code < next && (code < clear || code > eoi)) {
                    len = lens[code];
                } else if (code == next && have_prev) {
                    len = prev_len + 1u;
                } else {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                if (have_prev && next < 4096u) {
                    lens[next] = (uint16_t)(prev_len + 1u);
                    next++;
                    if (next == (1u << width) && width < 12u) {
                        width++;
                    }
                }
                produced += len;
                if (produced > pixels) {
                    return RFC_ERR_INVALID_ARGUMENT; /* excess pixels */
                }
                prev_len = len;
                have_prev = 1;
            }
        }
        p += n;
    }
    if (!ended || produced != pixels) {
        return RFC_ERR_INVALID_ARGUMENT; /* missing end code or short raster */
    }
    *pos = p;
    return RFC_OK;
}

typedef struct tg_cancel_src {
    const void *session;
    const rfc_exec_context *exec;
} tg_cancel_src;

static int tg_walk_cancelled(void *ctx) {
    const tg_cancel_src *c = (const tg_cancel_src *)ctx;
    tg_row_hook();
    return tg_cancelled(c->session, c->exec);
}

/* The structural pre-scan: the walk shared with the submit sniff
 * (rfc_gif_scan.h) up to the first image, the exact LZW length check of that
 * image, then the next record, which must be the trailer. */
static rfc_status gif_prescan(const void *session, const rfc_exec_context *exec,
                              const uint8_t *b, size_t size,
                              const rfc_limits *limits, gif_scan *out) {
    memset(out, 0, sizeof(*out));
    tg_cancel_src cc = {session, exec};
    rfc_gif_walk w;
    rfc_status st = rfc_gif_walk_begin_bytes(&w, b, size, exec, limits,
                                             tg_walk_cancelled, &cc);
    if (st != RFC_OK) {
        return st;
    }
    rfc_gif_image img;
    st = rfc_gif_scan_first_image(&w, &img);
    if (st == RFC_OK) {
        out->sw = img.width;
        out->sh = img.height;
        out->colors = img.colors;
        out->interlaced = img.interlaced;
        out->has_transparent = img.has_transparent;
        out->transparent = img.transparent;
        out->delay_cs = img.delay_cs;
        /* the limits apply to the layout this image decodes to */
        st = rfc_limits_check_image(limits, &img.layout, 1u);
    }
    if (st == RFC_OK) {
        size_t pos = (size_t)img.lzw_pos;
        if (pos >= size) {
            st = RFC_ERR_TRUNCATED_INPUT;
        } else {
            uint32_t min_code = b[pos++];
            st = gif_check_lzw(session, exec, b, size, &pos, min_code,
                               (uint64_t)img.width * img.height);
        }
        if (st == RFC_OK) {
            uint64_t p = pos;
            uint8_t kind = 0u;
            st = rfc_gif_next_record(&w, &p, &kind);
            if (st == RFC_OK && kind == 0x2Cu) {
                rfc_exec_set_detail(exec, "gif: more than one image (animated GIF is not supported yet)");
                st = RFC_ERR_UNSUPPORTED_COMBINATION; /* animation: D10 T02 s12 */
            }
        }
    }
    return st;
}

/* ------------------------------------------------------------------------ */
/* GIF: decode through giflib                                                */
/* ------------------------------------------------------------------------ */

typedef struct gif_in {
    const uint8_t *data;
    size_t size;
    size_t pos;
} gif_in;

static int gif_read(GifFileType *gif, GifByteType *buf, int n) {
    gif_in *in = (gif_in *)gif->UserData;
    if (n <= 0) {
        return 0;
    }
    size_t avail = in->size - in->pos;
    size_t k = (size_t)n < avail ? (size_t)n : avail;
    memcpy(buf, in->data + in->pos, k);
    in->pos += k;
    return (int)k;
}

static rfc_status gif_decode_bytes(const void *session,
                                   const rfc_exec_context *exec,
                                   const uint8_t *bytes, size_t size,
                                   rfc_image_probe *out_probe,
                                   rfc_frame **out_frames) {
    rfc_limits scratch;
    const rfc_limits *limits = tg_limits_of(exec, &scratch);
    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }
    gif_scan gs;
    st = gif_prescan(session, exec, bytes, size, limits, &gs);
    if (st != RFC_OK) {
        return st;
    }
    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = gs.has_transparent ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8;
    lay.alpha = gs.has_transparent ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
    lay.width = gs.sw;
    lay.height = gs.sh;
    lay.bits_per_sample = 8u;
    rfc_layout norm;
    st = rfc_layout_normalize_stride(&lay, &norm);
    if (st != RFC_OK) {
        return st;
    }
    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.orientation = RFC_ORIENTATION_NORMAL;
    meta.duration_ms = gs.delay_cs * 10u;
    if (out_probe) {
        memset(out_probe, 0, sizeof(*out_probe));
        out_probe->first_frame = norm;
        out_probe->frame_count = 1u;
        out_probe->total_pixels = (uint64_t)gs.sw * gs.sh;
        out_probe->total_decoded_bytes = (uint64_t)norm.stride_bytes * gs.sh;
        out_probe->container_meta = meta;
        st = rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
        if (st != RFC_OK || !out_frames) {
            return st;
        }
    }
    if (!out_frames) {
        return RFC_OK;
    }
    /* giflib's own state (colour maps, LZW tables); the row buffer and the
     * frame go through the job allocator. */
    tg_guard g;
    st = tg_reserve(exec, TG_GIF_STATE_BYTES, &g);
    if (st != RFC_OK) {
        return st;
    }
    const rfc_allocator *alloc = tg_alloc_of(exec);
    rfc_frame *frames = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(rfc_frame));
    uint8_t *row = (uint8_t *)alloc->alloc(alloc->ctx, gs.sw);
    if (!frames || !row) {
        if (frames) alloc->free(alloc->ctx, frames);
        if (row) alloc->free(alloc->ctx, row);
        tg_release(&g);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(frames, 0, sizeof(*frames));
    st = exec ? rfc_exec_alloc_frame(exec, &frames[0], &norm, &meta)
              : rfc_frame_alloc(&frames[0], &norm, &meta, alloc);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, frames);
        alloc->free(alloc->ctx, row);
        tg_release(&g);
        return st;
    }
    gif_in in = {bytes, size, 0u};
    int err = 0;
    GifFileType *gif = DGifOpen(&in, gif_read, &err);
    if (!gif) {
        st = RFC_ERR_INVALID_ARGUMENT;
    }
    int done = 0;
    while (st == RFC_OK && !done) {
        GifRecordType rt;
        if (DGifGetRecordType(gif, &rt) != GIF_OK) {
            st = RFC_ERR_INVALID_ARGUMENT;
            break;
        }
        if (rt == EXTENSION_RECORD_TYPE) {
            int code = 0;
            GifByteType *ext = NULL;
            if (DGifGetExtension(gif, &code, &ext) != GIF_OK) {
                st = RFC_ERR_INVALID_ARGUMENT;
                break;
            }
            while (ext) {
                if (DGifGetExtensionNext(gif, &ext) != GIF_OK) {
                    st = RFC_ERR_INVALID_ARGUMENT;
                    break;
                }
            }
        } else if (rt == IMAGE_DESC_RECORD_TYPE) {
            if (DGifGetImageDesc(gif) != GIF_OK ||
                (uint32_t)gif->Image.Width != gs.sw ||
                (uint32_t)gif->Image.Height != gs.sh) {
                st = RFC_ERR_INVALID_ARGUMENT;
                break;
            }
            const ColorMapObject *cm =
                gif->Image.ColorMap ? gif->Image.ColorMap : gif->SColorMap;
            if (!cm || (uint32_t)cm->ColorCount != gs.colors) {
                st = RFC_ERR_INVALID_ARGUMENT;
                break;
            }
            static const uint32_t k_start[4] = {0u, 4u, 2u, 1u};
            static const uint32_t k_step[4] = {8u, 8u, 4u, 2u};
            uint32_t passes = gs.interlaced ? 4u : 1u;
            uint32_t ch = gs.has_transparent ? 4u : 3u;
            for (uint32_t ps = 0u; ps < passes && st == RFC_OK; ps++) {
                uint32_t y0 = gs.interlaced ? k_start[ps] : 0u;
                uint32_t dy = gs.interlaced ? k_step[ps] : 1u;
                for (uint32_t y = y0; y < gs.sh; y += dy) {
                    tg_row_hook();
                    if (tg_cancelled(session, exec)) {
                        st = RFC_ERR_CANCELLED;
                        break;
                    }
                    if (DGifGetLine(gif, row, (int)gs.sw) != GIF_OK) {
                        st = RFC_ERR_INVALID_ARGUMENT;
                        break;
                    }
                    uint8_t *dst = frames[0].data + (size_t)y * norm.stride_bytes;
                    for (uint32_t x = 0u; x < gs.sw; x++) {
                        uint32_t idx = row[x];
                        if (idx >= gs.colors) {
                            st = RFC_ERR_INVALID_ARGUMENT;
                            break;
                        }
                        const GifColorType *c = &cm->Colors[idx];
                        dst[x * ch + 0u] = c->Red;
                        dst[x * ch + 1u] = c->Green;
                        dst[x * ch + 2u] = c->Blue;
                        if (ch == 4u) {
                            dst[x * ch + 3u] =
                                (idx == gs.transparent) ? 0u : 255u;
                        }
                    }
                    if (st != RFC_OK) {
                        break;
                    }
                }
            }
        } else if (rt == TERMINATE_RECORD_TYPE) {
            done = 1;
        } else {
            st = RFC_ERR_INVALID_ARGUMENT;
        }
    }
    if (gif) {
        int cerr = 0;
        DGifCloseFile(gif, &cerr);
    }
    alloc->free(alloc->ctx, row);
    tg_release(&g);
    if (st != RFC_OK) {
        rfc_frame_free(&frames[0]);
        alloc->free(alloc->ctx, frames);
        return st;
    }
    *out_frames = frames;
    return RFC_OK;
}

static rfc_status gif_probe(void *session, const rfc_exec_context *exec,
                            const uint8_t *bytes, size_t size,
                            rfc_image_probe *out_probe) {
    if (!out_probe || !bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (tg_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    return gif_decode_bytes(session, exec, bytes, size, out_probe, NULL);
}

static rfc_status gif_decode(void *session, const rfc_exec_context *exec,
                             const uint8_t *bytes, size_t size,
                             const rfc_setting_value *settings,
                             size_t setting_count, rfc_frame **out_frames,
                             size_t *out_frame_count, rfc_metadata *out_meta) {
    if (!out_frames || !out_frame_count || !bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (setting_count > 0u) {
        rfc_status vst = rfc_registry_validate_settings(RFC_FORMAT_GIF, settings,
                                                        setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
    }
    if (tg_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_frame *frames = NULL;
    rfc_status st = gif_decode_bytes(session, exec, bytes, size, NULL, &frames);
    if (st != RFC_OK) {
        return st;
    }
    *out_frames = frames;
    *out_frame_count = 1u;
    if (out_meta) {
        *out_meta = frames[0].metadata;
    }
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* GIF: exact-palette encode                                                 */
/* ------------------------------------------------------------------------ */

typedef struct gif_out {
    tg_outbuf *ob;
} gif_out;

static int gif_write(GifFileType *gif, const GifByteType *buf, int n) {
    gif_out *o = (gif_out *)gif->UserData;
    if (n <= 0) {
        return 0;
    }
    if (!tg_out_write_at(o->ob, o->ob->len, buf, (size_t)n)) {
        return 0;
    }
    return n;
}

/* Small open-addressing set of 24-bit colours -> palette index. */
typedef struct gif_pal {
    uint32_t key[1024];  /* rgb + 1, 0 = empty */
    uint16_t idx[1024];
    GifColorType colors[256];
    uint32_t count;
} gif_pal;

static int gif_pal_index(gif_pal *p, uint32_t rgb, uint32_t limit,
                         int *out_idx) {
    uint32_t h = (rgb * 2654435761u) >> 22;
    for (;;) {
        if (p->key[h] == 0u) {
            if (p->count >= limit) {
                return 0;
            }
            p->key[h] = rgb + 1u;
            p->idx[h] = (uint16_t)p->count;
            p->colors[p->count].Red = (GifByteType)(rgb >> 16);
            p->colors[p->count].Green = (GifByteType)(rgb >> 8);
            p->colors[p->count].Blue = (GifByteType)rgb;
            p->count++;
            *out_idx = p->idx[h];
            return 1;
        }
        if (p->key[h] == rgb + 1u) {
            *out_idx = p->idx[h];
            return 1;
        }
        h = (h + 1u) & 1023u;
    }
}

static rfc_status gif_encode(void *session, const rfc_exec_context *exec,
                             const rfc_frame *frames, size_t frame_count,
                             const rfc_metadata *meta,
                             const rfc_setting_value *settings,
                             size_t setting_count, rfc_encoded *out) {
    if (!out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    int strip = 0;
    rfc_status st = tg_parse_settings(RFC_FORMAT_GIF, settings, setting_count,
                                      &strip);
    if (st != RFC_OK) {
        return st;
    }
    if (!frames || frame_count == 0u || !frames[0].data) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count > 1u) {
        rfc_exec_set_detail(exec, "gif: more than one frame (animated GIF export is not supported yet)");
        return RFC_ERR_UNSUPPORTED_COMBINATION; /* animation: D10 T02 s12 */
    }
    const rfc_frame *fr = &frames[0];
    rfc_layout lay_eff;
    st = tg_frame_layout(fr, &lay_eff);
    if (st != RFC_OK) {
        return st;
    }
    const rfc_layout *L = &lay_eff;
    int rgba = 0;
    if (L->format == RFC_SAMPLE_RGBA8) {
        rgba = 1;
        if (L->alpha != RFC_ALPHA_STRAIGHT) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
    } else if (L->format != RFC_SAMPLE_RGB8 || L->alpha != RFC_ALPHA_NONE) {
        return L->format == RFC_SAMPLE_RGB8 ? RFC_ERR_UNSUPPORTED_ALPHA
                                            : RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (L->bits_per_sample != 8u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (L->width > 65535u || L->height > 65535u) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    rfc_limits scratch;
    const rfc_limits *limits = tg_limits_of(exec, &scratch);
    st = rfc_limits_check_image(limits, L, 1u);
    if (st != RFC_OK) {
        return st;
    }
    uint32_t dur = fr->metadata.duration_ms ? fr->metadata.duration_ms
                                            : (meta ? meta->duration_ms : 0u);
    if (dur % 10u != 0u || dur / 10u > 65535u) {
        rfc_exec_set_detail(exec, "gif: frame duration is not a whole number of centiseconds up to 655350 ms");
        return RFC_ERR_UNSUPPORTED_COMBINATION; /* centisecond delays only */
    }
    /* GIF carries frame timing only: inside a job the engine has already
     * refused other metadata or recorded its acknowledgement. */
    if (!exec || !exec->job) {
        st = rfc_metadata_check_source_preflight(
            RFC_FORMAT_GIF, strip ? RFC_META_POLICY_STRIP : RFC_META_POLICY_PRESERVE,
            RFC_EXACT_PIXEL_EXACT, meta, frames, frame_count, 0u, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }
    if (tg_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }
    const rfc_allocator *alloc = tg_alloc_of(exec);
    gif_pal *pal = (gif_pal *)alloc->alloc(alloc->ctx, sizeof(*pal));
    uint8_t *indices = pal ? (uint8_t *)alloc->alloc(alloc->ctx,
                                                     (size_t)L->width * L->height)
                           : NULL;
    if (!pal || !indices) {
        if (pal) alloc->free(alloc->ctx, pal);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(pal, 0, sizeof(*pal));
    /* Opaque colours first; one extra entry for the transparent RGB. */
    int have_transp = 0;
    uint32_t transp_rgb = 0u;
    for (uint32_t y = 0u; y < L->height && st == RFC_OK; y++) {
        tg_row_hook();
        if (tg_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        const uint8_t *src = fr->data + (size_t)y * L->stride_bytes;
        for (uint32_t x = 0u; x < L->width; x++) {
            const uint8_t *px = src + (size_t)x * (rgba ? 4u : 3u);
            uint32_t rgb = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            if (rgba && px[3] != 255u) {
                if (px[3] != 0u) {
                    rfc_exec_set_detail(exec, "gif: partial alpha (only 0 and 255 are exact in GIF)");
                    st = RFC_ERR_UNSUPPORTED_COMBINATION; /* partial alpha */
                    break;
                }
                if (have_transp && rgb != transp_rgb) {
                    rfc_exec_set_detail(exec, "gif: transparent pixels with differing RGB values");
                    st = RFC_ERR_UNSUPPORTED_COMBINATION; /* differing RGB */
                    break;
                }
                have_transp = 1;
                transp_rgb = rgb;
                indices[(size_t)y * L->width + x] = 255u; /* placeholder */
                continue;
            }
            int idx = 0;
            if (!gif_pal_index(pal, rgb, rgba ? 255u : 256u, &idx)) {
                rfc_exec_set_detail(exec, "gif: palette reduction (more distinct colours than the palette holds)");
                st = RFC_ERR_UNSUPPORTED_COMBINATION; /* palette reduction */
                break;
            }
            indices[(size_t)y * L->width + x] = (uint8_t)idx;
        }
    }
    uint32_t tindex = 0u;
    if (st == RFC_OK && rgba) {
        /* The transparent entry always exists so a fresh decode yields
         * rgba8; it holds the transparent pixels' RGB (black if none). */
        if (pal->count >= 256u) {
            rfc_exec_set_detail(exec, "gif: palette reduction (rgba8 needs a free entry for its transparent index)");
            st = RFC_ERR_UNSUPPORTED_COMBINATION;
        } else {
            tindex = pal->count++;
            pal->colors[tindex].Red = (GifByteType)(transp_rgb >> 16);
            pal->colors[tindex].Green = (GifByteType)(transp_rgb >> 8);
            pal->colors[tindex].Blue = (GifByteType)transp_rgb;
            if (have_transp) {
                for (uint32_t y = 0u; y < L->height && st == RFC_OK; y++) {
                    if (tg_cancelled(session, exec)) {
                        st = RFC_ERR_CANCELLED;
                        break;
                    }
                    const uint8_t *src = fr->data + (size_t)y * L->stride_bytes;
                    for (uint32_t x = 0u; x < L->width; x++) {
                        if (src[(size_t)x * 4u + 3u] == 0u) {
                            indices[(size_t)y * L->width + x] = (uint8_t)tindex;
                        }
                    }
                }
            }
        }
    }
    uint32_t table = 2u;
    int bits = 1;
    while (table < pal->count) {
        table <<= 1;
        bits++;
    }
    tg_outbuf ob = {alloc, NULL, 0u, 0u, 0u, 0};
    gif_out go = {&ob};
    GifFileType *gif = NULL;
    ColorMapObject *cm = NULL;
    int err = 0;
    tg_guard g;
    memset(&g, 0, sizeof(g));
    if (st == RFC_OK) {
        st = tg_reserve(exec, TG_GIF_STATE_BYTES, &g);
    }
    if (st == RFC_OK) {
        GifColorType full[256];
        memset(full, 0, sizeof(full));
        memcpy(full, pal->colors, sizeof(GifColorType) * pal->count);
        cm = GifMakeMapObject((int)table, full);
        gif = cm ? EGifOpen(&go, gif_write, &err) : NULL;
        if (!gif) {
            st = RFC_ERR_OUT_OF_MEMORY;
        }
    }
    int need_gce = rgba || dur > 0u;
    if (st == RFC_OK) {
        EGifSetGifVersion(gif, need_gce);
        if (EGifPutScreenDesc(gif, (int)L->width, (int)L->height, bits, 0, cm) !=
            GIF_OK) {
            st = ob.oom ? RFC_ERR_OUT_OF_MEMORY : RFC_ERR_INVALID_ARGUMENT;
        }
    }
    if (st == RFC_OK && need_gce) {
        GifByteType gce[4];
        gce[0] = (GifByteType)(rgba ? 1u : 0u);
        gce[1] = (GifByteType)((dur / 10u) & 0xFFu);
        gce[2] = (GifByteType)((dur / 10u) >> 8);
        gce[3] = (GifByteType)tindex;
        if (EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, gce) != GIF_OK) {
            st = ob.oom ? RFC_ERR_OUT_OF_MEMORY : RFC_ERR_INVALID_ARGUMENT;
        }
    }
    if (st == RFC_OK &&
        EGifPutImageDesc(gif, 0, 0, (int)L->width, (int)L->height, false, NULL) !=
            GIF_OK) {
        st = ob.oom ? RFC_ERR_OUT_OF_MEMORY : RFC_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t y = 0u; st == RFC_OK && y < L->height; y++) {
        if (tg_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        if (EGifPutLine(gif, indices + (size_t)y * L->width, (int)L->width) !=
            GIF_OK) {
            st = ob.oom ? RFC_ERR_OUT_OF_MEMORY : RFC_ERR_INVALID_ARGUMENT;
        }
    }
    if (gif) {
        int cerr = 0;
        if (EGifCloseFile(gif, &cerr) != GIF_OK && st == RFC_OK) {
            st = ob.oom ? RFC_ERR_OUT_OF_MEMORY : RFC_ERR_INVALID_ARGUMENT;
        }
    }
    /* giflib ignores the write callback's result for the trailer: the
     * sticky output error decides. */
    if (st == RFC_OK && ob.oom) {
        st = RFC_ERR_OUT_OF_MEMORY;
    }
    if (cm) {
        GifFreeMapObject(cm);
    }
    tg_release(&g);
    alloc->free(alloc->ctx, indices);
    alloc->free(alloc->ctx, pal);
    if (st != RFC_OK) {
        if (ob.data) {
            alloc->free(alloc->ctx, ob.data);
        }
        return st;
    }
    out->data = ob.data;
    out->len = ob.len;
    out->allocator = alloc;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static const rfc_session_adapter k_tiff_adapter = {
    RFC_CODEC_ABI_VERSION, RFC_FORMAT_TIFF, tg_open, tg_close, tif_probe,
    tif_encode, tif_decode, tg_cancel, NULL};

static const rfc_session_adapter k_gif_adapter = {
    RFC_CODEC_ABI_VERSION, RFC_FORMAT_GIF, tg_open, tg_close, gif_probe,
    gif_encode, gif_decode, tg_cancel, NULL};

rfc_status rfc_codecs_register_tiff_gif(void) {
    return rfc_registry_register_session_pair(&k_tiff_adapter, &k_gif_adapter);
}

rfc_status rfc_codecs_unregister_tiff_gif(void) {
    return rfc_registry_unregister_session_pair(RFC_FORMAT_TIFF, RFC_FORMAT_GIF);
}
