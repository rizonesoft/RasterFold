/* Method 1, "CM": per-channel weighted prediction, an LMS correction stage,
   five mixed context models, and one binary range coder per channel.
   spec/FOLD.md is the normative description; this file follows it step by step.

   The three channels (G, R-G, B-G) have separate models and separate streams.
   R depends on G only at the same pixel, B on G and R only at the same pixel,
   so the channels can run as a pipeline: one thread each, R a row behind G and
   B a row behind R, or on one thread with G, R and B one pixel apart. Both
   orders produce the same streams. Encoder and decoder share every modelling
   step: cm_model() and cm_code() are instantiated per channel and direction. */
#include "rf.h"
#include "rc.h"
#include "simd.h"

#define NMODEL 7
#define NROWS 2048          /* context rows per model */
#define NODES 32            /* node 0: row visit count; nodes 1..29: decisions */
#define WSEL 128            /* mixer weight sets per channel */
#define WSTRIDE 8           /* int32 slots per weight set and node */
#define LMSN 12
#define XROWS 32            /* rows of G and R residuals kept for later channels */

/* ------------------------------------------------------------------ tables */
static int16_t STRETCH[4096];
static int16_t SQUASH[4095];            /* squash(x) at [x + 2047] */
static uint32_t RATE[61];               /* floor(65536 / (n + 1.5)) */
static uint32_t DIVL[64];
static uint32_t ERRW[2][1024];          /* errw(e, 13) and errw(e, 12) for e < 1024 */
static uint8_t Q9T[2048];
static volatile int tables_ready;

static int squash_i(int d) {
    static const int t[33] = {1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546,
                              2047, 2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068,
                              4079, 4085, 4089, 4092, 4093, 4094};
    if (d > 2047) return 4095;
    if (d < -2047) return 1;
    int w = d & 127, i = (d >> 7) + 16;
    return (t[i] * (128 - w) + t[i + 1] * w + 64) >> 7;
}

static void cm_tables(void) {
    if (__atomic_load_n(&tables_ready, __ATOMIC_ACQUIRE) == 2) return;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&tables_ready, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&tables_ready, __ATOMIC_ACQUIRE) != 2) rf_yield();
        return;
    }
    int pi = 0;
    for (int x = -2047; x <= 2047; x++) {
        int v = squash_i(x);
        SQUASH[x + 2047] = (int16_t)v;
        for (int j = pi; j <= v; j++) STRETCH[j] = (int16_t)x;
        pi = v + 1;
    }
    for (int j = pi; j < 4096; j++) STRETCH[j] = 2047;
    for (int n = 0; n <= 60; n++) RATE[n] = (uint32_t)((65536 * 2) / (2 * n + 3));
    for (int i = 0; i < 64; i++) DIVL[i] = (1u << 24) / (uint32_t)(i + 1);
    for (uint32_t e = 0; e < 1024; e++) {
        int sh = 31 - __builtin_clz(e + 1) - 5;
        sh = sh < 0 ? 0 : sh;
        ERRW[0][e] = 4 + ((13 * DIVL[e >> sh]) >> sh);
        ERRW[1][e] = 4 + ((12 * DIVL[e >> sh]) >> sh);
    }
    for (int v = -1024; v < 1024; v++) {
        int a = v < 0 ? -v : v;
        int q = a == 0 ? 0 : a <= 1 ? 1 : a <= 3 ? 2 : a <= 7 ? 3 : 4;
        Q9T[v + 1024] = (uint8_t)(v < 0 ? 4 - q : 4 + q);
    }
    __atomic_store_n(&tables_ready, 2, __ATOMIC_RELEASE);
}

RF_INLINE int clampq(int v) { return v < -1024 ? -1024 : v > 1023 ? 1023 : v; }
RF_INLINE int q9(int v) { return Q9T[clampq(v) + 1024]; }
RF_INLINE int iabs(int v) { return v < 0 ? -v : v; }
/* k0's scaled 7-level quantizer as a scalar: 3 plus or minus the number of
   thresholds scl, 3*scl, 7*scl that 6*|v| reaches (mirrors the qcount3 call
   that builds k0). */
RF_INLINE int qn7(int v, int scl) {
    const int a = 6 * iabs(v);
    const int n = (a >= scl) + (a >= 3 * scl) + (a >= 7 * scl);
    return v > 0 ? 3 + n : v < 0 ? 3 - n : 3;
}
RF_INLINE int ilog2(uint32_t v) { return 31 - __builtin_clz(v | 1); }
RF_INLINE int qlog(int v) { return v <= 0 ? 0 : (v >= 16384 ? 15 : ilog2((uint32_t)v) + 1); }
RF_INLINE int med3(int w, int n, int nw) {
    int mx = w > n ? w : n, mn = w < n ? w : n;
    return nw >= mx ? mn : nw <= mn ? mx : w + n - nw;
}
/* Per lane: sign(d) * (number of thresholds t_i with |d| >= t_i) + mid. The
   thresholds arrive as t_i - 1 so that a greater-than compare decides >=. */
RF_INLINE __m128i qcount3(__m128i a, __m128i sgn, __m128i t1, __m128i t2, __m128i t3, int mid) {
    __m128i m = _mm_add_epi32(_mm_add_epi32(_mm_cmpgt_epi32(a, t1), _mm_cmpgt_epi32(a, t2)), _mm_cmpgt_epi32(a, t3));
    return _mm_sub_epi32(_mm_set1_epi32(mid), _mm_sign_epi32(m, sgn));
}
RF_INLINE __m128i qcount(__m128i a, __m128i sgn, __m128i t1, __m128i t2, __m128i t3, __m128i t4, int mid) {
    __m128i m = _mm_add_epi32(_mm_add_epi32(_mm_cmpgt_epi32(a, t1), _mm_cmpgt_epi32(a, t2)),
                              _mm_add_epi32(_mm_cmpgt_epi32(a, t3), _mm_cmpgt_epi32(a, t4)));
    return _mm_sub_epi32(_mm_set1_epi32(mid), _mm_sign_epi32(m, sgn));
}
/* Dot product of four small lanes with int16 coefficients (high halves zero). */
RF_INLINE int qdot(__m128i q, __m128i coef) {
    __m128i v = _mm_madd_epi16(q, coef);
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4E));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xB1));
    return _mm_cvtsi128_si32(v);
}

/* ------------------------------------------------------------ model state */
typedef struct {
    int32_t perr[4];    /* weighted predictor, per sub-predictor error */
    int32_t err;        /* weighted predictor error, 1/8 units */
    int32_t er;         /* coded residual, -128..127 */
    int32_t fe;         /* final prediction error, 1/8 units */
    int32_t pad;
} pos_t;

typedef struct {
    int nmodel;         /* 5 for a version 2 stream, 7 for version 3 */
    uint16_t *ctr;      /* nmodel * NROWS * NODES counters */
    int32_t *wt;        /* WSEL * NODES * WSTRIDE mixer weights */
    pos_t *pos;         /* 2 rows of w + 2 */
    int16_t *rows;      /* 3 rows of w plane samples */
    int32_t lms[LMSN] __attribute__((aligned(16)));
    renc enc;
    rdec dec;
    size_t bytes;       /* lower bound on flushed stream size (pos + pending + 4) */
    int bad;            /* decoder saw the impossible residual +128 */
    volatile int32_t done;  /* rows finished, published to the other channels */
    uint8_t pad[64];
} chan_t;

typedef struct {
    uint32_t w, h;
    size_t cap;         /* payload cap for encode early-exit */
    uint8_t *rgb;       /* row 0 pixel base: read by the encoder, written by the decoder */
    ptrdiff_t stride;   /* signed byte step from row y to row y + 1 */
    int r_off;          /* byte offset of R in a 3-byte pixel: 0 for RGB, 2 for BGR */
    int8_t *xe[2];      /* XROWS rows of the G and R residuals */
    int16_t *xf[2];     /* XROWS rows of the G and R final errors, clamped to +-2047 */
    chan_t ch[3];
    void *block;
    volatile int cancel;    /* worker threads exit at once when set before they start */
    volatile int abort;     /* set when a decoded stream runs out of bytes or encode exceeds cap */
} cm_t;

static int cm_alloc(cm_t *m, uint32_t w, uint32_t h, uint8_t *rgb, size_t bmp_stride, int nmodel, rf_error *err) {
    size_t ctr = (size_t)nmodel * NROWS * NODES * 2;
    size_t wt = (size_t)WSEL * NODES * WSTRIDE * 4;
    size_t pos = ((((size_t)w + 2) * 2 * sizeof(pos_t)) + 63) & ~(size_t)63;
    size_t rows = (((size_t)w * 3 * 2) + 63) & ~(size_t)63;
    size_t xe = (((size_t)w * XROWS) + 63) & ~(size_t)63, xf = (((size_t)w * XROWS * 2) + 63) & ~(size_t)63;
    uint8_t *b = rf_alloc((ctr + wt + pos + rows) * 3 + (xe + xf) * 2);
    if (!b) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    m->w = w; m->h = h; m->block = b; m->abort = 0;
    if (bmp_stride) {
        m->rgb = rgb + 54 + bmp_stride * (h - 1);
        m->stride = -(ptrdiff_t)bmp_stride;
        m->r_off = 2;
    } else {
        m->rgb = rgb;
        m->stride = (ptrdiff_t)w * 3;
        m->r_off = 0;
    }
    for (int c = 0; c < 3; c++) {
        chan_t *ch = &m->ch[c];
        ch->nmodel = nmodel;
        ch->ctr = (uint16_t *)b; b += ctr;
        ch->wt = (int32_t *)b; b += wt;
        ch->pos = (pos_t *)b; b += pos;
        ch->rows = (int16_t *)b; b += rows;
        for (size_t i = 0; i < (size_t)nmodel * NROWS * NODES; i++) ch->ctr[i] = (i % NODES) ? 32768 : 0;
        for (size_t i = 0; i < (size_t)WSEL * NODES; i++)
            for (int k = 0; k < WSTRIDE; k++) ch->wt[i * WSTRIDE + k] = k < nmodel ? 19661 : 0;
        for (int k = 0; k < LMSN; k++) ch->lms[k] = 0;
        ch->bytes = 5u;
        ch->bad = 0;
        ch->done = 0;
    }
    for (int c = 0; c < 2; c++) {
        m->xe[c] = (int8_t *)b; b += xe;
        m->xf[c] = (int16_t *)b; b += xf;
    }
    return RF_OK;
}

/* ------------------------------------------------------------ coding */
/* One adaptive decision, version 3 form: mix seven counters. */
RF_INLINE int code_bit7(chan_t *ch, uint16_t *const cp[NMODEL], const uint32_t *rate, int32_t *wrow, int node, int bit, const int enc) {
    uint16_t *c0 = cp[0] + node, *c1 = cp[1] + node, *c2 = cp[2] + node, *c3 = cp[3] + node;
    uint16_t *c4 = cp[4] + node, *c5 = cp[5] + node, *c6 = cp[6] + node;
    int32_t *w = wrow + (size_t)node * WSTRIDE;
    uint32_t v0 = *c0, v1 = *c1, v2 = *c2, v3 = *c3, v4 = *c4, v5 = *c5, v6 = *c6;
    int s0 = STRETCH[v0 >> 4], s1 = STRETCH[v1 >> 4], s2 = STRETCH[v2 >> 4];
    int s3 = STRETCH[v3 >> 4], s4 = STRETCH[v4 >> 4], s5 = STRETCH[v5 >> 4], s6 = STRETCH[v6 >> 4];
    __m128i wv0 = _mm_load_si128((const __m128i *)w);
    __m128i wv1 = _mm_load_si128((const __m128i *)(w + 4));
    __m128i sv = _mm_setr_epi16((int16_t)s0, (int16_t)s1, (int16_t)s2, (int16_t)s3,
                                (int16_t)s4, (int16_t)s5, (int16_t)s6, 64);
    int64_t dot = (int64_t)w[0] * s0 + (int64_t)w[1] * s1 + (int64_t)w[2] * s2 +
                  (int64_t)w[3] * s3 + (int64_t)w[4] * s4 + (int64_t)w[5] * s5 +
                  (int64_t)w[6] * s6 + (int64_t)w[7] * 64;
    int x = (int)(dot >> 16);
    x = x > 2047 ? 2047 : x < -2047 ? -2047 : x;
    uint32_t p = (uint32_t)SQUASH[x + 2047];
    uint32_t rt0 = rate[0], rt1 = rate[1], rt2 = rate[2], rt3 = rate[3];
    uint32_t rt4 = rate[4], rt5 = rate[5], rt6 = rate[6];
    if (enc) renc_bit(&ch->enc, bit, p);
    else bit = rdec_bit(&ch->dec, p);
    int err = (bit << 12) - (int)p;
    __m128i ev = _mm_set1_epi32((int32_t)((uint16_t)err | (4096u << 16)));
    __m128i ones = _mm_set1_epi16(1);
    _mm_store_si128((__m128i *)w, _mm_add_epi32(wv0, _mm_srai_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(sv, ones), ev), 13)));
    _mm_store_si128((__m128i *)(w + 4), _mm_add_epi32(wv1, _mm_srai_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(sv, ones), ev), 13)));
    const int64_t target = bit ? 65535 : 0;
    *c0 = (uint16_t)(v0 + (int32_t)(((target - (int64_t)v0) * (int64_t)rt0) >> 16));
    *c1 = (uint16_t)(v1 + (int32_t)(((target - (int64_t)v1) * (int64_t)rt1) >> 16));
    *c2 = (uint16_t)(v2 + (int32_t)(((target - (int64_t)v2) * (int64_t)rt2) >> 16));
    *c3 = (uint16_t)(v3 + (int32_t)(((target - (int64_t)v3) * (int64_t)rt3) >> 16));
    *c4 = (uint16_t)(v4 + (int32_t)(((target - (int64_t)v4) * (int64_t)rt4) >> 16));
    *c5 = (uint16_t)(v5 + (int32_t)(((target - (int64_t)v5) * (int64_t)rt5) >> 16));
    *c6 = (uint16_t)(v6 + (int32_t)(((target - (int64_t)v6) * (int64_t)rt6) >> 16));
    return bit;
}

/* One adaptive decision, version 2 form: mix five counters. */
RF_INLINE int code_bit5(chan_t *ch, uint16_t *const cp[NMODEL], const uint32_t *rate, int32_t *wrow, int node, int bit, const int enc) {
    uint16_t *c0 = cp[0] + node, *c1 = cp[1] + node, *c2 = cp[2] + node, *c3 = cp[3] + node, *c4 = cp[4] + node;
    int32_t *w = wrow + (size_t)node * WSTRIDE;
    uint32_t v0 = *c0, v1 = *c1, v2 = *c2, v3 = *c3, v4 = *c4;
    int s0 = STRETCH[v0 >> 4], s1 = STRETCH[v1 >> 4], s2 = STRETCH[v2 >> 4];
    int s3 = STRETCH[v3 >> 4], s4 = STRETCH[v4 >> 4];
    __m128i wv0 = _mm_load_si128((const __m128i *)w);
    __m128i wv1 = _mm_load_si128((const __m128i *)(w + 4));
    __m128i sv = _mm_setr_epi16((int16_t)s0, (int16_t)s1, (int16_t)s2, (int16_t)s3, (int16_t)s4, 64, 0, 0);
    int64_t dot = (int64_t)w[0] * s0 + (int64_t)w[1] * s1 + (int64_t)w[2] * s2 +
                  (int64_t)w[3] * s3 + (int64_t)w[4] * s4 + (int64_t)w[5] * 64;
    int x = (int)(dot >> 16);
    x = x > 2047 ? 2047 : x < -2047 ? -2047 : x;
    uint32_t p = (uint32_t)SQUASH[x + 2047];
    uint32_t rt0 = rate[0], rt1 = rate[1], rt2 = rate[2], rt3 = rate[3], rt4 = rate[4];
    if (enc) renc_bit(&ch->enc, bit, p);
    else bit = rdec_bit(&ch->dec, p);
    int err = (bit << 12) - (int)p;
    __m128i ev = _mm_set1_epi32((int32_t)((uint16_t)err | (4096u << 16)));
    __m128i ones = _mm_set1_epi16(1);
    _mm_store_si128((__m128i *)w, _mm_add_epi32(wv0, _mm_srai_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(sv, ones), ev), 13)));
    _mm_store_si128((__m128i *)(w + 4), _mm_add_epi32(wv1, _mm_srai_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(sv, ones), ev), 13)));
    const int64_t target = bit ? 65535 : 0;
    *c0 = (uint16_t)(v0 + (int32_t)(((target - (int64_t)v0) * (int64_t)rt0) >> 16));
    *c1 = (uint16_t)(v1 + (int32_t)(((target - (int64_t)v1) * (int64_t)rt1) >> 16));
    *c2 = (uint16_t)(v2 + (int32_t)(((target - (int64_t)v2) * (int64_t)rt2) >> 16));
    *c3 = (uint16_t)(v3 + (int32_t)(((target - (int64_t)v3) * (int64_t)rt3) >> 16));
    *c4 = (uint16_t)(v4 + (int32_t)(((target - (int64_t)v4) * (int64_t)rt4) >> 16));
    return bit;
}

RF_INLINE int code_bit(chan_t *ch, uint16_t *const cp[NMODEL], const uint32_t *rate, int32_t *wrow, int node, int bit, const int enc) {
    if (ch->nmodel == NMODEL) return code_bit7(ch, cp, rate, wrow, node, bit, enc);
    return code_bit5(ch, cp, rate, wrow, node, bit, enc);
}

RF_INLINE int code_raw(chan_t *ch, int bit, const int enc) {
    if (enc) { renc_bit(&ch->enc, bit, 2048); return bit; }
    return rdec_bit(&ch->dec, 2048);
}

/* Code the residual r (-128..127): zero flag, sign, exponent in unary, then the
   mantissa, two bits adaptive and the rest at probability 1/2. The decoder can
   produce +128 from a malformed stream; the caller rejects it. */
RF_INLINE int code_residual(chan_t *ch, uint16_t *const cp[NMODEL], const uint32_t *rate, int32_t *wrow, int r, const int enc) {
    if (!code_bit(ch, cp, rate, wrow, 1, r != 0, enc)) return 0;
    int neg = code_bit(ch, cp, rate, wrow, 2, r < 0, enc);
    int e = 0, got = 0;
    if (enc) {
        int a = iabs(r);                 /* 1..128 */
        e = ilog2((uint32_t)a);          /* a in [2^e, 2^(e+1)) */
        for (int k = 0; k < e; k++) code_bit(ch, cp, rate, wrow, 3 + k, 1, enc);
        if (e < 7) code_bit(ch, cp, rate, wrow, 3 + e, 0, enc);
        int rem = a - (1 << e);
        for (int b = e - 1; b >= 0; b--) {
            int bit = (rem >> b) & 1;
            if (b == e - 1) code_bit(ch, cp, rate, wrow, 10 + e, bit, enc);
            else if (b == e - 2) code_bit(ch, cp, rate, wrow, 14 + 2 * e + (got & 1), bit, enc);
            else code_raw(ch, bit, enc);
            got = (got << 1) | bit;
        }
        return r;
    }
    while (e < 7 && code_bit(ch, cp, rate, wrow, 3 + e, 0, enc)) e++;
    for (int b = e - 1; b >= 0; b--) {
        int bit;
        if (b == e - 1) bit = code_bit(ch, cp, rate, wrow, 10 + e, 0, enc);
        else if (b == e - 2) bit = code_bit(ch, cp, rate, wrow, 14 + 2 * e + (got & 1), 0, enc);
        else bit = code_raw(ch, 0, enc);
        got = (got << 1) | bit;
    }
    int a = (1 << e) + got;
    return neg ? -a : a;
}

/* ------------------------------------------------------------ one sample */
/* Precomputed row pointers for row y across one or three channels. */
typedef struct {
    uint8_t *rgb_row;
    size_t xs_row;
    int16_t *r0[3], *r1[3], *r2[3];
    pos_t *cur[3], *prv[3];
    int c_off[3];
} row_ctx_t;

RF_INLINE void row_ctx_init_ch(const cm_t *m, uint32_t y, int c, row_ctx_t *rc) {
    const size_t w = m->w;
    const size_t ro0 = (size_t)(y % 3) * w, ro1 = (size_t)((y + 2) % 3) * w, ro2 = (size_t)((y + 1) % 3) * w;
    const size_t cur_off = (y & 1) ? 0 : (w + 2), prv_off = (y & 1) ? (w + 2) : 0;
    rc->rgb_row = m->rgb + (ptrdiff_t)y * m->stride;
    rc->xs_row = (size_t)(y % XROWS) * w;
    rc->c_off[c] = c == 0 ? 1 : c == 1 ? m->r_off : 2 - m->r_off;
    const chan_t *ch = &m->ch[c];
    rc->r0[c] = ch->rows + ro0;
    rc->r1[c] = ch->rows + ro1;
    rc->r2[c] = ch->rows + ro2;
    rc->cur[c] = ch->pos + cur_off;
    rc->prv[c] = ch->pos + prv_off;
}

RF_INLINE void row_ctx_init(const cm_t *m, uint32_t y, row_ctx_t *rc) {
    const size_t w = m->w;
    const size_t ro0 = (size_t)(y % 3) * w, ro1 = (size_t)((y + 2) % 3) * w, ro2 = (size_t)((y + 1) % 3) * w;
    const size_t cur_off = (y & 1) ? 0 : (w + 2), prv_off = (y & 1) ? (w + 2) : 0;
    rc->rgb_row = m->rgb + (ptrdiff_t)y * m->stride;
    rc->xs_row = (size_t)(y % XROWS) * w;
    rc->c_off[0] = 1;
    rc->c_off[1] = m->r_off;
    rc->c_off[2] = 2 - m->r_off;
    for (int c = 0; c < 3; c++) {
        const chan_t *ch = &m->ch[c];
        rc->r0[c] = ch->rows + ro0;
        rc->r1[c] = ch->rows + ro1;
        rc->r2[c] = ch->rows + ro2;
        rc->cur[c] = ch->pos + cur_off;
        rc->prv[c] = ch->pos + prv_off;
    }
}

/* State handed from the modelling phase to the coding phase of one sample. */
typedef struct {
    uint16_t *cp[NMODEL];
    int32_t *wrow;
    int32_t pr[4] __attribute__((aligned(16)));
    int32_t wpred, pred8;
    int predv, base;
    __m128i x0, x1, x2;
} stage_t;

/* Prepare sample (x, y) of channel c: neighbours, prediction, contexts. The
   seven context rows are prefetched here; the coding phase reads them. */
RF_INLINE void cm_model(cm_t *m, const row_ctx_t *rc, const int c, uint32_t x, uint32_t y, const int interior, stage_t *s) {
    chan_t *ch = &m->ch[c];
    const int w = (int)m->w;
    const int xi = (int)x;
    const int16_t *r0 = rc->r0[c], *r1 = rc->r1[c], *r2 = rc->r2[c];
    const pos_t *cur = rc->cur[c], *prv = rc->prv[c];
    int g = 0, eg = 0, feg = 0, er_r = 0, fer = 0;
    const size_t xs = rc->xs_row + x;
    if (c >= 1) {
        g = rc->rgb_row[(size_t)x * 3 + 1];
        eg = m->xe[0][xs];
        feg = m->xf[0][xs];
    }
    if (c == 2) {
        er_r = m->xe[1][xs];
        fer = m->xf[1][xs];
    }

    /* Neighbours, spec section "Neighbours". */
    int W, N, NW, NE, NN, NNE, WW;
    const pos_t *pN = prv + x, *pNE, *pNW, *pW;
    if (interior) {
        W = r0[x - 1]; N = r1[x]; NW = r1[x - 1]; NE = r1[x + 1];
        NN = r2[x]; NNE = r2[x + 1];
        pNE = pN + 1; pNW = pN - 1; pW = cur + x - 1;
    } else {
        if (y >= 2 && x >= 1 && xi + 1 < w) {
            W = r0[x - 1]; N = r1[x]; NW = r1[x - 1]; NE = r1[x + 1];
            NN = r2[x]; NNE = r2[x + 1];
        } else if (y == 0) {
            W = x > 0 ? r0[x - 1] : 0;
            N = NW = NE = NN = NNE = W;
        } else {
            N = r1[x];
            W = x > 0 ? r0[x - 1] : N;
            NW = x > 0 ? r1[x - 1] : N;
            NE = xi + 1 < w ? r1[x + 1] : N;
            NN = y > 1 ? r2[x] : N;
            NNE = (y > 1 && xi + 1 < w) ? r2[x + 1] : NE;
        }
        pNE = xi + 1 < w ? pN + 1 : pN;
        pNW = x > 0 ? pN - 1 : pN;
        pW = x > 0 ? cur + x - 1 : cur;
    }
    WW = x > 1 ? r0[x - 2] : W;

    /* Weighted predictor (the JPEG XL self-correcting predictor). */
    int32_t *pr = s->pr;
    int32_t wpred;
    {
        uint32_t wt[4], ev[4] __attribute__((aligned(16)));
        _mm_store_si128((__m128i *)ev, _mm_add_epi32(_mm_add_epi32(_mm_loadu_si128((const __m128i *)pN->perr),
                                                                   _mm_loadu_si128((const __m128i *)pNE->perr)),
                                                     _mm_loadu_si128((const __m128i *)pNW->perr)));
        if (__builtin_expect((ev[0] | ev[1] | ev[2] | ev[3]) < 1024, 1)) {
            wt[0] = ERRW[0][ev[0]]; wt[1] = ERRW[1][ev[1]]; wt[2] = ERRW[1][ev[2]]; wt[3] = ERRW[1][ev[3]];
        } else {
            static const uint32_t maxw[4] = {13, 12, 12, 12};
            for (int k = 0; k < 4; k++) {
                int sh = ilog2(ev[k] + 1) - 5;
                sh = sh < 0 ? 0 : sh;
                wt[k] = 4 + ((maxw[k] * DIVL[ev[k] >> sh]) >> sh);
            }
        }
        int32_t n8 = N * 8, w8 = W * 8, ne8 = NE * 8;
        int32_t teW = (interior || x > 0) ? pW->err : 0, teN = pN->err, teNW = pNW->err, teNE = pNE->err;
        int32_t sumWN = teN + teW;
        pr[0] = w8 + ne8 - n8;
        pr[1] = n8 - (((sumWN + teNE) * 16) >> 5);
        pr[2] = w8 - (((sumWN + teNW) * 10) >> 5);
        pr[3] = n8 - (((teNW + teN + teNE) * 7) >> 5);
        uint32_t ws = wt[0] + wt[1] + wt[2] + wt[3];
        int lw = ilog2(ws) - 4;
        wt[0] >>= lw; wt[1] >>= lw; wt[2] >>= lw; wt[3] >>= lw;
        ws = wt[0] + wt[1] + wt[2] + wt[3];
        int64_t sum = (int64_t)(ws >> 1) - 1 + (int64_t)pr[0] * wt[0] + (int64_t)pr[1] * wt[1] +
                      (int64_t)pr[2] * wt[2] + (int64_t)pr[3] * wt[3];
        wpred = (int32_t)((sum * DIVL[ws - 1]) >> 24);
        int32_t mx = w8 > ne8 ? w8 : ne8, mn = w8 < ne8 ? w8 : ne8;
        mx = n8 > mx ? n8 : mx; mn = n8 < mn ? n8 : mn;
        int32_t cl = wpred < mn ? mn : wpred > mx ? mx : wpred;
        wpred = (((teN ^ teW) | (teN ^ teNW)) <= 0) ? cl : wpred;
    }

    /* LMS correction from neighbouring errors, the G and R errors at this pixel,
       and five local gradients. Inputs are clamped to +-2047. */
    int32_t lin[LMSN] __attribute__((aligned(16)));
    if (interior) {
        lin[0] = cur[x - 1].fe;
        lin[1] = prv[x].fe;
        lin[2] = prv[x - 1].fe;
        lin[3] = prv[x + 1].fe;
        lin[4] = cur[x - 2].fe;
    } else {
        lin[0] = x > 0 ? cur[x - 1].fe : 0;
        lin[1] = y > 0 ? prv[x].fe : 0;
        lin[2] = (x > 0 && y > 0) ? prv[x - 1].fe : 0;
        lin[3] = (y > 0 && xi + 1 < w) ? prv[x + 1].fe : 0;
        lin[4] = x > 1 ? cur[x - 2].fe : 0;
    }
    lin[5] = feg;
    lin[6] = fer;
    lin[7] = (N - W) * 8;
    lin[8] = (NE - N) * 8;
    lin[9] = (NW - W) * 8;
    lin[10] = (NN - N) * 8;
    lin[11] = (NNE - NE) * 8;
    const __m128i lo = _mm_set1_epi32(-2047), hi = _mm_set1_epi32(2047);
    __m128i x0 = _mm_min_epi32(_mm_max_epi32(_mm_load_si128((const __m128i *)lin), lo), hi);
    __m128i x1 = _mm_min_epi32(_mm_max_epi32(_mm_load_si128((const __m128i *)(lin + 4)), lo), hi);
    __m128i x2 = _mm_min_epi32(_mm_max_epi32(_mm_load_si128((const __m128i *)(lin + 8)), lo), hi);
    __m128i l0 = _mm_load_si128((const __m128i *)ch->lms), l1 = _mm_load_si128((const __m128i *)(ch->lms + 4));
    __m128i l2 = _mm_load_si128((const __m128i *)(ch->lms + 8));
    __m128i sacc = _mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(l0, x0), _mm_mullo_epi32(l1, x1)), _mm_mullo_epi32(l2, x2));
    sacc = _mm_add_epi32(sacc, _mm_shuffle_epi32(sacc, 0x4E));
    sacc = _mm_add_epi32(sacc, _mm_shuffle_epi32(sacc, 0xB1));
    int32_t corr = _mm_cvtsi128_si32(sacc) >> 16;
    corr = corr > 256 ? 256 : corr < -256 ? -256 : corr;
    const int32_t pred8 = wpred + corr;
    const int pred = (pred8 + 4) >> 3;
    const int base = g;
    int predv = pred + base;
    predv = predv < 0 ? 0 : predv > 255 ? 255 : predv;

    /* Contexts. */
    int eW, eN, eNW, eNE;
    if (interior) {
        eW = cur[x - 1].er; eN = prv[x].er; eNW = prv[x - 1].er; eNE = prv[x + 1].er;
    } else {
        eW = x > 0 ? cur[x - 1].er : (y > 0 ? prv[x].er : 0);
        eN = y > 0 ? prv[x].er : eW;
        eNW = (x > 0 && y > 0) ? prv[x - 1].er : eN;
        eNE = (y > 0 && xi + 1 < w) ? prv[x + 1].er : eN;
    }
    const int E = 2 * iabs(eW) + 2 * iabs(eN) + iabs(eNW) + iabs(eNE);
    const int qE = qlog(E);
    const int gq = c ? q9(eg) : 4;
    const int rq = c == 2 ? q9(er_r) : 4;
    const int scl = (E >> 2) + 2;
    const int band = qE < 4 ? 0 : qE < 8 ? 1 : 2;
    const __m128i nb = _mm_setr_epi32(W, N, NE, NW);
    /* k0: W, N, NE and NW against the prediction, 7 levels of the local scale. */
    const __m128i d0 = _mm_sub_epi32(nb, _mm_set1_epi32(pred));
    const __m128i a0 = _mm_abs_epi32(d0);
    const __m128i a6 = _mm_add_epi32(_mm_slli_epi32(a0, 2), _mm_slli_epi32(a0, 1));
    const __m128i q0 = qcount3(a6, d0, _mm_set1_epi32(scl - 1), _mm_set1_epi32(3 * scl - 1), _mm_set1_epi32(7 * scl - 1), 3);
    uint32_t k0 = (uint32_t)(qdot(q0, _mm_setr_epi16(343, 0, 49, 0, 7, 0, 1, 0)) * 3 + band);
    /* k1: local error energy with the G and R residuals at this pixel. */
    uint32_t k1 = (uint32_t)(qE * 81 + gq * 9 + rq);
    /* k2: the JPEG-LS gradients NE-N, N-NW and NW-W, 9 levels. */
    const __m128i d2 = _mm_sub_epi32(_mm_shuffle_epi32(nb, _MM_SHUFFLE(0, 3, 1, 2)), _mm_shuffle_epi32(nb, _MM_SHUFFLE(0, 0, 3, 1)));
    const __m128i q2 = qcount(_mm_abs_epi32(d2), d2, _mm_set1_epi32(0), _mm_set1_epi32(2), _mm_set1_epi32(6), _mm_set1_epi32(20), 4);
    uint32_t k2 = (uint32_t)(qdot(q2, _mm_setr_epi16(81, 0, 9, 0, 1, 0, 0, 0)) * 5 + (gq + 1) / 2);
    /* k3: four simple predictors against the prediction, 7 levels. */
    const __m128i d3 = _mm_sub_epi32(_mm_setr_epi32(med3(W, N, NW), W + N - NW, (W + NE + 1) >> 1, N + NE - NNE), _mm_set1_epi32(pred));
    const __m128i q3 = qcount3(_mm_abs_epi32(d3), d3, _mm_set1_epi32(0), _mm_set1_epi32(2), _mm_set1_epi32(6), 3);
    uint32_t k3 = (uint32_t)qdot(q3, _mm_setr_epi16(343, 0, 49, 0, 7, 0, 1, 0));
    /* k4: W and N against the prediction with the error energy. */
    uint32_t k4 = (uint32_t)((q9(W - pred) * 9 + q9(N - pred)) * 16 + qE);
    /* k5: NE and NW against the prediction with the error energy. */
    uint32_t k5 = (uint32_t)((q9(NE - pred) * 9 + q9(NW - pred)) * 16 + qE);
    /* k6: WW, NN and NNE against the prediction, 7 levels of the local scale. */
    uint32_t k6 = (uint32_t)(((qn7(WW - pred, scl) * 7 + qn7(NN - pred, scl)) * 7 +
                              qn7(NNE - pred, scl)) * 16 + qE);
    const uint32_t kk[NMODEL] = {k0, k1, k2, k3, k4, k5, k6};
    for (int k = 0; k < ch->nmodel; k++) {
        s->cp[k] = ch->ctr + ((size_t)k * NROWS + kk[k] % NROWS) * NODES;
        _mm_prefetch((const char *)s->cp[k], _MM_HINT_T0);
    }
    const int wsel = qE * 8 + (c ? (gq == 4 ? 0 : 1 + (iabs(eg) > 3)) : 3);
    s->wrow = ch->wt + (size_t)wsel * NODES * WSTRIDE;
    s->wpred = wpred;
    s->pred8 = pred8;
    s->predv = predv;
    s->base = base;
    s->x0 = x0; s->x1 = x1; s->x2 = x2;
}

/* Code the sample prepared by cm_model and update the channel state. The
   encoder passes the sample value; the decoder returns it. */
RF_INLINE int cm_code(cm_t *m, const row_ctx_t *rc, const int c, uint32_t x, int value, const stage_t *s, const int enc) {
    chan_t *ch = &m->ch[c];
    int16_t *r0 = rc->r0[c];
    pos_t *cur = rc->cur[c];
    pos_t *prv = rc->prv[c];
    uint16_t *const *cp = s->cp;
    uint32_t rate[NMODEL];
    for (int k = 0; k < ch->nmodel; k++) {
        uint32_t n = cp[k][0];
        rate[k] = RATE[n];
        cp[k][0] = (uint16_t)(n + (n < 60));
    }
    const int predv = s->predv, base = s->base;
    const int32_t pred8 = s->pred8, wpred = s->wpred;
    const int32_t *pr = s->pr;

    int r;
    if (enc) {
        r = (value - predv) & 255;
        r = r >= 128 ? r - 256 : r;
        code_residual(ch, cp, rate, s->wrow, r, enc);
    } else {
        r = code_residual(ch, cp, rate, s->wrow, 0, enc);
        if (__builtin_expect(ch->dec.in >= ch->dec.lim, 0)) rdec_tail(&ch->dec);
        ch->bad |= r == 128;
        value = (predv + r) & 255;
    }

    const int pv = value - base;
    r0[x] = (int16_t)pv;
    const int32_t ferr = pv * 8 - pred8;
    {
        const __m128i sg = _mm_set1_epi32(ferr > 0 ? 10 : ferr < 0 ? -10 : 0);
        const __m128i wlo = _mm_set1_epi32(-65536), whi = _mm_set1_epi32(65536);
        const __m128i l0 = _mm_load_si128((const __m128i *)ch->lms), l1 = _mm_load_si128((const __m128i *)(ch->lms + 4));
        const __m128i l2 = _mm_load_si128((const __m128i *)(ch->lms + 8));
        _mm_store_si128((__m128i *)ch->lms, _mm_min_epi32(_mm_max_epi32(_mm_add_epi32(l0, _mm_sign_epi32(sg, s->x0)), wlo), whi));
        _mm_store_si128((__m128i *)(ch->lms + 4), _mm_min_epi32(_mm_max_epi32(_mm_add_epi32(l1, _mm_sign_epi32(sg, s->x1)), wlo), whi));
        _mm_store_si128((__m128i *)(ch->lms + 8), _mm_min_epi32(_mm_max_epi32(_mm_add_epi32(l2, _mm_sign_epi32(sg, s->x2)), wlo), whi));
    }
    {
        const int32_t v8 = pv * 8;
        pos_t *pc = cur + x;
        pos_t *pa = prv + x + 1;
        pc->err = wpred - v8;
        __m128i ev = _mm_srli_epi32(_mm_add_epi32(_mm_abs_epi32(_mm_sub_epi32(_mm_load_si128((const __m128i *)pr), _mm_set1_epi32(v8))), _mm_set1_epi32(3)), 3);
        _mm_storeu_si128((__m128i *)pc->perr, ev);
        _mm_storeu_si128((__m128i *)pa->perr, _mm_add_epi32(_mm_loadu_si128((const __m128i *)pa->perr), ev));
        pc->er = r;
        pc->fe = ferr;
    }
    if (c < 2) {
        const size_t xs = rc->xs_row + x;
        m->xe[c][xs] = (int8_t)r;
        m->xf[c][xs] = (int16_t)(ferr > 2047 ? 2047 : ferr < -2047 ? -2047 : ferr);
    }
    if (!enc) rc->rgb_row[(size_t)x * 3 + (size_t)rc->c_off[c]] = (uint8_t)value;
    return value;
}

RF_INLINE int channel_value(const row_ctx_t *rc, int c, uint32_t x) {
    return rc->rgb_row[(size_t)x * 3 + (size_t)rc->c_off[c]];
}

/* ------------------------------------------------------------ drivers */
/* One thread: a three-stage pipeline per row, G at x, R at x - 1, B at x - 2.
   The three samples of one step do not depend on each other, so their
   dependency chains and cache misses overlap. */
RF_INLINE void run_pipelined(cm_t *m, const int enc) {
    const uint32_t w = m->w;
    for (uint32_t y = 0; y < m->h; y++) {
        row_ctx_t rc;
        row_ctx_init(m, y, &rc);
        if (y >= 2 && w >= 6) {
            for (uint32_t x = 0; x < 4; x++) {
                stage_t sg, sr, sb;
                const int dg = x < w, dr = x >= 1 && x - 1 < w, db = x >= 2;
                if (dg) cm_model(m, &rc, 0, x, y, 0, &sg);
                if (dr) cm_model(m, &rc, 1, x - 1, y, 0, &sr);
                if (db) cm_model(m, &rc, 2, x - 2, y, 0, &sb);
                if (dg) cm_code(m, &rc, 0, x, enc ? channel_value(&rc, 0, x) : 0, &sg, enc);
                if (dr) cm_code(m, &rc, 1, x - 1, enc ? channel_value(&rc, 1, x - 1) : 0, &sr, enc);
                if (db) cm_code(m, &rc, 2, x - 2, enc ? channel_value(&rc, 2, x - 2) : 0, &sb, enc);
            }
            for (uint32_t x = 4; x < w - 1; x++) {
                stage_t sg, sr, sb;
                cm_model(m, &rc, 0, x, y, 1, &sg);
                cm_model(m, &rc, 1, x - 1, y, 1, &sr);
                cm_model(m, &rc, 2, x - 2, y, 1, &sb);
                cm_code(m, &rc, 0, x, enc ? channel_value(&rc, 0, x) : 0, &sg, enc);
                cm_code(m, &rc, 1, x - 1, enc ? channel_value(&rc, 1, x - 1) : 0, &sr, enc);
                cm_code(m, &rc, 2, x - 2, enc ? channel_value(&rc, 2, x - 2) : 0, &sb, enc);
            }
            for (uint32_t x = w - 1; x < w + 2; x++) {
                stage_t sg, sr, sb;
                const int dg = x < w, dr = x >= 1 && x - 1 < w, db = x >= 2;
                if (dg) cm_model(m, &rc, 0, x, y, 0, &sg);
                if (dr) cm_model(m, &rc, 1, x - 1, y, 0, &sr);
                if (db) cm_model(m, &rc, 2, x - 2, y, 0, &sb);
                if (dg) cm_code(m, &rc, 0, x, enc ? channel_value(&rc, 0, x) : 0, &sg, enc);
                if (dr) cm_code(m, &rc, 1, x - 1, enc ? channel_value(&rc, 1, x - 1) : 0, &sr, enc);
                if (db) cm_code(m, &rc, 2, x - 2, enc ? channel_value(&rc, 2, x - 2) : 0, &sb, enc);
            }
        } else {
            for (uint32_t x = 0; x < w + 2; x++) {
                stage_t sg, sr, sb;
                const int dg = x < w, dr = x >= 1 && x - 1 < w, db = x >= 2;
                if (dg) cm_model(m, &rc, 0, x, y, 0, &sg);
                if (dr) cm_model(m, &rc, 1, x - 1, y, 0, &sr);
                if (db) cm_model(m, &rc, 2, x - 2, y, 0, &sb);
                if (dg) cm_code(m, &rc, 0, x, enc ? channel_value(&rc, 0, x) : 0, &sg, enc);
                if (dr) cm_code(m, &rc, 1, x - 1, enc ? channel_value(&rc, 1, x - 1) : 0, &sr, enc);
                if (db) cm_code(m, &rc, 2, x - 2, enc ? channel_value(&rc, 2, x - 2) : 0, &sb, enc);
            }
        }
        if (!enc) {
            for (int c = 0; c < 3; c++) {
                if (m->ch[c].dec.in >= m->ch[c].dec.lim) {
                    m->ch[c].bad = 1;
                    m->abort = 1;
                }
            }
            if (m->abort) break;
        } else if (m->cap < (size_t)m->w * m->h * 3u - 1u && (y & 31u) == 0u) {
            size_t tot = 24u;
            for (int c = 0; c < 3; c++) tot += m->ch[c].enc.pos + (size_t)m->ch[c].enc.pending;
            if (tot > m->cap) {
                m->abort = 1;
                break;
            }
        }
    }
}
static RF_NOINLINE void run_pipelined_enc(cm_t *m) { run_pipelined(m, 1); }
static RF_NOINLINE void run_pipelined_dec(cm_t *m) { run_pipelined(m, 0); }

/* One thread per channel. Row y of R starts after G finished row y, row y of B
   after G and R finished it. G and R stay less than XROWS rows ahead of B, so
   the residual rows B still needs are never overwritten. */
static void wait_done(volatile int32_t *done, int32_t need) {
    for (int spins = 0; __atomic_load_n(done, __ATOMIC_ACQUIRE) < need; spins++) {
        if (spins < 256) _mm_pause();
        else { rf_yield(); spins = 0; }
    }
}

RF_INLINE void run_channel(cm_t *m, const int c, const int enc) {
    const uint32_t w = m->w;
    const int check_cap = enc && (m->cap < (size_t)w * m->h * 3u - 1u);
    for (uint32_t y = 0; y < m->h; y++) {
        if (c >= 1) wait_done(&m->ch[0].done, (int32_t)y + 1);
        if (c == 2) wait_done(&m->ch[1].done, (int32_t)y + 1);
        if (c < 2 && y >= XROWS) wait_done(&m->ch[2].done, (int32_t)(y - XROWS + 1));
        if (!enc && __atomic_load_n(&m->abort, __ATOMIC_ACQUIRE)) break;
        row_ctx_t rc;
        row_ctx_init_ch(m, y, c, &rc);
        if (y >= 2 && w >= 4) {
            for (uint32_t x = 0; x < 2; x++) {
                stage_t s;
                cm_model(m, &rc, c, x, y, 0, &s);
                cm_code(m, &rc, c, x, enc ? channel_value(&rc, c, x) : 0, &s, enc);
            }
            for (uint32_t x = 2; x < w - 1; x++) {
                stage_t s;
                cm_model(m, &rc, c, x, y, 1, &s);
                cm_code(m, &rc, c, x, enc ? channel_value(&rc, c, x) : 0, &s, enc);
            }
            {
                uint32_t x = w - 1;
                stage_t s;
                cm_model(m, &rc, c, x, y, 0, &s);
                cm_code(m, &rc, c, x, enc ? channel_value(&rc, c, x) : 0, &s, enc);
            }
        } else {
            for (uint32_t x = 0; x < w; x++) {
                stage_t s;
                cm_model(m, &rc, c, x, y, 0, &s);
                cm_code(m, &rc, c, x, enc ? channel_value(&rc, c, x) : 0, &s, enc);
            }
        }
        if (!enc) {
            if (m->ch[c].dec.in >= m->ch[c].dec.lim) {
                m->ch[c].bad = 1;
                __atomic_store_n(&m->abort, 1, __ATOMIC_RELEASE);
            }
            if (__atomic_load_n(&m->abort, __ATOMIC_ACQUIRE)) break;
        } else if (check_cap && (y & 31u) == 0u) {
            __atomic_store_n(&m->ch[c].bytes, m->ch[c].enc.pos + (size_t)m->ch[c].enc.pending + 4u, __ATOMIC_RELAXED);
            size_t tot = 12u + __atomic_load_n(&m->ch[0].bytes, __ATOMIC_RELAXED)
                       + __atomic_load_n(&m->ch[1].bytes, __ATOMIC_RELAXED)
                       + __atomic_load_n(&m->ch[2].bytes, __ATOMIC_RELAXED);
            if (tot > m->cap) {
                __atomic_store_n(&m->abort, 1, __ATOMIC_RELEASE);
                break;
            }
            if (__atomic_load_n(&m->abort, __ATOMIC_ACQUIRE)) break;
        }
        __atomic_store_n(&m->ch[c].done, (int32_t)y + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&m->ch[c].done, (int32_t)m->h, __ATOMIC_RELEASE);
}
static RF_NOINLINE void run_g_enc(void *p) { run_channel(p, 0, 1); }
static RF_NOINLINE void run_r_enc(void *p) { if (!((cm_t *)p)->cancel) run_channel(p, 1, 1); }
static RF_NOINLINE void run_b_enc(void *p) { if (!((cm_t *)p)->cancel) run_channel(p, 2, 1); }
static RF_NOINLINE void run_g_dec(void *p) { run_channel(p, 0, 0); }
static RF_NOINLINE void run_r_dec(void *p) { if (!((cm_t *)p)->cancel) run_channel(p, 1, 0); }
static RF_NOINLINE void run_b_dec(void *p) { if (!((cm_t *)p)->cancel) run_channel(p, 2, 0); }

/* Runs channel 0 here and channels 1 and 2 on their own threads. The threads
   are created suspended and started only when both exist; otherwise they are
   released with cancel set, and the single-thread pipeline codes the image. */
static void run(cm_t *m, int threads, const int enc) {
    if (threads >= 2) {
        m->cancel = 0;
        void *t1 = rf_thread_create(enc ? run_r_enc : run_r_dec, m);
        void *t2 = rf_thread_create(enc ? run_b_enc : run_b_dec, m);
        if (t1 && t2) {
            rf_thread_resume(t1);
            rf_thread_resume(t2);
            if (enc) run_g_enc(m); else run_g_dec(m);
            rf_thread_join(t1);
            rf_thread_join(t2);
            return;
        }
        m->cancel = 1;
        if (t1) { rf_thread_resume(t1); rf_thread_join(t1); }
        if (t2) { rf_thread_resume(t2); rf_thread_join(t2); }
    }
    if (enc) run_pipelined_enc(m); else run_pipelined_dec(m);
}

/* ------------------------------------------------------------ interface */
/* Payload: u32 size of each channel stream (G, R, B), then the three streams.
   out must have at least 12 + 3 * (w * h + 64) writable bytes; cap bounds the
   final payload size. */
int rf_cm_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap, size_t *written, int threads, rf_error *err) {
    cm_tables();
    *written = 0;
    if (cap < 27u) return RF_OK;
    cm_t m;
    if (cm_alloc(&m, w, h, (uint8_t *)rgb, 0, NMODEL, err) != RF_OK) return RF_ERR_LIMIT;
    m.cap = cap;
    size_t each = (size_t)w * h + 64;
    for (int c = 0; c < 3; c++) renc_init(&m.ch[c].enc, out + 12 + each * c, each);
    run(&m, threads, 1);
    size_t total = 12;
    for (int c = 0; c < 3; c++) {
        renc_flush(&m.ch[c].enc);
        total += m.ch[c].enc.pos;
    }
    int fits = !m.abort && total <= cap;
    for (int c = 0; c < 3; c++) fits &= m.ch[c].enc.pos <= each;
    if (fits) {
        uint8_t *o = out + 12;
        for (int c = 0; c < 3; c++) {
            uint32_t n = (uint32_t)m.ch[c].enc.pos;
            out[c * 4] = (uint8_t)n; out[c * 4 + 1] = (uint8_t)(n >> 8);
            out[c * 4 + 2] = (uint8_t)(n >> 16); out[c * 4 + 3] = (uint8_t)(n >> 24);
            if (c > 0) memmove(o, out + 12 + each * c, n);
            o += n;
        }
        *written = total;
    }
    rf_free(m.block);
    return RF_OK;
}

int rf_cm_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, int threads, int version, rf_error *err) {
    cm_tables();
    if (n < 12) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm streams");
    uint64_t sz[3], sum = 12;
    for (int c = 0; c < 3; c++) {
        sz[c] = (uint64_t)in[c * 4] | ((uint64_t)in[c * 4 + 1] << 8) | ((uint64_t)in[c * 4 + 2] << 16) | ((uint64_t)in[c * 4 + 3] << 24);
        if (sz[c] < 5) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm streams");
        sum += sz[c];
    }
    if (sum != n) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm streams");
    cm_t m;
    if (cm_alloc(&m, w, h, out, stride, version == 3 ? NMODEL : 5, err) != RF_OK) return RF_ERR_LIMIT;
    const uint8_t *p = in + 12;
    for (int c = 0; c < 3; c++) {
        rdec_init(&m.ch[c].dec, p, (size_t)sz[c]);
        p += sz[c];
    }
    run(&m, threads, 0);
    /* Every stream must end exactly on its last byte. A residual of +128 has no
       encoder, so a stream that decodes one is malformed too. */
    int bad = 0;
    for (int c = 0; c < 3; c++) bad |= m.ch[c].bad | (m.ch[c].dec.in != m.ch[c].dec.end);
    rf_free(m.block);
    return bad ? rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm stream") : RF_OK;
}
