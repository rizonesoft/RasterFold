/* Converter-private zero-allocation JPEG marker scanner (D10 T02 s3 + s5).
 * Moved from converter/codecs/jxl.c so the JPEG XL reconstruction adapter and
 * the JPEG adapter share one parser; the JPEG XL acceptance rules are kept
 * by calling it without RFC_JPEG_SCAN_ALLOW_ARITHMETIC and ignoring the
 * extra structure it records. */

#include "jpeg_scan.h"

#include "rfc_verify.h"

#include <string.h>

static uint16_t scan_rd16be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int scan_add_sz(size_t a, size_t b, size_t *out) {
    if (a > (size_t)-1 - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static const char k_xmp_std[] = "http://ns.adobe.com/xap/1.0/";       /* + NUL: 29 */
static const char k_xmp_ext[] = "http://ns.adobe.com/xmp/extension/"; /* + NUL: 35 */

rfc_status rfc_jpeg_scan(const uint8_t *bytes,
                         size_t size,
                         size_t meta_cap,
                         uint32_t flags,
                         rfc_jpeg_scan_info *out) {
    memset(out, 0, sizeof(*out));
    out->adobe_transform = -1;
    if (!bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (size < 2u) {
        return (size == 0u || bytes[0] == 0xFFu) ? RFC_ERR_TRUNCATED_INPUT
                                                 : RFC_ERR_INVALID_ARGUMENT;
    }
    if (bytes[0] != 0xFFu || bytes[1] != 0xD8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    size_t pos = 2u;
    int seen_sof = 0;
    int seen_eoi = 0;
    int seen_sos = 0;
    out->header_adobe_transform = -1;
    uint16_t qt[4][64];
    uint8_t qt_defined[4] = {0u, 0u, 0u, 0u};
    uint16_t latched[4][64];
    uint8_t is_latched[4] = {0u, 0u, 0u, 0u};
    while (pos < size) {
        if (bytes[pos] != 0xFFu) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        while (pos < size && bytes[pos] == 0xFFu) {
            pos++;
        }
        if (pos >= size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint8_t marker = bytes[pos++];
        if (marker == 0x00u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (marker == 0xD9u) {
            seen_eoi = 1;
            break;
        }
        if (marker == 0x01u || (marker >= 0xD0u && marker <= 0xD7u)) {
            continue;
        }
        if (size - pos < 2u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint16_t seg_len = scan_rd16be(bytes + pos);
        if (seg_len < 2u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if ((size_t)seg_len > size - pos) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        const uint8_t *payload = bytes + pos + 2u;
        size_t plen = (size_t)seg_len - 2u;
        size_t payload_off = pos + 2u;
        pos += (size_t)seg_len;

        int is_arith_sof = (marker == 0xC9u || marker == 0xCAu);
        if (marker == 0xC0u || marker == 0xC1u || marker == 0xC2u ||
            (is_arith_sof && (flags & RFC_JPEG_SCAN_ALLOW_ARITHMETIC) != 0u)) {
            if (plen < 6u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            uint8_t prec = payload[0];
            uint16_t h = scan_rd16be(payload + 1u);
            uint16_t w = scan_rd16be(payload + 3u);
            uint8_t ncomp = payload[5];
            if (plen != 6u + (size_t)ncomp * 3u || w == 0u || h == 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (prec != 8u || (ncomp != 1u && ncomp != 3u)) {
                return RFC_ERR_UNSUPPORTED_SAMPLE;
            }
            out->sof_count++;
            if (!seen_sof) {
                out->width = w;
                out->height = h;
                out->components = ncomp;
                out->bits_per_sample = prec;
                out->sof_marker = marker;
                for (uint32_t c = 0u; c < (uint32_t)ncomp; c++) {
                    const uint8_t *cs = payload + 6u + (size_t)c * 3u;
                    out->component_ids[c] = cs[0];
                    out->h_samp[c] = (uint8_t)(cs[1] >> 4);
                    out->v_samp[c] = (uint8_t)(cs[1] & 0x0Fu);
                    out->quant_slot[c] = cs[2];
                }
                seen_sof = 1;
            }
        } else if ((marker >= 0xC3u && marker <= 0xCFu) && marker != 0xC4u &&
                   marker != 0xC8u && marker != 0xCCu) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        } else if (marker == 0xDBu) {
            size_t q = 0u;
            while (q < plen) {
                uint8_t pq = (uint8_t)(payload[q] >> 4);
                uint8_t tq = (uint8_t)(payload[q] & 0x0Fu);
                size_t need = 1u + (pq ? 128u : 64u);
                if (pq > 1u || tq > 3u || plen - q < need) {
                    break; /* the decoder refuses a malformed DQT */
                }
                for (size_t k = 0u; k < 64u; k++) {
                    qt[tq][k] = pq ? (uint16_t)scan_rd16be(payload + q + 1u + 2u * k)
                                   : (uint16_t)payload[q + 1u + k];
                }
                qt_defined[tq] = 1u;
                q += need;
            }
        } else if (marker == 0xDAu) {
            out->sos_count++;
            seen_sos = 1;
            /* SOS: Ns, Ns x (Cs, Td/Ta), Ss, Se, Ah/Al. Malformed headers are
             * counted (the decoder refuses them), never fatal here. */
            if (!seen_sof || plen < 1u || plen != 1u + 2u * (size_t)payload[0] + 3u ||
                payload[0] == 0u || payload[0] > 4u) {
                out->sos_malformed++;
            } else {
                uint8_t ns = payload[0];
                uint8_t ss = payload[1u + 2u * ns];
                uint8_t se = payload[2u + 2u * ns];
                uint8_t ah = (uint8_t)(payload[3u + 2u * ns] >> 4);
                for (uint8_t k = 0u; k < ns; k++) {
                    uint8_t cid = payload[1u + 2u * k];
                    uint8_t tbl = payload[2u + 2u * k];
                    uint32_t ci = 0u;
                    while (ci < out->components && out->component_ids[ci] != cid) {
                        ci++;
                    }
                    if (ci >= out->components) {
                        out->sos_malformed++;
                        continue;
                    }
                    if (ss == 0u && ah == 0u) {
                        out->dc_sel[ci] = (uint8_t)(tbl >> 4);
                    }
                    if (se > 0u) {
                        out->ac_sel[ci] = (uint8_t)(tbl & 0x0Fu);
                    }
                    if (!is_latched[ci]) {
                        uint8_t slot = out->quant_slot[ci] & 3u;
                        if (out->quant_slot[ci] > 3u || !qt_defined[slot]) {
                            out->quant_missing = 1;
                        } else {
                            memcpy(latched[ci], qt[slot], sizeof(latched[ci]));
                            is_latched[ci] = 1u;
                        }
                    }
                }
            }
            while (pos < size) {
                if (bytes[pos] != 0xFFu) {
                    pos++;
                    continue;
                }
                pos++;
                if (pos >= size) {
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                uint8_t b = bytes[pos];
                if (b == 0x00u || (b >= 0xD0u && b <= 0xD7u)) {
                    pos++;
                    continue;
                }
                if (b == 0xFFu) {
                    continue;
                }
                pos--;
                break;
            }
        } else if ((marker >= 0xE0u && marker <= 0xEFu) || marker == 0xFEu) {
            out->marker_count++;
            if (!scan_add_sz(out->marker_bytes, (size_t)seg_len + 2u,
                             &out->marker_bytes)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            /* Same recognition lengths as libjpeg's examine_app0/app14. */
            if (marker == 0xE0u && plen >= 14u &&
                memcmp(payload, "JFIF\x00", 5) == 0) {
                out->saw_jfif = 1;
                if (!seen_sos) {
                    out->header_jfif = 1;
                } else {
                    out->late_structural++;
                }
            } else if (marker == 0xEEu && plen >= 12u &&
                       memcmp(payload, "Adobe", 5) == 0) {
                out->saw_adobe = 1;
                out->adobe_transform = (int)payload[11];
                if (!seen_sos) {
                    out->header_adobe = 1;
                    out->header_adobe_transform = (int)payload[11];
                } else {
                    out->late_structural++;
                }
            } else if (marker == 0xE1u) {
                if (plen >= 6u && memcmp(payload, "Exif\x00\x00", 6) == 0) {
                    out->exif_count++;
                    if (!out->has_exif) {
                        const uint8_t *tiff = payload + 6u;
                        size_t tiff_len = plen - 6u;
                        rfc_orientation ori = RFC_ORIENTATION_UNSPECIFIED;
                        if (tiff_len > meta_cap) {
                            return RFC_ERR_LIMIT_EXCEEDED;
                        }
                        if (rfc_exif_parse_orientation(tiff, tiff_len, &ori) !=
                            RFC_OK) {
                            return RFC_ERR_INVALID_ARGUMENT;
                        }
                        out->has_exif = 1;
                        out->exif_off = payload_off + 6u;
                        out->exif_len = tiff_len;
                        out->orientation = (ori != RFC_ORIENTATION_UNSPECIFIED)
                                               ? ori
                                               : RFC_ORIENTATION_NORMAL;
                    }
                } else if (plen >= sizeof(k_xmp_std) &&
                           memcmp(payload, k_xmp_std, sizeof(k_xmp_std)) == 0) {
                    out->xmp_count++;
                    if (!out->has_xmp) {
                        size_t xlen = plen - sizeof(k_xmp_std);
                        if (xlen > meta_cap) {
                            return RFC_ERR_LIMIT_EXCEEDED;
                        }
                        if (xlen > 0u) {
                            out->has_xmp = 1;
                            out->xmp_off = payload_off + sizeof(k_xmp_std);
                            out->xmp_len = xlen;
                        }
                    }
                } else if (plen >= sizeof(k_xmp_ext) &&
                           memcmp(payload, k_xmp_ext, sizeof(k_xmp_ext)) == 0) {
                    out->xmp_ext_count++;
                }
            } else if (marker == 0xE2u) {
                if (plen >= 12u &&
                    memcmp(payload, "ICC_PROFILE\x00", 12) == 0) {
                    if (plen < 14u) {
                        out->icc_invalid = 1; /* signature without seq/count */
                    } else {
                        uint8_t seq = payload[12];
                        uint8_t tot = payload[13];
                        if (seq == 0u || tot == 0u || seq > tot ||
                            (out->icc_total_chunks != 0u &&
                             out->icc_total_chunks != tot)) {
                            out->icc_invalid = 1;
                        } else {
                            out->icc_total_chunks = tot;
                            if (!out->icc_seen[seq]) {
                                out->icc_seen[seq] = 1u;
                                out->icc_chunks[seq].off = payload_off + 14u;
                                out->icc_chunks[seq].len = plen - 14u;
                            } else {
                                out->icc_invalid = 1; /* duplicate sequence */
                            }
                        }
                    }
                } else if (plen >= 4u && memcmp(payload, "MPF\x00", 4) == 0) {
                    out->mpf_count++;
                }
            }
        }
    }

    if (!seen_eoi) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (!seen_sof) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t c = 0u; c < out->components; c++) {
        uint8_t slot = out->quant_slot[c] & 3u;
        if (is_latched[c] && qt_defined[slot] &&
            memcmp(latched[c], qt[slot], sizeof(latched[c])) != 0) {
            out->quant_redefined = 1;
        }
    }
    out->eoi_end = pos;
    for (size_t i = pos; i < size; i++) {
        if (bytes[i] != 0x00u && bytes[i] != 0xFFu) {
            out->trailing_nonpad++;
        }
    }
    return RFC_OK;
}

rfc_status rfc_jpeg_scan_materialize_meta(const rfc_jpeg_scan_info *info,
                                          const uint8_t *bytes,
                                          const rfc_allocator *alloc,
                                          size_t meta_cap,
                                          rfc_metadata *out_meta) {
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = info->has_exif ? info->orientation
                                           : RFC_ORIENTATION_NORMAL;

    if (info->icc_total_chunks > 0u && !info->icc_invalid) {
        size_t icc_sum = 0u;
        int complete = 1;
        for (uint32_t i = 1u; i <= (uint32_t)info->icc_total_chunks; i++) {
            if (!info->icc_seen[i]) {
                complete = 0;
                break;
            }
            if (!scan_add_sz(icc_sum, info->icc_chunks[i].len, &icc_sum)) {
                return RFC_ERR_OVERFLOW;
            }
        }
        if (complete && icc_sum > 0u) {
            if (icc_sum > meta_cap) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            uint8_t *icc = (uint8_t *)alloc->alloc(alloc->ctx, icc_sum);
            if (!icc) {
                return RFC_ERR_OUT_OF_MEMORY;
            }
            size_t wp = 0u;
            for (uint32_t i = 1u; i <= (uint32_t)info->icc_total_chunks; i++) {
                memcpy(icc + wp, bytes + info->icc_chunks[i].off,
                       info->icc_chunks[i].len);
                wp += info->icc_chunks[i].len;
            }
            out_meta->icc = icc;
            out_meta->icc_len = icc_sum;
        }
    }

    if (info->has_exif && info->exif_len > 0u) {
        if (info->exif_len > meta_cap) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        uint8_t *ex = (uint8_t *)alloc->alloc(alloc->ctx, info->exif_len);
        if (!ex) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(ex, bytes + info->exif_off, info->exif_len);
        out_meta->exif = ex;
        out_meta->exif_len = info->exif_len;
    }

    if (info->has_xmp && info->xmp_len > 0u) {
        if (info->xmp_len > meta_cap) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        uint8_t *xm = (uint8_t *)alloc->alloc(alloc->ctx, info->xmp_len);
        if (!xm) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(xm, bytes + info->xmp_off, info->xmp_len);
        out_meta->xmp = xm;
        out_meta->xmp_len = info->xmp_len;
    }
    return RFC_OK;
}

int rfc_jpeg_next_app_segment(const uint8_t *bytes,
                              size_t size,
                              size_t *pos,
                              uint8_t *out_marker,
                              const uint8_t **out_payload,
                              size_t *out_len) {
    size_t p = *pos;
    while (p < size) {
        if (bytes[p] != 0xFFu) {
            return 0;
        }
        while (p < size && bytes[p] == 0xFFu) {
            p++;
        }
        if (p >= size) {
            return 0;
        }
        uint8_t marker = bytes[p++];
        if (marker == 0xD9u || marker == 0x00u) {
            return 0;
        }
        if (marker == 0x01u || (marker >= 0xD0u && marker <= 0xD7u)) {
            continue;
        }
        if (size - p < 2u) {
            return 0;
        }
        uint16_t seg_len = scan_rd16be(bytes + p);
        if (seg_len < 2u || (size_t)seg_len > size - p) {
            return 0;
        }
        const uint8_t *payload = bytes + p + 2u;
        size_t plen = (size_t)seg_len - 2u;
        p += (size_t)seg_len;
        if (marker == 0xDAu) {
            while (p < size) {
                if (bytes[p] != 0xFFu) {
                    p++;
                    continue;
                }
                if (p + 1u >= size) {
                    return 0;
                }
                uint8_t b = bytes[p + 1u];
                if (b == 0x00u || (b >= 0xD0u && b <= 0xD7u)) {
                    p += 2u;
                    continue;
                }
                if (b == 0xFFu) {
                    p++;
                    continue;
                }
                break;
            }
        } else if ((marker >= 0xE0u && marker <= 0xEFu) || marker == 0xFEu) {
            *pos = p;
            *out_marker = marker;
            *out_payload = payload;
            *out_len = plen;
            return 1;
        }
    }
    return 0;
}
