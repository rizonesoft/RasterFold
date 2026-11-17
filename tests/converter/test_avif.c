/* C11 codec & format adapter test suite + CLI harness for D10 T02 s4:
 *   RFC_FORMAT_AVIF (libavif 1.4.2 + libaom 3.15.3 lossless 8/10/12-bit
 *   YUV444 IDENTITY RGB/RGBA encode/decode, non-native YUV420/422/400 import,
 *   all-opaque alpha retention, ICC/cICP/cLLI/mDCV/Exif/XMP + irot/imir
 *   orientation, same-format byte-identical verified copy, ISOBMFF preflight
 *   refusals, limits/budget, cancellation, and retry isolation). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
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

#include <aom/aom_codec.h>
#include <avif/avif.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

typedef struct tracked_alloc_state {
    size_t live;
    size_t peak;
    size_t calls;
} tracked_alloc_state;

typedef struct tracked_hdr {
    size_t size;
} tracked_hdr;

static void *tracked_alloc(void *ctx, size_t sz) {
    tracked_alloc_state *st = (tracked_alloc_state *)ctx;
    size_t total = sizeof(tracked_hdr) + (sz ? sz : 1u);
    tracked_hdr *h = (tracked_hdr *)malloc(total);
    if (!h) {
        return NULL;
    }
    h->size = sz;
    st->live += sz;
    if (st->live > st->peak) {
        st->peak = st->live;
    }
    st->calls++;
    return (void *)(h + 1);
}

static void tracked_free(void *ctx, void *ptr) {
    if (!ptr) {
        return;
    }
    tracked_alloc_state *st = (tracked_alloc_state *)ctx;
    tracked_hdr *h = ((tracked_hdr *)ptr) - 1;
    st->live -= h->size;
    free(h);
}

static void init_exec(rfc_exec_context *exec,
                      tracked_alloc_state *st,
                      rfc_allocator *al,
                      const rfc_limits *lim_opt,
                      uint32_t threads) {
    memset(st, 0, sizeof(*st));
    al->alloc = tracked_alloc;
    al->free = tracked_free;
    al->ctx = st;
    memset(exec, 0, sizeof(*exec));
    if (lim_opt) {
        exec->limits = *lim_opt;
    } else {
        rfc_limits_default(&exec->limits);
    }
    exec->allocator = al;
    exec->codec_threads = threads ? threads : 1u;
}

static FILE *fopen_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    int wp_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                     NULL, 0);
    int wm_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1,
                                     NULL, 0);
    if (wp_len <= 0 || wm_len <= 0 || wm_len > 16) {
        return NULL;
    }
    wchar_t *wp = (wchar_t *)malloc((size_t)wp_len * sizeof(wchar_t));
    if (!wp) {
        return NULL;
    }
    wchar_t wm[16];
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wp, wp_len);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, wm, wm_len);
    FILE *fp = _wfopen(wp, wm);
    free(wp);
    return fp;
#else
    return fopen(path, mode);
#endif
}

static int write_file_all(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen_utf8(path, "wb");
    if (!fp) {
        return 0;
    }
    int ok = (len == 0u || fwrite(data, 1u, len, fp) == len);
    if (fclose(fp) != 0) {
        ok = 0;
    }
    return ok;
}

static uint8_t *read_file_all(const char *path, size_t *out_len) {
    *out_len = 0u;
    FILE *fp = fopen_utf8(path, "rb");
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
    size_t n = (size_t)sz;
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (n > 0u && fread(buf, 1u, n, fp) != n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_len = n;
    return buf;
}

static void make_temp_path(char *buf, size_t cap, const char *tag) {
    static unsigned counter = 0u;
    counter++;
#ifdef _WIN32
    char td[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(td), td);
    CHECK(n > 0u && n < sizeof(td));
    snprintf(buf, cap, "%srfc_avif_%lu_%u_%s", td,
             (unsigned long)GetCurrentProcessId(), counter, tag);
#else
    snprintf(buf, cap, "/tmp/rfc_avif_%ld_%u_%s", (long)getpid(), counter, tag);
#endif
}

static void fill_pattern_frame(rfc_frame *fr,
                               uint32_t w,
                               uint32_t h,
                               rfc_sample_format fmt,
                               rfc_alpha_mode alpha,
                               uint32_t bps,
                               int force_opaque_alpha,
                               const rfc_metadata *meta,
                               const rfc_allocator *al) {
    uint32_t container_bits = (fmt == RFC_SAMPLE_RGB8 || fmt == RFC_SAMPLE_RGBA8 ||
                               fmt == RFC_SAMPLE_GRAY8)
                                  ? 8u
                                  : 16u;
    uint32_t ch = (fmt == RFC_SAMPLE_GRAY8 || fmt == RFC_SAMPLE_GRAY16) ? 1u
                  : (fmt == RFC_SAMPLE_RGBA8 || fmt == RFC_SAMPLE_RGBA16) ? 4u
                                                                          : 3u;
    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = fmt;
    lay.alpha = alpha;
    lay.width = w;
    lay.height = h;
    lay.stride_bytes = (size_t)w * (size_t)ch * (size_t)(container_bits / 8u);
    lay.bits_per_sample = bps;
    CHECK(rfc_frame_alloc(fr, &lay, meta, al) == RFC_OK);

    uint32_t max_nat = (bps == 16u) ? 65535u : ((1u << bps) - 1u);
    uint32_t shift = container_bits - bps;
    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *row = fr->data + (size_t)y * lay.stride_bytes;
        for (uint32_t x = 0u; x < w; x++) {
            for (uint32_t c = 0u; c < ch; c++) {
                uint32_t v;
                if (c == 3u && force_opaque_alpha) {
                    v = max_nat;
                } else {
                    v = ((x * 131u + y * 257u + c * 73u + (x ^ y) * 19u) &
                         max_nat);
                    if (c == 3u && ((x + y) % 5u == 0u)) {
                        v = max_nat;
                    } else if (c == 3u && ((x + y) % 7u == 0u)) {
                        v = 0u;
                    }
                }
                if (container_bits == 8u) {
                    row[(size_t)x * ch + c] = (uint8_t)v;
                } else {
                    uint16_t packed = (uint16_t)(v << shift);
                    memcpy(row + ((size_t)x * ch + c) * 2u, &packed, 2u);
                }
            }
        }
    }
}

static size_t build_le_exif_with_ori(uint16_t ori, uint8_t out[26]) {
    memset(out, 0, 26u);
    out[0] = 'I';
    out[1] = 'I';
    wr16le(out + 2u, 42u);
    wr32le(out + 4u, 8u);
    wr16le(out + 8u, 1u);
    wr16le(out + 10u, 0x0112u);
    wr16le(out + 12u, 3u);
    wr32le(out + 14u, 1u);
    wr16le(out + 18u, ori);
    return 26u;
}

static size_t build_le_exif_no_ori(uint8_t out[26]) {
    memset(out, 0, 26u);
    out[0] = 'I';
    out[1] = 'I';
    wr16le(out + 2u, 42u);
    wr32le(out + 4u, 8u);
    wr16le(out + 8u, 1u);
    wr16le(out + 10u, 0x011Au); /* XResolution */
    wr16le(out + 12u, 4u);
    wr32le(out + 14u, 1u);
    wr32le(out + 18u, 72u);
    return 26u;
}

static void build_sample_icc(uint8_t out[128]) {
    memset(out, 0, 128u);
    wr32be(out + 0u, 128u);
    memcpy(out + 4u, "lcms", 4u);
    wr32be(out + 8u, 0x02100000u);
    memcpy(out + 12u, "mntr", 4u);
    memcpy(out + 16u, "RGB ", 4u);
    memcpy(out + 20u, "XYZ ", 4u);
    memcpy(out + 36u, "acsp", 4u);
    out[80] = 'R';
    out[81] = 'F';
}

/* Search ISOBMFF nested container boxes recursively for a leaf box type. */
static const uint8_t *find_isobmff_box(const uint8_t *data,
                                       size_t len,
                                       const char type[4],
                                       size_t *out_pay_len) {
    size_t pos = 0u;
    while (pos + 8u <= len) {
        uint64_t sz = (uint64_t)rd32be(data + pos);
        const uint8_t *tp = data + pos + 4u;
        size_t hdr = 8u;
        if (sz == 1u) {
            if (pos + 16u > len) {
                return NULL;
            }
            sz = ((uint64_t)rd32be(data + pos + 8u) << 32) |
                 (uint64_t)rd32be(data + pos + 12u);
            hdr = 16u;
        } else if (sz == 0u) {
            sz = (uint64_t)(len - pos);
        }
        if (sz < (uint64_t)hdr || sz > (uint64_t)(len - pos)) {
            return NULL;
        }
        size_t pay_off = pos + hdr;
        size_t pay_len = (size_t)sz - hdr;
        if (memcmp(tp, type, 4u) == 0) {
            if (out_pay_len) {
                *out_pay_len = pay_len;
            }
            return data + pay_off;
        }
        if (memcmp(tp, "meta", 4u) == 0 && pay_len >= 4u) {
            const uint8_t *r =
                find_isobmff_box(data + pay_off + 4u, pay_len - 4u, type,
                                 out_pay_len);
            if (r) {
                return r;
            }
        } else if (memcmp(tp, "iprp", 4u) == 0 ||
                   memcmp(tp, "ipco", 4u) == 0) {
            const uint8_t *r =
                find_isobmff_box(data + pay_off, pay_len, type, out_pay_len);
            if (r) {
                return r;
            }
        }
        pos += (size_t)sz;
    }
    return NULL;
}

static uint8_t *synthesize_avif(uint32_t w,
                                uint32_t h,
                                uint32_t depth,
                                avifPixelFormat yuv_fmt,
                                avifRange yuv_range,
                                avifMatrixCoefficients matrix,
                                int with_alpha,
                                int opaque_alpha,
                                const uint8_t *icc,
                                size_t icc_len,
                                int with_cicp,
                                uint8_t cp,
                                uint8_t tc,
                                size_t *out_len) {
    *out_len = 0u;
    avifImage *img = avifImageCreate(w, h, depth, yuv_fmt);
    CHECK(img != NULL);
    img->yuvRange = yuv_range;
    img->matrixCoefficients = matrix;
    if (with_cicp) {
        img->colorPrimaries = (avifColorPrimaries)cp;
        img->transferCharacteristics = (avifTransferCharacteristics)tc;
    }
    if (icc && icc_len > 0u) {
        CHECK(avifImageSetProfileICC(img, icc, icc_len) == AVIF_RESULT_OK);
    }
    CHECK(avifImageAllocatePlanes(
              img, with_alpha ? AVIF_PLANES_ALL : AVIF_PLANES_YUV) ==
          AVIF_RESULT_OK);
    uint32_t maxv = (depth == 8u) ? 255u : ((1u << depth) - 1u);
    uint32_t cw = (yuv_fmt == AVIF_PIXEL_FORMAT_YUV420 ||
                   yuv_fmt == AVIF_PIXEL_FORMAT_YUV422)
                      ? ((w + 1u) / 2u)
                      : w;
    uint32_t ch_h = (yuv_fmt == AVIF_PIXEL_FORMAT_YUV420) ? ((h + 1u) / 2u) : h;
    for (uint32_t y = 0u; y < h; y++) {
        if (depth == 8u) {
            uint8_t *py = img->yuvPlanes[AVIF_CHAN_Y] +
                          (size_t)y * img->yuvRowBytes[AVIF_CHAN_Y];
            for (uint32_t x = 0u; x < w; x++) {
                uint32_t v = (yuv_range == AVIF_RANGE_LIMITED)
                                 ? (16u + ((x * 7u + y * 13u) % 219u))
                                 : ((x * 17u + y * 31u) & 255u);
                py[x] = (uint8_t)v;
            }
            if (with_alpha) {
                uint8_t *pa =
                    img->alphaPlane + (size_t)y * img->alphaRowBytes;
                for (uint32_t x = 0u; x < w; x++) {
                    pa[x] = opaque_alpha ? 255u : (uint8_t)((x * 29u + y) & 255u);
                }
            }
        } else {
            uint16_t *wy = (uint16_t *)(img->yuvPlanes[AVIF_CHAN_Y] +
                                        (size_t)y * img->yuvRowBytes[AVIF_CHAN_Y]);
            for (uint32_t x = 0u; x < w; x++) {
                wy[x] = (uint16_t)((x * 67u + y * 131u) & maxv);
            }
            if (with_alpha) {
                uint16_t *wa = (uint16_t *)(img->alphaPlane +
                                            (size_t)y * img->alphaRowBytes);
                for (uint32_t x = 0u; x < w; x++) {
                    wa[x] = opaque_alpha ? (uint16_t)maxv
                                         : (uint16_t)((x * 41u + y) & maxv);
                }
            }
        }
    }
    if (yuv_fmt != AVIF_PIXEL_FORMAT_YUV400) {
        for (uint32_t y = 0u; y < ch_h; y++) {
            if (depth == 8u) {
                uint8_t *pu = img->yuvPlanes[AVIF_CHAN_U] +
                              (size_t)y * img->yuvRowBytes[AVIF_CHAN_U];
                uint8_t *pv = img->yuvPlanes[AVIF_CHAN_V] +
                              (size_t)y * img->yuvRowBytes[AVIF_CHAN_V];
                for (uint32_t x = 0u; x < cw; x++) {
                    pu[x] = (uint8_t)(96u + ((x * 5u + y * 3u) & 63u));
                    pv[x] = (uint8_t)(96u + ((x * 7u + y * 9u) & 63u));
                }
            } else {
                uint16_t *wu = (uint16_t *)(img->yuvPlanes[AVIF_CHAN_U] +
                                            (size_t)y * img->yuvRowBytes[AVIF_CHAN_U]);
                uint16_t *wv = (uint16_t *)(img->yuvPlanes[AVIF_CHAN_V] +
                                            (size_t)y * img->yuvRowBytes[AVIF_CHAN_V]);
                uint32_t half = maxv / 2u;
                for (uint32_t x = 0u; x < cw; x++) {
                    wu[x] = (uint16_t)((half + (x * 13u + y)) & maxv);
                    wv[x] = (uint16_t)((half + (x * 19u + y)) & maxv);
                }
            }
        }
    }

    avifEncoder *enc = avifEncoderCreate();
    CHECK(enc != NULL);
    enc->codecChoice = AVIF_CODEC_CHOICE_AOM;
    enc->speed = 10;
    enc->quality = 90;
    avifAddImageFlags fl = (with_alpha && opaque_alpha)
                               ? AVIF_ADD_IMAGE_FLAG_FORCE_KEYFRAME
                               : AVIF_ADD_IMAGE_FLAG_SINGLE;
    avifRWData raw = AVIF_DATA_EMPTY;
    CHECK(avifEncoderAddImage(enc, img, 1u, fl) == AVIF_RESULT_OK);
    CHECK(avifEncoderFinish(enc, &raw) == AVIF_RESULT_OK);
    avifEncoderDestroy(enc);
    avifImageDestroy(img);
    uint8_t *out = (uint8_t *)malloc(raw.size);
    CHECK(out != NULL);
    memcpy(out, raw.data, raw.size);
    *out_len = raw.size;
    avifRWDataFree(&raw);
    return out;
}

static size_t wrap_isobmff_box(const char type[4],
                               const uint8_t *pay,
                               size_t pay_len,
                               uint8_t *out) {
    wr32be(out, (uint32_t)(8u + pay_len));
    memcpy(out + 4u, type, 4u);
    if (pay_len > 0u) {
        memcpy(out + 8u, pay, pay_len);
    }
    return 8u + pay_len;
}

static size_t wrap_fullbox(const char type[4],
                           uint8_t ver,
                           uint32_t flags,
                           const uint8_t *pay,
                           size_t pay_len,
                           uint8_t *out) {
    wr32be(out, (uint32_t)(12u + pay_len));
    memcpy(out + 4u, type, 4u);
    out[8] = ver;
    out[9] = (uint8_t)((flags >> 16) & 0xffu);
    out[10] = (uint8_t)((flags >> 8) & 0xffu);
    out[11] = (uint8_t)(flags & 0xffu);
    if (pay_len > 0u) {
        memcpy(out + 12u, pay, pay_len);
    }
    return 12u + pay_len;
}

static size_t build_synthetic_meta_avif(int duplicate_exif,
                                        int with_tmap,
                                        uint64_t exif_iloc_len,
                                        size_t big_icc_len,
                                        uint8_t *out,
                                        size_t out_cap) {
    uint8_t buf[2048];
    size_t pos = 0u;

    uint8_t ftyp_p[12] = {'a', 'v', 'i', 'f', 0, 0, 0, 0, 'm', 'i', 'f', '1'};
    pos += wrap_isobmff_box("ftyp", ftyp_p, sizeof(ftyp_p), out + pos);

    size_t mpos = 0u;
    uint8_t hdlr_p[16] = {0, 0, 0, 0, 'p', 'i', 'c', 't', 0, 0, 0, 0, 0, 0, 0, 0};
    mpos += wrap_fullbox("hdlr", 0u, 0u, hdlr_p, sizeof(hdlr_p), buf + mpos);

    uint8_t pitm_p[2] = {0u, (with_tmap == 2) ? 2u : 1u};
    mpos += wrap_fullbox("pitm", 0u, 0u, pitm_p, sizeof(pitm_p), buf + mpos);

    /* infe entries inside iinf */
    uint8_t iinf_items[256];
    size_t ipos = 0u;
    uint16_t cnt = 1u;
    /* item 1: av01 */
    uint8_t infe1[9] = {0, 1, 0, 0, 'a', 'v', '0', '1', 0};
    ipos += wrap_fullbox("infe", 2u, 0u, infe1, sizeof(infe1), iinf_items + ipos);
    if (with_tmap) {
        cnt++;
        const char *tt = (with_tmap == 2) ? "sato" : "tmap";
        uint8_t infe2[9] = {0, 2, 0, 0, (uint8_t)tt[0], (uint8_t)tt[1],
                            (uint8_t)tt[2], (uint8_t)tt[3], 0};
        ipos += wrap_fullbox("infe", 2u, 0u, infe2, sizeof(infe2),
                             iinf_items + ipos);
    }
    if (duplicate_exif || exif_iloc_len > 0u) {
        cnt++;
        uint8_t infe_ex[9] = {0, 3, 0, 0, 'E', 'x', 'i', 'f', 0};
        ipos += wrap_fullbox("infe", 2u, 0u, infe_ex, sizeof(infe_ex),
                             iinf_items + ipos);
    }
    if (duplicate_exif) {
        cnt++;
        uint8_t infe_ex2[9] = {0, 4, 0, 0, 'E', 'x', 'i', 'f', 0};
        ipos += wrap_fullbox("infe", 2u, 0u, infe_ex2, sizeof(infe_ex2),
                             iinf_items + ipos);
    }
    uint8_t iinf_p[2 + 256];
    wr16be(iinf_p, cnt);
    memcpy(iinf_p + 2u, iinf_items, ipos);
    mpos += wrap_fullbox("iinf", 0u, 0u, iinf_p, 2u + ipos, buf + mpos);

    if (with_tmap == 1) {
        uint8_t altr_p[16];
        wr32be(altr_p + 0u, 10u); /* group_id */
        wr32be(altr_p + 4u, 2u);  /* num_entities */
        wr32be(altr_p + 8u, 1u);  /* primary item */
        wr32be(altr_p + 12u, 2u); /* tmap item */
        uint8_t grpl_body[64];
        size_t glen = wrap_fullbox("altr", 0u, 0u, altr_p, sizeof(altr_p),
                                   grpl_body);
        mpos += wrap_isobmff_box("grpl", grpl_body, glen, buf + mpos);
    }

    if (duplicate_exif || exif_iloc_len > 0u) {
        uint8_t iref_p[64];
        size_t rpos = 0u;
        uint8_t cdsc1[6] = {0, 3, 0, 1, 0, 1};
        rpos += wrap_isobmff_box("cdsc", cdsc1, sizeof(cdsc1), iref_p + rpos);
        if (duplicate_exif) {
            uint8_t cdsc2[6] = {0, 4, 0, 1, 0, 1};
            rpos +=
                wrap_isobmff_box("cdsc", cdsc2, sizeof(cdsc2), iref_p + rpos);
        }
        mpos += wrap_fullbox("iref", 0u, 0u, iref_p, rpos, buf + mpos);
    }

    if (exif_iloc_len > 0u) {
        /* iloc ver 0: off_size=4, len_size=8, base_offset_size=0, item 3 with 1 extent */
        uint8_t iloc_p[32];
        size_t lpos = 0u;
        iloc_p[lpos++] = 0x48u;
        iloc_p[lpos++] = 0x00u;
        wr16be(iloc_p + lpos, 1u);
        lpos += 2u;
        wr16be(iloc_p + lpos, 3u);
        lpos += 2u;
        wr16be(iloc_p + lpos, 0u);
        lpos += 2u;
        wr16be(iloc_p + lpos, 1u);
        lpos += 2u;
        wr32be(iloc_p + lpos, 0u);
        lpos += 4u;
        wr32be(iloc_p + lpos, (uint32_t)(exif_iloc_len >> 32));
        lpos += 4u;
        wr32be(iloc_p + lpos, (uint32_t)(exif_iloc_len & 0xffffffffu));
        lpos += 4u;
        mpos += wrap_fullbox("iloc", 0u, 0u, iloc_p, lpos, buf + mpos);
    }

    if (big_icc_len > 0u) {
        /* Build iprp -> ipco -> colr('prof' + declared size). */
        uint8_t ipco_p[32];
        uint32_t colr_sz = (uint32_t)(8u + 4u + big_icc_len);
        wr32be(ipco_p, colr_sz);
        memcpy(ipco_p + 4u, "colr", 4u);
        memcpy(ipco_p + 8u, "prof", 4u);
        /* Note: intentionally truncate payload if caller tests colr size overflow,
         * or clamp inside actual payload if needed; to trigger colr meta_cap check cleanly,
         * keep full box valid by allocating when <= 1024 bytes. */
        if (big_icc_len <= 1024u) {
            uint8_t *iprp_tmp = (uint8_t *)calloc(1u, 128u + big_icc_len);
            CHECK(iprp_tmp != NULL);
            uint8_t *colr_b = iprp_tmp + 16u;
            wr32be(colr_b, colr_sz);
            memcpy(colr_b + 4u, "colr", 4u);
            memcpy(colr_b + 8u, "prof", 4u);
            size_t ipco_len = 4u + big_icc_len + 8u;
            wr32be(iprp_tmp + 8u, (uint32_t)(8u + ipco_len));
            memcpy(iprp_tmp + 12u, "ipco", 4u);
            wr32be(iprp_tmp, (uint32_t)(16u + ipco_len));
            memcpy(iprp_tmp + 4u, "iprp", 4u);
            size_t total_iprp = 16u + ipco_len;
            CHECK(pos + 12u + mpos + total_iprp <= out_cap);
            wr32be(out + pos, (uint32_t)(12u + mpos + total_iprp));
            memcpy(out + pos + 4u, "meta", 4u);
            wr32be(out + pos + 8u, 0u);
            memcpy(out + pos + 12u, buf, mpos);
            memcpy(out + pos + 12u + mpos, iprp_tmp, total_iprp);
            free(iprp_tmp);
            return pos + 12u + mpos + total_iprp;
        }
    }

    CHECK(pos + 12u + mpos <= out_cap);
    pos += wrap_fullbox("meta", 0u, 0u, buf, mpos, out + pos);
    return pos;
}

static size_t build_synthetic_opaque_combo_avif(size_t icc_len,
                                                size_t exif_len,
                                                size_t xmp_len,
                                                size_t zzzz_len,
                                                uint32_t ipma_repeat,
                                                uint8_t *out,
                                                size_t out_cap) {
    size_t pos = 0u;
    uint8_t ftyp_p[12] = {'a', 'v', 'i', 'f', 0, 0, 0, 0, 'm', 'i', 'f', '1'};
    pos += wrap_isobmff_box("ftyp", ftyp_p, sizeof(ftyp_p), out + pos);

    uint8_t *meta_buf = (uint8_t *)calloc(1u, out_cap);
    CHECK(meta_buf != NULL);
    size_t mpos = 0u;
    uint8_t hdlr_p[16] = {0, 0, 0, 0, 'p', 'i', 'c', 't', 0, 0, 0, 0, 0, 0, 0, 0};
    mpos += wrap_fullbox("hdlr", 0u, 0u, hdlr_p, sizeof(hdlr_p), meta_buf + mpos);
    uint8_t pitm_p[2] = {0u, 1u};
    mpos += wrap_fullbox("pitm", 0u, 0u, pitm_p, sizeof(pitm_p), meta_buf + mpos);

    /* iinf: item 1 (av01), optional item 2 (Exif), optional item 3 (XMP). */
    {
        uint8_t iinf_items[256];
        size_t ipos = 0u;
        uint16_t cnt = 1u;
        uint8_t infe1[9] = {0, 1, 0, 0, 'a', 'v', '0', '1', 0};
        ipos += wrap_fullbox("infe", 2u, 0u, infe1, sizeof(infe1),
                             iinf_items + ipos);
        if (exif_len > 0u) {
            cnt++;
            uint8_t infe2[9] = {0, 2, 0, 0, 'E', 'x', 'i', 'f', 0};
            ipos += wrap_fullbox("infe", 2u, 0u, infe2, sizeof(infe2),
                                 iinf_items + ipos);
        }
        if (xmp_len > 0u) {
            cnt++;
            uint8_t infe3[29] = {0, 3, 0, 0, 'm', 'i', 'm', 'e', 0,
                                 'a', 'p', 'p', 'l', 'i', 'c', 'a', 't',
                                 'i', 'o', 'n', '/', 'r', 'd', 'f', '+',
                                 'x', 'm', 'l', 0};
            ipos += wrap_fullbox("infe", 2u, 0u, infe3, sizeof(infe3),
                                 iinf_items + ipos);
        }
        uint8_t iinf_p[2 + 256];
        wr16be(iinf_p, cnt);
        memcpy(iinf_p + 2u, iinf_items, ipos);
        mpos += wrap_fullbox("iinf", 0u, 0u, iinf_p, 2u + ipos, meta_buf + mpos);
    }

    if (exif_len > 0u || xmp_len > 0u) {
        uint8_t iref_p[64];
        size_t rpos = 0u;
        if (exif_len > 0u) {
            uint8_t cdsc[6] = {0, 2, 0, 1, 0, 1};
            rpos += wrap_isobmff_box("cdsc", cdsc, sizeof(cdsc), iref_p + rpos);
        }
        if (xmp_len > 0u) {
            uint8_t cdsc[6] = {0, 3, 0, 1, 0, 1};
            rpos += wrap_isobmff_box("cdsc", cdsc, sizeof(cdsc), iref_p + rpos);
        }
        mpos += wrap_fullbox("iref", 0u, 0u, iref_p, rpos, meta_buf + mpos);

        uint8_t iloc_p[64];
        size_t lpos = 0u;
        uint16_t lcnt = (uint16_t)((exif_len > 0u ? 1u : 0u) +
                                   (xmp_len > 0u ? 1u : 0u));
        iloc_p[lpos++] = 0x44u;
        iloc_p[lpos++] = 0x00u;
        wr16be(iloc_p + lpos, lcnt);
        lpos += 2u;
        if (exif_len > 0u) {
            wr16be(iloc_p + lpos, 2u);
            lpos += 2u;
            wr16be(iloc_p + lpos, 0u);
            lpos += 2u;
            wr16be(iloc_p + lpos, 1u);
            lpos += 2u;
            wr32be(iloc_p + lpos, 0u);
            lpos += 4u;
            wr32be(iloc_p + lpos, (uint32_t)(exif_len + 4u));
            lpos += 4u;
        }
        if (xmp_len > 0u) {
            wr16be(iloc_p + lpos, 3u);
            lpos += 2u;
            wr16be(iloc_p + lpos, 0u);
            lpos += 2u;
            wr16be(iloc_p + lpos, 1u);
            lpos += 2u;
            wr32be(iloc_p + lpos, 0u);
            lpos += 4u;
            wr32be(iloc_p + lpos, (uint32_t)xmp_len);
            lpos += 4u;
        }
        mpos += wrap_fullbox("iloc", 0u, 0u, iloc_p, lpos, meta_buf + mpos);
    }

    if (icc_len > 0u || zzzz_len > 0u) {
        size_t ipco_cap = icc_len + zzzz_len + 64u;
        uint8_t *ipco_body = (uint8_t *)calloc(1u, ipco_cap);
        CHECK(ipco_body != NULL);
        size_t icpos = 0u;
        uint8_t zidx = 1u;
        if (icc_len > 0u) {
            uint8_t *colr_p = (uint8_t *)calloc(1u, 4u + icc_len);
            CHECK(colr_p != NULL);
            memcpy(colr_p, "prof", 4u);
            icpos += wrap_isobmff_box("colr", colr_p, 4u + icc_len,
                                      ipco_body + icpos);
            free(colr_p);
            zidx = 2u;
        }
        if (zzzz_len > 0u) {
            uint8_t *zp = (uint8_t *)calloc(1u, zzzz_len);
            CHECK(zp != NULL);
            icpos += wrap_isobmff_box("zzzz", zp, zzzz_len, ipco_body + icpos);
            free(zp);
        }
        size_t ipco_box_sz = 8u + icpos;
        size_t ipma_pay_sz = ipma_repeat ? (4u + (size_t)ipma_repeat * 4u) : 0u;
        size_t ipma_box_sz = ipma_repeat ? (12u + ipma_pay_sz) : 0u;
        CHECK(mpos + 8u + ipco_box_sz + ipma_box_sz < out_cap);
        wr32be(meta_buf + mpos, (uint32_t)(8u + ipco_box_sz + ipma_box_sz));
        memcpy(meta_buf + mpos + 4u, "iprp", 4u);
        mpos += 8u;
        mpos += wrap_isobmff_box("ipco", ipco_body, icpos, meta_buf + mpos);
        free(ipco_body);
        if (ipma_repeat > 0u) {
            uint8_t *ipma_p = (uint8_t *)calloc(1u, ipma_pay_sz);
            CHECK(ipma_p != NULL);
            wr32be(ipma_p, ipma_repeat);
            size_t ap = 4u;
            for (uint32_t ri = 0u; ri < ipma_repeat; ri++) {
                wr16be(ipma_p + ap, (uint16_t)(10u + ri));
                ipma_p[ap + 2u] = 1u;
                ipma_p[ap + 3u] = (uint8_t)(zidx & 0x7fu);
                ap += 4u;
            }
            mpos += wrap_fullbox("ipma", 0u, 0u, ipma_p, ap, meta_buf + mpos);
            free(ipma_p);
        }
    }

    CHECK(pos + 12u + mpos <= out_cap);
    pos += wrap_fullbox("meta", 0u, 0u, meta_buf, mpos, out + pos);
    free(meta_buf);
    return pos;
}

static uint8_t *build_synthetic_fanout_avif(uint32_t item_cnt,
                                            size_t *out_len) {
    size_t cap = 4096u + (size_t)item_cnt * 64u;
    uint8_t *out = (uint8_t *)malloc(cap);
    uint8_t *mbuf = (uint8_t *)malloc(cap);
    CHECK(out != NULL && mbuf != NULL);
    size_t pos = 0u;
    uint8_t ftyp_p[12] = {'a', 'v', 'i', 'f', 0, 0, 0, 0, 'm', 'i', 'f', '1'};
    pos += wrap_isobmff_box("ftyp", ftyp_p, sizeof(ftyp_p), out + pos);

    size_t mpos = 0u;
    uint8_t hdlr_p[16] = {0, 0, 0, 0, 'p', 'i', 'c', 't', 0, 0, 0, 0, 0, 0, 0, 0};
    mpos += wrap_fullbox("hdlr", 0u, 0u, hdlr_p, sizeof(hdlr_p), mbuf + mpos);
    uint8_t pitm_p[2] = {0u, 1u};
    mpos += wrap_fullbox("pitm", 0u, 0u, pitm_p, sizeof(pitm_p), mbuf + mpos);

    /* iinf: item 1 (av01), item_cnt unrelated Exif (ids 10..10+N-1), plus
     * two Exif items attached to primary 1 at the end. */
    uint32_t total_items = item_cnt + 3u;
    size_t iinf_off = mpos;
    mpos += 16u; /* fullbox(12) + entry_count(4) for iinf ver 1 */
    for (uint32_t k = 0u; k < item_cnt; k++) {
        uint8_t infe_p[11];
        wr32be(infe_p, 10u + k);
        wr16be(infe_p + 4u, 0u);
        memcpy(infe_p + 6u, "Exif", 4u);
        infe_p[10] = 0u;
        mpos += wrap_fullbox("infe", 3u, 0u, infe_p, sizeof(infe_p),
                             mbuf + mpos);
    }
    uint8_t infe_p1[11] = {0, 0, 0, 1, 0, 0, 'a', 'v', '0', '1', 0};
    mpos += wrap_fullbox("infe", 3u, 0u, infe_p1, sizeof(infe_p1), mbuf + mpos);
    uint8_t infe_e1[11] = {0, 0, 0, 2, 0, 0, 'E', 'x', 'i', 'f', 0};
    mpos += wrap_fullbox("infe", 3u, 0u, infe_e1, sizeof(infe_e1), mbuf + mpos);
    uint8_t infe_e2[11] = {0, 0, 0, 3, 0, 0, 'E', 'x', 'i', 'f', 0};
    mpos += wrap_fullbox("infe", 3u, 0u, infe_e2, sizeof(infe_e2), mbuf + mpos);
    wr32be(mbuf + iinf_off, (uint32_t)(mpos - iinf_off));
    memcpy(mbuf + iinf_off + 4u, "iinf", 4u);
    wr32be(mbuf + iinf_off + 8u, 0x01000000u); /* ver 1 */
    wr32be(mbuf + iinf_off + 12u, total_items);

    /* iref ver 1: N cdsc refs targeting unrelated item 99999 + 2 targeting 1. */
    size_t iref_off = mpos;
    mpos += 12u;
    for (uint32_t k = 0u; k < item_cnt; k++) {
        uint8_t cdsc[10];
        wr32be(cdsc, 10u + k);
        wr16be(cdsc + 4u, 1u);
        wr32be(cdsc + 6u, 99999u);
        mpos += wrap_isobmff_box("cdsc", cdsc, sizeof(cdsc), mbuf + mpos);
    }
    uint8_t cdsc_d1[10] = {0, 0, 0, 2, 0, 1, 0, 0, 0, 1};
    uint8_t cdsc_d2[10] = {0, 0, 0, 3, 0, 1, 0, 0, 0, 1};
    mpos += wrap_isobmff_box("cdsc", cdsc_d1, sizeof(cdsc_d1), mbuf + mpos);
    mpos += wrap_isobmff_box("cdsc", cdsc_d2, sizeof(cdsc_d2), mbuf + mpos);
    wr32be(mbuf + iref_off, (uint32_t)(mpos - iref_off));
    memcpy(mbuf + iref_off + 4u, "iref", 4u);
    wr32be(mbuf + iref_off + 8u, 0x01000000u); /* ver 1 */

    CHECK(pos + 12u + mpos <= cap);
    pos += wrap_fullbox("meta", 0u, 0u, mbuf, mpos, out + pos);
    free(mbuf);
    *out_len = pos;
    return out;
}

static uint8_t *build_grid_avif_from_frame(const rfc_frame *fr,
                                           size_t *out_len) {
    CHECK(fr != NULL);
    CHECK(fr->layout.width > 64u && fr->layout.width <= 128u);
    CHECK(fr->layout.height > 64u && fr->layout.height <= 128u);
    CHECK(fr->layout.bits_per_sample == 8u);
    int has_alpha = (fr->layout.format == RFC_SAMPLE_RGBA8);
    CHECK(has_alpha || fr->layout.format == RFC_SAMPLE_RGB8);
    uint32_t ch = has_alpha ? 4u : 3u;

    uint32_t tw[2] = {64u, fr->layout.width - 64u};
    uint32_t th[2] = {64u, fr->layout.height - 64u};
    uint32_t ox[2] = {0u, 64u};
    uint32_t oy[2] = {0u, 64u};
    avifImage *cells[4] = {NULL, NULL, NULL, NULL};
    for (uint32_t r = 0u; r < 2u; r++) {
        for (uint32_t c = 0u; c < 2u; c++) {
            uint32_t idx = r * 2u + c;
            avifImage *tile =
                avifImageCreate(tw[c], th[r], 8u, AVIF_PIXEL_FORMAT_YUV444);
            CHECK(tile != NULL);
            tile->yuvRange = AVIF_RANGE_FULL;
            tile->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
            tile->transferCharacteristics =
                AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
            tile->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
            CHECK(avifImageAllocatePlanes(
                      tile, has_alpha ? AVIF_PLANES_ALL : AVIF_PLANES_YUV) ==
                  AVIF_RESULT_OK);
            for (uint32_t y = 0u; y < th[r]; y++) {
                const uint8_t *srow =
                    fr->data + (size_t)(oy[r] + y) * fr->layout.stride_bytes +
                    (size_t)ox[c] * ch;
                uint8_t *dy =
                    tile->yuvPlanes[AVIF_CHAN_Y] +
                    (size_t)y * tile->yuvRowBytes[AVIF_CHAN_Y];
                uint8_t *du =
                    tile->yuvPlanes[AVIF_CHAN_U] +
                    (size_t)y * tile->yuvRowBytes[AVIF_CHAN_U];
                uint8_t *dv =
                    tile->yuvPlanes[AVIF_CHAN_V] +
                    (size_t)y * tile->yuvRowBytes[AVIF_CHAN_V];
                uint8_t *da =
                    has_alpha ? (tile->alphaPlane + (size_t)y * tile->alphaRowBytes)
                              : NULL;
                for (uint32_t x = 0u; x < tw[c]; x++) {
                    const uint8_t *px = srow + (size_t)x * ch;
                    dy[x] = px[1];
                    du[x] = px[2];
                    dv[x] = px[0];
                    if (da) {
                        da[x] = px[3];
                    }
                }
            }
            cells[idx] = tile;
        }
    }

    avifEncoder *enc = avifEncoderCreate();
    CHECK(enc != NULL);
    enc->codecChoice = AVIF_CODEC_CHOICE_AOM;
    enc->speed = 10;
    enc->quality = AVIF_QUALITY_LOSSLESS;
    enc->qualityAlpha = AVIF_QUALITY_LOSSLESS;
    CHECK(avifEncoderAddImageGrid(enc, 2u, 2u, (const avifImage *const *)cells,
                                  AVIF_ADD_IMAGE_FLAG_SINGLE) == AVIF_RESULT_OK);
    avifRWData raw = AVIF_DATA_EMPTY;
    CHECK(avifEncoderFinish(enc, &raw) == AVIF_RESULT_OK);
    avifEncoderDestroy(enc);
    for (uint32_t i = 0u; i < 4u; i++) {
        avifImageDestroy(cells[i]);
    }
    uint8_t *out = (uint8_t *)malloc(raw.size);
    CHECK(out != NULL);
    memcpy(out, raw.data, raw.size);
    *out_len = raw.size;
    avifRWDataFree(&raw);
    return out;
}

/* ------------------------------------------------------------------------ */
/* Suite 1: Registry contract & upstream libavif/libaom checks              */
/* ------------------------------------------------------------------------ */

static void test_registration_and_upstream_contract(void) {
    CHECK(rfc_codecs_unregister_avif() == RFC_OK);
    const rfc_capability *cap = rfc_registry_find(RFC_FORMAT_AVIF);
    CHECK(cap != NULL);
    CHECK(strcmp(cap->name, "avif") == 0);
    CHECK(cap->availability == RFC_AVAILABILITY_USABLE);
    CHECK(cap->can_read == 1 && cap->can_write == 1);
    CHECK(cap->metadata_items ==
          (RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION | RFC_META_ITEM_EXIF |
           RFC_META_ITEM_XMP));
    CHECK(rfc_registry_select(RFC_FORMAT_AVIF, RFC_MODE_READ) ==
          RFC_ERR_NO_ADAPTER);
    CHECK(rfc_registry_select(RFC_FORMAT_AVIF, RFC_MODE_WRITE) ==
          RFC_ERR_NO_ADAPTER);

    CHECK(rfc_codecs_register_avif() == RFC_OK);
    CHECK(rfc_codecs_register_avif() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_select(RFC_FORMAT_AVIF, RFC_MODE_READ) == RFC_OK);
    CHECK(rfc_registry_select(RFC_FORMAT_AVIF, RFC_MODE_WRITE) == RFC_OK);

    const char *reason = NULL;
    rfc_setting_value s_ok[2] = {{"metadata", "preserve"},
                                 {"exactness", "pixel-exact"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_AVIF, s_ok, 2u, &reason) ==
          RFC_OK);
    rfc_setting_value s_bad_key[1] = {{"quality", "90"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_AVIF, s_bad_key, 1u,
                                         &reason) == RFC_ERR_UNKNOWN_SETTING);
    rfc_setting_value s_bad_val[1] = {{"exactness", "coefficient-exact"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_AVIF, s_bad_val, 1u,
                                         &reason) == RFC_ERR_SETTING_VALUE);

    const char *aver = avifVersion();
    const char *aomver = aom_codec_version_str();
    const char *cname = avifCodecName(
        AVIF_CODEC_CHOICE_AOM, AVIF_CODEC_FLAG_CAN_ENCODE | AVIF_CODEC_FLAG_CAN_DECODE);
    CHECK(aver && aver[0] != '\0');
    CHECK(aomver && aomver[0] != '\0');
    CHECK(cname && cname[0] != '\0');

    printf("avif: registration_and_upstream_contract ok (avif=%s aom=%s)\n",
           aver, aomver);
}

/* ------------------------------------------------------------------------ */
/* Suite 2: Lossless 8/10/12-bit, alpha, padding, determinism, import       */
/* ------------------------------------------------------------------------ */

static void test_lossless_depths_alpha_padding_and_import(void) {
    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_AVIF);
    CHECK(adp != NULL);

    typedef struct depth_case {
        uint32_t w, h, bps;
        rfc_sample_format fmt;
        rfc_alpha_mode alpha;
        int opaque_alpha;
    } depth_case;

    const depth_case cases[] = {
        {1u, 1u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 0},
        {3u, 5u, 8u, RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 0},
        {17u, 9u, 8u, RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 1}, /* all-opaque RGBA8 */
        {17u, 11u, 10u, RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 0},
        {15u, 13u, 10u, RFC_SAMPLE_RGBA16, RFC_ALPHA_STRAIGHT, 1}, /* all-opaque RGBA16 */
        {19u, 7u, 12u, RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 0},
        {12u, 10u, 12u, RFC_SAMPLE_RGBA16, RFC_ALPHA_STRAIGHT, 0},
    };

    for (size_t ci = 0u; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        const depth_case *tc = &cases[ci];
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);

        rfc_metadata meta;
        memset(&meta, 0, sizeof(meta));
        meta.orientation = RFC_ORIENTATION_NORMAL;
        rfc_frame fr;
        memset(&fr, 0, sizeof(fr));
        fill_pattern_frame(&fr, tc->w, tc->h, tc->fmt, tc->alpha, tc->bps,
                           tc->opaque_alpha, &meta, &alloc);

        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(es, &exec, &fr, 1u, &meta, NULL, 0u, &enc) ==
              RFC_OK);
        adp->close_session(es);
        CHECK(enc.len > 12u);

        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_image_probe pr;
        memset(&pr, 0, sizeof(pr));
        CHECK(adp->probe_session(ds, &exec, enc.data, enc.len, &pr) == RFC_OK);
        CHECK(pr.first_frame.width == tc->w && pr.first_frame.height == tc->h);
        CHECK(pr.first_frame.bits_per_sample == tc->bps);
        CHECK(pr.first_frame.format == tc->fmt);
        CHECK(pr.first_frame.alpha == tc->alpha);
        CHECK(pr.frame_count == 1u);
        rfc_metadata_free(&pr.container_meta, &alloc);

        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(ds, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(ds);
        CHECK(dc == 1u && df != NULL);
        CHECK(df[0].layout.format == tc->fmt);
        CHECK(df[0].layout.alpha == tc->alpha);
        CHECK(df[0].layout.bits_per_sample == tc->bps);
        rfc_sample_mismatch mm;
        memset(&mm, 0, sizeof(mm));
        CHECK(rfc_verify_frames_equal(&fr, 1u, df, dc, &mm) == RFC_OK);

        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&fr);
        CHECK(st.live == 0u);
    }

    /* Reject 16-bit, low padding nonzero bits, gray, float, and premultiplied alpha. */
    {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);

        rfc_layout l16 = {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 4u, 4u, 24u, 16u};
        rfc_frame f16 = {0};
        CHECK(rfc_frame_alloc(&f16, &l16, NULL, &alloc) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(es, &exec, &f16, 1u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_UNSUPPORTED_SAMPLE);
        rfc_frame_free(&f16);

        rfc_layout l10 = {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 4u, 4u, 24u, 10u};
        rfc_frame f10 = {0};
        CHECK(rfc_frame_alloc(&f10, &l10, NULL, &alloc) == RFC_OK);
        uint16_t bad_px = 0x0001u;
        memcpy(f10.data, &bad_px, 2u);
        CHECK(adp->encode_session(es, &exec, &f10, 1u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_UNSUPPORTED_SAMPLE);
        rfc_frame_free(&f10);

        rfc_layout lg8 = {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 4u, 4u, 4u, 8u};
        rfc_frame fg8 = {0};
        CHECK(rfc_frame_alloc(&fg8, &lg8, NULL, &alloc) == RFC_OK);
        CHECK(adp->encode_session(es, &exec, &fg8, 1u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_UNSUPPORTED_SAMPLE);
        rfc_frame_free(&fg8);

        rfc_layout lf = {RFC_SAMPLE_RGB_FLOAT, RFC_ALPHA_NONE, 4u, 4u, 48u, 32u};
        rfc_frame ff = {0};
        CHECK(rfc_frame_alloc(&ff, &lf, NULL, &alloc) == RFC_OK);
        CHECK(adp->encode_session(es, &exec, &ff, 1u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_UNSUPPORTED_SAMPLE);
        rfc_frame_free(&ff);

        rfc_layout lp = {RFC_SAMPLE_RGBA8, RFC_ALPHA_PREMULTIPLIED, 4u, 4u, 16u,
                         8u};
        rfc_frame fp = {0};
        CHECK(rfc_frame_alloc(&fp, &lp, NULL, &alloc) == RFC_OK);
        CHECK(adp->encode_session(es, &exec, &fp, 1u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_UNSUPPORTED_ALPHA);
        rfc_frame_free(&fp);

        adp->close_session(es);
        CHECK(st.live == 0u);
    }

    /* Multi-thread deterministic encoding and decoding parity (t=1 vs t=4). */
    {
        rfc_exec_context e1, e4;
        tracked_alloc_state s1, s4;
        rfc_allocator a1, a4;
        init_exec(&e1, &s1, &a1, NULL, 1u);
        init_exec(&e4, &s4, &a4, NULL, 4u);

        rfc_frame fr = {0};
        fill_pattern_frame(&fr, 64u, 48u, RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT,
                           8u, 0, NULL, &a1);
        void *es1 = NULL, *es4 = NULL;
        CHECK(adp->open_session(&e1, &es1) == RFC_OK);
        CHECK(adp->open_session(&e4, &es4) == RFC_OK);
        rfc_encoded enc1 = {0}, enc4 = {0};
        CHECK(adp->encode_session(es1, &e1, &fr, 1u, NULL, NULL, 0u, &enc1) ==
              RFC_OK);
        CHECK(adp->encode_session(es4, &e4, &fr, 1u, NULL, NULL, 0u, &enc4) ==
              RFC_OK);
        CHECK(enc1.len == enc4.len &&
              memcmp(enc1.data, enc4.data, enc1.len) == 0);

        rfc_frame *df1 = NULL, *df4 = NULL;
        size_t dc1 = 0u, dc4 = 0u;
        rfc_metadata dm1 = {0}, dm4 = {0};
        CHECK(adp->decode_session(es1, &e1, enc1.data, enc1.len, NULL, 0u, &df1,
                                  &dc1, &dm1) == RFC_OK);
        CHECK(adp->decode_session(es4, &e4, enc4.data, enc4.len, NULL, 0u, &df4,
                                  &dc4, &dm4) == RFC_OK);
        rfc_sample_mismatch mm;
        memset(&mm, 0, sizeof(mm));
        CHECK(rfc_verify_frames_equal(df1, dc1, df4, dc4, &mm) == RFC_OK);

        adp->close_session(es1);
        adp->close_session(es4);
        rfc_metadata_free(&dm1, &a1);
        rfc_metadata_free(&dm4, &a4);
        rfc_frame_free(&df1[0]);
        a1.free(a1.ctx, df1);
        rfc_frame_free(&df4[0]);
        a4.free(a4.ctx, df4);
        rfc_encoded_free(&enc1);
        rfc_encoded_free(&enc4);
        rfc_frame_free(&fr);
        CHECK(s1.live == 0u && s4.live == 0u);
    }

    /* Non-native YUV420 / YUV422 / YUV400 monochrome imports + full/limited range. */
    {
        typedef struct yuv_case {
            avifPixelFormat fmt;
            avifRange range;
            uint32_t depth;
            int alpha;
            rfc_sample_format want_sample;
        } yuv_case;
        const yuv_case ycases[] = {
            {AVIF_PIXEL_FORMAT_YUV420, AVIF_RANGE_FULL, 8u, 0, RFC_SAMPLE_RGB8},
            {AVIF_PIXEL_FORMAT_YUV420, AVIF_RANGE_LIMITED, 10u, 1, RFC_SAMPLE_RGBA16},
            {AVIF_PIXEL_FORMAT_YUV422, AVIF_RANGE_FULL, 8u, 0, RFC_SAMPLE_RGB8},
            {AVIF_PIXEL_FORMAT_YUV422, AVIF_RANGE_FULL, 12u, 0, RFC_SAMPLE_RGB16},
            {AVIF_PIXEL_FORMAT_YUV400, AVIF_RANGE_FULL, 8u, 0, RFC_SAMPLE_RGB8},
            {AVIF_PIXEL_FORMAT_YUV400, AVIF_RANGE_LIMITED, 10u, 1, RFC_SAMPLE_RGBA16},
        };
        for (size_t i = 0u; i < sizeof(ycases) / sizeof(ycases[0]); i++) {
            const yuv_case *yc = &ycases[i];
            size_t blen = 0u;
            uint8_t *blob = synthesize_avif(
                24u, 18u, yc->depth, yc->fmt, yc->range,
                AVIF_MATRIX_COEFFICIENTS_BT709, yc->alpha, 0, NULL, 0u, 1,
                AVIF_COLOR_PRIMARIES_BT709, AVIF_TRANSFER_CHARACTERISTICS_SRGB,
                &blen);
            rfc_exec_context exec;
            tracked_alloc_state st;
            rfc_allocator alloc;
            init_exec(&exec, &st, &alloc, NULL, 1u);
            void *ds = NULL;
            CHECK(adp->open_session(&exec, &ds) == RFC_OK);
            rfc_frame *df = NULL;
            size_t dc = 0u;
            rfc_metadata dm = {0};
            CHECK(adp->decode_session(ds, &exec, blob, blen, NULL, 0u, &df, &dc,
                                      &dm) == RFC_OK);
            adp->close_session(ds);
            CHECK(dc == 1u);
            CHECK(df[0].layout.format == yc->want_sample);
            CHECK(df[0].layout.bits_per_sample == yc->depth);
            if (yc->depth > 8u) {
                uint16_t low_mask = (uint16_t)((1u << (16u - yc->depth)) - 1u);
                const uint16_t *wp = (const uint16_t *)df[0].data;
                CHECK((wp[0] & low_mask) == 0u);
            }
            if (yc->fmt == AVIF_PIXEL_FORMAT_YUV400 && yc->depth == 8u) {
                CHECK(df[0].data[0] == df[0].data[1] &&
                      df[0].data[1] == df[0].data[2]);
            }
            rfc_metadata_free(&dm, &alloc);
            rfc_frame_free(&df[0]);
            alloc.free(alloc.ctx, df);
            free(blob);
            CHECK(st.live == 0u);
        }
    }

    /* Multi-tile 2x2 edge-cropped RGBA8 grid (117x113 -> 64x64, 53x64, 64x49, 53x49)
     * plus scheduler AVIF(grid) -> PNG decode. */
    {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);

        rfc_frame ref_fr = {0};
        fill_pattern_frame(&ref_fr, 117u, 113u, RFC_SAMPLE_RGBA8,
                           RFC_ALPHA_STRAIGHT, 8u, 0, NULL, &alloc);
        size_t glen = 0u;
        uint8_t *gblob = build_grid_avif_from_frame(&ref_fr, &glen);

        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_image_probe pr;
        memset(&pr, 0, sizeof(pr));
        CHECK(adp->probe_session(ds, &exec, gblob, glen, &pr) == RFC_OK);
        CHECK(pr.first_frame.width == 117u && pr.first_frame.height == 113u);
        CHECK(pr.first_frame.format == RFC_SAMPLE_RGBA8);
        CHECK(pr.first_frame.alpha == RFC_ALPHA_STRAIGHT);
        CHECK(pr.first_frame.bits_per_sample == 8u);
        CHECK(pr.frame_count == 1u);
        rfc_metadata_free(&pr.container_meta, &alloc);

        /* Tight 4 MiB budget refuses grid decode; 32 MiB budget succeeds cleanly. */
        rfc_budget *tb = rfc_budget_create(4ull * 1024ull * 1024ull);
        CHECK(tb != NULL);
        rfc_allocator ba;
        rfc_budget_allocator(tb, &ba);
        rfc_exec_context bexec = exec;
        bexec.budget = tb;
        bexec.allocator = &ba;
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(ds, &bexec, gblob, glen, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_destroy(tb);

        rfc_budget *okb = rfc_budget_create(32ull * 1024ull * 1024ull);
        CHECK(okb != NULL);
        rfc_budget_allocator(okb, &ba);
        bexec.budget = okb;
        bexec.allocator = &ba;
        CHECK(adp->decode_session(ds, &bexec, gblob, glen, NULL, 0u, &df, &dc,
                                  &dm) == RFC_OK);
        adp->close_session(ds);
        CHECK(dc == 1u && df != NULL);
        rfc_sample_mismatch mm;
        memset(&mm, 0, sizeof(mm));
        CHECK(rfc_verify_frames_equal(&ref_fr, 1u, df, dc, &mm) == RFC_OK);
        rfc_metadata_free(&dm, &ba);
        rfc_frame_free(&df[0]);
        ba.free(ba.ctx, df);
        rfc_budget_stats bstats;
        rfc_budget_stats_get(okb, &bstats);
        CHECK(bstats.current_allocated_bytes == 0u);
        rfc_budget_destroy(okb);

        CHECK(rfc_codecs_register_basic() == RFC_OK);
        rfc_scheduler_config scfg;
        rfc_scheduler_config_default(&scfg);
        rfc_scheduler *sched = rfc_scheduler_create(&scfg);
        CHECK(sched != NULL);
        char p_png[512];
        make_temp_path(p_png, sizeof(p_png), "grid.png");
        remove(p_png);
        rfc_io_sink png_sink;
        rfc_io_sink_init_path(&png_sink, p_png, RFC_EXPORT_REFUSE_EXISTING);
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = RFC_FORMAT_AVIF;
        sp.target_format = RFC_FORMAT_PNG;
        sp.source_bytes = gblob;
        sp.source_size = glen;
        sp.io_sink = &png_sink;
        sp.source_layout = &ref_fr.layout;
        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK &&
              snap.state == RFC_JOB_SUCCEEDED);
        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        remove(p_png);
        CHECK(rfc_codecs_unregister_basic() == RFC_OK);

        free(gblob);
        rfc_frame_free(&ref_fr);
        CHECK(st.live == 0u);
    }

    rfc_registry_release_session(RFC_FORMAT_AVIF);
    printf("avif: lossless_depths_alpha_padding_and_import ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 3: Metadata, colourimetry, orientation, ICC precedence, strip      */
/* ------------------------------------------------------------------------ */

static void test_metadata_colorimetry_and_orientation(void) {
    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_AVIF);
    CHECK(adp != NULL);

    uint8_t icc[128];
    build_sample_icc(icc);
    const char xmp[] = "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rf>ok</rf></x:xmpmeta>";

    /* 1. Native orientation 1..8 via irot/imir without EXIF. */
    for (int oi = 1; oi <= 8; oi++) {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);

        rfc_metadata meta = {NULL, 0u, (rfc_orientation)oi, 0u, {0},
                             NULL, 0u, NULL, 0u};
        rfc_frame fr = {0};
        fill_pattern_frame(&fr, 11u, 7u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           0, &meta, &alloc);
        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(es, &exec, &fr, 1u, &meta, NULL, 0u, &enc) ==
              RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(es, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(es);
        CHECK(dc == 1u && dm.exif_len == 0u);
        CHECK(dm.orientation == (rfc_orientation)oi);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&fr);
        CHECK(st.live == 0u);
    }

    /* 2. ICC + XMP + raw TIFF EXIF vs Exif\0\0-prefixed EXIF + orientation sync & conflict. */
    {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);

        uint8_t raw_exif[26];
        build_le_exif_with_ori(6u, raw_exif);
        uint8_t pref_exif[32];
        memcpy(pref_exif, "Exif\0\0", 6u);
        memcpy(pref_exif + 6u, raw_exif, 26u);

        rfc_metadata meta = {icc, sizeof(icc), RFC_ORIENTATION_ROTATE_90, 0u,
                             {0}, pref_exif, sizeof(pref_exif),
                             (const uint8_t *)xmp, strlen(xmp)};
        rfc_frame fr = {0};
        fill_pattern_frame(&fr, 12u, 9u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           0, &meta, &alloc);
        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(es, &exec, &fr, 1u, &meta, NULL, 0u, &enc) ==
              RFC_OK);

        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(es, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        CHECK(dm.icc_len == sizeof(icc) && memcmp(dm.icc, icc, sizeof(icc)) == 0);
        CHECK(dm.xmp_len == strlen(xmp) && memcmp(dm.xmp, xmp, dm.xmp_len) == 0);
        CHECK(dm.exif_len == sizeof(pref_exif) &&
              memcmp(dm.exif, pref_exif, sizeof(pref_exif)) == 0);
        CHECK(dm.orientation == RFC_ORIENTATION_ROTATE_90);

        /* Raw 26-byte TIFF EXIF also round-trips cleanly. */
        {
            rfc_metadata m26 = {icc, sizeof(icc), RFC_ORIENTATION_ROTATE_90, 0u,
                                {0}, raw_exif, sizeof(raw_exif), NULL, 0u};
            rfc_frame f26 = {0};
            fill_pattern_frame(&f26, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE,
                               8u, 0, &m26, &alloc);
            rfc_encoded e26 = {0};
            CHECK(adp->encode_session(es, &exec, &f26, 1u, &m26, NULL, 0u,
                                      &e26) == RFC_OK);
            rfc_frame *df26 = NULL;
            size_t dc26 = 0u;
            rfc_metadata dm26 = {0};
            CHECK(adp->decode_session(es, &exec, e26.data, e26.len, NULL, 0u,
                                      &df26, &dc26, &dm26) == RFC_OK);
            CHECK(dm26.exif_len == sizeof(raw_exif) &&
                  memcmp(dm26.exif, raw_exif, sizeof(raw_exif)) == 0);
            rfc_metadata_free(&dm26, &alloc);
            rfc_frame_free(&df26[0]);
            alloc.free(alloc.ctx, df26);
            rfc_encoded_free(&e26);
            rfc_frame_free(&f26);
        }

        /* Conflicting meta.orientation != exif orientation -> RFC_ERR_INVALID_ARGUMENT. */
        fr.metadata.orientation = RFC_ORIENTATION_ROTATE_180;
        rfc_encoded enc_bad = {0};
        CHECK(adp->encode_session(es, &exec, &fr, 1u, &fr.metadata, NULL, 0u,
                                  &enc_bad) == RFC_ERR_INVALID_ARGUMENT);

        /* metadata=strip drops icc, exif, xmp, and resets orientation to NORMAL. */
        fr.metadata.orientation = RFC_ORIENTATION_ROTATE_90;
        rfc_setting_value strip_sv[1] = {{"metadata", "strip"}};
        rfc_encoded enc_strip = {0};
        CHECK(adp->encode_session(es, &exec, &fr, 1u, &fr.metadata, strip_sv,
                                  1u, &enc_strip) == RFC_OK);
        rfc_frame *dfs = NULL;
        size_t dcs = 0u;
        rfc_metadata dms = {0};
        CHECK(adp->decode_session(es, &exec, enc_strip.data, enc_strip.len,
                                  NULL, 0u, &dfs, &dcs, &dms) == RFC_OK);
        CHECK(dms.icc_len == 0u && dms.exif_len == 0u && dms.xmp_len == 0u);
        CHECK(dms.orientation == RFC_ORIENTATION_NORMAL);
        CHECK(!rfc_colorimetry_has_any(&dms.color));

        /* Raw container check on stripped stream: no irot/imir/clli/mdcv boxes. */
        CHECK(find_isobmff_box(enc_strip.data, enc_strip.len, "irot", NULL) ==
              NULL);
        CHECK(find_isobmff_box(enc_strip.data, enc_strip.len, "imir", NULL) ==
              NULL);
        CHECK(find_isobmff_box(enc_strip.data, enc_strip.len, "mdcv", NULL) ==
              NULL);
        CHECK(find_isobmff_box(enc_strip.data, enc_strip.len, "clli", NULL) ==
              NULL);

        adp->close_session(es);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_metadata_free(&dms, &alloc);
        rfc_frame_free(&dfs[0]);
        alloc.free(alloc.ctx, dfs);
        rfc_encoded_free(&enc);
        rfc_encoded_free(&enc_strip);
        rfc_frame_free(&fr);
        CHECK(st.live == 0u);
    }

    /* 3. Non-ICC cICP + whole-nit cLLI + mDCV (R,G,B,W <-> ISOBMFF G,B,R,W) + ICC precedence. */
    {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);

        rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                             NULL, 0u, NULL, 0u};
        meta.color.has_cicp = 1u;
        meta.color.cicp[0] = 9u;
        meta.color.cicp[1] = 16u;
        meta.color.cicp[2] = 0u;
        meta.color.cicp[3] = 1u;
        meta.color.has_clli = 1u;
        meta.color.clli_max_cll = 10000000u; /* 1000 nits */
        meta.color.clli_max_fall = 4000000u; /* 400 nits */
        meta.color.has_mdcv = 1u;
        meta.color.mdcv_xy[0] = 35400u; /* Rx */
        meta.color.mdcv_xy[1] = 14600u; /* Ry */
        meta.color.mdcv_xy[2] = 8500u;  /* Gx */
        meta.color.mdcv_xy[3] = 39850u; /* Gy */
        meta.color.mdcv_xy[4] = 6550u;  /* Bx */
        meta.color.mdcv_xy[5] = 2300u;  /* By */
        meta.color.mdcv_xy[6] = 15635u; /* Wx */
        meta.color.mdcv_xy[7] = 16450u; /* Wy */
        meta.color.mdcv_max_dl = 10000000u;
        meta.color.mdcv_min_dl = 50u;

        rfc_frame fr = {0};
        fill_pattern_frame(&fr, 10u, 10u, RFC_SAMPLE_RGB16, RFC_ALPHA_NONE,
                           10u, 0, &meta, &alloc);
        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(es, &exec, &fr, 1u, &meta, NULL, 0u, &enc) ==
              RFC_OK);

        /* Verify raw ISOBMFF mdcv box bytes store G, B, R, W order! */
        size_t mdcv_len = 0u;
        const uint8_t *mdcv_p =
            find_isobmff_box(enc.data, enc.len, "mdcv", &mdcv_len);
        CHECK(mdcv_p != NULL && mdcv_len == 24u);
        CHECK(rd16be(mdcv_p + 0u) == 8500u && rd16be(mdcv_p + 2u) == 39850u);
        CHECK(rd16be(mdcv_p + 4u) == 6550u && rd16be(mdcv_p + 6u) == 2300u);
        CHECK(rd16be(mdcv_p + 8u) == 35400u && rd16be(mdcv_p + 10u) == 14600u);
        CHECK(rd16be(mdcv_p + 12u) == 15635u && rd16be(mdcv_p + 14u) == 16450u);
        CHECK(rd32be(mdcv_p + 16u) == 10000000u && rd32be(mdcv_p + 20u) == 50u);

        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(es, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        CHECK(dm.color.has_cicp && memcmp(dm.color.cicp, meta.color.cicp, 4u) == 0);
        CHECK(dm.color.has_clli &&
              dm.color.clli_max_cll == 10000000u &&
              dm.color.clli_max_fall == 4000000u);
        CHECK(dm.color.has_mdcv &&
              memcmp(dm.color.mdcv_xy, meta.color.mdcv_xy,
                     sizeof(meta.color.mdcv_xy)) == 0 &&
              dm.color.mdcv_max_dl == 10000000u &&
              dm.color.mdcv_min_dl == 50u);

        /* Verify unsupported colorimetry refusals upfront in standalone encode. */
        rfc_encoded bad_e = {0};
        rfc_frame fbad = {0};
        rfc_metadata m_gama = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                               NULL, 0u, NULL, 0u};
        m_gama.color.has_gama = 1u;
        m_gama.color.gama_scaled = 45455u;
        fill_pattern_frame(&fbad, 4u, 4u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           0, &m_gama, &alloc);
        CHECK(adp->encode_session(es, &exec, &fbad, 1u, &m_gama, NULL, 0u,
                                  &bad_e) == RFC_ERR_METADATA_POLICY);
        rfc_frame_free(&fbad);

        /* External AVIF with BOTH ICC and nclx cICP: decode applies ICC precedence. */
        size_t dual_len = 0u;
        uint8_t *dual = synthesize_avif(
            8u, 8u, 8u, AVIF_PIXEL_FORMAT_YUV420, AVIF_RANGE_FULL,
            AVIF_MATRIX_COEFFICIENTS_BT709, 0, 0, icc, sizeof(icc), 1,
            AVIF_COLOR_PRIMARIES_BT2020, AVIF_TRANSFER_CHARACTERISTICS_PQ,
            &dual_len);
        rfc_frame *dd = NULL;
        size_t ddc = 0u;
        rfc_metadata ddm = {0};
        CHECK(adp->decode_session(es, &exec, dual, dual_len, NULL, 0u, &dd,
                                  &ddc, &ddm) == RFC_OK);
        CHECK(ddm.icc_len == sizeof(icc) && !ddm.color.has_cicp);

        adp->close_session(es);
        rfc_metadata_free(&ddm, &alloc);
        rfc_frame_free(&dd[0]);
        alloc.free(alloc.ctx, dd);
        free(dual);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&fr);
        CHECK(st.live == 0u);
    }

    /* 4. EXIF without orientation tag + explicit meta.orientation. */
    {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);

        uint8_t ex_no_ori[26];
        build_le_exif_no_ori(ex_no_ori);
        rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_ROTATE_180, 0u, {0},
                             ex_no_ori, sizeof(ex_no_ori), NULL, 0u};
        rfc_frame fr = {0};
        fill_pattern_frame(&fr, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           0, &meta, &alloc);
        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(es, &exec, &fr, 1u, &meta, NULL, 0u, &enc) ==
              RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(es, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(es);
        CHECK(dm.exif_len == 26u && memcmp(dm.exif, ex_no_ori, 26u) == 0);
        CHECK(dm.orientation == RFC_ORIENTATION_ROTATE_180);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&fr);
        CHECK(st.live == 0u);
    }

    rfc_registry_release_session(RFC_FORMAT_AVIF);
    printf("avif: metadata_colorimetry_and_orientation ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 4: Same-format exactness=byte-identical & cross-format pipelines   */
/* ------------------------------------------------------------------------ */

static void test_byte_identical_and_cross_format(void) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_OK);

    /* Synthesize valid YUV420 AVIF source bytes to confirm byte-identical
     * copies exact input bytes without re-encoding to YUV444. */
    uint8_t icc[128];
    build_sample_icc(icc);
    size_t src_len = 0u;
    uint8_t *src_avif = synthesize_avif(
        20u, 16u, 8u, AVIF_PIXEL_FORMAT_YUV420, AVIF_RANGE_FULL,
        AVIF_MATRIX_COEFFICIENTS_BT709, 0, 0, icc, sizeof(icc), 0, 2, 2,
        &src_len);

    char p_src[512], p_copy[512], p_bad[512], p_bad_out[512], p_fold[512],
        p_jxl[512], p_back[512];
    make_temp_path(p_src, sizeof(p_src), "src.avif");
    make_temp_path(p_copy, sizeof(p_copy), "copy.avif");
    make_temp_path(p_bad, sizeof(p_bad), "bad.avif");
    make_temp_path(p_bad_out, sizeof(p_bad_out), "bad_out.avif");
    make_temp_path(p_fold, sizeof(p_fold), "mid.fold");
    make_temp_path(p_jxl, sizeof(p_jxl), "mid.jxl");
    make_temp_path(p_back, sizeof(p_back), "back.avif");
    remove(p_src);
    remove(p_copy);
    remove(p_bad);
    remove(p_bad_out);
    remove(p_fold);
    remove(p_jxl);
    remove(p_back);

    CHECK(write_file_all(p_src, src_avif, src_len));
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    /* 1. AVIF -> AVIF with exactness=byte-identical, metadata=preserve. */
    rfc_io_source src_io;
    rfc_io_sink dst_io;
    rfc_io_source_init_path(&src_io, p_src, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, p_copy, RFC_EXPORT_REFUSE_EXISTING);
    rfc_setting_value sv_bi[2] = {{"exactness", "byte-identical"},
                                  {"metadata", "preserve"}};
    rfc_job_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_format = RFC_FORMAT_AVIF;
    sp.target_format = RFC_FORMAT_AVIF;
    sp.io_source = &src_io;
    sp.io_sink = &dst_io;
    sp.settings = sv_bi;
    sp.setting_count = 2u;
    rfc_job *job = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    rfc_job_snapshot snap;
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED);
    rfc_verify_report vrep;
    CHECK(rfc_job_verify_report_get(job, &vrep) == RFC_OK);
    CHECK(vrep.exactness == RFC_EXACT_BYTE_IDENTICAL &&
          memcmp(vrep.source_hash, vrep.verified_hash, 32u) == 0);
    rfc_job_release(job);

    size_t copy_len = 0u;
    uint8_t *copy_bytes = read_file_all(p_copy, &copy_len);
    CHECK(copy_bytes != NULL && copy_len == src_len &&
          memcmp(copy_bytes, src_avif, src_len) == 0);
    free(copy_bytes);

    /* 2. Corrupt AVIF under exactness=byte-identical must fail Stage 1 import. */
    uint8_t *corrupt = (uint8_t *)malloc(src_len);
    CHECK(corrupt != NULL);
    memcpy(corrupt, src_avif, src_len);
    corrupt[0] = 'X';
    CHECK(write_file_all(p_bad, corrupt, src_len));
    free(corrupt);
    rfc_io_source_init_path(&src_io, p_bad, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, p_bad_out, RFC_EXPORT_REFUSE_EXISTING);
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_FAILED && snap.failing_stage == RFC_STAGE_IMPORT);
    rfc_job_release(job);
    FILE *chk_bad = fopen(p_bad_out, "rb");
    CHECK(chk_bad == NULL);

    /* 3. Cross-format AVIF -> JXL with byte-identical refused upfront. */
    sp.source_format = RFC_FORMAT_AVIF;
    sp.target_format = RFC_FORMAT_JPEG_XL;
    rfc_io_source_init_path(&src_io, p_src, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, p_bad_out, RFC_EXPORT_REFUSE_EXISTING);
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(job == NULL);

    /* 4. Cross-format pipeline: AVIF -> .fold -> JXL -> AVIF (metadata stripped for .fold). */
    rfc_setting_value sv_strip[1] = {{"metadata", "strip"}};
    rfc_io_source_init_path(&src_io, p_src, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, p_fold, RFC_EXPORT_REFUSE_EXISTING);
    memset(&sp, 0, sizeof(sp));
    sp.source_format = RFC_FORMAT_AVIF;
    sp.target_format = RFC_FORMAT_RASTERFOLD;
    sp.io_source = &src_io;
    sp.io_sink = &dst_io;
    sp.settings = sv_strip;
    sp.setting_count = 1u;
    sp.expected_working_set_bytes = 64u * 1024u * 1024u;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK &&
          snap.state == RFC_JOB_SUCCEEDED);
    rfc_job_release(job);

    rfc_io_source_init_path(&src_io, p_fold, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, p_jxl, RFC_EXPORT_REFUSE_EXISTING);
    sp.source_format = RFC_FORMAT_RASTERFOLD;
    sp.target_format = RFC_FORMAT_JPEG_XL;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK &&
          snap.state == RFC_JOB_SUCCEEDED);
    rfc_job_release(job);

    rfc_io_source_init_path(&src_io, p_jxl, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, p_back, RFC_EXPORT_REFUSE_EXISTING);
    sp.source_format = RFC_FORMAT_JPEG_XL;
    sp.target_format = RFC_FORMAT_AVIF;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK &&
          snap.state == RFC_JOB_SUCCEEDED);
    rfc_job_release(job);

    rfc_scheduler_destroy(sched);
    free(src_avif);
    remove(p_src);
    remove(p_copy);
    remove(p_bad);
    remove(p_bad_out);
    remove(p_fold);
    remove(p_jxl);
    remove(p_back);

    rfc_codecs_unregister_jxl();
    rfc_codecs_unregister_basic();
    printf("avif: byte_identical_and_cross_format ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 5: Hostile ISOBMFF preflight, limits, budget, cancel, retry        */
/* ------------------------------------------------------------------------ */

static void test_refusals_preflight_limits_and_cancel(void) {
    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_AVIF);
    CHECK(adp != NULL);

    /* 1. Animated avis major brand refused upfront. */
    {
        uint8_t avis[20] = {0, 0, 0, 20, 'f', 't', 'y', 'p',
                            'a', 'v', 'i', 's', 0, 0, 0, 0, 'a', 'v', 'i', 'f'};
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_image_probe pr;
        CHECK(adp->probe_session(ds, &exec, avis, sizeof(avis), &pr) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(ds, &exec, avis, sizeof(avis), NULL, 0u, &df,
                                  &dc, &dm) == RFC_ERR_UNSUPPORTED_COMBINATION);
        adp->close_session(ds);
        CHECK(st.calls == 0u && st.live == 0u);
    }

    /* 2. Multi-frame animated sequence built with libavif -> refused. */
    {
        avifImage *f1 = avifImageCreate(8u, 8u, 8u, AVIF_PIXEL_FORMAT_YUV444);
        avifImage *f2 = avifImageCreate(8u, 8u, 8u, AVIF_PIXEL_FORMAT_YUV444);
        CHECK(f1 && f2);
        CHECK(avifImageAllocatePlanes(f1, AVIF_PLANES_YUV) == AVIF_RESULT_OK);
        CHECK(avifImageAllocatePlanes(f2, AVIF_PLANES_YUV) == AVIF_RESULT_OK);
        memset(f1->yuvPlanes[AVIF_CHAN_Y], 64, (size_t)f1->yuvRowBytes[AVIF_CHAN_Y] * 8u);
        memset(f2->yuvPlanes[AVIF_CHAN_Y], 128, (size_t)f2->yuvRowBytes[AVIF_CHAN_Y] * 8u);
        avifEncoder *enc = avifEncoderCreate();
        CHECK(enc != NULL);
        enc->codecChoice = AVIF_CODEC_CHOICE_AOM;
        enc->speed = 10;
        CHECK(avifEncoderAddImage(enc, f1, 1u, AVIF_ADD_IMAGE_FLAG_NONE) ==
              AVIF_RESULT_OK);
        CHECK(avifEncoderAddImage(enc, f2, 1u, AVIF_ADD_IMAGE_FLAG_NONE) ==
              AVIF_RESULT_OK);
        avifRWData raw = AVIF_DATA_EMPTY;
        CHECK(avifEncoderFinish(enc, &raw) == AVIF_RESULT_OK);
        avifEncoderDestroy(enc);
        avifImageDestroy(f1);
        avifImageDestroy(f2);

        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(ds, &exec, raw.data, raw.size, NULL, 0u, &df,
                                  &dc, &dm) == RFC_ERR_UNSUPPORTED_COMBINATION);
        adp->close_session(ds);
        avifRWDataFree(&raw);
        CHECK(st.live == 0u);
    }

    /* 3. Gain-map (tmap), duplicate Exif items, oversized iloc/colr(prof) preflight refusals. */
    {
        uint8_t syn[2048];
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        rfc_limits tight;
        rfc_limits_default(&tight);
        tight.max_metadata_bytes = 128u;
        init_exec(&exec, &st, &alloc, &tight, 1u);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);

        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};

        size_t n_tmap = build_synthetic_meta_avif(0, 1, 0u, 0u, syn, sizeof(syn));
        CHECK(adp->decode_session(ds, &exec, syn, n_tmap, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_UNSUPPORTED_COMBINATION);

        size_t n_sato = build_synthetic_meta_avif(0, 2, 0u, 0u, syn, sizeof(syn));
        CHECK(adp->decode_session(ds, &exec, syn, n_sato, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_UNSUPPORTED_COMBINATION);

        size_t n_dup = build_synthetic_meta_avif(1, 0, 0u, 0u, syn, sizeof(syn));
        CHECK(adp->decode_session(ds, &exec, syn, n_dup, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_UNSUPPORTED_COMBINATION);

        size_t n_iloc =
            build_synthetic_meta_avif(0, 0, 512u, 0u, syn, sizeof(syn));
        CHECK(adp->decode_session(ds, &exec, syn, n_iloc, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_LIMIT_EXCEEDED);

        size_t n_colr =
            build_synthetic_meta_avif(0, 0, 0u, 256u, syn, sizeof(syn));
        CHECK(adp->decode_session(ds, &exec, syn, n_colr, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_LIMIT_EXCEEDED);

        adp->close_session(ds);
        CHECK(st.calls == 0u && st.live == 0u);
    }

    /* 3b. Opaque properties: huge unknown ipco, ipma amplification, and budget reservation. */
    {
        size_t cap = 400u * 1024u;
        uint8_t *syn_big = (uint8_t *)calloc(1u, cap);
        CHECK(syn_big != NULL);
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};

        /* Unassociated 300 KiB 'zzzz' property exceeds 256 KiB preflight cap. */
        size_t n_hostile =
            build_synthetic_opaque_combo_avif(0u, 0u, 0u, 300u * 1024u, 0u,
                                              syn_big, cap);
        CHECK(adp->decode_session(ds, &exec, syn_big, n_hostile, NULL, 0u, &df,
                                  &dc, &dm) == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(st.calls == 0u && st.live == 0u);

        /* 8 KiB 'zzzz' associated 40 times in ipma (328 KiB total > 256 KiB). */
        size_t n_amp = build_synthetic_opaque_combo_avif(0u, 0u, 0u, 8192u, 40u,
                                                         syn_big, cap);
        CHECK(adp->decode_session(ds, &exec, syn_big, n_amp, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(st.calls == 0u && st.live == 0u);

        /* 16 KiB 'zzzz' billed into parse reservation rejects before libavif. */
        rfc_budget *bp =
            rfc_budget_create(15ull * 1024ull * 1024ull + 40000ull);
        CHECK(bp != NULL);
        rfc_allocator ba;
        rfc_budget_allocator(bp, &ba);
        rfc_exec_context bex = exec;
        bex.budget = bp;
        bex.allocator = &ba;
        size_t n_plain =
            build_synthetic_opaque_combo_avif(0u, 0u, 0u, 0u, 0u, syn_big, cap);
        CHECK(adp->decode_session(ds, &bex, syn_big, n_plain, NULL, 0u, &df,
                                  &dc, &dm) == RFC_ERR_INVALID_ARGUMENT);
        size_t n_op16k =
            build_synthetic_opaque_combo_avif(0u, 0u, 0u, 16384u, 0u, syn_big,
                                              cap);
        CHECK(adp->decode_session(ds, &bex, syn_big, n_op16k, NULL, 0u, &df,
                                  &dc, &dm) == RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_stats bs;
        rfc_budget_stats_get(bp, &bs);
        CHECK(bs.current_allocated_bytes == 0u);
        rfc_budget_destroy(bp);

        adp->close_session(ds);
        free(syn_big);
        CHECK(st.calls == 0u && st.live == 0u);
    }

    /* 3c. max_metadata_bytes == 0 exact enforcement & combined ICC + XMP cap. */
    {
        uint8_t syn[4096];
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_metadata_bytes = 0u;
        init_exec(&exec, &st, &alloc, &lim, 1u);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};

        size_t n_icc32 = build_synthetic_opaque_combo_avif(32u, 0u, 0u, 0u, 0u,
                                                           syn, sizeof(syn));
        CHECK(adp->decode_session(ds, &exec, syn, n_icc32, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_LIMIT_EXCEEDED);
        size_t n_ex32 = build_synthetic_opaque_combo_avif(0u, 32u, 0u, 0u, 0u,
                                                          syn, sizeof(syn));
        CHECK(adp->decode_session(ds, &exec, syn, n_ex32, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_LIMIT_EXCEEDED);
        adp->close_session(ds);

        lim.max_metadata_bytes = 128u;
        init_exec(&exec, &st, &alloc, &lim, 1u);
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        size_t n_combo = build_synthetic_opaque_combo_avif(80u, 0u, 80u, 0u, 0u,
                                                           syn, sizeof(syn));
        CHECK(adp->decode_session(ds, &exec, syn, n_combo, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_LIMIT_EXCEEDED);
        adp->close_session(ds);
        CHECK(st.calls == 0u && st.live == 0u);
    }

    /* 3d. High-fanout 25,000-item ISOBMFF preflight linear pass & cancellation. */
    {
        size_t flen = 0u;
        uint8_t *fan = build_synthetic_fanout_avif(25000u, &flen);
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_image_probe pr;
        memset(&pr, 0, sizeof(pr));
        CHECK(adp->probe_session(ds, &exec, fan, flen, &pr) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(st.live == 0u);

        adp->cancel_session(ds);
        CHECK(adp->probe_session(ds, &exec, fan, flen, &pr) ==
              RFC_ERR_CANCELLED);
        adp->close_session(ds);
        free(fan);
        CHECK(st.live == 0u);
    }

    /* 4. Tight budget rejection + cooperative cancel + retry isolation. */
    {
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        rfc_frame fr = {0};
        fill_pattern_frame(&fr, 16u, 16u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           0, NULL, &alloc);

        /* Tight budget (4 MiB < min enc work) must refuse cleanly. */
        rfc_budget *tb = rfc_budget_create(4ull * 1024ull * 1024ull);
        CHECK(tb != NULL);
        exec.budget = tb;
        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(es, &exec, &fr, 1u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_OUT_OF_MEMORY);

        /* Cooperative cancel before encode returns RFC_ERR_CANCELLED, then
         * a new session on unrestricted budget succeeds (retry isolation). */
        exec.budget = NULL;
        adp->cancel_session(es);
        CHECK(adp->encode_session(es, &exec, &fr, 1u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_CANCELLED);
        adp->close_session(es);
        rfc_budget_destroy(tb);

        void *es2 = NULL;
        CHECK(adp->open_session(&exec, &es2) == RFC_OK);
        CHECK(adp->encode_session(es2, &exec, &fr, 1u, NULL, NULL, 0u, &enc) ==
              RFC_OK);

        /* Decompression ratio / width limits on decode. */
        rfc_limits lim_w;
        rfc_limits_default(&lim_w);
        lim_w.max_width = 8u;
        rfc_exec_context ew = exec;
        ew.limits = lim_w;
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(es2, &ew, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_ERR_LIMIT_EXCEEDED);
        adp->close_session(es2);
        rfc_encoded_free(&enc);
        rfc_frame_free(&fr);
        CHECK(st.live == 0u);
    }

    rfc_registry_release_session(RFC_FORMAT_AVIF);
    printf("avif: refusals_preflight_limits_and_cancel ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 7: CLI harness for tests/converter/test_avif.py & LD_PRELOAD HWM   */
/* ------------------------------------------------------------------------ */

static int parse_rfwe(const char *path,
                      rfc_layout *out_lay,
                      rfc_metadata *out_meta,
                      uint8_t **out_raw,
                      uint8_t **out_pix,
                      size_t *out_pay_len) {
    size_t flen = 0u;
    uint8_t *raw = read_file_all(path, &flen);
    if (!raw || flen < 48u || memcmp(raw, "RFWE", 4u) != 0) {
        free(raw);
        return 0;
    }
    memset(out_lay, 0, sizeof(*out_lay));
    out_lay->width = rd32le(raw + 4u);
    out_lay->height = rd32le(raw + 8u);
    out_lay->format = (rfc_sample_format)rd32le(raw + 12u);
    out_lay->alpha = (rfc_alpha_mode)rd32le(raw + 16u);
    out_lay->bits_per_sample = rd32le(raw + 20u);
    out_lay->stride_bytes = (size_t)rd32le(raw + 24u);
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = (rfc_orientation)rd32le(raw + 28u);
    size_t icc_len = rd32le(raw + 32u);
    size_t exif_len = rd32le(raw + 36u);
    size_t xmp_len = rd32le(raw + 40u);
    size_t pay_len = rd32le(raw + 44u);
    if (48u + icc_len + exif_len + xmp_len + pay_len != flen) {
        free(raw);
        return 0;
    }
    size_t off = 48u;
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

static int parse_fmt_cli(const char *s, rfc_format_id *out) {
    if (strcmp(s, "avif") == 0) {
        *out = RFC_FORMAT_AVIF;
        return 1;
    }
    if (strcmp(s, "jxl") == 0 || strcmp(s, "jpeg-xl") == 0) {
        *out = RFC_FORMAT_JPEG_XL;
        return 1;
    }
    if (strcmp(s, "png") == 0) {
        *out = RFC_FORMAT_PNG;
        return 1;
    }
    if (strcmp(s, "bmp") == 0) {
        *out = RFC_FORMAT_BMP;
        return 1;
    }
    if (strcmp(s, "webp") == 0) {
        *out = RFC_FORMAT_WEBP;
        return 1;
    }
    if (strcmp(s, "rasterfold") == 0 || strcmp(s, "fold") == 0) {
        *out = RFC_FORMAT_RASTERFOLD;
        return 1;
    }
    return 0;
}

static int cli_main(int argc, char **argv) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_webp() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_OK);
    CHECK(rfc_codecs_register_avif() == RFC_OK);

    int rc = 2;
    const char *sub = argv[2];

    if (strcmp(sub, "encode") == 0 && (argc >= 5 && argc <= 7)) {
        rfc_layout lay;
        rfc_metadata meta;
        uint8_t *raw = NULL, *pix = NULL;
        size_t pay_len = 0u;
        CHECK(parse_rfwe(argv[3], &lay, &meta, &raw, &pix, &pay_len));
        const char *policy = (argc >= 6) ? argv[5] : "preserve";
        uint32_t thr = (argc == 7) ? (uint32_t)strtoul(argv[6], NULL, 10) : 1u;

        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, thr ? thr : 1u);
        rfc_frame fr = {0};
        CHECK(rfc_frame_alloc(&fr, &lay, &meta, &alloc) == RFC_OK);
        memcpy(fr.data, pix, pay_len);

        rfc_setting_value sv[1] = {{"metadata", policy}};
        const rfc_session_adapter *adp =
            rfc_registry_acquire_session(RFC_FORMAT_AVIF);
        CHECK(adp != NULL);
        void *es = NULL;
        CHECK(adp->open_session(&exec, &es) == RFC_OK);
        rfc_encoded enc = {0};
        rfc_status r =
            adp->encode_session(es, &exec, &fr, 1u, &meta, sv, 1u, &enc);
        adp->close_session(es);
        rfc_registry_release_session(RFC_FORMAT_AVIF);
        if (r == RFC_OK) {
            CHECK(write_file_all(argv[4], enc.data, enc.len));
        }
        size_t elen = enc.len;
        rfc_encoded_free(&enc);
        rfc_frame_free(&fr);
        free(raw);
        CHECK(st.live == 0u);
        printf("STATUS=%s OUT_BYTES=%zu\n", rfc_status_name(r), elen);
        rc = (r == RFC_OK) ? 0 : 2;
    } else if (strcmp(sub, "encode-grid") == 0 && argc == 5) {
        rfc_layout lay;
        rfc_metadata meta;
        uint8_t *raw = NULL, *pix = NULL;
        size_t pay_len = 0u;
        CHECK(parse_rfwe(argv[3], &lay, &meta, &raw, &pix, &pay_len));
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        rfc_frame fr = {0};
        CHECK(rfc_frame_alloc(&fr, &lay, &meta, &alloc) == RFC_OK);
        memcpy(fr.data, pix, pay_len);
        size_t glen = 0u;
        uint8_t *gblob = build_grid_avif_from_frame(&fr, &glen);
        CHECK(write_file_all(argv[4], gblob, glen));
        free(gblob);
        rfc_frame_free(&fr);
        free(raw);
        CHECK(st.live == 0u);
        printf("STATUS=ok OUT_BYTES=%zu\n", glen);
        rc = 0;
    } else if (strcmp(sub, "decode") == 0 && (argc == 5 || argc == 6)) {
        size_t alen = 0u;
        uint8_t *ab = read_file_all(argv[3], &alen);
        if (!ab) {
            printf("STATUS=io-error\n");
            goto cleanup;
        }
        uint32_t thr = (argc == 6) ? (uint32_t)strtoul(argv[5], NULL, 10) : 1u;
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, thr ? thr : 1u);
        const rfc_session_adapter *adp =
            rfc_registry_acquire_session(RFC_FORMAT_AVIF);
        CHECK(adp != NULL);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_frame *frames = NULL;
        size_t fc = 0u;
        rfc_metadata dm = {0};
        rfc_status r =
            adp->decode_session(ds, &exec, ab, alen, NULL, 0u, &frames, &fc,
                                &dm);
        adp->close_session(ds);
        rfc_registry_release_session(RFC_FORMAT_AVIF);
        free(ab);
        if (r != RFC_OK) {
            printf("STATUS=%s LIVE=%zu\n", rfc_status_name(r), st.live);
            CHECK(st.live == 0u);
            goto cleanup;
        }
        uint32_t ch =
            (frames[0].layout.format == RFC_SAMPLE_RGBA8 ||
             frames[0].layout.format == RFC_SAMPLE_RGBA16)
                ? 4u
                : 3u;
        uint32_t S = (frames[0].layout.bits_per_sample <= 8u) ? 1u : 2u;
        size_t row_b = (size_t)frames[0].layout.width * ch * S;
        size_t pay_len = row_b * frames[0].layout.height;
        uint8_t hdr[48];
        memcpy(hdr, "RFWE", 4u);
        wr32le(hdr + 4u, frames[0].layout.width);
        wr32le(hdr + 8u, frames[0].layout.height);
        wr32le(hdr + 12u, (uint32_t)frames[0].layout.format);
        wr32le(hdr + 16u, (uint32_t)frames[0].layout.alpha);
        wr32le(hdr + 20u, frames[0].layout.bits_per_sample);
        wr32le(hdr + 24u, (uint32_t)row_b);
        wr32le(hdr + 28u, (uint32_t)dm.orientation);
        wr32le(hdr + 32u, (uint32_t)dm.icc_len);
        wr32le(hdr + 36u, (uint32_t)dm.exif_len);
        wr32le(hdr + 40u, (uint32_t)dm.xmp_len);
        wr32le(hdr + 44u, (uint32_t)pay_len);
        FILE *fp = fopen_utf8(argv[4], "wb");
        CHECK(fp != NULL);
        CHECK(fwrite(hdr, 1u, sizeof(hdr), fp) == sizeof(hdr));
        if (dm.icc_len > 0u) CHECK(fwrite(dm.icc, 1u, dm.icc_len, fp) == dm.icc_len);
        if (dm.exif_len > 0u) CHECK(fwrite(dm.exif, 1u, dm.exif_len, fp) == dm.exif_len);
        if (dm.xmp_len > 0u) CHECK(fwrite(dm.xmp, 1u, dm.xmp_len, fp) == dm.xmp_len);
        for (uint32_t y = 0u; y < frames[0].layout.height; y++) {
            CHECK(fwrite(frames[0].data +
                             (size_t)y * frames[0].layout.stride_bytes,
                         1u, row_b, fp) == row_b);
        }
        fclose(fp);
        printf("STATUS=ok WIDTH=%u HEIGHT=%u SAMPLE=%u ALPHA=%u BPS=%u "
               "ORIENTATION=%u ICC_LEN=%zu EXIF_LEN=%zu XMP_LEN=%zu "
               "HAS_CICP=%u HAS_CLLI=%u HAS_MDCV=%u\n",
               frames[0].layout.width, frames[0].layout.height,
               (uint32_t)frames[0].layout.format,
               (uint32_t)frames[0].layout.alpha,
               frames[0].layout.bits_per_sample, (uint32_t)dm.orientation,
               dm.icc_len, dm.exif_len, dm.xmp_len,
               (unsigned)dm.color.has_cicp, (unsigned)dm.color.has_clli,
               (unsigned)dm.color.has_mdcv);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&frames[0]);
        alloc.free(alloc.ctx, frames);
        CHECK(st.live == 0u);
        rc = 0;
    } else if (strcmp(sub, "probe") == 0 && argc == 4) {
        size_t alen = 0u;
        uint8_t *ab = read_file_all(argv[3], &alen);
        if (!ab) {
            printf("STATUS=io-error\n");
            goto cleanup;
        }
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        const rfc_session_adapter *adp =
            rfc_registry_acquire_session(RFC_FORMAT_AVIF);
        CHECK(adp != NULL);
        void *ps = NULL;
        CHECK(adp->open_session(&exec, &ps) == RFC_OK);
        rfc_image_probe pr;
        memset(&pr, 0, sizeof(pr));
        rfc_status r = adp->probe_session(ps, &exec, ab, alen, &pr);
        adp->close_session(ps);
        rfc_registry_release_session(RFC_FORMAT_AVIF);
        free(ab);
        rfc_metadata_free(&pr.container_meta, &alloc);
        CHECK(st.live == 0u);
        printf("STATUS=%s WIDTH=%u HEIGHT=%u SAMPLE=%u ALPHA=%u BPS=%u FRAMES=%u\n",
               rfc_status_name(r), pr.first_frame.width, pr.first_frame.height,
               (uint32_t)pr.first_frame.format, (uint32_t)pr.first_frame.alpha,
               pr.first_frame.bits_per_sample, pr.frame_count);
        rc = (r == RFC_OK) ? 0 : 2;
    } else if (strcmp(sub, "convert") == 0 && (argc == 9 || argc == 10 || argc == 11)) {
        rfc_format_id src_f = 0, dst_f = 0;
        CHECK(parse_fmt_cli(argv[3], &src_f));
        CHECK(parse_fmt_cli(argv[4], &dst_f));
        uint32_t ack = (argc >= 10) ? (uint32_t)strtoul(argv[9], NULL, 0) : 0u;
        uint32_t thr = (argc == 11) ? (uint32_t)strtoul(argv[10], NULL, 10) : 1u;
        rfc_scheduler_config scfg;
        rfc_scheduler_config_default(&scfg);
        rfc_scheduler *sched = rfc_scheduler_create(&scfg);
        CHECK(sched != NULL);
        rfc_io_source sio;
        rfc_io_sink dio;
        rfc_io_source_init_path(&sio, argv[5], RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&dio, argv[6], RFC_EXPORT_REFUSE_EXISTING);
        rfc_setting_value sv[2] = {{"metadata", argv[7]},
                                   {"exactness", argv[8]}};
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = src_f;
        sp.target_format = dst_f;
        sp.io_source = &sio;
        sp.io_sink = &dio;
        sp.settings = sv;
        sp.setting_count = 2u;
        sp.acknowledged_unsupported_metadata = ack;
        sp.requested_codec_threads = thr;
        if (strcmp(argv[8], "byte-identical") != 0) {
            sp.expected_working_set_bytes = 64u * 1024u * 1024u;
        }
        rfc_job *j = NULL;
        rfc_status sst = rfc_scheduler_submit(sched, NULL, &sp, &j);
        if (sst != RFC_OK) {
            printf("STATUS=%s STAGE=preflight\n", rfc_status_name(sst));
            rfc_scheduler_destroy(sched);
            goto cleanup;
        }
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        printf("STATUS=%s STAGE=%u\n", rfc_status_name(snap.status),
               (unsigned)snap.failing_stage);
        rfc_job_release(j);
        rfc_scheduler_destroy(sched);
        rc = (snap.state == RFC_JOB_SUCCEEDED) ? 0 : 2;
    } else if (strcmp(sub, "budget-peak") == 0 && argc == 7) {
        const char *op = argv[3];
        uint32_t threads = (uint32_t)strtoul(argv[6], NULL, 10);
        if (threads == 0u) {
            threads = 1u;
        }
        rfc_budget *b = rfc_budget_create(512ull * 1024ull * 1024ull);
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
            rfc_registry_acquire_session(RFC_FORMAT_AVIF);
        CHECK(adp != NULL);
        void *sess = NULL;
        CHECK(adp->open_session(&b_exec, &sess) == RFC_OK);
        if (strcmp(op, "encode") == 0) {
            rfc_layout lay;
            rfc_metadata meta;
            uint8_t *raw = NULL, *pix = NULL;
            size_t pay_len = 0u;
            CHECK(parse_rfwe(argv[4], &lay, &meta, &raw, &pix, &pay_len));
            rfc_exec_context fr_ex;
            tracked_alloc_state fr_st;
            rfc_allocator fr_alloc;
            init_exec(&fr_ex, &fr_st, &fr_alloc, NULL, 1u);
            rfc_frame fr = {0};
            CHECK(rfc_frame_alloc(&fr, &lay, &meta, &fr_alloc) == RFC_OK);
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
            rfc_registry_release_session(RFC_FORMAT_AVIF);
            rfc_budget_stats bs;
            rfc_budget_stats_get(b, &bs);
            printf("STATUS=%s PEAK_BUDGET=%llu CURR_ALLOC=%llu OUT_BYTES=%zu\n",
                   rfc_status_name(r),
                   (unsigned long long)bs.peak_allocated_bytes,
                   (unsigned long long)bs.current_allocated_bytes, elen);
            rfc_budget_destroy(b);
            rc = (r == RFC_OK) ? 0 : 2;
        } else if (strcmp(op, "decode") == 0) {
            size_t alen = 0u;
            uint8_t *ab = read_file_all(argv[4], &alen);
            CHECK(ab != NULL);
            rfc_frame *df = NULL;
            size_t dc = 0u;
            rfc_metadata dm = {0};
            rfc_status r = adp->decode_session(sess, &b_exec, ab, alen, NULL,
                                               0u, &df, &dc, &dm);
            rfc_metadata_free(&dm, &ba);
            if (df) {
                rfc_frame_free(&df[0]);
                ba.free(ba.ctx, df);
            }
            free(ab);
            adp->close_session(sess);
            rfc_registry_release_session(RFC_FORMAT_AVIF);
            rfc_budget_stats bs;
            rfc_budget_stats_get(b, &bs);
            printf("STATUS=%s PEAK_BUDGET=%llu CURR_ALLOC=%llu OUT_BYTES=%zu\n",
                   rfc_status_name(r),
                   (unsigned long long)bs.peak_allocated_bytes,
                   (unsigned long long)bs.current_allocated_bytes, dc);
            rfc_budget_destroy(b);
            rc = (r == RFC_OK) ? 0 : 2;
        } else {
            adp->close_session(sess);
            rfc_registry_release_session(RFC_FORMAT_AVIF);
            rfc_budget_destroy(b);
        }
    }

cleanup:
    rfc_codecs_unregister_avif();
    rfc_codecs_unregister_jxl();
    rfc_codecs_unregister_webp();
    rfc_codecs_unregister_basic();
    return rc;
}

#ifdef _WIN32
static int win32_get_utf8_argv(int *out_argc, char ***out_argv) {
    const wchar_t *p = GetCommandLineW();
    if (!p) {
        return 0;
    }
    size_t max_chars = wcslen(p) + 1u;
    wchar_t *tmp = (wchar_t *)malloc(max_chars * sizeof(wchar_t));
    if (!tmp) {
        return 0;
    }
    int cap = 16;
    int argc = 0;
    char **argv = (char **)calloc((size_t)cap, sizeof(char *));
    if (!argv) {
        free(tmp);
        return 0;
    }
    int first = 1;
    while (*p != L'\0') {
        while (*p == L' ' || *p == L'\t') {
            p++;
        }
        if (*p == L'\0') {
            break;
        }
        size_t ti = 0u;
        int in_q = 0;
        if (first) {
            first = 0;
            while (*p != L'\0' && (in_q || (*p != L' ' && *p != L'\t'))) {
                if (*p == L'"') {
                    in_q = !in_q;
                } else {
                    tmp[ti++] = *p;
                }
                p++;
            }
        } else {
            while (*p != L'\0') {
                size_t bs = 0u;
                while (*p == L'\\') {
                    bs++;
                    p++;
                }
                if (*p == L'"') {
                    for (size_t k = 0u; k < bs / 2u; k++) {
                        tmp[ti++] = L'\\';
                    }
                    if ((bs & 1u) != 0u) {
                        tmp[ti++] = L'"';
                    } else if (in_q && p[1] == L'"') {
                        tmp[ti++] = L'"';
                        p++;
                    } else {
                        in_q = !in_q;
                    }
                    p++;
                } else {
                    for (size_t k = 0u; k < bs; k++) {
                        tmp[ti++] = L'\\';
                    }
                    if (*p == L'\0' || (!in_q && (*p == L' ' || *p == L'\t'))) {
                        break;
                    }
                    tmp[ti++] = *p++;
                }
            }
        }
        tmp[ti] = L'\0';
        int u8_len = WideCharToMultiByte(CP_UTF8, 0, tmp, (int)(ti + 1u), NULL,
                                         0, NULL, NULL);
        if (u8_len <= 0) {
            goto fail;
        }
        char *u8 = (char *)malloc((size_t)u8_len);
        if (!u8) {
            goto fail;
        }
        WideCharToMultiByte(CP_UTF8, 0, tmp, (int)(ti + 1u), u8, u8_len, NULL,
                            NULL);
        if (argc + 1 >= cap) {
            cap *= 2;
            char **na = (char **)realloc(argv, (size_t)cap * sizeof(char *));
            if (!na) {
                free(u8);
                goto fail;
            }
            argv = na;
        }
        argv[argc++] = u8;
        argv[argc] = NULL;
    }
    free(tmp);
    *out_argc = argc;
    *out_argv = argv;
    return 1;

fail:
    free(tmp);
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
    return 0;
}

static void win32_free_utf8_argv(int argc, char **argv) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    int u8_argc = 0;
    char **u8_argv = NULL;
    if (win32_get_utf8_argv(&u8_argc, &u8_argv) && u8_argc >= 3 &&
        strcmp(u8_argv[1], "cli") == 0) {
        int rc = cli_main(u8_argc, u8_argv);
        win32_free_utf8_argv(u8_argc, u8_argv);
        return rc;
    }
    win32_free_utf8_argv(u8_argc, u8_argv);
    (void)argc;
    (void)argv;
#else
    if (argc >= 3 && strcmp(argv[1], "cli") == 0) {
        return cli_main(argc, argv);
    }
#endif

    test_registration_and_upstream_contract();
    test_lossless_depths_alpha_padding_and_import();
    test_metadata_colorimetry_and_orientation();
    test_byte_identical_and_cross_format();
    test_refusals_preflight_limits_and_cancel();

    CHECK(rfc_registry_acquire_session(RFC_FORMAT_AVIF) != NULL);
    CHECK(rfc_codecs_unregister_avif() == RFC_ERR_BUSY);
    rfc_registry_release_session(RFC_FORMAT_AVIF);
    CHECK(rfc_codecs_unregister_avif() == RFC_OK);
    CHECK(rfc_codecs_unregister_avif() == RFC_OK);
    printf("avif: all checks passed (%d checks)\n", g_checks);
    return 0;
}
