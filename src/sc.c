/* Methods 5 and 6: screen content with backward-reference copies and binary range coding (version 3 only).
   The encoder writes method 6; method 5 is decode-only. spec/FOLD.md ("Method 5, screen content" and
   "Method 6, screen content v2") is the normative description; research/screencode2.py is the prototype
   rf_sc2_encode reproduces byte for byte. */
#include "rf.h"
#include "rc.h"

#define SC_MIN_MATCH 4u
#define SC2_MIN_INHERIT 2u
#define SC_MAX_MATCH 4096u
#define SC_CHAIN 64u
#define SC2_WINDOW ((1u << 20) - 1u)
#define SC_PREV_RING (1u << 20)
#define SC_PREV_MASK (SC_PREV_RING - 1u)
#define SC_HASH_BITS 16u
#define SC_CACHE_BITS 8u
#define SC2_CACHE_BITS 11u

enum { SC_LITERAL = 0, SC_CACHE = 1, SC_COPY = 2 };
enum { SC2_COPY = 0, SC2_EQ_W = 1, SC2_EQ_N = 2, SC2_MTF = 3, SC2_CACHE = 4, SC2_LIT = 5 };

/* RFC 9649 section 3.6.2.2.1 (Figure 20): distance code v in 1..120 names entry v - 1 as (xi, yi). */
static const int8_t sc_map[120][2] = {
    {0, 1}, {1, 0}, {1, 1}, {-1, 1}, {0, 2}, {2, 0}, {1, 2}, {-1, 2}, {2, 1}, {-2, 1}, {2, 2}, {-2, 2},
    {0, 3}, {3, 0}, {1, 3}, {-1, 3}, {3, 1}, {-3, 1}, {2, 3}, {-2, 3}, {3, 2}, {-3, 2}, {0, 4}, {4, 0},
    {1, 4}, {-1, 4}, {4, 1}, {-4, 1}, {3, 3}, {-3, 3}, {2, 4}, {-2, 4}, {4, 2}, {-4, 2}, {0, 5}, {3, 4},
    {-3, 4}, {4, 3}, {-4, 3}, {5, 0}, {1, 5}, {-1, 5}, {5, 1}, {-5, 1}, {2, 5}, {-2, 5}, {5, 2}, {-5, 2},
    {4, 4}, {-4, 4}, {3, 5}, {-3, 5}, {5, 3}, {-5, 3}, {0, 6}, {6, 0}, {1, 6}, {-1, 6}, {6, 1}, {-6, 1},
    {2, 6}, {-2, 6}, {6, 2}, {-6, 2}, {4, 5}, {-4, 5}, {5, 4}, {-5, 4}, {3, 6}, {-3, 6}, {6, 3}, {-6, 3},
    {0, 7}, {7, 0}, {1, 7}, {-1, 7}, {5, 5}, {-5, 5}, {7, 1}, {-7, 1}, {4, 6}, {-4, 6}, {6, 4}, {-6, 4},
    {2, 7}, {-2, 7}, {7, 2}, {-7, 2}, {3, 7}, {-3, 7}, {7, 3}, {-7, 3}, {5, 6}, {-5, 6}, {6, 5}, {-6, 5},
    {8, 0}, {4, 7}, {-4, 7}, {7, 4}, {-7, 4}, {8, 1}, {8, 2}, {6, 6}, {-6, 6}, {8, 3}, {5, 7}, {-5, 7},
    {7, 5}, {-7, 5}, {8, 4}, {6, 7}, {-6, 7}, {7, 6}, {-7, 6}, {8, 5}, {7, 7}, {-7, 7}, {8, 6}, {8, 7},
};

typedef struct { uint16_t c, n; } sc_ctr;

/* Method 5 counter layout (decode-only). */
typedef struct {
    sc_ctr is_copy[3], is_cache[3];
    sc_ctr lit_g[256], lit_rg[4][256], lit_bg[4][256];
    sc_ctr cache_tree[256];
    sc_ctr len_tree[32], len_extra[32];
    sc_ctr dist_tree[2][64], dist_extra[64];
} sc_model;

/* Method 6 counter layout. */
typedef struct {
    sc_ctr is_copy[6], is_inherit[6], inh_b0[6], inh_b1, inh_b2;
    sc_ctr len_tree[2][32], len_extra[2][24];
    sc_ctr is_same_row[2], is_rep_dy, rep_dy_b0, rep_dy_b1[2];
    sc_ctr dy_tree[64], dy_extra[40];
    sc_ctr dx0_tree[64], dx0_extra[40];
    sc_ctr dx_zero[2], dx_sign, dx_tree[64], dx_extra[50];
    sc_ctr is_eq_w[12], is_eq_n[6], is_mtf[6], mtf_tree[16];
    sc_ctr is_cache[6], cache_tree[1u << SC2_CACHE_BITS];
    sc_ctr lit_g[8][256], lit_rg[4][256], lit_bg[4][256];
} sc2_model;

static const uint16_t sc_rate[61] = {
    43690, 26214, 18724, 14563, 11915, 10082, 8738, 7710, 6898, 6241, 5698, 5242, 4854, 4519, 4228, 3971,
    3744, 3542, 3360, 3196, 3048, 2912, 2788, 2674, 2570, 2473, 2383, 2299, 2221, 2148, 2080, 2016,
    1956, 1899, 1846, 1795, 1747, 1702, 1659, 1618, 1579, 1542, 1506, 1472, 1440, 1409, 1379, 1351,
    1323, 1297, 1272, 1248, 1224, 1202, 1180, 1159, 1139, 1120, 1101, 1083, 1065,
};

typedef struct {
    sc2_model *full, *dyn_full;
    uint16_t *tkey;
    sc_ctr *tval;
    uint32_t *cache, *dyn_cache;
    uint16_t *ckey;
    uint32_t *cval;
    uint32_t *orig_ring, *dyn_ring;
    uint32_t tused, cused, ring_cap;
} sc2_mctx;

/* Method 4's counter: zero-initialized memory represents {32768, 0}; probability max(1, cur) / 65536 of a 1. */
RF_INLINE uint32_t sc_take(sc_ctr *k, int bit) {
    uint32_t cur = k->n ? k->c : 32768u, p = cur ? cur : 1u, r = sc_rate[k->n];
    if (k->n < 60u) k->n++;
    if (bit) {
        if (cur < 65535u) { uint32_t s = ((65535u - cur) * r) >> 16; k->c = (uint16_t)(cur + (s ? s : 1u)); }
    } else if (cur > 0u) {
        uint32_t s = (cur * r) >> 16; k->c = (uint16_t)(cur - (s ? s : 1u));
    }
    return p;
}

RF_INLINE sc_ctr *sc2_at(sc2_mctx *mc, size_t idx) {
    if (mc->full) return &((sc_ctr *)mc->full)[idx];
    uint32_t key = (uint32_t)idx + 1u;
    for (uint32_t i = (key * 0x9E37u) & 511u;; i = (i + 1u) & 511u) {
        if (mc->tkey[i] == key) return &mc->tval[i];
        if (!mc->tkey[i]) { mc->tused++; mc->tkey[i] = (uint16_t)key; mc->tval[i] = (sc_ctr){0, 0}; return &mc->tval[i]; }
    }
}

static int sc2_mctx_ensure(sc2_mctx *mc) {
    if (mc->full || mc->tused < 360u) return 1;
    sc2_model *m = (sc2_model *)rf_alloc(sizeof(sc2_model));
    if (!m) return 0;
    for (uint32_t i = 0; i < 512u; i++)
        if (mc->tkey[i]) ((sc_ctr *)m)[mc->tkey[i] - 1u] = mc->tval[i];
    mc->full = mc->dyn_full = m;
    return 1;
}

static int sc2_cache_grow(sc2_mctx *mc) {
    uint32_t *c = (uint32_t *)rf_alloc(((size_t)1u << SC2_CACHE_BITS) * sizeof(uint32_t));
    if (!c) return 0;
    for (uint32_t i = 0; i < 128u; i++)
        if (mc->ckey[i]) c[mc->ckey[i] - 1u] = mc->cval[i];
    mc->cache = mc->dyn_cache = c;
    return 1;
}

RF_INLINE uint32_t sc2_cache_get(const sc2_mctx *mc, uint32_t ci) {
    if (mc->cache) return mc->cache[ci];
    uint32_t key = ci + 1u;
    for (uint32_t i = (key * 0x9E37u) & 127u;; i = (i + 1u) & 127u) {
        if (!mc->ckey[i]) return 0u;
        if (mc->ckey[i] == key) return mc->cval[i];
    }
}

RF_INLINE int sc2_cache_set(sc2_mctx *mc, uint32_t ci, uint32_t px) {
    uint32_t tagged = 0xFF000000u | px;
    if (mc->cache) { mc->cache[ci] = tagged; return 1; }
    uint32_t key = ci + 1u;
    for (uint32_t i = (key * 0x9E37u) & 127u;; i = (i + 1u) & 127u) {
        if (mc->ckey[i] == key) { mc->cval[i] = tagged; return 1; }
        if (!mc->ckey[i]) {
            if (mc->cused >= 96u) {
                if (!sc2_cache_grow(mc)) return 0;
                mc->cache[ci] = tagged;
                return 1;
            }
            mc->cused++;
            mc->ckey[i] = (uint16_t)key;
            mc->cval[i] = tagged;
            return 1;
        }
    }
}

#define SC2_OFF(field) (__builtin_offsetof(sc2_model, field) / sizeof(sc_ctr))

RF_INLINE void sc_put(renc *e, sc_ctr *k, int bit) {
    uint32_t p = sc_take(k, bit);
    renc_bit16(e, bit, p);
}

static void sc2_put_tree(renc *e, sc2_mctx *mc, size_t base, uint32_t value, int levels) {
    uint32_t node = 1;
    for (int i = levels - 1; i >= 0; i--) {
        int bit = (int)((value >> i) & 1u);
        sc_put(e, sc2_at(mc, base + node), bit);
        node = 2u * node + (uint32_t)bit;
    }
}

static void sc2_put_extra(renc *e, sc_ctr *first, uint32_t nbits, uint32_t value) {
    for (uint32_t i = nbits; i-- > 0;) {
        int bit = (int)((value >> i) & 1u);
        if (i == nbits - 1u) sc_put(e, first, bit);
        else renc_bit16(e, bit, 32768u);
    }
}

/* RFC 9649 section 3.6.2.2 prefix code extended uniformly to 1 <= value <= 2^25 (symbols 0..49). */
static uint32_t sc_prefix(uint32_t value, uint32_t *nbits, uint32_t *extra) {
    uint32_t d = value - 1u;
    if (d < 4u) { *nbits = 0; *extra = 0; return d; }
    uint32_t high = 31u - (uint32_t)__builtin_clz(d);
    *nbits = high - 1u;
    *extra = d & ((1u << *nbits) - 1u);
    return 2u * high + ((d >> (high - 1u)) & 1u);
}

static uint32_t sc_prefix_value(uint32_t symbol, uint32_t extra) {
    if (symbol < 4u) return symbol + 1u;
    uint32_t nbits = (symbol - 2u) >> 1;
    return ((2u + (symbol & 1u)) << nbits) + extra + 1u;
}

RF_INLINE uint32_t sc_cache_index(uint32_t rgb) {
    return (uint32_t)(0x1E35A7BDu * (0xFF000000u | rgb)) >> (32u - SC_CACHE_BITS);
}

RF_INLINE uint32_t sc2_cache_index(uint32_t rgb) {
    return (uint32_t)(0x1E35A7BDu * (0xFF000000u | rgb)) >> (32u - SC2_CACHE_BITS);
}

RF_INLINE uint32_t sc_pixel(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
RF_INLINE uint32_t sc_bgr_pixel(const uint8_t *p) { return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
RF_INLINE uint64_t sc_rd64(const uint8_t *p) { uint64_t v; __builtin_memcpy(&v, p, 8u); return v; }
RF_INLINE uint32_t sc_rd32(const uint8_t *p) { uint32_t v; __builtin_memcpy(&v, p, 4u); return v; }
RF_INLINE int sc_eq3(const uint8_t *a, const uint8_t *b) {
    return ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2])) == 0;
}
RF_INLINE int sc_eq12(const uint8_t *a, const uint8_t *b) {
    return ((sc_rd64(a) ^ sc_rd64(b)) | (uint64_t)(sc_rd32(a + 8u) ^ sc_rd32(b + 8u))) == 0;
}
RF_INLINE uint32_t sc_fnv1a32(const uint8_t *key) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 12; i++) h = (h ^ key[i]) * 16777619u;
    return h;
}

static uint32_t sc_map_distance(uint32_t v, uint32_t w) {
    int64_t d = (int64_t)sc_map[v - 1u][0] + (int64_t)sc_map[v - 1u][1] * (int64_t)w;
    return d < 1 ? 1u : (uint32_t)d;
}

RF_INLINE void sc2_mtf4(uint32_t *q, uint32_t val) {
    int pos = 3;
    for (int i = 0; i < 4; i++) {
        if (q[i] == val) { pos = i; break; }
    }
    for (int i = pos; i > 0; i--) q[i] = q[i - 1];
    q[0] = val;
}

RF_INLINE uint32_t sc2_ring_add(uint32_t rpos, uint32_t add, uint32_t ring_cap) {
    uint32_t v = rpos + add;
    return v >= ring_cap ? v - ring_cap : v;
}

/* Up to 4 distinct causal copy distances in 1..min(p, SC2_WINDOW) at pixel p >= 1. */
static int sc2_gather_cands(uint32_t p, uint32_t w, uint32_t x, uint32_t y, uint32_t rpos, uint32_t ring_cap,
                            const uint32_t *orig_ring, const uint32_t *rep, uint32_t *out) {
    uint32_t maxd = p < SC2_WINDOW ? p : SC2_WINDOW;
    if (maxd < 1u) return 0;
    int n = 0;
#define SC2_ADD(cand_expr) do { \
    uint32_t _d = (cand_expr); \
    if (_d >= 1u && _d <= maxd) { \
        int _dup = 0; \
        for (int _k = 0; _k < n; _k++) if (out[_k] == _d) { _dup = 1; break; } \
        if (!_dup) { out[n++] = _d; if (n == 4) return 4; } \
    } \
} while (0)
    if (y >= 1u) {
        SC2_ADD(orig_ring[sc2_ring_add(rpos, 3u, ring_cap)]);
        if (x + 1u < w) SC2_ADD(orig_ring[sc2_ring_add(rpos, 4u, ring_cap)]);
        if (x >= 1u) SC2_ADD(orig_ring[sc2_ring_add(rpos, 2u, ring_cap)]);
        if (x + 2u < w) SC2_ADD(orig_ring[sc2_ring_add(rpos, 5u, ring_cap)]);
        if (x >= 2u) SC2_ADD(orig_ring[sc2_ring_add(rpos, 1u, ring_cap)]);
    }
    if (x >= 1u) SC2_ADD(orig_ring[rpos == 0u ? ring_cap - 1u : rpos - 1u]);
    for (int i = 0; i < 4; i++) SC2_ADD(rep[i]);
    SC2_ADD(1u);
    SC2_ADD(w);
    SC2_ADD(w + 1u);
    SC2_ADD(w - 1u);
#undef SC2_ADD
    return n;
}

static void sc2_ring_fill(uint32_t *ring, uint32_t ring_cap, uint32_t q, uint32_t count, uint32_t val) {
    if (count >= ring_cap) {
        for (uint32_t i = 0; i < ring_cap; i++) ring[i] = val;
        return;
    }
    uint32_t pos = q % ring_cap;
    uint32_t first = ring_cap - pos < count ? ring_cap - pos : count;
    for (uint32_t i = 0; i < first; i++) ring[pos + i] = val;
    uint32_t rem = count - first;
    for (uint32_t i = 0; i < rem; i++) ring[i] = val;
}

RF_INLINE int sc2_enc_update_color(sc2_mctx *mc, uint32_t *mtf, uint32_t *mtf_cnt, uint32_t px) {
    if (*mtf_cnt > 0u && mtf[0] == px) return 1;
    uint32_t n = *mtf_cnt, pos = n < 16u ? n : 15u;
    int found = 0;
    for (uint32_t i = 1u; i < n; i++) {
        if (mtf[i] == px) { pos = i; found = 1; break; }
    }
    if (!found && n < 16u) *mtf_cnt = n + 1u;
    for (uint32_t i = pos; i > 0u; i--) mtf[i] = mtf[i - 1u];
    mtf[0] = px;
    return sc2_cache_set(mc, sc2_cache_index(px), px);
}

#define SC_MICRO_BKT 256u
#define SC_TINY_BKT 4096u

/* `head` (or `tiny_bkt` for `cap <= 8192`) and `nprev` store 1-based node ids (`0` = empty).
   At most one node is inserted per pixel in strictly increasing `q`, so `p - uq >= node_cnt - id`
   and `SC_PREV_RING = 2^20 > SC2_WINDOW` never overwrites an in-window node. */
typedef struct {
    const uint8_t *rgb;
    uint32_t n, w, node_cnt, node_cap, max_nodes, bkt_cnt, bkt_lim, tiny_used;
    uint32_t *head, *npos, *nprev;
    uint64_t *tiny_bkt;
    uint16_t *ntag;
    uint8_t *dyn_head, *dyn_nodes;
} sc_search;

static int sc_grow_head(sc_search *s) {
    uint32_t *new_head = (uint32_t *)rf_alloc(((size_t)1u << SC_HASH_BITS) * sizeof(uint32_t));
    if (!new_head) return 0;
    for (uint32_t i = 0; i < s->bkt_cnt; i++) {
        uint64_t v = s->tiny_bkt[i];
        if (v) new_head[(uint32_t)(v >> 32)] = (uint32_t)v;
    }
    s->head = new_head;
    s->tiny_bkt = 0;
    s->dyn_head = (uint8_t *)new_head;
    return 1;
}

RF_INLINE uint32_t sc_head_get(const sc_search *s, uint32_t b) {
    if (s->head) return s->head[b];
    uint32_t mask = s->bkt_cnt - 1u;
    for (uint32_t i = (b * 0x9E37u) & mask;; i = (i + 1u) & mask) {
        uint64_t v = s->tiny_bkt[i];
        if (!v) return 0u;
        if ((uint32_t)(v >> 32) == b) return (uint32_t)v;
    }
}

RF_INLINE int sc_head_set(sc_search *s, uint32_t b, uint32_t id) {
    if (s->head) { s->head[b] = id; return 1; }
    uint32_t mask = s->bkt_cnt - 1u;
    for (uint32_t i = (b * 0x9E37u) & mask;; i = (i + 1u) & mask) {
        uint64_t v = s->tiny_bkt[i];
        if (!v) {
            if (s->tiny_used >= s->bkt_lim) {
                if (!sc_grow_head(s)) return 0;
                s->head[b] = id;
                return 1;
            }
            s->tiny_used++;
            s->tiny_bkt[i] = ((uint64_t)b << 32) | id;
            return 1;
        }
        if ((uint32_t)(v >> 32) == b) { s->tiny_bkt[i] = ((uint64_t)b << 32) | id; return 1; }
    }
}

static int sc_grow_nodes(sc_search *s, uint32_t need) {
    uint32_t new_cap = s->node_cap;
    while (new_cap < need && new_cap < s->max_nodes) {
        new_cap = new_cap <= s->max_nodes / 4u ? new_cap * 4u : s->max_nodes;
    }
    size_t cap_sz = (size_t)new_cap;
    size_t off_prev = cap_sz * sizeof(uint32_t);
    size_t off_tag = off_prev + cap_sz * sizeof(uint32_t);
    uint8_t *mem = (uint8_t *)rf_alloc(off_tag + cap_sz * sizeof(uint16_t));
    if (!mem) return 0;
    uint32_t *new_pos = (uint32_t *)mem;
    uint32_t *new_prev = (uint32_t *)(mem + off_prev);
    uint16_t *new_tag = (uint16_t *)(mem + off_tag);
    size_t used = (size_t)s->node_cnt;
    if (used) {
        __builtin_memcpy(new_pos, s->npos, used * sizeof(uint32_t));
        __builtin_memcpy(new_prev, s->nprev, used * sizeof(uint32_t));
        __builtin_memcpy(new_tag, s->ntag, used * sizeof(uint16_t));
    }
    rf_free(s->dyn_nodes);
    s->npos = new_pos;
    s->nprev = new_prev;
    s->ntag = new_tag;
    s->node_cap = new_cap;
    s->dyn_nodes = mem;
    return 1;
}

RF_INLINE int sc_ensure_nodes(sc_search *s, uint32_t add_nodes) {
    if (s->node_cap < s->max_nodes && s->node_cnt + add_nodes > s->node_cap && !sc_grow_nodes(s, s->node_cnt + add_nodes)) return 0;
    return 1;
}

RF_INLINE uint32_t sc_match_tail(const uint8_t *a, const uint8_t *b, size_t off, size_t lim_bytes) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    while (off + 8u <= lim_bytes) {
        uint64_t va = sc_rd64(a + off), vb = sc_rd64(b + off);
        if (va != vb) return (uint32_t)((off + (size_t)((uint32_t)__builtin_ctzll(va ^ vb) >> 3)) / 3u);
        off += 8u;
    }
#endif
    while (off < lim_bytes && a[off] == b[off]) off++;
    return (uint32_t)(off / 3u);
}

RF_INLINE uint32_t sc_match_len(const uint8_t *a, const uint8_t *b, uint32_t limit) {
    if (!sc_eq12(a, b)) return 0;
    return sc_match_tail(a, b, 12u, 3u * (size_t)limit);
}

/* Match length >= 2 for inherited distance candidates (limit >= 2). */
RF_INLINE uint32_t sc2_match_len2(const uint8_t *a, const uint8_t *b, uint32_t limit) {
    if (!sc_eq3(a, b) || !sc_eq3(a + 3u, b + 3u)) return 0;
    if (limit >= 4u && sc_eq12(a, b)) return sc_match_tail(a, b, 12u, 3u * (size_t)limit);
    if (limit >= 3u && sc_eq3(a + 6u, b + 6u)) return 3u;
    return 2u;
}

/* General 4-pixel hash search: candidates distance 1, distance w, then up to 64 entries of the bucket chain
   within SC2_WINDOW; returns the longest match >= min_need (4..limit), shorter distance on a tie (0: none). */
static uint32_t sc2_find_gen(const sc_search *s, uint32_t p, uint32_t limit, uint32_t min_need, uint32_t *dist_out) {
    const uint8_t *cur = s->rgb + 3u * (size_t)p;
    uint32_t best = 0, best_d = 0;
    {
        const uint8_t *b = cur - 3u;
        if (sc_eq3(cur + 3u * (size_t)(min_need - 1u), b + 3u * (size_t)(min_need - 1u))) {
            uint32_t len = sc_match_len(cur, b, limit);
            if (len >= min_need) {
                best = len;
                best_d = 1u;
                if (best == limit) { *dist_out = 1u; return best; }
            }
        }
    }
    if (p >= s->w && s->w > 1u && s->w <= SC2_WINDOW) {
        const uint8_t *b = cur - 3u * (size_t)s->w;
        uint32_t probe = best >= min_need ? best : min_need - 1u;
        if (probe < limit && sc_eq3(cur + 3u * (size_t)probe, b + 3u * (size_t)probe)) {
            uint32_t len = sc_match_len(cur, b, limit);
            if (len >= min_need && len > best) { best = len; best_d = s->w; }
        }
    }
    uint32_t h = sc_fnv1a32(cur);
    uint16_t tag16 = (uint16_t)h;
    uint32_t id = sc_head_get(s, h >> (32u - SC_HASH_BITS));
    for (uint32_t visited = 0; id != 0u && visited < SC_CHAIN; visited++) {
        if (s->node_cnt - id >= SC_PREV_RING) break;
        uint32_t slot = (id - 1u) & SC_PREV_MASK;
        uint32_t uq = s->npos[slot], d = p - uq;
        if (d > SC2_WINDOW || (best == limit && d >= best_d)) break;
        uint32_t next_id = s->nprev[slot];
        if (s->ntag[slot] == tag16) {
            const uint8_t *b = s->rgb + 3u * (size_t)uq;
            uint32_t need_idx = best < min_need ? min_need - 1u : (d < best_d ? best - 1u : best);
            if (need_idx < limit && sc_eq3(cur + 3u * (size_t)need_idx, b + 3u * (size_t)need_idx)) {
                uint32_t len = sc_match_len(cur, b, limit);
                if (len >= min_need && (len > best || (len == best && d < best_d))) { best = len; best_d = d; }
            }
        }
        id = next_id;
    }
    if (best < min_need) return 0;
    *dist_out = best_d;
    return best;
}

static int sc_insert(sc_search *s, uint32_t q) {
    if (q + SC_MIN_MATCH <= s->n) {
        uint32_t h = sc_fnv1a32(s->rgb + 3u * (size_t)q);
        uint32_t b = h >> (32u - SC_HASH_BITS);
        uint32_t id = ++s->node_cnt, slot = (id - 1u) & SC_PREV_MASK;
        s->npos[slot] = q;
        s->nprev[slot] = sc_head_get(s, b);
        s->ntag[slot] = (uint16_t)h;
        if (!sc_head_set(s, b, id)) return 0;
    }
    return 1;
}

/* Necessary-condition prefilter with zero false negatives: every L >= 4 copy [p, p + L) is covered by the
   4-pixel intervals [q, q + 4) of duplicate 12-byte keys q in [p, p + L - 3). */
static RF_NOINLINE int sc_likely_half(const uint8_t *rgb, uint32_t w, uint32_t n) {
    if (n < SC_MIN_MATCH) return 0;
    uint32_t half_need = (uint32_t)(((uint64_t)n + 1u) >> 1);
    uint32_t last = n - SC_MIN_MATCH;
    uint32_t adj_cov = 0, adj_rem = 0;
    for (uint32_t q = 1u; q <= last; q++) {
        const uint8_t *cur = rgb + 3u * (size_t)q;
        if (sc_eq12(cur, cur - 3u) || (q >= w && w > 1u && w <= SC2_WINDOW && sc_eq12(cur, cur - 3u * (size_t)w))) {
            adj_rem = 4u;
        } else if (q >= 256u && !adj_rem && (uint64_t)adj_cov * 4u < (uint64_t)q) {
            break;
        }
        if (adj_rem) {
            if (++adj_cov >= half_need) return 1;
            adj_rem--;
        }
    }
    if (adj_cov + adj_rem >= half_need) return 1;
    uint32_t log_bits = 14u;
    while (log_bits < 24u && ((uint64_t)1u << log_bits) < 8u * (uint64_t)n) log_bits++;
    size_t nwords = (size_t)1u << (log_bits - 6u);
    uint64_t seen_stack[256];
    uint64_t *seen = seen_stack;
    if (nwords <= 256u) {
        __builtin_memset(seen_stack, 0, nwords * sizeof(uint64_t));
    } else {
        seen = (uint64_t *)rf_alloc(nwords * sizeof(uint64_t));
        if (!seen) return 1;
    }
    uint32_t wshift = 70u - log_bits;
    uint32_t cov = 0, rem = 0;
    for (uint32_t q = 0; q <= last; q++) {
        if ((uint64_t)cov + (uint64_t)(n - q) < (uint64_t)half_need) {
            if (seen != seen_stack) rf_free(seen);
            return 0;
        }
        const uint8_t *cur = rgb + 3u * (size_t)q;
        uint64_t lo = sc_rd64(cur);
        uint32_t hi = sc_rd32(cur + 8u);
        uint64_t h = (lo * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)hi * 0xBF58476D1CE4E5B9ULL);
        h ^= h >> 32;
        h *= 0xD6E8FEB86659FD93ULL;
        size_t widx = (size_t)(h >> wshift);
        uint64_t mask = ((uint64_t)1u << (h & 63u)) | ((uint64_t)1u << ((h >> 6) & 63u));
        uint64_t word = seen[widx];
        if ((word & mask) == mask) {
            rem = 4u;
        } else {
            seen[widx] = word | mask;
        }
        if (rem) {
            if (++cov >= half_need) {
                if (seen != seen_stack) rf_free(seen);
                return 1;
            }
            rem--;
        }
    }
    if (seen != seen_stack) rf_free(seen);
    return cov + rem >= half_need;
}

/* Single-pass method 6 parse and range coding: aborts as soon as 2 * covered4 >= n becomes impossible
   or flushed output reaches cap. */
static RF_NOINLINE int sc2_encode_if_half(sc_search *s, renc *e, sc2_mctx *mc, size_t cap, int *oom) {
    uint32_t mtf[16];
    uint32_t mtf_cnt = 0;
    uint32_t rep[4] = {1u, s->w, s->w + 1u, s->w - 1u};
    uint32_t rep_dy[4] = {1u, 2u, 4u, 8u};
    uint32_t cands[4];
    uint32_t ring_cap = mc->ring_cap;
    int prev_kind = SC2_LIT, prev_same_row = 0;
    uint32_t covered4 = 0;

    if (!sc2_mctx_ensure(mc)) { *oom = 1; return 0; }
    uint32_t px0 = sc_pixel(s->rgb);
    uint32_t r0 = px0 >> 16, g0 = (px0 >> 8) & 0xFFu, b0 = px0 & 0xFFu;
    sc2_put_tree(e, mc, SC2_OFF(lit_g) + (size_t)4u * 256u, g0, 8);
    sc2_put_tree(e, mc, SC2_OFF(lit_rg) + (size_t)(g0 >> 6) * 256u, (r0 - g0) & 0xFFu, 8);
    sc2_put_tree(e, mc, SC2_OFF(lit_bg) + (size_t)(g0 >> 6) * 256u, (b0 - g0) & 0xFFu, 8);
    if (e->pos + (size_t)e->pending + 4u >= cap) return 0;
    mc->orig_ring[0] = 0u;
    if (!sc_ensure_nodes(s, 1u) || !sc_insert(s, 0u) || !sc2_enc_update_color(mc, mtf, &mtf_cnt, px0)) {
        *oom = 1;
        return 0;
    }

    uint32_t x = 1u < s->w ? 1u : 0u;
    uint32_t y = 1u < s->w ? 0u : 1u;
    uint32_t rpos = 1u;
    for (uint32_t p = 1u; p < s->n;) {
        if ((uint64_t)(p - covered4) * 2u > (uint64_t)s->n || e->pos + (size_t)e->pending + 4u >= cap) return 0;
        if (!sc2_mctx_ensure(mc)) { *oom = 1; return 0; }

        uint32_t limit = s->n - p < SC_MAX_MATCH ? s->n - p : SC_MAX_MATCH;
        int ncand = sc2_gather_cands(p, s->w, x, y, rpos, ring_cap, mc->orig_ring, rep, cands);
        const uint8_t *cur = s->rgb + 3u * (size_t)p;

        uint32_t best_inh_len = 0, best_inh_dist = 0;
        int best_inh_idx = -1;
        if (limit >= SC2_MIN_INHERIT) {
            for (int idx = 0; idx < ncand; idx++) {
                uint32_t d = cands[idx];
                const uint8_t *b = cur - 3u * (size_t)d;
                if (best_inh_len >= SC2_MIN_INHERIT && (best_inh_len == limit || !sc_eq3(cur + 3u * (size_t)best_inh_len, b + 3u * (size_t)best_inh_len)))
                    continue;
                uint32_t ml = sc2_match_len2(cur, b, limit);
                if (ml >= SC2_MIN_INHERIT && ml > best_inh_len) {
                    best_inh_len = ml;
                    best_inh_idx = idx;
                    best_inh_dist = d;
                }
            }
        }

        uint32_t best_gen_len = 0, best_gen_dist = 0;
        uint32_t min_gen = best_inh_len >= SC2_MIN_INHERIT ? (best_inh_len + 2u > SC_MIN_MATCH ? best_inh_len + 2u : SC_MIN_MATCH) : SC_MIN_MATCH;
        if (limit >= min_gen) {
            best_gen_len = sc2_find_gen(s, p, limit, min_gen, &best_gen_dist);
        }

        int use_copy = 0, use_inh = 0, chosen_inh_idx = -1;
        uint32_t chosen_len = 0, chosen_dist = 0;
        if (best_inh_len >= SC2_MIN_INHERIT && (best_gen_len < SC_MIN_MATCH || best_inh_len + 1u >= best_gen_len)) {
            use_copy = 1;
            use_inh = 1;
            chosen_len = best_inh_len;
            chosen_dist = best_inh_dist;
            chosen_inh_idx = best_inh_idx;
        } else if (best_gen_len >= SC_MIN_MATCH) {
            use_copy = 1;
            chosen_len = best_gen_len;
            chosen_dist = best_gen_dist;
            for (int idx = 0; idx < ncand; idx++) {
                if (cands[idx] == best_gen_dist) { use_inh = 1; chosen_inh_idx = idx; break; }
            }
        }

        if (use_copy) {
            sc_put(e, sc2_at(mc, SC2_OFF(is_copy) + (size_t)prev_kind), 1);
            sc_put(e, sc2_at(mc, SC2_OFF(is_inherit) + (size_t)prev_kind), use_inh);
            if (use_inh) {
                if (ncand > 1) {
                    sc_put(e, sc2_at(mc, SC2_OFF(inh_b0) + (size_t)prev_kind), chosen_inh_idx == 0);
                    if (chosen_inh_idx > 0 && ncand > 2) {
                        sc_put(e, sc2_at(mc, SC2_OFF(inh_b1)), chosen_inh_idx == 1);
                        if (chosen_inh_idx > 1 && ncand > 3)
                            sc_put(e, sc2_at(mc, SC2_OFF(inh_b2)), chosen_inh_idx == 2);
                    }
                }
                uint32_t nb, ex, sym = sc_prefix(chosen_len - 1u, &nb, &ex);
                sc2_put_tree(e, mc, SC2_OFF(len_tree) + 32u, sym, 5);
                sc2_put_extra(e, sc2_at(mc, SC2_OFF(len_extra) + 24u + sym), nb, ex);
            } else {
                uint32_t nb, ex, sym = sc_prefix(chosen_len - 3u, &nb, &ex);
                sc2_put_tree(e, mc, SC2_OFF(len_tree), sym, 5);
                sc2_put_extra(e, sc2_at(mc, SC2_OFF(len_extra) + sym), nb, ex);
                uint32_t q = p - chosen_dist;
                uint32_t qy = q / s->w, qx = q - qy * s->w;
                uint32_t dy = y - qy;
                int same_row;
                if (y == 0u) same_row = 1;
                else if (x == 0u) same_row = 0;
                else {
                    same_row = dy == 0u;
                    sc_put(e, sc2_at(mc, SC2_OFF(is_same_row) + (size_t)prev_same_row), same_row);
                }
                if (same_row) {
                    uint32_t dx = x - qx;
                    sym = sc_prefix(dx, &nb, &ex);
                    sc2_put_tree(e, mc, SC2_OFF(dx0_tree), sym, 6);
                    sc2_put_extra(e, sc2_at(mc, SC2_OFF(dx0_extra) + sym), nb, ex);
                    prev_same_row = 1;
                } else {
                    int ridx = -1;
                    for (int i = 0; i < 4; i++) if (rep_dy[i] == dy) { ridx = i; break; }
                    int in_rdy = ridx >= 0;
                    sc_put(e, sc2_at(mc, SC2_OFF(is_rep_dy)), in_rdy);
                    if (in_rdy) {
                        sc_put(e, sc2_at(mc, SC2_OFF(rep_dy_b0)), ridx == 0);
                        if (ridx > 0) {
                            sc_put(e, sc2_at(mc, SC2_OFF(rep_dy_b1) + 0u), ridx == 1);
                            if (ridx > 1) sc_put(e, sc2_at(mc, SC2_OFF(rep_dy_b1) + 1u), ridx == 2);
                        }
                    } else {
                        sym = sc_prefix(dy, &nb, &ex);
                        sc2_put_tree(e, mc, SC2_OFF(dy_tree), sym, 6);
                        sc2_put_extra(e, sc2_at(mc, SC2_OFF(dy_extra) + sym), nb, ex);
                    }
                    sc2_mtf4(rep_dy, dy);
                    int32_t dx = (int32_t)x - (int32_t)qx;
                    sc_put(e, sc2_at(mc, SC2_OFF(dx_zero) + (size_t)in_rdy), dx == 0);
                    if (dx != 0) {
                        sc_put(e, sc2_at(mc, SC2_OFF(dx_sign)), dx < 0);
                        uint32_t adx = dx < 0 ? (uint32_t)(-dx) : (uint32_t)dx;
                        sym = sc_prefix(adx, &nb, &ex);
                        sc2_put_tree(e, mc, SC2_OFF(dx_tree), sym, 6);
                        sc2_put_extra(e, sc2_at(mc, SC2_OFF(dx_extra) + sym), nb, ex);
                    }
                    prev_same_row = 0;
                }
            }

            sc2_mtf4(rep, chosen_dist);
            if (chosen_len >= SC_MIN_MATCH) covered4 += chosen_len;
            uint32_t end = p + chosen_len;
            if (end == s->n) return (uint64_t)covered4 * 2u >= (uint64_t)s->n;
            if (e->pos + (size_t)e->pending + 4u >= cap) return 0;

            sc2_ring_fill(mc->orig_ring, ring_cap, p, chosen_len, chosen_dist);
            for (uint32_t q = p; q < end;) {
                const uint8_t *qcur = s->rgb + 3u * (size_t)q;
                uint32_t px = sc_pixel(qcur);
                if (!sc2_enc_update_color(mc, mtf, &mtf_cnt, px)) { *oom = 1; return 0; }
                uint32_t rem = end - q;
                if (rem >= 5u && (chosen_dist == 1u || sc_eq12(qcur + 3u, qcur))) {
                    uint32_t run = chosen_dist == 1u ? rem - 1u : sc_match_len(qcur + 3u, qcur, rem - 1u);
                    uint32_t const_end = q + run - 2u;
                    uint32_t q0 = const_end - q > SC_CHAIN ? const_end - SC_CHAIN : q;
                    uint32_t tail_end = q + run + 1u;
                    if (!sc_ensure_nodes(s, tail_end - q0)) { *oom = 1; return 0; }
                    uint32_t h = sc_fnv1a32(qcur);
                    uint32_t b = h >> (32u - SC_HASH_BITS);
                    uint16_t tag16 = (uint16_t)h;
                    uint32_t hp = sc_head_get(s, b);
                    for (uint32_t r = q0; r < const_end; r++) {
                        uint32_t id = ++s->node_cnt, slot = (id - 1u) & SC_PREV_MASK;
                        s->npos[slot] = r;
                        s->nprev[slot] = hp;
                        s->ntag[slot] = tag16;
                        hp = id;
                    }
                    if (const_end > q0 && !sc_head_set(s, b, hp)) { *oom = 1; return 0; }
                    for (uint32_t r = const_end; r < tail_end; r++) {
                        if (!sc_insert(s, r)) { *oom = 1; return 0; }
                    }
                    q = tail_end;
                    continue;
                }
                if (rem >= 6u && (chosen_dist == 2u || sc_eq12(qcur + 6u, qcur))) {
                    uint32_t run = chosen_dist == 2u ? rem - 2u : sc_match_len(qcur + 6u, qcur, rem - 2u);
                    uint32_t px1 = sc_pixel(qcur + 3u);
                    if (!sc2_enc_update_color(mc, mtf, &mtf_cnt, px1)) { *oom = 1; return 0; }
                    if ((run & 1u) && !sc2_enc_update_color(mc, mtf, &mtf_cnt, px)) { *oom = 1; return 0; }
                    uint32_t const_end = q + run - 1u;
                    uint32_t q0 = const_end - q > 2u * SC_CHAIN ? const_end - 2u * SC_CHAIN : q;
                    uint32_t tail_end = q + run + 2u;
                    if (!sc_ensure_nodes(s, tail_end - q0)) { *oom = 1; return 0; }
                    for (uint32_t r = q0; r < tail_end; r++) {
                        if (!sc_insert(s, r)) { *oom = 1; return 0; }
                    }
                    q = tail_end;
                    continue;
                }
                if (!sc_ensure_nodes(s, 1u) || !sc_insert(s, q)) { *oom = 1; return 0; }
                q++;
            }
            p = end;
            y = p / s->w;
            x = p - y * s->w;
            rpos = p % ring_cap;
            prev_kind = SC2_COPY;
            continue;
        }

        sc_put(e, sc2_at(mc, SC2_OFF(is_copy) + (size_t)prev_kind), 0);
        uint32_t px = sc_pixel(cur);
        int has_w = x >= 1u, has_n = y >= 1u;
        uint32_t w_rgb = has_w ? sc_pixel(cur - 3u) : 0u;
        uint32_t n_rgb = has_n ? sc_pixel(cur - 3u * (size_t)s->w) : 0u;
        int tk = SC2_LIT;
        int coded_nb = 0;

        if (has_w) {
            int wn_eq = has_n && w_rgb == n_rgb;
            int eq = px == w_rgb;
            sc_put(e, sc2_at(mc, SC2_OFF(is_eq_w) + (size_t)(prev_kind * 2 + wn_eq)), eq);
            if (eq) { tk = SC2_EQ_W; coded_nb = 1; }
        }
        if (!coded_nb && has_n && (!has_w || n_rgb != w_rgb)) {
            int eq = px == n_rgb;
            sc_put(e, sc2_at(mc, SC2_OFF(is_eq_n) + (size_t)prev_kind), eq);
            if (eq) { tk = SC2_EQ_N; coded_nb = 1; }
        }
        if (!coded_nb) {
            int midx = -1;
            for (uint32_t i = 0; i < mtf_cnt; i++) {
                if (mtf[i] == px) { midx = (int)i; break; }
            }
            if (midx >= 0) {
                sc_put(e, sc2_at(mc, SC2_OFF(is_mtf) + (size_t)prev_kind), 1);
                sc2_put_tree(e, mc, SC2_OFF(mtf_tree), (uint32_t)midx, 4);
                tk = SC2_MTF;
            } else {
                sc_put(e, sc2_at(mc, SC2_OFF(is_mtf) + (size_t)prev_kind), 0);
                uint32_t ci = sc2_cache_index(px);
                if (sc2_cache_get(mc, ci) == (0xFF000000u | px)) {
                    sc_put(e, sc2_at(mc, SC2_OFF(is_cache) + (size_t)prev_kind), 1);
                    sc2_put_tree(e, mc, SC2_OFF(cache_tree), ci, SC2_CACHE_BITS);
                    tk = SC2_CACHE;
                } else {
                    sc_put(e, sc2_at(mc, SC2_OFF(is_cache) + (size_t)prev_kind), 0);
                    uint32_t r = px >> 16, g = (px >> 8) & 0xFFu, b = px & 0xFFu;
                    uint32_t wg_ctx = has_w ? (((w_rgb >> 8) & 0xFFu) >> 5) : 4u;
                    sc2_put_tree(e, mc, SC2_OFF(lit_g) + (size_t)wg_ctx * 256u, g, 8);
                    sc2_put_tree(e, mc, SC2_OFF(lit_rg) + (size_t)(g >> 6) * 256u, (r - g) & 0xFFu, 8);
                    sc2_put_tree(e, mc, SC2_OFF(lit_bg) + (size_t)(g >> 6) * 256u, (b - g) & 0xFFu, 8);
                    tk = SC2_LIT;
                }
            }
        }

        prev_kind = tk;
        if (p + 1u == s->n) return (uint64_t)covered4 * 2u >= (uint64_t)s->n;
        if (e->pos + (size_t)e->pending + 4u >= cap) return 0;
        mc->orig_ring[rpos] = 0u;
        if (!sc_ensure_nodes(s, 1u) || !sc_insert(s, p) || !sc2_enc_update_color(mc, mtf, &mtf_cnt, px)) {
            *oom = 1;
            return 0;
        }
        p++;
        if (++x == s->w) { x = 0u; y++; }
        if (++rpos == ring_cap) rpos = 0u;
    }
    return (uint64_t)covered4 * 2u >= (uint64_t)s->n;
}

/* Each frame stays well under 4096 stack bytes so freestanding Windows x64 never emits ___chkstk_ms,
   and `cap <= 64` encodes run with zero heap allocations unless they outgrow the micro tables. */
static RF_NOINLINE int sc2_encode_micro_a(sc_search *s, renc *e, sc2_mctx *mc, size_t cap, int *oom) {
    uint32_t npos[512];
    uint16_t ntag[512];
    s->npos = npos;
    s->ntag = ntag;
    int ok = sc2_encode_if_half(s, e, mc, cap, oom);
    if (!s->dyn_nodes) { s->npos = 0; s->ntag = 0; }
    return ok;
}

static RF_NOINLINE int sc2_encode_micro_c(sc_search *s, renc *e, sc2_mctx *mc, size_t cap, int *oom) {
    uint32_t ring_small[656];
    uint32_t cval[128];
    uint16_t ckey[128];
    __builtin_memset(ckey, 0, sizeof ckey);
    if (mc->ring_cap <= 656u) {
        __builtin_memset(ring_small, 0, (size_t)mc->ring_cap * sizeof(uint32_t));
        mc->orig_ring = ring_small;
    } else {
        mc->orig_ring = mc->dyn_ring = (uint32_t *)rf_alloc((size_t)mc->ring_cap * sizeof(uint32_t));
        if (!mc->orig_ring) { *oom = 1; return 0; }
    }
    mc->ckey = ckey;
    mc->cval = cval;
    int ok = sc2_encode_micro_a(s, e, mc, cap, oom);
    if (!mc->dyn_ring) mc->orig_ring = 0;
    mc->ckey = 0;
    mc->cval = 0;
    return ok;
}

static RF_NOINLINE int sc2_encode_micro_b(sc_search *s, renc *e, sc2_mctx *mc, size_t cap, int *oom) {
    uint64_t tiny_bkt[64];
    uint32_t nprev[512];
    __builtin_memset(tiny_bkt, 0, sizeof tiny_bkt);
    s->tiny_bkt = tiny_bkt;
    s->nprev = nprev;
    int ok = sc2_encode_micro_c(s, e, mc, cap, oom);
    if (!s->dyn_nodes) s->nprev = 0;
    if (!s->dyn_head) s->tiny_bkt = 0;
    return ok;
}

static RF_NOINLINE int sc2_encode_micro(const uint8_t *rgb, uint32_t w, uint32_t n, uint8_t *out, size_t cap, size_t *written, rf_error *err) {
    uint16_t tkey[512];
    sc_ctr tval[512];
    uint8_t buf[64];
    __builtin_memset(tkey, 0, sizeof tkey);
    sc2_mctx mc = {0};
    mc.tkey = tkey;
    mc.tval = tval;
    mc.ring_cap = w + 3u;
    sc_search s = {0};
    s.rgb = rgb;
    s.n = n;
    s.w = w;
    s.max_nodes = n < SC_PREV_RING ? n : SC_PREV_RING;
    s.node_cap = s.max_nodes < 512u ? s.max_nodes : 512u;
    s.bkt_cnt = 64u;
    s.bkt_lim = 48u;
    renc e;
    renc_init(&e, buf, cap);
    int oom = 0;
    if (sc2_encode_micro_b(&s, &e, &mc, cap, &oom)) {
        renc_flush(&e);
        if (e.pos < cap) {
            __builtin_memcpy(out, buf, e.pos);
            *written = e.pos;
        }
    }
    rf_free(mc.dyn_full);
    rf_free(mc.dyn_cache);
    rf_free(mc.dyn_ring);
    rf_free(s.dyn_nodes);
    rf_free(s.dyn_head);
    if (oom) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    return RF_OK;
}

/* Writes the method 6 payload when L >= 4 copies cover at least half the pixels and the payload is smaller than
   `cap` bytes; otherwise *written = 0 and `out` is untouched (the caller keeps its own choice). */
int rf_sc2_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap, size_t *written, rf_error *err) {
    *written = 0;
    if (cap <= 1u) return RF_OK;
    uint32_t n = w * h;
    if (n < SC_MIN_MATCH) return RF_OK;
    if (cap <= 64u) return sc2_encode_micro(rgb, w, n, out, cap, written, err);
    if (!sc_likely_half(rgb, w, n)) return RF_OK;

    uint16_t tkey_small[512];
    sc_ctr tval_small[512];
    int sparse_mc = cap <= 2048u;
    if (sparse_mc) __builtin_memset(tkey_small, 0, sizeof tkey_small);

    uint32_t bkt_cnt = cap <= 2048u ? SC_MICRO_BKT : (cap <= 8192u ? SC_TINY_BKT : 0u);
    sc_search s;
    s.rgb = rgb;
    s.n = n;
    s.w = w;
    s.node_cnt = 0;
    s.max_nodes = n < SC_PREV_RING ? n : SC_PREV_RING;
    uint32_t init_nodes = cap <= 2048u ? 2048u : (cap <= 8192u ? 8192u : (cap <= 65536u ? 16384u : 65536u));
    s.node_cap = s.max_nodes < init_nodes ? s.max_nodes : init_nodes;
    s.bkt_cnt = bkt_cnt;
    s.bkt_lim = (bkt_cnt * 3u) >> 2;
    s.tiny_used = 0;
    s.dyn_head = 0;
    s.dyn_nodes = 0;

    uint32_t ring_cap = w + 3u;
    size_t init_len = (size_t)s.node_cap;
    size_t off_cache = sparse_mc ? 0u : ((sizeof(sc2_model) + 7u) & ~(size_t)7u);
    size_t off_ring = off_cache + (((size_t)1u << SC2_CACHE_BITS) * sizeof(uint32_t));
    size_t off_head = ((off_ring + (size_t)ring_cap * sizeof(uint32_t)) + 7u) & ~(size_t)7u;
    size_t head_bytes = bkt_cnt ? (size_t)bkt_cnt * sizeof(uint64_t) : ((size_t)1u << SC_HASH_BITS) * sizeof(uint32_t);
    size_t off_pos = off_head + head_bytes;
    size_t off_prev = off_pos + init_len * sizeof(uint32_t);
    size_t off_tag = off_prev + init_len * sizeof(uint32_t);
    size_t off_buf = off_tag + init_len * sizeof(uint16_t);
    uint8_t *arena = (uint8_t *)rf_alloc(off_buf + cap);
    if (!arena) return rf_fail(err, RF_ERR_LIMIT, "out of memory");

    sc2_mctx mc = {0};
    mc.full = sparse_mc ? 0 : (sc2_model *)arena;
    mc.tkey = sparse_mc ? tkey_small : 0;
    mc.tval = sparse_mc ? tval_small : 0;
    mc.cache = (uint32_t *)(arena + off_cache);
    mc.orig_ring = (uint32_t *)(arena + off_ring);
    mc.ring_cap = ring_cap;

    s.head = bkt_cnt ? 0 : (uint32_t *)(arena + off_head);
    s.tiny_bkt = bkt_cnt ? (uint64_t *)(arena + off_head) : 0;
    s.npos = (uint32_t *)(arena + off_pos);
    s.nprev = (uint32_t *)(arena + off_prev);
    s.ntag = (uint16_t *)(arena + off_tag);
    uint8_t *buf = arena + off_buf;
    renc e;
    renc_init(&e, buf, cap);
    int oom = 0;
    if (sc2_encode_if_half(&s, &e, &mc, cap, &oom)) {
        renc_flush(&e);
        if (e.pos < cap) {
            __builtin_memcpy(out, buf, e.pos);
            *written = e.pos;
        }
    }
    rf_free(mc.dyn_full);
    rf_free(mc.dyn_cache);
    rf_free(mc.dyn_ring);
    rf_free(s.dyn_nodes);
    rf_free(s.dyn_head);
    rf_free(arena);
    if (oom) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    return RF_OK;
}

RF_INLINE int sc_get(rdec *d, sc_ctr *k) {
    if (d->in >= d->lim) rdec_tail(d);
    uint32_t cur = k->n ? k->c : 32768u, p = cur ? cur : 1u;
    int bit = rdec_bit16(d, p);
    sc_take(k, bit);
    return bit;
}

RF_INLINE int sc_get_half(rdec *d) {
    if (d->in >= d->lim) rdec_tail(d);
    return rdec_bit16(d, 32768u);
}

static uint32_t sc_get_tree(rdec *d, sc_ctr *tree, int levels) {
    uint32_t node = 1;
    for (int i = 0; i < levels; i++) node = 2u * node + (uint32_t)sc_get(d, &tree[node]);
    return node - (1u << levels);
}

static uint32_t sc_get_extra(rdec *d, sc_ctr *first, uint32_t nbits) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < nbits; i++) v = (v << 1) | (uint32_t)(i == 0 ? sc_get(d, first) : sc_get_half(d));
    return v;
}

static int sc_bad(rf_error *err) { return rf_fail(err, RF_ERR_FORMAT, "invalid fold: screen content stream"); }

/* Method 5 decoder (decode-only; preserved byte-for-byte for legacy method 5 files). */
static int sc_decode_m5(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, rf_error *err) {
    if (n < 5u || w == 0u || h == 0u) return sc_bad(err);
    size_t total = (size_t)w * h;
    sc_model *m = (sc_model *)rf_alloc(sizeof(sc_model));
    if (!m) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    rdec d;
    rdec_init(&d, in, n);
    uint32_t cache[1u << SC_CACHE_BITS];
    uint8_t cache_set[1u << SC_CACHE_BITS];
    __builtin_memset(cache_set, 0, sizeof cache_set);
    size_t row_off = stride ? 54u + (size_t)(h - 1u) * stride : 0u;
    uint32_t x = 0;
    int prev_kind = SC_LITERAL, prev_short = 0, bad = 0;
    for (size_t p = 0; p < total && !bad;) {
        if (sc_get(&d, &m->is_copy[prev_kind])) {
            uint32_t sym = sc_get_tree(&d, m->len_tree, 5);
            if (sym > 23u) { bad = 1; break; }
            uint32_t nb = sym < 4u ? 0u : (sym - 2u) >> 1;
            uint32_t len = sc_prefix_value(sym, sc_get_extra(&d, &m->len_extra[sym], nb)) + 3u;
            if (len > SC_MAX_MATCH || len > total - p) { bad = 1; break; }
            sym = sc_get_tree(&d, m->dist_tree[prev_short], 6);
            if (sym > 39u) { bad = 1; break; }
            nb = sym < 4u ? 0u : (sym - 2u) >> 1;
            uint32_t v = sc_prefix_value(sym, sc_get_extra(&d, &m->dist_extra[sym], nb));
            uint32_t dist = v <= 120u ? sc_map_distance(v, w) : v - 120u;
            if (dist > p) { bad = 1; break; }
            if (!stride) {
                for (size_t q = p; q < p + len; q++) {
                    uint8_t *dst = out + 3u * q;
                    const uint8_t *src = dst - 3u * (size_t)dist;
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                    uint32_t px = sc_pixel(dst), ci = sc_cache_index(px);
                    cache[ci] = px;
                    cache_set[ci] = 1;
                }
            } else {
                size_t sq = p - (size_t)dist;
                uint32_t sy = (uint32_t)(sq / w), sx = (uint32_t)(sq % w);
                size_t src_off = 54u + (size_t)(h - 1u - sy) * stride;
                for (uint32_t i = 0; i < len; i++) {
                    const uint8_t *src = out + src_off + 3u * (size_t)sx;
                    uint8_t *dst = out + row_off + 3u * (size_t)x;
                    uint8_t b = src[0], g = src[1], r = src[2];
                    dst[0] = b; dst[1] = g; dst[2] = r;
                    uint32_t px = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b, ci = sc_cache_index(px);
                    cache[ci] = px;
                    cache_set[ci] = 1;
                    if (++sx == w) { sx = 0; src_off -= stride; }
                    if (++x == w) { x = 0; row_off -= stride; }
                }
            }
            p += len;
            prev_kind = SC_COPY;
            prev_short = v <= 120u;
            continue;
        }
        uint32_t px;
        if (sc_get(&d, &m->is_cache[prev_kind])) {
            uint32_t ci = sc_get_tree(&d, m->cache_tree, 8);
            if (!cache_set[ci]) { bad = 1; break; }
            px = cache[ci];
            prev_kind = SC_CACHE;
        } else {
            uint32_t g = sc_get_tree(&d, m->lit_g, 8);
            uint32_t r = (sc_get_tree(&d, m->lit_rg[g >> 6], 8) + g) & 0xFFu;
            uint32_t b = (sc_get_tree(&d, m->lit_bg[g >> 6], 8) + g) & 0xFFu;
            px = (r << 16) | (g << 8) | b;
            prev_kind = SC_LITERAL;
        }
        if (!stride) {
            uint8_t *dst = out + 3u * p;
            dst[0] = (uint8_t)(px >> 16); dst[1] = (uint8_t)(px >> 8); dst[2] = (uint8_t)px;
        } else {
            uint8_t *dst = out + row_off + 3u * (size_t)x;
            dst[0] = (uint8_t)px; dst[1] = (uint8_t)(px >> 8); dst[2] = (uint8_t)(px >> 16);
            if (++x == w) { x = 0; row_off -= stride; }
        }
        uint32_t ci = sc_cache_index(px);
        cache[ci] = px;
        cache_set[ci] = 1;
        p++;
    }
    if (d.in >= d.lim) rdec_tail(&d);
    rf_free(m);
    if (bad || d.in != d.end) return sc_bad(err);
    return RF_OK;
}

RF_INLINE void sc2_dec_update_color(uint32_t *mtf, uint32_t *mtf_cnt, uint32_t *cache, uint32_t px) {
    if (*mtf_cnt > 0u && mtf[0] == px) return;
    uint32_t n = *mtf_cnt, pos = n < 16u ? n : 15u;
    int found = 0;
    for (uint32_t i = 1u; i < n; i++) {
        if (mtf[i] == px) { pos = i; found = 1; break; }
    }
    if (!found && n < 16u) *mtf_cnt = n + 1u;
    for (uint32_t i = pos; i > 0u; i--) mtf[i] = mtf[i - 1u];
    mtf[0] = px;
    cache[sc2_cache_index(px)] = 0xFF000000u | px;
}

/* Method 6 decoder: single zero-filled arena holds sc2_model, 2048-entry cache, and (w + 3)-entry orig_ring. */
static int sc_decode_m6(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, rf_error *err) {
    if (n < 5u || w == 0u || h == 0u) return sc_bad(err);
    size_t total = (size_t)w * h;
    uint32_t ring_cap = w + 3u;
    size_t off_cache = (sizeof(sc2_model) + 7u) & ~(size_t)7u;
    size_t off_ring = off_cache + (((size_t)1u << SC2_CACHE_BITS) * sizeof(uint32_t));
    uint8_t *arena = (uint8_t *)rf_alloc(off_ring + (size_t)ring_cap * sizeof(uint32_t));
    if (!arena) return rf_fail(err, RF_ERR_LIMIT, "out of memory");

    sc2_model *m = (sc2_model *)arena;
    uint32_t *cache = (uint32_t *)(arena + off_cache);
    uint32_t *orig_ring = (uint32_t *)(arena + off_ring);
    uint32_t mtf[16];
    uint32_t mtf_cnt = 0;
    uint32_t rep[4] = {1u, w, w + 1u, w - 1u};
    uint32_t rep_dy[4] = {1u, 2u, 4u, 8u};
    uint32_t cands[4];

    rdec d;
    rdec_init(&d, in, n);
    size_t row_off = stride ? 54u + (size_t)(h - 1u) * stride : 0u;
    uint32_t x = 0, y = 0, rpos = 0;

    uint32_t g0 = sc_get_tree(&d, m->lit_g[4], 8);
    uint32_t r0 = (sc_get_tree(&d, m->lit_rg[g0 >> 6], 8) + g0) & 0xFFu;
    uint32_t b0 = (sc_get_tree(&d, m->lit_bg[g0 >> 6], 8) + g0) & 0xFFu;
    uint32_t px0 = (r0 << 16) | (g0 << 8) | b0;
    if (!stride) {
        out[0] = (uint8_t)r0; out[1] = (uint8_t)g0; out[2] = (uint8_t)b0;
    } else {
        uint8_t *dst0 = out + row_off;
        dst0[0] = (uint8_t)b0; dst0[1] = (uint8_t)g0; dst0[2] = (uint8_t)r0;
    }
    orig_ring[0] = 0u;
    sc2_dec_update_color(mtf, &mtf_cnt, cache, px0);
    if (++x == w) { x = 0u; y = 1u; if (stride) row_off -= stride; }
    rpos = 1u;

    int prev_kind = SC2_LIT, prev_same_row = 0, bad = 0;
    for (size_t p = 1u; p < total && !bad;) {
        int ncand = sc2_gather_cands((uint32_t)p, w, x, y, rpos, ring_cap, orig_ring, rep, cands);
        if (sc_get(&d, &m->is_copy[prev_kind])) {
            int use_inh = sc_get(&d, &m->is_inherit[prev_kind]);
            uint32_t len = 0, dist = 0;
            if (use_inh) {
                int idx;
                if (ncand == 1 || sc_get(&d, &m->inh_b0[prev_kind])) idx = 0;
                else if (ncand == 2 || sc_get(&d, &m->inh_b1)) idx = 1;
                else if (ncand == 3 || sc_get(&d, &m->inh_b2)) idx = 2;
                else idx = 3;
                dist = cands[idx];
                uint32_t sym = sc_get_tree(&d, m->len_tree[1], 5);
                if (sym > 23u) { bad = 1; break; }
                uint32_t nb = sym < 4u ? 0u : (sym - 2u) >> 1;
                len = sc_prefix_value(sym, sc_get_extra(&d, &m->len_extra[1][sym], nb)) + 1u;
                if (len > SC_MAX_MATCH || len > total - p) { bad = 1; break; }
            } else {
                uint32_t sym = sc_get_tree(&d, m->len_tree[0], 5);
                if (sym > 23u) { bad = 1; break; }
                uint32_t nb = sym < 4u ? 0u : (sym - 2u) >> 1;
                len = sc_prefix_value(sym, sc_get_extra(&d, &m->len_extra[0][sym], nb)) + 3u;
                if (len > SC_MAX_MATCH || len > total - p) { bad = 1; break; }
                int same_row;
                if (y == 0u) same_row = 1;
                else if (x == 0u) same_row = 0;
                else same_row = sc_get(&d, &m->is_same_row[prev_same_row]);
                if (same_row) {
                    sym = sc_get_tree(&d, m->dx0_tree, 6);
                    if (sym > 39u) { bad = 1; break; }
                    nb = sym < 4u ? 0u : (sym - 2u) >> 1;
                    uint32_t dx = sc_prefix_value(sym, sc_get_extra(&d, &m->dx0_extra[sym], nb));
                    if (dx > x || dx > SC2_WINDOW) { bad = 1; break; }
                    dist = dx;
                    prev_same_row = 1;
                } else {
                    int in_rdy = sc_get(&d, &m->is_rep_dy);
                    uint32_t dy;
                    if (in_rdy) {
                        int ridx;
                        if (sc_get(&d, &m->rep_dy_b0)) ridx = 0;
                        else if (sc_get(&d, &m->rep_dy_b1[0])) ridx = 1;
                        else if (sc_get(&d, &m->rep_dy_b1[1])) ridx = 2;
                        else ridx = 3;
                        dy = rep_dy[ridx];
                    } else {
                        sym = sc_get_tree(&d, m->dy_tree, 6);
                        if (sym > 39u) { bad = 1; break; }
                        nb = sym < 4u ? 0u : (sym - 2u) >> 1;
                        dy = sc_prefix_value(sym, sc_get_extra(&d, &m->dy_extra[sym], nb));
                    }
                    if (dy > y || dy > SC2_WINDOW) { bad = 1; break; }
                    sc2_mtf4(rep_dy, dy);
                    int64_t dx;
                    if (sc_get(&d, &m->dx_zero[in_rdy])) {
                        dx = 0;
                    } else {
                        int neg = sc_get(&d, &m->dx_sign);
                        sym = sc_get_tree(&d, m->dx_tree, 6);
                        if (sym > 49u) { bad = 1; break; }
                        nb = sym < 4u ? 0u : (sym - 2u) >> 1;
                        int64_t adx = (int64_t)sc_prefix_value(sym, sc_get_extra(&d, &m->dx_extra[sym], nb));
                        dx = neg ? -adx : adx;
                    }
                    int64_t qx = (int64_t)x - dx;
                    if (qx < 0 || qx >= (int64_t)w) { bad = 1; break; }
                    int64_t dist64 = (int64_t)dy * (int64_t)w + dx;
                    if (dist64 < 1 || dist64 > (int64_t)SC2_WINDOW) { bad = 1; break; }
                    dist = (uint32_t)dist64;
                    prev_same_row = 0;
                }
            }

            sc2_ring_fill(orig_ring, ring_cap, (uint32_t)p, len, dist);
            if (!stride) {
                if (dist == 1u) {
                    const uint8_t *src = out + 3u * (p - 1u);
                    uint8_t r = src[0], g = src[1], b = src[2];
                    uint32_t px = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                    sc2_dec_update_color(mtf, &mtf_cnt, cache, px);
                    uint8_t *dst = out + 3u * p;
                    for (uint32_t i = 0; i < len; i++, dst += 3u) {
                        dst[0] = r; dst[1] = g; dst[2] = b;
                    }
                } else {
                    for (size_t q = p; q < p + len; q++) {
                        uint8_t *dst = out + 3u * q;
                        const uint8_t *src = dst - 3u * (size_t)dist;
                        uint8_t r = src[0], g = src[1], b = src[2];
                        dst[0] = r; dst[1] = g; dst[2] = b;
                        sc2_dec_update_color(mtf, &mtf_cnt, cache, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
                    }
                }
                p += len;
                y = (uint32_t)(p / w);
                x = (uint32_t)(p - (size_t)y * w);
            } else {
                size_t sq = p - (size_t)dist;
                uint32_t sy = (uint32_t)(sq / w), sx = (uint32_t)(sq % w);
                size_t src_off = 54u + (size_t)(h - 1u - sy) * stride;
                for (uint32_t i = 0; i < len; i++) {
                    const uint8_t *src = out + src_off + 3u * (size_t)sx;
                    uint8_t *dst = out + row_off + 3u * (size_t)x;
                    uint8_t b = src[0], g = src[1], r = src[2];
                    dst[0] = b; dst[1] = g; dst[2] = r;
                    sc2_dec_update_color(mtf, &mtf_cnt, cache, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
                    if (++sx == w) { sx = 0; src_off -= stride; }
                    if (++x == w) { x = 0; y++; row_off -= stride; }
                }
                p += len;
            }
            rpos = (uint32_t)(p % ring_cap);
            sc2_mtf4(rep, dist);
            prev_kind = SC2_COPY;
            continue;
        }

        int has_w = x >= 1u, has_n = y >= 1u;
        uint32_t w_rgb = 0u, n_rgb = 0u;
        if (!stride) {
            if (has_w) w_rgb = sc_pixel(out + 3u * (p - 1u));
            if (has_n) n_rgb = sc_pixel(out + 3u * (p - w));
        } else {
            const uint8_t *dst = out + row_off + 3u * (size_t)x;
            if (has_w) w_rgb = sc_bgr_pixel(dst - 3u);
            if (has_n) n_rgb = sc_bgr_pixel(dst + stride);
        }

        uint32_t px = 0u;
        int tk = SC2_LIT, decoded_nb = 0;
        if (has_w) {
            int wn_eq = has_n && w_rgb == n_rgb;
            if (sc_get(&d, &m->is_eq_w[prev_kind * 2 + wn_eq])) {
                px = w_rgb;
                tk = SC2_EQ_W;
                decoded_nb = 1;
            }
        }
        if (!decoded_nb && has_n && (!has_w || n_rgb != w_rgb)) {
            if (sc_get(&d, &m->is_eq_n[prev_kind])) {
                px = n_rgb;
                tk = SC2_EQ_N;
                decoded_nb = 1;
            }
        }
        if (!decoded_nb) {
            if (sc_get(&d, &m->is_mtf[prev_kind])) {
                uint32_t midx = sc_get_tree(&d, m->mtf_tree, 4);
                if (midx >= mtf_cnt) { bad = 1; break; }
                px = mtf[midx];
                tk = SC2_MTF;
            } else if (sc_get(&d, &m->is_cache[prev_kind])) {
                uint32_t ci = sc_get_tree(&d, m->cache_tree, SC2_CACHE_BITS);
                uint32_t cv = cache[ci];
                if (!cv) { bad = 1; break; }
                px = cv & 0x00FFFFFFu;
                tk = SC2_CACHE;
            } else {
                uint32_t wg_ctx = has_w ? (((w_rgb >> 8) & 0xFFu) >> 5) : 4u;
                uint32_t g = sc_get_tree(&d, m->lit_g[wg_ctx], 8);
                uint32_t r = (sc_get_tree(&d, m->lit_rg[g >> 6], 8) + g) & 0xFFu;
                uint32_t b = (sc_get_tree(&d, m->lit_bg[g >> 6], 8) + g) & 0xFFu;
                px = (r << 16) | (g << 8) | b;
                tk = SC2_LIT;
            }
        }

        if (!stride) {
            uint8_t *dst = out + 3u * p;
            dst[0] = (uint8_t)(px >> 16); dst[1] = (uint8_t)(px >> 8); dst[2] = (uint8_t)px;
            if (++x == w) { x = 0u; y++; }
        } else {
            uint8_t *dst = out + row_off + 3u * (size_t)x;
            dst[0] = (uint8_t)px; dst[1] = (uint8_t)(px >> 8); dst[2] = (uint8_t)(px >> 16);
            if (++x == w) { x = 0u; y++; row_off -= stride; }
        }
        orig_ring[rpos] = 0u;
        sc2_dec_update_color(mtf, &mtf_cnt, cache, px);
        p++;
        if (++rpos == ring_cap) rpos = 0u;
        prev_kind = tk;
    }

    if (d.in >= d.lim) rdec_tail(&d);
    rf_free(arena);
    if (bad || d.in != d.end) return sc_bad(err);
    return RF_OK;
}

/* Decodes directly into `out`: packed RGB when stride == 0, else bottom-up BGR rows of `stride` bytes at out + 54. */
int rf_sc_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, uint32_t method, rf_error *err) {
    if (method == RF_METHOD_SC) return sc_decode_m5(in, n, out, stride, w, h, err);
    if (method == RF_METHOD_SC2) return sc_decode_m6(in, n, out, stride, w, h, err);
    return rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported fold: method");
}
