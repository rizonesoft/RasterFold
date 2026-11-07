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
#define NODES 16            /* v3 node 0: row visit count; nodes 1..15: decisions */
#define NODES_V2 32         /* v2 node 0: row visit count; nodes 1..29: decisions */
#define WSEL 128            /* mixer weight sets per channel */
#define WSTRIDE 8           /* int32 slots per weight set and node */
#define LMSN 12
#define XROWS 4             /* rows of G and R residuals kept for later channels */

/* ------------------------------------------------------------------ tables */
static int16_t STRETCH[4096];
static int16_t SQUASH[4095];            /* squash(x) at [x + 2047] */
static uint32_t RATE[73];               /* v3: floor(65536 * 2 / (2 * n + 7)) */
static uint32_t RATE_V2[61];            /* v2: floor(65536 * 2 / (2 * n + 3)) */
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
    for (int n = 0; n <= 72; n++) RATE[n] = (uint32_t)((65536 * 2) / (2 * n + 7));
    for (int n = 0; n <= 60; n++) RATE_V2[n] = (uint32_t)((65536 * 2) / (2 * n + 3));
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
    uint16_t perr[4];   /* weighted predictor, per sub-predictor error */
    int16_t err;        /* weighted predictor error, 1/8 units */
    int16_t fe;         /* final prediction error, 1/8 units */
    int16_t er;         /* coded residual, -128..127 */
    uint16_t ex;        /* version 3: the gated gradient was defined here and equalled the sample */
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
    uint16_t eg, et;    /* version 3: gated-gradient and default prediction error sums */
    uint16_t *mbase[NMODEL];
    size_t ctr_rows;
    size_t wsel_cnt;
    volatile int32_t done;  /* rows finished, published to the other channels */
    volatile int32_t warm_ready;
    void *th_handle;
    uint8_t pad[32];
} chan_t;

typedef struct cm_s {
    uint32_t w, h;
    size_t cap;         /* payload cap for encode early-exit */
    uint8_t *rgb;       /* row 0 pixel base: read by the encoder, written by the decoder */
    ptrdiff_t stride;   /* signed byte step from row y to row y + 1 */
    int r_off;          /* byte offset of R in a 3-byte pixel: 0 for RGB, 2 for BGR */
    int8_t *xe[2];      /* XROWS rows of the G and R residuals */
    int16_t *xf[2];     /* XROWS rows of the G and R final errors, clamped to +-2047 */
    chan_t ch[3];
    void *block;
    struct cm_s *next_strip;
    struct cm_s *chain_next;
    uint32_t warm_rows;
    int is_first_strip;
    int check_cap;
    volatile int cancel;    /* worker threads exit at once when set before they start */
    volatile int abort;     /* set when a decoded stream runs out of bytes or encode exceeds cap */
} cm_t;

#ifndef STRIP_MODE
#define STRIP_MODE 23
#endif
#ifndef WARM_ROWS
#define WARM_ROWS 96
#endif
#ifndef CAP_VISIT
#define CAP_VISIT 24
#endif

static const uint16_t MROWS_C[3][NMODEL] = {
    {2048,   16,  729, 2048, 1296, 1296, 2048},
    {2048,  144, 2048, 2048, 1296, 1296, 2048},
    {2048, 1296, 2048, 2048, 1296, 1296, 2048},
};
static const uint16_t WSEL_C[3] = {32, 96, 96};

static void chan_init_cold(chan_t *ch) {
    const int nmodel = ch->nmodel;
    if (nmodel != NMODEL) {
        for (size_t i = 0; i < (size_t)nmodel * NROWS * NODES_V2; i++) ch->ctr[i] = (i % NODES_V2) ? 32768 : 0;
        for (size_t i = 0; i < (size_t)WSEL * NODES_V2; i++)
            for (int k = 0; k < WSTRIDE; k++) ch->wt[i * WSTRIDE + k] = k < nmodel ? 19661 : 0;
        return;
    }
    const __m128i c0_v = _mm_setr_epi16(0, -32768, -32768, 30000, 28000, 24000, 18000, 12000);
    const __m128i c1_v = _mm_setr_epi16(6000, 29000, 29000, 29000, 29000, 30000, 30000, 30000);
    for (size_t r = 0; r < ch->ctr_rows; r++) {
        _mm_store_si128((__m128i *)(ch->ctr + r * NODES), c0_v);
        _mm_store_si128((__m128i *)(ch->ctr + r * NODES + 8), c1_v);
    }
    const __m128i w0_v = _mm_set1_epi32(19661);
    const __m128i w1_v = _mm_setr_epi32(19661, 19661, 19661, 0);
    for (size_t i = 0; i < ch->wsel_cnt * NODES; i++) {
        _mm_store_si128((__m128i *)(ch->wt + i * WSTRIDE), w0_v);
        _mm_store_si128((__m128i *)(ch->wt + i * WSTRIDE + 4), w1_v);
    }
}

static void chan_copy_warm(chan_t *dst, const chan_t *src) {
    const __m128i cap_v = _mm_setr_epi16(CAP_VISIT, -1, -1, -1, -1, -1, -1, -1);
    const size_t nrows = src->ctr_rows;
    for (size_t r = 0; r < nrows; r++) {
        __m128i v0 = _mm_load_si128((const __m128i *)(src->ctr + r * NODES));
        __m128i v1 = _mm_load_si128((const __m128i *)(src->ctr + r * NODES + 8));
        _mm_store_si128((__m128i *)(dst->ctr + r * NODES), _mm_min_epu16(v0, cap_v));
        _mm_store_si128((__m128i *)(dst->ctr + r * NODES + 8), v1);
    }
    memcpy(dst->wt, src->wt, src->wsel_cnt * NODES * WSTRIDE * sizeof(int32_t));
    for (int k = 0; k < LMSN; k++) dst->lms[k] = src->lms[k];
    dst->eg = src->eg;
    dst->et = src->et;
}

static void strip_bounds(uint32_t h, int S, int s, uint32_t *y0_out, uint32_t *y1_out);

static cm_t *cm_alloc_strips(int S, uint32_t w, uint32_t h, uint8_t *rgb, size_t bmp_stride, int nmodel, rf_error *err) {
    size_t ctr_sz[3], wt_sz[3], total_ch = 0;
    size_t pos = ((((size_t)w + 2) * 2 * sizeof(pos_t)) + 63) & ~(size_t)63;
    size_t rows = (((size_t)w * 3 * 2) + 63) & ~(size_t)63;
    const size_t nodes = (nmodel == NMODEL) ? NODES : NODES_V2;
    for (int c = 0; c < 3; c++) {
        size_t rsum = 0;
        for (int k = 0; k < nmodel; k++) rsum += (nmodel == NMODEL) ? MROWS_C[c][k] : NROWS;
        size_t wcnt = (nmodel == NMODEL) ? WSEL_C[c] : WSEL;
        ctr_sz[c] = ((rsum * nodes * 2) + 63) & ~(size_t)63;
        wt_sz[c] = ((wcnt * nodes * WSTRIDE * 4) + 63) & ~(size_t)63;
        total_ch += ctr_sz[c] + wt_sz[c] + pos + rows;
    }
    size_t xe = (((size_t)w * XROWS) + 63) & ~(size_t)63, xf = (((size_t)w * XROWS * 2) + 63) & ~(size_t)63;
    size_t strip_sz = total_ch + (xe + xf) * 2;
    size_t hdr_sz = ((sizeof(cm_t) * (size_t)S) + 63) & ~(size_t)63;
    uint8_t *raw = rf_alloc(hdr_sz + strip_sz * (size_t)S);
    if (!raw) {
        rf_fail(err, RF_ERR_LIMIT, "out of memory");
        return 0;
    }
    cm_t *m = (cm_t *)raw;
    memset(m, 0, sizeof(cm_t) * (size_t)S);
    uint8_t *b = raw + hdr_sz;
    uint32_t wr = (S == 2 ? 68u : (h / 6u > 96u ? h / 6u : 96u));
    for (int s = 0; s < S; s++) {
        uint32_t y0, y1;
        strip_bounds(h, S, s, &y0, &y1);
        uint32_t sh = y1 - y0;
        cm_t *ms = &m[s];
        ms->w = w; ms->h = sh; ms->block = raw; ms->abort = 0;
        ms->next_strip = (s + 1 < S) ? &m[s + 1] : 0;
        ms->warm_rows = (s + 1 < S) ? (wr < sh ? wr : sh) : 0;
        ms->is_first_strip = (s == 0);
        if (bmp_stride) {
            uint8_t *s_out = rgb + bmp_stride * (h - y1);
            ms->rgb = s_out + 54 + bmp_stride * (sh - 1);
            ms->stride = -(ptrdiff_t)bmp_stride;
            ms->r_off = 2;
        } else {
            ms->rgb = rgb + (size_t)y0 * w * 3u;
            ms->stride = (ptrdiff_t)w * 3;
            ms->r_off = 0;
        }
        for (int c = 0; c < 3; c++) {
            chan_t *ch = &ms->ch[c];
            size_t rsum = 0;
            for (int k = 0; k < nmodel; k++) rsum += (nmodel == NMODEL) ? MROWS_C[c][k] : NROWS;
            ch->ctr_rows = rsum;
            ch->wsel_cnt = (nmodel == NMODEL) ? WSEL_C[c] : WSEL;
            ch->nmodel = nmodel;
            ch->ctr = (uint16_t *)b; b += ctr_sz[c];
            ch->wt = (int32_t *)b; b += wt_sz[c];
            ch->pos = (pos_t *)b; b += pos;
            ch->rows = (int16_t *)b; b += rows;
            uint16_t *mp = ch->ctr;
            for (int k = 0; k < nmodel; k++) {
                ch->mbase[k] = mp;
                mp += (size_t)((nmodel == NMODEL) ? MROWS_C[c][k] : NROWS) * nodes;
            }
            for (int k = 0; k < LMSN; k++) ch->lms[k] = (nmodel == NMODEL && c == 2 && k == 6) ? 20000 : 0;
            ch->bytes = 5u;
            ch->bad = 0;
            ch->done = 0;
            ch->warm_ready = (s == 0) ? 1 : 0;
            ch->eg = ch->et = 0;
        }
        for (int c = 0; c < 2; c++) {
            ms->xe[c] = (int8_t *)b; b += xe;
            ms->xf[c] = (int16_t *)b; b += xf;
        }
    }
    return m;
}

/* ------------------------------------------------------------ coding */
/* One adaptive decision, version 3 form: mix seven counters. */
RF_INLINE int code_bit7(chan_t *ch, uint16_t *const cp[NMODEL], const uint16_t *rate, int32_t *wrow, int node, int bit, const int enc) {
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
    __m128i cv = _mm_setr_epi16((int16_t)v0, (int16_t)v1, (int16_t)v2, (int16_t)v3,
                                (int16_t)v4, (int16_t)v5, (int16_t)v6, 0);
    int64_t dot = (int64_t)w[0] * s0 + (int64_t)w[1] * s1 + (int64_t)w[2] * s2 +
                  (int64_t)w[3] * s3 + (int64_t)w[4] * s4 + (int64_t)w[5] * s5 +
                  (int64_t)w[6] * s6 + (int64_t)w[7] * 64;
    int64_t dx = dot >> 16;
    int x = (int)(dx > 2047 ? 2047 : dx < -2047 ? -2047 : dx);
    uint32_t p = (uint32_t)SQUASH[x + 2047];
    if (enc) renc_bit(&ch->enc, bit, p);
    else bit = rdec_bit(&ch->dec, p);
    int err = (bit << 12) - (int)p;
    __m128i ev = _mm_set1_epi32((int32_t)((uint16_t)err | (4096u << 16)));
    __m128i ones = _mm_set1_epi16(1);
    _mm_store_si128((__m128i *)w, _mm_add_epi32(wv0, _mm_srai_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(sv, ones), ev), 13)));
    _mm_store_si128((__m128i *)(w + 4), _mm_add_epi32(wv1, _mm_srai_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(sv, ones), ev), 13)));
    __m128i rv = _mm_load_si128((const __m128i *)rate);
    __m128i mask = _mm_set1_epi16((int16_t)-bit);
    __m128i not_mask = _mm_set1_epi16((int16_t)(bit - 1));
    __m128i xv = _mm_xor_si128(cv, mask);
    __m128i hi = _mm_mulhi_epu16(xv, rv);
    __m128i adj = _mm_and_si128(_mm_cmpeq_epi16(_mm_mullo_epi16(xv, rv), _mm_setzero_si128()), not_mask);
    __m128i uv16 = _mm_sub_epi16(_mm_add_epi16(cv, _mm_xor_si128(hi, not_mask)), adj);
    *c0 = (uint16_t)_mm_extract_epi16(uv16, 0);
    *c1 = (uint16_t)_mm_extract_epi16(uv16, 1);
    *c2 = (uint16_t)_mm_extract_epi16(uv16, 2);
    *c3 = (uint16_t)_mm_extract_epi16(uv16, 3);
    *c4 = (uint16_t)_mm_extract_epi16(uv16, 4);
    *c5 = (uint16_t)_mm_extract_epi16(uv16, 5);
    *c6 = (uint16_t)_mm_extract_epi16(uv16, 6);
    return bit;
}

/* One adaptive decision, version 2 form: mix five counters. */
RF_INLINE int code_bit5(chan_t *ch, uint16_t *const cp[NMODEL], const uint16_t *rate, int32_t *wrow, int node, int bit, const int enc) {
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
    int64_t dx = dot >> 16;
    int x = (int)(dx > 2047 ? 2047 : dx < -2047 ? -2047 : dx);
    uint32_t p = (uint32_t)SQUASH[x + 2047];
    if (enc) renc_bit(&ch->enc, bit, p);
    else bit = rdec_bit(&ch->dec, p);
    int err = (bit << 12) - (int)p;
    __m128i ev = _mm_set1_epi32((int32_t)((uint16_t)err | (4096u << 16)));
    __m128i ones = _mm_set1_epi16(1);
    _mm_store_si128((__m128i *)w, _mm_add_epi32(wv0, _mm_srai_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(sv, ones), ev), 13)));
    _mm_store_si128((__m128i *)(w + 4), _mm_add_epi32(wv1, _mm_srai_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(sv, ones), ev), 13)));
    const int64_t target = bit ? 65535 : 0;
    *c0 = (uint16_t)(v0 + (int32_t)(((target - (int64_t)v0) * (int64_t)rate[0]) >> 16));
    *c1 = (uint16_t)(v1 + (int32_t)(((target - (int64_t)v1) * (int64_t)rate[1]) >> 16));
    *c2 = (uint16_t)(v2 + (int32_t)(((target - (int64_t)v2) * (int64_t)rate[2]) >> 16));
    *c3 = (uint16_t)(v3 + (int32_t)(((target - (int64_t)v3) * (int64_t)rate[3]) >> 16));
    *c4 = (uint16_t)(v4 + (int32_t)(((target - (int64_t)v4) * (int64_t)rate[4]) >> 16));
    return bit;
}

RF_INLINE int code_bit(chan_t *ch, uint16_t *const cp[NMODEL], const uint16_t *rate, int32_t *wrow, int node, int bit, const int enc, const int nmodel) {
    if (nmodel == NMODEL) return code_bit7(ch, cp, rate, wrow, node, bit, enc);
    return code_bit5(ch, cp, rate, wrow, node, bit, enc);
}

RF_INLINE int code_raw(chan_t *ch, int bit, const int enc) {
    if (enc) { renc_bit(&ch->enc, bit, 2048); return bit; }
    return rdec_bit(&ch->dec, 2048);
}

/* Code the residual r (-128..127): zero flag, sign, exponent in unary, then the
   mantissa, two bits adaptive and the rest at probability 1/2. The decoder can
   produce +128 from a malformed stream; the caller rejects it. */
RF_INLINE int code_residual(chan_t *ch, uint16_t *const cp[NMODEL], const uint16_t *rate, int32_t *wrow, int r, const int enc, const int nmodel) {
    if (!code_bit(ch, cp, rate, wrow, 1, r != 0, enc, nmodel)) return 0;
    int neg = code_bit(ch, cp, rate, wrow, 2, r < 0, enc, nmodel);
    int e = 0, got = 0;
    if (enc) {
        int a = iabs(r);                 /* 1..128 */
        e = ilog2((uint32_t)a);          /* a in [2^e, 2^(e+1)) */
        #pragma clang loop unroll(disable)
        for (int k = 0; k < e; k++) code_bit(ch, cp, rate, wrow, 3 + (k < 5 ? k : 5), 1, enc, nmodel);
        if (e < 7) code_bit(ch, cp, rate, wrow, 3 + (e < 5 ? e : 5), 0, enc, nmodel);
        int rem = a - (1 << e);
        if (e >= 1) {
            int b0 = (rem >> (e - 1)) & 1;
            code_bit(ch, cp, rate, wrow, 8 + (e < 4 ? e : 4), b0, enc, nmodel);
            if (e >= 2) {
                int b1 = (rem >> (e - 2)) & 1;
                code_bit(ch, cp, rate, wrow, e == 2 ? 13 + b0 : 15, b1, enc, nmodel);
                for (int b = e - 3; b >= 0; b--) code_raw(ch, (rem >> b) & 1, enc);
            }
        }
        return r;
    }
    #pragma clang loop unroll(disable)
    while (e < 7 && code_bit(ch, cp, rate, wrow, (nmodel == NMODEL) ? (3 + (e < 5 ? e : 5)) : (3 + e), 0, enc, nmodel)) e++;
    if (e >= 1) {
        got = code_bit(ch, cp, rate, wrow, (nmodel == NMODEL) ? (8 + (e < 4 ? e : 4)) : (10 + e), 0, enc, nmodel);
        if (e >= 2) {
            got = (got << 1) | code_bit(ch, cp, rate, wrow, (nmodel == NMODEL) ? (e == 2 ? 13 + got : 15) : (14 + 2 * e + got), 0, enc, nmodel);
            for (int b = e - 3; b >= 0; b--) got = (got << 1) | code_raw(ch, 0, enc);
        }
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
    int gp, gpdef, pt, gate;
    __m128i x0, x1, x2;
} stage_t;

/* Distance of a sample from a prediction modulo 256, capped at 32. */
static inline int gg_dist(int v, int p) {
    int d = (v - p) & 255;
    d = d < 256 - d ? d : 256 - d;
    return d < 32 ? d : 32;
}

/* Prepare sample (x, y) of channel c: neighbours, prediction, contexts. The
   seven context rows are prefetched here; the coding phase reads them. */
RF_INLINE void cm_model(cm_t *m, const row_ctx_t *rc, const int c, uint32_t x, uint32_t y, const int interior, stage_t *s, const int nmodel) {
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
    int W, N, NW, NE, NN, NNE, WW = 0;
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
    if (nmodel == NMODEL) WW = (interior || x > 1) ? r0[x - 2] : W;

    /* Weighted predictor (the JPEG XL self-correcting predictor). */
    int32_t *pr = s->pr;
    int32_t wpred;
    {
        uint32_t wt[4], ev[4] __attribute__((aligned(16)));
        __m128i eN16 = _mm_loadl_epi64((const __m128i *)pN->perr);
        __m128i eNE16 = _mm_loadl_epi64((const __m128i *)pNE->perr);
        __m128i eNW16 = _mm_loadl_epi64((const __m128i *)pNW->perr);
        _mm_store_si128((__m128i *)ev, _mm_cvtepu16_epi32(_mm_adds_epu16(_mm_adds_epu16(eN16, eNE16), eNW16)));
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
        if (nmodel == NMODEL) {
            pr[0] = w8 + (((ne8 - n8) * 3) >> 3);
            pr[1] = n8 - (((sumWN + teNE) * 12) >> 5);
            pr[2] = w8 - (((sumWN + teNW) * 8) >> 5);
            pr[3] = n8 - (((teNW + teN + teNE) * 5) >> 5);
        } else {
            pr[0] = w8 + ne8 - n8;
            pr[1] = n8 - (((sumWN + teNE) * 16) >> 5);
            pr[2] = w8 - (((sumWN + teNW) * 10) >> 5);
            pr[3] = n8 - (((teNW + teN + teNE) * 7) >> 5);
        }
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
    const int scl = (E >> 2) + ((nmodel == NMODEL) ? 3 : 2);
    const int band = qE < 4 ? 0 : qE < 8 ? 1 : 2;
    const __m128i pv = _mm_set1_epi32(pred);
    const __m128i t1 = _mm_set1_epi32(scl - 1), t2 = _mm_set1_epi32(3 * scl - 1), t3 = _mm_set1_epi32(7 * scl - 1);
    const __m128i nb = _mm_setr_epi32(W, N, NE, NW);
    /* k0: W, N, NE and NW against the prediction, 7 levels of the local scale. */
    const __m128i d0 = _mm_sub_epi32(nb, pv);
    const __m128i a0 = _mm_abs_epi32(d0);
    const __m128i a6 = _mm_add_epi32(_mm_slli_epi32(a0, 2), _mm_slli_epi32(a0, 1));
    const __m128i q0 = qcount3(a6, d0, t1, t2, t3, 3);
    uint32_t k0 = (uint32_t)(qdot(q0, (nmodel == NMODEL) ? _mm_setr_epi16(195, 0, 31, 0, 5, 0, 1, 0) : _mm_setr_epi16(343, 0, 49, 0, 7, 0, 1, 0)) * 3 + band);
    /* k1: local error energy with the G and R residuals at this pixel. */
    uint32_t k1 = (uint32_t)((nmodel == NMODEL) ? (c == 0 ? qE : c == 1 ? qE * 9 + gq : qE * 81 + gq * 9 + rq) : (qE * 81 + gq * 9 + rq));
    /* k2: the JPEG-LS gradients NE-N, N-NW and NW-W, 9 levels. */
    const __m128i d2 = _mm_sub_epi32(_mm_shuffle_epi32(nb, _MM_SHUFFLE(0, 3, 1, 2)), _mm_shuffle_epi32(nb, _MM_SHUFFLE(0, 0, 3, 1)));
    const __m128i q2 = qcount(_mm_abs_epi32(d2), d2, _mm_set1_epi32(0), _mm_set1_epi32(2), _mm_set1_epi32(6), _mm_set1_epi32(20), 4);
    uint32_t q2d = (uint32_t)qdot(q2, _mm_setr_epi16(81, 0, 9, 0, 1, 0, 0, 0));
    const int cq = (c == 2 ? rq : gq);
    uint32_t k2 = (nmodel == NMODEL) ? (c == 0 ? q2d : ((q2d * 3u + (uint32_t)(cq < 3 ? 0 : cq > 5 ? 2 : 1)) & 2047u)) : ((q2d * 5u + (uint32_t)((gq + 1) / 2)) & 2047u);
    /* k3: four simple predictors against the prediction, 7 levels. */
    const __m128i d3 = _mm_sub_epi32(_mm_setr_epi32(med3(W, N, NW), W + N - NW, (W + NE + 1) >> 1, N + NE - NNE), pv);
    const __m128i q3 = qcount3(_mm_abs_epi32(d3), d3, _mm_set1_epi32(0), _mm_set1_epi32(2), _mm_set1_epi32(6), 3);
    uint32_t k3 = (uint32_t)qdot(q3, (nmodel == NMODEL) ? _mm_setr_epi16(289, 0, 43, 0, 7, 0, 1, 0) : _mm_setr_epi16(343, 0, 49, 0, 7, 0, 1, 0)) & 2047u;
    /* k4: W and N against the prediction with the error energy. */
    const int qe_c = (nmodel == NMODEL && c >= 1) ? (((qE >> 1) * 2) + (cq != 4)) : qE;
    uint32_t k4 = (uint32_t)((q9(W - pred) * 9 + q9(N - pred)) * 16 + qe_c);
    uint32_t k5 = 0, k6 = 0;
    if (nmodel == NMODEL) {
        /* k5: NE and NW against the prediction with the error energy. */
        k5 = (uint32_t)((q9(NE - pred) * 9 + q9(NW - pred)) * 16 + qe_c);
        /* k6: WW, NN and NNE against the prediction, 7 levels of the local scale. */
        const __m128i d6 = _mm_sub_epi32(_mm_setr_epi32(WW, NN, NNE, 0), pv);
        const __m128i a6_k = _mm_abs_epi32(d6);
        const __m128i q6 = qcount3(_mm_add_epi32(_mm_slli_epi32(a6_k, 2), _mm_slli_epi32(a6_k, 1)), d6, t1, t2, t3, 3);
        k6 = ((uint32_t)qdot(q6, _mm_setr_epi16(49, 0, 7, 0, 1, 0, 0, 0)) * 6u + (uint32_t)((qE * 3) >> 3)) & 2047u;
    }
    const uint32_t kk[NMODEL] = {k0 & 2047u, k1, k2, k3, k4, k5, k6};
    const size_t nodes = (nmodel == NMODEL) ? NODES : NODES_V2;
    for (int k = 0; k < nmodel; k++) {
        s->cp[k] = ch->mbase[k] + (size_t)kk[k] * nodes;
        _mm_prefetch((const char *)s->cp[k], _MM_HINT_T0);
    }
    /* Version 3 gated gradient: where the gradient of the raw channel values
       was exact at every neighbour it reads, and it has been more than twice
       as accurate as the default prediction on this channel, code against it. */
    const int pt = predv;
    int gate = 0, gp = 0, gpdef = 0, use = 0;
    if (nmodel == NMODEL) {
        const ptrdiff_t st = m->stride;
        const uint8_t *v0 = rc->rgb_row + rc->c_off[c];
        if (x > 0 && y > 0) {
            const uint8_t *v1 = v0 - st;
            gp = (v0[(size_t)(x - 1) * 3] + v1[(size_t)x * 3] - v1[(size_t)(x - 1) * 3]) & 255;
            gpdef = 1;
            gate = cur[x - 1].ex && prv[x].ex && prv[x - 1].ex && (xi + 1 == w || prv[x + 1].ex);
        } else if (x == 0 && y >= 2) {
            gp = (2 * v0[-st] - v0[-2 * st]) & 255;
            gpdef = 1;
            /* cur and prv alternate rows, so cur[0] still holds row y - 2. */
            gate = prv[0].ex && (w == 1 || prv[1].ex) && cur[0].ex;
        } else if (y == 0 && x >= 2) {
            gp = (2 * v0[(size_t)(x - 1) * 3] - v0[(size_t)(x - 2) * 3]) & 255;
            gpdef = 1;
            gate = cur[x - 1].ex && cur[x - 2].ex;
        }
        if (gate) use = 2 * (int)ch->eg < (int)ch->et;
        if (use) predv = gp;
    }
    const int wsel = (nmodel == NMODEL) ? (c == 0 ? (qE * 2 + use) : c == 1 ? ((qE * 3 + (gq == 4 ? 0 : 1 + (iabs(eg) > 3))) * 2 + use) : ((qE * 3 + ((rq == 4 && gq == 4) ? 0 : 1 + (iabs(er_r) > 3))) * 2 + use)) : (qE * 8 + (c ? (gq == 4 ? 0 : 1 + (iabs(eg) > 3)) : 3));
    s->wrow = ch->wt + (size_t)wsel * nodes * WSTRIDE;
    s->wpred = wpred;
    s->pred8 = pred8;
    s->predv = predv;
    s->base = base;
    s->gp = gp; s->gpdef = gpdef; s->pt = pt; s->gate = gate;
    s->x0 = x0; s->x1 = x1; s->x2 = x2;
}

/* Code the sample prepared by cm_model and update the channel state. The
   encoder passes the sample value; the decoder returns it. */
RF_INLINE int cm_code(cm_t *m, const row_ctx_t *rc, const int c, uint32_t x, int value, const stage_t *s, const int enc, const int nmodel) {
    chan_t *ch = &m->ch[c];
    int16_t *r0 = rc->r0[c];
    pos_t *cur = rc->cur[c];
    pos_t *prv = rc->prv[c];
    uint16_t *const *cp = s->cp;
    uint16_t rate[8] __attribute__((aligned(16)));
    if (nmodel == NMODEL) {
        const int is_gg = (s->predv != s->base);
        for (int k = 0; k < NMODEL; k++) {
            uint32_t n = cp[k][0];
            uint32_t rn = (is_gg && n >= 2) ? (n - 2) : n;
            rate[k] = (uint16_t)RATE[rn];
            cp[k][0] = (uint16_t)(n + (n < 72));
        }
    } else {
        for (int k = 0; k < nmodel; k++) {
            uint32_t n = cp[k][0];
            rate[k] = (uint16_t)RATE_V2[n];
            cp[k][0] = (uint16_t)(n + (n < 60));
        }
    }
    const int predv = s->predv, base = s->base;
    const int32_t pred8 = s->pred8, wpred = s->wpred;
    const int32_t *pr = s->pr;

    int r;
    if (enc) {
        r = (value - predv) & 255;
        r = r >= 128 ? r - 256 : r;
        code_residual(ch, cp, rate, s->wrow, r, enc, nmodel);
    } else {
        r = code_residual(ch, cp, rate, s->wrow, 0, enc, nmodel);
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
        pc->err = (int16_t)(wpred - v8);
        __m128i ev32 = _mm_srli_epi32(_mm_add_epi32(_mm_abs_epi32(_mm_sub_epi32(_mm_load_si128((const __m128i *)pr), _mm_set1_epi32(v8))), _mm_set1_epi32(3)), 3);
        __m128i ev16 = _mm_packus_epi32(ev32, ev32);
        _mm_storel_epi64((__m128i *)pc->perr, ev16);
        _mm_storel_epi64((__m128i *)pa->perr, _mm_adds_epu16(_mm_loadl_epi64((const __m128i *)pa->perr), ev16));
        pc->er = (int16_t)r;
        pc->fe = (int16_t)ferr;
        pc->ex = (uint16_t)(s->gpdef && value == s->gp);
        if (s->gate) {
            ch->eg = (uint16_t)(ch->eg + gg_dist(value, s->gp));
            ch->et = (uint16_t)(ch->et + gg_dist(value, s->pt));
            if (ch->eg >= 4096 || ch->et >= 4096) { ch->eg >>= 1; ch->et >>= 1; }
        }
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

#if defined(__clang__)
#define RF_MINSIZE __attribute__((noinline, minsize))
#else
#define RF_MINSIZE RF_NOINLINE
#endif

static RF_MINSIZE void cm_model_ni(cm_t *m, const row_ctx_t *rc, int c, uint32_t x, uint32_t y, int interior, stage_t *s, int nmodel) {
    cm_model(m, rc, c, x, y, interior, s, nmodel);
}

static RF_MINSIZE int cm_code_ni(cm_t *m, const row_ctx_t *rc, int c, uint32_t x, int value, const stage_t *s, int enc, int nmodel) {
    return cm_code(m, rc, c, x, value, s, enc, nmodel);
}

static RF_NOINLINE void cm_code_enc7(cm_t *m, const row_ctx_t *rc, int c, uint32_t x, const stage_t *s) {
    cm_code(m, rc, c, x, channel_value(rc, c, x), s, 1, NMODEL);
}

static RF_NOINLINE void cm_code_dec7(cm_t *m, const row_ctx_t *rc, int c, uint32_t x, const stage_t *s) {
    cm_code(m, rc, c, x, 0, s, 0, NMODEL);
}

/* ------------------------------------------------------------ drivers */
/* One thread: a three-stage pipeline per row, G at x, R at x - 1, B at x - 2.
   The three samples of one step do not depend on each other, so their
   dependency chains and cache misses overlap. */
RF_INLINE void run_pipelined(cm_t *m, const int enc, const int nmodel) {
    const uint32_t w = m->w;
    if (m->is_first_strip) {
        for (int c = 0; c < 3; c++) chan_init_cold(&m->ch[c]);
    } else if (m->abort || m->ch[0].warm_ready != 1 || m->ch[1].warm_ready != 1 || m->ch[2].warm_ready != 1) {
        m->abort = 1;
        if (m->next_strip) {
            m->next_strip->abort = 1;
            for (int c = 0; c < 3; c++) m->next_strip->ch[c].warm_ready = 2;
        }
        return;
    }
    for (uint32_t y = 0; y < m->h; y++) {
        row_ctx_t rc;
        row_ctx_init(m, y, &rc);
        if (y >= 2 && w >= 5 && nmodel == NMODEL) {
            for (uint32_t x = 0; x < 4; x++) {
                stage_t sg, sr, sb;
                const int dg = x < w, dr = x >= 1 && x - 1 < w, db = x >= 2;
                if (dg) cm_model_ni(m, &rc, 0, x, y, 0, &sg, nmodel);
                if (dr) cm_model_ni(m, &rc, 1, x - 1, y, 0, &sr, nmodel);
                if (db) cm_model_ni(m, &rc, 2, x - 2, y, 0, &sb, nmodel);
                if (dg) cm_code_ni(m, &rc, 0, x, enc ? channel_value(&rc, 0, x) : 0, &sg, enc, nmodel);
                if (dr) cm_code_ni(m, &rc, 1, x - 1, enc ? channel_value(&rc, 1, x - 1) : 0, &sr, enc, nmodel);
                if (db) cm_code_ni(m, &rc, 2, x - 2, enc ? channel_value(&rc, 2, x - 2) : 0, &sb, enc, nmodel);
            }
            for (uint32_t x = 4; x < w - 1; x++) {
                stage_t sg, sr, sb;
                cm_model(m, &rc, 0, x, y, 1, &sg, NMODEL);
                cm_model(m, &rc, 1, x - 1, y, 1, &sr, NMODEL);
                cm_model(m, &rc, 2, x - 2, y, 1, &sb, NMODEL);
                if (enc) {
                    cm_code_enc7(m, &rc, 0, x, &sg);
                    cm_code_enc7(m, &rc, 1, x - 1, &sr);
                    cm_code_enc7(m, &rc, 2, x - 2, &sb);
                } else {
                    cm_code_dec7(m, &rc, 0, x, &sg);
                    cm_code_dec7(m, &rc, 1, x - 1, &sr);
                    cm_code_dec7(m, &rc, 2, x - 2, &sb);
                }
            }
            for (uint32_t x = w - 1; x < w + 2; x++) {
                stage_t sg, sr, sb;
                const int dg = x < w, dr = x >= 1 && x - 1 < w, db = x >= 2;
                if (dg) cm_model_ni(m, &rc, 0, x, y, 0, &sg, nmodel);
                if (dr) cm_model_ni(m, &rc, 1, x - 1, y, 0, &sr, nmodel);
                if (db) cm_model_ni(m, &rc, 2, x - 2, y, 0, &sb, nmodel);
                if (dg) cm_code_ni(m, &rc, 0, x, enc ? channel_value(&rc, 0, x) : 0, &sg, enc, nmodel);
                if (dr) cm_code_ni(m, &rc, 1, x - 1, enc ? channel_value(&rc, 1, x - 1) : 0, &sr, enc, nmodel);
                if (db) cm_code_ni(m, &rc, 2, x - 2, enc ? channel_value(&rc, 2, x - 2) : 0, &sb, enc, nmodel);
            }
        } else {
            for (uint32_t x = 0; x < w + 2; x++) {
                stage_t sg, sr, sb;
                const int dg = x < w, dr = x >= 1 && x - 1 < w, db = x >= 2;
                if (dg) cm_model_ni(m, &rc, 0, x, y, 0, &sg, nmodel);
                if (dr) cm_model_ni(m, &rc, 1, x - 1, y, 0, &sr, nmodel);
                if (db) cm_model_ni(m, &rc, 2, x - 2, y, 0, &sb, nmodel);
                if (dg) cm_code_ni(m, &rc, 0, x, enc ? channel_value(&rc, 0, x) : 0, &sg, enc, nmodel);
                if (dr) cm_code_ni(m, &rc, 1, x - 1, enc ? channel_value(&rc, 1, x - 1) : 0, &sr, enc, nmodel);
                if (db) cm_code_ni(m, &rc, 2, x - 2, enc ? channel_value(&rc, 2, x - 2) : 0, &sb, enc, nmodel);
            }
        }
        if (m->next_strip && y + 1 == m->warm_rows) {
            for (int c = 0; c < 3; c++) {
                chan_copy_warm(&m->next_strip->ch[c], &m->ch[c]);
                m->next_strip->ch[c].warm_ready = 1;
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
        } else if (m->check_cap && (y & 31u) == 0u) {
            size_t tot = 24u;
            for (int c = 0; c < 3; c++) tot += m->ch[c].enc.pos + (size_t)m->ch[c].enc.pending;
            if (tot > m->cap) {
                m->abort = 1;
                break;
            }
        }
    }
    if (m->next_strip) {
        if (m->abort) m->next_strip->abort = 1;
        if (!m->next_strip->ch[0].warm_ready) {
            m->next_strip->abort = 1;
            for (int c = 0; c < 3; c++) m->next_strip->ch[c].warm_ready = 2;
        }
    }
}
static RF_NOINLINE void run_pipelined_enc(cm_t *m) { run_pipelined(m, 1, NMODEL); }
static RF_NOINLINE void run_pipelined_dec7(cm_t *m) { run_pipelined(m, 0, NMODEL); }
static RF_NOINLINE void run_pipelined_dec5(cm_t *m) { run_pipelined(m, 0, 5); }

static void wait_done(volatile int32_t *done, int32_t need) {
    for (int spins = 0; __atomic_load_n(done, __ATOMIC_ACQUIRE) < need; spins++) {
        if (spins < 256) _mm_pause();
        else { rf_yield(); spins = 0; }
    }
}

RF_INLINE void run_channel(cm_t *m, const int c, const int enc, const int nmodel) {
    const uint32_t w = m->w;
    const int check_cap = enc && m->check_cap;
    if (m->is_first_strip) chan_init_cold(&m->ch[c]);
    else {
        wait_done(&m->ch[c].warm_ready, 1);
        if (__atomic_load_n(&m->ch[c].warm_ready, __ATOMIC_ACQUIRE) != 1)
            __atomic_store_n(&m->abort, 1, __ATOMIC_RELEASE);
    }
    for (uint32_t y = 0; y < m->h; y++) {
        if (c >= 1) wait_done(&m->ch[0].done, (int32_t)y + 1);
        if (c == 2) wait_done(&m->ch[1].done, (int32_t)y + 1);
        if (c < 2 && y >= XROWS) wait_done(&m->ch[2].done, (int32_t)(y - XROWS + 1));
        if (__atomic_load_n(&m->abort, __ATOMIC_ACQUIRE)) break;
        row_ctx_t rc;
        row_ctx_init_ch(m, y, c, &rc);
        if (y >= 2 && w >= 4 && nmodel == NMODEL) {
            for (uint32_t x = 0; x < 2; x++) {
                stage_t s;
                cm_model_ni(m, &rc, c, x, y, 0, &s, nmodel);
                cm_code_ni(m, &rc, c, x, enc ? channel_value(&rc, c, x) : 0, &s, enc, nmodel);
            }
            for (uint32_t x = 2; x < w - 1; x++) {
                stage_t s;
                cm_model(m, &rc, c, x, y, 1, &s, NMODEL);
                if (enc) cm_code_enc7(m, &rc, c, x, &s);
                else cm_code(m, &rc, c, x, 0, &s, 0, NMODEL);
            }
            {
                uint32_t x = w - 1;
                stage_t s;
                cm_model_ni(m, &rc, c, x, y, 0, &s, nmodel);
                cm_code_ni(m, &rc, c, x, enc ? channel_value(&rc, c, x) : 0, &s, enc, nmodel);
            }
        } else {
            for (uint32_t x = 0; x < w; x++) {
                stage_t s;
                cm_model_ni(m, &rc, c, x, y, 0, &s, nmodel);
                cm_code_ni(m, &rc, c, x, enc ? channel_value(&rc, c, x) : 0, &s, enc, nmodel);
            }
        }
        if (m->next_strip && y + 1 == m->warm_rows) {
            chan_copy_warm(&m->next_strip->ch[c], &m->ch[c]);
            __atomic_store_n(&m->next_strip->ch[c].warm_ready, 1, __ATOMIC_RELEASE);
            if (m->next_strip->ch[c].th_handle) rf_thread_resume(m->next_strip->ch[c].th_handle);
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
    if (m->next_strip) {
        if (__atomic_load_n(&m->abort, __ATOMIC_ACQUIRE))
            __atomic_store_n(&m->next_strip->abort, 1, __ATOMIC_RELEASE);
        if (__atomic_load_n(&m->next_strip->ch[c].warm_ready, __ATOMIC_ACQUIRE) == 0) {
            __atomic_store_n(&m->next_strip->abort, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&m->next_strip->ch[c].warm_ready, 2, __ATOMIC_RELEASE);
            if (m->next_strip->ch[c].th_handle) rf_thread_resume(m->next_strip->ch[c].th_handle);
        }
    }
}
static RF_NOINLINE void run_channel_enc(cm_t *m, int c) {
    for (; m && !m->cancel; m = m->chain_next) run_channel(m, c, 1, NMODEL);
}
static RF_NOINLINE void run_channel_dec7(cm_t *m, int c) {
    for (; m && !m->cancel; m = m->chain_next) run_channel(m, c, 0, NMODEL);
}
static RF_MINSIZE void run_channel_dec5(cm_t *m, int c) {
    if (!m->cancel) run_channel(m, c, 0, 5);
}
static void run_g_enc(void *p) { run_channel_enc((cm_t *)p, 0); }
static void run_r_enc(void *p) { run_channel_enc((cm_t *)p, 1); }
static void run_b_enc(void *p) { run_channel_enc((cm_t *)p, 2); }
static void run_g_dec7(void *p) { run_channel_dec7((cm_t *)p, 0); }
static void run_r_dec7(void *p) { run_channel_dec7((cm_t *)p, 1); }
static void run_b_dec7(void *p) { run_channel_dec7((cm_t *)p, 2); }
static void run_g_dec5(void *p) { run_channel_dec5((cm_t *)p, 0); }
static void run_r_dec5(void *p) { run_channel_dec5((cm_t *)p, 1); }
static void run_b_dec5(void *p) { run_channel_dec5((cm_t *)p, 2); }

#ifdef RF_TEST_THREAD_FAIL
extern uint32_t g_test_thread_fail_mask;
extern int g_test_thread_calls;
extern int g_test_thread_injected;
extern int g_test_thread_live;
static inline void *cm_thread_create(void (*fn)(void *), void *arg) {
    int idx = g_test_thread_calls++;
    if (idx < 32 && ((g_test_thread_fail_mask >> idx) & 1u)) {
        g_test_thread_injected++;
        return 0;
    }
    void *h = rf_thread_create(fn, arg);
    if (h) g_test_thread_live++;
    return h;
}
static inline void cm_thread_join(void *h) {
    rf_thread_join(h);
    g_test_thread_live--;
}
#else
#define cm_thread_create rf_thread_create
#define cm_thread_join rf_thread_join
#endif

static void run_one_strip(cm_t *m, int threads, const int enc) {
    const int v3 = m->ch[0].nmodel == NMODEL;
    m->chain_next = 0;
    if (threads >= 2) {
        m->cancel = 0;
        void *t1 = cm_thread_create(enc ? run_r_enc : (v3 ? run_r_dec7 : run_r_dec5), m);
        void *t2 = cm_thread_create(enc ? run_b_enc : (v3 ? run_b_dec7 : run_b_dec5), m);
        if (t1 && t2) {
            rf_thread_resume(t1);
            rf_thread_resume(t2);
            if (enc) run_g_enc(m); else if (v3) run_g_dec7(m); else run_g_dec5(m);
            cm_thread_join(t1);
            cm_thread_join(t2);
            return;
        }
        m->cancel = 1;
        if (t1) { rf_thread_resume(t1); cm_thread_join(t1); }
        if (t2) { rf_thread_resume(t2); cm_thread_join(t2); }
    }
    if (enc) run_pipelined_enc(m); else if (v3) run_pipelined_dec7(m); else run_pipelined_dec5(m);
}

static void run_strips(cm_t *m, int S, int threads, const int enc) {
    const int v3 = m[0].ch[0].nmodel == NMODEL;
    if (threads <= 0) {
        int ncpu = rf_cpu_count();
        threads = ncpu < 1 ? 1 : (ncpu > 9 ? 9 : ncpu);
    }
    if (S > 1 && threads >= 3 * S) {
        void (*fn_g)(void *) = enc ? run_g_enc : (v3 ? run_g_dec7 : run_g_dec5);
        void (*fn_r)(void *) = enc ? run_r_enc : (v3 ? run_r_dec7 : run_r_dec5);
        void (*fn_b)(void *) = enc ? run_b_enc : (v3 ? run_b_dec7 : run_b_dec5);
        void *th[12];
        int nth = 0, ok = 1;
        for (int s = 0; s < S; s++) { m[s].cancel = 0; m[s].chain_next = 0; }
        for (int s = 0; s < S; s++) {
            if (s > 0) {
                m[s].ch[0].th_handle = th[nth] = cm_thread_create(fn_g, &m[s]);
                if (!th[nth]) ok = 0;
                nth++;
            }
            m[s].ch[1].th_handle = th[nth] = cm_thread_create(fn_r, &m[s]);
            if (!th[nth]) ok = 0;
            nth++;
            m[s].ch[2].th_handle = th[nth] = cm_thread_create(fn_b, &m[s]);
            if (!th[nth]) ok = 0;
            nth++;
        }
        if (ok) {
            rf_thread_resume(m[0].ch[1].th_handle);
            rf_thread_resume(m[0].ch[2].th_handle);
            fn_g(&m[0]);
            for (int i = 0; i < nth; i++) cm_thread_join(th[i]);
            return;
        }
        for (int s = 0; s < S; s++) {
            m[s].cancel = 1;
            for (int c = 0; c < 3; c++) m[s].ch[c].th_handle = 0;
        }
        for (int i = 0; i < nth; i++) {
            if (th[i]) { rf_thread_resume(th[i]); cm_thread_join(th[i]); }
        }
    }
    if (S > 1 && threads >= 2 && v3) {
        for (int s = 0; s < S; s++) { m[s].cancel = 0; m[s].chain_next = m[s].next_strip; }
        void *t1 = cm_thread_create(enc ? run_r_enc : run_r_dec7, &m[0]);
        void *t2 = cm_thread_create(enc ? run_b_enc : run_b_dec7, &m[0]);
        if (t1 && t2) {
            rf_thread_resume(t1);
            rf_thread_resume(t2);
            if (enc) run_g_enc(&m[0]); else run_g_dec7(&m[0]);
            cm_thread_join(t1);
            cm_thread_join(t2);
            return;
        }
        for (int s = 0; s < S; s++) m[s].cancel = 1;
        if (t1) { rf_thread_resume(t1); cm_thread_join(t1); }
        if (t2) { rf_thread_resume(t2); cm_thread_join(t2); }
    }
    for (int s = 0; s < S; s++) run_one_strip(&m[s], S == 1 ? threads : 1, enc);
}

static void strip_bounds(uint32_t h, int S, int s, uint32_t *y0_out, uint32_t *y1_out) {
    uint32_t w_rows = (S == 2 ? 68u : (h / 10u > 64u ? h / 10u : 64u));
    uint32_t total_extra = (uint32_t)(S * (S - 1) / 2) * w_rows;
    if (S == 1 || h <= total_extra + (uint32_t)S * 32u) {
        *y0_out = (uint32_t)(((uint64_t)s * h) / (uint32_t)S);
        *y1_out = (uint32_t)(((uint64_t)(s + 1) * h) / (uint32_t)S);
    } else {
        uint32_t rem = h - total_extra;
        uint32_t q = rem / (uint32_t)S, r_rem = rem % (uint32_t)S;
        uint32_t y0 = 0;
        for (int k = 0; k < s; k++) {
            y0 += q + ((uint32_t)k < r_rem ? 1u : 0u) + (uint32_t)(S - 1 - k) * w_rows;
        }
        *y0_out = y0;
        *y1_out = y0 + q + ((uint32_t)s < r_rem ? 1u : 0u) + (uint32_t)(S - 1 - s) * w_rows;
    }
}

static int choose_strips(uint32_t h) {
    return h >= 768 ? 3 : 1;
}

/* ------------------------------------------------------------ interface */
/* Payload: version 3 carries 1-byte strip count S, 3 * S u32 stream sizes, and
   3 * S streams in strip order. out must have at least 1 + 12 * S + 3 * w * h +
   3 * S * (64 / S) <= 3 * w * h + 226 writable bytes; cap bounds the final
   payload size. */
int rf_cm_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap, size_t *written, int threads, rf_error *err) {
    cm_tables();
    *written = 0;
    int S = choose_strips(h);
    size_t hdr = 1u + (size_t)S * 12u;
    if (cap < hdr + (size_t)S * 15u) return RF_OK;
    cm_t *m = cm_alloc_strips(S, w, h, (uint8_t *)rgb, 0, NMODEL, err);
    if (!m) return RF_ERR_LIMIT;
    size_t off_each[12];
    size_t cap_each[12];
    size_t cur_off = hdr;
    int check_cap = cap < (size_t)w * h * 3u - 1u;
    for (int s = 0; s < S; s++) {
        m[s].cap = cap;
        m[s].check_cap = check_cap;
        size_t each = (size_t)w * m[s].h + (64u / (size_t)S);
        for (int c = 0; c < 3; c++) {
            int idx = s * 3 + c;
            off_each[idx] = cur_off;
            cap_each[idx] = each;
            renc_init(&m[s].ch[c].enc, out + cur_off, each);
            cur_off += each;
        }
    }
    run_strips(m, S, threads, 1);
    size_t total = hdr;
    int fits = 1;
    for (int s = 0; s < S; s++) {
        if (m[s].abort) fits = 0;
        for (int c = 0; c < 3; c++) {
            renc_flush(&m[s].ch[c].enc);
            total += m[s].ch[c].enc.pos;
            if (m[s].ch[c].enc.pos > cap_each[s * 3 + c]) fits = 0;
        }
    }
    if (fits && total <= cap) {
        out[0] = (uint8_t)S;
        uint8_t *o = out + hdr;
        for (int s = 0; s < S; s++) {
            for (int c = 0; c < 3; c++) {
                int idx = s * 3 + c;
                uint32_t n = (uint32_t)m[s].ch[c].enc.pos;
                size_t sz_off = 1u + (size_t)idx * 4u;
                out[sz_off + 0] = (uint8_t)n;
                out[sz_off + 1] = (uint8_t)(n >> 8);
                out[sz_off + 2] = (uint8_t)(n >> 16);
                out[sz_off + 3] = (uint8_t)(n >> 24);
                if (o != out + off_each[idx]) memmove(o, out + off_each[idx], n);
                o += n;
            }
        }
        *written = total;
    }
    rf_free(m[0].block);
    return RF_OK;
}

int rf_cm_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, int threads, int version, rf_error *err) {
    cm_tables();
    int S = 1;
    size_t hdr = 12u;
    if (version == 3) {
        if (n < 1u) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm streams");
        S = in[0];
        if (S < 1 || S > 4 || (uint32_t)S > h) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm strips");
        hdr = 1u + (size_t)S * 12u;
    }
    if (n < hdr) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm streams");
    uint64_t sz[12] = {0}, sum = hdr;
    const uint8_t *sz_p = in + (version == 3 ? 1u : 0u);
    for (int i = 0; i < S * 3; i++) {
        const uint8_t *q = sz_p + (size_t)i * 4u;
        sz[i] = (uint64_t)q[0] | ((uint64_t)q[1] << 8) | ((uint64_t)q[2] << 16) | ((uint64_t)q[3] << 24);
        if (sz[i] < 5) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm streams");
        sum += sz[i];
    }
    if (sum != n) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm streams");
    int nmodel = version == 3 ? NMODEL : 5;
    cm_t *m = cm_alloc_strips(S, w, h, out, stride, nmodel, err);
    if (!m) return RF_ERR_LIMIT;
    const uint8_t *p = in + hdr;
    for (int s = 0; s < S; s++) {
        for (int c = 0; c < 3; c++) {
            rdec_init(&m[s].ch[c].dec, p, (size_t)sz[s * 3 + c]);
            p += sz[s * 3 + c];
        }
    }
    run_strips(m, S, threads, 0);
    int bad = 0;
    for (int s = 0; s < S; s++) {
        for (int c = 0; c < 3; c++) bad |= m[s].ch[c].bad | (m[s].ch[c].dec.in != m[s].ch[c].dec.end);
    }
    rf_free(m[0].block);
    return bad ? rf_fail(err, RF_ERR_FORMAT, "invalid fold: cm stream") : RF_OK;
}
