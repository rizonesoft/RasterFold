/* C11 codec & format adapter test suite + CLI harness for D10 T02 s3:
 *   RFC_FORMAT_JPEG_XL (libjxl 0.13.0 lossless 1..16-bit left-aligned pixel
 *   encode/decode, XYB lossy decode, ICC/Exif/XMP/"brob" boxes, native
 *   orientation, JPEG -> JPEG XL original-byte-reconstruction, limits/budget,
 *   cancellation, retry isolation, and zero-alloc preflight refusals). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <unistd.h>
#endif

#ifndef JXL_STATIC_DEFINE
#define JXL_STATIC_DEFINE
#endif
#ifndef JXL_THREADS_STATIC_DEFINE
#define JXL_THREADS_STATIC_DEFINE
#endif
#ifndef JXL_CMS_STATIC_DEFINE
#define JXL_CMS_STATIC_DEFINE
#endif

#include "rasterfold.h"
#include "rfc_codec.h"
#include "rfc_codecs.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_report.h"
#include "rfc_verify.h"
#include "rf_shared_io.h"

#include <jxl/cms.h>
#include <jxl/decode.h>
#include <jxl/encode.h>

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

/* ------------------------------------------------------------------------ */
/* Portable helpers: endian, tracked allocator, files, frames, upstream JXL */
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

static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static uint32_t png_crc32(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0u; i < len; i++) {
        c ^= (uint32_t)data[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

typedef struct tracked_alloc_state {
    size_t live;
    size_t calls;
    size_t cancel_at_call;
    const rfc_jpeg_reconstruct_adapter *cancel_jrec;
    void *cancel_sess;
    int unreg_was_busy;
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
    st->calls++;
    if (st->cancel_at_call > 0u && st->calls == st->cancel_at_call &&
        st->cancel_jrec != NULL) {
        if (rfc_registry_unregister_jpeg_reconstruct() == RFC_ERR_BUSY) {
            st->unreg_was_busy = 1;
        }
        st->cancel_jrec->cancel_session(st->cancel_sess);
    }
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

#define OOM_MAX_PTRS 4096u

typedef struct oom_alloc_ctx {
    void *ptrs[OOM_MAX_PTRS];
    size_t sizes[OOM_MAX_PTRS];
    size_t live;
    size_t calls;
    size_t fail_at_call;
    size_t invalid_frees;
    int overflow;
} oom_alloc_ctx;

static void *oom_alloc(void *ctx, size_t sz) {
    oom_alloc_ctx *o = (oom_alloc_ctx *)ctx;
    o->calls++;
    if (o->fail_at_call > 0u && o->calls == o->fail_at_call) {
        return NULL;
    }
    void *p = malloc(sz ? sz : 1u);
    if (!p) {
        return NULL;
    }
    int stored = 0;
    for (size_t i = 0u; i < OOM_MAX_PTRS; i++) {
        if (o->ptrs[i] == NULL) {
            o->ptrs[i] = p;
            o->sizes[i] = sz;
            stored = 1;
            break;
        }
    }
    if (!stored) {
        o->overflow = 1;
        free(p);
        return NULL;
    }
    o->live += sz;
    return p;
}

static void oom_free(void *ctx, void *ptr) {
    if (!ptr) {
        return;
    }
    oom_alloc_ctx *o = (oom_alloc_ctx *)ctx;
    for (size_t i = 0u; i < OOM_MAX_PTRS; i++) {
        if (o->ptrs[i] == ptr) {
            o->ptrs[i] = NULL;
            o->live -= o->sizes[i];
            o->sizes[i] = 0u;
            free(ptr);
            return;
        }
    }
    o->invalid_frees++;
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

static rfc_status test_alloc_frame(rfc_frame *f,
                                   const rfc_layout *lay,
                                   const rfc_metadata *meta,
                                   const rfc_allocator *al) {
    return rfc_frame_alloc(f, lay, meta, al);
}

static int write_file_all(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen(path, "wb");
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
    snprintf(buf, cap, "%srfc_jxl_%lu_%u_%s", td,
             (unsigned long)GetCurrentProcessId(), counter, tag);
#else
    snprintf(buf, cap, "/tmp/rfc_jxl_%ld_%u_%s", (long)getpid(), counter, tag);
#endif
}

static void fill_pattern_frame(rfc_frame *fr,
                               uint32_t w,
                               uint32_t h,
                               rfc_sample_format fmt,
                               rfc_alpha_mode alpha,
                               uint32_t bps,
                               const rfc_metadata *meta,
                               const rfc_allocator *al) {
    uint32_t S = (fmt == RFC_SAMPLE_GRAY8 || fmt == RFC_SAMPLE_RGB8 ||
                  fmt == RFC_SAMPLE_RGBA8)
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
    lay.stride_bytes = (size_t)w * (size_t)ch * (size_t)(S / 8u);
    lay.bits_per_sample = bps;
    CHECK(test_alloc_frame(fr, &lay, meta, al) == RFC_OK);

    uint32_t max_nat = (bps == 16u) ? 65535u : ((1u << bps) - 1u);
    uint32_t shift = S - bps;
    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *row = fr->data + (size_t)y * lay.stride_bytes;
        for (uint32_t x = 0u; x < w; x++) {
            for (uint32_t c = 0u; c < ch; c++) {
                uint32_t v =
                    ((x * 37u + y * 53u + c * 97u) ^ (x * y + c * 13u)) %
                    (max_nat + 1u);
                if (x == 0u && y == 0u) {
                    v = max_nat; /* guarantee native max sample present */
                } else if (x == 1u && y == 0u && ch == 4u && c == 3u) {
                    v = 0u; /* invisible alpha pixel with arbitrary RGB */
                }
                if (S == 8u) {
                    row[(size_t)x * ch + c] = (uint8_t)(v << shift);
                } else {
                    uint16_t aligned = (uint16_t)(v << shift);
                    wr16le(row + ((size_t)x * ch + c) * 2u, aligned);
                }
            }
        }
    }
}

/* Build a valid EXIF TIFF blob (endian: 1 LE "II*\0", 0 BE "MM\0*"). */
static void make_exif_blob(int little,
                           uint16_t orientation,
                           uint8_t out_tiff[26]) {
    if (little) {
        CHECK(rfc_exif_build_orientation_tiff26((rfc_orientation)orientation,
                                                out_tiff) == RFC_OK);
    } else {
        memset(out_tiff, 0, 26u);
        out_tiff[0] = 'M';
        out_tiff[1] = 'M';
        out_tiff[2] = 0x00u;
        out_tiff[3] = 0x2Au;
        wr32be(out_tiff + 4u, 8u);
        wr16be(out_tiff + 8u, 1u);
        wr16be(out_tiff + 10u, 0x0112u);
        wr16be(out_tiff + 12u, 3u);
        wr32be(out_tiff + 14u, 1u);
        wr16be(out_tiff + 18u, orientation);
    }
}

/* Valid LE TIFF containing Software tag (0x0131) and no Orientation (0x0112). */
static void make_exif_without_0112(uint8_t out_tiff[34]) {
    memset(out_tiff, 0, 34u);
    out_tiff[0] = 'I';
    out_tiff[1] = 'I';
    out_tiff[2] = 0x2Au;
    out_tiff[3] = 0x00u;
    wr32le(out_tiff + 4u, 8u);
    wr16le(out_tiff + 8u, 1u);
    wr16le(out_tiff + 10u, 0x0131u); /* Software ASCII */
    wr16le(out_tiff + 12u, 2u);
    wr32le(out_tiff + 14u, 8u);
    wr32le(out_tiff + 18u, 26u);
    memcpy(out_tiff + 26u, "RF-JXL!\0", 8u);
}

static size_t build_png_with_gama(uint8_t *out, size_t cap) {
    /* Minimal valid 2x2 RGB8 PNG with gAMA=45455 (non-ICC PNG colourimetry). */
    static const uint8_t k_png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    static const uint8_t ihdr[13] = {0, 0, 0, 2, 0, 0, 0, 2, 8, 2, 0, 0, 0};
    static const uint8_t gama[4] = {0, 0, 0xB1u, 0x8Fu};
    /* Valid uncompressed zlib stream covering 2 scanlines of 1+6=7 bytes (14 bytes). */
    static const uint8_t idat[25] = {
        0x78u, 0x01u, 0x01u, 0x0Eu, 0x00u, 0xF1u, 0xFFu,
        0x00u, 10u, 20u, 30u, 40u, 50u, 60u,
        0x00u, 70u, 80u, 90u, 100u, 110u, 120u,
        0x1Bu, 0xA8u, 0x02u, 0xADu
    };
    /* Compute exact Adler-32 for idat cleanly via simple loop below. */
    uint8_t idat_fixed[25];
    memcpy(idat_fixed, idat, sizeof(idat));
    {
        uint32_t s1 = 1u, s2 = 0u;
        for (size_t i = 7u; i < 21u; i++) {
            s1 = (s1 + idat_fixed[i]) % 65521u;
            s2 = (s2 + s1) % 65521u;
        }
        wr32be(idat_fixed + 21u, (s2 << 16) | s1);
    }
    struct {
        const char *tag;
        const uint8_t *data;
        size_t len;
    } chunks[4] = {
        {"IHDR", ihdr, sizeof(ihdr)},
        {"gAMA", gama, sizeof(gama)},
        {"IDAT", idat_fixed, sizeof(idat_fixed)},
        {"IEND", NULL, 0u},
    };
    size_t pos = 0u;
    CHECK(cap >= 8u);
    memcpy(out, k_png_sig, 8u);
    pos += 8u;
    for (int i = 0; i < 4; i++) {
        CHECK(pos + 12u + chunks[i].len <= cap);
        wr32be(out + pos, (uint32_t)chunks[i].len);
        memcpy(out + pos + 4u, chunks[i].tag, 4u);
        if (chunks[i].len > 0u) {
            memcpy(out + pos + 8u, chunks[i].data, chunks[i].len);
        }
        uint32_t crc = png_crc32(out + pos + 4u, 4u + chunks[i].len);
        wr32be(out + pos + 8u + chunks[i].len, crc);
        pos += 12u + chunks[i].len;
    }
    return pos;
}

/* ------------------------------------------------------------------------ */
/* Embedded minimal JPEG and sRGB ICC fixtures                              */
/* ------------------------------------------------------------------------ */

static const uint8_t k_srgb_icc[] = {
    0x00u, 0x00u, 0x02u, 0x4cu, 0x6cu, 0x63u, 0x6du, 0x73u, 0x04u, 0x40u, 0x00u, 0x00u,
    0x6du, 0x6eu, 0x74u, 0x72u, 0x52u, 0x47u, 0x42u, 0x20u, 0x58u, 0x59u, 0x5au, 0x20u,
    0x07u, 0xeau, 0x00u, 0x0bu, 0x00u, 0x10u, 0x00u, 0x16u, 0x00u, 0x1bu, 0x00u, 0x15u,
    0x61u, 0x63u, 0x73u, 0x70u, 0x41u, 0x50u, 0x50u, 0x4cu, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xf6u, 0xd6u,
    0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0xd3u, 0x2du, 0x6cu, 0x63u, 0x6du, 0x73u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0bu,
    0x64u, 0x65u, 0x73u, 0x63u, 0x00u, 0x00u, 0x01u, 0x08u, 0x00u, 0x00u, 0x00u, 0x36u,
    0x63u, 0x70u, 0x72u, 0x74u, 0x00u, 0x00u, 0x01u, 0x40u, 0x00u, 0x00u, 0x00u, 0x4cu,
    0x77u, 0x74u, 0x70u, 0x74u, 0x00u, 0x00u, 0x01u, 0x8cu, 0x00u, 0x00u, 0x00u, 0x14u,
    0x63u, 0x68u, 0x61u, 0x64u, 0x00u, 0x00u, 0x01u, 0xa0u, 0x00u, 0x00u, 0x00u, 0x2cu,
    0x72u, 0x58u, 0x59u, 0x5au, 0x00u, 0x00u, 0x01u, 0xccu, 0x00u, 0x00u, 0x00u, 0x14u,
    0x62u, 0x58u, 0x59u, 0x5au, 0x00u, 0x00u, 0x01u, 0xe0u, 0x00u, 0x00u, 0x00u, 0x14u,
    0x67u, 0x58u, 0x59u, 0x5au, 0x00u, 0x00u, 0x01u, 0xf4u, 0x00u, 0x00u, 0x00u, 0x14u,
    0x72u, 0x54u, 0x52u, 0x43u, 0x00u, 0x00u, 0x02u, 0x08u, 0x00u, 0x00u, 0x00u, 0x20u,
    0x67u, 0x54u, 0x52u, 0x43u, 0x00u, 0x00u, 0x02u, 0x08u, 0x00u, 0x00u, 0x00u, 0x20u,
    0x62u, 0x54u, 0x52u, 0x43u, 0x00u, 0x00u, 0x02u, 0x08u, 0x00u, 0x00u, 0x00u, 0x20u,
    0x63u, 0x68u, 0x72u, 0x6du, 0x00u, 0x00u, 0x02u, 0x28u, 0x00u, 0x00u, 0x00u, 0x24u,
    0x6du, 0x6cu, 0x75u, 0x63u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
    0x00u, 0x00u, 0x00u, 0x0cu, 0x65u, 0x6eu, 0x55u, 0x53u, 0x00u, 0x00u, 0x00u, 0x1au,
    0x00u, 0x00u, 0x00u, 0x1cu, 0x00u, 0x73u, 0x00u, 0x52u, 0x00u, 0x47u, 0x00u, 0x42u,
    0x00u, 0x20u, 0x00u, 0x62u, 0x00u, 0x75u, 0x00u, 0x69u, 0x00u, 0x6cu, 0x00u, 0x74u,
    0x00u, 0x2du, 0x00u, 0x69u, 0x00u, 0x6eu, 0x00u, 0x00u, 0x6du, 0x6cu, 0x75u, 0x63u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x0cu,
    0x65u, 0x6eu, 0x55u, 0x53u, 0x00u, 0x00u, 0x00u, 0x30u, 0x00u, 0x00u, 0x00u, 0x1cu,
    0x00u, 0x4eu, 0x00u, 0x6fu, 0x00u, 0x20u, 0x00u, 0x63u, 0x00u, 0x6fu, 0x00u, 0x70u,
    0x00u, 0x79u, 0x00u, 0x72u, 0x00u, 0x69u, 0x00u, 0x67u, 0x00u, 0x68u, 0x00u, 0x74u,
    0x00u, 0x2cu, 0x00u, 0x20u, 0x00u, 0x75u, 0x00u, 0x73u, 0x00u, 0x65u, 0x00u, 0x20u,
    0x00u, 0x66u, 0x00u, 0x72u, 0x00u, 0x65u, 0x00u, 0x65u, 0x00u, 0x6cu, 0x00u, 0x79u,
    0x58u, 0x59u, 0x5au, 0x20u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xf6u, 0xd6u,
    0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0xd3u, 0x2du, 0x73u, 0x66u, 0x33u, 0x32u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x0cu, 0x42u, 0x00u, 0x00u, 0x05u, 0xdeu,
    0xffu, 0xffu, 0xf3u, 0x25u, 0x00u, 0x00u, 0x07u, 0x93u, 0x00u, 0x00u, 0xfdu, 0x90u,
    0xffu, 0xffu, 0xfbu, 0xa1u, 0xffu, 0xffu, 0xfdu, 0xa2u, 0x00u, 0x00u, 0x03u, 0xdcu,
    0x00u, 0x00u, 0xc0u, 0x6eu, 0x58u, 0x59u, 0x5au, 0x20u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x6fu, 0xa0u, 0x00u, 0x00u, 0x38u, 0xf5u, 0x00u, 0x00u, 0x03u, 0x90u,
    0x58u, 0x59u, 0x5au, 0x20u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x24u, 0x9fu,
    0x00u, 0x00u, 0x0fu, 0x84u, 0x00u, 0x00u, 0xb6u, 0xc3u, 0x58u, 0x59u, 0x5au, 0x20u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x62u, 0x97u, 0x00u, 0x00u, 0xb7u, 0x87u,
    0x00u, 0x00u, 0x18u, 0xd9u, 0x70u, 0x61u, 0x72u, 0x61u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x03u, 0x00u, 0x00u, 0x00u, 0x02u, 0x66u, 0x66u, 0x00u, 0x00u, 0xf2u, 0xa7u,
    0x00u, 0x00u, 0x0du, 0x59u, 0x00u, 0x00u, 0x13u, 0xd0u, 0x00u, 0x00u, 0x0au, 0x5bu,
    0x63u, 0x68u, 0x72u, 0x6du, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u, 0x00u, 0x00u,
    0x00u, 0x00u, 0xa3u, 0xd7u, 0x00u, 0x00u, 0x54u, 0x7bu, 0x00u, 0x00u, 0x4cu, 0xcdu,
    0x00u, 0x00u, 0x99u, 0x9au, 0x00u, 0x00u, 0x26u, 0x66u, 0x00u, 0x00u, 0x0fu, 0x5cu,
};

static const uint8_t k_jpeg_baseline_16x16[] = {
    0xffu, 0xd8u, 0xffu, 0xe0u, 0x00u, 0x10u, 0x4au, 0x46u, 0x49u, 0x46u, 0x00u, 0x01u,
    0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x00u, 0x00u, 0xffu, 0xdbu, 0x00u, 0x43u,
    0x00u, 0x05u, 0x03u, 0x04u, 0x04u, 0x04u, 0x03u, 0x05u, 0x04u, 0x04u, 0x04u, 0x05u,
    0x05u, 0x05u, 0x06u, 0x07u, 0x0cu, 0x08u, 0x07u, 0x07u, 0x07u, 0x07u, 0x0fu, 0x0bu,
    0x0bu, 0x09u, 0x0cu, 0x11u, 0x0fu, 0x12u, 0x12u, 0x11u, 0x0fu, 0x11u, 0x11u, 0x13u,
    0x16u, 0x1cu, 0x17u, 0x13u, 0x14u, 0x1au, 0x15u, 0x11u, 0x11u, 0x18u, 0x21u, 0x18u,
    0x1au, 0x1du, 0x1du, 0x1fu, 0x1fu, 0x1fu, 0x13u, 0x17u, 0x22u, 0x24u, 0x22u, 0x1eu,
    0x24u, 0x1cu, 0x1eu, 0x1fu, 0x1eu, 0xffu, 0xdbu, 0x00u, 0x43u, 0x01u, 0x05u, 0x05u,
    0x05u, 0x07u, 0x06u, 0x07u, 0x0eu, 0x08u, 0x08u, 0x0eu, 0x1eu, 0x14u, 0x11u, 0x14u,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0xffu, 0xc0u, 0x00u, 0x11u, 0x08u, 0x00u, 0x10u, 0x00u, 0x10u, 0x03u,
    0x01u, 0x22u, 0x00u, 0x02u, 0x11u, 0x01u, 0x03u, 0x11u, 0x01u, 0xffu, 0xc4u, 0x00u,
    0x1fu, 0x00u, 0x00u, 0x01u, 0x05u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u,
    0x06u, 0x07u, 0x08u, 0x09u, 0x0au, 0x0bu, 0xffu, 0xc4u, 0x00u, 0xb5u, 0x10u, 0x00u,
    0x02u, 0x01u, 0x03u, 0x03u, 0x02u, 0x04u, 0x03u, 0x05u, 0x05u, 0x04u, 0x04u, 0x00u,
    0x00u, 0x01u, 0x7du, 0x01u, 0x02u, 0x03u, 0x00u, 0x04u, 0x11u, 0x05u, 0x12u, 0x21u,
    0x31u, 0x41u, 0x06u, 0x13u, 0x51u, 0x61u, 0x07u, 0x22u, 0x71u, 0x14u, 0x32u, 0x81u,
    0x91u, 0xa1u, 0x08u, 0x23u, 0x42u, 0xb1u, 0xc1u, 0x15u, 0x52u, 0xd1u, 0xf0u, 0x24u,
    0x33u, 0x62u, 0x72u, 0x82u, 0x09u, 0x0au, 0x16u, 0x17u, 0x18u, 0x19u, 0x1au, 0x25u,
    0x26u, 0x27u, 0x28u, 0x29u, 0x2au, 0x34u, 0x35u, 0x36u, 0x37u, 0x38u, 0x39u, 0x3au,
    0x43u, 0x44u, 0x45u, 0x46u, 0x47u, 0x48u, 0x49u, 0x4au, 0x53u, 0x54u, 0x55u, 0x56u,
    0x57u, 0x58u, 0x59u, 0x5au, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u, 0x68u, 0x69u, 0x6au,
    0x73u, 0x74u, 0x75u, 0x76u, 0x77u, 0x78u, 0x79u, 0x7au, 0x83u, 0x84u, 0x85u, 0x86u,
    0x87u, 0x88u, 0x89u, 0x8au, 0x92u, 0x93u, 0x94u, 0x95u, 0x96u, 0x97u, 0x98u, 0x99u,
    0x9au, 0xa2u, 0xa3u, 0xa4u, 0xa5u, 0xa6u, 0xa7u, 0xa8u, 0xa9u, 0xaau, 0xb2u, 0xb3u,
    0xb4u, 0xb5u, 0xb6u, 0xb7u, 0xb8u, 0xb9u, 0xbau, 0xc2u, 0xc3u, 0xc4u, 0xc5u, 0xc6u,
    0xc7u, 0xc8u, 0xc9u, 0xcau, 0xd2u, 0xd3u, 0xd4u, 0xd5u, 0xd6u, 0xd7u, 0xd8u, 0xd9u,
    0xdau, 0xe1u, 0xe2u, 0xe3u, 0xe4u, 0xe5u, 0xe6u, 0xe7u, 0xe8u, 0xe9u, 0xeau, 0xf1u,
    0xf2u, 0xf3u, 0xf4u, 0xf5u, 0xf6u, 0xf7u, 0xf8u, 0xf9u, 0xfau, 0xffu, 0xc4u, 0x00u,
    0x1fu, 0x01u, 0x00u, 0x03u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u,
    0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u,
    0x06u, 0x07u, 0x08u, 0x09u, 0x0au, 0x0bu, 0xffu, 0xc4u, 0x00u, 0xb5u, 0x11u, 0x00u,
    0x02u, 0x01u, 0x02u, 0x04u, 0x04u, 0x03u, 0x04u, 0x07u, 0x05u, 0x04u, 0x04u, 0x00u,
    0x01u, 0x02u, 0x77u, 0x00u, 0x01u, 0x02u, 0x03u, 0x11u, 0x04u, 0x05u, 0x21u, 0x31u,
    0x06u, 0x12u, 0x41u, 0x51u, 0x07u, 0x61u, 0x71u, 0x13u, 0x22u, 0x32u, 0x81u, 0x08u,
    0x14u, 0x42u, 0x91u, 0xa1u, 0xb1u, 0xc1u, 0x09u, 0x23u, 0x33u, 0x52u, 0xf0u, 0x15u,
    0x62u, 0x72u, 0xd1u, 0x0au, 0x16u, 0x24u, 0x34u, 0xe1u, 0x25u, 0xf1u, 0x17u, 0x18u,
    0x19u, 0x1au, 0x26u, 0x27u, 0x28u, 0x29u, 0x2au, 0x35u, 0x36u, 0x37u, 0x38u, 0x39u,
    0x3au, 0x43u, 0x44u, 0x45u, 0x46u, 0x47u, 0x48u, 0x49u, 0x4au, 0x53u, 0x54u, 0x55u,
    0x56u, 0x57u, 0x58u, 0x59u, 0x5au, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u, 0x68u, 0x69u,
    0x6au, 0x73u, 0x74u, 0x75u, 0x76u, 0x77u, 0x78u, 0x79u, 0x7au, 0x82u, 0x83u, 0x84u,
    0x85u, 0x86u, 0x87u, 0x88u, 0x89u, 0x8au, 0x92u, 0x93u, 0x94u, 0x95u, 0x96u, 0x97u,
    0x98u, 0x99u, 0x9au, 0xa2u, 0xa3u, 0xa4u, 0xa5u, 0xa6u, 0xa7u, 0xa8u, 0xa9u, 0xaau,
    0xb2u, 0xb3u, 0xb4u, 0xb5u, 0xb6u, 0xb7u, 0xb8u, 0xb9u, 0xbau, 0xc2u, 0xc3u, 0xc4u,
    0xc5u, 0xc6u, 0xc7u, 0xc8u, 0xc9u, 0xcau, 0xd2u, 0xd3u, 0xd4u, 0xd5u, 0xd6u, 0xd7u,
    0xd8u, 0xd9u, 0xdau, 0xe2u, 0xe3u, 0xe4u, 0xe5u, 0xe6u, 0xe7u, 0xe8u, 0xe9u, 0xeau,
    0xf2u, 0xf3u, 0xf4u, 0xf5u, 0xf6u, 0xf7u, 0xf8u, 0xf9u, 0xfau, 0xffu, 0xdau, 0x00u,
    0x0cu, 0x03u, 0x01u, 0x00u, 0x02u, 0x11u, 0x03u, 0x11u, 0x00u, 0x3fu, 0x00u, 0xf0u,
    0xcfu, 0x0eu, 0xf8u, 0x43u, 0xeeu, 0xfeu, 0xebu, 0xf4u, 0xafu, 0x49u, 0xf0u, 0xe7u,
    0x84u, 0x3eu, 0xefu, 0xeeu, 0xbfu, 0x4au, 0xeeu, 0xfcu, 0x39u, 0xe1u, 0x0fu, 0xbbu,
    0xfbu, 0xafu, 0xd2u, 0xbdu, 0x27u, 0xc3u, 0xbeu, 0x10u, 0xfbu, 0xbfu, 0xbau, 0xfdu,
    0x28u, 0xc3u, 0x62u, 0x43u, 0x82u, 0xb8u, 0xd7u, 0xe1u, 0xf7u, 0x8fu, 0xffu, 0xd9u,
};

static const uint8_t k_jpeg_prog_16x16[] = {
    0xffu, 0xd8u, 0xffu, 0xe0u, 0x00u, 0x10u, 0x4au, 0x46u, 0x49u, 0x46u, 0x00u, 0x01u,
    0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x00u, 0x00u, 0xffu, 0xdbu, 0x00u, 0x43u,
    0x00u, 0x05u, 0x03u, 0x04u, 0x04u, 0x04u, 0x03u, 0x05u, 0x04u, 0x04u, 0x04u, 0x05u,
    0x05u, 0x05u, 0x06u, 0x07u, 0x0cu, 0x08u, 0x07u, 0x07u, 0x07u, 0x07u, 0x0fu, 0x0bu,
    0x0bu, 0x09u, 0x0cu, 0x11u, 0x0fu, 0x12u, 0x12u, 0x11u, 0x0fu, 0x11u, 0x11u, 0x13u,
    0x16u, 0x1cu, 0x17u, 0x13u, 0x14u, 0x1au, 0x15u, 0x11u, 0x11u, 0x18u, 0x21u, 0x18u,
    0x1au, 0x1du, 0x1du, 0x1fu, 0x1fu, 0x1fu, 0x13u, 0x17u, 0x22u, 0x24u, 0x22u, 0x1eu,
    0x24u, 0x1cu, 0x1eu, 0x1fu, 0x1eu, 0xffu, 0xdbu, 0x00u, 0x43u, 0x01u, 0x05u, 0x05u,
    0x05u, 0x07u, 0x06u, 0x07u, 0x0eu, 0x08u, 0x08u, 0x0eu, 0x1eu, 0x14u, 0x11u, 0x14u,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0xffu, 0xc2u, 0x00u, 0x11u, 0x08u, 0x00u, 0x10u, 0x00u, 0x10u, 0x03u,
    0x01u, 0x22u, 0x00u, 0x02u, 0x11u, 0x01u, 0x03u, 0x11u, 0x01u, 0xffu, 0xc4u, 0x00u,
    0x15u, 0x00u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x06u, 0x05u, 0xffu, 0xc4u, 0x00u, 0x14u,
    0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xffu, 0xdau, 0x00u, 0x0cu, 0x03u, 0x01u,
    0x00u, 0x02u, 0x10u, 0x03u, 0x10u, 0x00u, 0x00u, 0x01u, 0x0au, 0x96u, 0xeau, 0x53u,
    0xffu, 0xc4u, 0x00u, 0x15u, 0x10u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0xffu,
    0xdau, 0x00u, 0x08u, 0x01u, 0x01u, 0x00u, 0x01u, 0x05u, 0x02u, 0x39u, 0x23u, 0x92u,
    0x39u, 0x23u, 0x92u, 0xffu, 0xc4u, 0x00u, 0x17u, 0x11u, 0x01u, 0x00u, 0x03u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x05u, 0x00u, 0x21u, 0x31u, 0xffu, 0xdau, 0x00u, 0x08u, 0x01u, 0x03u, 0x01u, 0x01u,
    0x3fu, 0x01u, 0x15u, 0xacu, 0xb9u, 0xffu, 0xc4u, 0x00u, 0x15u, 0x11u, 0x01u, 0x01u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x03u, 0x00u, 0xffu, 0xdau, 0x00u, 0x08u, 0x01u, 0x02u, 0x01u, 0x01u,
    0x3fu, 0x01u, 0x34u, 0xbfu, 0xffu, 0xc4u, 0x00u, 0x15u, 0x10u, 0x01u, 0x01u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x31u, 0xffu, 0xdau, 0x00u, 0x08u, 0x01u, 0x01u, 0x00u, 0x06u, 0x3fu,
    0x02u, 0x88u, 0x88u, 0xffu, 0xc4u, 0x00u, 0x15u, 0x10u, 0x01u, 0x01u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x31u, 0xffu, 0xdau, 0x00u, 0x08u, 0x01u, 0x01u, 0x00u, 0x01u, 0x3fu, 0x21u,
    0x99u, 0x32u, 0x64u, 0xcfu, 0xffu, 0xdau, 0x00u, 0x0cu, 0x03u, 0x01u, 0x00u, 0x02u,
    0x00u, 0x03u, 0x00u, 0x00u, 0x00u, 0x10u, 0xa3u, 0xffu, 0xc4u, 0x00u, 0x14u, 0x11u,
    0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xffu, 0xdau, 0x00u, 0x08u, 0x01u, 0x03u, 0x01u,
    0x01u, 0x3fu, 0x10u, 0x67u, 0xffu, 0xc4u, 0x00u, 0x14u, 0x11u, 0x01u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0xffu, 0xdau, 0x00u, 0x08u, 0x01u, 0x02u, 0x01u, 0x01u, 0x3fu, 0x10u,
    0x7fu, 0xffu, 0xc4u, 0x00u, 0x15u, 0x10u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xf1u,
    0xffu, 0xdau, 0x00u, 0x08u, 0x01u, 0x01u, 0x00u, 0x01u, 0x3fu, 0x10u, 0x8au, 0x9au,
    0x9au, 0x8au, 0xffu, 0xd9u,
};

static const uint8_t k_jpeg_gray_16x16[] = {
    0xffu, 0xd8u, 0xffu, 0xe0u, 0x00u, 0x10u, 0x4au, 0x46u, 0x49u, 0x46u, 0x00u, 0x01u,
    0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x00u, 0x00u, 0xffu, 0xdbu, 0x00u, 0x43u,
    0x00u, 0x03u, 0x02u, 0x02u, 0x03u, 0x02u, 0x02u, 0x03u, 0x03u, 0x03u, 0x03u, 0x04u,
    0x03u, 0x03u, 0x04u, 0x05u, 0x08u, 0x05u, 0x05u, 0x04u, 0x04u, 0x05u, 0x0au, 0x07u,
    0x07u, 0x06u, 0x08u, 0x0cu, 0x0au, 0x0cu, 0x0cu, 0x0bu, 0x0au, 0x0bu, 0x0bu, 0x0du,
    0x0eu, 0x12u, 0x10u, 0x0du, 0x0eu, 0x11u, 0x0eu, 0x0bu, 0x0bu, 0x10u, 0x16u, 0x10u,
    0x11u, 0x13u, 0x14u, 0x15u, 0x15u, 0x15u, 0x0cu, 0x0fu, 0x17u, 0x18u, 0x16u, 0x14u,
    0x18u, 0x12u, 0x14u, 0x15u, 0x14u, 0xffu, 0xc0u, 0x00u, 0x0bu, 0x08u, 0x00u, 0x10u,
    0x00u, 0x10u, 0x01u, 0x01u, 0x11u, 0x00u, 0xffu, 0xc4u, 0x00u, 0x1fu, 0x00u, 0x00u,
    0x01u, 0x05u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u,
    0x09u, 0x0au, 0x0bu, 0xffu, 0xc4u, 0x00u, 0xb5u, 0x10u, 0x00u, 0x02u, 0x01u, 0x03u,
    0x03u, 0x02u, 0x04u, 0x03u, 0x05u, 0x05u, 0x04u, 0x04u, 0x00u, 0x00u, 0x01u, 0x7du,
    0x01u, 0x02u, 0x03u, 0x00u, 0x04u, 0x11u, 0x05u, 0x12u, 0x21u, 0x31u, 0x41u, 0x06u,
    0x13u, 0x51u, 0x61u, 0x07u, 0x22u, 0x71u, 0x14u, 0x32u, 0x81u, 0x91u, 0xa1u, 0x08u,
    0x23u, 0x42u, 0xb1u, 0xc1u, 0x15u, 0x52u, 0xd1u, 0xf0u, 0x24u, 0x33u, 0x62u, 0x72u,
    0x82u, 0x09u, 0x0au, 0x16u, 0x17u, 0x18u, 0x19u, 0x1au, 0x25u, 0x26u, 0x27u, 0x28u,
    0x29u, 0x2au, 0x34u, 0x35u, 0x36u, 0x37u, 0x38u, 0x39u, 0x3au, 0x43u, 0x44u, 0x45u,
    0x46u, 0x47u, 0x48u, 0x49u, 0x4au, 0x53u, 0x54u, 0x55u, 0x56u, 0x57u, 0x58u, 0x59u,
    0x5au, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u, 0x68u, 0x69u, 0x6au, 0x73u, 0x74u, 0x75u,
    0x76u, 0x77u, 0x78u, 0x79u, 0x7au, 0x83u, 0x84u, 0x85u, 0x86u, 0x87u, 0x88u, 0x89u,
    0x8au, 0x92u, 0x93u, 0x94u, 0x95u, 0x96u, 0x97u, 0x98u, 0x99u, 0x9au, 0xa2u, 0xa3u,
    0xa4u, 0xa5u, 0xa6u, 0xa7u, 0xa8u, 0xa9u, 0xaau, 0xb2u, 0xb3u, 0xb4u, 0xb5u, 0xb6u,
    0xb7u, 0xb8u, 0xb9u, 0xbau, 0xc2u, 0xc3u, 0xc4u, 0xc5u, 0xc6u, 0xc7u, 0xc8u, 0xc9u,
    0xcau, 0xd2u, 0xd3u, 0xd4u, 0xd5u, 0xd6u, 0xd7u, 0xd8u, 0xd9u, 0xdau, 0xe1u, 0xe2u,
    0xe3u, 0xe4u, 0xe5u, 0xe6u, 0xe7u, 0xe8u, 0xe9u, 0xeau, 0xf1u, 0xf2u, 0xf3u, 0xf4u,
    0xf5u, 0xf6u, 0xf7u, 0xf8u, 0xf9u, 0xfau, 0xffu, 0xdau, 0x00u, 0x08u, 0x01u, 0x01u,
    0x00u, 0x00u, 0x3fu, 0x00u, 0xf9u, 0x23u, 0xf6u, 0x6du, 0xb6u, 0xffu, 0x00u, 0x8fu,
    0x5eu, 0x3du, 0x2bu, 0xf5u, 0x5bu, 0xf6u, 0x6du, 0xb6u, 0xffu, 0x00u, 0x8fu, 0x5eu,
    0x3du, 0x2bu, 0xf2u, 0xffu, 0x00u, 0xf6u, 0x6du, 0xb6u, 0xffu, 0x00u, 0x8fu, 0x5eu,
    0x3du, 0x2bu, 0xf5u, 0x57u, 0xf6u, 0x6du, 0xb6u, 0xffu, 0x00u, 0x8fu, 0x5eu, 0x3du,
    0x2bu, 0xffu, 0xd9u,
};

static const uint8_t k_jpeg_baseline_16x8[] = {
    0xffu, 0xd8u, 0xffu, 0xe0u, 0x00u, 0x10u, 0x4au, 0x46u, 0x49u, 0x46u, 0x00u, 0x01u,
    0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x00u, 0x00u, 0xffu, 0xdbu, 0x00u, 0x43u,
    0x00u, 0x05u, 0x03u, 0x04u, 0x04u, 0x04u, 0x03u, 0x05u, 0x04u, 0x04u, 0x04u, 0x05u,
    0x05u, 0x05u, 0x06u, 0x07u, 0x0cu, 0x08u, 0x07u, 0x07u, 0x07u, 0x07u, 0x0fu, 0x0bu,
    0x0bu, 0x09u, 0x0cu, 0x11u, 0x0fu, 0x12u, 0x12u, 0x11u, 0x0fu, 0x11u, 0x11u, 0x13u,
    0x16u, 0x1cu, 0x17u, 0x13u, 0x14u, 0x1au, 0x15u, 0x11u, 0x11u, 0x18u, 0x21u, 0x18u,
    0x1au, 0x1du, 0x1du, 0x1fu, 0x1fu, 0x1fu, 0x13u, 0x17u, 0x22u, 0x24u, 0x22u, 0x1eu,
    0x24u, 0x1cu, 0x1eu, 0x1fu, 0x1eu, 0xffu, 0xdbu, 0x00u, 0x43u, 0x01u, 0x05u, 0x05u,
    0x05u, 0x07u, 0x06u, 0x07u, 0x0eu, 0x08u, 0x08u, 0x0eu, 0x1eu, 0x14u, 0x11u, 0x14u,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu, 0x1eu,
    0x1eu, 0x1eu, 0xffu, 0xc0u, 0x00u, 0x11u, 0x08u, 0x00u, 0x08u, 0x00u, 0x10u, 0x03u,
    0x01u, 0x11u, 0x00u, 0x02u, 0x11u, 0x01u, 0x03u, 0x11u, 0x01u, 0xffu, 0xc4u, 0x00u,
    0x1fu, 0x00u, 0x00u, 0x01u, 0x05u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u,
    0x06u, 0x07u, 0x08u, 0x09u, 0x0au, 0x0bu, 0xffu, 0xc4u, 0x00u, 0xb5u, 0x10u, 0x00u,
    0x02u, 0x01u, 0x03u, 0x03u, 0x02u, 0x04u, 0x03u, 0x05u, 0x05u, 0x04u, 0x04u, 0x00u,
    0x00u, 0x01u, 0x7du, 0x01u, 0x02u, 0x03u, 0x00u, 0x04u, 0x11u, 0x05u, 0x12u, 0x21u,
    0x31u, 0x41u, 0x06u, 0x13u, 0x51u, 0x61u, 0x07u, 0x22u, 0x71u, 0x14u, 0x32u, 0x81u,
    0x91u, 0xa1u, 0x08u, 0x23u, 0x42u, 0xb1u, 0xc1u, 0x15u, 0x52u, 0xd1u, 0xf0u, 0x24u,
    0x33u, 0x62u, 0x72u, 0x82u, 0x09u, 0x0au, 0x16u, 0x17u, 0x18u, 0x19u, 0x1au, 0x25u,
    0x26u, 0x27u, 0x28u, 0x29u, 0x2au, 0x34u, 0x35u, 0x36u, 0x37u, 0x38u, 0x39u, 0x3au,
    0x43u, 0x44u, 0x45u, 0x46u, 0x47u, 0x48u, 0x49u, 0x4au, 0x53u, 0x54u, 0x55u, 0x56u,
    0x57u, 0x58u, 0x59u, 0x5au, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u, 0x68u, 0x69u, 0x6au,
    0x73u, 0x74u, 0x75u, 0x76u, 0x77u, 0x78u, 0x79u, 0x7au, 0x83u, 0x84u, 0x85u, 0x86u,
    0x87u, 0x88u, 0x89u, 0x8au, 0x92u, 0x93u, 0x94u, 0x95u, 0x96u, 0x97u, 0x98u, 0x99u,
    0x9au, 0xa2u, 0xa3u, 0xa4u, 0xa5u, 0xa6u, 0xa7u, 0xa8u, 0xa9u, 0xaau, 0xb2u, 0xb3u,
    0xb4u, 0xb5u, 0xb6u, 0xb7u, 0xb8u, 0xb9u, 0xbau, 0xc2u, 0xc3u, 0xc4u, 0xc5u, 0xc6u,
    0xc7u, 0xc8u, 0xc9u, 0xcau, 0xd2u, 0xd3u, 0xd4u, 0xd5u, 0xd6u, 0xd7u, 0xd8u, 0xd9u,
    0xdau, 0xe1u, 0xe2u, 0xe3u, 0xe4u, 0xe5u, 0xe6u, 0xe7u, 0xe8u, 0xe9u, 0xeau, 0xf1u,
    0xf2u, 0xf3u, 0xf4u, 0xf5u, 0xf6u, 0xf7u, 0xf8u, 0xf9u, 0xfau, 0xffu, 0xc4u, 0x00u,
    0x1fu, 0x01u, 0x00u, 0x03u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u,
    0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u,
    0x06u, 0x07u, 0x08u, 0x09u, 0x0au, 0x0bu, 0xffu, 0xc4u, 0x00u, 0xb5u, 0x11u, 0x00u,
    0x02u, 0x01u, 0x02u, 0x04u, 0x04u, 0x03u, 0x04u, 0x07u, 0x05u, 0x04u, 0x04u, 0x00u,
    0x01u, 0x02u, 0x77u, 0x00u, 0x01u, 0x02u, 0x03u, 0x11u, 0x04u, 0x05u, 0x21u, 0x31u,
    0x06u, 0x12u, 0x41u, 0x51u, 0x07u, 0x61u, 0x71u, 0x13u, 0x22u, 0x32u, 0x81u, 0x08u,
    0x14u, 0x42u, 0x91u, 0xa1u, 0xb1u, 0xc1u, 0x09u, 0x23u, 0x33u, 0x52u, 0xf0u, 0x15u,
    0x62u, 0x72u, 0xd1u, 0x0au, 0x16u, 0x24u, 0x34u, 0xe1u, 0x25u, 0xf1u, 0x17u, 0x18u,
    0x19u, 0x1au, 0x26u, 0x27u, 0x28u, 0x29u, 0x2au, 0x35u, 0x36u, 0x37u, 0x38u, 0x39u,
    0x3au, 0x43u, 0x44u, 0x45u, 0x46u, 0x47u, 0x48u, 0x49u, 0x4au, 0x53u, 0x54u, 0x55u,
    0x56u, 0x57u, 0x58u, 0x59u, 0x5au, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u, 0x68u, 0x69u,
    0x6au, 0x73u, 0x74u, 0x75u, 0x76u, 0x77u, 0x78u, 0x79u, 0x7au, 0x82u, 0x83u, 0x84u,
    0x85u, 0x86u, 0x87u, 0x88u, 0x89u, 0x8au, 0x92u, 0x93u, 0x94u, 0x95u, 0x96u, 0x97u,
    0x98u, 0x99u, 0x9au, 0xa2u, 0xa3u, 0xa4u, 0xa5u, 0xa6u, 0xa7u, 0xa8u, 0xa9u, 0xaau,
    0xb2u, 0xb3u, 0xb4u, 0xb5u, 0xb6u, 0xb7u, 0xb8u, 0xb9u, 0xbau, 0xc2u, 0xc3u, 0xc4u,
    0xc5u, 0xc6u, 0xc7u, 0xc8u, 0xc9u, 0xcau, 0xd2u, 0xd3u, 0xd4u, 0xd5u, 0xd6u, 0xd7u,
    0xd8u, 0xd9u, 0xdau, 0xe2u, 0xe3u, 0xe4u, 0xe5u, 0xe6u, 0xe7u, 0xe8u, 0xe9u, 0xeau,
    0xf2u, 0xf3u, 0xf4u, 0xf5u, 0xf6u, 0xf7u, 0xf8u, 0xf9u, 0xfau, 0xffu, 0xdau, 0x00u,
    0x0cu, 0x03u, 0x01u, 0x00u, 0x02u, 0x11u, 0x03u, 0x11u, 0x00u, 0x3fu, 0x00u, 0xccu,
    0xafu, 0xd1u, 0x4fu, 0x84u, 0x0au, 0x00u, 0xffu, 0xd9u,
};

/* Direct upstream JxlEncoder helper for generating fixtures JXL->decoder tests
 * without going through rfc_jxl_encode (covers XYB lossy, brob boxes, odd
 * alpha depths, animations, non-zero Exif TIFF offsets). */
static void upstream_encode_jxl(uint32_t w,
                                uint32_t h,
                                uint32_t ch,
                                uint32_t bps,
                                uint32_t alpha_bits,
                                int lossless,
                                int uses_orig,
                                int animated,
                                int compress_boxes,
                                const uint8_t *exif_box_payload,
                                size_t exif_box_len,
                                const uint8_t *xmp_payload,
                                size_t xmp_len,
                                const uint8_t *pixels,
                                size_t pix_len,
                                uint8_t **out_buf,
                                size_t *out_len) {
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    CHECK(enc != NULL);
    int need_box = (exif_box_payload != NULL || xmp_payload != NULL);
    if (need_box) {
        CHECK(JxlEncoderUseBoxes(enc) == JXL_ENC_SUCCESS);
    }
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = w;
    bi.ysize = h;
    bi.bits_per_sample = bps;
    bi.exponent_bits_per_sample = 0u;
    bi.uses_original_profile = uses_orig ? JXL_TRUE : JXL_FALSE;
    bi.num_color_channels = (ch == 1u) ? 1u : 3u;
    bi.num_extra_channels = (ch == 4u) ? 1u : 0u;
    bi.alpha_bits = (ch == 4u) ? alpha_bits : 0u;
    bi.have_animation = animated ? JXL_TRUE : JXL_FALSE;
    if (animated) {
        bi.animation.tps_numerator = 10u;
        bi.animation.tps_denominator = 1u;
    }
    CHECK(JxlEncoderSetBasicInfo(enc, &bi) == JXL_ENC_SUCCESS);
    JxlColorEncoding ce;
    JxlColorEncodingSetToSRGB(&ce, (ch == 1u) ? JXL_TRUE : JXL_FALSE);
    CHECK(JxlEncoderSetColorEncoding(enc, &ce) == JXL_ENC_SUCCESS);

    if (exif_box_payload) {
        JxlBoxType t = {'E', 'x', 'i', 'f'};
        CHECK(JxlEncoderAddBox(enc, t, exif_box_payload, exif_box_len,
                               compress_boxes ? JXL_TRUE : JXL_FALSE) ==
              JXL_ENC_SUCCESS);
    }
    if (xmp_payload) {
        JxlBoxType t = {'x', 'm', 'l', ' '};
        CHECK(JxlEncoderAddBox(enc, t, xmp_payload, xmp_len,
                               compress_boxes ? JXL_TRUE : JXL_FALSE) ==
              JXL_ENC_SUCCESS);
    }
    if (need_box) {
        JxlEncoderCloseBoxes(enc);
    }

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    CHECK(fs != NULL);
    if (lossless) {
        CHECK(JxlEncoderSetFrameLossless(fs, JXL_TRUE) == JXL_ENC_SUCCESS);
        CHECK(JxlEncoderFrameSettingsSetOption(
                  fs, JXL_ENC_FRAME_SETTING_MODULAR, 1) == JXL_ENC_SUCCESS);
    } else {
        CHECK(JxlEncoderSetFrameDistance(fs, 1.5f) == JXL_ENC_SUCCESS);
    }
    JxlBitDepth bd = {JXL_BIT_DEPTH_FROM_CODESTREAM, 0u, 0u};
    JxlPixelFormat px = {ch, (bps > 8u) ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8,
                         JXL_NATIVE_ENDIAN, 0u};
    CHECK(JxlEncoderSetFrameBitDepth(fs, &bd) == JXL_ENC_SUCCESS);
    CHECK(JxlEncoderAddImageFrame(fs, &px, pixels, pix_len) == JXL_ENC_SUCCESS);
    if (animated) {
        CHECK(JxlEncoderAddImageFrame(fs, &px, pixels, pix_len) ==
              JXL_ENC_SUCCESS);
    }
    JxlEncoderCloseFrames(enc);

    size_t cap = 4096u, len = 0u;
    uint8_t *buf = (uint8_t *)malloc(cap);
    CHECK(buf != NULL);
    for (;;) {
        uint8_t *next = buf + len;
        size_t avail = cap - len;
        JxlEncoderStatus es = JxlEncoderProcessOutput(enc, &next, &avail);
        len = (size_t)(next - buf);
        if (es == JXL_ENC_NEED_MORE_OUTPUT) {
            cap *= 2u;
            buf = (uint8_t *)realloc(buf, cap);
            CHECK(buf != NULL);
        } else {
            CHECK(es == JXL_ENC_SUCCESS);
            break;
        }
    }
    JxlEncoderDestroy(enc);
    *out_buf = buf;
    *out_len = len;
}

static void upstream_verify_native_codestream(const uint8_t *jxl_bytes,
                                              size_t jxl_len,
                                              uint32_t exp_w,
                                              uint32_t exp_h,
                                              uint32_t exp_bps,
                                              uint32_t exp_alpha_bits,
                                              const void *exp_native_pixels,
                                              size_t exp_native_len) {
    JxlDecoder *dec = JxlDecoderCreate(NULL);
    CHECK(dec != NULL);
    CHECK(JxlDecoderSetKeepOrientation(dec, JXL_TRUE) == JXL_DEC_SUCCESS);
    CHECK(JxlDecoderSetUnpremultiplyAlpha(dec, JXL_FALSE) == JXL_DEC_SUCCESS);
    CHECK(JxlDecoderSubscribeEvents(
              dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS);
    CHECK(JxlDecoderSetInput(dec, jxl_bytes, jxl_len) == JXL_DEC_SUCCESS);
    JxlDecoderCloseInput(dec);

    uint8_t *out = NULL;
    JxlBasicInfo bi;
    memset(&bi, 0, sizeof(bi));
    int got_bi = 0, got_img = 0;
    for (;;) {
        JxlDecoderStatus ds = JxlDecoderProcessInput(dec);
        if (ds == JXL_DEC_SUCCESS) {
            break;
        } else if (ds == JXL_DEC_BASIC_INFO) {
            CHECK(JxlDecoderGetBasicInfo(dec, &bi) == JXL_DEC_SUCCESS);
            CHECK(bi.xsize == exp_w && bi.ysize == exp_h);
            CHECK(bi.bits_per_sample == exp_bps);
            CHECK(bi.alpha_bits == exp_alpha_bits);
            CHECK(bi.uses_original_profile == JXL_TRUE);
            got_bi = 1;
        } else if (ds == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            CHECK(got_bi);
            uint32_t nch = bi.num_color_channels + (bi.alpha_bits ? 1u : 0u);
            JxlPixelFormat px = {nch,
                                 (exp_bps > 8u) ? JXL_TYPE_UINT16
                                                : JXL_TYPE_UINT8,
                                 JXL_NATIVE_ENDIAN, 0u};
            size_t bsz = 0u;
            CHECK(JxlDecoderImageOutBufferSize(dec, &px, &bsz) ==
                  JXL_DEC_SUCCESS);
            CHECK(bsz == exp_native_len);
            out = (uint8_t *)malloc(bsz);
            CHECK(out != NULL);
            CHECK(JxlDecoderSetImageOutBuffer(dec, &px, out, bsz) ==
                  JXL_DEC_SUCCESS);
            JxlBitDepth bd = {JXL_BIT_DEPTH_FROM_CODESTREAM, 0u, 0u};
            CHECK(JxlDecoderSetImageOutBitDepth(dec, &bd) == JXL_DEC_SUCCESS);
        } else if (ds == JXL_DEC_FULL_IMAGE) {
            got_img = 1;
        } else {
            CHECK(0 && "unexpected upstream JxlDecoder status");
        }
    }
    CHECK(got_bi && got_img);
    CHECK(memcmp(out, exp_native_pixels, exp_native_len) == 0);
    free(out);
    JxlDecoderDestroy(dec);
}

static void upstream_encode_jxl_color(uint32_t w,
                                      uint32_t h,
                                      const JxlColorEncoding *ce_opt,
                                      const uint8_t *icc_opt,
                                      size_t icc_len,
                                      int lossless,
                                      int uses_orig,
                                      const uint8_t *exif_box_payload,
                                      size_t exif_box_len,
                                      const uint8_t *xmp_payload,
                                      size_t xmp_len,
                                      const uint8_t *pixels,
                                      size_t pix_len,
                                      uint8_t **out_buf,
                                      size_t *out_len) {
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    CHECK(enc != NULL);
    JxlEncoderSetCms(enc, *JxlGetDefaultCms());
    int need_box = (exif_box_payload != NULL || xmp_payload != NULL);
    if (need_box) {
        CHECK(JxlEncoderUseBoxes(enc) == JXL_ENC_SUCCESS);
    }
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = w;
    bi.ysize = h;
    bi.bits_per_sample = 8u;
    bi.exponent_bits_per_sample = 0u;
    bi.uses_original_profile = uses_orig ? JXL_TRUE : JXL_FALSE;
    bi.num_color_channels = 3u;
    bi.num_extra_channels = 0u;
    CHECK(JxlEncoderSetBasicInfo(enc, &bi) == JXL_ENC_SUCCESS);
    if (icc_opt && icc_len > 0u) {
        CHECK(JxlEncoderSetICCProfile(enc, icc_opt, icc_len) == JXL_ENC_SUCCESS);
    } else {
        CHECK(ce_opt != NULL);
        CHECK(JxlEncoderSetColorEncoding(enc, ce_opt) == JXL_ENC_SUCCESS);
    }
    if (exif_box_payload) {
        JxlBoxType t = {'E', 'x', 'i', 'f'};
        CHECK(JxlEncoderAddBox(enc, t, exif_box_payload, exif_box_len,
                               JXL_FALSE) == JXL_ENC_SUCCESS);
    }
    if (xmp_payload) {
        JxlBoxType t = {'x', 'm', 'l', ' '};
        CHECK(JxlEncoderAddBox(enc, t, xmp_payload, xmp_len, JXL_FALSE) ==
              JXL_ENC_SUCCESS);
    }
    if (need_box) {
        JxlEncoderCloseBoxes(enc);
    }
    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    CHECK(fs != NULL);
    if (lossless) {
        CHECK(JxlEncoderSetFrameLossless(fs, JXL_TRUE) == JXL_ENC_SUCCESS);
        CHECK(JxlEncoderFrameSettingsSetOption(
                  fs, JXL_ENC_FRAME_SETTING_MODULAR, 1) == JXL_ENC_SUCCESS);
    } else {
        CHECK(JxlEncoderSetFrameDistance(fs, 1.0f) == JXL_ENC_SUCCESS);
    }
    JxlPixelFormat px = {3u, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0u};
    CHECK(JxlEncoderAddImageFrame(fs, &px, pixels, pix_len) == JXL_ENC_SUCCESS);
    JxlEncoderCloseFrames(enc);

    size_t cap = 4096u, len = 0u;
    uint8_t *buf = (uint8_t *)malloc(cap);
    CHECK(buf != NULL);
    for (;;) {
        uint8_t *next = buf + len;
        size_t avail = cap - len;
        JxlEncoderStatus es = JxlEncoderProcessOutput(enc, &next, &avail);
        len = (size_t)(next - buf);
        if (es == JXL_ENC_NEED_MORE_OUTPUT) {
            cap *= 2u;
            buf = (uint8_t *)realloc(buf, cap);
            CHECK(buf != NULL);
        } else {
            CHECK(es == JXL_ENC_SUCCESS);
            break;
        }
    }
    JxlEncoderDestroy(enc);
    *out_buf = buf;
    *out_len = len;
}

static void upstream_decode_xyb_pixels(const uint8_t *jxl_bytes,
                                       size_t jxl_len,
                                       const JxlColorEncoding *out_ce,
                                       const uint8_t *out_icc,
                                       size_t out_icc_len,
                                       uint8_t **out_pixels,
                                       size_t *out_pix_len,
                                       uint8_t **out_data_icc,
                                       size_t *out_data_icc_len) {
    JxlDecoder *dec = JxlDecoderCreate(NULL);
    CHECK(dec != NULL);
    CHECK(JxlDecoderSetKeepOrientation(dec, JXL_TRUE) == JXL_DEC_SUCCESS);
    CHECK(JxlDecoderSetCms(dec, *JxlGetDefaultCms()) == JXL_DEC_SUCCESS);
    CHECK(JxlDecoderSubscribeEvents(
              dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                       JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS);
    CHECK(JxlDecoderSetInput(dec, jxl_bytes, jxl_len) == JXL_DEC_SUCCESS);
    JxlDecoderCloseInput(dec);

    uint8_t *pix = NULL;
    size_t pix_len = 0u;
    uint8_t *dicc = NULL;
    size_t dicc_len = 0u;
    float *fbuf = NULL;
    size_t fbuf_len = 0u;
    int cms_flt = 0;
    const JxlCmsInterface *cms = JxlGetDefaultCms();
    JxlColorEncoding lin_ce, dst_ce;
    memset(&lin_ce, 0, sizeof(lin_ce));
    memset(&dst_ce, 0, sizeof(dst_ce));
    uint8_t *lin_icc = NULL;
    size_t lin_icc_len = 0u;
    JxlBasicInfo bi;
    memset(&bi, 0, sizeof(bi));
    int got_bi = 0, got_img = 0;
    for (;;) {
        JxlDecoderStatus ds = JxlDecoderProcessInput(dec);
        if (ds == JXL_DEC_SUCCESS) {
            break;
        } else if (ds == JXL_DEC_BASIC_INFO) {
            CHECK(JxlDecoderGetBasicInfo(dec, &bi) == JXL_DEC_SUCCESS);
            got_bi = 1;
        } else if (ds == JXL_DEC_COLOR_ENCODING) {
            if (out_icc && out_icc_len > 0u) {
                JXL_BOOL is_cmyk = JXL_FALSE;
                CHECK(cms != NULL && cms->set_fields_from_icc != NULL);
                CHECK(cms->set_fields_from_icc(cms->set_fields_data, out_icc,
                                               out_icc_len, &dst_ce,
                                               &is_cmyk) &&
                      !is_cmyk);
                if (dst_ce.transfer_function == JXL_TRANSFER_FUNCTION_UNKNOWN) {
                    cms_flt = 1;
                    CHECK(JxlDecoderGetColorAsEncodedProfile(
                              dec, JXL_COLOR_PROFILE_TARGET_DATA, &lin_ce) ==
                          JXL_DEC_SUCCESS);
                    CHECK(JxlDecoderGetICCProfileSize(
                              dec, JXL_COLOR_PROFILE_TARGET_DATA,
                              &lin_icc_len) == JXL_DEC_SUCCESS);
                    lin_icc = (uint8_t *)malloc(lin_icc_len);
                    CHECK(lin_icc != NULL);
                    CHECK(JxlDecoderGetColorAsICCProfile(
                              dec, JXL_COLOR_PROFILE_TARGET_DATA, lin_icc,
                              lin_icc_len) == JXL_DEC_SUCCESS);
                } else {
                    CHECK(JxlDecoderSetOutputColorProfile(dec, NULL, out_icc,
                                                          out_icc_len) ==
                          JXL_DEC_SUCCESS);
                }
            } else if (out_ce) {
                CHECK(JxlDecoderSetOutputColorProfile(dec, out_ce, NULL, 0u) ==
                      JXL_DEC_SUCCESS);
            }
            if (out_data_icc) {
                CHECK(JxlDecoderGetICCProfileSize(
                          dec, JXL_COLOR_PROFILE_TARGET_DATA, &dicc_len) ==
                      JXL_DEC_SUCCESS);
                CHECK(dicc_len > 0u);
                dicc = (uint8_t *)malloc(dicc_len);
                CHECK(dicc != NULL);
                CHECK(JxlDecoderGetColorAsICCProfile(
                          dec, JXL_COLOR_PROFILE_TARGET_DATA, dicc, dicc_len) ==
                      JXL_DEC_SUCCESS);
            }
        } else if (ds == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            CHECK(got_bi);
            if (cms_flt) {
                JxlPixelFormat fpx = {bi.num_color_channels, JXL_TYPE_FLOAT,
                                      JXL_NATIVE_ENDIAN, 0u};
                CHECK(JxlDecoderImageOutBufferSize(dec, &fpx, &fbuf_len) ==
                      JXL_DEC_SUCCESS);
                fbuf = (float *)malloc(fbuf_len);
                CHECK(fbuf != NULL);
                CHECK(JxlDecoderSetImageOutBuffer(dec, &fpx, fbuf, fbuf_len) ==
                      JXL_DEC_SUCCESS);
            } else {
                JxlPixelFormat px = {bi.num_color_channels, JXL_TYPE_UINT8,
                                     JXL_NATIVE_ENDIAN, 0u};
                CHECK(JxlDecoderImageOutBufferSize(dec, &px, &pix_len) ==
                      JXL_DEC_SUCCESS);
                pix = (uint8_t *)malloc(pix_len);
                CHECK(pix != NULL);
                CHECK(JxlDecoderSetImageOutBuffer(dec, &px, pix, pix_len) ==
                      JXL_DEC_SUCCESS);
            }
        } else if (ds == JXL_DEC_FULL_IMAGE) {
            got_img = 1;
        } else {
            CHECK(0 && "unexpected upstream XYB decoder status");
        }
    }
    CHECK(got_bi && got_img);
    JxlDecoderDestroy(dec);
    if (cms_flt) {
        CHECK(fbuf != NULL && lin_icc != NULL);
        pix_len = (size_t)bi.xsize * (size_t)bi.ysize *
                  (size_t)bi.num_color_channels;
        pix = (uint8_t *)malloc(pix_len);
        CHECK(pix != NULL);
        JxlColorProfile in_p = {{lin_icc, lin_icc_len}, lin_ce,
                                bi.num_color_channels};
        JxlColorProfile out_p = {{out_icc, out_icc_len}, dst_ce,
                                 bi.num_color_channels};
        void *ci = cms->init(cms->init_data, 1u, (size_t)bi.xsize, &in_p,
                             &out_p, 255.0f);
        CHECK(ci != NULL);
        float *sb = cms->get_src_buf(ci, 0u);
        float *db = cms->get_dst_buf(ci, 0u);
        size_t n_row = (size_t)bi.xsize * (size_t)bi.num_color_channels;
        for (uint32_t y = 0u; y < bi.ysize; y++) {
            memcpy(sb, fbuf + (size_t)y * n_row, n_row * sizeof(float));
            CHECK(cms->run(ci, 0u, sb, db, (size_t)bi.xsize));
            for (size_t k = 0u; k < n_row; k++) {
                float v = db[k];
                if (v <= 0.0f) v = 0.0f;
                if (v >= 1.0f) v = 1.0f;
                pix[(size_t)y * n_row + k] = (uint8_t)(v * 255.0f + 0.5f);
            }
        }
        cms->destroy(ci);
        free(fbuf);
        free(lin_icc);
    }
    CHECK(pix != NULL);
    *out_pixels = pix;
    *out_pix_len = pix_len;
    if (out_data_icc && out_data_icc_len) {
        *out_data_icc = dicc;
        *out_data_icc_len = dicc_len;
    }
}

static void upstream_extract_raw_exif_box(const uint8_t *jxl_bytes,
                                          size_t jxl_len,
                                          uint8_t **out_box,
                                          size_t *out_box_len) {
    JxlDecoder *dec = JxlDecoderCreate(NULL);
    CHECK(dec != NULL);
    CHECK(JxlDecoderSetDecompressBoxes(dec, JXL_TRUE) == JXL_DEC_SUCCESS);
    CHECK(JxlDecoderSubscribeEvents(
              dec, JXL_DEC_BOX | JXL_DEC_BOX_COMPLETE) == JXL_DEC_SUCCESS);
    CHECK(JxlDecoderSetInput(dec, jxl_bytes, jxl_len) == JXL_DEC_SUCCESS);
    JxlDecoderCloseInput(dec);

    uint8_t *buf = NULL;
    size_t cap = 4096u;
    size_t used = 0u;
    int capturing = 0;
    for (;;) {
        JxlDecoderStatus ds = JxlDecoderProcessInput(dec);
        if (ds == JXL_DEC_SUCCESS) {
            if (capturing) {
                size_t rem = JxlDecoderReleaseBoxBuffer(dec);
                used = cap - rem;
                capturing = 0;
            }
            break;
        } else if (ds == JXL_DEC_BOX) {
            if (capturing) {
                size_t rem = JxlDecoderReleaseBoxBuffer(dec);
                used = cap - rem;
                capturing = 0;
                break;
            }
            JxlBoxType bt;
            CHECK(JxlDecoderGetBoxType(dec, bt, JXL_TRUE) == JXL_DEC_SUCCESS);
            if (memcmp(bt, "Exif", 4) == 0) {
                buf = (uint8_t *)malloc(cap);
                CHECK(buf != NULL);
                CHECK(JxlDecoderSetBoxBuffer(dec, buf, cap) == JXL_DEC_SUCCESS);
                capturing = 1;
            }
        } else if (ds == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
            CHECK(capturing);
            size_t rem = JxlDecoderReleaseBoxBuffer(dec);
            size_t have = cap - rem;
            cap *= 2u;
            buf = (uint8_t *)realloc(buf, cap);
            CHECK(buf != NULL);
            CHECK(JxlDecoderSetBoxBuffer(dec, buf + have, cap - have) ==
                  JXL_DEC_SUCCESS);
        } else if (ds == JXL_DEC_BOX_COMPLETE) {
            if (capturing) {
                size_t rem = JxlDecoderReleaseBoxBuffer(dec);
                used = cap - rem;
                capturing = 0;
            }
        } else {
            CHECK(0 && "unexpected box decoder status");
        }
    }
    JxlDecoderDestroy(dec);
    CHECK(buf != NULL && used >= 4u);
    *out_box = buf;
    *out_box_len = used;
}

/* ------------------------------------------------------------------------ */
/* Suite 1: Registration, busy guard, and upstream contract                 */
/* ------------------------------------------------------------------------ */

static void test_registration_and_upstream_contract(void) {
    CHECK(rfc_registry_select(RFC_FORMAT_JPEG_XL, RFC_MODE_READ) ==
          RFC_ERR_NO_ADAPTER);
    CHECK(rfc_codecs_unregister_jxl() == RFC_OK);

    CHECK(rfc_codecs_register_jxl() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_select(RFC_FORMAT_JPEG_XL, RFC_MODE_READ) == RFC_OK);
    CHECK(rfc_registry_select(RFC_FORMAT_JPEG_XL, RFC_MODE_WRITE) == RFC_OK);

    /* Busy protection for session adapter AND jrec adapter separately. */
    const rfc_session_adapter *sadp =
        rfc_registry_acquire_session(RFC_FORMAT_JPEG_XL);
    CHECK(sadp != NULL);
    CHECK(rfc_codecs_unregister_jxl() == RFC_ERR_BUSY);
    rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_JPEG_XL) == sadp);
    CHECK(rfc_registry_jpeg_reconstruct_adapter() != NULL);

    const rfc_jpeg_reconstruct_adapter *jrec =
        rfc_registry_acquire_jpeg_reconstruct();
    CHECK(jrec != NULL);
    CHECK(rfc_codecs_unregister_jxl() == RFC_ERR_BUSY);
    CHECK(rfc_registry_session_adapter(RFC_FORMAT_JPEG_XL) == sadp);
    rfc_registry_release_jpeg_reconstruct();

    CHECK(rfc_codecs_unregister_jxl() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_OK);
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_webp() == RFC_OK);

    printf("jxl: registration_and_upstream_contract ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 2: Depths (1/4/8/10/12/16), padding refusal, alpha, XYB, threads   */
/* ------------------------------------------------------------------------ */

static void test_depths_padding_alpha_and_xyb(void) {
    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_JPEG_XL);
    CHECK(adp != NULL);

    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    static const struct {
        rfc_sample_format fmt;
        rfc_alpha_mode alpha;
        uint32_t bps;
    } cases[] = {
        {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 1u},
        {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 8u},
        {RFC_SAMPLE_GRAY16, RFC_ALPHA_NONE, 10u},
        {RFC_SAMPLE_GRAY16, RFC_ALPHA_NONE, 16u},
        {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u},
        {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u},
        {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 4u},
        {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 8u},
        {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 12u},
        {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 16u},
        {RFC_SAMPLE_RGBA16, RFC_ALPHA_STRAIGHT, 12u},
        {RFC_SAMPLE_RGBA16, RFC_ALPHA_STRAIGHT, 16u},
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rfc_frame src = {0};
        fill_pattern_frame(&src, 13u, 11u, cases[i].fmt, cases[i].alpha,
                           cases[i].bps, NULL, &alloc);

        void *sess = NULL;
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(sess, &exec, &src, 1u, NULL, NULL, 0u,
                                  &enc) == RFC_OK);
        adp->close_session(sess);

        /* Upstream JxlDecoder native value check for 1/4/10/12-bit cases. */
        if (cases[i].bps != 8u && cases[i].bps != 16u) {
            uint32_t S = (cases[i].bps <= 8u) ? 8u : 16u;
            uint32_t shift = S - cases[i].bps;
            uint32_t nch = (cases[i].fmt == RFC_SAMPLE_GRAY8 ||
                            cases[i].fmt == RFC_SAMPLE_GRAY16)
                               ? 1u
                               : ((cases[i].alpha == RFC_ALPHA_STRAIGHT) ? 4u
                                                                         : 3u);
            size_t count = (size_t)13u * 11u * nch;
            uint8_t *nat = (uint8_t *)malloc(src.data_len);
            CHECK(nat != NULL);
            if (S == 8u) {
                for (size_t k = 0u; k < count; k++) {
                    nat[k] = (uint8_t)(src.data[k] >> shift);
                }
            } else {
                for (size_t k = 0u; k < count; k++) {
                    uint16_t v = 0u;
                    memcpy(&v, src.data + k * 2u, 2u);
                    v = (uint16_t)(v >> shift);
                    memcpy(nat + k * 2u, &v, 2u);
                }
            }
            upstream_verify_native_codestream(
                enc.data, enc.len, 13u, 11u, cases[i].bps,
                (cases[i].alpha == RFC_ALPHA_STRAIGHT) ? cases[i].bps : 0u, nat,
                src.data_len);
            free(nat);
        }

        rfc_image_probe pr;
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->probe_session(sess, &exec, enc.data, enc.len, &pr) ==
              RFC_OK);
        CHECK(pr.frame_count == 1u);
        CHECK(pr.first_frame.format == cases[i].fmt &&
              pr.first_frame.alpha == cases[i].alpha &&
              pr.first_frame.bits_per_sample == cases[i].bps);
        rfc_metadata_free(&pr.container_meta, &alloc);

        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dc == 1u && df[0].data_len == src.data_len);
        CHECK(memcmp(df[0].data, src.data, src.data_len) == 0);

        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
        CHECK(st.live == 0u);
    }

    /* Nonzero low padding bit refusals: 4-bit color & alpha, 12-bit color & alpha. */
    {
        rfc_frame bad = {0};
        fill_pattern_frame(&bad, 4u, 4u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u,
                           NULL, &alloc);
        bad.data[0] |= 0x01u;
        void *sess = NULL;
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        rfc_encoded enc = {0};
        CHECK(adp->encode_session(sess, &exec, &bad, 1u, NULL, NULL, 0u,
                                  &enc) == RFC_ERR_UNSUPPORTED_SAMPLE);
        adp->close_session(sess);
        rfc_frame_free(&bad);

        fill_pattern_frame(&bad, 4u, 4u, RFC_SAMPLE_RGBA16, RFC_ALPHA_STRAIGHT,
                           12u, NULL, &alloc);
        bad.data[6] |= 0x01u; /* alpha low padding bit */
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &exec, &bad, 1u, NULL, NULL, 0u,
                                  &enc) == RFC_ERR_UNSUPPORTED_SAMPLE);
        adp->close_session(sess);
        rfc_frame_free(&bad);
        CHECK(st.live == 0u);
    }

    /* Mismatched alpha bit depth in JXL codestream refuses cleanly on decode. */
    {
        uint8_t pix[4 * 4 * 4];
        memset(pix, 0x22, sizeof(pix));
        uint8_t *jbuf = NULL;
        size_t jlen = 0u;
        upstream_encode_jxl(4u, 4u, 4u, 8u, 4u, 1, 1, 0, 0, NULL, 0u, NULL,
                            0u, pix, sizeof(pix), &jbuf, &jlen);
        void *sess = NULL;
        rfc_image_probe pr;
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->probe_session(sess, &exec, jbuf, jlen, &pr) ==
              RFC_ERR_UNSUPPORTED_SAMPLE);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(sess, &exec, jbuf, jlen, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_UNSUPPORTED_SAMPLE);
        adp->close_session(sess);
        free(jbuf);
        CHECK(st.live == 0u);
    }

    /* All-opaque RGBA8 preserves RGBA8 and A==0 retains invisible RGB. */
    {
        rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 4u, 4u, 16u,
                          8u};
        rfc_frame fr = {0};
        CHECK(test_alloc_frame(&fr, &lay, NULL, &alloc) == RFC_OK);
        memset(fr.data, 0xFF, fr.data_len);
        void *sess = NULL;
        rfc_encoded enc = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &exec, &fr, 1u, NULL, NULL, 0u,
                                  &enc) == RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dc == 1u && df[0].layout.format == RFC_SAMPLE_RGBA8 &&
              df[0].layout.alpha == RFC_ALPHA_STRAIGHT &&
              memcmp(df[0].data, fr.data, fr.data_len) == 0);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&fr);
        CHECK(st.live == 0u);
    }

    /* Lossy XYB external JXL decode: structured sRGB, structured Display P3,
     * raw non-sRGB ICC (WantICC=1), plus JXL(XYB Display P3) -> PNG job. */
    {
        uint8_t pix[16 * 16 * 3];
        for (size_t k = 0u; k < sizeof(pix); k++) {
            pix[k] = (uint8_t)((k * 29u) & 255u);
        }
        uint8_t *jbuf = NULL;
        size_t jlen = 0u;
        upstream_encode_jxl(16u, 16u, 3u, 8u, 0u, 0, 0, 0, 0, NULL, 0u, NULL,
                            0u, pix, sizeof(pix), &jbuf, &jlen);
        void *sess = NULL;
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->decode_session(sess, &exec, jbuf, jlen, NULL, 0u, &df, &dc,
                                  &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dc == 1u && df[0].layout.format == RFC_SAMPLE_RGB8 &&
              df[0].layout.width == 16u && df[0].layout.height == 16u &&
              dm.icc == NULL);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        free(jbuf);

        /* Structured Display P3 XYB: pixels and returned ICC must match P3, not sRGB. */
        uint8_t p3_pix[16 * 16 * 3];
        for (size_t k = 0u; k < 16u * 16u; k++) {
            p3_pix[k * 3u + 0u] = 220u;
            p3_pix[k * 3u + 1u] = (uint8_t)(20u + (k % 40u));
            p3_pix[k * 3u + 2u] = 30u;
        }
        JxlColorEncoding srgb_ce, p3_ce;
        JxlColorEncodingSetToSRGB(&srgb_ce, JXL_FALSE);
        memset(&p3_ce, 0, sizeof(p3_ce));
        p3_ce.color_space = JXL_COLOR_SPACE_RGB;
        p3_ce.white_point = JXL_WHITE_POINT_D65;
        p3_ce.primaries = JXL_PRIMARIES_P3;
        p3_ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
        p3_ce.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;

        uint8_t *jbuf_p3 = NULL;
        size_t jlen_p3 = 0u;
        upstream_encode_jxl_color(16u, 16u, &p3_ce, NULL, 0u, 0, 0, NULL, 0u,
                                  NULL, 0u, p3_pix, sizeof(p3_pix), &jbuf_p3,
                                  &jlen_p3);

        uint8_t *exp_p3_pix = NULL, *exp_p3_icc = NULL, *srgb_pix = NULL;
        size_t exp_p3_len = 0u, exp_p3_icc_len = 0u, srgb_len = 0u;
        upstream_decode_xyb_pixels(jbuf_p3, jlen_p3, &p3_ce, NULL, 0u,
                                   &exp_p3_pix, &exp_p3_len, &exp_p3_icc,
                                   &exp_p3_icc_len);
        upstream_decode_xyb_pixels(jbuf_p3, jlen_p3, &srgb_ce, NULL, 0u,
                                   &srgb_pix, &srgb_len, NULL, NULL);
        CHECK(exp_p3_len == sizeof(p3_pix) && srgb_len == sizeof(p3_pix));
        CHECK(memcmp(exp_p3_pix, srgb_pix, sizeof(p3_pix)) != 0);

        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->decode_session(sess, &exec, jbuf_p3, jlen_p3, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dc == 1u && dm.icc != NULL && dm.icc_len == exp_p3_icc_len &&
              memcmp(dm.icc, exp_p3_icc, exp_p3_icc_len) == 0);
        CHECK(df[0].data_len == exp_p3_len &&
              memcmp(df[0].data, exp_p3_pix, exp_p3_len) == 0);
        CHECK(memcmp(df[0].data, srgb_pix, exp_p3_len) != 0);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);

        /* End-to-end JXL(XYB Display P3) -> PNG job preserves P3 pixels + ICC. */
        {
            rfc_scheduler_config scfg;
            rfc_scheduler_config_default(&scfg);
            scfg.max_workers = 1u;
            rfc_scheduler *sched = rfc_scheduler_create(&scfg);
            CHECK(sched != NULL);
            char png_out[512];
            make_temp_path(png_out, sizeof(png_out), "xyb_p3.png");
            remove(png_out);
            rfc_io_sink sink;
            rfc_io_sink_init_path(&sink, png_out, RFC_EXPORT_REFUSE_EXISTING);
            rfc_job_spec sp;
            memset(&sp, 0, sizeof(sp));
            sp.source_format = RFC_FORMAT_JPEG_XL;
            sp.target_format = RFC_FORMAT_PNG;
            sp.source_bytes = jbuf_p3;
            sp.source_size = jlen_p3;
            sp.io_sink = &sink;
            sp.expected_working_set_bytes = 32u * 1024u * 1024u;
            rfc_job *j = NULL;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
            CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
            rfc_job_snapshot sn;
            CHECK(rfc_job_snapshot_get(j, &sn) == RFC_OK);
            CHECK(sn.state == RFC_JOB_SUCCEEDED && sn.status == RFC_OK);
            rfc_verify_report rep;
            CHECK(rfc_job_verify_report_get(j, &rep) == RFC_OK);
            CHECK(rep.valid == 1);
            rfc_job_release(j);
            remove(png_out);
            rfc_scheduler_destroy(sched);
        }

        /* Raw non-sRGB ICC XYB (Branch B: WantICC() == 1). */
        uint8_t *mod_icc = (uint8_t *)malloc(exp_p3_icc_len);
        CHECK(mod_icc != NULL && exp_p3_icc_len >= 132u);
        memcpy(mod_icc, exp_p3_icc, exp_p3_icc_len);
        uint32_t tag_cnt = rd32be(mod_icc + 128u);
        uint32_t bumped_offs[3] = {0u, 0u, 0u};
        size_t bumped_n = 0u;
        for (uint32_t ti = 0u; ti < tag_cnt; ti++) {
            size_t rec = 132u + (size_t)ti * 12u;
            if (rec + 12u > exp_p3_icc_len) break;
            if (memcmp(mod_icc + rec, "cicp", 4u) == 0) {
                uint32_t coff = rd32be(mod_icc + rec + 4u);
                uint32_t csz = rd32be(mod_icc + rec + 8u);
                if (coff + 12u <= exp_p3_icc_len && csz >= 12u) {
                    mod_icc[coff + 9u] = 2u; /* unspecified transfer -> skip CICP shortcut */
                }
            } else if (memcmp(mod_icc + rec, "rTRC", 4u) == 0 ||
                memcmp(mod_icc + rec, "gTRC", 4u) == 0 ||
                memcmp(mod_icc + rec, "bTRC", 4u) == 0) {
                uint32_t toff = rd32be(mod_icc + rec + 4u);
                uint32_t tsz = rd32be(mod_icc + rec + 8u);
                int seen = 0;
                for (size_t bi = 0u; bi < bumped_n; bi++) {
                    if (bumped_offs[bi] == toff) seen = 1;
                }
                if (!seen && toff + 20u <= exp_p3_icc_len && tsz >= 20u &&
                    memcmp(mod_icc + toff, "para", 4u) == 0) {
                    uint32_t v = rd32be(mod_icc + toff + 16u);
                    wr32be(mod_icc + toff + 16u, v + 4096u);
                    if (bumped_n < 3u) bumped_offs[bumped_n++] = toff;
                }
            }
        }
        CHECK(bumped_n > 0u);
        uint8_t *jbuf_raw = NULL;
        size_t jlen_raw = 0u;
        upstream_encode_jxl_color(16u, 16u, NULL, mod_icc, exp_p3_icc_len, 0, 0,
                                  NULL, 0u, NULL, 0u, p3_pix, sizeof(p3_pix),
                                  &jbuf_raw, &jlen_raw);
        {
            /* Confirm upstream TARGET_ORIGINAL has no encoded profile. */
            JxlDecoder *cdec = JxlDecoderCreate(NULL);
            CHECK(cdec != NULL);
            CHECK(JxlDecoderSubscribeEvents(cdec, JXL_DEC_COLOR_ENCODING) ==
                  JXL_DEC_SUCCESS);
            CHECK(JxlDecoderSetInput(cdec, jbuf_raw, jlen_raw) ==
                  JXL_DEC_SUCCESS);
            JxlDecoderCloseInput(cdec);
            int checked_enc = 0;
            for (;;) {
                JxlDecoderStatus ds = JxlDecoderProcessInput(cdec);
                if (ds == JXL_DEC_COLOR_ENCODING) {
                    JxlColorEncoding ce_tmp;
                    CHECK(JxlDecoderGetColorAsEncodedProfile(
                              cdec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                              &ce_tmp) != JXL_DEC_SUCCESS);
                    checked_enc = 1;
                    break;
                } else if (ds == JXL_DEC_SUCCESS || ds == JXL_DEC_ERROR) {
                    break;
                }
            }
            CHECK(checked_enc == 1);
            JxlDecoderDestroy(cdec);
        }
        uint8_t *exp_mod_pix = NULL;
        size_t exp_mod_len = 0u;
        upstream_decode_xyb_pixels(jbuf_raw, jlen_raw, NULL, mod_icc,
                                   exp_p3_icc_len, &exp_mod_pix, &exp_mod_len,
                                   NULL, NULL);
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->decode_session(sess, &exec, jbuf_raw, jlen_raw, NULL, 0u,
                                  &df, &dc, &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dc == 1u && dm.icc != NULL && dm.icc_len == exp_p3_icc_len &&
              memcmp(dm.icc, mod_icc, exp_p3_icc_len) == 0);
        CHECK(df[0].data_len == exp_mod_len &&
              memcmp(df[0].data, exp_mod_pix, exp_mod_len) == 0);
        CHECK(memcmp(df[0].data, srgb_pix, exp_mod_len) != 0);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);

        /* End-to-end JXL(XYB mod_icc) -> PNG job preserves mod_icc + pixels. */
        {
            rfc_scheduler_config mcfg;
            rfc_scheduler_config_default(&mcfg);
            mcfg.max_workers = 1u;
            rfc_scheduler *msched = rfc_scheduler_create(&mcfg);
            CHECK(msched != NULL);
            char mpng[512];
            make_temp_path(mpng, sizeof(mpng), "xyb_mod.png");
            remove(mpng);
            rfc_io_sink msink;
            rfc_io_sink_init_path(&msink, mpng, RFC_EXPORT_REFUSE_EXISTING);
            rfc_job_spec msp;
            memset(&msp, 0, sizeof(msp));
            msp.source_format = RFC_FORMAT_JPEG_XL;
            msp.target_format = RFC_FORMAT_PNG;
            msp.source_bytes = jbuf_raw;
            msp.source_size = jlen_raw;
            msp.io_sink = &msink;
            msp.expected_working_set_bytes = 32u * 1024u * 1024u;
            rfc_job *mj = NULL;
            CHECK(rfc_scheduler_submit(msched, NULL, &msp, &mj) == RFC_OK);
            CHECK(rfc_job_wait(mj, UINT32_MAX) == RFC_OK);
            rfc_job_snapshot msn;
            CHECK(rfc_job_snapshot_get(mj, &msn) == RFC_OK);
            CHECK(msn.state == RFC_JOB_SUCCEEDED && msn.status == RFC_OK);
            rfc_verify_report mrep;
            CHECK(rfc_job_verify_report_get(mj, &mrep) == RFC_OK);
            CHECK(mrep.valid == 1);
            rfc_job_release(mj);
            remove(mpng);
            rfc_scheduler_destroy(msched);
        }

        free(exp_mod_pix);
        free(jbuf_raw);
        free(mod_icc);
        free(exp_p3_pix);
        free(exp_p3_icc);
        free(srgb_pix);
        free(jbuf_p3);

        /* Finding #1: live-pointer OOM fault sweep on metadata clone paths. */
        uint8_t exif_oom[26];
        make_exif_blob(1, 6u, exif_oom);
        static const char k_oom_xmp[] = "<x:xmpmeta>OOM</x:xmpmeta>";
        rfc_metadata oom_m = {k_srgb_icc, sizeof(k_srgb_icc),
                              RFC_ORIENTATION_ROTATE_90, 0u, {0}, exif_oom,
                              sizeof(exif_oom), (const uint8_t *)k_oom_xmp,
                              sizeof(k_oom_xmp) - 1u};
        rfc_frame oom_src = {0};
        fill_pattern_frame(&oom_src, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE,
                           8u, &oom_m, &alloc);
        rfc_encoded oom_jxl = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &exec, &oom_src, 1u, &oom_m, NULL, 0u,
                                  &oom_jxl) == RFC_OK);
        adp->close_session(sess);
        rfc_frame_free(&oom_src);

        oom_alloc_ctx octx;
        memset(&octx, 0, sizeof(octx));
        rfc_allocator oal = {oom_alloc, oom_free, &octx};
        rfc_exec_context oex;
        memset(&oex, 0, sizeof(oex));
        rfc_limits_default(&oex.limits);
        oex.allocator = &oal;
        oex.codec_threads = 1u;

        CHECK(adp->open_session(&oex, &sess) == RFC_OK);
        CHECK(adp->decode_session(sess, &oex, oom_jxl.data, oom_jxl.len, NULL,
                                  0u, &df, &dc, &dm) == RFC_OK);
        adp->close_session(sess);
        size_t total_calls = octx.calls;
        CHECK(total_calls >= 4u && !octx.overflow && octx.invalid_frees == 0u);
        rfc_metadata_free(&dm, &oal);
        rfc_frame_free(&df[0]);
        oal.free(oal.ctx, df);
        CHECK(octx.live == 0u && octx.invalid_frees == 0u);

        for (size_t fc = total_calls; fc >= 1u; fc--) {
            memset(&octx, 0, sizeof(octx));
            octx.fail_at_call = fc;
            void *os = NULL;
            rfc_status ors = adp->open_session(&oex, &os);
            if (ors == RFC_OK) {
                df = NULL;
                dc = 0u;
                memset(&dm, 0, sizeof(dm));
                ors = adp->decode_session(os, &oex, oom_jxl.data, oom_jxl.len,
                                          NULL, 0u, &df, &dc, &dm);
                adp->close_session(os);
            }
            CHECK(ors != RFC_OK);
            if (fc >= total_calls - 2u) {
                CHECK(ors == RFC_ERR_OUT_OF_MEMORY);
            }
            CHECK(!octx.overflow && octx.invalid_frees == 0u &&
                  octx.live == 0u);
        }
        rfc_encoded_free(&oom_jxl);
        CHECK(st.live == 0u);
    }

    /* Thread count 1 vs 4 byte identity + animated/unsupported refusals. */
    {
        rfc_frame src = {0};
        fill_pattern_frame(&src, 64u, 64u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           NULL, &alloc);
        rfc_exec_context ex1, ex4;
        tracked_alloc_state st1, st4;
        rfc_allocator a1, a4;
        init_exec(&ex1, &st1, &a1, NULL, 1u);
        init_exec(&ex4, &st4, &a4, NULL, 4u);
        void *s1 = NULL, *s4 = NULL;
        rfc_encoded e1 = {0}, e4 = {0};
        CHECK(adp->open_session(&ex1, &s1) == RFC_OK);
        CHECK(adp->encode_session(s1, &ex1, &src, 1u, NULL, NULL, 0u, &e1) ==
              RFC_OK);
        adp->close_session(s1);
        CHECK(adp->open_session(&ex4, &s4) == RFC_OK);
        CHECK(adp->encode_session(s4, &ex4, &src, 1u, NULL, NULL, 0u, &e4) ==
              RFC_OK);
        adp->close_session(s4);
        CHECK(e1.len == e4.len && memcmp(e1.data, e4.data, e1.len) == 0);
        rfc_encoded_free(&e1);
        rfc_encoded_free(&e4);
        CHECK(st1.live == 0u && st4.live == 0u);

        rfc_frame two[2] = {src, src};
        CHECK(adp->open_session(&exec, &s1) == RFC_OK);
        CHECK(adp->encode_session(s1, &exec, two, 2u, NULL, NULL, 0u, &e1) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(adp->encode_session(s1, &exec, two, 0u, NULL, NULL, 0u, &e1) ==
              RFC_ERR_INVALID_ARGUMENT);
        adp->close_session(s1);
        rfc_frame_free(&src);

        /* Animated upstream JXL refuses cleanly with 0 allocations. */
        uint8_t pix[8 * 8 * 3];
        memset(pix, 0x55, sizeof(pix));
        uint8_t *abuf = NULL;
        size_t alen = 0u;
        upstream_encode_jxl(8u, 8u, 3u, 8u, 0u, 1, 1, 1, 0, NULL, 0u, NULL, 0u,
                            pix, sizeof(pix), &abuf, &alen);
        rfc_image_probe pr;
        CHECK(adp->open_session(&exec, &s1) == RFC_OK);
        CHECK(adp->probe_session(s1, &exec, abuf, alen, &pr) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        adp->close_session(s1);
        free(abuf);
        CHECK(st.live == 0u);
    }

    rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
    printf("jxl: depths_padding_alpha_and_xyb ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 3: Metadata (ICC, Exif, XMP, brob, native ori) & cross-format      */
/* ------------------------------------------------------------------------ */

static void test_metadata_boxes_orientation_and_cross_format(void) {
    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_JPEG_XL);
    CHECK(adp != NULL);
    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    static const char k_xmp[] =
        "<?xpacket begin=''?><x:xmpmeta>RF-JXL</x:xmpmeta>";
    uint8_t exif_le6[26], exif_be3[26], exif_no_ori[34];
    make_exif_blob(1, 6u, exif_le6);
    make_exif_blob(0, 3u, exif_be3);
    make_exif_without_0112(exif_no_ori);

    /* 1. Full sRGB ICC + LE Exif Ori=6 + XMP preserve round trip + strip. */
    {
        rfc_metadata m = {k_srgb_icc, sizeof(k_srgb_icc), RFC_ORIENTATION_ROTATE_90,
                          0u, {0}, exif_le6, sizeof(exif_le6),
                          (const uint8_t *)k_xmp, sizeof(k_xmp) - 1u};
        rfc_frame src = {0};
        fill_pattern_frame(&src, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           &m, &alloc);
        void *sess = NULL;
        rfc_encoded enc = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &m, NULL, 0u, &enc) ==
              RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        CHECK(dc == 1u && dm.icc_len == sizeof(k_srgb_icc) &&
              memcmp(dm.icc, k_srgb_icc, sizeof(k_srgb_icc)) == 0);
        CHECK(dm.exif_len == sizeof(exif_le6) &&
              memcmp(dm.exif, exif_le6, sizeof(exif_le6)) == 0);
        CHECK(dm.xmp_len == sizeof(k_xmp) - 1u &&
              memcmp(dm.xmp, k_xmp, dm.xmp_len) == 0);
        CHECK(dm.orientation == RFC_ORIENTATION_ROTATE_90);

        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);

        rfc_setting_value strip_s[1] = {{"metadata", "strip"}};
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &m, strip_s, 1u,
                                  &enc) == RFC_OK);
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dc == 1u && dm.icc == NULL && dm.exif == NULL && dm.xmp == NULL &&
              dm.orientation == RFC_ORIENTATION_NORMAL);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
        CHECK(st.live == 0u);
    }

    /* 2. Conflicting EXIF Ori=6 vs meta Ori=1 refuses; EXIF without 0x0112 + Ori=6
     *    and orientation-only Ori=6 without EXIF both preserve cleanly. */
    {
        rfc_metadata conflict = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0},
                                 exif_le6, sizeof(exif_le6), NULL, 0u};
        rfc_frame src = {0};
        fill_pattern_frame(&src, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           &conflict, &alloc);
        void *sess = NULL;
        rfc_encoded enc = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &conflict, NULL, 0u,
                                  &enc) == RFC_ERR_INVALID_ARGUMENT);
        rfc_frame_free(&src);

        rfc_metadata no112 = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                              exif_no_ori, sizeof(exif_no_ori), NULL, 0u};
        fill_pattern_frame(&src, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           &no112, &alloc);
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &no112, NULL, 0u,
                                  &enc) == RFC_OK);
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        CHECK(dm.orientation == RFC_ORIENTATION_ROTATE_90 &&
              dm.exif_len == sizeof(exif_no_ori) &&
              memcmp(dm.exif, exif_no_ori, sizeof(exif_no_ori)) == 0);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&src);

        rfc_metadata ori_only = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                                 NULL, 0u, NULL, 0u};
        fill_pattern_frame(&src, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           &ori_only, &alloc);
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &ori_only, NULL, 0u,
                                  &enc) == RFC_OK);
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dm.orientation == RFC_ORIENTATION_ROTATE_90 && dm.exif == NULL);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
        CHECK(st.live == 0u);
    }

    /* 3. "brob" compressed Exif (non-zero 4-byte BE TIFF offset) + XMP + zip bomb cap. */
    {
        uint8_t pix[8 * 8 * 3];
        memset(pix, 0x44, sizeof(pix));
        uint8_t ex_off[4 + 6 + 26];
        wr32be(ex_off, 6u); /* 6-byte prefix before MM* TIFF */
        memcpy(ex_off + 4u, "PADDING", 6u);
        memcpy(ex_off + 10u, exif_be3, sizeof(exif_be3));

        char big_xmp[1024];
        memset(big_xmp, ' ', sizeof(big_xmp));
        memcpy(big_xmp, "<x:xmpmeta>", 11u);
        memcpy(big_xmp + sizeof(big_xmp) - 12u, "</x:xmpmeta>", 12u);

        uint8_t *jbuf = NULL;
        size_t jlen = 0u;
        upstream_encode_jxl(8u, 8u, 3u, 8u, 0u, 1, 1, 0, 1, ex_off,
                            sizeof(ex_off), (const uint8_t *)big_xmp,
                            sizeof(big_xmp), pix, sizeof(pix), &jbuf, &jlen);

        void *sess = NULL;
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->decode_session(sess, &exec, jbuf, jlen, NULL, 0u, &df, &dc,
                                  &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dm.exif_len == sizeof(exif_be3) &&
              memcmp(dm.exif, exif_be3, sizeof(exif_be3)) == 0);
        CHECK(dm.xmp_len == sizeof(big_xmp) &&
              memcmp(dm.xmp, big_xmp, sizeof(big_xmp)) == 0);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);

        rfc_limits small_meta;
        rfc_limits_default(&small_meta);
        small_meta.max_metadata_bytes = 256u;
        rfc_exec_context mex;
        tracked_alloc_state mst;
        rfc_allocator mal;
        init_exec(&mex, &mst, &mal, &small_meta, 1u);
        CHECK(adp->open_session(&mex, &sess) == RFC_OK);
        CHECK(adp->decode_session(sess, &mex, jbuf, jlen, NULL, 0u, &df, &dc,
                                  &dm) == RFC_ERR_LIMIT_EXCEEDED);
        adp->close_session(sess);
        CHECK(mst.live == 0u);
        free(jbuf);
        CHECK(st.live == 0u);
    }

    /* 3b. Finding #4: "Exif\0\0"-prefixed EXIF encodes offset=6, round-trips
     *     byte-for-byte (plus WebP <-> JXL), and offset=0 "Exif\0\0" rejects. */
    {
        uint8_t prefixed_exif[32];
        memcpy(prefixed_exif, "Exif\x00\x00", 6u);
        memcpy(prefixed_exif + 6u, exif_le6, sizeof(exif_le6));

        rfc_metadata mp = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                           prefixed_exif, sizeof(prefixed_exif), NULL, 0u};
        rfc_frame src = {0};
        fill_pattern_frame(&src, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           &mp, &alloc);
        void *sess = NULL;
        rfc_encoded enc = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &exec, &src, 1u, &mp, NULL, 0u, &enc) ==
              RFC_OK);

        uint8_t *raw_box = NULL;
        size_t raw_box_len = 0u;
        upstream_extract_raw_exif_box(enc.data, enc.len, &raw_box,
                                      &raw_box_len);
        CHECK(raw_box_len == 36u);
        CHECK(rd32be(raw_box) == 6u);
        CHECK(memcmp(raw_box + 4u, prefixed_exif, sizeof(prefixed_exif)) == 0);
        CHECK(memcmp(raw_box + 10u, "II*\0", 4u) == 0);
        free(raw_box);

        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->decode_session(sess, &exec, enc.data, enc.len, NULL, 0u, &df,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sess);
        CHECK(dc == 1u && dm.orientation == RFC_ORIENTATION_ROTATE_90);
        CHECK(dm.exif_len == sizeof(prefixed_exif) &&
              memcmp(dm.exif, prefixed_exif, sizeof(prefixed_exif)) == 0);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);
        rfc_frame_free(&src);

        /* WebP <-> JXL cross-format preserve round-trip with Exif\0\0. */
        {
            rfc_scheduler_config wcfg;
            rfc_scheduler_config_default(&wcfg);
            wcfg.max_workers = 1u;
            rfc_scheduler *wsched = rfc_scheduler_create(&wcfg);
            CHECK(wsched != NULL);
            char wp[512], j2p[512];
            make_temp_path(wp, sizeof(wp), "exif6_w.webp");
            make_temp_path(j2p, sizeof(j2p), "exif6_j.jxl");
            remove(wp);
            remove(j2p);

            rfc_io_sink wsink, jsink;
            rfc_io_sink_init_path(&wsink, wp, RFC_EXPORT_REFUSE_EXISTING);
            rfc_job_spec sp;
            memset(&sp, 0, sizeof(sp));
            sp.source_format = RFC_FORMAT_JPEG_XL;
            sp.target_format = RFC_FORMAT_WEBP;
            sp.source_bytes = enc.data;
            sp.source_size = enc.len;
            sp.io_sink = &wsink;
            sp.expected_working_set_bytes = 32u * 1024u * 1024u;
            rfc_job *job = NULL;
            CHECK(rfc_scheduler_submit(wsched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            rfc_job_snapshot sn;
            CHECK(rfc_job_snapshot_get(job, &sn) == RFC_OK);
            CHECK(sn.state == RFC_JOB_SUCCEEDED && sn.status == RFC_OK);
            rfc_job_release(job);

            size_t wlen = 0u;
            uint8_t *wbytes = read_file_all(wp, &wlen);
            CHECK(wbytes != NULL && wlen > 0u);
            rfc_io_sink_init_path(&jsink, j2p, RFC_EXPORT_REFUSE_EXISTING);
            memset(&sp, 0, sizeof(sp));
            sp.source_format = RFC_FORMAT_WEBP;
            sp.target_format = RFC_FORMAT_JPEG_XL;
            sp.source_bytes = wbytes;
            sp.source_size = wlen;
            sp.io_sink = &jsink;
            sp.expected_working_set_bytes = 32u * 1024u * 1024u;
            CHECK(rfc_scheduler_submit(wsched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &sn) == RFC_OK);
            CHECK(sn.state == RFC_JOB_SUCCEEDED && sn.status == RFC_OK);
            rfc_job_release(job);

            remove(wp);
            remove(j2p);
            free(wbytes);
            rfc_scheduler_destroy(wsched);
        }
        rfc_encoded_free(&enc);

        /* Malformed Exif box with offset=0 pointing to "Exif\0\0" instead of TIFF
         * magic must be rejected by both probe and decode. */
        uint8_t bad_pix[8 * 8 * 3];
        memset(bad_pix, 0x33, sizeof(bad_pix));
        uint8_t bad_box[36];
        wr32be(bad_box, 0u);
        memcpy(bad_box + 4u, prefixed_exif, sizeof(prefixed_exif));
        uint8_t *bad_jxl = NULL;
        size_t bad_jlen = 0u;
        upstream_encode_jxl(8u, 8u, 3u, 8u, 0u, 1, 1, 0, 0, bad_box,
                            sizeof(bad_box), NULL, 0u, bad_pix, sizeof(bad_pix),
                            &bad_jxl, &bad_jlen);
        rfc_image_probe bad_pr = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->probe_session(sess, &exec, bad_jxl, bad_jlen, &bad_pr) ==
              RFC_ERR_INVALID_ARGUMENT);
        CHECK(adp->decode_session(sess, &exec, bad_jxl, bad_jlen, NULL, 0u, &df,
                                  &dc, &dm) == RFC_ERR_INVALID_ARGUMENT);
        adp->close_session(sess);
        free(bad_jxl);
        CHECK(st.live == 0u);
    }

    /* 4. Cross-format PNG(gAMA) -> JXL policy, WebP <-> JXL, JXL -> BMP/FOLD. */
    {
        rfc_scheduler_config scfg;
        rfc_scheduler_config_default(&scfg);
        scfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&scfg);
        CHECK(sched != NULL);

        uint8_t png_gama[512];
        size_t png_len = build_png_with_gama(png_gama, sizeof(png_gama));

        char jxl_path[512];
        make_temp_path(jxl_path, sizeof(jxl_path), "cross_out.jxl");
        rfc_io_sink sink;
        rfc_io_sink_init_path(&sink, jxl_path, RFC_EXPORT_REFUSE_EXISTING);
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = RFC_FORMAT_PNG;
        sp.target_format = RFC_FORMAT_JPEG_XL;
        sp.source_bytes = png_gama;
        sp.source_size = png_len;
        sp.io_sink = &sink;
        sp.expected_working_set_bytes = 32u * 1024u * 1024u;

        rfc_job *j = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot sn;
        CHECK(rfc_job_snapshot_get(j, &sn) == RFC_OK);
        CHECK(sn.status == RFC_ERR_METADATA_POLICY);
        rfc_job_release(j);

        sp.acknowledged_unsupported_metadata = RFC_META_ITEM_ICC;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_SUCCEEDED);
        rfc_job_release(j);

        /* Verify JXL -> WebP (synthesizing canonical TIFF) and JXL -> RASTERFOLD
         * metadata policy enforcement + ack. */
        size_t jlen = 0u;
        uint8_t *jbytes = read_file_all(jxl_path, &jlen);
        CHECK(jbytes != NULL && jlen > 0u);
        remove(jxl_path);
        free(jbytes);

        char webp_path[512];
        make_temp_path(webp_path, sizeof(webp_path), "jxl_to_webp.webp");
        char fold_path[512];
        make_temp_path(fold_path, sizeof(fold_path), "jxl_to_fold.fold");
        rfc_metadata m_ori = {NULL, 0u, RFC_ORIENTATION_ROTATE_90, 0u, {0},
                              NULL, 0u, NULL, 0u};
        rfc_frame f_ori = {0};
        fill_pattern_frame(&f_ori, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           &m_ori, &alloc);
        void *sess = NULL;
        rfc_encoded jxl_ori = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &exec, &f_ori, 1u, &m_ori, NULL, 0u,
                                  &jxl_ori) == RFC_OK);
        adp->close_session(sess);
        rfc_frame_free(&f_ori);

        rfc_io_sink_init_path(&sink, webp_path, RFC_EXPORT_REFUSE_EXISTING);
        memset(&sp, 0, sizeof(sp));
        sp.source_format = RFC_FORMAT_JPEG_XL;
        sp.target_format = RFC_FORMAT_WEBP;
        sp.source_bytes = jxl_ori.data;
        sp.source_size = jxl_ori.len;
        sp.io_sink = &sink;
        sp.expected_working_set_bytes = 32u * 1024u * 1024u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_SUCCEEDED);
        rfc_job_release(j);

        rfc_io_sink_init_path(&sink, fold_path, RFC_EXPORT_REFUSE_EXISTING);
        sp.target_format = RFC_FORMAT_RASTERFOLD;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j, &sn) == RFC_OK);
        CHECK(sn.status == RFC_ERR_METADATA_POLICY);
        rfc_job_release(j);

        sp.acknowledged_unsupported_metadata = RFC_META_ITEM_ORIENTATION;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_SUCCEEDED);
        rfc_job_release(j);

        remove(webp_path);
        remove(fold_path);
        rfc_encoded_free(&jxl_ori);
        rfc_scheduler_destroy(sched);
        CHECK(st.live == 0u);
    }

    rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
    printf("jxl: metadata_boxes_orientation_and_cross_format ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 4: JPEG -> JPEG XL original-byte-reconstruction & preflight checks */
/* ------------------------------------------------------------------------ */

static size_t build_augmented_jpeg(const uint8_t *base_jpeg,
                                   size_t base_len,
                                   const uint8_t *tiff_exif,
                                   size_t exif_len,
                                   const uint8_t *xmp,
                                   size_t xmp_len,
                                   const uint8_t *icc,
                                   size_t icc_len,
                                   uint8_t *out,
                                   size_t cap) {
    CHECK(base_len >= 4u && base_jpeg[0] == 0xFFu && base_jpeg[1] == 0xD8u);
    size_t pos = 0u;
    CHECK(cap >= 2u);
    out[pos++] = 0xFFu;
    out[pos++] = 0xD8u;

    if (tiff_exif && exif_len > 0u) {
        size_t sl = 2u + 6u + exif_len;
        CHECK(pos + 2u + sl <= cap);
        out[pos++] = 0xFFu;
        out[pos++] = 0xE1u;
        wr16be(out + pos, (uint16_t)sl);
        memcpy(out + pos + 2u, "Exif\x00\x00", 6u);
        memcpy(out + pos + 8u, tiff_exif, exif_len);
        pos += sl;
    }
    if (xmp && xmp_len > 0u) {
        size_t sl = 2u + 29u + xmp_len;
        CHECK(pos + 2u + sl <= cap);
        out[pos++] = 0xFFu;
        out[pos++] = 0xE1u;
        wr16be(out + pos, (uint16_t)sl);
        memcpy(out + pos + 2u, "http://ns.adobe.com/xap/1.0/\x00", 29u);
        memcpy(out + pos + 31u, xmp, xmp_len);
        pos += sl;
    }
    if (icc && icc_len > 10u) {
        /* Split across 2 APP2 chunks to exercise multi-chunk ICC assembly. */
        size_t c1 = icc_len / 2u;
        size_t c2 = icc_len - c1;
        size_t sl1 = 2u + 14u + c1;
        size_t sl2 = 2u + 14u + c2;
        CHECK(pos + 4u + sl1 + sl2 <= cap);
        out[pos++] = 0xFFu;
        out[pos++] = 0xE2u;
        wr16be(out + pos, (uint16_t)sl1);
        memcpy(out + pos + 2u, "ICC_PROFILE\x00", 12u);
        out[pos + 14u] = 1u;
        out[pos + 15u] = 2u;
        memcpy(out + pos + 16u, icc, c1);
        pos += sl1;

        out[pos++] = 0xFFu;
        out[pos++] = 0xE2u;
        wr16be(out + pos, (uint16_t)sl2);
        memcpy(out + pos + 2u, "ICC_PROFILE\x00", 12u);
        out[pos + 14u] = 2u;
        out[pos + 15u] = 2u;
        memcpy(out + pos + 16u, icc + c1, c2);
        pos += sl2;
    }

    size_t tail = base_len - 2u;
    CHECK(pos + tail <= cap);
    memcpy(out + pos, base_jpeg + 2u, tail);
    pos += tail;
    return pos;
}

static void test_jpeg_reconstruction_and_exactness(void) {
    const rfc_session_adapter *sadp =
        rfc_registry_acquire_session(RFC_FORMAT_JPEG_XL);
    const rfc_jpeg_reconstruct_adapter *jrec =
        rfc_registry_acquire_jpeg_reconstruct();
    CHECK(sadp != NULL && jrec != NULL);

    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    /* 1. Preflight refusals at zero allocations. */
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    rfc_scheduler_stats s_before, s_after;
    rfc_scheduler_stats_get(sched, &s_before);

    rfc_layout lay16 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 16u, 16u, 48u, 8u};
    rfc_setting_value sv_coeff[1] = {{"exactness", "coefficient-exact"}};
    rfc_setting_value sv_recon[1] = {
        {"exactness", "original-byte-reconstruction"}};
    rfc_setting_value sv_recon_strip[2] = {
        {"exactness", "original-byte-reconstruction"}, {"metadata", "strip"}};

    rfc_job_spec bad;
    memset(&bad, 0, sizeof(bad));
    bad.source_format = RFC_FORMAT_JPEG;
    bad.target_format = RFC_FORMAT_JPEG_XL;
    bad.source_bytes = k_jpeg_baseline_16x16;
    bad.source_size = sizeof(k_jpeg_baseline_16x16);
    bad.source_layout = &lay16;
    bad.settings = sv_coeff;
    bad.setting_count = 1u;
    rfc_job *jb = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &bad, &jb) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(jb == NULL);

    bad.source_format = RFC_FORMAT_JPEG_XL;
    bad.target_format = RFC_FORMAT_JPEG;
    CHECK(rfc_scheduler_submit(sched, NULL, &bad, &jb) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(jb == NULL);

    bad.source_format = RFC_FORMAT_PNG;
    bad.target_format = RFC_FORMAT_JPEG_XL;
    bad.settings = sv_recon;
    CHECK(rfc_scheduler_submit(sched, NULL, &bad, &jb) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(jb == NULL);

    bad.source_format = RFC_FORMAT_JPEG;
    bad.settings = sv_recon_strip;
    bad.setting_count = 2u;
    CHECK(rfc_scheduler_submit(sched, NULL, &bad, &jb) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(jb == NULL);

    rfc_scheduler_stats_get(sched, &s_after);
    CHECK(s_after.submission_alloc_count == s_before.submission_alloc_count);

    /* 2. Baseline, progressive, gray, and APP1+APP2 augmented JPEGs through
     *    full end-to-end layoutless scheduler jobs AND direct entry points. */
    uint8_t aug_base[4096], aug_prog[4096];
    uint8_t exif_le6[26];
    make_exif_blob(1, 6u, exif_le6);
    static const char k_xmp[] =
        "<?xpacket begin=''?><x:xmpmeta>JPEG-RECON</x:xmpmeta>";
    size_t aug_base_len = build_augmented_jpeg(
        k_jpeg_baseline_16x16, sizeof(k_jpeg_baseline_16x16), exif_le6,
        sizeof(exif_le6), (const uint8_t *)k_xmp, sizeof(k_xmp) - 1u,
        k_srgb_icc, sizeof(k_srgb_icc), aug_base, sizeof(aug_base));
    size_t aug_prog_len = build_augmented_jpeg(
        k_jpeg_prog_16x16, sizeof(k_jpeg_prog_16x16), exif_le6,
        sizeof(exif_le6), (const uint8_t *)k_xmp, sizeof(k_xmp) - 1u,
        k_srgb_icc, sizeof(k_srgb_icc), aug_prog, sizeof(aug_prog));

    struct {
        const uint8_t *bytes;
        size_t len;
        int expect_meta;
        rfc_sample_format exp_fmt;
    } jpegs[4] = {
        {k_jpeg_baseline_16x16, sizeof(k_jpeg_baseline_16x16), 0,
         RFC_SAMPLE_RGB8},
        {k_jpeg_prog_16x16, sizeof(k_jpeg_prog_16x16), 0, RFC_SAMPLE_RGB8},
        {k_jpeg_gray_16x16, sizeof(k_jpeg_gray_16x16), 0, RFC_SAMPLE_GRAY8},
        {aug_base, aug_base_len, 1, RFC_SAMPLE_RGB8},
    };
    (void)aug_prog_len;

    for (size_t i = 0u; i < 4u; i++) {
        char jxl_out[512];
        make_temp_path(jxl_out, sizeof(jxl_out), "jrec.jxl");
        rfc_io_sink sink;
        rfc_io_sink_init_path(&sink, jxl_out, RFC_EXPORT_REFUSE_EXISTING);

        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = RFC_FORMAT_JPEG;
        sp.target_format = RFC_FORMAT_JPEG_XL;
        sp.source_bytes = jpegs[i].bytes;
        sp.source_size = jpegs[i].len;
        /* source_layout == NULL and expected_working_set_bytes == 0 tests
         * layoutless JPEG -> JXL admission as well! */
        sp.settings = sv_recon;
        sp.setting_count = 1u;
        sp.io_sink = &sink;

        rfc_job *j = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot sn;
        CHECK(rfc_job_snapshot_get(j, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_SUCCEEDED && sn.status == RFC_OK);
        rfc_verify_report rep;
        CHECK(rfc_job_verify_report_get(j, &rep) == RFC_OK);
        CHECK(rep.valid == 1 &&
              rep.exactness == RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION &&
              memcmp(rep.source_hash, rep.verified_hash, 32u) == 0);
        rfc_job_release(j);

        /* Verify emitted .jxl decodes to pixels AND reconstructs original JPEG. */
        size_t em_len = 0u;
        uint8_t *em = read_file_all(jxl_out, &em_len);
        CHECK(em != NULL && em_len > 0u);
        remove(jxl_out);

        void *ss = NULL;
        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(sadp->open_session(&exec, &ss) == RFC_OK);
        CHECK(sadp->decode_session(ss, &exec, em, em_len, NULL, 0u, &df, &dc,
                                   &dm) == RFC_OK);
        sadp->close_session(ss);
        CHECK(dc == 1u && df[0].layout.width == 16u &&
              df[0].layout.height == 16u &&
              df[0].layout.format == jpegs[i].exp_fmt);
        if (jpegs[i].expect_meta) {
            CHECK(dm.icc_len == sizeof(k_srgb_icc) &&
                  memcmp(dm.icc, k_srgb_icc, sizeof(k_srgb_icc)) == 0);
            CHECK(dm.exif_len == sizeof(exif_le6) &&
                  memcmp(dm.exif, exif_le6, sizeof(exif_le6)) == 0);
            CHECK(dm.xmp_len == sizeof(k_xmp) - 1u &&
                  memcmp(dm.xmp, k_xmp, dm.xmp_len) == 0);
        }
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&df[0]);
        alloc.free(alloc.ctx, df);

        rfc_encoded rec_jpg = {0};
        rfc_metadata rec_m = {0};
        CHECK(rfc_jxl_reconstruct_jpeg(&exec, em, em_len, &rec_jpg, &rec_m) ==
              RFC_OK);
        CHECK(rec_jpg.len == jpegs[i].len &&
              memcmp(rec_jpg.data, jpegs[i].bytes, jpegs[i].len) == 0);
        rfc_encoded_free(&rec_jpg);
        rfc_metadata_free(&rec_m, &alloc);
        free(em);
        CHECK(st.live == 0u);
    }

    /* Progressive APP1+APP2 JPEG direct rfc_jxl_transcode_jpeg + reconstruct. */
    {
        rfc_encoded jxl_p = {0};
        rfc_metadata src_m = {0};
        CHECK(rfc_jxl_transcode_jpeg(&exec, aug_prog, aug_prog_len, sv_recon,
                                     1u, &jxl_p, &src_m) == RFC_OK);
        rfc_encoded rec_p = {0};
        rfc_metadata dst_m = {0};
        CHECK(rfc_jxl_reconstruct_jpeg(&exec, jxl_p.data, jxl_p.len, &rec_p,
                                       &dst_m) == RFC_OK);
        CHECK(rec_p.len == aug_prog_len &&
              memcmp(rec_p.data, aug_prog, aug_prog_len) == 0);
        rfc_encoded_free(&jxl_p);
        rfc_encoded_free(&rec_p);
        rfc_metadata_free(&src_m, &alloc);
        rfc_metadata_free(&dst_m, &alloc);
        CHECK(st.live == 0u);
    }

    /* Pixel-encoded JXL without jbrd refuses JPEG reconstruction loudly. */
    {
        rfc_frame src = {0};
        fill_pattern_frame(&src, 8u, 8u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           NULL, &alloc);
        void *ss = NULL;
        rfc_encoded enc = {0};
        CHECK(sadp->open_session(&exec, &ss) == RFC_OK);
        CHECK(sadp->encode_session(ss, &exec, &src, 1u, NULL, NULL, 0u,
                                   &enc) == RFC_OK);
        sadp->close_session(ss);
        rfc_frame_free(&src);

        rfc_encoded rec_jpg = {0};
        rfc_metadata rec_m = {0};
        CHECK(rfc_jxl_reconstruct_jpeg(&exec, enc.data, enc.len, &rec_jpg,
                                       &rec_m) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        rfc_encoded_free(&enc);
        CHECK(st.live == 0u);
    }

    /* Finding #2: jxl_jpeg_reconstruct enforces max_width/height/pixels/decoded
     * and aggregate max_metadata_bytes while keeping coded orientation dims. */
    {
        rfc_encoded jxl_aug = {0};
        rfc_metadata aug_sm = {0};
        CHECK(rfc_jxl_transcode_jpeg(&exec, aug_base, aug_base_len, sv_recon,
                                     1u, &jxl_aug, &aug_sm) == RFC_OK);
        rfc_metadata_free(&aug_sm, &alloc);

        rfc_limits tight_w, tight_h, tight_px, tight_dec, tight_agg;
        rfc_limits_default(&tight_w);
        tight_w.max_width = 12u;
        rfc_limits_default(&tight_h);
        tight_h.max_height = 12u;
        rfc_limits_default(&tight_px);
        tight_px.max_pixels = 200u;
        rfc_limits_default(&tight_dec);
        tight_dec.max_decoded_bytes = 500u;
        rfc_limits_default(&tight_agg);
        /* Individual items in aug_base: ICC=588B, Exif=26B, XMP=51B (all < 600),
         * while aggregate = 665B > 600B. */
        tight_agg.max_metadata_bytes = 600u;

        const rfc_limits *cases[5] = {&tight_w, &tight_h, &tight_px, &tight_dec,
                                      &tight_agg};
        for (size_t ci = 0u; ci < 5u; ci++) {
            rfc_exec_context lex;
            tracked_alloc_state lst;
            rfc_allocator lal;
            init_exec(&lex, &lst, &lal, cases[ci], 1u);
            rfc_encoded rec = {0};
            rfc_metadata rm = {0};
            CHECK(rfc_jxl_reconstruct_jpeg(&lex, jxl_aug.data, jxl_aug.len,
                                           &rec, &rm) == RFC_ERR_LIMIT_EXCEEDED);
            CHECK(lst.live == 0u);
        }

        /* Scheduler JPEG -> JPEG XL with aggregate metadata cap fails cleanly. */
        {
            char agg_path[512];
            make_temp_path(agg_path, sizeof(agg_path), "agg_lim.jxl");
            remove(agg_path);
            rfc_io_sink asink;
            rfc_io_sink_init_path(&asink, agg_path, RFC_EXPORT_REFUSE_EXISTING);
            rfc_job_spec asp;
            memset(&asp, 0, sizeof(asp));
            asp.source_format = RFC_FORMAT_JPEG;
            asp.target_format = RFC_FORMAT_JPEG_XL;
            asp.source_bytes = aug_base;
            asp.source_size = aug_base_len;
            asp.settings = sv_recon;
            asp.setting_count = 1u;
            asp.limits_override = &tight_agg;
            asp.io_sink = &asink;
            rfc_job *aj = NULL;
            CHECK(rfc_scheduler_submit(sched, NULL, &asp, &aj) == RFC_OK);
            CHECK(rfc_job_wait(aj, UINT32_MAX) == RFC_OK);
            rfc_job_snapshot asn;
            CHECK(rfc_job_snapshot_get(aj, &asn) == RFC_OK);
            CHECK(asn.state == RFC_JOB_FAILED &&
                  asn.status == RFC_ERR_LIMIT_EXCEEDED &&
                  asn.budget.current_allocated_bytes == 0u);
            FILE *af = fopen(agg_path, "rb");
            CHECK(af == NULL);
            rfc_job_release(aj);
        }
        rfc_encoded_free(&jxl_aug);

        /* Non-square 16x8 JPEG with Ori=6 succeeds at exact max_width=16, max_height=8. */
        uint8_t rect_aug[1024];
        size_t rect_len = build_augmented_jpeg(
            k_jpeg_baseline_16x8, sizeof(k_jpeg_baseline_16x8), exif_le6,
            sizeof(exif_le6), NULL, 0u, NULL, 0u, rect_aug, sizeof(rect_aug));
        rfc_encoded jxl_rect = {0};
        rfc_metadata rsrc_m = {0};
        CHECK(rfc_jxl_transcode_jpeg(&exec, rect_aug, rect_len, sv_recon, 1u,
                                     &jxl_rect, &rsrc_m) == RFC_OK);
        rfc_metadata_free(&rsrc_m, &alloc);

        rfc_limits exact_rect;
        rfc_limits_default(&exact_rect);
        exact_rect.max_width = 16u;
        exact_rect.max_height = 8u;
        rfc_exec_context rex;
        tracked_alloc_state rst;
        rfc_allocator ral;
        init_exec(&rex, &rst, &ral, &exact_rect, 1u);
        rfc_encoded rec_rect = {0};
        rfc_metadata rdst_m = {0};
        CHECK(rfc_jxl_reconstruct_jpeg(&rex, jxl_rect.data, jxl_rect.len,
                                       &rec_rect, &rdst_m) == RFC_OK);
        CHECK(rec_rect.len == rect_len &&
              memcmp(rec_rect.data, rect_aug, rect_len) == 0);
        rfc_encoded_free(&rec_rect);
        rfc_metadata_free(&rdst_m, &ral);
        rfc_encoded_free(&jxl_rect);
        CHECK(rst.live == 0u && st.live == 0u);
    }

    /* Finding #5: 255-chunk JPEG APP2 ICC profile boundary does not wrap. */
    {
        /* 255 chunks covering 588B k_srgb_icc: 177 chunks * 2B + 78 chunks * 3B. */
        size_t jpg255_cap =
            sizeof(k_jpeg_baseline_16x16) + 255u * 18u + sizeof(k_srgb_icc);
        uint8_t *jpg255 = (uint8_t *)malloc(jpg255_cap);
        CHECK(jpg255 != NULL);
        size_t wp = 0u;
        jpg255[wp++] = 0xFFu;
        jpg255[wp++] = 0xD8u;
        size_t icc_off = 0u;
        for (uint32_t seq = 1u; seq <= 255u; seq++) {
            size_t clen = (seq <= 177u) ? 2u : 3u;
            size_t sl = 2u + 14u + clen;
            jpg255[wp++] = 0xFFu;
            jpg255[wp++] = 0xE2u;
            wr16be(jpg255 + wp, (uint16_t)sl);
            memcpy(jpg255 + wp + 2u, "ICC_PROFILE\x00", 12u);
            jpg255[wp + 14u] = (uint8_t)seq;
            jpg255[wp + 15u] = 255u;
            memcpy(jpg255 + wp + 16u, k_srgb_icc + icc_off, clen);
            wp += sl;
            icc_off += clen;
        }
        CHECK(icc_off == sizeof(k_srgb_icc));
        memcpy(jpg255 + wp, k_jpeg_baseline_16x16 + 2u,
               sizeof(k_jpeg_baseline_16x16) - 2u);
        size_t jpg255_len = wp + sizeof(k_jpeg_baseline_16x16) - 2u;

        void *jsess = NULL;
        CHECK(jrec->open_session(&exec, &jsess) == RFC_OK);
        rfc_image_probe jpr = {0};
        CHECK(jrec->probe_jpeg(jsess, &exec, jpg255, jpg255_len, &jpr) ==
              RFC_OK);
        CHECK(jpr.container_meta.icc_len == sizeof(k_srgb_icc) &&
              memcmp(jpr.container_meta.icc, k_srgb_icc, sizeof(k_srgb_icc)) ==
                  0);
        rfc_metadata_free(&jpr.container_meta, &alloc);

        rfc_encoded jxl255 = {0};
        rfc_metadata sm255 = {0};
        CHECK(jrec->transcode_jpeg_to_jxl(jsess, &exec, jpg255, jpg255_len,
                                          sv_recon, 1u, &jxl255, &sm255) ==
              RFC_OK);
        CHECK(sm255.icc_len == sizeof(k_srgb_icc) &&
              memcmp(sm255.icc, k_srgb_icc, sizeof(k_srgb_icc)) == 0);
        rfc_metadata_free(&sm255, &alloc);

        rfc_encoded rec255 = {0};
        rfc_metadata dm255 = {0};
        CHECK(jrec->reconstruct_jpeg_from_jxl(jsess, &exec, jxl255.data,
                                              jxl255.len, &rec255, &dm255) ==
              RFC_OK);
        jrec->close_session(jsess);
        CHECK(dm255.icc_len == sizeof(k_srgb_icc) &&
              memcmp(dm255.icc, k_srgb_icc, sizeof(k_srgb_icc)) == 0);
        CHECK(rec255.len == jpg255_len &&
              memcmp(rec255.data, jpg255, jpg255_len) == 0);
        rfc_metadata_free(&dm255, &alloc);
        rfc_encoded_free(&rec255);
        rfc_encoded_free(&jxl255);
        free(jpg255);
        CHECK(st.live == 0u);
    }

    rfc_scheduler_destroy(sched);
    rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
    rfc_registry_release_jpeg_reconstruct();
    printf("jxl: jpeg_reconstruction_and_exactness ok\n");
}

/* ------------------------------------------------------------------------ */
/* Suite 5: Cancellation (encode/transcode/reconstruct), limits, budget     */
/* ------------------------------------------------------------------------ */

static void sleep_ms_port(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
#endif
}

typedef struct cancel_thr_arg {
    const void *adapter;
    int is_jrec;
    void *session;
    volatile int cancel_issued;
    volatile int unreg_was_busy;
} cancel_thr_arg;

#ifdef _WIN32
static DWORD WINAPI cancel_thread_fn(LPVOID p)
#else
static void *cancel_thread_fn(void *p)
#endif
{
    cancel_thr_arg *a = (cancel_thr_arg *)p;
    sleep_ms_port(1u);
    if (a->is_jrec) {
        if (rfc_registry_unregister_jpeg_reconstruct() == RFC_ERR_BUSY) {
            a->unreg_was_busy = 1;
        }
        ((const rfc_jpeg_reconstruct_adapter *)a->adapter)
            ->cancel_session(a->session);
    } else {
        if (rfc_registry_unregister_session(RFC_FORMAT_JPEG_XL) ==
            RFC_ERR_BUSY) {
            a->unreg_was_busy = 1;
        }
        ((const rfc_session_adapter *)a->adapter)->cancel_session(a->session);
    }
    a->cancel_issued = 1;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void run_cancel_thread_and_join(cancel_thr_arg *arg, void (*fn)(cancel_thr_arg *)) {
    (void)fn;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0u, cancel_thread_fn, arg, 0u, NULL);
    CHECK(th != NULL);
    fn(arg);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#else
    pthread_t th;
    CHECK(pthread_create(&th, NULL, cancel_thread_fn, arg) == 0);
    fn(arg);
    CHECK(pthread_join(th, NULL) == 0);
#endif
}

typedef struct direct_cancel_box {
    cancel_thr_arg base;
    const rfc_exec_context *exec;
    const rfc_frame *frame;
    const uint8_t *in_bytes;
    size_t in_len;
    rfc_status out_st;
} direct_cancel_box;

static void run_enc_cancel(cancel_thr_arg *raw) {
    direct_cancel_box *b = (direct_cancel_box *)raw;
    const rfc_session_adapter *adp = (const rfc_session_adapter *)raw->adapter;
    rfc_encoded enc = {0};
    b->out_st = adp->encode_session(raw->session, b->exec, b->frame, 1u, NULL,
                                    NULL, 0u, &enc);
    rfc_encoded_free(&enc);
}

static rfc_status cancel_immediately_stage(rfc_job *job,
                                           const rfc_exec_context *exec,
                                           void *user_ctx) {
    (void)exec;
    (void)user_ctx;
    CHECK(rfc_job_cancel(job) == RFC_OK);
    return RFC_ERR_CANCELLED;
}

static void test_cancellation_limits_budget_and_file_safety(void) {
    const rfc_session_adapter *adp =
        rfc_registry_acquire_session(RFC_FORMAT_JPEG_XL);
    const rfc_jpeg_reconstruct_adapter *jrec =
        rfc_registry_acquire_jpeg_reconstruct();
    CHECK(adp != NULL && jrec != NULL);

    rfc_exec_context exec;
    tracked_alloc_state st;
    rfc_allocator alloc;
    init_exec(&exec, &st, &alloc, NULL, 1u);

    /* 1. Malformed & truncated JXL preflight checks (0 allocations). */
    {
        const uint8_t trunc1[1] = {0xFFu};
        const uint8_t trunc_sig[6] = {0x00u, 0x00u, 0x00u, 0x0Cu, 'J', 'X'};
        const uint8_t bad_ftyp[28] = {
            0, 0, 0, 12, 'J', 'X', 'L', ' ', 0x0Du, 0x0Au, 0x87u, 0x0Au,
            0, 0, 0, 16, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f', 0, 0, 0, 0};
        const uint8_t bad_magic[4] = {0x11u, 0x22u, 0x33u, 0x44u};
        void *sess = NULL;
        rfc_image_probe pr;
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->probe_session(sess, &exec, trunc1, sizeof(trunc1), &pr) ==
              RFC_ERR_TRUNCATED_INPUT);
        CHECK(adp->probe_session(sess, &exec, trunc_sig, sizeof(trunc_sig),
                                 &pr) == RFC_ERR_TRUNCATED_INPUT);
        CHECK(adp->probe_session(sess, &exec, bad_ftyp, sizeof(bad_ftyp),
                                 &pr) == RFC_ERR_INVALID_ARGUMENT);
        CHECK(adp->probe_session(sess, &exec, bad_magic, sizeof(bad_magic),
                                 &pr) == RFC_ERR_INVALID_ARGUMENT);
        adp->close_session(sess);
        CHECK(st.live == 0u);
    }

    /* 2. Tight rfc_budget OOM on encode, decode, transcode, and reconstruct. */
    {
        rfc_frame src = {0};
        fill_pattern_frame(&src, 64u, 64u, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u,
                           NULL, &alloc);
        void *sess = NULL;
        rfc_encoded valid_jxl = {0};
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &exec, &src, 1u, NULL, NULL, 0u,
                                  &valid_jxl) == RFC_OK);
        adp->close_session(sess);

        rfc_budget *tiny = rfc_budget_create(2048u);
        CHECK(tiny != NULL);
        rfc_allocator ta;
        rfc_budget_allocator(tiny, &ta);
        rfc_exec_context tex;
        memset(&tex, 0, sizeof(tex));
        rfc_limits_default(&tex.limits);
        tex.codec_threads = 1u;
        tex.allocator = &ta;
        tex.budget = tiny;

        rfc_encoded oom_enc = {0};
        CHECK(adp->open_session(&tex, &sess) == RFC_OK);
        CHECK(adp->encode_session(sess, &tex, &src, 1u, NULL, NULL, 0u,
                                  &oom_enc) == RFC_ERR_OUT_OF_MEMORY);
        adp->close_session(sess);
        rfc_budget_stats bs;
        rfc_budget_stats_get(tiny, &bs);
        CHECK(bs.rejected_count > 0u && bs.current_allocated_bytes == 0u);

        rfc_frame *df = NULL;
        size_t dc = 0u;
        rfc_metadata dm = {0};
        CHECK(adp->open_session(&tex, &sess) == RFC_OK);
        CHECK(adp->decode_session(sess, &tex, valid_jxl.data, valid_jxl.len,
                                  NULL, 0u, &df, &dc, &dm) ==
              RFC_ERR_OUT_OF_MEMORY);
        adp->close_session(sess);
        rfc_budget_stats_get(tiny, &bs);
        CHECK(bs.rejected_count > 0u && bs.current_allocated_bytes == 0u);

        /* Tight OOM on JPEG transcode & reconstruct. */
        rfc_encoded oom_tr = {0};
        rfc_metadata tr_m = {0};
        rfc_setting_value sv[1] = {{"exactness", "original-byte-reconstruction"}};
        CHECK(jrec->open_session(&tex, &sess) == RFC_OK);
        CHECK(jrec->transcode_jpeg_to_jxl(sess, &tex, k_jpeg_baseline_16x16,
                                          sizeof(k_jpeg_baseline_16x16), sv, 1u,
                                          &oom_tr, &tr_m) ==
              RFC_ERR_OUT_OF_MEMORY);
        jrec->close_session(sess);
        rfc_budget_stats_get(tiny, &bs);
        CHECK(bs.current_allocated_bytes == 0u);

        rfc_encoded valid_jrec_jxl = {0};
        CHECK(rfc_jxl_transcode_jpeg(&exec, k_jpeg_baseline_16x16,
                                     sizeof(k_jpeg_baseline_16x16), sv, 1u,
                                     &valid_jrec_jxl, &tr_m) == RFC_OK);
        rfc_metadata_free(&tr_m, &alloc);

        CHECK(jrec->open_session(&tex, &sess) == RFC_OK);
        CHECK(jrec->reconstruct_jpeg_from_jxl(
                  sess, &tex, valid_jrec_jxl.data, valid_jrec_jxl.len, &oom_tr,
                  &tr_m) == RFC_ERR_OUT_OF_MEMORY);
        jrec->close_session(sess);
        rfc_budget_stats_get(tiny, &bs);
        CHECK(bs.current_allocated_bytes == 0u);

        rfc_budget_destroy(tiny);
        rfc_encoded_free(&valid_jxl);
        rfc_encoded_free(&valid_jrec_jxl);
        rfc_frame_free(&src);
        CHECK(st.live == 0u);
    }

    /* 3. Mid-encode / cancel + busy unregister protection + retry isolation. */
    {
        rfc_frame noise = {0};
        fill_pattern_frame(&noise, 1024u, 1024u, RFC_SAMPLE_RGB8,
                           RFC_ALPHA_NONE, 8u, NULL, &alloc);
        void *sess = NULL;
        CHECK(adp->open_session(&exec, &sess) == RFC_OK);
        direct_cancel_box box;
        memset(&box, 0, sizeof(box));
        box.base.adapter = adp;
        box.base.is_jrec = 0;
        box.base.session = sess;
        box.exec = &exec;
        box.frame = &noise;
        run_cancel_thread_and_join(&box.base, run_enc_cancel);
        CHECK(box.base.unreg_was_busy == 1);
        CHECK(box.out_st == RFC_ERR_CANCELLED);
        adp->close_session(sess);
        rfc_frame_free(&noise);
        CHECK(st.live == 0u);

        /* Instant-pre-cancel transcode & reconstruct paths cleanly return RFC_ERR_CANCELLED
         * and verify jrec cancel_session + RFC_ERR_BUSY. */
        CHECK(jrec->open_session(&exec, &sess) == RFC_OK);
        jrec->cancel_session(sess);
        CHECK(rfc_registry_unregister_jpeg_reconstruct() == RFC_ERR_BUSY);
        rfc_encoded tr = {0};
        rfc_metadata tm = {0};
        rfc_setting_value sv[1] = {{"exactness", "original-byte-reconstruction"}};
        CHECK(jrec->transcode_jpeg_to_jxl(
                  sess, &exec, k_jpeg_baseline_16x16,
                  sizeof(k_jpeg_baseline_16x16), sv, 1u, &tr, &tm) ==
              RFC_ERR_CANCELLED);
        jrec->close_session(sess);

        rfc_encoded valid_jrec = {0};
        CHECK(rfc_jxl_transcode_jpeg(&exec, k_jpeg_baseline_16x16,
                                     sizeof(k_jpeg_baseline_16x16), sv, 1u,
                                     &valid_jrec, &tm) == RFC_OK);
        rfc_metadata_free(&tm, &alloc);

        CHECK(jrec->open_session(&exec, &sess) == RFC_OK);
        jrec->cancel_session(sess);
        CHECK(jrec->reconstruct_jpeg_from_jxl(
                  sess, &exec, valid_jrec.data, valid_jrec.len, &tr, &tm) ==
              RFC_ERR_CANCELLED);
        jrec->close_session(sess);

        /* Deterministic mid-transcode and mid-reconstruction cancellation via
         * tracked_alloc trigger (proving RFC_ERR_CANCELLED + RFC_ERR_BUSY). */
        size_t live_base = st.live;
        CHECK(jrec->open_session(&exec, &sess) == RFC_OK);
        st.calls = 0u;
        st.cancel_at_call = 4u;
        st.cancel_jrec = jrec;
        st.cancel_sess = sess;
        st.unreg_was_busy = 0;
        CHECK(jrec->transcode_jpeg_to_jxl(
                  sess, &exec, k_jpeg_baseline_16x16,
                  sizeof(k_jpeg_baseline_16x16), sv, 1u, &tr, &tm) ==
              RFC_ERR_CANCELLED);
        CHECK(st.unreg_was_busy == 1);
        CHECK(tr.data == NULL);
        st.cancel_at_call = 0u;
        st.cancel_jrec = NULL;
        st.cancel_sess = NULL;
        jrec->close_session(sess);
        CHECK(st.live == live_base);

        CHECK(jrec->open_session(&exec, &sess) == RFC_OK);
        st.calls = 0u;
        st.cancel_at_call = 3u;
        st.cancel_jrec = jrec;
        st.cancel_sess = sess;
        st.unreg_was_busy = 0;
        CHECK(jrec->reconstruct_jpeg_from_jxl(
                  sess, &exec, valid_jrec.data, valid_jrec.len, &tr, &tm) ==
              RFC_ERR_CANCELLED);
        CHECK(st.unreg_was_busy == 1);
        CHECK(tr.data == NULL);
        st.cancel_at_call = 0u;
        st.cancel_jrec = NULL;
        st.cancel_sess = NULL;
        jrec->close_session(sess);
        CHECK(st.live == live_base);

        rfc_encoded_free(&valid_jrec);
        CHECK(st.live == 0u);
    }

    /* 4. Scheduler cancel-during-stage + retry isolation + file-safety. */
    {
        rfc_scheduler_config scfg;
        rfc_scheduler_config_default(&scfg);
        scfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&scfg);
        CHECK(sched != NULL);
        rfc_job_slot *slot = rfc_job_slot_create();
        CHECK(slot != NULL);

        char out_path[512];
        make_temp_path(out_path, sizeof(out_path), "retry.jxl");
        remove(out_path);

        rfc_io_sink sink;
        rfc_io_sink_init_path(&sink, out_path, RFC_EXPORT_REFUSE_EXISTING);
        rfc_setting_value sv[1] = {
            {"exactness", "original-byte-reconstruction"}};
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = RFC_FORMAT_JPEG;
        sp.target_format = RFC_FORMAT_JPEG_XL;
        sp.source_bytes = k_jpeg_baseline_16x16;
        sp.source_size = sizeof(k_jpeg_baseline_16x16);
        sp.settings = sv;
        sp.setting_count = 1u;
        sp.io_sink = &sink;
        sp.has_stage_ops = 1;
        sp.stage_ops.on_verify = cancel_immediately_stage;

        rfc_job *j1 = NULL;
        CHECK(rfc_scheduler_submit(sched, slot, &sp, &j1) == RFC_OK);
        CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot sn;
        CHECK(rfc_job_snapshot_get(j1, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_CANCELLED &&
              sn.budget.current_allocated_bytes == 0u);
        FILE *check_f = fopen(out_path, "rb");
        CHECK(check_f == NULL); /* no published output */
        rfc_job_release(j1);

        /* Clear hook on a fresh spec with same file destination after retry. */
        rfc_job *j2 = NULL;
        CHECK(rfc_scheduler_retry(sched, slot, &j2) == RFC_OK);
        CHECK(rfc_job_wait(j2, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j2, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_CANCELLED &&
              sn.budget.current_allocated_bytes == 0u);
        rfc_job_release(j2);
        rfc_job_slot_destroy(slot);

        /* Pre-existing file safety: RFC_ERR_DESTINATION_EXISTS preserves bytes. */
        const uint8_t sentinel[7] = {'S', 'A', 'F', 'E', 'J', 'X', 'L'};
        CHECK(write_file_all(out_path, sentinel, sizeof(sentinel)));
        sp.has_stage_ops = 0;
        memset(&sp.stage_ops, 0, sizeof(sp.stage_ops));
        rfc_job *j3 = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &j3) ==
              RFC_ERR_DESTINATION_EXISTS);
        CHECK(j3 == NULL);
        size_t slen = 0u;
        uint8_t *after = read_file_all(out_path, &slen);
        CHECK(after != NULL && slen == sizeof(sentinel) &&
              memcmp(after, sentinel, sizeof(sentinel)) == 0);
        free(after);
        remove(out_path);

        /* 5. Default-budget 512x512 & 2048x2048 RGB8 t=1 & t=3 compare runs. */
        static const uint32_t dims[2] = {512u, 2048u};
        for (int di = 0; di < 2; di++) {
            uint32_t dim = dims[di];
            rfc_frame big = {0};
            fill_pattern_frame(&big, dim, dim, RFC_SAMPLE_RGB8, RFC_ALPHA_NONE,
                               8u, NULL, &alloc);
            char ppm_path[512];
            make_temp_path(ppm_path, sizeof(ppm_path), "cmp.ppm");
            FILE *pfp = fopen(ppm_path, "wb");
            CHECK(pfp != NULL);
            fprintf(pfp, "P6\n%u %u\n255\n", dim, dim);
            CHECK(fwrite(big.data, 1u, big.data_len, pfp) == big.data_len);
            fclose(pfp);
            rfc_frame_free(&big);

            for (uint32_t ti = 0u; ti < 2u; ti++) {
                uint32_t thr = (ti == 0u) ? 1u : 3u;
                char jxl_out_path[512];
                make_temp_path(jxl_out_path, sizeof(jxl_out_path),
                               "cmp_pix.jxl");
                remove(jxl_out_path);
                rfc_io_source src_io;
                rfc_io_source_init_path(&src_io, ppm_path, RFC_IMPORT_DEFAULT);
                rfc_compare_candidate_spec cand;
                memset(&cand, 0, sizeof(cand));
                cand.target_format = RFC_FORMAT_JPEG_XL;
                cand.dest_path_utf8 = jxl_out_path;

                rfc_compare_spec cspec;
                memset(&cspec, 0, sizeof(cspec));
                cspec.io_source = &src_io;
                cspec.source_format = RFC_FORMAT_PNM;
                cspec.candidates = &cand;
                cspec.candidate_count = 1u;
                cspec.warmup_iterations = 0u;
                cspec.measured_iterations = 1u;
                cspec.requested_threads = thr;
                cspec.metadata_policy = RFC_META_POLICY_PRESERVE;

                rfc_comparison_report *crep =
                    (rfc_comparison_report *)calloc(1u, sizeof(*crep));
                CHECK(crep != NULL);
                CHECK(rfc_compare_run(&cspec, crep) == RFC_OK);
                CHECK(crep->candidate_count == 1u &&
                      crep->candidates[0].status == RFC_OK &&
                      crep->candidates[0].effective_threads == thr &&
                      crep->candidates[0].size.produced_file_bytes > 0u);
                free(crep);
                remove(jxl_out_path);
            }
            remove(ppm_path);
        }

        /* 6. rfc_compare_run for JPEG -> JPEG XL original-byte-reconstruction. */
        {
            char jpg_path[512];
            char jxl_out_path[512];
            make_temp_path(jpg_path, sizeof(jpg_path), "cmp_src.jpg");
            make_temp_path(jxl_out_path, sizeof(jxl_out_path), "cmp_jrec.jxl");
            remove(jxl_out_path);
            CHECK(write_file_all(jpg_path, k_jpeg_baseline_16x16,
                                 sizeof(k_jpeg_baseline_16x16)));
            rfc_io_source src_io;
            rfc_io_source_init_path(&src_io, jpg_path, RFC_IMPORT_DEFAULT);

            rfc_compare_candidate_spec cand;
            memset(&cand, 0, sizeof(cand));
            cand.target_format = RFC_FORMAT_JPEG_XL;
            cand.dest_path_utf8 = jxl_out_path;
            cand.settings = sv;
            cand.setting_count = 1u;

            rfc_compare_spec cspec;
            memset(&cspec, 0, sizeof(cspec));
            cspec.io_source = &src_io;
            cspec.source_format = RFC_FORMAT_JPEG;
            cspec.candidates = &cand;
            cspec.candidate_count = 1u;
            cspec.warmup_iterations = 0u;
            cspec.measured_iterations = 1u;
            cspec.requested_threads = 1u;
            cspec.metadata_policy = RFC_META_POLICY_PRESERVE;

            rfc_comparison_report *crep =
                (rfc_comparison_report *)calloc(1u, sizeof(*crep));
            CHECK(crep != NULL);
            CHECK(rfc_compare_run(&cspec, crep) == RFC_OK);
            CHECK(crep->candidate_count == 1u &&
                  crep->candidates[0].status == RFC_OK &&
                  crep->raw_frame_bytes == 16ull * 16ull * 3ull &&
                  crep->candidates[0].size.produced_file_bytes > 0u);
            free(crep);
            remove(jxl_out_path);
            remove(jpg_path);
        }

        rfc_scheduler_destroy(sched);
        CHECK(st.live == 0u);
    }

    rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
    rfc_registry_release_jpeg_reconstruct();
    printf("jxl: cancellation_limits_budget_and_file_safety ok\n");
}

/* ------------------------------------------------------------------------ */
/* CLI harness for test_jxl.py (mirrors test_webp.c RFWE exchange format)   */
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
    if (strcmp(s, "jxl") == 0 || strcmp(s, "jpeg-xl") == 0) {
        *out = RFC_FORMAT_JPEG_XL;
        return 1;
    }
    if (strcmp(s, "jpeg") == 0) {
        *out = RFC_FORMAT_JPEG;
        return 1;
    }
    if (strcmp(s, "png") == 0) {
        *out = RFC_FORMAT_PNG;
        return 1;
    }
    if (strcmp(s, "webp") == 0) {
        *out = RFC_FORMAT_WEBP;
        return 1;
    }
    if (strcmp(s, "rasterfold") == 0) {
        *out = RFC_FORMAT_RASTERFOLD;
        return 1;
    }
    return 0;
}

static int cli_main(int argc, char **argv) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    CHECK(rfc_codecs_register_webp() == RFC_OK);
    CHECK(rfc_codecs_register_jxl() == RFC_OK);

    const char *sub = argv[2];
    if (strcmp(sub, "encode") == 0 && (argc == 7 || argc == 8)) {
        rfc_format_id fmt = 0;
        CHECK(parse_fmt_cli(argv[3], &fmt));
        rfc_layout lay;
        rfc_metadata meta;
        uint8_t *raw = NULL, *pix = NULL;
        size_t pay_len = 0u;
        CHECK(parse_rfwe(argv[4], &lay, &meta, &raw, &pix, &pay_len));
        uint32_t thr =
            (argc == 8) ? (uint32_t)strtoul(argv[7], NULL, 10) : 1u;

        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, thr ? thr : 1u);
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

    if (strcmp(sub, "decode") == 0 && argc == 6) {
        size_t jlen = 0u;
        uint8_t *jb = read_file_all(argv[4], &jlen);
        if (!jb) {
            printf("STATUS=io-error\n");
            return 2;
        }
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        const rfc_session_adapter *adp =
            rfc_registry_acquire_session(RFC_FORMAT_JPEG_XL);
        CHECK(adp != NULL);
        void *ds = NULL;
        CHECK(adp->open_session(&exec, &ds) == RFC_OK);
        rfc_frame *frames = NULL;
        size_t fc = 0u;
        rfc_metadata dm = {0};
        rfc_status r =
            adp->decode_session(ds, &exec, jb, jlen, NULL, 0u, &frames, &fc,
                                &dm);
        adp->close_session(ds);
        rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
        free(jb);
        if (r != RFC_OK) {
            printf("STATUS=%s LIVE=%zu\n", rfc_status_name(r), st.live);
            CHECK(st.live == 0u);
            return 2;
        }
        uint32_t ch =
            (frames[0].layout.format == RFC_SAMPLE_GRAY8 ||
             frames[0].layout.format == RFC_SAMPLE_GRAY16)
                ? 1u
                : ((frames[0].layout.format == RFC_SAMPLE_RGBA8 ||
                    frames[0].layout.format == RFC_SAMPLE_RGBA16)
                       ? 4u
                       : 3u);
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
        FILE *fp = fopen(argv[5], "wb");
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
        printf("STATUS=ok WIDTH=%u HEIGHT=%u SAMPLE=%u BPS=%u ORIENTATION=%u "
               "ICC_LEN=%zu EXIF_LEN=%zu XMP_LEN=%zu\n",
               frames[0].layout.width, frames[0].layout.height,
               (uint32_t)frames[0].layout.format,
               frames[0].layout.bits_per_sample, (uint32_t)dm.orientation,
               dm.icc_len, dm.exif_len, dm.xmp_len);
        rfc_metadata_free(&dm, &alloc);
        rfc_frame_free(&frames[0]);
        alloc.free(alloc.ctx, frames);
        CHECK(st.live == 0u);
        return 0;
    }

    if (strcmp(sub, "convert-jrec") == 0 && argc == 5) {
        rfc_scheduler_config scfg;
        rfc_scheduler_config_default(&scfg);
        rfc_scheduler *sched = rfc_scheduler_create(&scfg);
        CHECK(sched != NULL);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_path(&src_io, argv[3], RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&dst_io, argv[4], RFC_EXPORT_REFUSE_EXISTING);
        rfc_setting_value sv[1] = {
            {"exactness", "original-byte-reconstruction"}};
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = RFC_FORMAT_JPEG;
        sp.target_format = RFC_FORMAT_JPEG_XL;
        sp.io_source = &src_io;
        sp.io_sink = &dst_io;
        sp.settings = sv;
        sp.setting_count = 1u;
        rfc_job *j = NULL;
        rfc_status sst = rfc_scheduler_submit(sched, NULL, &sp, &j);
        if (sst != RFC_OK) {
            printf("STATUS=%s\n", rfc_status_name(sst));
            rfc_scheduler_destroy(sched);
            return 2;
        }
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        printf("STATUS=%s\n", rfc_status_name(snap.status));
        rfc_job_release(j);
        rfc_scheduler_destroy(sched);
        return snap.state == RFC_JOB_SUCCEEDED ? 0 : 2;
    }

    if (strcmp(sub, "reconstruct-jxl") == 0 && argc == 5) {
        size_t jlen = 0u;
        uint8_t *jb = read_file_all(argv[3], &jlen);
        if (!jb) {
            printf("STATUS=io-error\n");
            return 2;
        }
        rfc_exec_context exec;
        tracked_alloc_state st;
        rfc_allocator alloc;
        init_exec(&exec, &st, &alloc, NULL, 1u);
        rfc_encoded rec = {0};
        rfc_metadata dm = {0};
        rfc_status r =
            rfc_jxl_reconstruct_jpeg(&exec, jb, jlen, &rec, &dm);
        free(jb);
        if (r == RFC_OK) {
            CHECK(write_file_all(argv[4], rec.data, rec.len));
        }
        size_t rlen = rec.len;
        rfc_encoded_free(&rec);
        rfc_metadata_free(&dm, &alloc);
        CHECK(st.live == 0u);
        printf("STATUS=%s OUT_BYTES=%zu\n", rfc_status_name(r), rlen);
        return r == RFC_OK ? 0 : 2;
    }

    if (strcmp(sub, "convert") == 0 && argc == 9) {
        rfc_format_id sfmt = 0, dfmt = 0;
        CHECK(parse_fmt_cli(argv[3], &sfmt) && parse_fmt_cli(argv[4], &dfmt));
        rfc_setting_value sv[1] = {{"metadata", argv[7]}};
        uint32_t ack = (uint32_t)strtoul(argv[8], NULL, 0);
        rfc_scheduler_config scfg;
        rfc_scheduler_config_default(&scfg);
        rfc_scheduler *sched = rfc_scheduler_create(&scfg);
        CHECK(sched != NULL);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_path(&src_io, argv[5], RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&dst_io, argv[6], RFC_EXPORT_REFUSE_EXISTING);
        rfc_job_spec sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_format = sfmt;
        sp.target_format = dfmt;
        sp.io_source = &src_io;
        sp.io_sink = &dst_io;
        sp.settings = sv;
        sp.setting_count = 1u;
        sp.acknowledged_unsupported_metadata = ack;
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
               has_rep ? rfc_verify_meta_outcome_name(rep.meta_outcome)
                       : "none",
               has_rep ? rep.metadata.unsupported_items : 0u);
        rfc_job_release(j);
        rfc_scheduler_destroy(sched);
        return snap.state == RFC_JOB_SUCCEEDED ? 0 : 2;
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
            rfc_registry_acquire_session(RFC_FORMAT_JPEG_XL);
        CHECK(adp != NULL);
        void *sess = NULL;
        CHECK(adp->open_session(&b_exec, &sess) == RFC_OK);

        if (strcmp(argv[3], "encode") == 0) {
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
            rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
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
            size_t jlen = 0u;
            uint8_t *jb = read_file_all(argv[4], &jlen);
            CHECK(jb != NULL);
            rfc_frame *df = NULL;
            size_t dc = 0u;
            rfc_metadata dm = {0};
            rfc_status r = adp->decode_session(sess, &b_exec, jb, jlen, NULL,
                                               0u, &df, &dc, &dm);
            rfc_metadata_free(&dm, &ba);
            if (df) {
                rfc_frame_free(&df[0]);
                ba.free(ba.ctx, df);
            }
            free(jb);
            adp->close_session(sess);
            rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
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
        rfc_registry_release_session(RFC_FORMAT_JPEG_XL);
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
    test_depths_padding_alpha_and_xyb();
    test_metadata_boxes_orientation_and_cross_format();
    test_jpeg_reconstruction_and_exactness();
    test_cancellation_limits_budget_and_file_safety();

    printf("jxl: all checks passed (%d checks)\n", g_checks);
    return 0;
}
