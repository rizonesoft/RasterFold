/* Methods 2 and 3: palette with equality contexts and binary range coding. */
#include "rf.h"
#include "rc.h"

typedef struct {
    uint16_t d1[256];
    uint8_t d1_n[256];
    uint16_t d2[128];
    uint8_t d2_n[128];
    uint16_t d3[128];
    uint8_t d3_n[128];
    uint16_t tree[256];
    uint8_t tree_n[256];
    uint16_t rate[61];
} pal_model;

typedef struct {
    pal_model m;
    uint32_t ht_key[1024];
    uint8_t ht_idx[1024];
} pal_scratch;

static void pal_model_init(pal_model *m) {
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < 256; i++) m->d1[i] = 32768u;
    for (int i = 0; i < 128; i++) { m->d2[i] = 32768u; m->d3[i] = 32768u; }
    for (int i = 0; i < 256; i++) m->tree[i] = 32768u;
    for (int n = 0; n <= 60; n++) m->rate[n] = (uint16_t)((65536 * 2) / (2 * n + 3));
}

RF_INLINE void pal_update_m3(uint16_t *c, uint32_t r, int bit) {
    uint32_t cur = (uint32_t)*c;
    if (bit) {
        if (cur < 65535u) {
            uint32_t step = ((65535u - cur) * r) >> 16;
            if (step == 0u) step = 1u;
            *c = (uint16_t)(cur + step);
        }
    } else {
        if (cur > 0u) {
            uint32_t step = (cur * r) >> 16;
            if (step == 0u) step = 1u;
            *c = (uint16_t)(cur - step);
        }
    }
}

RF_INLINE void pal_enc_bit(renc *enc, const uint16_t *rate_tab, uint16_t *c, uint8_t *n, int bit) {
    uint32_t cur = (uint32_t)*c;
    uint32_t p = cur >> 4;
    if (p == 0) p = 1u;
    uint8_t cnt = *n;
    uint32_t r = rate_tab[cnt];
    if (cnt < 60u) *n = (uint8_t)(cnt + 1u);
    renc_bit(enc, bit, p);
    pal_update_m3(c, r, bit);
}

RF_INLINE int pal_dec_bit(rdec *dec, const uint16_t *rate_tab, uint16_t *c, uint8_t *n, int m3) {
    uint32_t p = (uint32_t)(*c >> 4);
    if (p == 0) p = 1u;
    uint32_t r = rate_tab[*n];
    if (*n < 60u) (*n)++;
    int bit = rdec_bit(dec, p);
    if (m3) {
        pal_update_m3(c, r, bit);
    } else {
        int32_t target = bit ? 65535 : 0;
        int32_t cur = (int32_t)*c;
        *c = (uint16_t)(cur + (((target - cur) * (int32_t)r) >> 16));
    }
    return bit;
}

RF_INLINE uint32_t pal_neighbours_m2(const uint8_t *cur_row, const uint8_t *prv_row, uint32_t w, uint32_t x, uint32_t y,
                                     uint32_t *a_out, uint32_t *b_out, uint32_t *d_out) {
    uint32_t a, b, c, d;
    if (y == 0) {
        a = x > 0 ? cur_row[x - 1] : 0u;
        b = c = d = a;
    } else {
        b = prv_row[x];
        a = x > 0 ? cur_row[x - 1] : b;
        c = x > 0 ? prv_row[x - 1] : b;
        d = (x + 1u < w) ? prv_row[x + 1u] : b;
    }
    *a_out = a;
    *b_out = b;
    *d_out = d;
    return (uint32_t)(a == b) | ((uint32_t)(a == c) << 1) | ((uint32_t)(a == d) << 2)
         | ((uint32_t)(b == c) << 3) | ((uint32_t)(b == d) << 4);
}

RF_INLINE uint32_t pal_pat_m3(const uint8_t *cur_row, const uint8_t *prv_row, const uint8_t *prv2_row,
                              uint32_t w, uint32_t x, uint32_t y,
                              uint32_t *a_out, uint32_t *b_out, uint32_t *c_out, uint32_t *d_out, uint32_t *ww_out) {
    uint32_t a, b, c, d, ww, nn;
    if (y >= 2u && x >= 2u && x + 1u < w) {
        a = cur_row[x - 1u];
        b = prv_row[x];
        c = prv_row[x - 1u];
        d = prv_row[x + 1u];
        ww = cur_row[x - 2u];
        nn = prv2_row[x];
    } else {
        if (y == 0) {
            a = x > 0 ? cur_row[x - 1] : 0u;
            b = c = d = a;
            nn = a;
        } else {
            b = prv_row[x];
            a = x > 0 ? cur_row[x - 1] : b;
            c = x > 0 ? prv_row[x - 1] : b;
            d = (x + 1u < w) ? prv_row[x + 1u] : b;
            nn = (y == 1u) ? b : prv2_row[x];
        }
        ww = (x >= 2u) ? cur_row[x - 2u] : a;
    }
    *a_out = a;
    *b_out = b;
    *c_out = c;
    *d_out = d;
    *ww_out = ww;
    return (uint32_t)(a == b) | ((uint32_t)(a == c) << 1) | ((uint32_t)(a == d) << 2)
         | ((uint32_t)(b == c) << 3) | ((uint32_t)(b == d) << 4)
         | ((uint32_t)(ww == a) << 5) | ((uint32_t)(nn == b) << 6);
}

RF_INLINE uint32_t pal_third_m3(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t ww) {
    return (d != a && d != b) ? d : ((c != a && c != b) ? c : ((ww != a && ww != b) ? ww : a));
}

/* 1024-slot open-addressing hash of R<<16|G<<8|B (stopping at the 257th colour),
   packed in 1024 stack bytes (< 4096) so Windows x64 freestanding builds never
   emit ___chkstk_ms and >256-colour photographs do zero heap allocations. */
static RF_NOINLINE uint32_t pal_build_internal(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *pal, uint8_t *ht_idx_out) {
    size_t npix = (size_t)w * h;
    if (npix == 0) return 0;
    uint8_t ht_idx_local[1024];
    uint8_t *ht_idx = ht_idx_out ? ht_idx_out : ht_idx_local;
    memset(ht_idx, 0, 1024u);
    pal[0] = rgb[0];
    pal[1] = rgb[1];
    pal[2] = rgb[2];
    uint32_t first_key = ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
    uint32_t count = 1u;
    uint32_t prev_key0 = first_key, prev_key1 = first_key;
    for (size_t i = 1; i < npix; i++) {
        uint32_t r = rgb[3 * i + 0], g = rgb[3 * i + 1], b = rgb[3 * i + 2];
        uint32_t key = (r << 16) | (g << 8) | b;
        if (key == prev_key0 || key == prev_key1) continue;
        prev_key1 = prev_key0;
        prev_key0 = key;
        if (key == first_key) continue;
        uint32_t slot = (key * 0x9E3779B1u) >> 22;
        while (ht_idx[slot] != 0) {
            uint32_t idx = ht_idx[slot];
            if (pal[3 * idx + 0] == (uint8_t)r && pal[3 * idx + 1] == (uint8_t)g && pal[3 * idx + 2] == (uint8_t)b)
                break;
            slot = (slot + 1u) & 1023u;
        }
        if (ht_idx[slot] == 0) {
            if (count == 256u) return 0;
            ht_idx[slot] = (uint8_t)count;
            pal[3 * count + 0] = (uint8_t)r;
            pal[3 * count + 1] = (uint8_t)g;
            pal[3 * count + 2] = (uint8_t)b;
            count++;
        }
    }
    return count;
}

RF_NOINLINE uint32_t rf_pal_build(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *pal) {
    return pal_build_internal(rgb, w, h, pal, 0);
}

int rf_pal_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap, size_t *written, rf_error *err) {
    *written = 0;
    uint8_t pal[256 * 3];
    uint8_t ht_idx[1024];
    uint32_t count = pal_build_internal(rgb, w, h, pal, ht_idx);
    if (count == 0) return RF_OK;
    size_t hdr_len = 2u + 3u * (size_t)count + 4u;
    if (cap < hdr_len + 5u) return RF_OK;
    size_t stream_cap = cap - hdr_len;
    size_t row_bytes = (size_t)w * 3u;
    size_t scratch_bytes = (sizeof(pal_scratch) + 63u) & ~(size_t)63u;
    uint8_t *alloc_buf = rf_alloc(scratch_bytes + cap + 64u + row_bytes);
    if (!alloc_buf) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    pal_scratch *sc = (pal_scratch *)alloc_buf;
    uint8_t *work = alloc_buf + scratch_bytes;
    uint8_t *rows = work + cap + 64u;
    pal_model_init(&sc->m);
    renc enc;
    renc_init(&enc, work + hdr_len, stream_cap + 32u);
    uint32_t hit = 0;
    if (count == 1u) {
        size_t npix = (size_t)w * h;
        for (size_t i = 0; i < npix; i++) {
            uint32_t d1_ctx = 127u * 2u + hit;
            pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[d1_ctx], &sc->m.d1_n[d1_ctx], 1);
            hit = 1u;
        }
        if (enc.pos > stream_cap) {
            rf_free(alloc_buf);
            return RF_OK;
        }
    } else {
        uint32_t first_key = ((uint32_t)pal[0] << 16) | ((uint32_t)pal[1] << 8) | (uint32_t)pal[2];
        uint32_t prev_key0 = first_key, prev_idx0 = 0;
        uint32_t prev_key1 = first_key, prev_idx1 = 0;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *cur_row = rows + (size_t)(y % 3u) * w;
            const uint8_t *prv_row = rows + (size_t)((y + 2u) % 3u) * w;
            const uint8_t *prv2_row = rows + (size_t)((y + 1u) % 3u) * w;
            const uint8_t *p = rgb + (size_t)y * w * 3u;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t r = p[3 * x + 0], g = p[3 * x + 1], bl = p[3 * x + 2];
                uint32_t key = (r << 16) | (g << 8) | bl;
                uint32_t idx;
                if (key == prev_key0) {
                    idx = prev_idx0;
                    cur_row[x] = (uint8_t)idx;
                    if (y >= 2u && x >= 2u && x + 1u < w) {
                        uint32_t b = prv_row[x];
                        uint32_t c = prv_row[x - 1u];
                        uint32_t d = prv_row[x + 1u];
                        uint32_t ww = cur_row[x - 2u];
                        uint32_t nn = prv2_row[x];
                        uint32_t pat = (uint32_t)(idx == b) | ((uint32_t)(idx == c) << 1) | ((uint32_t)(idx == d) << 2)
                                     | ((uint32_t)(b == c) << 3) | ((uint32_t)(b == d) << 4)
                                     | ((uint32_t)(ww == idx) << 5) | ((uint32_t)(nn == b) << 6);
                        uint32_t d1_ctx = pat * 2u + hit;
                        pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[d1_ctx], &sc->m.d1_n[d1_ctx], 1);
                        hit = 1u;
                        continue;
                    }
                } else if (key == prev_key1) {
                    idx = prev_idx1;
                    prev_key1 = prev_key0;
                    prev_idx1 = prev_idx0;
                    prev_key0 = key;
                    prev_idx0 = idx;
                    cur_row[x] = (uint8_t)idx;
                } else if (key == first_key) {
                    idx = 0;
                    prev_key1 = prev_key0;
                    prev_idx1 = prev_idx0;
                    prev_key0 = key;
                    prev_idx0 = 0;
                    cur_row[x] = 0;
                } else {
                    uint32_t slot = (key * 0x9E3779B1u) >> 22;
                    for (;;) {
                        idx = ht_idx[slot];
                        if (idx != 0 && pal[3 * idx + 0] == (uint8_t)r && pal[3 * idx + 1] == (uint8_t)g && pal[3 * idx + 2] == (uint8_t)bl)
                            break;
                        slot = (slot + 1u) & 1023u;
                    }
                    prev_key1 = prev_key0;
                    prev_idx1 = prev_idx0;
                    prev_key0 = key;
                    prev_idx0 = idx;
                    cur_row[x] = (uint8_t)idx;
                }
                uint32_t a, b, c, d, ww;
                uint32_t pat = pal_pat_m3(cur_row, prv_row, prv2_row, w, x, y, &a, &b, &c, &d, &ww);
                uint32_t d1_ctx = pat * 2u + hit;
                if (idx == a) {
                    pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[d1_ctx], &sc->m.d1_n[d1_ctx], 1);
                    hit = 1u;
                    continue;
                }
                pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[d1_ctx], &sc->m.d1_n[d1_ctx], 0);
                hit = 0;
                if (count == 2u) continue;
                if (b != a) {
                    if (idx == b) {
                        pal_enc_bit(&enc, sc->m.rate, &sc->m.d2[pat], &sc->m.d2_n[pat], 1);
                        continue;
                    }
                    pal_enc_bit(&enc, sc->m.rate, &sc->m.d2[pat], &sc->m.d2_n[pat], 0);
                }
                uint32_t e = pal_third_m3(a, b, c, d, ww);
                if (e != a && e != b) {
                    if (idx == e) {
                        pal_enc_bit(&enc, sc->m.rate, &sc->m.d3[pat], &sc->m.d3_n[pat], 1);
                        continue;
                    }
                    pal_enc_bit(&enc, sc->m.rate, &sc->m.d3[pat], &sc->m.d3_n[pat], 0);
                }
                uint32_t node = 1u;
                for (int bit_pos = 7; bit_pos >= 0; bit_pos--) {
                    int bit = (int)((idx >> bit_pos) & 1u);
                    pal_enc_bit(&enc, sc->m.rate, &sc->m.tree[node], &sc->m.tree_n[node], bit);
                    node = node * 2u + (uint32_t)bit;
                }
            }
            if (enc.pos > stream_cap) {
                rf_free(alloc_buf);
                return RF_OK;
            }
        }
    }
    renc_flush(&enc);
    size_t stream_len = enc.pos;
    if (stream_len > stream_cap) {
        rf_free(alloc_buf);
        return RF_OK;
    }
    uint32_t c_m1 = count - 1u;
    work[0] = (uint8_t)c_m1;
    work[1] = (uint8_t)(c_m1 >> 8);
    memcpy(work + 2, pal, 3u * (size_t)count);
    size_t s_off = 2u + 3u * (size_t)count;
    uint32_t s32 = (uint32_t)stream_len;
    work[s_off + 0] = (uint8_t)s32;
    work[s_off + 1] = (uint8_t)(s32 >> 8);
    work[s_off + 2] = (uint8_t)(s32 >> 16);
    work[s_off + 3] = (uint8_t)(s32 >> 24);
    size_t total = hdr_len + stream_len;
    memcpy(out, work, total);
    *written = total;
    rf_free(alloc_buf);
    return RF_OK;
}

int rf_pal_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, uint32_t method, rf_error *err) {
    if (n < 6u) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
    uint32_t count_m1 = (uint32_t)in[0] | ((uint32_t)in[1] << 8);
    if (count_m1 > 255u) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
    uint32_t count = count_m1 + 1u;
    size_t pal_bytes = 3u * (size_t)count;
    if (n < 6u + pal_bytes) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
    const uint8_t *pal = in + 2;
    const uint8_t *sp = in + 2u + pal_bytes;
    uint32_t stream_len = (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24);
    if (stream_len < 5u || (uint64_t)stream_len != (uint64_t)(n - 6u - pal_bytes))
        return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
    size_t scratch_bytes = (sizeof(pal_scratch) + 63u) & ~(size_t)63u;
    uint8_t *alloc_buf = rf_alloc(scratch_bytes + (size_t)w * 3u);
    if (!alloc_buf) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    pal_scratch *sc = (pal_scratch *)alloc_buf;
    uint8_t *rows = alloc_buf + scratch_bytes;
    memset(sc->ht_key, 0xFF, sizeof(sc->ht_key));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t key = ((uint32_t)pal[3 * i + 0] << 16) | ((uint32_t)pal[3 * i + 1] << 8) | (uint32_t)pal[3 * i + 2];
        uint32_t slot = (key * 0x9E3779B1u) >> 22;
        while (sc->ht_key[slot] != 0xFFFFFFFFu) {
            if (sc->ht_key[slot] == key) {
                rf_free(alloc_buf);
                return rf_fail(err, RF_ERR_FORMAT, "invalid fold: duplicate palette colour");
            }
            slot = (slot + 1u) & 1023u;
        }
        sc->ht_key[slot] = key;
    }
    pal_model_init(&sc->m);
    rdec dec;
    rdec_init(&dec, sp + 4, stream_len);
    uint32_t hit = 0;
    int bad = 0;
    int m3 = (method == RF_METHOD_PAL2);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *cur_row = rows + (size_t)(y % 3u) * w;
        const uint8_t *prv_row = rows + (size_t)((y + 2u) % 3u) * w;
        const uint8_t *prv2_row = rows + (size_t)((y + 1u) % 3u) * w;
        uint8_t *dst = stride ? (out + 54u + (size_t)(h - 1u - y) * stride) : (out + (size_t)y * w * 3u);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t a, b, c = 0, d = 0, ww = 0, e = 0;
            uint32_t pat = m3 ? pal_pat_m3(cur_row, prv_row, prv2_row, w, x, y, &a, &b, &c, &d, &ww)
                              : pal_neighbours_m2(cur_row, prv_row, w, x, y, &a, &b, &e);
            uint32_t d1_ctx = pat * 2u + hit;
            uint32_t idx;
            if (pal_dec_bit(&dec, sc->m.rate, &sc->m.d1[d1_ctx], &sc->m.d1_n[d1_ctx], m3)) {
                idx = a;
                hit = 1u;
            } else if (m3 && count == 2u) {
                idx = 1u - a;
                hit = 0;
            } else {
                hit = 0;
                if (b != a && pal_dec_bit(&dec, sc->m.rate, &sc->m.d2[pat], &sc->m.d2_n[pat], m3)) {
                    idx = b;
                } else {
                    if (m3) e = pal_third_m3(a, b, c, d, ww);
                    if (e != a && e != b && pal_dec_bit(&dec, sc->m.rate, &sc->m.d3[pat], &sc->m.d3_n[pat], m3)) {
                        idx = e;
                    } else {
                        uint32_t node = 1u;
                        for (int bit_pos = 0; bit_pos < 8; bit_pos++) {
                            int bit = pal_dec_bit(&dec, sc->m.rate, &sc->m.tree[node], &sc->m.tree_n[node], m3);
                            node = node * 2u + (uint32_t)bit;
                        }
                        idx = node - 256u;
                        if (idx >= count || idx == a || idx == b || idx == e) {
                            bad = 1;
                            break;
                        }
                    }
                }
            }
            if (dec.in >= dec.lim) rdec_tail(&dec);
            cur_row[x] = (uint8_t)idx;
            const uint8_t *col = pal + 3u * (size_t)idx;
            size_t px = 3u * (size_t)x;
            if (stride) {
                dst[px + 0] = col[2];
                dst[px + 1] = col[1];
                dst[px + 2] = col[0];
            } else {
                dst[px + 0] = col[0];
                dst[px + 1] = col[1];
                dst[px + 2] = col[2];
            }
        }
        if (bad || dec.in >= dec.lim) {
            bad = 1;
            break;
        }
    }
    rf_free(alloc_buf);
    if (bad || dec.in != dec.end) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette stream");
    return RF_OK;
}
