/* Methods 2, 3 and 4: palette with equality contexts and binary range coding.
   The encoder writes method 4; methods 2 and 3 are decode-only. */
#include "rf.h"
#include "rc.h"

typedef struct {
    uint16_t d1[768];            /* methods 2 and 3 use the first 256 */
    uint8_t d1_n[768];
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
    uint16_t val[3][256];        /* method 4 palette field: the three Rice inputs per entry */
    uint8_t ord[256];
    uint8_t nidx[256];           /* first-seen index -> sorted index */
    uint8_t spal[768];
} pal_scratch;

static void pal_model_init(pal_model *m) {
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < 768; i++) m->d1[i] = 32768u;
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

/* Method 4 decision: probability max(1, c) / 65536. */
RF_INLINE void pal_enc_bit(renc *enc, const uint16_t *rate_tab, uint16_t *c, uint8_t *n, int bit) {
    uint32_t cur = (uint32_t)*c;
    if (bit && cur == 65535u && *n == 60u) {
        renc_bit16(enc, 1, 65535u);
        return;
    }
    uint32_t p = cur;
    if (p == 0) p = 1u;
    uint8_t cnt = *n;
    uint32_t r = rate_tab[cnt];
    if (cnt < 60u) *n = (uint8_t)(cnt + 1u);
    renc_bit16(enc, bit, p);
    pal_update_m3(c, r, bit);
}

/* mode: 0 = method 2, 1 = method 3, 2 = method 4. */
RF_INLINE int pal_dec_bit(rdec *dec, const uint16_t *rate_tab, uint16_t *c, uint8_t *n, int mode) {
    uint32_t p = mode == 2 ? (uint32_t)*c : (uint32_t)(*c >> 4);
    if (p == 0) p = 1u;
    uint32_t r = rate_tab[*n];
    if (*n < 60u) (*n)++;
    int bit = mode == 2 ? rdec_bit16(dec, p) : rdec_bit(dec, p);
    if (mode) {
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

RF_INLINE int pal_same4(const uint8_t *p) {
    uint64_t a, b;
    uint32_t c, d;
    __builtin_memcpy(&a, p, 8u);
    __builtin_memcpy(&b, p + 3u, 8u);
    __builtin_memcpy(&c, p + 8u, 4u);
    __builtin_memcpy(&d, p + 11u, 4u);
    return a == b && c == d;
}

/* Method 4 border class of a pixel: 1 on row 0, 2 in column 0 of later rows, else 0. */
RF_INLINE uint32_t pal_class(uint32_t x, uint32_t y) { return y == 0 ? 256u : (x == 0 ? 512u : 0u); }

/* Method 4 palette field (spec/FOLD.md, "Method 4"): a bit string, most
   significant bit first. */
RF_INLINE uint32_t pal_zz(int32_t v) { return v >= 0 ? (uint32_t)v << 1 : (((uint32_t)(-v)) << 1) - 1u; }

static void pal_put(uint8_t *p, size_t *bit, uint32_t v, uint32_t n) {
    for (uint32_t i = n; i-- > 0;) {
        p[*bit >> 3] |= (uint8_t)(((v >> i) & 1u) << (7u - (*bit & 7u)));
        (*bit)++;
    }
}

static void pal_put_rice(uint8_t *p, size_t *bit, uint32_t v, uint32_t k) {
    for (uint32_t q = v >> k; q > 0; q--) pal_put(p, bit, 1u, 1u);
    pal_put(p, bit, 0u, 1u);
    pal_put(p, bit, v, k);
}

/* Fills sc->val and k[3] for the sorted palette and returns the field's size in bytes. */
static size_t pal_field_plan(pal_scratch *sc, const uint8_t *pal, uint32_t count, uint32_t k[3]) {
    uint32_t pr = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = pal + 3u * (size_t)i;
        uint32_t r = e[0], g = e[1], b = e[2];
        sc->val[0][i] = (uint16_t)(r - pr);
        sc->val[1][i] = (uint16_t)pal_zz((int32_t)g - (int32_t)r);
        sc->val[2][i] = (uint16_t)pal_zz((int32_t)b - (int32_t)g);
        pr = r;
    }
    size_t bits = 12u;
    for (int c = 0; c < 3; c++) {
        uint32_t best = 0xFFFFFFFFu;
        for (uint32_t kk = 0; kk <= 8u; kk++) {
            uint32_t cost = 0;
            for (uint32_t i = 0; i < count; i++) cost += ((uint32_t)sc->val[c][i] >> kk) + 1u + kk;
            if (cost < best) { best = cost; k[c] = kk; }
        }
        bits += best;
    }
    return (bits + 7u) >> 3;
}

static void pal_field_write(const pal_scratch *sc, uint32_t count, const uint32_t k[3], uint8_t *out, size_t len) {
    size_t bit = 0;
    memset(out, 0, len);
    for (int c = 0; c < 3; c++) pal_put(out, &bit, k[c], 4u);
    for (uint32_t i = 0; i < count; i++)
        for (int c = 0; c < 3; c++) pal_put_rice(out, &bit, sc->val[c][i], k[c]);
}

typedef struct { const uint8_t *p; size_t nbits, bit; int bad; } pal_bits;

static uint32_t pal_get(pal_bits *r, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (r->bit >= r->nbits) { r->bad = 1; return 0; }
        v = (v << 1) | ((uint32_t)(r->p[r->bit >> 3] >> (7u - (r->bit & 7u))) & 1u);
        r->bit++;
    }
    return v;
}

static uint32_t pal_get_rice(pal_bits *r, uint32_t k) {
    uint32_t q = 0;
    while (pal_get(r, 1u)) {
        if (++q > 511u) { r->bad = 1; return 0; }
    }
    if (r->bad) return 0;
    return (q << k) | pal_get(r, k);
}

/* Reads the palette field at `in` (`n` bytes available) into pal[3 * count] and
   returns its size in bytes, or 0 when the field is invalid. */
static size_t pal_field_read(const uint8_t *in, size_t n, uint32_t count, uint8_t *pal) {
    pal_bits r = {in, n * 8u, 0, 0};
    uint32_t k[3];
    for (int c = 0; c < 3; c++) {
        k[c] = pal_get(&r, 4u);
        if (k[c] > 8u) return 0;
    }
    uint32_t pr = 0, pkey = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t red = pr + pal_get_rice(&r, k[0]);
        uint32_t ug = pal_get_rice(&r, k[1]);
        uint32_t ub = pal_get_rice(&r, k[2]);
        if (r.bad || red > 255u) return 0;
        int32_t g = (int32_t)red + ((ug & 1u) ? -(int32_t)((ug + 1u) >> 1) : (int32_t)(ug >> 1));
        if (g < 0 || g > 255) return 0;
        int32_t b = g + ((ub & 1u) ? -(int32_t)((ub + 1u) >> 1) : (int32_t)(ub >> 1));
        if (b < 0 || b > 255) return 0;
        uint32_t key = (red << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        if (i > 0 && key <= pkey) return 0;
        pkey = key;
        pr = red;
        uint8_t *e = pal + 3u * (size_t)i;
        e[0] = (uint8_t)red;
        e[1] = (uint8_t)g;
        e[2] = (uint8_t)b;
    }
    while (r.bit & 7u) {
        if (pal_get(&r, 1u) || r.bad) return 0;
    }
    return r.bit >> 3;
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
        if (key == prev_key0) {
            while (i + 9u <= npix && pal_same4(rgb + 3u * i) && pal_same4(rgb + 3u * (i + 4u))) i += 8u;
            while (i + 5u <= npix && pal_same4(rgb + 3u * i)) i += 4u;
            continue;
        }
        if (key == prev_key1) continue;
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
    if (cap < 1u + 2u + 5u) return RF_OK;
    size_t row_bytes = (size_t)w * 3u;
    size_t scratch_bytes = (sizeof(pal_scratch) + 63u) & ~(size_t)63u;
    uint8_t *alloc_buf = rf_alloc(scratch_bytes + row_bytes);
    if (!alloc_buf) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    pal_scratch *sc = (pal_scratch *)alloc_buf;
    uint8_t *rows = alloc_buf + scratch_bytes;
    /* Method 4 stores the palette sorted by R << 16 | G << 8 | B. */
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = pal + 3u * (size_t)i;
        uint32_t key = ((uint32_t)e[0] << 16) | ((uint32_t)e[1] << 8) | (uint32_t)e[2];
        uint32_t j = i;
        while (j > 0) {
            const uint8_t *q = pal + 3u * (size_t)sc->ord[j - 1u];
            if ((((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | (uint32_t)q[2]) < key) break;
            sc->ord[j] = sc->ord[j - 1u];
            j--;
        }
        sc->ord[j] = (uint8_t)i;
    }
    for (uint32_t i = 0; i < count; i++) {
        sc->nidx[sc->ord[i]] = (uint8_t)i;
        memcpy(sc->spal + 3u * (size_t)i, pal + 3u * (size_t)sc->ord[i], 3u);
    }
    memcpy(pal, sc->spal, 3u * (size_t)count);
    const uint8_t *nidx = sc->nidx;
    uint32_t field_k[3];
    size_t field_len = pal_field_plan(sc, pal, count, field_k);
    size_t hdr_len = 1u + field_len;
    if (cap < hdr_len + 5u) {
        rf_free(alloc_buf);
        return RF_OK;
    }
    size_t stream_cap = cap - hdr_len;
    pal_field_write(sc, count, field_k, out + 1, field_len);
    pal_model_init(&sc->m);
    renc enc;
    renc_init(&enc, out + hdr_len, stream_cap);
    uint32_t hit = 0;
    if (count == 1u) {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t d1_ctx = 127u * 2u + hit + pal_class(x, y);
                pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[d1_ctx], &sc->m.d1_n[d1_ctx], 1);
                hit = 1u;
            }
        }
        if (enc.pos > stream_cap) {
            rf_free(alloc_buf);
            return RF_OK;
        }
    } else {
        uint32_t first_key = ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
        uint32_t first_idx = nidx[0];
        uint32_t prev_key0 = first_key, prev_idx0 = first_idx;
        uint32_t prev_key1 = first_key, prev_idx1 = first_idx;
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
                        if (ww == idx && b == c && b == d) {
                            uint32_t run_ctx = pat * 2u + 1u;
                            uint32_t idx4 = idx * 0x01010101u;
                            uint32_t b4 = b * 0x01010101u;
                            uint32_t nn4 = nn * 0x01010101u;
                            while (x + 5u < w && pal_same4(p + 3u * (size_t)x)) {
                                uint32_t pb4, pnn4;
                                __builtin_memcpy(&pb4, prv_row + x + 2u, 4u);
                                __builtin_memcpy(&pnn4, prv2_row + x + 1u, 4u);
                                if (pb4 != b4 || pnn4 != nn4) break;
                                __builtin_memcpy(cur_row + x + 1u, &idx4, 4u);
                                if (sc->m.d1[run_ctx] == 65535u && sc->m.d1_n[run_ctx] == 60u) {
                                    if (enc.range >= (1u << 25)) {
                                        uint64_t rg = enc.range;
                                        rg = (rg * 65535u) >> 16;
                                        rg = (rg * 65535u) >> 16;
                                        rg = (rg * 65535u) >> 16;
                                        enc.range = (uint32_t)((rg * 65535u) >> 16);
                                    } else {
                                        renc_bit16(&enc, 1, 65535u);
                                        renc_bit16(&enc, 1, 65535u);
                                        renc_bit16(&enc, 1, 65535u);
                                        renc_bit16(&enc, 1, 65535u);
                                    }
                                } else {
                                    pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[run_ctx], &sc->m.d1_n[run_ctx], 1);
                                    pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[run_ctx], &sc->m.d1_n[run_ctx], 1);
                                    pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[run_ctx], &sc->m.d1_n[run_ctx], 1);
                                    pal_enc_bit(&enc, sc->m.rate, &sc->m.d1[run_ctx], &sc->m.d1_n[run_ctx], 1);
                                }
                                x += 4u;
                            }
                        }
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
                    idx = first_idx;
                    prev_key1 = prev_key0;
                    prev_idx1 = prev_idx0;
                    prev_key0 = key;
                    prev_idx0 = idx;
                    cur_row[x] = (uint8_t)idx;
                } else {
                    uint32_t slot = (key * 0x9E3779B1u) >> 22;
                    for (;;) {
                        idx = ht_idx[slot];     /* a first-seen index; 0 marks an empty slot */
                        if (idx != 0) {
                            idx = nidx[idx];
                            if (pal[3 * idx + 0] == (uint8_t)r && pal[3 * idx + 1] == (uint8_t)g && pal[3 * idx + 2] == (uint8_t)bl)
                                break;
                        }
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
                uint32_t d1_ctx = pat * 2u + hit + pal_class(x, y);
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
    out[0] = (uint8_t)(count - 1u);
    *written = hdr_len + stream_len;
    rf_free(alloc_buf);
    return RF_OK;
}

int rf_pal_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, uint32_t method, rf_error *err) {
    int m4 = (method == RF_METHOD_PAL3);
    uint8_t pal4[768];
    uint32_t count;
    const uint8_t *pal, *stream;
    uint32_t stream_len;
    if (m4) {
        if (n < 8u) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
        count = (uint32_t)in[0] + 1u;
        size_t field_len = pal_field_read(in + 1, n - 1u, count, pal4);
        if (field_len == 0 || n - 1u - field_len < 5u) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
        pal = pal4;
        stream = in + 1u + field_len;
        stream_len = (uint32_t)(n - 1u - field_len);
    } else {
        if (n < 6u) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
        uint32_t count_m1 = (uint32_t)in[0] | ((uint32_t)in[1] << 8);
        if (count_m1 > 255u) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
        count = count_m1 + 1u;
        size_t pal_bytes = 3u * (size_t)count;
        if (n < 6u + pal_bytes) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
        pal = in + 2;
        const uint8_t *sp = in + 2u + pal_bytes;
        stream = sp + 4;
        stream_len = (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24);
        if (stream_len < 5u || (uint64_t)stream_len != (uint64_t)(n - 6u - pal_bytes))
            return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette");
    }
    size_t scratch_bytes = (sizeof(pal_scratch) + 63u) & ~(size_t)63u;
    uint8_t *alloc_buf = rf_alloc(scratch_bytes + (size_t)w * 3u);
    if (!alloc_buf) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    pal_scratch *sc = (pal_scratch *)alloc_buf;
    uint8_t *rows = alloc_buf + scratch_bytes;
    memset(sc->ht_key, 0xFF, sizeof(sc->ht_key));
    /* Method 4's field is strictly increasing, so its colours are already distinct. */
    for (uint32_t i = 0; i < count && !m4; i++) {
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
    rdec_init(&dec, stream, stream_len);
    uint32_t hit = 0;
    int bad = 0;
    int m3 = (method != RF_METHOD_PAL);      /* methods 3 and 4 share neighbours, pattern and update */
    int mode = m4 ? 2 : m3;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *cur_row = rows + (size_t)(y % 3u) * w;
        const uint8_t *prv_row = rows + (size_t)((y + 2u) % 3u) * w;
        const uint8_t *prv2_row = rows + (size_t)((y + 1u) % 3u) * w;
        uint8_t *dst = stride ? (out + 54u + (size_t)(h - 1u - y) * stride) : (out + (size_t)y * w * 3u);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t a, b, c = 0, d = 0, ww = 0, e = 0;
            uint32_t pat = m3 ? pal_pat_m3(cur_row, prv_row, prv2_row, w, x, y, &a, &b, &c, &d, &ww)
                              : pal_neighbours_m2(cur_row, prv_row, w, x, y, &a, &b, &e);
            uint32_t d1_ctx = pat * 2u + hit + (m4 ? pal_class(x, y) : 0u);
            uint32_t idx;
            if (pal_dec_bit(&dec, sc->m.rate, &sc->m.d1[d1_ctx], &sc->m.d1_n[d1_ctx], mode)) {
                idx = a;
                hit = 1u;
            } else if (m3 && count == 2u) {
                idx = 1u - a;
                hit = 0;
            } else {
                hit = 0;
                if (b != a && pal_dec_bit(&dec, sc->m.rate, &sc->m.d2[pat], &sc->m.d2_n[pat], mode)) {
                    idx = b;
                } else {
                    if (m3) e = pal_third_m3(a, b, c, d, ww);
                    if (e != a && e != b && pal_dec_bit(&dec, sc->m.rate, &sc->m.d3[pat], &sc->m.d3_n[pat], mode)) {
                        idx = e;
                    } else {
                        uint32_t node = 1u;
                        for (int bit_pos = 0; bit_pos < 8; bit_pos++) {
                            int bit = pal_dec_bit(&dec, sc->m.rate, &sc->m.tree[node], &sc->m.tree_n[node], mode);
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
