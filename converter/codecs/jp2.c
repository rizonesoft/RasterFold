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

#ifndef OPJ_STATIC
#define OPJ_STATIC
#endif

#include "rfc_codecs.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <openjpeg.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JP2_MAX_BOXES 64u
#define JP2_MAX_THREADS 16u

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

static rfc_status jp2_scan_input(const uint8_t *bytes,
                                 size_t size,
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
        return jp2_scan_codestream(bytes, size, 0, 0, 0u, 0u, 0u, out);
    }
    /* JP2 container signature? */
    if (size < 12u || memcmp(bytes, k_jp2_sig, 12u) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    out->is_jp2 = 1;

    size_t off = 12u;
    uint32_t box_count = 0u;
    int saw_ftyp = 0;
    int saw_jp2h = 0;
    int saw_ihdr = 0;
    int saw_jp2c = 0;
    uint32_t ihdr_w = 0u, ihdr_h = 0u;
    uint16_t ihdr_nc = 0u;
    uint8_t ihdr_bpc = 0u;
    int has_enumcs = 0;
    uint32_t enumcs = 0u;

    while (off < size) {
        if (++box_count > JP2_MAX_BOXES) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
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
            size_t sub = poff;
            size_t sub_end = poff + plen;
            uint32_t sub_count = 0u;
            int saw_bpcc = 0;
            while (sub < sub_end) {
                if (++sub_count > JP2_MAX_BOXES) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
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
                    if (ihdr_bpc != 0xFFu) {
                        if ((ihdr_bpc & 0x80u) != 0u ||
                            ((ihdr_bpc & 0x7Fu) + 1u) > 16u) {
                            return RFC_ERR_UNSUPPORTED_SAMPLE;
                        }
                    }
                    saw_ihdr = 1;
                } else if (stype == 0x62706363u) { /* bpcc */
                    saw_bpcc = 1;
                    if (!saw_ihdr || splen != (size_t)ihdr_nc) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                    uint8_t b0 = bytes[spoff];
                    for (size_t bi = 0u; bi < splen; bi++) {
                        uint8_t bb = bytes[spoff + bi];
                        if ((bb & 0x80u) != 0u || ((bb & 0x7Fu) + 1u) > 16u ||
                            bb != b0) {
                            return RFC_ERR_UNSUPPORTED_SAMPLE;
                        }
                    }
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
                        if (!has_enumcs) {
                            enumcs = jp2_be32(bytes + spoff + 3u);
                            has_enumcs = 1;
                        }
                    } else if (meth == 2u || meth == 3u) {
                        if (splen <= 3u) {
                            return RFC_ERR_INVALID_ARGUMENT;
                        }
                        if (!out->icc) {
                            out->icc = bytes + spoff + 3u;
                            out->icc_len = splen - 3u;
                        }
                    } else {
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
                    out->has_cdef = 1;
                    for (uint16_t ei = 0u; ei < n_ent; ei++) {
                        const uint8_t *e = bytes + spoff + 2u + (size_t)ei * 6u;
                        uint16_t cn = jp2_be16(e);
                        uint16_t typ = jp2_be16(e + 2u);
                        if (typ == 2u) {
                            /* Premultiplied alpha */
                            return RFC_ERR_UNSUPPORTED_ALPHA;
                        }
                        if (typ == 1u) {
                            if (cn != 3u) {
                                return RFC_ERR_UNSUPPORTED_COMBINATION;
                            }
                            out->has_straight_alpha = 1;
                        } else if (typ != 0u) {
                            return RFC_ERR_UNSUPPORTED_COMBINATION;
                        }
                    }
                }
                sub = snext;
            }
            if (!saw_ihdr || (ihdr_bpc == 0xFFu && !saw_bpcc)) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        } else if (tbox == 0x6A703263u) { /* jp2c */
            if (!saw_jp2c) {
                out->codestream = bytes + poff;
                out->codestream_len = plen;
                saw_jp2c = 1;
            }
        } else if (tbox == 0x786D6C20u) { /* xml  */
            if (plen > 0u && !out->xmp) {
                out->xmp = bytes + poff;
                out->xmp_len = plen;
            }
        } else if (tbox == 0x75756964u) { /* uuid */
            if (plen > 16u) {
                if (memcmp(bytes + poff, k_xmp_uuid, 16u) == 0) {
                    if (!out->xmp) {
                        out->xmp = bytes + poff + 16u;
                        out->xmp_len = plen - 16u;
                    }
                } else if (!out->exif) {
                    out->exif = bytes + poff;
                    out->exif_len = plen;
                }
            }
        }
        off = next;
    }

    if (!saw_ftyp || !saw_jp2h || !saw_ihdr || !saw_jp2c) {
        return RFC_ERR_TRUNCATED_INPUT;
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
                               ihdr_w, ihdr_h, ihdr_nc, out);
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
    int cancelled;
    int oom;
    rfc_budget *budget;
    uint64_t reserved;
} jp2_mem_writer;

static OPJ_SIZE_T jp2_write_cb(void *p_buffer,
                               OPJ_SIZE_T p_nb_bytes,
                               void *p_user_data) {
    jp2_mem_writer *w = (jp2_mem_writer *)p_user_data;
    if (!w || !p_buffer) {
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
        if (w->budget) {
            uint64_t delta = (uint64_t)ncap - w->reserved;
            if (rfc_budget_reserve(w->budget, delta) != RFC_OK) {
                w->oom = 1;
                return (OPJ_SIZE_T)-1;
            }
            w->reserved = (uint64_t)ncap;
        }
        uint8_t *nb = (uint8_t *)realloc(w->data, ncap);
        if (!nb) {
            w->oom = 1;
            return (OPJ_SIZE_T)-1;
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
    free(w->data);
    w->data = NULL;
    w->len = 0u;
    w->cap = 0u;
    w->pos = 0u;
    if (w->budget && w->reserved > 0u) {
        rfc_budget_release(w->budget, w->reserved);
        w->reserved = 0u;
    }
}

static rfc_status jp2_apply_threads(opj_codec_t *codec,
                                    const rfc_exec_context *exec) {
    int n = 1;
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
 * plus tile/wavelet working buffers (~2x component array + 256 KiB). */
static rfc_status jp2_compute_work_reserve(uint32_t w,
                                           uint32_t h,
                                           uint16_t nc,
                                           uint64_t *out_bytes) {
    uint64_t px = 0u, comps = 0u, total = 0u;
    if (!jp2_mul_u64((uint64_t)w, (uint64_t)h, &px) ||
        !jp2_mul_u64(px, (uint64_t)nc * 4ull, &comps) ||
        !jp2_mul_u64(comps, 2ull, &total) ||
        !jp2_add_u64(total, 256ull * 1024ull, &total)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    *out_bytes = total;
    return RFC_OK;
}

/* Splice a 34-byte cdef box inside jp2h when RGBA + ICC caused OpenJPEG to
 * omit cdef (enumcs == 0). Returns newly allocated buffer via alloc. */
static rfc_status jp2_ensure_rgba_cdef(const uint8_t *in_buf,
                                       size_t in_len,
                                       const rfc_allocator *alloc,
                                       uint8_t **out_buf,
                                       size_t *out_len) {
    /* Locate jp2h box and check whether cdef already exists inside it. */
    if (in_len < 12u || memcmp(in_buf, k_jp2_sig, 12u) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t off = 12u;
    size_t jp2h_off = 0u, jp2h_poff = 0u, jp2h_plen = 0u, jp2h_end = 0u;
    int found_jp2h = 0;
    int has_cdef = 0;
    uint32_t guard = 0u;
    while (off < in_len && ++guard <= JP2_MAX_BOXES) {
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
            uint32_t sguard = 0u;
            while (sub < next && ++sguard <= JP2_MAX_BOXES) {
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
        uint8_t *copy = (uint8_t *)alloc->alloc(alloc->ctx, in_len);
        if (!copy) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, in_buf, in_len);
        *out_buf = copy;
        *out_len = in_len;
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
    st = jp2_scan_input(bytes, size, &info);
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
    st = jp2_scan_input(bytes, size, &info);
    if (st != RFC_OK) {
        return st;
    }
    if (info.has_unsupported_colr && meta_policy == RFC_META_POLICY_PRESERVE) {
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

    /* Check metadata size limits before any OpenJPEG allocation. */
    rfc_metadata meta_chk;
    memset(&meta_chk, 0, sizeof(meta_chk));
    if (meta_policy == RFC_META_POLICY_PRESERVE) {
        meta_chk.icc_len = info.icc_len;
        meta_chk.xmp_len = info.xmp_len;
        meta_chk.exif_len = info.exif_len;
        st = rfc_limits_check_metadata(limits, &meta_chk);
        if (st != RFC_OK) {
            return st;
        }
    }

    /* Reserve OpenJPEG internal working memory on the job budget before decode. */
    uint64_t work_reserve = 0u;
    st = jp2_compute_work_reserve(info.width, info.height, info.numcomps,
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

    opj_codec_t *dec = opj_create_decompress(info.is_jp2 ? OPJ_CODEC_JP2
                                                         : OPJ_CODEC_J2K);
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

    stream = opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_TRUE);
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
            uint16_t *row16 = (uint16_t *)row;
            for (uint32_t x = 0u; x < w; x++) {
                size_t idx = row_px + (size_t)x;
                for (uint16_t c = 0u; c < nc; c++) {
                    OPJ_INT32 v = img->comps[c].data[idx];
                    if (v < 0) {
                        v = 0;
                    } else if (v > max_val) {
                        v = max_val;
                    }
                    row16[(size_t)x * nc + c] =
                        (uint16_t)((uint32_t)v << shift);
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

    /* Validate reduced-precision low padding bits before OpenJPEG allocation. */
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
                const uint16_t *row16 = (const uint16_t *)row;
                size_t n_samp = (size_t)w * (size_t)nc;
                for (size_t i = 0u; i < n_samp; i++) {
                    if (((uint32_t)row16[i] & low_mask) != 0u) {
                        return RFC_ERR_INVALID_ARGUMENT;
                    }
                }
            }
        }
    }

    /* Pick ICC profile (frame or container) if policy is preserve. */
    const uint8_t *icc_ptr = NULL;
    size_t icc_len = 0u;
    if (meta_policy == RFC_META_POLICY_PRESERVE) {
        if (fr->metadata.icc && fr->metadata.icc_len > 0u) {
            icc_ptr = fr->metadata.icc;
            icc_len = fr->metadata.icc_len;
        } else if (meta && meta->icc && meta->icc_len > 0u) {
            icc_ptr = meta->icc;
            icc_len = meta->icc_len;
        }
        if (icc_len > UINT32_MAX) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }

    uint64_t work_reserve = 0u;
    st = jp2_compute_work_reserve(w, h, nc, &work_reserve);
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
    mw.budget = exec->budget;

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
            const uint16_t *row16 = (const uint16_t *)row;
            for (uint32_t x = 0u; x < w; x++) {
                size_t idx = row_px + (size_t)x;
                for (uint16_t c = 0u; c < nc; c++) {
                    uint32_t v = (uint32_t)row16[(size_t)x * nc + c];
                    img->comps[c].data[idx] = (OPJ_INT32)(v >> shift);
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

    enc = opj_create_compress(OPJ_CODEC_JP2);
    if (!enc) {
        st = RFC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
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

    stream = opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_FALSE);
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

    /* Release OpenJPEG handles and working reserve before final output copy. */
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
        st = jp2_ensure_rgba_cdef(mw.data, mw.len, exec->allocator, &final_buf,
                                  &final_len);
    } else {
        final_buf = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx,
                                                      mw.len);
        if (!final_buf) {
            st = RFC_ERR_OUT_OF_MEMORY;
        } else {
            memcpy(final_buf, mw.data, mw.len);
            final_len = mw.len;
            st = RFC_OK;
        }
    }
    if (st != RFC_OK) {
        goto cleanup;
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
