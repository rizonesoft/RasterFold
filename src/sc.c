/* Method 5: screen content as literals, colour-cache hits and backward-reference copies (version 3 only).
   spec/FOLD.md ("Method 5, screen content") is the normative description; research/screencode.py is the
   prototype this file reproduces byte for byte. */
#include "rf.h"
#include "rc.h"

#define SC_MIN_MATCH 4u
#define SC_MAX_MATCH 4096u
#define SC_CHAIN 64u
#define SC_WINDOW ((1u << 20) - 120u)
#define SC_PREV_RING (1u << 20)
#define SC_PREV_MASK (SC_PREV_RING - 1u)
#define SC_HASH_BITS 16u
#define SC_CACHE_BITS 8u

enum { SC_LITERAL = 0, SC_CACHE = 1, SC_COPY = 2 };

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

typedef struct {
    sc_ctr is_copy[3], is_cache[3];
    sc_ctr lit_g[256], lit_rg[4][256], lit_bg[4][256];
    sc_ctr cache_tree[256];
    sc_ctr len_tree[32], len_extra[32];
    sc_ctr dist_tree[2][64], dist_extra[64];
} sc_model;

static const uint16_t sc_rate[61] = {
    43690, 26214, 18724, 14563, 11915, 10082, 8738, 7710, 6898, 6241, 5698, 5242, 4854, 4519, 4228, 3971,
    3744, 3542, 3360, 3196, 3048, 2912, 2788, 2674, 2570, 2473, 2383, 2299, 2221, 2148, 2080, 2016,
    1956, 1899, 1846, 1795, 1747, 1702, 1659, 1618, 1579, 1542, 1506, 1472, 1440, 1409, 1379, 1351,
    1323, 1297, 1272, 1248, 1224, 1202, 1180, 1159, 1139, 1120, 1101, 1083, 1065,
};

typedef struct {
    sc_model *full, *dyn_full;
    uint16_t *tkey;
    sc_ctr *tval;
    uint32_t tused;
} sc_mctx;

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
RF_INLINE sc_ctr *sc_at(sc_mctx *mc, size_t idx) {
    if (mc->full) return &((sc_ctr *)mc->full)[idx];
    uint32_t key = (uint32_t)idx + 1u;
    for (uint32_t i = (key * 0x9E37u) & 511u;; i = (i + 1u) & 511u) {
        if (mc->tkey[i] == key) return &mc->tval[i];
        if (!mc->tkey[i]) { mc->tused++; mc->tkey[i] = (uint16_t)key; mc->tval[i] = (sc_ctr){0, 0}; return &mc->tval[i]; }
    }
}
static int sc_mctx_ensure(sc_mctx *mc) {
    if (mc->full || mc->tused < 360u) return 1;
    sc_model *m = (sc_model *)rf_alloc(sizeof(sc_model));
    if (!m) return 0;
    for (uint32_t i = 0; i < 512u; i++)
        if (mc->tkey[i]) ((sc_ctr *)m)[mc->tkey[i] - 1u] = mc->tval[i];
    mc->full = mc->dyn_full = m;
    return 1;
}
#define SC_OFF(field) (__builtin_offsetof(sc_model, field) / sizeof(sc_ctr))
RF_INLINE void sc_put(renc *e, sc_ctr *k, int bit) {
    uint32_t p = sc_take(k, bit);
    renc_bit16(e, bit, p);
}
static void sc_put_tree(renc *e, sc_mctx *mc, size_t base, uint32_t value, int levels) {
    uint32_t node = 1;
    for (int i = levels - 1; i >= 0; i--) {
        int bit = (int)((value >> i) & 1u);
        sc_put(e, sc_at(mc, base + node), bit);
        node = 2u * node + (uint32_t)bit;
    }
}
static void sc_put_extra(renc *e, sc_ctr *first, uint32_t nbits, uint32_t value) {
    for (uint32_t i = nbits; i-- > 0;) {
        int bit = (int)((value >> i) & 1u);
        if (i == nbits - 1u) sc_put(e, first, bit);
        else renc_bit16(e, bit, 32768u);
    }
}

/* RFC 9649 section 3.6.2.2: symbol, extra bit count and extra bits for a value >= 1. */
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
RF_INLINE uint32_t sc_pixel(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
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
static uint32_t sc_distance_code(uint32_t dist, uint32_t w) {
    if (dist > 7u * w + 8u) return dist + 120u;
    for (uint32_t v = 1; v <= 120u; v++)
        if (sc_map_distance(v, w) == dist) return v;
    return dist + 120u;
}

#define SC_MICRO_BKT 256u
#define SC_TINY_BKT 4096u

/* `head` (or `tiny_bkt` for `cap <= 4096`) and `nprev` store 1-based node ids (`0` = empty).
   At most one node is inserted per pixel in strictly increasing `q`, so `p - uq > node_cnt - id`
   and `SC_PREV_RING = 2^20 > SC_WINDOW` never overwrites an in-window node. */
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

RF_INLINE uint32_t sc_match_len(const uint8_t *a, const uint8_t *b, uint32_t limit) {
    if (!sc_eq12(a, b)) return 0;
    size_t lim_bytes = 3u * (size_t)limit;
    size_t off = 12u;
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

/* The frozen search: candidates distance 1, distance w, then up to 64 entries of the bucket chain within the
   window; the longest match of 4..4096 pixels, the shorter distance on a tie. Returns the length (0: none). */
static uint32_t sc_find(const sc_search *s, uint32_t p, uint32_t *dist_out) {
    uint32_t limit = s->n - p < SC_MAX_MATCH ? s->n - p : SC_MAX_MATCH;
    if (limit < SC_MIN_MATCH) return 0;
    const uint8_t *cur = s->rgb + 3u * (size_t)p;
    uint32_t best = 0, best_d = 0;
    if (p >= 1u) {
        uint32_t len = sc_match_len(cur, cur - 3u, limit);
        if (len >= SC_MIN_MATCH) {
            best = len;
            best_d = 1u;
            if (best == limit) { *dist_out = 1u; return best; }
        }
    }
    if (p >= s->w && s->w > 1u && s->w <= SC_WINDOW) {
        const uint8_t *b = cur - 3u * (size_t)s->w;
        if (best < SC_MIN_MATCH || (best < limit && sc_eq3(cur + 3u * (size_t)best, b + 3u * (size_t)best))) {
            uint32_t len = sc_match_len(cur, b, limit);
            if (len >= SC_MIN_MATCH && len > best) { best = len; best_d = s->w; }
        }
    }
    uint32_t h = sc_fnv1a32(cur);
    uint16_t tag16 = (uint16_t)h;
    uint32_t id = sc_head_get(s, h >> (32u - SC_HASH_BITS));
    for (uint32_t visited = 0; id != 0u && visited < SC_CHAIN; visited++) {
        if (s->node_cnt - id >= SC_PREV_RING) break;
        uint32_t slot = (id - 1u) & SC_PREV_MASK;
        uint32_t uq = s->npos[slot], d = p - uq;
        if (d > SC_WINDOW || (best == limit && d >= best_d)) break;
        uint32_t next_id = s->nprev[slot];
        if (s->ntag[slot] == tag16) {
            const uint8_t *b = s->rgb + 3u * (size_t)uq;
            uint32_t need_idx = best < SC_MIN_MATCH ? 0u : (d < best_d ? best - 1u : best);
            if (need_idx < limit && (best < SC_MIN_MATCH || sc_eq3(cur + 3u * (size_t)need_idx, b + 3u * (size_t)need_idx))) {
                uint32_t len = sc_match_len(cur, b, limit);
                if (len >= SC_MIN_MATCH && (len > best || (len == best && d < best_d))) { best = len; best_d = d; }
            }
        }
        id = next_id;
    }
    if (best < SC_MIN_MATCH) return 0;
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

/* Necessary-condition prefilter with zero false negatives: every copy [p, p + L) is covered by the 4-pixel
   intervals [q, q + 4) of duplicate 12-byte keys q in [p, p + L - 3), so covered <= |union of duplicate [q, q + 4)|. */
__attribute__((noinline)) static int sc_likely_half(const uint8_t *rgb, uint32_t w, uint32_t n) {
    if (n < SC_MIN_MATCH) return 0;
    uint32_t half_need = (uint32_t)(((uint64_t)n + 1u) >> 1);
    uint32_t last = n - SC_MIN_MATCH;
    uint32_t adj_cov = 0, adj_rem = 0;
    for (uint32_t q = 1u; q <= last; q++) {
        const uint8_t *cur = rgb + 3u * (size_t)q;
        if (sc_eq12(cur, cur - 3u) || (q >= w && w > 1u && w <= SC_WINDOW && sc_eq12(cur, cur - 3u * (size_t)w))) {
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

/* Single-pass parse and range coding: aborts as soon as covered * 2 >= n becomes impossible or flushed output reaches cap. */
static RF_NOINLINE int sc_encode_if_half(sc_search *s, renc *e, sc_mctx *mc, size_t cap, int *oom) {
    uint32_t cache[1u << SC_CACHE_BITS];
    uint8_t cache_set[1u << SC_CACHE_BITS];
    __builtin_memset(cache_set, 0, sizeof cache_set);
    int prev_kind = SC_LITERAL, prev_short = 0;
    uint32_t covered = 0;
    for (uint32_t p = 0; p < s->n;) {
        if ((uint64_t)(p - covered) * 2u > (uint64_t)s->n || e->pos + (size_t)e->pending + 4u >= cap) return 0;
        if (!sc_mctx_ensure(mc)) { *oom = 1; return 0; }
        uint32_t dist = 0, len = sc_find(s, p, &dist);
        if (len) {
            sc_put(e, sc_at(mc, SC_OFF(is_copy) + (size_t)prev_kind), 1);
            uint32_t nb, ex, sym = sc_prefix(len - 3u, &nb, &ex);
            sc_put_tree(e, mc, SC_OFF(len_tree), sym, 5);
            sc_put_extra(e, sc_at(mc, SC_OFF(len_extra) + sym), nb, ex);
            uint32_t v = sc_distance_code(dist, s->w);
            sym = sc_prefix(v, &nb, &ex);
            sc_put_tree(e, mc, SC_OFF(dist_tree) + (size_t)prev_short * 64u, sym, 6);
            sc_put_extra(e, sc_at(mc, SC_OFF(dist_extra) + sym), nb, ex);
            prev_short = v <= 120u;
            covered += len;
            uint32_t end = p + len;
            if (end == s->n) return (uint64_t)covered * 2u >= (uint64_t)s->n;
            if (e->pos + (size_t)e->pending + 4u >= cap) return 0;
            for (uint32_t q = p; q < end;) {
                const uint8_t *cur = s->rgb + 3u * (size_t)q;
                uint32_t px = sc_pixel(cur), ci = sc_cache_index(px);
                cache[ci] = px;
                cache_set[ci] = 1;
                uint32_t rem = end - q;
                if (rem >= 5u && (dist == 1u || sc_eq12(cur + 3u, cur))) {
                    /* Pixels q .. q + run share one RGB triple, so 4-pixel keys in [q, q + run - 2) share one hash
                       and `sc_find` never walks past the most recent SC_CHAIN entries of that bucket. */
                    uint32_t run = dist == 1u ? rem - 1u : sc_match_len(cur + 3u, cur, rem - 1u);
                    uint32_t const_end = q + run - 2u;
                    uint32_t q0 = const_end - q > SC_CHAIN ? const_end - SC_CHAIN : q;
                    uint32_t tail_end = q + run + 1u;
                    if (!sc_ensure_nodes(s, tail_end - q0)) { *oom = 1; return 0; }
                    uint32_t h = sc_fnv1a32(cur);
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
                if (rem >= 6u && (dist == 2u || sc_eq12(cur + 6u, cur))) {
                    /* Pixels q .. q + run + 1 alternate two RGB triples, so [q, q + run - 1) alternates two buckets. */
                    uint32_t run = dist == 2u ? rem - 2u : sc_match_len(cur + 6u, cur, rem - 2u);
                    uint32_t px1 = sc_pixel(cur + 3u), ci1 = sc_cache_index(px1);
                    cache[ci1] = px1;
                    cache_set[ci1] = 1;
                    if (run & 1u) cache[ci] = px;
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
            prev_kind = SC_COPY;
            continue;
        }
        uint32_t px = sc_pixel(s->rgb + 3u * (size_t)p), ci = sc_cache_index(px);
        int hit = cache_set[ci] && cache[ci] == px;
        sc_put(e, sc_at(mc, SC_OFF(is_copy) + (size_t)prev_kind), 0);
        sc_put(e, sc_at(mc, SC_OFF(is_cache) + (size_t)prev_kind), hit);
        if (hit) {
            sc_put_tree(e, mc, SC_OFF(cache_tree), ci, 8);
        } else {
            uint32_t r = px >> 16, g = (px >> 8) & 0xFFu, b = px & 0xFFu;
            sc_put_tree(e, mc, SC_OFF(lit_g), g, 8);
            sc_put_tree(e, mc, SC_OFF(lit_rg) + (size_t)(g >> 6) * 256u, (r - g) & 0xFFu, 8);
            sc_put_tree(e, mc, SC_OFF(lit_bg) + (size_t)(g >> 6) * 256u, (b - g) & 0xFFu, 8);
        }
        prev_kind = hit ? SC_CACHE : SC_LITERAL;
        if (p + 1u == s->n) return (uint64_t)covered * 2u >= (uint64_t)s->n;
        if (e->pos + (size_t)e->pending + 4u >= cap) return 0;
        if (!sc_ensure_nodes(s, 1u) || !sc_insert(s, p)) { *oom = 1; return 0; }
        cache[ci] = px;
        cache_set[ci] = 1;
        p++;
    }
    return (uint64_t)covered * 2u >= (uint64_t)s->n;
}

/* Each frame stays well under 4096 stack bytes so freestanding Windows x64 never emits ___chkstk_ms,
   and `cap <= 64` encodes run with zero heap allocations unless they outgrow the micro tables. */
static RF_NOINLINE int sc_encode_micro_a(sc_search *s, renc *e, sc_mctx *mc, size_t cap, int *oom) {
    uint32_t npos[512];
    uint16_t ntag[512];
    s->npos = npos;
    s->ntag = ntag;
    int ok = sc_encode_if_half(s, e, mc, cap, oom);
    if (!s->dyn_nodes) { s->npos = 0; s->ntag = 0; }
    return ok;
}
static RF_NOINLINE int sc_encode_micro_b(sc_search *s, renc *e, sc_mctx *mc, size_t cap, int *oom) {
    uint64_t tiny_bkt[64];
    uint32_t nprev[512];
    __builtin_memset(tiny_bkt, 0, sizeof tiny_bkt);
    s->tiny_bkt = tiny_bkt;
    s->nprev = nprev;
    int ok = sc_encode_micro_a(s, e, mc, cap, oom);
    if (!s->dyn_nodes) s->nprev = 0;
    if (!s->dyn_head) s->tiny_bkt = 0;
    return ok;
}
static RF_NOINLINE int sc_encode_micro(const uint8_t *rgb, uint32_t w, uint32_t n, uint8_t *out, size_t cap, size_t *written, rf_error *err) {
    uint16_t tkey[512];
    sc_ctr tval[512];
    uint8_t buf[64];
    __builtin_memset(tkey, 0, sizeof tkey);
    sc_mctx mc = {0, 0, tkey, tval, 0};
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
    if (sc_encode_micro_b(&s, &e, &mc, cap, &oom)) {
        renc_flush(&e);
        if (e.pos < cap) {
            __builtin_memcpy(out, buf, e.pos);
            *written = e.pos;
        }
    }
    rf_free(mc.dyn_full);
    rf_free(s.dyn_nodes);
    rf_free(s.dyn_head);
    if (oom) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    return RF_OK;
}

/* Writes the method 5 payload when copies cover at least half the pixels and the payload is smaller than
   `cap` bytes; otherwise *written = 0 and `out` is untouched (the caller keeps its own choice). */
int rf_sc_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap, size_t *written, rf_error *err) {
    *written = 0;
    if (cap <= 1u) return RF_OK;
    uint32_t n = w * h;
    if (n < SC_MIN_MATCH) return RF_OK;
    if (cap <= 64u) return sc_encode_micro(rgb, w, n, out, cap, written, err);
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
    size_t init_len = (size_t)s.node_cap;
    size_t off_head = sparse_mc ? 0u : ((sizeof(sc_model) + 7u) & ~(size_t)7u);
    size_t head_bytes = bkt_cnt ? (size_t)bkt_cnt * sizeof(uint64_t) : ((size_t)1u << SC_HASH_BITS) * sizeof(uint32_t);
    size_t off_pos = off_head + head_bytes;
    size_t off_prev = off_pos + init_len * sizeof(uint32_t);
    size_t off_tag = off_prev + init_len * sizeof(uint32_t);
    size_t off_buf = off_tag + init_len * sizeof(uint16_t);
    uint8_t *arena = (uint8_t *)rf_alloc(off_buf + cap);
    if (!arena) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    sc_mctx mc = {sparse_mc ? 0 : (sc_model *)arena, 0, sparse_mc ? tkey_small : 0, sparse_mc ? tval_small : 0, 0};
    s.head = bkt_cnt ? 0 : (uint32_t *)(arena + off_head);
    s.tiny_bkt = bkt_cnt ? (uint64_t *)(arena + off_head) : 0;
    s.npos = (uint32_t *)(arena + off_pos);
    s.nprev = (uint32_t *)(arena + off_prev);
    s.ntag = (uint16_t *)(arena + off_tag);
    uint8_t *buf = arena + off_buf;
    renc e;
    renc_init(&e, buf, cap);
    int oom = 0;
    if (sc_encode_if_half(&s, &e, &mc, cap, &oom)) {
        renc_flush(&e);
        if (e.pos < cap) {
            __builtin_memcpy(out, buf, e.pos);
            *written = e.pos;
        }
    }
    rf_free(mc.dyn_full);
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

/* Decodes directly into `out`: packed RGB when stride == 0, else bottom-up BGR rows of `stride` bytes at out + 54. */
int rf_sc_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, rf_error *err) {
    if (n < 5u) return sc_bad(err);
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
