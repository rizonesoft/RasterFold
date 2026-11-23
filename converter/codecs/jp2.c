/* RasterFold Converter -- JPEG 2000 session adapter (D10 T02 s9).
 *
 * Uses bundled OpenJPEG 2.5.4 (libopenjp2.a, BSD-2-Clause, pinned in
 * converter/vendor.json) with reversible 5-3 lossless wavelet transform
 * (irreversible=0, tcp_numlayers=1, tcp_rates[0]=0, cp_disto_alloc=1).
 * Irreversible 9-7 output is never offered or emitted as lossless.
 *
 * Supported native sample formats:
 *   GRAY8, GRAY16, RGB8, RGBA8, RGB16, RGBA16
 *   (unsigned 1..8 bits in 8-bit storage, 9..16 bits in 16-bit storage,
 *    left-aligned in MSBs with zero low padding bits per rfc_image.h).
 *
 * Container & bare-codestream rules:
 *   - Encode emits a JP2 container (OPJ_CODEC_JP2). When RGBA is encoded
 *     with an ICC profile (colr meth=2), OpenJPEG suppresses cdef because
 *     enumcs == 0; this adapter splices a valid 34-byte cdef box inside
 *     jp2h marking channel 3 as straight alpha (typ=1, asoc=0).
 *   - Decode accepts JP2 containers (jP  / ftyp / jp2h / jp2c) and bare
 *     J2K codestreams (SOC + SIZ) for 1-component (GRAY) and 3-component
 *     (RGB) unsigned images; ambiguous 4-component bare J2K codestreams
 *     are refused before pixel allocation (RFC_ERR_UNSUPPORTED_COMBINATION).
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef OPJ_STATIC
#define OPJ_STATIC
#endif

#include "rfc_codecs.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <openjpeg.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JP2_MAX_THREADS 16u
#define JP2_STREAM_CHUNK_BYTES (64u * 1024u)

#ifdef _WIN32
static SRWLOCK g_jp2_env_lock = SRWLOCK_INIT;
#else
static pthread_mutex_t g_jp2_env_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static atomic_uintptr_t g_jp2_test_hook = 0;

void rfc_codecs_jp2_set_hook_for_testing(void (*fn)(int phase)) {
    atomic_store_explicit(&g_jp2_test_hook, (uintptr_t)fn,
                          memory_order_release);
}

static void jp2_invoke_hook(int phase) {
    void (*fn)(int) =
        (void (*)(int))atomic_load_explicit(&g_jp2_test_hook,
                                            memory_order_acquire);
    if (fn) {
        fn(phase);
    }
}

typedef struct jp2_session {
    atomic_int cancelled;
    const rfc_allocator *allocator;
} jp2_session;

static int jp2_is_cancelled(const jp2_session *sess,
                            const rfc_exec_context *exec) {
    if (sess && atomic_load_explicit(&sess->cancelled, memory_order_acquire)) {
        return 1;
    }
    if (exec && exec->job && rfc_job_is_cancelled(exec->job)) {
        return 1;
    }
    return 0;
}

static uint16_t jp2_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t jp2_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t jp2_be64(const uint8_t *p) {
    return ((uint64_t)jp2_be32(p) << 32) | (uint64_t)jp2_be32(p + 4);
}

static void jp2_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static const rfc_limits *jp2_limits_of(const rfc_exec_context *exec,
                                       rfc_limits *fallback) {
    if (exec && exec->limits.max_width > 0u) {
        return &exec->limits;
    }
    rfc_limits_default(fallback);
    return fallback;
}

static int jp2_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0u && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static int jp2_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static void jp2_quiet_msg(const char *msg, void *client_data) {
    (void)msg;
    (void)client_data;
}

static rfc_status jp2_parse_settings(const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_metadata_policy *out_policy,
                                     rfc_exactness_class *out_exact) {
    *out_policy = RFC_META_POLICY_PRESERVE;
    *out_exact = RFC_EXACT_PIXEL_EXACT;
    for (size_t i = 0u; i < setting_count; i++) {
        if (!settings || !settings[i].id || !settings[i].value) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(settings[i].id, "metadata") == 0) {
            if (strcmp(settings[i].value, "preserve") == 0) {
                *out_policy = RFC_META_POLICY_PRESERVE;
            } else if (strcmp(settings[i].value, "strip") == 0) {
                *out_policy = RFC_META_POLICY_STRIP;
            } else {
                return RFC_ERR_SETTING_VALUE;
            }
        } else if (strcmp(settings[i].id, "exactness") == 0) {
            if (strcmp(settings[i].value, "pixel-exact") == 0) {
                *out_exact = RFC_EXACT_PIXEL_EXACT;
            } else if (strcmp(settings[i].value, "byte-identical") == 0) {
                *out_exact = RFC_EXACT_BYTE_IDENTICAL;
            } else {
                return RFC_ERR_SETTING_VALUE;
            }
        } else {
            return RFC_ERR_UNKNOWN_SETTING;
        }
    }
    if (*out_policy == RFC_META_POLICY_STRIP &&
        *out_exact == RFC_EXACT_BYTE_IDENTICAL) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    return RFC_OK;
}

/* ---- Bounded zero-allocation JP2 box + J2K SIZ/COD pre-scanner ---- */

typedef struct jp2_scan_info {
    int is_jp2;
    uint32_t width;
    uint32_t height;
    uint16_t numcomps;
    uint8_t prec;
    uint64_t num_tiles;
    size_t jp2h_len;
    rfc_sample_format sample_format;
    rfc_alpha_mode alpha_mode;
    const uint8_t *codestream;
    size_t codestream_len;
    const uint8_t *icc;
    size_t icc_len;
    const uint8_t *xmp;
    size_t xmp_len;
    const uint8_t *exif;
    size_t exif_len;
    int has_cdef;
    int has_straight_alpha;
    int has_unsupported_colr;
    int has_unpreserved_meta;
    int is_reversible;
} jp2_scan_info;

static const uint8_t k_jp2_sig[12] = {
    0x00u, 0x00u, 0x00u, 0x0Cu, 0x6Au, 0x50u,
    0x20u, 0x20u, 0x0Du, 0x0Au, 0x87u, 0x0Au
};

static const uint8_t k_xmp_uuid[16] = {
    0xBEu, 0x7Au, 0xCFu, 0xCBu, 0x97u, 0xA9u, 0x42u, 0xE8u,
    0x9Cu, 0x71u, 0x99u, 0x94u, 0x91u, 0xE3u, 0xAFu, 0xACu
};

/* Standard JP2 Exif UUID ("JpgTiffExif->JP2"). */
static const uint8_t k_exif_uuid[16] = {
    0x4Au, 0x70u, 0x67u, 0x54u, 0x69u, 0x66u, 0x66u, 0x45u,
    0x78u, 0x69u, 0x66u, 0x2Du, 0x3Eu, 0x4Au, 0x50u, 0x32u
};

static rfc_status jp2_read_box(const uint8_t *buf,
                               size_t buf_len,
                               size_t off,
                               uint32_t *out_type,
                               size_t *out_payload_off,
                               size_t *out_payload_len,
                               size_t *out_next_off) {
    if (off + 8u > buf_len) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    uint32_t lbox = jp2_be32(buf + off);
    uint32_t tbox = jp2_be32(buf + off + 4u);
    size_t hdr = 8u;
    uint64_t total = lbox;
    if (lbox == 1u) {
        if (off + 16u > buf_len) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        total = jp2_be64(buf + off + 8u);
        hdr = 16u;
        if (total < 16u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
    } else if (lbox == 0u) {
        total = (uint64_t)(buf_len - off);
    } else if (lbox < 8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (total > (uint64_t)(buf_len - off)) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    *out_type = tbox;
    *out_payload_off = off + hdr;
    *out_payload_len = (size_t)(total - (uint64_t)hdr);
    *out_next_off = off + (size_t)total;
    return RFC_OK;
}

static rfc_status jp2_scan_codestream(const uint8_t *cs,
                                      size_t cs_len,
                                      int is_jp2,
                                      int has_ihdr,
                                      uint32_t ihdr_w,
                                      uint32_t ihdr_h,
                                      uint16_t ihdr_nc,
                                      uint8_t ihdr_prec,
                                      jp2_scan_info *out) {
    if (!cs || cs_len < 4u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    /* SOC (FF 4F) followed immediately by SIZ (FF 51). */
    if (cs[0] != 0xFFu || cs[1] != 0x4Fu || cs[2] != 0xFFu || cs[3] != 0x51u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (cs_len < 6u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    uint16_t lsiz = jp2_be16(cs + 4u);
    if (lsiz < 41u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if ((size_t)4u + (size_t)lsiz > cs_len) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    const uint8_t *s = cs + 6u; /* after FF 51 Lsiz */
    uint32_t xsiz = jp2_be32(s + 2u);
    uint32_t ysiz = jp2_be32(s + 6u);
    uint32_t xosiz = jp2_be32(s + 10u);
    uint32_t yosiz = jp2_be32(s + 14u);
    uint32_t xtsiz = jp2_be32(s + 18u);
    uint32_t ytsiz = jp2_be32(s + 22u);
    uint32_t xtosiz = jp2_be32(s + 26u);
    uint32_t ytosiz = jp2_be32(s + 30u);
    uint16_t csiz = jp2_be16(s + 34u);

    if (csiz == 0u || lsiz != (uint16_t)(38u + 3u * csiz)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (xsiz <= xosiz || ysiz <= yosiz || xtsiz == 0u || ytsiz == 0u ||
        xtosiz > xosiz || ytosiz > yosiz ||
        (uint64_t)xtosiz + (uint64_t)xtsiz <= (uint64_t)xosiz ||
        (uint64_t)ytosiz + (uint64_t)ytsiz <= (uint64_t)yosiz) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint64_t tx = ((uint64_t)(xsiz - xtosiz) + (uint64_t)xtsiz - 1ull) /
                  (uint64_t)xtsiz;
    uint64_t ty = ((uint64_t)(ysiz - ytosiz) + (uint64_t)ytsiz - 1ull) /
                  (uint64_t)ytsiz;
    uint64_t num_tiles = 0u;
    if (!jp2_mul_u64(tx, ty, &num_tiles) || num_tiles == 0u) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    uint32_t w = xsiz - xosiz;
    uint32_t h = ysiz - yosiz;
    if (has_ihdr && (w != ihdr_w || h != ihdr_h || csiz != ihdr_nc)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (csiz != 1u && csiz != 3u && csiz != 4u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (!is_jp2 && csiz == 4u) {
        /* Bare J2K has no colr/cdef box to distinguish straight RGBA from
         * CMYK or auxiliary channels (D10 T02 s9 plan review #2). */
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }

    uint8_t prec0 = 0u;
    for (uint16_t c = 0u; c < csiz; c++) {
        uint8_t ssiz = s[36u + 3u * c];
        uint8_t xrsiz = s[36u + 3u * c + 1u];
        uint8_t yrsiz = s[36u + 3u * c + 2u];
        if ((ssiz & 0x80u) != 0u) {
            return RFC_ERR_UNSUPPORTED_SAMPLE; /* signed samples */
        }
        uint8_t p = (uint8_t)((ssiz & 0x7Fu) + 1u);
        if (p < 1u || p > 16u) {
            return RFC_ERR_UNSUPPORTED_SAMPLE;
        }
        if (xrsiz != 1u || yrsiz != 1u) {
            return RFC_ERR_UNSUPPORTED_SAMPLE; /* subsampled */
        }
        if (c == 0u) {
            prec0 = p;
        } else if (p != prec0) {
            return RFC_ERR_UNSUPPORTED_SAMPLE; /* mixed precision */
        }
    }
    if (has_ihdr && ihdr_prec != 0u && prec0 != ihdr_prec) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    /* Walk main-header markers up to SOT (FF 90) to validate COD (FF 52). */
    size_t pos = 4u + (size_t)lsiz;
    int saw_cod = 0;
    int saw_sot = 0;
    int reversible = 0;
    uint32_t marker_guard = 0u;
    while (pos + 2u <= cs_len && marker_guard < 256u) {
        marker_guard++;
        if (cs[pos] != 0xFFu) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        uint8_t m = cs[pos + 1u];
        if (m == 0x90u) { /* SOT */
            saw_sot = 1;
            break;
        }
        if (m == 0xD9u) { /* EOC before SOT */
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (m == 0x4Fu || m == 0x93u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (pos + 4u > cs_len) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint16_t mlen = jp2_be16(cs + pos + 2u);
        if (mlen < 2u || pos + 2u + (size_t)mlen > cs_len) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (m == 0x52u) { /* COD */
            if (mlen < 12u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            /* SPcod transform byte is at offset 2+2+1+4+5 = 11 from marker start
             * (Scod:1, SGcod:4, SPcod: num_decomp(1)+cbw(1)+cbh(1)+cbsty(1)+xform(1)). */
            uint8_t xform = cs[pos + 13u];
            if (xform != 0u && xform != 1u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            reversible = (xform == 1u) ? 1 : 0;
            saw_cod = 1;
        }
        pos += 2u + (size_t)mlen;
    }
    if (!saw_cod || !saw_sot) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    out->width = w;
    out->height = h;
    out->numcomps = csiz;
    out->prec = prec0;
    out->num_tiles = num_tiles;
    out->codestream = cs;
    out->codestream_len = cs_len;
    out->is_reversible = reversible;
    if (csiz == 1u) {
        out->sample_format = (prec0 <= 8u) ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_GRAY16;
        out->alpha_mode = RFC_ALPHA_NONE;
    } else if (csiz == 3u) {
        out->sample_format = (prec0 <= 8u) ? RFC_SAMPLE_RGB8 : RFC_SAMPLE_RGB16;
        out->alpha_mode = RFC_ALPHA_NONE;
    } else {
        out->sample_format = (prec0 <= 8u) ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGBA16;
        out->alpha_mode = RFC_ALPHA_STRAIGHT;
    }
    return RFC_OK;
}

static int jp2_is_xml_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Decode one UTF-8 scalar value (no overlongs, no surrogates, <= U+10FFFF). */
static int jp2_utf8_next(const uint8_t *p, size_t len, size_t *pos, uint32_t *out_cp) {
    if (*pos >= len) {
        return 0;
    }
    uint8_t b0 = p[*pos];
    if (b0 < 0x80u) {
        *out_cp = b0;
        (*pos)++;
        return 1;
    }
    if ((b0 & 0xE0u) == 0xC0u) {
        if (*pos + 1u >= len) return 0;
        uint8_t b1 = p[*pos + 1u];
        if ((b1 & 0xC0u) != 0x80u) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(b1 & 0x3Fu);
        if (cp < 0x80u) return 0;
        *out_cp = cp;
        *pos += 2u;
        return 1;
    }
    if ((b0 & 0xF0u) == 0xE0u) {
        if (*pos + 2u >= len) return 0;
        uint8_t b1 = p[*pos + 1u], b2 = p[*pos + 2u];
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
                      ((uint32_t)(b1 & 0x3Fu) << 6) |
                      (uint32_t)(b2 & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
        *out_cp = cp;
        *pos += 3u;
        return 1;
    }
    if ((b0 & 0xF8u) == 0xF0u) {
        if (*pos + 3u >= len) return 0;
        uint8_t b1 = p[*pos + 1u], b2 = p[*pos + 2u], b3 = p[*pos + 3u];
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u ||
            (b3 & 0xC0u) != 0x80u) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x07u) << 18) |
                      ((uint32_t)(b1 & 0x3Fu) << 12) |
                      ((uint32_t)(b2 & 0x3Fu) << 6) |
                      (uint32_t)(b3 & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu) return 0;
        *out_cp = cp;
        *pos += 4u;
        return 1;
    }
    return 0;
}

static int jp2_xml_is_char(uint32_t cp) {
    return cp == 0x09u || cp == 0x0Au || cp == 0x0Du ||
           (cp >= 0x20u && cp <= 0xD7FFu) ||
           (cp >= 0xE000u && cp <= 0xFFFDu) ||
           (cp >= 0x10000u && cp <= 0x10FFFFu);
}

/* XML 1.0 (5th ed.) NameStartChar minus ':'. */
static int jp2_xml_is_ncname_start(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || cp == '_' || (cp >= 'a' && cp <= 'z') ||
           (cp >= 0xC0u && cp <= 0xD6u) || (cp >= 0xD8u && cp <= 0xF6u) ||
           (cp >= 0xF8u && cp <= 0x2FFu) || (cp >= 0x370u && cp <= 0x37Du) ||
           (cp >= 0x37Fu && cp <= 0x1FFFu) ||
           (cp >= 0x200Cu && cp <= 0x200Du) ||
           (cp >= 0x2070u && cp <= 0x218Fu) ||
           (cp >= 0x2C00u && cp <= 0x2FEFu) ||
           (cp >= 0x3001u && cp <= 0xD7FFu) ||
           (cp >= 0xF900u && cp <= 0xFDCFu) ||
           (cp >= 0xFDF0u && cp <= 0xFFFDu) ||
           (cp >= 0x10000u && cp <= 0xEFFFFu);
}

/* XML 1.0 (5th ed.) NameChar minus ':'. */
static int jp2_xml_is_ncname_char(uint32_t cp) {
    return jp2_xml_is_ncname_start(cp) || cp == '-' || cp == '.' ||
           (cp >= '0' && cp <= '9') || cp == 0xB7u ||
           (cp >= 0x0300u && cp <= 0x036Fu) ||
           (cp >= 0x203Fu && cp <= 0x2040u);
}

static int jp2_xml_valid_ncname(const uint8_t *p, size_t len) {
    if (!p || len == 0u) {
        return 0;
    }
    size_t pos = 0u;
    uint32_t cp = 0u;
    if (!jp2_utf8_next(p, len, &pos, &cp) || !jp2_xml_is_ncname_start(cp)) {
        return 0;
    }
    while (pos < len) {
        if (!jp2_utf8_next(p, len, &pos, &cp) || !jp2_xml_is_ncname_char(cp)) {
            return 0;
        }
    }
    return 1;
}

static int jp2_xml_valid_qname(const uint8_t *p, size_t len) {
    if (!p || len == 0u) {
        return 0;
    }
    size_t colon = SIZE_MAX;
    for (size_t k = 0u; k < len; k++) {
        if (p[k] == ':') {
            if (colon != SIZE_MAX) {
                return 0;
            }
            colon = k;
        }
    }
    if (colon == SIZE_MAX) {
        return jp2_xml_valid_ncname(p, len);
    }
    return jp2_xml_valid_ncname(p, colon) &&
           jp2_xml_valid_ncname(p + colon + 1u, len - colon - 1u);
}

static size_t jp2_utf8_encode(uint32_t cp, uint8_t out[4]) {
    if (cp < 0x80u) {
        out[0] = (uint8_t)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | (cp >> 12));
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    out[0] = (uint8_t)(0xF0u | (cp >> 18));
    out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 4u;
}

static void *jp2_xml_realloc(const rfc_allocator *alloc,
                             void *old_ptr,
                             size_t old_bytes,
                             size_t new_bytes) {
    if (!alloc || !alloc->alloc || !alloc->free || new_bytes == 0u) {
        return NULL;
    }
    void *np = alloc->alloc(alloc->ctx, new_bytes);
    if (!np) {
        return NULL;
    }
    if (old_ptr && old_bytes > 0u) {
        size_t copy_n = (old_bytes < new_bytes) ? old_bytes : new_bytes;
        memcpy(np, old_ptr, copy_n);
        alloc->free(alloc->ctx, old_ptr);
    }
    return np;
}

typedef struct jp2_xml_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} jp2_xml_buf;

static int jp2_xml_buf_append(jp2_xml_buf *b,
                              const rfc_allocator *alloc,
                              int *oom,
                              const uint8_t *src,
                              size_t n) {
    if (n == 0u) {
        return 1;
    }
    if (n > SIZE_MAX - b->len) {
        if (oom) *oom = 1;
        return 0;
    }
    size_t need = b->len + n;
    if (need > b->cap) {
        size_t ncap = b->cap ? b->cap : 256u;
        while (ncap < need) {
            if (ncap > SIZE_MAX / 2u) {
                ncap = need;
                break;
            }
            ncap *= 2u;
        }
        uint8_t *nd = (uint8_t *)jp2_xml_realloc(alloc, b->data, b->cap, ncap);
        if (!nd) {
            if (oom) *oom = 1;
            return 0;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, src, n);
    b->len = need;
    return 1;
}

/* Validate an XML 1.0 attribute value (no literal '<', valid UTF-8 XML chars,
 * valid numeric character references and predefined entity references) and, if
 * out_buf != NULL, append its normalized/decoded UTF-8 value into out_buf via
 * alloc. Returns 1 if well-formed, 0 otherwise (setting *oom = 1 on budget/OOM). */
static int jp2_xml_decode_attr_val(const uint8_t *val,
                                   size_t val_len,
                                   jp2_xml_buf *out_buf,
                                   const rfc_allocator *alloc,
                                   int *oom) {
    size_t pos = 0u;
    while (pos < val_len) {
        uint8_t c = val[pos];
        if (c == '<') {
            return 0; /* unescaped '<' forbidden in XML AttValue */
        }
        uint32_t out_cp = 0u;
        if (c == '&') {
            pos++;
            size_t ref_start = pos;
            while (pos < val_len && val[pos] != ';') {
                pos++;
            }
            if (pos >= val_len || pos == ref_start) {
                return 0;
            }
            size_t ref_len = pos - ref_start;
            const uint8_t *ref = val + ref_start;
            pos++; /* skip ';' */
            if (ref[0] == '#') {
                uint32_t cp = 0u;
                if (ref_len >= 3u && ref[1] == 'x') {
                    for (size_t k = 2u; k < ref_len; k++) {
                        uint8_t d = ref[k];
                        uint32_t v = 0u;
                        if (d >= '0' && d <= '9') v = (uint32_t)(d - '0');
                        else if (d >= 'a' && d <= 'f') v = (uint32_t)(d - 'a' + 10);
                        else if (d >= 'A' && d <= 'F') v = (uint32_t)(d - 'A' + 10);
                        else return 0;
                        if (cp > (0x10FFFFu - v) / 16u) return 0;
                        cp = cp * 16u + v;
                    }
                } else if (ref_len >= 2u) {
                    for (size_t k = 1u; k < ref_len; k++) {
                        uint8_t d = ref[k];
                        if (d < '0' || d > '9') return 0;
                        uint32_t v = (uint32_t)(d - '0');
                        if (cp > (0x10FFFFu - v) / 10u) return 0;
                        cp = cp * 10u + v;
                    }
                } else {
                    return 0;
                }
                if (!jp2_xml_is_char(cp)) {
                    return 0;
                }
                out_cp = cp;
            } else if (ref_len == 3u && memcmp(ref, "amp", 3u) == 0) {
                out_cp = '&';
            } else if (ref_len == 2u && memcmp(ref, "lt", 2u) == 0) {
                out_cp = '<';
            } else if (ref_len == 2u && memcmp(ref, "gt", 2u) == 0) {
                out_cp = '>';
            } else if (ref_len == 4u && memcmp(ref, "quot", 4u) == 0) {
                out_cp = '"';
            } else if (ref_len == 4u && memcmp(ref, "apos", 4u) == 0) {
                out_cp = '\'';
            } else {
                return 0;
            }
        } else if (c == '\r') {
            pos++;
            if (pos < val_len && val[pos] == '\n') {
                pos++;
            }
            out_cp = ' '; /* XML 1.0 §3.3.3 whitespace normalization */
        } else if (c == '\n' || c == '\t') {
            pos++;
            out_cp = ' ';
        } else {
            if (!jp2_utf8_next(val, val_len, &pos, &out_cp) ||
                !jp2_xml_is_char(out_cp)) {
                return 0;
            }
        }
        if (out_buf) {
            uint8_t enc[4];
            size_t elen = jp2_utf8_encode(out_cp, enc);
            if (!jp2_xml_buf_append(out_buf, alloc, oom, enc, elen)) {
                return 0;
            }
        }
    }
    return 1;
}

static int jp2_xml_is_reserved_xml_target(const uint8_t *p, size_t len) {
    if (len != 3u) {
        return 0;
    }
    return (p[0] == 'x' || p[0] == 'X') &&
           (p[1] == 'm' || p[1] == 'M') &&
           (p[2] == 'l' || p[2] == 'L');
}

/* Parse a comment starting at p[*pos] == "<!--". Validates UTF-8 XML chars and
 * forbids "--" before "-->". */
static int jp2_xml_skip_comment(const uint8_t *p, size_t len, size_t *pos) {
    if (len - *pos < 7u || memcmp(p + *pos, "<!--", 4u) != 0) {
        return 0;
    }
    size_t j = *pos + 4u;
    while (j < len) {
        if (p[j] == '-' && j + 1u < len && p[j + 1u] == '-') {
            if (j + 2u < len && p[j + 2u] == '>') {
                *pos = j + 3u;
                return 1;
            }
            return 0; /* '--' is forbidden inside XML comments */
        }
        uint32_t cp = 0u;
        if (!jp2_utf8_next(p, len, &j, &cp) || !jp2_xml_is_char(cp)) {
            return 0;
        }
    }
    return 0;
}

/* Parse a processing instruction starting at p[*pos] == "<?". Target must be a
 * valid NCName (and not case-insensitive "xml"); body chars must be valid XML
 * 1.0 Char values. */
static int jp2_xml_skip_pi(const uint8_t *p, size_t len, size_t *pos) {
    if (len - *pos < 4u || p[*pos] != '<' || p[*pos + 1u] != '?') {
        return 0;
    }
    size_t j = *pos + 2u;
    size_t t_start = j;
    while (j < len && !jp2_is_xml_ws(p[j]) &&
           !(p[j] == '?' && j + 1u < len && p[j + 1u] == '>')) {
        j++;
    }
    size_t t_len = j - t_start;
    if (!jp2_xml_valid_ncname(p + t_start, t_len) ||
        jp2_xml_is_reserved_xml_target(p + t_start, t_len)) {
        return 0;
    }
    if (j + 1u < len && p[j] == '?' && p[j + 1u] == '>') {
        *pos = j + 2u;
        return 1;
    }
    if (j >= len || !jp2_is_xml_ws(p[j])) {
        return 0;
    }
    while (j < len) {
        if (p[j] == '?' && j + 1u < len && p[j + 1u] == '>') {
            *pos = j + 2u;
            return 1;
        }
        uint32_t cp = 0u;
        if (!jp2_utf8_next(p, len, &j, &cp) || !jp2_xml_is_char(cp)) {
            return 0;
        }
    }
    return 0;
}

/* Skip Misc* (whitespace, comments, and legal PIs). Returns 0 on malformed
 * comment/PI, 1 otherwise. */
static int jp2_xml_skip_misc(const uint8_t *p, size_t len, size_t *pos) {
    while (*pos < len) {
        while (*pos < len && jp2_is_xml_ws(p[*pos])) {
            (*pos)++;
        }
        size_t rem = len - *pos;
        if (rem >= 2u && p[*pos] == '<' && p[*pos + 1u] == '?') {
            if (!jp2_xml_skip_pi(p, len, pos)) {
                return 0;
            }
            continue;
        }
        if (rem >= 4u && memcmp(p + *pos, "<!--", 4u) == 0) {
            if (!jp2_xml_skip_comment(p, len, pos)) {
                return 0;
            }
            continue;
        }
        break;
    }
    return 1;
}

typedef struct jp2_xml_elem {
    size_t qname_off;
    size_t qname_len;
} jp2_xml_elem;

typedef struct jp2_xml_ns {
    size_t depth;
    size_t prefix_off; /* ignored when prefix_len == 0 (default namespace) */
    size_t prefix_len;
    size_t uri_off;    /* offset into uri_arena */
    size_t uri_len;
} jp2_xml_ns;

typedef struct jp2_xml_attr {
    size_t name_off;
    size_t name_len;
    size_t prefix_off;
    size_t prefix_len;
    size_t local_off;
    size_t local_len;
    size_t val_off;
    size_t val_len;
    int is_xmlns;
    const uint8_t *ns_uri;
    size_t ns_uri_len;
} jp2_xml_attr;

typedef struct jp2_xml_parser {
    const rfc_allocator *alloc;
    int oom;
    jp2_xml_elem *stack;
    size_t depth;
    size_t stack_cap;
    jp2_xml_ns *ns;
    size_t ns_count;
    size_t ns_cap;
    jp2_xml_buf uri_arena;
    jp2_xml_attr *attrs;
    size_t attr_count;
    size_t attr_cap;
    size_t *ht;
    size_t ht_cap;
} jp2_xml_parser;

static void jp2_xml_parser_free(jp2_xml_parser *xp) {
    if (xp->alloc && xp->alloc->free) {
        if (xp->stack) xp->alloc->free(xp->alloc->ctx, xp->stack);
        if (xp->ns) xp->alloc->free(xp->alloc->ctx, xp->ns);
        if (xp->uri_arena.data) xp->alloc->free(xp->alloc->ctx, xp->uri_arena.data);
        if (xp->attrs) xp->alloc->free(xp->alloc->ctx, xp->attrs);
        if (xp->ht) xp->alloc->free(xp->alloc->ctx, xp->ht);
    }
    memset(xp, 0, sizeof(*xp));
}

static int jp2_xml_stack_push(jp2_xml_parser *xp, size_t off, size_t len) {
    if (xp->depth == xp->stack_cap) {
        size_t ncap = xp->stack_cap ? xp->stack_cap * 2u : 64u;
        if (ncap < xp->depth + 1u || ncap > SIZE_MAX / sizeof(jp2_xml_elem)) {
            xp->oom = 1;
            return 0;
        }
        jp2_xml_elem *ns = (jp2_xml_elem *)jp2_xml_realloc(
            xp->alloc, xp->stack, xp->stack_cap * sizeof(*ns), ncap * sizeof(*ns));
        if (!ns) {
            xp->oom = 1;
            return 0;
        }
        xp->stack = ns;
        xp->stack_cap = ncap;
    }
    xp->stack[xp->depth].qname_off = off;
    xp->stack[xp->depth].qname_len = len;
    xp->depth++;
    return 1;
}

static int jp2_xml_ns_push(jp2_xml_parser *xp,
                           size_t depth,
                           size_t prefix_off,
                           size_t prefix_len,
                           size_t uri_off,
                           size_t uri_len) {
    if (xp->ns_count == xp->ns_cap) {
        size_t ncap = xp->ns_cap ? xp->ns_cap * 2u : 16u;
        if (ncap < xp->ns_count + 1u || ncap > SIZE_MAX / sizeof(jp2_xml_ns)) {
            xp->oom = 1;
            return 0;
        }
        jp2_xml_ns *nn = (jp2_xml_ns *)jp2_xml_realloc(
            xp->alloc, xp->ns, xp->ns_cap * sizeof(*nn), ncap * sizeof(*nn));
        if (!nn) {
            xp->oom = 1;
            return 0;
        }
        xp->ns = nn;
        xp->ns_cap = ncap;
    }
    xp->ns[xp->ns_count].depth = depth;
    xp->ns[xp->ns_count].prefix_off = prefix_off;
    xp->ns[xp->ns_count].prefix_len = prefix_len;
    xp->ns[xp->ns_count].uri_off = uri_off;
    xp->ns[xp->ns_count].uri_len = uri_len;
    xp->ns_count++;
    return 1;
}

static void jp2_xml_ns_pop_depth(jp2_xml_parser *xp, size_t depth) {
    while (xp->ns_count > 0u && xp->ns[xp->ns_count - 1u].depth == depth) {
        xp->uri_arena.len = xp->ns[xp->ns_count - 1u].uri_off;
        xp->ns_count--;
    }
}

static int jp2_xml_attr_push(jp2_xml_parser *xp, const jp2_xml_attr *a) {
    if (xp->attr_count == xp->attr_cap) {
        size_t ncap = xp->attr_cap ? xp->attr_cap * 2u : 16u;
        if (ncap < xp->attr_count + 1u || ncap > SIZE_MAX / sizeof(jp2_xml_attr)) {
            xp->oom = 1;
            return 0;
        }
        jp2_xml_attr *na = (jp2_xml_attr *)jp2_xml_realloc(
            xp->alloc, xp->attrs, xp->attr_cap * sizeof(*na), ncap * sizeof(*na));
        if (!na) {
            xp->oom = 1;
            return 0;
        }
        xp->attrs = na;
        xp->attr_cap = ncap;
    }
    xp->attrs[xp->attr_count++] = *a;
    return 1;
}

static uint64_t jp2_xml_fnv1a(uint64_t h, const uint8_t *data, size_t n) {
    for (size_t k = 0u; k < n; k++) {
        h ^= (uint64_t)data[k];
        h *= 1099511628211ull;
    }
    return h;
}

static int jp2_xml_ht_prepare(jp2_xml_parser *xp, size_t count) {
    size_t need = 32u;
    while (need < count * 2u + 8u) {
        if (need > SIZE_MAX / 2u) {
            xp->oom = 1;
            return 0;
        }
        need *= 2u;
    }
    if (need > xp->ht_cap) {
        if (need > SIZE_MAX / sizeof(size_t)) {
            xp->oom = 1;
            return 0;
        }
        size_t *nh = (size_t *)jp2_xml_realloc(
            xp->alloc, xp->ht, xp->ht_cap * sizeof(size_t), need * sizeof(size_t));
        if (!nh) {
            xp->oom = 1;
            return 0;
        }
        xp->ht = nh;
        xp->ht_cap = need;
    }
    for (size_t k = 0u; k < need; k++) {
        xp->ht[k] = SIZE_MAX;
    }
    return 1;
}

static const char k_xml_ns_uri[] = "http://www.w3.org/XML/1998/namespace";
static const char k_xmlns_ns_uri[] = "http://www.w3.org/2000/xmlns/";
static const char k_xmp_ns_uri[] = "adobe:ns:meta/";

/* Resolve an XML namespace prefix (or default namespace when prefix_len == 0)
 * in the active scope. Returns 1 on success (for prefix_len == 0, *out_len may
 * be 0 meaning no default namespace); returns 0 when a non-empty prefix is
 * unbound. */
static int jp2_xml_resolve_ns(const jp2_xml_parser *xp,
                              const uint8_t *p,
                              const uint8_t *prefix,
                              size_t prefix_len,
                              const uint8_t **out_uri,
                              size_t *out_len) {
    if (prefix_len == 3u && memcmp(prefix, "xml", 3u) == 0) {
        *out_uri = (const uint8_t *)k_xml_ns_uri;
        *out_len = sizeof(k_xml_ns_uri) - 1u;
        return 1;
    }
    for (size_t idx = xp->ns_count; idx > 0u; idx--) {
        const jp2_xml_ns *e = &xp->ns[idx - 1u];
        if (e->prefix_len == prefix_len &&
            (prefix_len == 0u || memcmp(p + e->prefix_off, prefix, prefix_len) == 0)) {
            *out_uri = e->uri_len ? (xp->uri_arena.data + e->uri_off) : NULL;
            *out_len = e->uri_len;
            return 1;
        }
    }
    if (prefix_len == 0u) {
        *out_uri = NULL;
        *out_len = 0u;
        return 1;
    }
    return 0;
}

/* Normalize an XML EncName (already validated as [A-Za-z][A-Za-z0-9._-]*)
 * identically to Python encodings.normalize_encoding: ASCII lowercase, keep
 * [a-z0-9.], collapse runs of '-'/'_' between tokens into a single '_'. */
static size_t jp2_xml_normalize_enc(const uint8_t *src,
                                    size_t n,
                                    char *dst,
                                    size_t cap) {
    size_t out = 0u;
    for (size_t k = 0u; k < n; k++) {
        uint8_t c = src[k];
        if (c == '-' || c == '_') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c - 'A' + 'a');
        }
        if (out + 1u >= cap) {
            return SIZE_MAX;
        }
        dst[out++] = (char)c;
    }
    if (out == 0u || out >= cap) {
        return SIZE_MAX;
    }
    dst[out] = '\0';
    return out;
}

static int jp2_enc_norm_in_sorted_list(const char *norm,
                                       const char *const *list,
                                       size_t count) {
    size_t lo = 0u, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = strcmp(norm, list[mid]);
        if (cmp == 0) {
            return 1;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }
    return 0;
}

#include "jp2_iso2022_tables.h"

static uint16_t jp2_dbcs_lookup_u16(const uint16_t *tbl, uint8_t b1, uint8_t b2) {
    if (b1 < 0x21u || b1 > 0x7Eu || b2 < 0x21u || b2 > 0x7Eu) {
        return 0u;
    }
    return tbl[(uint32_t)(b1 - 0x21u) * 94u + (uint32_t)(b2 - 0x21u)];
}

static void jp2_emit_utf8_bmp(uint8_t *out, size_t *pos, uint16_t cp) {
    if (cp < 0x80u) {
        out[(*pos)++] = (uint8_t)cp;
    } else if (cp < 0x800u) {
        out[(*pos)++] = (uint8_t)(0xC0u | ((cp >> 6) & 0x1Fu));
        out[(*pos)++] = (uint8_t)(0x80u | (cp & 0x3Fu));
    } else {
        out[(*pos)++] = (uint8_t)(0xE0u | ((cp >> 12) & 0x0Fu));
        out[(*pos)++] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[(*pos)++] = (uint8_t)(0x80u | (cp & 0x3Fu));
    }
}

/* Decode 7-bit ISO-2022-{JP,KR,CN} post-XMLDecl bytes into exact UTF-8 in a
 * job-budgeted buffer using ICU 78 Unicode BMP tables (zero synthetic codepoints)
 * so XML 1.0 + Namespaces 1.0 validation matches reference parsers on both
 * literal characters and numeric character references, while preserving the
 * original raw XMP packet bytes verbatim. */
static rfc_status jp2_iso2022_to_utf8(const uint8_t *src,
                                      size_t len,
                                      int mode,
                                      const rfc_allocator *alloc,
                                      uint8_t **out_buf,
                                      size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0u;
    if (len > (SIZE_MAX - 1u) / 3u) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    size_t cap = len * 3u + 1u;
    uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    size_t i = 0u;
    size_t o = 0u;
    int state = 0; /* 0=ASCII; mode1: 1=JIS0208, 2=JIS0212, 3=KATA, 4=JIS-Roman; mode2: 1=KSX1001; mode3: 1=GB2312, 2=CNS-P1 */
    int g1_kind = 0;
    int g2_kind = 0; /* mode1: 1=ISO8859-1, 2=ISO8859-7; mode3: 1=CNS-P2 */

    while (i < len) {
        uint8_t b = src[i];
        if (b >= 0x80u) {
            alloc->free(alloc->ctx, buf);
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (b == 0x1Bu) {
            if (mode == 1) {
                if (i + 2u < len && src[i + 1u] == '(') {
                    uint8_t f = src[i + 2u];
                    if (f == 'B') {
                        state = 0;
                        i += 3u;
                        continue;
                    } else if (f == 'J') {
                        state = 4;
                        i += 3u;
                        continue;
                    } else if (f == 'I') {
                        state = 3;
                        i += 3u;
                        continue;
                    }
                } else if (i + 2u < len && src[i + 1u] == '$' &&
                           (src[i + 2u] == '@' || src[i + 2u] == 'B')) {
                    state = 1;
                    i += 3u;
                    continue;
                } else if (i + 3u < len && src[i + 1u] == '$' &&
                           src[i + 2u] == '(' && src[i + 3u] == 'D') {
                    state = 2;
                    i += 4u;
                    continue;
                } else if (i + 2u < len && src[i + 1u] == '.' &&
                           (src[i + 2u] == 'A' || src[i + 2u] == 'F')) {
                    g2_kind = (src[i + 2u] == 'A') ? 1 : 2;
                    i += 3u;
                    continue;
                } else if (g2_kind != 0 && i + 2u < len && src[i + 1u] == 'N' &&
                           src[i + 2u] >= 0x21u && src[i + 2u] <= 0x7Eu) {
                    const uint16_t *gt = (g2_kind == 1) ? k_jp2_g2_a_u16 : k_jp2_g2_f_u16;
                    uint16_t cp = gt[src[i + 2u] - 0x21u];
                    if (cp == 0u) {
                        alloc->free(alloc->ctx, buf);
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                    jp2_emit_utf8_bmp(buf, &o, cp);
                    i += 3u;
                    continue;
                }
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_INVALID_ARGUMENT;
            } else if (mode == 2) {
                if (i + 3u < len && src[i + 1u] == '$' &&
                    src[i + 2u] == ')' && src[i + 3u] == 'C') {
                    g1_kind = 1;
                    i += 4u;
                    continue;
                }
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_INVALID_ARGUMENT;
            } else {
                if (i + 3u < len && src[i + 1u] == '$' && src[i + 2u] == ')') {
                    if (src[i + 3u] == 'A') {
                        g1_kind = 1;
                        i += 4u;
                        continue;
                    } else if (src[i + 3u] == 'G') {
                        g1_kind = 2;
                        i += 4u;
                        continue;
                    }
                } else if (i + 3u < len && src[i + 1u] == '$' &&
                           src[i + 2u] == '*' && src[i + 3u] == 'H') {
                    g2_kind = 1;
                    i += 4u;
                    continue;
                } else if (g2_kind == 1 && i + 3u < len && src[i + 1u] == 'N') {
                    uint16_t cp = jp2_dbcs_lookup_u16(k_cns_p2_u16, src[i + 2u], src[i + 3u]);
                    if (cp == 0u) {
                        alloc->free(alloc->ctx, buf);
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                    jp2_emit_utf8_bmp(buf, &o, cp);
                    i += 4u;
                    continue;
                }
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
        if ((mode == 2 || mode == 3) && b == 0x0Eu) {
            if (g1_kind == 0 || state != 0) {
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_INVALID_ARGUMENT;
            }
            state = g1_kind;
            i++;
            continue;
        }
        if ((mode == 2 || mode == 3) && b == 0x0Fu) {
            if (state == 0) {
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_INVALID_ARGUMENT;
            }
            state = 0;
            i++;
            continue;
        }
        if (state == 0) {
            buf[o++] = b;
            i++;
        } else if (mode == 1 && state == 4) {
            if (b == 0x5Cu) {
                jp2_emit_utf8_bmp(buf, &o, 0x00A5u);
            } else if (b == 0x7Eu) {
                jp2_emit_utf8_bmp(buf, &o, 0x203Eu);
            } else {
                buf[o++] = b;
            }
            i++;
        } else if (mode == 1 && state == 3) {
            if (b < 0x21u || b > 0x5Fu) {
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_INVALID_ARGUMENT;
            }
            jp2_emit_utf8_bmp(buf, &o, (uint16_t)(0xFF61u + (b - 0x21u)));
            i++;
        } else {
            if (i + 1u >= len) {
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_INVALID_ARGUMENT;
            }
            const uint16_t *tbl = NULL;
            if (mode == 1 && state == 1) {
                tbl = k_jis0208_u16;
            } else if (mode == 1 && state == 2) {
                tbl = k_jis0212_u16;
            } else if (mode == 2 && state == 1) {
                tbl = k_ksx1001_u16;
            } else if (mode == 3 && state == 1) {
                tbl = k_gb2312_u16;
            } else if (mode == 3 && state == 2) {
                tbl = k_cns_p1_u16;
            }
            uint16_t cp = tbl ? jp2_dbcs_lookup_u16(tbl, b, src[i + 1u]) : 0u;
            if (cp == 0u) {
                alloc->free(alloc->ctx, buf);
                return RFC_ERR_INVALID_ARGUMENT;
            }
            jp2_emit_utf8_bmp(buf, &o, cp);
            i += 2u;
        }
    }
    if (state != 0 && !(mode == 1 && state == 4)) {
        alloc->free(alloc->ctx, buf);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_buf = buf;
    *out_len = o;
    return RFC_OK;
}

/* Parse strict W3C XML 1.0 (5th ed.) §2.8 XMLDecl starting at p[*pos]:
 * '<?xml' S VersionInfo EncodingDecl? SDDecl? S? '?>'
 * Also verifies any declared encoding (normalized via jp2_xml_normalize_enc)
 * is compatible with this UTF-8 byte stream (UTF-8 aliases, or Expat-supported
 * 7-bit-ASCII-identical character encodings only when no UTF-8 BOM and all
 * document bytes are < 0x80). */
static int jp2_xml_parse_xmldecl(const uint8_t *p,
                                 size_t len,
                                 int has_bom,
                                 size_t *pos,
                                 int *out_iso2022_mode) {
    if (out_iso2022_mode) {
        *out_iso2022_mode = 0;
    }
    size_t i = *pos;
    if (len - i < 6u || memcmp(p + i, "<?xml", 5u) != 0 ||
        !jp2_is_xml_ws(p[i + 5u])) {
        return 0;
    }
    i += 5u;
    /* VersionInfo ::= S 'version' Eq ("'" VersionNum "'" | '"' VersionNum '"') */
    size_t ws0 = i;
    while (i < len && jp2_is_xml_ws(p[i])) i++;
    if (i == ws0 || len - i < 7u || memcmp(p + i, "version", 7u) != 0) {
        return 0;
    }
    i += 7u;
    while (i < len && jp2_is_xml_ws(p[i])) i++;
    if (i >= len || p[i] != '=') return 0;
    i++;
    while (i < len && jp2_is_xml_ws(p[i])) i++;
    if (i >= len || (p[i] != '"' && p[i] != '\'')) return 0;
    uint8_t q = p[i++];
    if (len - i < 3u || p[i] != '1' || p[i + 1u] != '.') return 0;
    i += 2u;
    size_t d0 = i;
    while (i < len && p[i] >= '0' && p[i] <= '9') i++;
    if (i == d0 || i >= len || p[i] != q) return 0;
    i++; /* closing quote of VersionNum */

    /* Optional EncodingDecl ::= S 'encoding' Eq ('"' EncName '"' | "'" EncName "'") */
    size_t after_ver = i;
    while (i < len && jp2_is_xml_ws(p[i])) i++;
    if (i > after_ver && len - i >= 8u && memcmp(p + i, "encoding", 8u) == 0 &&
        (len - i == 8u || jp2_is_xml_ws(p[i + 8u]) || p[i + 8u] == '=')) {
        i += 8u;
        while (i < len && jp2_is_xml_ws(p[i])) i++;
        if (i >= len || p[i] != '=') return 0;
        i++;
        while (i < len && jp2_is_xml_ws(p[i])) i++;
        if (i >= len || (p[i] != '"' && p[i] != '\'')) return 0;
        q = p[i++];
        size_t enc_start = i;
        if (i >= len ||
            !((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= 'a' && p[i] <= 'z'))) {
            return 0;
        }
        i++;
        while (i < len && p[i] != q) {
            uint8_t c = p[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
                return 0;
            }
            i++;
        }
        if (i >= len || p[i] != q) return 0;
        size_t enc_len = i - enc_start;
        char norm_enc[64];
        if (jp2_xml_normalize_enc(p + enc_start, enc_len,
                                  norm_enc, sizeof(norm_enc)) == SIZE_MAX) {
            return 0;
        }
        /* Sorted normalized UTF-8 and 7-bit-ASCII-compatible encodings across
         * Python/iconv/ICU (normalized by ASCII lowercase + stripping -/_). */
        static const char *const k_utf8_encs[] = {
            "cp1208", "cp65001", "csutf8", "ibm1208", "ibm1209", "ibm13496",
            "ibm13497", "ibm17592", "ibm17593", "ibm5304", "ibm5305",
            "iso10646utf8", "u8", "unicode11utf8", "unicode20utf8", "utf",
            "utf8", "utf8sig", "utf8ucs2", "utf8ucs4", "windows65001",
            "xutf8j"
        };
        static const char *const k_ascii_7bit_encs[] = {
            "ansi1251", "ansix3.4", "ansix3.41968", "ansix3.41986",
            "ansix341968", "arabic", "armscii8", "ascii", "ascii7",
            "asmo708", "baltic", "big5", "big5hk", "big5hkscs", "big5tw",
            "bigfive", "ccsid00858", "cesu8", "cnbig5", "cngb", "cns11643",
            "compoundtext", "cp00858", "cp10007", "cp1004", "cp1006",
            "cp1008", "cp1046", "cp1051", "cp1089", "cp1098", "cp1124",
            "cp1125", "cp1129", "cp1131", "cp1133", "cp1161", "cp1162",
            "cp1163", "cp1167", "cp1250", "cp1251", "cp1252", "cp1253",
            "cp1254", "cp1255", "cp1256", "cp1257", "cp1258", "cp1282",
            "cp1361", "cp1363", "cp1383", "cp1386", "cp154", "cp33722",
            "cp367", "cp437", "cp4909", "cp5347", "cp720", "cp737", "cp770",
            "cp771", "cp772", "cp773", "cp774", "cp775", "cp813", "cp819",
            "cp850", "cp851", "cp852", "cp855", "cp856", "cp857", "cp858",
            "cp860", "cp861", "cp862", "cp863", "cp864", "cp865", "cp866",
            "cp866nav", "cp866u", "cp868", "cp869", "cp874", "cp878",
            "cp891", "cp901", "cp902", "cp903", "cp904", "cp9066", "cp912",
            "cp913", "cp914", "cp915", "cp916", "cp920", "cp921", "cp922",
            "cp923", "cp932", "cp936", "cp943", "cp943c", "cp9448", "cp949",
            "cp949c", "cp950", "cp964", "cp970", "cpar", "cpgr", "cphu",
            "cpibm861", "cpis", "csascii", "csbig5", "csdecmcs", "cseuckr",
            "cseucpkdfmtjapanese", "csgb2312", "cshproman8", "csibm1008",
            "csibm1124", "csibm1129", "csibm1133", "csibm1161",
            "csibm11621162", "csibm1163", "csibm1167", "csibm4909",
            "csibm5347", "csibm851", "csibm855", "csibm856", "csibm857",
            "csibm858", "csibm860", "csibm861", "csibm863", "csibm864",
            "csibm865", "csibm866", "csibm868", "csibm869", "csibm891",
            "csibm901", "csibm902", "csibm903", "csibm904", "csibm9066",
            "csibm921", "csibm922", "csibm932", "csibm943", "csibm9448",
            "csiso10367box", "csiso111ecmacyrillic", "csiso143iecp271",
            "csiso153gost1976874", "csisolatin0", "csisolatin1",
            "csisolatin2", "csisolatin3", "csisolatin4", "csisolatin5",
            "csisolatin6", "csisolatin9", "csisolatinarabic",
            "csisolatincyrillic", "csisolatingreek", "csisolatinhebrew",
            "cskoi8r", "csksc56011987", "csmacintosh", "cspc775baltic",
            "cspc850multilingual", "cspc851", "cspc858multilingual",
            "cspc862latinhebrew", "cspc8codepage437", "cspcp852", "cspcp855",
            "csptcp154", "csshiftjis", "cswindows31j", "cwi", "cwi2",
            "cyrillic", "cyrillicasian", "dec", "decmcs", "dos720", "dos862",
            "ecma114", "ecma118", "ecma128", "ecmacyrillic", "elot928",
            "euccn", "eucgb2312cn", "eucjis", "eucjis2004", "eucjisx0213",
            "eucjp", "eucjp2007", "eucjpms", "eucjpopen", "eucjpwin",
            "euckr", "eucth", "euctw", "euctw2014",
            "extendedunixcodepackedformatforjapanese", "gb13000", "gb18030",
            "gb180302000", "gb180302022", "gb2312", "gbk", "georgianacademy",
            "georgianps", "gost19768", "gost1976874", "greek", "greek8",
            "hebrew", "hebrew8", "hkbig5", "hkscs", "hkscsbig5", "hp15cn",
            "hpgreek8", "hproman8", "hproman9", "hpthai8", "hpturkish8",
            "ibm00858", "ibm1004", "ibm1006", "ibm1006p1001995", "ibm1008",
            "ibm1046", "ibm1051", "ibm1051p1001995", "ibm1089",
            "ibm1089p1001995", "ibm1098", "ibm1098p1001995", "ibm1124",
            "ibm1124p1001996", "ibm1125", "ibm1125p1001997", "ibm1129",
            "ibm1129p1001997", "ibm1131", "ibm1131p1001997", "ibm1133",
            "ibm1133p1001997", "ibm1161", "ibm1162", "ibm1162p1001999",
            "ibm1163", "ibm1167", "ibm1168", "ibm1168p1002002", "ibm1212",
            "ibm1213", "ibm1250", "ibm1250p1001995", "ibm1251",
            "ibm1251p1001995", "ibm1252", "ibm1252p1002000", "ibm1253",
            "ibm1253p1001995", "ibm1254", "ibm1254p1001995", "ibm1255",
            "ibm1255p1001995", "ibm1256", "ibm1256p1101997", "ibm1257",
            "ibm1257p1001995", "ibm1258", "ibm1258p1001997", "ibm1363",
            "ibm1363p1101997", "ibm1363p11b1998", "ibm1363vasciivsubvpua",
            "ibm1363vsubvpua", "ibm1373", "ibm1373p1002002", "ibm1375",
            "ibm1375p1002008", "ibm1383", "ibm1383p1101999", "ibm1383vpua",
            "ibm1386", "ibm1386p1002001", "ibm1386vsubvpua", "ibm1392",
            "ibm33722", "ibm33722p1201999", "ibm33722p12ap12a2009u2",
            "ibm33722vasciivpua", "ibm33722vpua", "ibm367", "ibm437",
            "ibm437p1001995", "ibm4902", "ibm4909", "ibm4909p1001999",
            "ibm5012", "ibm5012p1001999", "ibm5050", "ibm5346",
            "ibm5346p1001998", "ibm5347", "ibm5347p1001998", "ibm5348",
            "ibm5348p1001997", "ibm5349", "ibm5349p1001998", "ibm5350",
            "ibm5350p1001998", "ibm5351", "ibm5351p1001998", "ibm5352",
            "ibm5352p1001998", "ibm5353", "ibm5353p1001998", "ibm5354",
            "ibm5354p1001998", "ibm5471", "ibm5471p1002006", "ibm65025",
            "ibm720", "ibm720p1001997", "ibm737", "ibm737p1001997", "ibm775",
            "ibm775p1001996", "ibm813", "ibm813p1001995", "ibm819", "ibm848",
            "ibm850", "ibm850p1001995", "ibm851", "ibm851p1001995", "ibm852",
            "ibm852p1001995", "ibm855", "ibm855p1001995", "ibm856",
            "ibm856p1001995", "ibm857", "ibm857p1001995", "ibm858",
            "ibm858p1001997", "ibm860", "ibm860p1001995", "ibm861",
            "ibm861p1001995", "ibm862", "ibm862p1001995", "ibm863",
            "ibm863p1001995", "ibm864", "ibm864x1101999", "ibm865",
            "ibm865p1001995", "ibm866", "ibm866nav", "ibm866p1001995",
            "ibm867", "ibm867p1001998", "ibm868", "ibm868p1001995", "ibm869",
            "ibm869p1001995", "ibm874", "ibm874p1001995", "ibm878",
            "ibm878p1001996", "ibm891", "ibm9005", "ibm9005x1102007",
            "ibm901", "ibm901p1001999", "ibm902", "ibm902p1001999", "ibm903",
            "ibm904", "ibm9066", "ibm912", "ibm912p1001995", "ibm913",
            "ibm913p1002000", "ibm914", "ibm914p1001995", "ibm915",
            "ibm915p1001995", "ibm916", "ibm916p1001995", "ibm920",
            "ibm920p1001995", "ibm921", "ibm921p1001995", "ibm922",
            "ibm922p1001999", "ibm923", "ibm923p1001998", "ibm932",
            "ibm932vsubvpua", "ibm9400", "ibm942", "ibm942p12a1999",
            "ibm942vsubvpua", "ibm943", "ibm943c", "ibm943p1301999",
            "ibm943p15a2003", "ibm943vasciivsubvpua", "ibm943vsubvpua",
            "ibm9447", "ibm9447p1002002", "ibm9448", "ibm9448x1002005",
            "ibm9449", "ibm9449p1002002", "ibm949", "ibm949c",
            "ibm949p1101999", "ibm949p11a1999", "ibm949vasciivsubvpua",
            "ibm949vsubvpua", "ibm950", "ibm950p1101999", "ibm954",
            "ibm954p1012007", "ibm964", "ibm964p1101999", "ibm964vpua",
            "ibm970", "ibm970p110p1102006u2", "ibm970vpua", "ibmeuccn",
            "ibmeucjp", "ibmeuckr", "ibmeuctw", "iecp271", "isciibng",
            "isciidev", "isciiguj", "isciigur", "isciiknd", "isciimlm",
            "isciiori", "isciitlg", "isciitml", "isiri3342", "iso10367box",
            "iso646.irv1991", "iso646us", "iso6937", "iso8859", "iso88591",
            "iso885910", "iso8859101992", "iso8859101998", "iso885911",
            "iso8859112001", "iso885911987", "iso885913", "iso885914",
            "iso8859141998", "iso885915", "iso885915fdis", "iso885916",
            "iso8859162001", "iso88592", "iso885921987", "iso88593",
            "iso885931988", "iso88594", "iso885941988", "iso88595",
            "iso885951988", "iso88596", "iso885961987", "iso88596e",
            "iso88596i", "iso88597", "iso885971987", "iso88598",
            "iso885981988", "iso88598e", "iso88598i", "iso88599",
            "iso885991989", "iso88599e", "isoceltic", "isoir100", "isoir101",
            "isoir109", "isoir110", "isoir111", "isoir126", "isoir127",
            "isoir138", "isoir143", "isoir144", "isoir148", "isoir149",
            "isoir153", "isoir155", "isoir156", "isoir157", "isoir166",
            "isoir179", "isoir193", "isoir197", "isoir199", "isoir203",
            "isoir209", "isoir226", "isoir6", "jisx0213", "johab", "koi8",
            "koi8r", "koi8ru", "koi8t", "koi8u", "korean", "ksc", "ksc5601",
            "ksc56011987", "ksc56011989", "ksx1001", "kz1048", "l1", "l10",
            "l2", "l3", "l4", "l5", "l6", "l7", "l8", "l9", "latin",
            "latin0", "latin1", "latin10", "latin2", "latin3", "latin4",
            "latin5", "latin6", "latin7", "latin8", "latin9", "lmbcs",
            "lmbcs1", "mac", "macarabic", "macce", "maccenteuro",
            "maccentraleurope", "maccroatian", "maccy", "maccyrillic",
            "macfarsi", "macgr", "macgreek", "maciceland", "macintosh",
            "macis", "maclatin2", "macos0210.2", "macos2910.2",
            "macos3510.2", "macos6210.4", "macos7310.2", "macroman",
            "macromanian", "macsami", "mactr", "macturkish", "macuk",
            "macukrainian", "mik", "ms1361", "ms874", "ms932", "ms936",
            "ms949", "ms950", "ms950hkscs", "msansi", "msarab", "mscp949",
            "mscyrl", "msee", "msgreek", "mshebr", "mskanji",
            "msmaccyrillic", "msturk", "os2latin1", "osf00010001",
            "osf00010002", "osf00010003", "osf00010004", "osf00010005",
            "osf00010006", "osf00010007", "osf00010008", "osf00010009",
            "osf0001000a", "osf00010020", "osf00030010", "osf0004000a",
            "osf0005000a", "osf05010001", "osf10010001", "osf10010004",
            "osf10010006", "osf100201b5", "osf10020352", "osf10020354",
            "osf10020357", "osf10020359", "osf1002035d", "osf1002035e",
            "osf1002035f", "osf10020364", "osf10020365", "osf1002037b",
            "osf10020387", "osf10020388", "osf100203b5", "palmos", "pck",
            "pt154", "ptcp154", "r8", "r9", "rawunicodeescape", "rk1048",
            "roman8", "roman9", "ruscii", "scsu", "shiftjis", "shiftjis78",
            "sjis", "sjis78", "sjisopen", "sjiswin", "strk10482002",
            "stsev35888", "suneugreek", "thai", "thai8", "tis620",
            "tis620.25291", "tis620.2533", "tis620.25330", "tis6200",
            "tis62025290", "tis62025291", "ts5881", "tscii", "turkish",
            "turkish8", "uhc", "ujis", "unicodeescape", "us", "usascii",
            "viscii", "winbaltrim", "windows10000", "windows10006",
            "windows10007", "windows10029", "windows10081", "windows1250",
            "windows1251", "windows1252", "windows1253", "windows1254",
            "windows1255", "windows1256", "windows1257", "windows1258",
            "windows20127", "windows20866", "windows21866", "windows28592",
            "windows28593", "windows28594", "windows28595", "windows28596",
            "windows28597", "windows28598", "windows28599", "windows28603",
            "windows28605", "windows31j", "windows437", "windows51949",
            "windows54936", "windows57002", "windows57003", "windows57004",
            "windows57005", "windows57006", "windows57007", "windows57008",
            "windows57009", "windows57010", "windows57011", "windows720",
            "windows737", "windows775", "windows850", "windows852",
            "windows855", "windows857", "windows858", "windows861",
            "windows862", "windows866", "windows869", "windows874",
            "windows8742000", "windows932", "windows936", "windows9362000",
            "windows949", "windows9492000", "windows950", "windows9502000",
            "winsami2", "ws2", "x11compoundtext", "xbig5", "xcompoundtext",
            "xeucjp", "xibm1006", "xibm1098", "xibm1124", "xibm1363",
            "xibm1363c", "xibm33722", "xibm33722a", "xibm33722c", "xibm720",
            "xibm737", "xibm856", "xibm867", "xibm874", "xibm921", "xibm922",
            "xibm942", "xibm942c", "xibm943", "xibm949", "xibm949c",
            "xibm950", "xibm954", "xibm954c", "xibm964", "xibm970",
            "xiscii91", "xisciias", "xisciibe", "xisciide", "xisciigu",
            "xisciika", "xisciima", "xisciior", "xisciipa", "xisciita",
            "xisciite", "xiso885911", "xiso88596s", "xjisautodetect",
            "xksc5601", "xmacce", "xmaccentraleurope", "xmaccentraleurroman",
            "xmaccyrillic", "xmacgreek", "xmacjapanese", "xmackorean",
            "xmacroman", "xmacsimpchinese", "xmactradchinese", "xmacturkish",
            "xmacukraine", "xms9320213", "xms950hkscs", "xmscp932",
            "xroman8", "xsjis", "xwindows1256s", "xwindows874", "xwindows950"
        };
        static const char *const k_iso2022_jp_encs[] = {
            "csiso2022jp", "csiso2022jp2", "csjisencoding", "ibm5054",
            "iso2022jp", "iso2022jp1", "iso2022jp2", "iso2022jp2004",
            "iso2022jp3", "iso2022jpext", "jis", "jis7", "jis8",
            "jisencoding", "xwindows50220", "xwindows50221",
            "xwindowsiso2022jp"
        };
        static const char *const k_iso2022_kr_encs[] = {
            "csiso2022kr", "ibm25546", "iso2022kr"
        };
        static const char *const k_iso2022_cn_encs[] = {
            "csiso2022cn", "iso2022cn", "iso2022cncns", "iso2022cnext",
            "xiso2022cncns", "xiso2022cngb"
        };
        int iso_mode = 0;
        if (jp2_enc_norm_in_sorted_list(
                norm_enc, k_utf8_encs,
                sizeof(k_utf8_encs) / sizeof(k_utf8_encs[0]))) {
            /* UTF-8 matches this UTF-8 byte stream (with or without UTF-8 BOM). */
        } else if (jp2_enc_norm_in_sorted_list(
                       norm_enc, k_ascii_7bit_encs,
                       sizeof(k_ascii_7bit_encs) /
                           sizeof(k_ascii_7bit_encs[0]))) {
            if (has_bom) {
                return 0;
            }
            for (size_t k = 0u; k < len; k++) {
                if (p[k] >= 0x80u) {
                    return 0;
                }
            }
        } else {
            if (jp2_enc_norm_in_sorted_list(
                    norm_enc, k_iso2022_jp_encs,
                    sizeof(k_iso2022_jp_encs) / sizeof(k_iso2022_jp_encs[0]))) {
                iso_mode = 1;
            } else if (jp2_enc_norm_in_sorted_list(
                           norm_enc, k_iso2022_kr_encs,
                           sizeof(k_iso2022_kr_encs) /
                               sizeof(k_iso2022_kr_encs[0]))) {
                iso_mode = 2;
            } else if (jp2_enc_norm_in_sorted_list(
                           norm_enc, k_iso2022_cn_encs,
                           sizeof(k_iso2022_cn_encs) /
                               sizeof(k_iso2022_cn_encs[0]))) {
                iso_mode = 3;
            } else {
                return 0; /* encoding contradicts or is unsupported on this byte stream */
            }
            if (has_bom) {
                return 0;
            }
            for (size_t k = 0u; k < len; k++) {
                if (p[k] >= 0x80u) {
                    return 0;
                }
            }
            if (out_iso2022_mode) {
                *out_iso2022_mode = iso_mode;
            }
        }
        i++;
        after_ver = i;
        while (i < len && jp2_is_xml_ws(p[i])) i++;
    }

    /* Optional SDDecl ::= S 'standalone' Eq (("'" ('yes'|'no') "'") | ...) */
    if (i > after_ver && len - i >= 10u && memcmp(p + i, "standalone", 10u) == 0 &&
        (len - i == 10u || jp2_is_xml_ws(p[i + 10u]) || p[i + 10u] == '=')) {
        i += 10u;
        while (i < len && jp2_is_xml_ws(p[i])) i++;
        if (i >= len || p[i] != '=') return 0;
        i++;
        while (i < len && jp2_is_xml_ws(p[i])) i++;
        if (i >= len || (p[i] != '"' && p[i] != '\'')) return 0;
        q = p[i++];
        if (len - i >= 3u && memcmp(p + i, "yes", 3u) == 0) {
            i += 3u;
        } else if (len - i >= 2u && memcmp(p + i, "no", 2u) == 0) {
            i += 2u;
        } else {
            return 0;
        }
        if (i >= len || p[i] != q) return 0;
        i++;
        while (i < len && jp2_is_xml_ws(p[i])) i++;
    }

    if (len - i < 2u || p[i] != '?' || p[i + 1u] != '>') {
        return 0;
    }
    *pos = i + 2u;
    return 1;
}

/* Parse an XML start-tag with full W3C XML 1.0 + Namespaces 1.0 scoping,
 * unbounded dynamic attribute storage, hash-table duplicate checks (both raw
 * QName and expanded {uri}local), and prefix binding verification. */
static int jp2_xml_parse_start_tag(jp2_xml_parser *xp,
                                   const uint8_t *p,
                                   size_t len,
                                   size_t *pos,
                                   int check_xmp_root,
                                   size_t *out_qname_off,
                                   size_t *out_qname_len,
                                   int *out_self_closing) {
    size_t i = *pos;
    if (i >= len || p[i] != '<') {
        return 0;
    }
    i++; /* skip '<' */
    size_t qname_start = i;
    size_t colon_pos = SIZE_MAX;
    while (i < len && !jp2_is_xml_ws(p[i]) && p[i] != '>' && p[i] != '/') {
        uint8_t ch = p[i];
        if (ch == '<' || ch == '"' || ch == '\'' || ch == '=') {
            return 0;
        }
        if (ch == ':') {
            if (colon_pos != SIZE_MAX) {
                return 0;
            }
            colon_pos = i;
        }
        i++;
    }
    size_t qname_len = i - qname_start;
    if (i >= len || !jp2_xml_valid_qname(p + qname_start, qname_len) ||
        (!jp2_is_xml_ws(p[i]) && p[i] != '>' && p[i] != '/')) {
        return 0;
    }
    const uint8_t *el_prefix = NULL;
    size_t el_prefix_len = 0u;
    const uint8_t *el_local = p + qname_start;
    size_t el_local_len = qname_len;
    if (colon_pos != SIZE_MAX) {
        el_prefix = p + qname_start;
        el_prefix_len = colon_pos - qname_start;
        el_local = p + colon_pos + 1u;
        el_local_len = i - (colon_pos + 1u);
        if (el_prefix_len == 5u && memcmp(el_prefix, "xmlns", 5u) == 0) {
            return 0;
        }
    }

    xp->attr_count = 0u;
    int self_closing = 0;
    while (i < len) {
        size_t ws_start = i;
        while (i < len && jp2_is_xml_ws(p[i])) {
            i++;
        }
        if (i >= len) {
            return 0;
        }
        if (p[i] == '>') {
            i++;
            self_closing = 0;
            break;
        }
        if (p[i] == '/' && i + 1u < len && p[i + 1u] == '>') {
            i += 2u;
            self_closing = 1;
            break;
        }
        if (i == ws_start) {
            return 0; /* XML 1.0 requires whitespace before every attribute */
        }
        size_t name_start = i;
        size_t a_colon = SIZE_MAX;
        while (i < len && !jp2_is_xml_ws(p[i]) && p[i] != '=' &&
               p[i] != '>' && p[i] != '/' && p[i] != '"' && p[i] != '\'') {
            if (p[i] == ':' && a_colon == SIZE_MAX) {
                a_colon = i;
            }
            i++;
        }
        size_t name_len = i - name_start;
        if (!jp2_xml_valid_qname(p + name_start, name_len)) {
            return 0;
        }
        while (i < len && jp2_is_xml_ws(p[i])) i++;
        if (i >= len || p[i] != '=') return 0;
        i++;
        while (i < len && jp2_is_xml_ws(p[i])) i++;
        if (i >= len || (p[i] != '"' && p[i] != '\'')) return 0;
        uint8_t quote = p[i++];
        size_t val_start = i;
        while (i < len && p[i] != quote) i++;
        if (i >= len) return 0;
        size_t val_len = i - val_start;
        i++; /* skip closing quote */
        if (i < len && !jp2_is_xml_ws(p[i]) && p[i] != '>' && p[i] != '/') {
            return 0;
        }

        jp2_xml_attr a;
        memset(&a, 0, sizeof(a));
        a.name_off = name_start;
        a.name_len = name_len;
        a.val_off = val_start;
        a.val_len = val_len;
        if (a_colon != SIZE_MAX) {
            a.prefix_off = name_start;
            a.prefix_len = a_colon - name_start;
            a.local_off = a_colon + 1u;
            a.local_len = (name_start + name_len) - (a_colon + 1u);
            if (a.prefix_len == 5u && memcmp(p + a.prefix_off, "xmlns", 5u) == 0) {
                a.is_xmlns = 1;
            }
        } else {
            a.local_off = name_start;
            a.local_len = name_len;
            if (name_len == 5u && memcmp(p + name_start, "xmlns", 5u) == 0) {
                a.is_xmlns = 1;
            }
        }
        if (!jp2_xml_attr_push(xp, &a)) {
            return 0;
        }
    }

    /* Check raw QName uniqueness across ALL attributes (unbounded). */
    if (xp->attr_count > 1u) {
        if (!jp2_xml_ht_prepare(xp, xp->attr_count)) {
            return 0;
        }
        size_t mask = 32u;
        while (mask < xp->attr_count * 2u + 8u) mask *= 2u;
        mask -= 1u;
        for (size_t k = 0u; k < xp->attr_count; k++) {
            const jp2_xml_attr *a = &xp->attrs[k];
            uint64_t h = jp2_xml_fnv1a(14695981039346656037ull,
                                       p + a->name_off, a->name_len);
            size_t slot = (size_t)h & mask;
            while (xp->ht[slot] != SIZE_MAX) {
                const jp2_xml_attr *prev = &xp->attrs[xp->ht[slot]];
                if (prev->name_len == a->name_len &&
                    memcmp(p + prev->name_off, p + a->name_off, a->name_len) == 0) {
                    return 0; /* duplicate attribute QName */
                }
                slot = (slot + 1u) & mask;
            }
            xp->ht[slot] = k;
        }
    }

    /* Pass 1: validate all attribute values and push xmlns bindings at xp->depth. */
    size_t cur_depth = xp->depth;
    for (size_t k = 0u; k < xp->attr_count; k++) {
        const jp2_xml_attr *a = &xp->attrs[k];
        if (!a->is_xmlns) {
            if (!jp2_xml_decode_attr_val(p + a->val_off, a->val_len,
                                         NULL, NULL, NULL)) {
                return 0;
            }
            continue;
        }
        size_t uri_off = xp->uri_arena.len;
        if (!jp2_xml_decode_attr_val(p + a->val_off, a->val_len,
                                     &xp->uri_arena, xp->alloc, &xp->oom)) {
            return 0;
        }
        size_t uri_len = xp->uri_arena.len - uri_off;
        const uint8_t *uri = uri_len ? (xp->uri_arena.data + uri_off) : NULL;
        int is_xml_uri = (uri_len == sizeof(k_xml_ns_uri) - 1u &&
                          memcmp(uri, k_xml_ns_uri, uri_len) == 0);
        int is_xmlns_uri = (uri_len == sizeof(k_xmlns_ns_uri) - 1u &&
                            memcmp(uri, k_xmlns_ns_uri, uri_len) == 0);
        if (a->prefix_len == 0u) {
            /* Default namespace: xmlns="..." */
            if (is_xml_uri || is_xmlns_uri) {
                return 0;
            }
            if (!jp2_xml_ns_push(xp, cur_depth, 0u, 0u, uri_off, uri_len)) {
                return 0;
            }
        } else {
            /* Prefixed binding: xmlns:<local>="..." */
            const uint8_t *bpref = p + a->local_off;
            size_t bplen = a->local_len;
            if (uri_len == 0u ||
                (bplen == 5u && memcmp(bpref, "xmlns", 5u) == 0)) {
                return 0; /* Namespaces 1.0 forbids empty prefixed binding and xmlns */
            }
            if (bplen == 3u && memcmp(bpref, "xml", 3u) == 0) {
                if (!is_xml_uri) {
                    return 0;
                }
            } else if (is_xml_uri || is_xmlns_uri) {
                return 0;
            }
            if (!jp2_xml_ns_push(xp, cur_depth, a->local_off, bplen, uri_off, uri_len)) {
                return 0;
            }
        }
    }

    /* Pass 2: resolve element prefix and check XMP root expanded name. */
    const uint8_t *el_uri = NULL;
    size_t el_uri_len = 0u;
    if (!jp2_xml_resolve_ns(xp, p, el_prefix, el_prefix_len, &el_uri, &el_uri_len)) {
        return 0; /* unbound element prefix */
    }
    if (check_xmp_root) {
        if (el_local_len != 7u || memcmp(el_local, "xmpmeta", 7u) != 0 ||
            el_uri_len != sizeof(k_xmp_ns_uri) - 1u ||
            memcmp(el_uri, k_xmp_ns_uri, el_uri_len) != 0) {
            return 0;
        }
    }

    /* Pass 3: resolve prefixed non-xmlns attributes and check expanded-name uniqueness. */
    size_t non_xmlns_count = 0u;
    for (size_t k = 0u; k < xp->attr_count; k++) {
        jp2_xml_attr *a = &xp->attrs[k];
        if (a->is_xmlns) {
            continue;
        }
        if (a->prefix_len > 0u) {
            if (!jp2_xml_resolve_ns(xp, p, p + a->prefix_off, a->prefix_len,
                                    &a->ns_uri, &a->ns_uri_len)) {
                return 0; /* unbound attribute prefix */
            }
        }
        non_xmlns_count++;
    }
    if (non_xmlns_count > 1u) {
        if (!jp2_xml_ht_prepare(xp, non_xmlns_count)) {
            return 0;
        }
        size_t mask = 32u;
        while (mask < non_xmlns_count * 2u + 8u) mask *= 2u;
        mask -= 1u;
        for (size_t k = 0u; k < xp->attr_count; k++) {
            const jp2_xml_attr *a = &xp->attrs[k];
            if (a->is_xmlns) {
                continue;
            }
            uint64_t h = 14695981039346656037ull;
            if (a->ns_uri_len > 0u) {
                h = jp2_xml_fnv1a(h, a->ns_uri, a->ns_uri_len);
            }
            uint8_t sep = 0u;
            h = jp2_xml_fnv1a(h, &sep, 1u);
            h = jp2_xml_fnv1a(h, p + a->local_off, a->local_len);
            size_t slot = (size_t)h & mask;
            while (xp->ht[slot] != SIZE_MAX) {
                const jp2_xml_attr *prev = &xp->attrs[xp->ht[slot]];
                if (prev->ns_uri_len == a->ns_uri_len &&
                    prev->local_len == a->local_len &&
                    (a->ns_uri_len == 0u ||
                     memcmp(prev->ns_uri, a->ns_uri, a->ns_uri_len) == 0) &&
                    memcmp(p + prev->local_off, p + a->local_off, a->local_len) == 0) {
                    return 0; /* duplicate expanded attribute name */
                }
                slot = (slot + 1u) & mask;
            }
            xp->ht[slot] = k;
        }
    }

    if (self_closing) {
        jp2_xml_ns_pop_depth(xp, cur_depth);
    }
    *pos = i;
    *out_qname_off = qname_start;
    *out_qname_len = qname_len;
    *out_self_closing = self_closing;
    return 1;
}

static rfc_status jp2_classify_xmp_xml(const uint8_t *p,
                                       size_t len,
                                       const rfc_allocator *alloc,
                                       int *out_is_xmp) {
    *out_is_xmp = 0;
    if (!p || len < 11u) {
        return RFC_OK;
    }
    if (!alloc || !alloc->alloc || !alloc->free) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t i = 0u;
    int has_bom = 0;
    if (len >= 3u && p[0] == 0xEFu && p[1] == 0xBBu && p[2] == 0xBFu) {
        i = 3u;
        has_bom = 1;
    }
    /* Optional XMLDecl strictly at document start (after optional UTF-8 BOM). */
    int iso2022_mode = 0;
    if (len - i >= 6u && memcmp(p + i, "<?xml", 5u) == 0 &&
        jp2_is_xml_ws(p[i + 5u])) {
        if (!jp2_xml_parse_xmldecl(p, len, has_bom, &i, &iso2022_mode)) {
            return RFC_OK;
        }
    }

    jp2_xml_parser xp;
    memset(&xp, 0, sizeof(xp));
    xp.alloc = alloc;
    uint8_t *iso_decoded = NULL;
    int ok = 0;

    if (iso2022_mode != 0) {
        int has_shift = 0;
        for (size_t k = i; k < len; k++) {
            if (p[k] == 0x1Bu ||
                ((iso2022_mode == 2 || iso2022_mode == 3) &&
                 (p[k] == 0x0Eu || p[k] == 0x0Fu))) {
                has_shift = 1;
                break;
            }
        }
        if (has_shift) {
            size_t dec_len = 0u;
            rfc_status ist = jp2_iso2022_to_utf8(p + i, len - i, iso2022_mode,
                                                 alloc, &iso_decoded, &dec_len);
            if (ist == RFC_ERR_OUT_OF_MEMORY) {
                return RFC_ERR_OUT_OF_MEMORY;
            }
            if (ist != RFC_OK) {
                goto done;
            }
            p = iso_decoded;
            len = dec_len;
            i = 0u;
        }
    }
    /* Prolog Misc*: whitespace, comments, and any legal XML PI (including
     * <?xpacket ...?> and <?xml-stylesheet ...?>). */
    if (!jp2_xml_skip_misc(p, len, &i)) {
        goto done;
    }

    size_t root_off = 0u, root_len = 0u;
    int root_self = 0;
    if (!jp2_xml_parse_start_tag(&xp, p, len, &i, 1, &root_off, &root_len,
                                 &root_self)) {
        goto done;
    }
    if (!root_self) {
        if (!jp2_xml_stack_push(&xp, root_off, root_len)) {
            goto done;
        }
    }
    /* Walk the complete element tree to verify balanced closing tags, scoped
     * namespaces, and XML 1.0 well-formedness through EOF. */
    while (xp.depth > 0u) {
        if (i >= len) {
            goto done; /* truncated before closing root element */
        }
        if (p[i] == '<') {
            size_t rem = len - i;
            if (rem >= 2u && p[i + 1u] == '/') {
                i += 2u;
                size_t en_start = i;
                while (i < len && !jp2_is_xml_ws(p[i]) && p[i] != '>') {
                    i++;
                }
                size_t en_len = i - en_start;
                while (i < len && jp2_is_xml_ws(p[i])) {
                    i++;
                }
                if (i >= len || p[i] != '>') {
                    goto done;
                }
                i++; /* skip '>' */
                size_t top_idx = xp.depth - 1u;
                if (en_len != xp.stack[top_idx].qname_len ||
                    memcmp(p + en_start, p + xp.stack[top_idx].qname_off, en_len) != 0) {
                    goto done; /* mismatched closing tag */
                }
                xp.depth--;
                jp2_xml_ns_pop_depth(&xp, xp.depth);
            } else if (rem >= 4u && memcmp(p + i, "<!--", 4u) == 0) {
                if (!jp2_xml_skip_comment(p, len, &i)) {
                    goto done;
                }
            } else if (rem >= 2u && p[i + 1u] == '?') {
                if (!jp2_xml_skip_pi(p, len, &i)) {
                    goto done;
                }
            } else if (rem >= 9u && memcmp(p + i, "<![CDATA[", 9u) == 0) {
                i += 9u;
                int closed = 0;
                while (i < len) {
                    if (p[i] == ']' && i + 2u < len &&
                        p[i + 1u] == ']' && p[i + 2u] == '>') {
                        i += 3u;
                        closed = 1;
                        break;
                    }
                    uint32_t cp = 0u;
                    if (!jp2_utf8_next(p, len, &i, &cp) || !jp2_xml_is_char(cp)) {
                        goto done;
                    }
                }
                if (!closed) {
                    goto done;
                }
            } else {
                size_t cn_off = 0u, cn_len = 0u;
                int cn_self = 0;
                if (!jp2_xml_parse_start_tag(&xp, p, len, &i, 0, &cn_off, &cn_len,
                                             &cn_self)) {
                    goto done;
                }
                if (!cn_self) {
                    if (!jp2_xml_stack_push(&xp, cn_off, cn_len)) {
                        goto done;
                    }
                }
            }
        } else if (p[i] == '&') {
            /* Validate entity or numeric character reference in text content. */
            size_t ref_start = i;
            i++;
            while (i < len && p[i] != ';') {
                if (p[i] == '<' || p[i] == '&') {
                    goto done;
                }
                i++;
            }
            if (i >= len) {
                goto done;
            }
            i++; /* include ';' */
            if (!jp2_xml_decode_attr_val(p + ref_start, i - ref_start,
                                         NULL, NULL, NULL)) {
                goto done;
            }
        } else {
            if (p[i] == ']' && i + 2u < len &&
                p[i + 1u] == ']' && p[i + 2u] == '>') {
                goto done; /* ']]>' is forbidden in CharData */
            }
            uint32_t cp = 0u;
            if (!jp2_utf8_next(p, len, &i, &cp) || !jp2_xml_is_char(cp)) {
                goto done;
            }
        }
    }
    /* Epilog Misc*: whitespace, comments, and legal PIs (e.g. <?xpacket end="w"?>). */
    if (!jp2_xml_skip_misc(p, len, &i)) {
        goto done;
    }
    ok = (i == len) ? 1 : 0;

done:
    {
        int was_oom = xp.oom;
        jp2_xml_parser_free(&xp);
        if (iso_decoded) {
            alloc->free(alloc->ctx, iso_decoded);
        }
        if (was_oom) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        *out_is_xmp = ok;
        return RFC_OK;
    }
}

static rfc_status jp2_scan_input(const uint8_t *bytes,
                                 size_t size,
                                 const rfc_limits *limits,
                                 const rfc_allocator *alloc,
                                 jp2_scan_info *out) {
    memset(out, 0, sizeof(*out));
    if (!bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < 4u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    /* Bare J2K codestream? */
    if (bytes[0] == 0xFFu && bytes[1] == 0x4Fu &&
        bytes[2] == 0xFFu && bytes[3] == 0x51u) {
        out->is_jp2 = 0;
        return jp2_scan_codestream(bytes, size, 0, 0, 0u, 0u, 0u, 0u, out);
    }
    /* JP2 container signature? */
    if (size < 12u || memcmp(bytes, k_jp2_sig, 12u) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    out->is_jp2 = 1;

    size_t off = 12u;
    int saw_ftyp = 0;
    int saw_jp2h = 0;
    int saw_ihdr = 0;
    int saw_jp2c = 0;
    uint32_t ihdr_w = 0u, ihdr_h = 0u;
    uint16_t ihdr_nc = 0u;
    uint8_t ihdr_bpc = 0u;
    uint8_t ihdr_prec = 0u;
    int has_colr_winner = 0;
    int has_enumcs = 0;
    uint32_t enumcs = 0u;

    while (off < size) {
        uint32_t tbox = 0u;
        size_t poff = 0u, plen = 0u, next = 0u;
        rfc_status st =
            jp2_read_box(bytes, size, off, &tbox, &poff, &plen, &next);
        if (st != RFC_OK) {
            return st;
        }
        if (tbox == 0x66747970u) { /* ftyp */
            if (plen < 8u) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            saw_ftyp = 1;
        } else if (tbox == 0x6A703268u) { /* jp2h */
            if (saw_jp2h) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            saw_jp2h = 1;
            out->jp2h_len = plen;
            size_t sub = poff;
            size_t sub_end = poff + plen;
            int saw_bpcc = 0;
            while (sub < sub_end) {
                uint32_t stype = 0u;
                size_t spoff = 0u, splen = 0u, snext = 0u;
                st = jp2_read_box(bytes, sub_end, sub, &stype, &spoff, &splen,
                                  &snext);
                if (st != RFC_OK) {
                    return st;
                }
                if (stype == 0x69686472u) { /* ihdr */
                    if (saw_ihdr || splen != 14u) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                    ihdr_h = jp2_be32(bytes + spoff);
                    ihdr_w = jp2_be32(bytes + spoff + 4u);
                    ihdr_nc = jp2_be16(bytes + spoff + 8u);
                    ihdr_bpc = bytes[spoff + 10u];
                    uint8_t comp_t = bytes[spoff + 11u];
                    if (ihdr_w == 0u || ihdr_h == 0u || ihdr_nc == 0u ||
                        comp_t != 7u) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                    if (ihdr_nc != 1u && ihdr_nc != 3u && ihdr_nc != 4u) {
                        return RFC_ERR_UNSUPPORTED_SAMPLE;
                    }
                    if (ihdr_bpc != 0xFFu) {
                        if ((ihdr_bpc & 0x80u) != 0u ||
                            ((ihdr_bpc & 0x7Fu) + 1u) > 16u) {
                            return RFC_ERR_UNSUPPORTED_SAMPLE;
                        }
                        ihdr_prec = (uint8_t)((ihdr_bpc & 0x7Fu) + 1u);
                    }
                    saw_ihdr = 1;
                } else if (stype == 0x62706363u) { /* bpcc */
                    if (!saw_ihdr || saw_bpcc || ihdr_bpc != 0xFFu ||
                        splen != (size_t)ihdr_nc) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                    saw_bpcc = 1;
                    uint8_t b0 = bytes[spoff];
                    for (size_t bi = 0u; bi < splen; bi++) {
                        uint8_t bb = bytes[spoff + bi];
                        if ((bb & 0x80u) != 0u || ((bb & 0x7Fu) + 1u) > 16u ||
                            bb != b0) {
                            return RFC_ERR_UNSUPPORTED_SAMPLE;
                        }
                    }
                    ihdr_prec = (uint8_t)((b0 & 0x7Fu) + 1u);
                } else if (stype == 0x70636C72u || stype == 0x636D6170u) {
                    /* pclr / cmap (paletted JP2) */
                    return RFC_ERR_UNSUPPORTED_COMBINATION;
                } else if (stype == 0x636F6C72u) { /* colr */
                    if (splen < 3u) {
                        return RFC_ERR_TRUNCATED_INPUT;
                    }
                    uint8_t meth = bytes[spoff];
                    if (meth == 1u) {
                        if (splen < 7u) {
                            return RFC_ERR_TRUNCATED_INPUT;
                        }
                        /* Match OpenJPEG (jp2.c:1482, 1567, 1586): only meth 1
                         * and meth 2 are supported; first supported colr wins
                         * and meth > 2 is ignored by OpenJPEG. */
                        if (!has_colr_winner) {
                            enumcs = jp2_be32(bytes + spoff + 3u);
                            has_enumcs = 1;
                            has_colr_winner = 1;
                            out->has_unsupported_colr = 0;
                        }
                    } else if (meth == 2u) {
                        if (splen <= 3u) {
                            return RFC_ERR_INVALID_ARGUMENT;
                        }
                        if (!has_colr_winner) {
                            out->icc = bytes + spoff + 3u;
                            out->icc_len = splen - 3u;
                            has_colr_winner = 1;
                            out->has_unsupported_colr = 0;
                        }
                    } else if (!has_colr_winner) {
                        out->has_unsupported_colr = 1;
                    }
                } else if (stype == 0x63646566u) { /* cdef */
                    if (splen < 2u) {
                        return RFC_ERR_TRUNCATED_INPUT;
                    }
                    uint16_t n_ent = jp2_be16(bytes + spoff);
                    if (splen != (size_t)2u + (size_t)n_ent * 6u) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                    if (!saw_ihdr || n_ent != ihdr_nc) {
                        return RFC_ERR_UNSUPPORTED_COMBINATION;
                    }
                    out->has_cdef = 1;
                    uint8_t color_mask = 0u;
                    int alpha_count = 0;
                    for (uint16_t ei = 0u; ei < n_ent; ei++) {
                        const uint8_t *e = bytes + spoff + 2u + (size_t)ei * 6u;
                        uint16_t cn = jp2_be16(e);
                        uint16_t typ = jp2_be16(e + 2u);
                        uint16_t asoc = jp2_be16(e + 4u);
                        if (typ == 2u) {
                            /* Premultiplied alpha */
                            return RFC_ERR_UNSUPPORTED_ALPHA;
                        }
                        if (typ == 1u) {
                            /* Require channel 3 whole-image straight alpha. */
                            if (ihdr_nc != 4u || cn != 3u || asoc != 0u ||
                                alpha_count != 0) {
                                return RFC_ERR_UNSUPPORTED_COMBINATION;
                            }
                            alpha_count = 1;
                            out->has_straight_alpha = 1;
                        } else if (typ == 0u) {
                            /* Color channels must map 1:1 without duplicates. */
                            uint16_t max_c = (ihdr_nc == 4u) ? 3u : ihdr_nc;
                            if (cn >= max_c || asoc != (uint16_t)(cn + 1u) ||
                                (color_mask & (uint8_t)(1u << cn)) != 0u) {
                                return RFC_ERR_UNSUPPORTED_COMBINATION;
                            }
                            color_mask |= (uint8_t)(1u << cn);
                        } else {
                            return RFC_ERR_UNSUPPORTED_COMBINATION;
                        }
                    }
                    uint8_t want_mask = (ihdr_nc == 1u) ? 0x01u : 0x07u;
                    if (color_mask != want_mask ||
                        (ihdr_nc == 4u && alpha_count != 1)) {
                        return RFC_ERR_UNSUPPORTED_COMBINATION;
                    }
                }
                sub = snext;
            }
            if (!saw_ihdr || (ihdr_bpc == 0xFFu && !saw_bpcc)) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        } else if (tbox == 0x6A703263u) { /* jp2c */
            if (saw_jp2c) {
                /* Multi-codestream JP2 is unsupported; refuse rather than drop. */
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
            out->codestream = bytes + poff;
            out->codestream_len = plen;
            saw_jp2c = 1;
        } else if (tbox == 0x786D6C20u) { /* xml  */
            if (plen > 0u) {
                if (limits && plen > limits->max_metadata_bytes) {
                    return RFC_ERR_LIMIT_EXCEEDED;
                }
                if (out->xmp) {
                    out->has_unpreserved_meta = 1;
                } else {
                    int is_xmp = 0;
                    rfc_status xst = jp2_classify_xmp_xml(
                        bytes + poff, plen, alloc, &is_xmp);
                    if (xst != RFC_OK) {
                        return xst;
                    }
                    if (is_xmp) {
                        out->xmp = bytes + poff;
                        out->xmp_len = plen;
                    } else {
                        out->has_unpreserved_meta = 1;
                    }
                }
            }
        } else if (tbox == 0x75756964u) { /* uuid */
            if (plen < 16u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (memcmp(bytes + poff, k_xmp_uuid, 16u) == 0) {
                if (plen > 16u) {
                    if (!out->xmp) {
                        out->xmp = bytes + poff + 16u;
                        out->xmp_len = plen - 16u;
                    } else {
                        out->has_unpreserved_meta = 1;
                    }
                }
            } else if (memcmp(bytes + poff, k_exif_uuid, 16u) == 0) {
                if (plen > 16u) {
                    if (!out->exif) {
                        out->exif = bytes + poff + 16u;
                        out->exif_len = plen - 16u;
                    } else {
                        out->has_unpreserved_meta = 1;
                    }
                }
            } else {
                out->has_unpreserved_meta = 1;
            }
        }
        off = next;
    }

    if (!saw_ftyp || !saw_jp2h || !saw_ihdr || !saw_jp2c) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (ihdr_nc == 4u && (!out->has_cdef || !out->has_straight_alpha)) {
        /* 4-component JP2 without unambiguous whole-image straight-alpha cdef
         * (e.g. sRGB + unspecified auxiliary channel) cannot be labeled RGBA. */
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (has_enumcs && !out->icc) {
        /* 16 = sRGB, 17 = greyscale; reject sYCC(18), e-YCC(24), CMYK(12), etc. */
        if (enumcs == 16u) {
            if (ihdr_nc != 3u && ihdr_nc != 4u) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
        } else if (enumcs == 17u) {
            if (ihdr_nc != 1u) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
        } else {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
    }
    return jp2_scan_codestream(out->codestream, out->codestream_len, 1, 1,
                               ihdr_w, ihdr_h, ihdr_nc, ihdr_prec, out);
}

/* ---- OpenJPEG memory stream with cancellation ---- */

typedef struct jp2_mem_reader {
    const uint8_t *data;
    size_t len;
    size_t pos;
    const jp2_session *sess;
    const rfc_exec_context *exec;
    int cancelled;
} jp2_mem_reader;

static OPJ_SIZE_T jp2_read_cb(void *p_buffer,
                              OPJ_SIZE_T p_nb_bytes,
                              void *p_user_data) {
    jp2_mem_reader *r = (jp2_mem_reader *)p_user_data;
    if (!r || !p_buffer) {
        return (OPJ_SIZE_T)-1;
    }
    if (jp2_is_cancelled(r->sess, r->exec)) {
        r->cancelled = 1;
        return (OPJ_SIZE_T)-1;
    }
    if (r->pos >= r->len) {
        return (OPJ_SIZE_T)-1;
    }
    size_t avail = r->len - r->pos;
    size_t take = (p_nb_bytes < avail) ? (size_t)p_nb_bytes : avail;
    memcpy(p_buffer, r->data + r->pos, take);
    r->pos += take;
    return (OPJ_SIZE_T)take;
}

static OPJ_OFF_T jp2_skip_cb(OPJ_OFF_T p_nb_bytes, void *p_user_data) {
    jp2_mem_reader *r = (jp2_mem_reader *)p_user_data;
    if (!r || p_nb_bytes < 0) {
        return -1;
    }
    if (jp2_is_cancelled(r->sess, r->exec)) {
        r->cancelled = 1;
        return -1;
    }
    size_t step = (size_t)p_nb_bytes;
    if (step > r->len - r->pos) {
        r->pos = r->len;
        return -1;
    }
    r->pos += step;
    return p_nb_bytes;
}

static OPJ_BOOL jp2_seek_cb(OPJ_OFF_T p_nb_bytes, void *p_user_data) {
    jp2_mem_reader *r = (jp2_mem_reader *)p_user_data;
    if (!r || p_nb_bytes < 0) {
        return OPJ_FALSE;
    }
    if (jp2_is_cancelled(r->sess, r->exec)) {
        r->cancelled = 1;
        return OPJ_FALSE;
    }
    if ((size_t)p_nb_bytes > r->len) {
        return OPJ_FALSE;
    }
    r->pos = (size_t)p_nb_bytes;
    return OPJ_TRUE;
}

typedef struct jp2_mem_writer {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t pos;
    const jp2_session *sess;
    const rfc_exec_context *exec;
    const rfc_allocator *alloc;
    int cancelled;
    int oom;
} jp2_mem_writer;

static OPJ_SIZE_T jp2_write_cb(void *p_buffer,
                               OPJ_SIZE_T p_nb_bytes,
                               void *p_user_data) {
    jp2_mem_writer *w = (jp2_mem_writer *)p_user_data;
    if (!w || !p_buffer || !w->alloc) {
        return (OPJ_SIZE_T)-1;
    }
    if (jp2_is_cancelled(w->sess, w->exec)) {
        w->cancelled = 1;
        return (OPJ_SIZE_T)-1;
    }
    size_t n = (size_t)p_nb_bytes;
    if (n > SIZE_MAX - w->pos) {
        w->oom = 1;
        return (OPJ_SIZE_T)-1;
    }
    size_t need = w->pos + n;
    if (need > w->cap) {
        size_t ncap = w->cap ? w->cap : 4096u;
        while (ncap < need) {
            if (ncap > SIZE_MAX / 2u) {
                ncap = need;
                break;
            }
            ncap *= 2u;
        }
        uint8_t *nb = (uint8_t *)w->alloc->alloc(w->alloc->ctx, ncap);
        if (!nb) {
            w->oom = 1;
            return (OPJ_SIZE_T)-1;
        }
        if (w->data && w->len > 0u) {
            memcpy(nb, w->data, w->len);
        }
        if (w->data) {
            w->alloc->free(w->alloc->ctx, w->data);
        }
        w->data = nb;
        w->cap = ncap;
    }
    memcpy(w->data + w->pos, p_buffer, n);
    w->pos += n;
    if (w->pos > w->len) {
        w->len = w->pos;
    }
    return p_nb_bytes;
}

static OPJ_OFF_T jp2_wskip_cb(OPJ_OFF_T p_nb_bytes, void *p_user_data) {
    jp2_mem_writer *w = (jp2_mem_writer *)p_user_data;
    if (!w || p_nb_bytes < 0) {
        return -1;
    }
    if (jp2_is_cancelled(w->sess, w->exec)) {
        w->cancelled = 1;
        return -1;
    }
    size_t step = (size_t)p_nb_bytes;
    if (step > SIZE_MAX - w->pos) {
        return -1;
    }
    w->pos += step;
    if (w->pos > w->len) {
        if (w->pos > w->cap) {
            uint8_t zero = 0u;
            size_t end = w->pos;
            w->pos = end - 1u;
            if (jp2_write_cb(&zero, 1u, w) == (OPJ_SIZE_T)-1) {
                return -1;
            }
        } else {
            memset(w->data + w->len, 0, w->pos - w->len);
            w->len = w->pos;
        }
    }
    return p_nb_bytes;
}

static OPJ_BOOL jp2_wseek_cb(OPJ_OFF_T p_nb_bytes, void *p_user_data) {
    jp2_mem_writer *w = (jp2_mem_writer *)p_user_data;
    if (!w || p_nb_bytes < 0) {
        return OPJ_FALSE;
    }
    if (jp2_is_cancelled(w->sess, w->exec)) {
        w->cancelled = 1;
        return OPJ_FALSE;
    }
    w->pos = (size_t)p_nb_bytes;
    return OPJ_TRUE;
}

static void jp2_writer_cleanup(jp2_mem_writer *w) {
    if (w->data && w->alloc) {
        w->alloc->free(w->alloc->ctx, w->data);
    }
    w->data = NULL;
    w->len = 0u;
    w->cap = 0u;
    w->pos = 0u;
}

/* OpenJPEG 2.5.4 constructors (j2k.c:6856, 10755) read OPJ_NUM_THREADS from
 * the environment and spawn a thread pool before opj_codec_set_threads can be
 * called. Always serialize creation under g_jp2_env_lock so concurrent jobs
 * cannot observe a transient OPJ_NUM_THREADS value, and preserve arbitrarily
 * long environment values. */
static opj_codec_t *jp2_create_codec_safe(int is_decompress,
                                          OPJ_CODEC_FORMAT fmt) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_jp2_env_lock);
#else
    pthread_mutex_lock(&g_jp2_env_lock);
#endif
    const char *cur = getenv("OPJ_NUM_THREADS");
    char stack_saved[64];
    char *heap_saved = NULL;
    const char *restore_val = NULL;
    int modified = 0;
    if (cur && strcmp(cur, "0") != 0) {
        size_t slen = strlen(cur);
        if (slen < sizeof(stack_saved)) {
            memcpy(stack_saved, cur, slen + 1u);
            restore_val = stack_saved;
        } else {
            heap_saved = (char *)malloc(slen + 1u);
            if (!heap_saved) {
#ifdef _WIN32
                ReleaseSRWLockExclusive(&g_jp2_env_lock);
#else
                pthread_mutex_unlock(&g_jp2_env_lock);
#endif
                return NULL;
            }
            memcpy(heap_saved, cur, slen + 1u);
            restore_val = heap_saved;
        }
#ifdef _WIN32
        int set_rc = _putenv_s("OPJ_NUM_THREADS", "0");
#else
        int set_rc = setenv("OPJ_NUM_THREADS", "0", 1);
#endif
        if (set_rc != 0) {
            free(heap_saved);
#ifdef _WIN32
            ReleaseSRWLockExclusive(&g_jp2_env_lock);
#else
            pthread_mutex_unlock(&g_jp2_env_lock);
#endif
            return NULL;
        }
        modified = 1;
    }
    opj_codec_t *c = is_decompress ? opj_create_decompress(fmt)
                                   : opj_create_compress(fmt);
    int restore_rc = 0;
    if (modified) {
#ifdef _WIN32
        restore_rc = _putenv_s("OPJ_NUM_THREADS", restore_val);
#else
        restore_rc = setenv("OPJ_NUM_THREADS", restore_val, 1);
#endif
    }
    free(heap_saved);
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_jp2_env_lock);
#else
    pthread_mutex_unlock(&g_jp2_env_lock);
#endif
    if (restore_rc != 0) {
        if (c) {
            opj_destroy_codec(c);
        }
        return NULL;
    }
    return c;
}

static rfc_status jp2_apply_threads(opj_codec_t *codec,
                                    const rfc_exec_context *exec) {
    /* OpenJPEG thread count 0 = single-threaded inline (zero worker threads);
     * N >= 2 = N worker threads capped to JP2_MAX_THREADS. */
    int n = 0;
    if (exec && exec->codec_threads > 1u) {
        uint32_t t = (exec->codec_threads > JP2_MAX_THREADS)
                         ? JP2_MAX_THREADS
                         : exec->codec_threads;
        n = (int)t;
    }
    if (!opj_codec_set_threads(codec, n)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

/* Estimate OpenJPEG's internal working memory for reservation:
 * opj_image_t stores int32_t per pixel per component (4 * nc * w * h),
 * wavelet/tile working buffers (~2x component array), per-tile structures
 * (4 KiB per tile), header + ICC profile copy bytes (jp2h buffer + OpenJPEG
 * icc_profile_buf copy), the stream chunk buffer (JP2_STREAM_CHUNK_BYTES),
 * and fixed codec state (256 KiB). */
static rfc_status jp2_compute_work_reserve(uint32_t w,
                                           uint32_t h,
                                           uint16_t nc,
                                           uint64_t num_tiles,
                                           uint64_t hdr_and_icc_bytes,
                                           uint64_t *out_bytes) {
    uint64_t px = 0u, comps = 0u, tile_bytes = 0u, total = 0u;
    uint64_t tiles = (num_tiles > 0u) ? num_tiles : 1u;
    if (!jp2_mul_u64((uint64_t)w, (uint64_t)h, &px) ||
        !jp2_mul_u64(px, (uint64_t)nc * 4ull, &comps) ||
        !jp2_mul_u64(comps, 2ull, &total) ||
        !jp2_mul_u64(tiles, 4096ull, &tile_bytes) ||
        !jp2_add_u64(total, tile_bytes, &total) ||
        !jp2_add_u64(total, hdr_and_icc_bytes, &total) ||
        !jp2_add_u64(total, (uint64_t)JP2_STREAM_CHUNK_BYTES, &total) ||
        !jp2_add_u64(total, 256ull * 1024ull, &total)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    *out_bytes = total;
    return RFC_OK;
}

/* Splice a 34-byte cdef box inside jp2h when RGBA + ICC caused OpenJPEG to
 * omit cdef (enumcs == 0). If cdef is already present, sets *out_spliced = 0
 * without allocating a second copy so the caller can transfer in_buf directly. */
static rfc_status jp2_ensure_rgba_cdef(const uint8_t *in_buf,
                                       size_t in_len,
                                       const rfc_allocator *alloc,
                                       uint8_t **out_buf,
                                       size_t *out_len,
                                       int *out_spliced) {
    *out_buf = NULL;
    *out_len = 0u;
    *out_spliced = 0;
    /* Locate jp2h box and check whether cdef already exists inside it. */
    if (in_len < 12u || memcmp(in_buf, k_jp2_sig, 12u) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t off = 12u;
    size_t jp2h_off = 0u, jp2h_poff = 0u, jp2h_plen = 0u, jp2h_end = 0u;
    int found_jp2h = 0;
    int has_cdef = 0;
    while (off < in_len) {
        uint32_t tbox = 0u;
        size_t poff = 0u, plen = 0u, next = 0u;
        rfc_status st =
            jp2_read_box(in_buf, in_len, off, &tbox, &poff, &plen, &next);
        if (st != RFC_OK) {
            return st;
        }
        if (tbox == 0x6A703268u) {
            jp2h_off = off;
            jp2h_poff = poff;
            jp2h_plen = plen;
            jp2h_end = next;
            found_jp2h = 1;
            size_t sub = poff;
            while (sub < next) {
                uint32_t stype = 0u;
                size_t sp = 0u, sl = 0u, sn = 0u;
                st = jp2_read_box(in_buf, next, sub, &stype, &sp, &sl, &sn);
                if (st != RFC_OK) {
                    return st;
                }
                if (stype == 0x63646566u) {
                    has_cdef = 1;
                }
                sub = sn;
            }
            break;
        }
        off = next;
    }
    if (!found_jp2h) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (has_cdef) {
        return RFC_OK;
    }
    /* Standard 32-bit jp2h box header expected (OpenJPEG always writes 8-byte hdr). */
    if (jp2h_poff - jp2h_off != 8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    static const uint8_t k_cdef_rgba[34] = {
        0x00u, 0x00u, 0x00u, 0x22u, /* length = 34 */
        'c',   'd',   'e',   'f',
        0x00u, 0x04u,               /* N = 4 channels */
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, /* cn=0, typ=0 (color), asoc=1 (R) */
        0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x02u, /* cn=1, typ=0 (color), asoc=2 (G) */
        0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x03u, /* cn=2, typ=0 (color), asoc=3 (B) */
        0x00u, 0x03u, 0x00u, 0x01u, 0x00u, 0x00u  /* cn=3, typ=1 (straight alpha), asoc=0 (whole) */
    };
    size_t new_jp2h_total = 8u + jp2h_plen + sizeof(k_cdef_rgba);
    if (new_jp2h_total > UINT32_MAX || in_len > SIZE_MAX - sizeof(k_cdef_rgba)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    size_t total_out = in_len + sizeof(k_cdef_rgba);
    uint8_t *dst = (uint8_t *)alloc->alloc(alloc->ctx, total_out);
    if (!dst) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(dst, in_buf, jp2h_end);
    jp2_put_be32(dst + jp2h_off, (uint32_t)new_jp2h_total);
    memcpy(dst + jp2h_end, k_cdef_rgba, sizeof(k_cdef_rgba));
    if (jp2h_end < in_len) {
        memcpy(dst + jp2h_end + sizeof(k_cdef_rgba), in_buf + jp2h_end,
               in_len - jp2h_end);
    }
    *out_buf = dst;
    *out_len = total_out;
    *out_spliced = 1;
    return RFC_OK;
}

/* ---- Session adapter entry points ---- */

static rfc_status jp2_open_session(const rfc_exec_context *exec,
                                   void **out_session) {
    if (!exec || !exec->allocator || !out_session) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    jp2_session *s =
        (jp2_session *)exec->allocator->alloc(exec->allocator->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&s->cancelled, 0);
    s->allocator = exec->allocator;
    *out_session = s;
    return RFC_OK;
}

static void jp2_close_session(void *session) {
    jp2_session *s = (jp2_session *)session;
    if (!s) {
        return;
    }
    const rfc_allocator *al = s->allocator;
    if (al && al->free) {
        al->free(al->ctx, s);
    }
}

static void jp2_cancel_session(void *session) {
    jp2_session *s = (jp2_session *)session;
    if (s) {
        atomic_store_explicit(&s->cancelled, 1, memory_order_release);
    }
}

static rfc_status jp2_copy_meta_blob(const uint8_t *src,
                                     size_t len,
                                     const rfc_allocator *al,
                                     const uint8_t **dst_ptr,
                                     size_t *dst_len) {
    if (!src || len == 0u) {
        return RFC_OK;
    }
    uint8_t *buf = (uint8_t *)al->alloc(al->ctx, len);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(buf, src, len);
    *dst_ptr = buf;
    *dst_len = len;
    return RFC_OK;
}

static rfc_status jp2_probe_session(void *session,
                                    const rfc_exec_context *exec,
                                    const uint8_t *bytes,
                                    size_t size,
                                    rfc_image_probe *out_probe) {
    jp2_session *s = (jp2_session *)session;
    if (!s || !exec || !exec->allocator || !bytes || !out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (jp2_is_cancelled(s, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    const rfc_limits *limits = jp2_limits_of(exec, &lim_def);
    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }

    jp2_scan_info info;
    st = jp2_scan_input(bytes, size, limits, exec->allocator, &info);
    if (st != RFC_OK) {
        return st;
    }

    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = info.sample_format;
    lay.alpha = info.alpha_mode;
    lay.width = info.width;
    lay.height = info.height;
    lay.bits_per_sample = info.prec;
    rfc_layout norm;
    st = rfc_layout_normalize_stride(&lay, &norm);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_limits_check_image(limits, &norm, 1u);
    if (st != RFC_OK) {
        return st;
    }

    uint64_t fbytes = 0u;
    uint64_t fpixels = 0u;
    st = rfc_limits_compute_frame_bytes(&norm, 1u, &fbytes);
    if (st != RFC_OK) {
        return st;
    }
    if (!jp2_mul_u64((uint64_t)norm.width, (uint64_t)norm.height, &fpixels)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    /* Populate container metadata from scanned JP2 boxes (bounded by limits). */
    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.icc_len = info.icc_len;
    meta.xmp_len = info.xmp_len;
    meta.exif_len = info.exif_len;
    st = rfc_limits_check_metadata(limits, &meta);
    if (st != RFC_OK) {
        return st;
    }
    memset(&meta, 0, sizeof(meta));
    if (info.icc && info.icc_len > 0u) {
        st = jp2_copy_meta_blob(info.icc, info.icc_len, exec->allocator,
                                &meta.icc, &meta.icc_len);
    }
    if (st == RFC_OK && info.xmp && info.xmp_len > 0u) {
        st = jp2_copy_meta_blob(info.xmp, info.xmp_len, exec->allocator,
                                &meta.xmp, &meta.xmp_len);
    }
    if (st == RFC_OK && info.exif && info.exif_len > 0u) {
        st = jp2_copy_meta_blob(info.exif, info.exif_len, exec->allocator,
                                &meta.exif, &meta.exif_len);
    }
    if (st != RFC_OK) {
        rfc_metadata_free(&meta, exec->allocator);
        return st;
    }

    out_probe->first_frame = norm;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = fpixels;
    out_probe->total_decoded_bytes = fbytes;
    out_probe->container_meta = meta;

    st = rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
    if (st != RFC_OK) {
        rfc_metadata_free(&out_probe->container_meta, exec->allocator);
        memset(out_probe, 0, sizeof(*out_probe));
        return st;
    }
    return RFC_OK;
}

static rfc_status jp2_decode_session(void *session,
                                     const rfc_exec_context *exec,
                                     const uint8_t *bytes,
                                     size_t size,
                                     const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_frame **out_frames,
                                     size_t *out_frame_count,
                                     rfc_metadata *out_meta) {
    jp2_session *s = (jp2_session *)session;
    if (!s || !exec || !exec->allocator || !bytes || !out_frames ||
        !out_frame_count || !out_meta) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    memset(out_meta, 0, sizeof(*out_meta));

    rfc_metadata_policy meta_policy = RFC_META_POLICY_PRESERVE;
    rfc_exactness_class exactness = RFC_EXACT_PIXEL_EXACT;
    rfc_status st =
        jp2_parse_settings(settings, setting_count, &meta_policy, &exactness);
    if (st != RFC_OK) {
        return st;
    }
    if (jp2_is_cancelled(s, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    const rfc_limits *limits = jp2_limits_of(exec, &lim_def);
    st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }

    jp2_scan_info info;
    st = jp2_scan_input(bytes, size, limits, exec->allocator, &info);
    if (st != RFC_OK) {
        return st;
    }
    if ((info.has_unsupported_colr || info.has_unpreserved_meta) &&
        meta_policy == RFC_META_POLICY_PRESERVE) {
        return RFC_ERR_METADATA_POLICY;
    }

    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = info.sample_format;
    lay.alpha = info.alpha_mode;
    lay.width = info.width;
    lay.height = info.height;
    lay.bits_per_sample = info.prec;
    rfc_layout norm;
    st = rfc_layout_normalize_stride(&lay, &norm);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_limits_check_image(limits, &norm, 1u);
    if (st != RFC_OK) {
        return st;
    }
    uint64_t fbytes = 0u;
    st = rfc_limits_compute_frame_bytes(&norm, 1u, &fbytes);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_limits_check_decompression(limits, (uint64_t)size, fbytes);
    if (st != RFC_OK) {
        return st;
    }

    /* Check metadata size limits before any OpenJPEG allocation under BOTH
     * preserve and strip, since OpenJPEG reads and allocates embedded colr ICC
     * regardless of whether the adapter later drops it. */
    rfc_metadata meta_chk;
    memset(&meta_chk, 0, sizeof(meta_chk));
    meta_chk.icc_len = info.icc_len;
    meta_chk.xmp_len = info.xmp_len;
    meta_chk.exif_len = info.exif_len;
    st = rfc_limits_check_metadata(limits, &meta_chk);
    if (st != RFC_OK) {
        return st;
    }

    /* Reserve OpenJPEG internal working memory on the job budget before decode
     * (including full jp2h box buffer + selected ICC copy). */
    uint64_t hdr_icc_bytes = 0u;
    if (!jp2_add_u64((uint64_t)info.jp2h_len, (uint64_t)info.icc_len,
                     &hdr_icc_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t work_reserve = 0u;
    st = jp2_compute_work_reserve(info.width, info.height, info.numcomps,
                                  info.num_tiles, hdr_icc_bytes,
                                  &work_reserve);
    if (st != RFC_OK) {
        return st;
    }
    int reserved = 0;
    if (exec->budget) {
        st = rfc_budget_reserve(exec->budget, work_reserve);
        if (st != RFC_OK) {
            return st;
        }
        reserved = 1;
    }

    jp2_invoke_hook(1);
    if (jp2_is_cancelled(s, exec)) {
        if (reserved) {
            rfc_budget_release(exec->budget, work_reserve);
        }
        return RFC_ERR_CANCELLED;
    }

    opj_codec_t *dec = jp2_create_codec_safe(
        1, info.is_jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
    opj_stream_t *stream = NULL;
    opj_image_t *img = NULL;
    jp2_mem_reader mr;
    memset(&mr, 0, sizeof(mr));
    mr.data = bytes;
    mr.len = size;
    mr.pos = 0u;
    mr.sess = s;
    mr.exec = exec;

    if (!dec) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    /* Observe thread count right after constructor returns, BEFORE
     * jp2_apply_threads could tear down any constructor-created pool. */
    jp2_invoke_hook(4);
    opj_set_info_handler(dec, jp2_quiet_msg, NULL);
    opj_set_warning_handler(dec, jp2_quiet_msg, NULL);
    opj_set_error_handler(dec, jp2_quiet_msg, NULL);

    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    if (!opj_setup_decoder(dec, &dparams)) {
        st = RFC_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }
    st = jp2_apply_threads(dec, exec);
    if (st != RFC_OK) {
        goto cleanup;
    }

    stream = opj_stream_create(JP2_STREAM_CHUNK_BYTES, OPJ_TRUE);
    if (!stream) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    opj_stream_set_user_data(stream, &mr, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)size);
    opj_stream_set_read_function(stream, jp2_read_cb);
    opj_stream_set_skip_function(stream, jp2_skip_cb);
    opj_stream_set_seek_function(stream, jp2_seek_cb);

    if (!opj_read_header(stream, dec, &img) || !img) {
        st = mr.cancelled ? RFC_ERR_CANCELLED : RFC_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (jp2_is_cancelled(s, exec)) {
        st = RFC_ERR_CANCELLED;
        goto cleanup;
    }
    /* Verify OpenJPEG's parsed header matches our pre-scan and constraints. */
    if (img->numcomps != (OPJ_UINT32)info.numcomps ||
        img->x1 <= img->x0 || img->y1 <= img->y0 ||
        (img->x1 - img->x0) != info.width ||
        (img->y1 - img->y0) != info.height) {
        st = RFC_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (img->color_space == OPJ_CLRSPC_SYCC ||
        img->color_space == OPJ_CLRSPC_EYCC ||
        img->color_space == OPJ_CLRSPC_CMYK ||
        (img->color_space == OPJ_CLRSPC_UNKNOWN && !info.icc &&
         !(img->icc_profile_buf && img->icc_profile_len > 0u))) {
        st = RFC_ERR_UNSUPPORTED_COMBINATION;
        goto cleanup;
    }
    for (OPJ_UINT32 ci = 0u; ci < img->numcomps; ci++) {
        const opj_image_comp_t *c = &img->comps[ci];
        if (c->sgnd != 0u || c->dx != 1u || c->dy != 1u ||
            c->w != info.width || c->h != info.height ||
            c->prec != (OPJ_UINT32)info.prec) {
            st = RFC_ERR_UNSUPPORTED_SAMPLE;
            goto cleanup;
        }
    }

    if (!opj_decode(dec, stream, img) || !opj_end_decompress(dec, stream)) {
        st = mr.cancelled ? RFC_ERR_CANCELLED : RFC_ERR_TRUNCATED_INPUT;
        goto cleanup;
    }
    if (jp2_is_cancelled(s, exec)) {
        st = RFC_ERR_CANCELLED;
        goto cleanup;
    }
    for (OPJ_UINT32 ci = 0u; ci < img->numcomps; ci++) {
        if (!img->comps[ci].data) {
            st = RFC_ERR_TRUNCATED_INPUT;
            goto cleanup;
        }
    }

    /* Validate frame against job/exec limits and allocate converter output frame. */
    st = rfc_exec_check_next_frame(exec, &norm, NULL);
    if (st != RFC_OK) {
        goto cleanup;
    }
    jp2_invoke_hook(3);
    rfc_frame *frames = (rfc_frame *)exec->allocator->alloc(
        exec->allocator->ctx, sizeof(*frames));
    if (!frames) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    memset(frames, 0, sizeof(*frames));
    st = rfc_exec_alloc_frame(exec, &frames[0], &norm, NULL);
    if (st != RFC_OK) {
        exec->allocator->free(exec->allocator->ctx, frames);
        goto cleanup;
    }

    /* Pack OpenJPEG planar int32_t components into left-aligned converter frame. */
    const int is_16 = (info.prec > 8u);
    const uint32_t storage_bits = is_16 ? 16u : 8u;
    const uint32_t shift = storage_bits - (uint32_t)info.prec;
    const OPJ_INT32 max_val = (OPJ_INT32)((1u << info.prec) - 1u);
    const uint32_t w = info.width;
    const uint32_t h = info.height;
    const uint16_t nc = info.numcomps;

    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *row = frames[0].data + (size_t)y * norm.stride_bytes;
        size_t row_px = (size_t)y * (size_t)w;
        if (!is_16) {
            for (uint32_t x = 0u; x < w; x++) {
                size_t idx = row_px + (size_t)x;
                for (uint16_t c = 0u; c < nc; c++) {
                    OPJ_INT32 v = img->comps[c].data[idx];
                    if (v < 0) {
                        v = 0;
                    } else if (v > max_val) {
                        v = max_val;
                    }
                    row[(size_t)x * nc + c] = (uint8_t)((uint32_t)v << shift);
                }
            }
        } else {
            for (uint32_t x = 0u; x < w; x++) {
                size_t idx = row_px + (size_t)x;
                for (uint16_t c = 0u; c < nc; c++) {
                    OPJ_INT32 v = img->comps[c].data[idx];
                    if (v < 0) {
                        v = 0;
                    } else if (v > max_val) {
                        v = max_val;
                    }
                    uint16_t sv = (uint16_t)((uint32_t)v << shift);
                    memcpy(row + ((size_t)x * nc + c) * 2u, &sv, sizeof(sv));
                }
            }
        }
    }

    /* Extract metadata when policy is preserve (strip drops all metadata). */
    rfc_metadata dmeta;
    memset(&dmeta, 0, sizeof(dmeta));
    if (meta_policy == RFC_META_POLICY_PRESERVE) {
        const uint8_t *icc_src = info.icc;
        size_t icc_len = info.icc_len;
        if ((!icc_src || icc_len == 0u) && img->icc_profile_buf &&
            img->icc_profile_len > 0u) {
            icc_src = img->icc_profile_buf;
            icc_len = (size_t)img->icc_profile_len;
        }
        if (icc_src && icc_len > 0u) {
            st = jp2_copy_meta_blob(icc_src, icc_len, exec->allocator,
                                    &dmeta.icc, &dmeta.icc_len);
        }
        if (st == RFC_OK && info.xmp && info.xmp_len > 0u) {
            st = jp2_copy_meta_blob(info.xmp, info.xmp_len, exec->allocator,
                                    &dmeta.xmp, &dmeta.xmp_len);
        }
        if (st == RFC_OK && info.exif && info.exif_len > 0u) {
            st = jp2_copy_meta_blob(info.exif, info.exif_len, exec->allocator,
                                    &dmeta.exif, &dmeta.exif_len);
        }
        if (st != RFC_OK) {
            rfc_metadata_free(&dmeta, exec->allocator);
            rfc_frame_free(&frames[0]);
            exec->allocator->free(exec->allocator->ctx, frames);
            goto cleanup;
        }
    }

    *out_frames = frames;
    *out_frame_count = 1u;
    *out_meta = dmeta;
    st = RFC_OK;

cleanup:
    if (img) {
        opj_image_destroy(img);
    }
    if (stream) {
        opj_stream_destroy(stream);
    }
    if (dec) {
        opj_destroy_codec(dec);
    }
    if (reserved) {
        rfc_budget_release(exec->budget, work_reserve);
    }
    return st;
}

static rfc_status jp2_encode_session(void *session,
                                     const rfc_exec_context *exec,
                                     const rfc_frame *frames,
                                     size_t frame_count,
                                     const rfc_metadata *meta,
                                     const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_encoded *out_encoded) {
    jp2_session *s = (jp2_session *)session;
    if (!s || !exec || !exec->allocator || !frames || !out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    if (frame_count == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count > 1u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (jp2_is_cancelled(s, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_metadata_policy meta_policy = RFC_META_POLICY_PRESERVE;
    rfc_exactness_class exactness = RFC_EXACT_PIXEL_EXACT;
    rfc_status st =
        jp2_parse_settings(settings, setting_count, &meta_policy, &exactness);
    if (st != RFC_OK) {
        return st;
    }
    if (exactness == RFC_EXACT_BYTE_IDENTICAL) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }

    const rfc_frame *fr = &frames[0];
    if (!fr->data) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_layout norm;
    st = rfc_layout_normalize_stride(&fr->layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    rfc_layout eff_lay = fr->layout;
    if (eff_lay.stride_bytes == 0u) {
        eff_lay.stride_bytes = norm.stride_bytes;
    }
    size_t min_bytes = 0u;
    st = rfc_layout_validate(&eff_lay, &min_bytes);
    if (st != RFC_OK) {
        return st;
    }
    if (fr->data_len < min_bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint16_t nc = 0u;
    int is_16 = 0;
    int expect_alpha = 0;
    switch (eff_lay.format) {
        case RFC_SAMPLE_GRAY8:
            nc = 1u; is_16 = 0; expect_alpha = 0; break;
        case RFC_SAMPLE_GRAY16:
            nc = 1u; is_16 = 1; expect_alpha = 0; break;
        case RFC_SAMPLE_RGB8:
            nc = 3u; is_16 = 0; expect_alpha = 0; break;
        case RFC_SAMPLE_RGBA8:
            nc = 4u; is_16 = 0; expect_alpha = 1; break;
        case RFC_SAMPLE_RGB16:
            nc = 3u; is_16 = 1; expect_alpha = 0; break;
        case RFC_SAMPLE_RGBA16:
            nc = 4u; is_16 = 1; expect_alpha = 1; break;
        default:
            return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (expect_alpha) {
        if (eff_lay.alpha != RFC_ALPHA_STRAIGHT) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
    } else {
        if (eff_lay.alpha != RFC_ALPHA_NONE) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
    }
    uint32_t storage_bits = is_16 ? 16u : 8u;
    uint32_t prec = eff_lay.bits_per_sample ? (uint32_t)eff_lay.bits_per_sample
                                            : storage_bits;
    if ((!is_16 && (prec < 1u || prec > 8u)) ||
        (is_16 && (prec < 9u || prec > 16u))) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    rfc_limits lim_def;
    const rfc_limits *limits = jp2_limits_of(exec, &lim_def);
    st = rfc_limits_check_image(limits, &norm, 1u);
    if (st != RFC_OK) {
        return st;
    }

    /* Validate reduced-precision low padding bits before OpenJPEG allocation.
     * Use alignment-safe memcpy for 16-bit samples so odd padded strides are UB-free. */
    const uint32_t shift = storage_bits - prec;
    const uint32_t low_mask = (shift > 0u) ? ((1u << shift) - 1u) : 0u;
    const uint32_t w = eff_lay.width;
    const uint32_t h = eff_lay.height;
    if (low_mask != 0u) {
        for (uint32_t y = 0u; y < h; y++) {
            const uint8_t *row = fr->data + (size_t)y * eff_lay.stride_bytes;
            if (!is_16) {
                size_t n_samp = (size_t)w * (size_t)nc;
                for (size_t i = 0u; i < n_samp; i++) {
                    if (((uint32_t)row[i] & low_mask) != 0u) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                }
            } else {
                size_t n_samp = (size_t)w * (size_t)nc;
                for (size_t i = 0u; i < n_samp; i++) {
                    uint16_t v16 = 0u;
                    memcpy(&v16, row + i * 2u, sizeof(v16));
                    if (((uint32_t)v16 & low_mask) != 0u) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                }
            }
        }
    }

    /* Validate effective metadata limits and preservation policy before work. */
    const uint8_t *eff_icc = NULL;
    size_t eff_icc_len = 0u;
    const uint8_t *eff_exif = NULL;
    size_t eff_exif_len = 0u;
    const uint8_t *eff_xmp = NULL;
    size_t eff_xmp_len = 0u;
    rfc_orientation eff_ori = RFC_ORIENTATION_UNSPECIFIED;
    rfc_colorimetry eff_color;
    memset(&eff_color, 0, sizeof(eff_color));
    if (fr->metadata.icc && fr->metadata.icc_len > 0u) {
        eff_icc = fr->metadata.icc;
        eff_icc_len = fr->metadata.icc_len;
    } else if (meta && meta->icc && meta->icc_len > 0u) {
        eff_icc = meta->icc;
        eff_icc_len = meta->icc_len;
    }
    if (fr->metadata.exif && fr->metadata.exif_len > 0u) {
        eff_exif = fr->metadata.exif;
        eff_exif_len = fr->metadata.exif_len;
    } else if (meta && meta->exif && meta->exif_len > 0u) {
        eff_exif = meta->exif;
        eff_exif_len = meta->exif_len;
    }
    if (fr->metadata.xmp && fr->metadata.xmp_len > 0u) {
        eff_xmp = fr->metadata.xmp;
        eff_xmp_len = fr->metadata.xmp_len;
    } else if (meta && meta->xmp && meta->xmp_len > 0u) {
        eff_xmp = meta->xmp;
        eff_xmp_len = meta->xmp_len;
    }
    if (fr->metadata.orientation != RFC_ORIENTATION_UNSPECIFIED) {
        eff_ori = fr->metadata.orientation;
    } else if (meta && meta->orientation != RFC_ORIENTATION_UNSPECIFIED) {
        eff_ori = meta->orientation;
    }
    if (rfc_colorimetry_has_any(&fr->metadata.color)) {
        eff_color = fr->metadata.color;
    } else if (meta && rfc_colorimetry_has_any(&meta->color)) {
        eff_color = meta->color;
    }
    if (!rfc_colorimetry_is_valid(&eff_color) ||
        eff_ori > RFC_ORIENTATION_ROTATE_270) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_metadata check_m = {eff_icc, eff_icc_len, eff_ori, 0u, eff_color,
                            eff_exif, eff_exif_len, eff_xmp, eff_xmp_len};
    st = rfc_limits_check_metadata(limits, &check_m);
    if (st != RFC_OK) {
        return st;
    }
    if (!exec->job) {
        st = rfc_metadata_check_source_preflight(
            RFC_FORMAT_JPEG2000, meta_policy, exactness, meta, frames,
            frame_count, 0u, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }

    const uint8_t *icc_ptr = NULL;
    size_t icc_len = 0u;
    if (meta_policy == RFC_META_POLICY_PRESERVE && eff_icc &&
        eff_icc_len > 0u) {
        if (eff_icc_len > UINT32_MAX) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        icc_ptr = eff_icc;
        icc_len = eff_icc_len;
    }

    uint64_t enc_icc_bytes = 0u;
    if (!jp2_mul_u64((uint64_t)icc_len, 2ull, &enc_icc_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t work_reserve = 0u;
    st = jp2_compute_work_reserve(w, h, nc, 1u, enc_icc_bytes,
                                  &work_reserve);
    if (st != RFC_OK) {
        return st;
    }
    int reserved = 0;
    if (exec->budget) {
        st = rfc_budget_reserve(exec->budget, work_reserve);
        if (st != RFC_OK) {
            return st;
        }
        reserved = 1;
    }

    jp2_invoke_hook(2);
    if (jp2_is_cancelled(s, exec)) {
        if (reserved) {
            rfc_budget_release(exec->budget, work_reserve);
        }
        return RFC_ERR_CANCELLED;
    }

    opj_image_cmptparm_t parms[4];
    memset(parms, 0, sizeof(parms));
    for (uint16_t c = 0u; c < nc; c++) {
        parms[c].dx = 1u;
        parms[c].dy = 1u;
        parms[c].w = w;
        parms[c].h = h;
        parms[c].x0 = 0u;
        parms[c].y0 = 0u;
        parms[c].prec = prec;
        parms[c].sgnd = 0u;
    }
    OPJ_COLOR_SPACE cs = (nc == 1u) ? OPJ_CLRSPC_GRAY : OPJ_CLRSPC_SRGB;
    opj_image_t *img = opj_image_create((OPJ_UINT32)nc, parms, cs);
    opj_codec_t *enc = NULL;
    opj_stream_t *stream = NULL;
    jp2_mem_writer mw;
    memset(&mw, 0, sizeof(mw));
    mw.sess = s;
    mw.exec = exec;
    mw.alloc = exec->allocator;

    if (!img) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    img->x0 = 0u;
    img->y0 = 0u;
    img->x1 = w;
    img->y1 = h;
    if (nc == 4u) {
        img->comps[3].alpha = 1u;
    }
    if (icc_ptr && icc_len > 0u) {
        img->icc_profile_buf = (OPJ_BYTE *)icc_ptr;
        img->icc_profile_len = (OPJ_UINT32)icc_len;
    }

    for (uint32_t y = 0u; y < h; y++) {
        const uint8_t *row = fr->data + (size_t)y * eff_lay.stride_bytes;
        size_t row_px = (size_t)y * (size_t)w;
        if (!is_16) {
            for (uint32_t x = 0u; x < w; x++) {
                size_t idx = row_px + (size_t)x;
                for (uint16_t c = 0u; c < nc; c++) {
                    uint32_t v = (uint32_t)row[(size_t)x * nc + c];
                    img->comps[c].data[idx] = (OPJ_INT32)(v >> shift);
                }
            }
        } else {
            for (uint32_t x = 0u; x < w; x++) {
                size_t idx = row_px + (size_t)x;
                for (uint16_t c = 0u; c < nc; c++) {
                    uint16_t v16 = 0u;
                    memcpy(&v16, row + ((size_t)x * nc + c) * 2u, sizeof(v16));
                    img->comps[c].data[idx] = (OPJ_INT32)((uint32_t)v16 >> shift);
                }
            }
        }
    }

    opj_cparameters_t cp;
    opj_set_default_encoder_parameters(&cp);
    /* Reversible 5-3 lossless mode; irreversible 9-7 is never emitted. */
    cp.irreversible = 0;
    cp.tcp_numlayers = 1;
    cp.tcp_rates[0] = 0.0f;
    cp.cp_disto_alloc = 1;
    cp.tcp_mct = (nc >= 3u) ? 1 : 0;
    /* Clamp numresolution to 1..6 such that 1<<(n-1) <= min(w, h) (plan #6). */
    uint32_t min_dim = (w < h) ? w : h;
    int nres = 1;
    while (nres < 6 && ((uint32_t)1u << nres) <= min_dim) {
        nres++;
    }
    cp.numresolution = nres;

    enc = jp2_create_codec_safe(0, OPJ_CODEC_JP2);
    if (!enc) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    jp2_invoke_hook(5);
    opj_set_info_handler(enc, jp2_quiet_msg, NULL);
    opj_set_warning_handler(enc, jp2_quiet_msg, NULL);
    opj_set_error_handler(enc, jp2_quiet_msg, NULL);

    if (!opj_setup_encoder(enc, &cp, img)) {
        st = RFC_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }
    /* Clear borrowed ICC pointer right after opj_setup_encoder copied it. */
    img->icc_profile_buf = NULL;
    img->icc_profile_len = 0u;

    st = jp2_apply_threads(enc, exec);
    if (st != RFC_OK) {
        goto cleanup;
    }

    stream = opj_stream_create(JP2_STREAM_CHUNK_BYTES, OPJ_FALSE);
    if (!stream) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    opj_stream_set_user_data(stream, &mw, NULL);
    opj_stream_set_write_function(stream, jp2_write_cb);
    opj_stream_set_skip_function(stream, jp2_wskip_cb);
    opj_stream_set_seek_function(stream, jp2_wseek_cb);

    if (!opj_start_compress(enc, img, stream) || !opj_encode(enc, stream) ||
        !opj_end_compress(enc, stream)) {
        if (mw.cancelled || jp2_is_cancelled(s, exec)) {
            st = RFC_ERR_CANCELLED;
        } else if (mw.oom) {
            st = RFC_ERR_OUT_OF_MEMORY;
        } else {
            st = RFC_ERR_INVALID_ARGUMENT;
        }
        goto cleanup;
    }
    if (jp2_is_cancelled(s, exec)) {
        st = RFC_ERR_CANCELLED;
        goto cleanup;
    }

    /* Release OpenJPEG handles and working reserve before finalizing output. */
    opj_stream_destroy(stream);
    stream = NULL;
    opj_destroy_codec(enc);
    enc = NULL;
    opj_image_destroy(img);
    img = NULL;
    if (reserved) {
        rfc_budget_release(exec->budget, work_reserve);
        reserved = 0;
    }

    uint8_t *final_buf = NULL;
    size_t final_len = 0u;
    if (nc == 4u) {
        int spliced = 0;
        st = jp2_ensure_rgba_cdef(mw.data, mw.len, exec->allocator, &final_buf,
                                  &final_len, &spliced);
        if (st != RFC_OK) {
            goto cleanup;
        }
        if (!spliced) {
            /* Transfer mw.data directly with zero second copy. */
            final_buf = mw.data;
            final_len = mw.len;
            mw.data = NULL;
        }
    } else {
        /* Transfer mw.data directly with zero second copy. */
        final_buf = mw.data;
        final_len = mw.len;
        mw.data = NULL;
    }

    out_encoded->data = final_buf;
    out_encoded->len = final_len;
    out_encoded->allocator = exec->allocator;
    st = RFC_OK;

cleanup:
    if (img) {
        img->icc_profile_buf = NULL;
        img->icc_profile_len = 0u;
        opj_image_destroy(img);
    }
    if (stream) {
        opj_stream_destroy(stream);
    }
    if (enc) {
        opj_destroy_codec(enc);
    }
    jp2_writer_cleanup(&mw);
    if (reserved) {
        rfc_budget_release(exec->budget, work_reserve);
    }
    return st;
}

static const rfc_session_adapter k_jp2_adapter = {
    .abi_version = RFC_CODEC_ABI_VERSION,
    .format = RFC_FORMAT_JPEG2000,
    .open_session = jp2_open_session,
    .close_session = jp2_close_session,
    .probe_session = jp2_probe_session,
    .decode_session = jp2_decode_session,
    .encode_session = jp2_encode_session,
    .cancel_session = jp2_cancel_session
};

rfc_status rfc_codecs_register_jp2(void) {
    return rfc_registry_register_session(&k_jp2_adapter);
}

rfc_status rfc_codecs_unregister_jp2(void) {
    return rfc_registry_unregister_session(RFC_FORMAT_JPEG2000);
}
