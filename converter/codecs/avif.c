/* Shipped C11 codec session adapter for D10 T02 s4:
 *   - RFC_FORMAT_AVIF (bundled libavif 1.4.2 + libaom 3.15.3 + libsharpyuv):
 *     lossless full-range 4:4:4 GBR Identity AV1 encode across 8/10/12-bit
 *     RGB/RGBA (retaining auxiliary alpha even when all-opaque), still AVIF
 *     decode/probe across 8/10/12-bit YUV400/420/422/444 and grid tiles,
 *     ICC/CICP/cLLI/mDCV colorimetry + irot/imir + Exif + XMP handling,
 *     zero-allocation ISOBMFF preflight, immediate decoded-sample equality
 *     verification on encode, and rfc_budget reservation + cancellation. */

#include "rfc_codecs.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <avif/avif.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Allocator, limits, cancellation, arithmetic, and endian helpers          */
/* ------------------------------------------------------------------------ */

static void *avif_stdlib_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void avif_stdlib_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const rfc_allocator k_avif_stdlib_alloc = {
    avif_stdlib_alloc, avif_stdlib_free, NULL
};

static const rfc_allocator *avif_alloc_of(const rfc_exec_context *exec) {
    const rfc_allocator *a = rfc_exec_allocator(exec);
    return (a && a->alloc && a->free) ? a : &k_avif_stdlib_alloc;
}

static const rfc_limits *avif_limits_of(const rfc_exec_context *exec,
                                        rfc_limits *scratch) {
    const rfc_limits *l = rfc_exec_limits(exec);
    if (l) {
        return l;
    }
    rfc_limits_default(scratch);
    return scratch;
}

static size_t avif_meta_cap_of(const rfc_limits *limits) {
    uint64_t cap = RFC_MAX_ICC_BYTES;
    if (limits) {
        cap = (limits->max_metadata_bytes < cap) ? limits->max_metadata_bytes
                                                 : cap;
    }
    if (cap > SIZE_MAX) {
        cap = SIZE_MAX;
    }
    return (size_t)cap;
}

typedef struct rfc_avif_session_box {
    atomic_int cancelled;
} rfc_avif_session_box;

static int avif_is_cancelled(const void *session,
                             const rfc_exec_context *exec) {
    const rfc_avif_session_box *s = (const rfc_avif_session_box *)session;
    if (s && atomic_load_explicit(&s->cancelled, memory_order_acquire) != 0) {
        return 1;
    }
    return exec ? rfc_exec_is_cancelled(exec) : 0;
}

typedef struct avif_budget_guard {
    rfc_budget *budget;
    size_t bytes;
    int active;
} avif_budget_guard;

static rfc_status avif_reserve_budget(const rfc_exec_context *exec,
                                      size_t bytes,
                                      avif_budget_guard *out) {
    memset(out, 0, sizeof(*out));
    rfc_budget *b = rfc_exec_budget(exec);
    if (!b || bytes == 0u) {
        return RFC_OK;
    }
    rfc_status st = rfc_budget_reserve(b, bytes);
    if (st != RFC_OK) {
        return st;
    }
    out->budget = b;
    out->bytes = bytes;
    out->active = 1;
    return RFC_OK;
}

static void avif_release_budget(avif_budget_guard *g) {
    if (g && g->active && g->budget) {
        rfc_budget_release(g->budget, g->bytes);
        g->budget = NULL;
        g->bytes = 0u;
        g->active = 0;
    }
}

static int avif_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0u && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static int avif_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static uint16_t avif_rd16be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t avif_rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t avif_rd64be(const uint8_t *p) {
    return ((uint64_t)avif_rd32be(p) << 32) | (uint64_t)avif_rd32be(p + 4u);
}

static uint64_t avif_rdu_be(const uint8_t *p, uint8_t sz) {
    uint64_t v = 0u;
    for (uint8_t i = 0u; i < sz; i++) {
        v = (v << 8) | (uint64_t)p[i];
    }
    return v;
}

static void avif_wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static void avif_wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static int avif_stage_work_bytes(uint64_t pixels,
                                 uint32_t bps,
                                 int has_alpha,
                                 uint32_t threads,
                                 int is_encode,
                                 size_t meta_bytes,
                                 size_t *out_bytes) {
    uint32_t eff_thr = threads ? (threads > 64u ? 64u : threads) : 1u;
    uint64_t pix = pixels ? pixels : 1ull;
    uint64_t plane_bytes = 0u, dec_scaled = 0u, mult_bytes = 0u, thr_bytes = 0u,
             meta_scaled = 0u, sum = 0u;
    uint64_t bpc = (bps > 8u) ? 2ull : 1ull;
    uint64_t planes = has_alpha ? 4ull : 3ull;
    uint64_t fixed = is_encode ? (15ull * 1024ull * 1024ull)
                               : (7ull * 1024ull * 1024ull);
    uint64_t per_pix_mult =
        is_encode ? 44ull : (16ull + 8ull * (uint64_t)eff_thr);
    /* Leave 1 * decoded_bytes + 1 MiB headroom inside job->budget for frame
     * and container copies billed alongside internal libavif/libaom heaps. */
    if (!avif_mul_u64(pix, bpc * planes, &plane_bytes) ||
        !avif_mul_u64(plane_bytes, 3ull, &dec_scaled) ||
        !avif_mul_u64(pix, per_pix_mult, &mult_bytes) ||
        !avif_mul_u64((uint64_t)eff_thr, 8ull * 1024ull * 1024ull,
                      &thr_bytes) ||
        !avif_mul_u64((uint64_t)meta_bytes, 2ull, &meta_scaled) ||
        !avif_add_u64(dec_scaled, mult_bytes, &sum) ||
        !avif_add_u64(sum, thr_bytes, &sum) ||
        !avif_add_u64(sum, fixed, &sum) ||
        !avif_add_u64(sum, meta_scaled, &sum) ||
        sum > (uint64_t)SIZE_MAX) {
        return 0;
    }
    *out_bytes = (size_t)sum;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Zero-allocation ISOBMFF preflight scanner                                */
/* ------------------------------------------------------------------------ */

typedef struct isobmff_box_view {
    uint8_t type[4];
    size_t pay_off;
    size_t pay_len;
    size_t next_off;
} isobmff_box_view;

static rfc_status isobmff_next_box(const uint8_t *data,
                                   size_t len,
                                   size_t pos,
                                   isobmff_box_view *out) {
    if (pos >= len) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (len - pos < 8u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    uint64_t sz = (uint64_t)avif_rd32be(data + pos);
    memcpy(out->type, data + pos + 4u, 4u);
    size_t hdr = 8u;
    if (sz == 1u) {
        if (len - pos < 16u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        sz = avif_rd64be(data + pos + 8u);
        hdr = 16u;
    }
    if (memcmp(out->type, "uuid", 4) == 0) {
        if (len - pos < hdr + 16u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        hdr += 16u;
    }
    if (sz == 0u) {
        out->pay_off = pos + hdr;
        out->pay_len = len - out->pay_off;
        out->next_off = len;
        return RFC_OK;
    }
    if (sz < (uint64_t)hdr) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (sz > (uint64_t)(len - pos)) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    out->pay_off = pos + hdr;
    out->pay_len = (size_t)sz - hdr;
    out->next_off = pos + (size_t)sz;
    return RFC_OK;
}

static int iloc_field_valid(uint8_t n) {
    return n == 0u || n == 4u || n == 8u;
}

#define AVIF_PREFLIGHT_MAX_OPAQUE_BYTES (256ull * 1024ull)
#define AVIF_PREFLIGHT_MAX_IDAT_BYTES (1024ull * 1024ull)
#define AVIF_PREFLIGHT_MAX_ITEMS 1024u
#define AVIF_PREFLIGHT_MAX_ASSOCS 8192ull
#define AVIF_PREFLIGHT_MAX_EXTENTS 8192ull
#define AVIF_PREFLIGHT_MAX_PROPS 1024ull
#define AVIF_PREFLIGHT_MAX_RAW_IDS 131072u
#define AVIF_PREFLIGHT_MAX_STRUCT_META_BYTES (4ull * 1024ull * 1024ull)

typedef struct avif_u32_vec {
    uint32_t stack_buf[64];
    uint32_t *heap_buf;
    size_t count;
    size_t cap;
    const rfc_allocator *alloc;
    int sorted;
} avif_u32_vec;

static void avif_u32_vec_init(avif_u32_vec *v, const rfc_allocator *alloc) {
    v->heap_buf = NULL;
    v->count = 0u;
    v->cap = sizeof(v->stack_buf) / sizeof(v->stack_buf[0]);
    v->alloc = alloc;
    v->sorted = 0;
}

static void avif_u32_vec_free(avif_u32_vec *v) {
    if (v->heap_buf && v->alloc && v->alloc->free) {
        v->alloc->free(v->alloc->ctx, v->heap_buf);
    }
    v->heap_buf = NULL;
    v->count = 0u;
    v->cap = 0u;
}

static uint32_t *avif_u32_vec_data(avif_u32_vec *v) {
    return v->heap_buf ? v->heap_buf : v->stack_buf;
}

static const uint32_t *avif_u32_vec_cdata(const avif_u32_vec *v) {
    return v->heap_buf ? v->heap_buf : v->stack_buf;
}

static rfc_status avif_u32_vec_push(avif_u32_vec *v, uint32_t val) {
    uint32_t *arr = avif_u32_vec_data(v);
    if (v->count == v->cap) {
        if (!v->alloc || !v->alloc->alloc || !v->alloc->free) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        if (v->cap > SIZE_MAX / (2u * sizeof(uint32_t))) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        size_t new_cap = v->cap * 2u;
        uint32_t *nb = (uint32_t *)v->alloc->alloc(
            v->alloc->ctx, new_cap * sizeof(uint32_t));
        if (!nb) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(nb, arr, v->count * sizeof(uint32_t));
        if (v->heap_buf) {
            v->alloc->free(v->alloc->ctx, v->heap_buf);
        }
        v->heap_buf = nb;
        v->cap = new_cap;
        arr = nb;
    }
    arr[v->count++] = val;
    v->sorted = 0;
    return RFC_OK;
}

static rfc_status avif_u32_vec_push_bounded_id(avif_u32_vec *v, uint32_t val) {
    if (v->count >= AVIF_PREFLIGHT_MAX_RAW_IDS) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return avif_u32_vec_push(v, val);
}

static int avif_cmp_u32(const void *a, const void *b) {
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    return (va > vb) - (va < vb);
}

static void avif_u32_vec_sort(avif_u32_vec *v) {
    if (v->count > 1u && !v->sorted) {
        qsort(avif_u32_vec_data(v), v->count, sizeof(uint32_t), avif_cmp_u32);
    }
    v->sorted = 1;
}

static uint64_t avif_u32_vec_count_distinct(avif_u32_vec *v) {
    if (v->count == 0u) {
        return 0u;
    }
    avif_u32_vec_sort(v);
    const uint32_t *arr = avif_u32_vec_cdata(v);
    uint64_t distinct = 1u;
    for (size_t i = 1u; i < v->count; i++) {
        if (arr[i] != arr[i - 1u]) {
            distinct++;
        }
    }
    return distinct;
}

static int avif_u32_vec_contains(const avif_u32_vec *v, uint32_t val) {
    if (v->count == 0u) {
        return 0;
    }
    const uint32_t *arr = avif_u32_vec_cdata(v);
    size_t lo = 0u;
    size_t hi = v->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (arr[mid] == val) {
            return 1;
        }
        if (arr[mid] < val) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static rfc_status isobmff_scan_iloc(const void *session,
                                    const rfc_exec_context *exec,
                                    const uint8_t *data,
                                    size_t pay_off,
                                    size_t pay_len,
                                    avif_u32_vec *io_item_ids,
                                    uint64_t *out_total_extents) {
    *out_total_extents = 0u;
    if (pay_len < 8u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    const uint8_t *p = data + pay_off;
    uint8_t ver = p[0];
    if (ver > 2u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint8_t off_sz = (uint8_t)((p[4] >> 4) & 0x0fu);
    uint8_t len_sz = (uint8_t)(p[4] & 0x0fu);
    uint8_t base_sz = (uint8_t)((p[5] >> 4) & 0x0fu);
    uint8_t idx_sz = (ver == 1u || ver == 2u) ? (uint8_t)(p[5] & 0x0fu) : 0u;
    if (!iloc_field_valid(off_sz) || !iloc_field_valid(len_sz) ||
        !iloc_field_valid(base_sz) || !iloc_field_valid(idx_sz)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t cur = 6u;
    uint32_t item_cnt = 0u;
    if (ver < 2u) {
        if (pay_len - cur < 2u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        item_cnt = avif_rd16be(p + cur);
        cur += 2u;
    } else {
        if (pay_len - cur < 4u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        item_cnt = avif_rd32be(p + cur);
        cur += 4u;
    }
    size_t id_sz = (ver < 2u) ? 2u : 4u;
    size_t per_hdr = id_sz + ((ver == 1u || ver == 2u) ? 2u : 0u) + 2u +
                     (size_t)base_sz + 2u;
    size_t ext_sz = (size_t)idx_sz + (size_t)off_sz + (size_t)len_sz;
    for (uint32_t i = 0u; i < item_cnt; i++) {
        if ((i & 63u) == 0u && avif_is_cancelled(session, exec)) {
            return RFC_ERR_CANCELLED;
        }
        if (pay_len - cur < per_hdr) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint32_t iid = (ver < 2u) ? (uint32_t)avif_rd16be(p + cur)
                                  : avif_rd32be(p + cur);
        rfc_status st = avif_u32_vec_push_bounded_id(io_item_ids, iid);
        if (st != RFC_OK) {
            return st;
        }
        cur += id_sz + ((ver == 1u || ver == 2u) ? 2u : 0u) + 2u +
               (size_t)base_sz;
        uint16_t ext_cnt = avif_rd16be(p + cur);
        cur += 2u;
        if (ext_cnt > 0u &&
            ((uint64_t)ext_cnt * (uint64_t)ext_sz > (uint64_t)(pay_len - cur))) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (!avif_add_u64(*out_total_extents, (uint64_t)ext_cnt,
                          out_total_extents) ||
            *out_total_extents > AVIF_PREFLIGHT_MAX_EXTENTS) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        cur += (size_t)ext_cnt * ext_sz;
    }
    return RFC_OK;
}

static rfc_status isobmff_iloc_item_extent_sum(const void *session,
                                                const rfc_exec_context *exec,
                                                const uint8_t *data,
                                                size_t pay_off,
                                                size_t pay_len,
                                                uint32_t target_id,
                                                int *out_found,
                                                uint64_t *out_bytes) {
    *out_found = 0;
    *out_bytes = 0u;
    if (pay_len < 8u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    const uint8_t *p = data + pay_off;
    uint8_t ver = p[0];
    if (ver > 2u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint8_t off_sz = (uint8_t)((p[4] >> 4) & 0x0fu);
    uint8_t len_sz = (uint8_t)(p[4] & 0x0fu);
    uint8_t base_sz = (uint8_t)((p[5] >> 4) & 0x0fu);
    uint8_t idx_sz = (ver == 1u || ver == 2u) ? (uint8_t)(p[5] & 0x0fu) : 0u;
    if (!iloc_field_valid(off_sz) || !iloc_field_valid(len_sz) ||
        !iloc_field_valid(base_sz) || !iloc_field_valid(idx_sz)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t cur = 6u;
    uint32_t item_cnt = 0u;
    if (ver < 2u) {
        if (pay_len - cur < 2u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        item_cnt = avif_rd16be(p + cur);
        cur += 2u;
    } else {
        if (pay_len - cur < 4u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        item_cnt = avif_rd32be(p + cur);
        cur += 4u;
    }
    size_t id_sz = (ver < 2u) ? 2u : 4u;
    size_t per_hdr = id_sz + ((ver == 1u || ver == 2u) ? 2u : 0u) + 2u +
                     (size_t)base_sz + 2u;
    for (uint32_t i = 0u; i < item_cnt; i++) {
        if ((i & 63u) == 0u && avif_is_cancelled(session, exec)) {
            return RFC_ERR_CANCELLED;
        }
        if (pay_len - cur < per_hdr) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        uint32_t iid = (ver < 2u) ? (uint32_t)avif_rd16be(p + cur)
                                  : avif_rd32be(p + cur);
        cur += id_sz;
        if (ver == 1u || ver == 2u) {
            cur += 2u;
        }
        cur += 2u + (size_t)base_sz;
        uint16_t ext_cnt = avif_rd16be(p + cur);
        cur += 2u;
        size_t ext_sz = (size_t)idx_sz + (size_t)off_sz + (size_t)len_sz;
        if (ext_cnt > 0u &&
            ((uint64_t)ext_cnt * (uint64_t)ext_sz > (uint64_t)(pay_len - cur))) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (iid == target_id) {
            *out_found = 1;
            uint64_t total = 0u;
            for (uint16_t e = 0u; e < ext_cnt; e++) {
                if ((e & 63u) == 0u && avif_is_cancelled(session, exec)) {
                    return RFC_ERR_CANCELLED;
                }
                const uint8_t *ep = p + cur + (size_t)e * ext_sz +
                                    (size_t)idx_sz + (size_t)off_sz;
                uint64_t elen = (len_sz > 0u) ? avif_rdu_be(ep, len_sz) : 0u;
                if (!avif_add_u64(total, elen, &total)) {
                    return RFC_ERR_LIMIT_EXCEEDED;
                }
            }
            *out_bytes = total;
            return RFC_OK;
        }
        cur += (size_t)ext_cnt * ext_sz;
    }
    return RFC_OK;
}

static rfc_status isobmff_collect_iref_items_and_cdsc(
    const void *session,
    const rfc_exec_context *exec,
    const uint8_t *data,
    size_t iref_off,
    size_t iref_len,
    uint32_t primary_id,
    avif_u32_vec *io_item_ids,
    avif_u32_vec *out_cdsc_from_ids) {
    if (iref_len < 4u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    uint8_t ver = data[iref_off];
    size_t ref_sz = (ver == 0u) ? 2u : 4u;
    size_t pos = iref_off + 4u;
    size_t end = iref_off + iref_len;
    size_t box_idx = 0u;
    while (pos < end) {
        if ((box_idx & 63u) == 0u && avif_is_cancelled(session, exec)) {
            return RFC_ERR_CANCELLED;
        }
        box_idx++;
        isobmff_box_view b;
        rfc_status st = isobmff_next_box(data, end, pos, &b);
        if (st != RFC_OK) {
            return st;
        }
        if (b.pay_len < ref_sz + 2u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        const uint8_t *rp = data + b.pay_off;
        uint32_t fid = (ver == 0u) ? (uint32_t)avif_rd16be(rp)
                                   : avif_rd32be(rp);
        uint16_t cnt = avif_rd16be(rp + ref_sz);
        if ((uint64_t)cnt * (uint64_t)ref_sz >
            (uint64_t)(b.pay_len - ref_sz - 2u)) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        st = avif_u32_vec_push_bounded_id(io_item_ids, fid);
        if (st != RFC_OK) {
            return st;
        }
        int is_cdsc = (memcmp(b.type, "cdsc", 4) == 0);
        int is_dimg = (memcmp(b.type, "dimg", 4) == 0);
        if (is_cdsc || is_dimg) {
            int cdsc_matches = 0;
            for (uint16_t k = 0u; k < cnt; k++) {
                if ((k & 63u) == 0u && avif_is_cancelled(session, exec)) {
                    return RFC_ERR_CANCELLED;
                }
                const uint8_t *tp = rp + ref_sz + 2u + (size_t)k * ref_sz;
                uint32_t to_id = (ver == 0u) ? (uint32_t)avif_rd16be(tp)
                                             : avif_rd32be(tp);
                if (is_dimg) {
                    st = avif_u32_vec_push_bounded_id(io_item_ids, to_id);
                    if (st != RFC_OK) {
                        return st;
                    }
                }
                if (is_cdsc && primary_id != 0u && to_id == primary_id) {
                    cdsc_matches = 1;
                }
            }
            if (cdsc_matches) {
                st = avif_u32_vec_push(out_cdsc_from_ids, fid);
                if (st != RFC_OK) {
                    return st;
                }
            }
        }
        pos = b.next_off;
    }
    avif_u32_vec_sort(out_cdsc_from_ids);
    return RFC_OK;
}

static rfc_status isobmff_collect_grpl_altr_siblings(
    const void *session,
    const rfc_exec_context *exec,
    const uint8_t *data,
    size_t grpl_off,
    size_t grpl_len,
    uint32_t target_id,
    int *out_in_altr,
    avif_u32_vec *out_siblings) {
    *out_in_altr = 0;
    size_t pos = grpl_off;
    size_t end = grpl_off + grpl_len;
    size_t box_idx = 0u;
    while (pos < end) {
        if ((box_idx & 63u) == 0u && avif_is_cancelled(session, exec)) {
            return RFC_ERR_CANCELLED;
        }
        box_idx++;
        isobmff_box_view b;
        rfc_status st = isobmff_next_box(data, end, pos, &b);
        if (st != RFC_OK) {
            return st;
        }
        if (memcmp(b.type, "altr", 4) == 0) {
            if (b.pay_len < 12u) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            const uint8_t *gp = data + b.pay_off;
            uint32_t num = avif_rd32be(gp + 8u);
            if ((uint64_t)num * 4ull > (uint64_t)(b.pay_len - 12u)) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            int found_target = 0;
            for (uint32_t i = 0u; i < num; i++) {
                if ((i & 63u) == 0u && avif_is_cancelled(session, exec)) {
                    return RFC_ERR_CANCELLED;
                }
                uint32_t eid = avif_rd32be(gp + 12u + (size_t)i * 4u);
                if (eid == target_id) {
                    found_target = 1;
                    break;
                }
            }
            if (found_target) {
                *out_in_altr = 1;
                for (uint32_t i = 0u; i < num; i++) {
                    if ((i & 63u) == 0u && avif_is_cancelled(session, exec)) {
                        return RFC_ERR_CANCELLED;
                    }
                    uint32_t eid = avif_rd32be(gp + 12u + (size_t)i * 4u);
                    st = avif_u32_vec_push(out_siblings, eid);
                    if (st != RFC_OK) {
                        return st;
                    }
                }
                avif_u32_vec_sort(out_siblings);
                return RFC_OK;
            }
        }
        pos = b.next_off;
    }
    return RFC_OK;
}

static int isobmff_is_known_non_opaque_prop(const uint8_t type[4]) {
    static const char *const k_known[] = {
        "ispe", "auxC", "colr", "av1C", "pasp", "clap", "irot",
        "imir", "pixi", "a1op", "lsel", "a1lx", "clli"
    };
    for (size_t i = 0u; i < sizeof(k_known) / sizeof(k_known[0]); i++) {
        if (memcmp(type, k_known[i], 4) == 0) {
            return 1;
        }
    }
    return 0;
}

static rfc_status isobmff_check_iprp(const void *session,
                                     const rfc_exec_context *exec,
                                     const rfc_allocator *alloc,
                                     const uint8_t *data,
                                     size_t iprp_off,
                                     size_t iprp_len,
                                     size_t meta_cap,
                                     avif_u32_vec *io_item_ids,
                                     uint64_t *io_icc_bytes,
                                     uint64_t *io_opaque_alloc_bytes,
                                     uint64_t *io_unknown_opaque_bytes,
                                     uint64_t *io_prop_count,
                                     uint64_t *io_ipma_assoc_count) {
    avif_u32_vec prop_opaque_tags;
    avif_u32_vec_init(&prop_opaque_tags, alloc);
    rfc_status st = RFC_OK;
    uint32_t mdcv_decl_cnt = 0u;
    uint32_t mdcv_assoc_cnt = 0u;
    uint64_t unknown_cap =
        ((uint64_t)meta_cap < AVIF_PREFLIGHT_MAX_OPAQUE_BYTES)
            ? (uint64_t)meta_cap
            : AVIF_PREFLIGHT_MAX_OPAQUE_BYTES;

    size_t pos = iprp_off;
    size_t end = iprp_off + iprp_len;
    while (pos < end) {
        if (avif_is_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            goto done;
        }
        isobmff_box_view b;
        st = isobmff_next_box(data, end, pos, &b);
        if (st != RFC_OK) {
            goto done;
        }
        if (memcmp(b.type, "ipco", 4) == 0) {
            size_t cpos = b.pay_off;
            size_t cend = b.pay_off + b.pay_len;
            uint32_t pi = 0u;
            while (cpos < cend) {
                if ((pi & 63u) == 0u && avif_is_cancelled(session, exec)) {
                    st = RFC_ERR_CANCELLED;
                    goto done;
                }
                pi++;
                isobmff_box_view prop;
                st = isobmff_next_box(data, cend, cpos, &prop);
                if (st != RFC_OK) {
                    goto done;
                }
                *io_prop_count += 1ull;
                if (*io_prop_count > AVIF_PREFLIGHT_MAX_PROPS) {
                    st = RFC_ERR_LIMIT_EXCEEDED;
                    goto done;
                }
                uint32_t tag = 0u;
                if (memcmp(prop.type, "colr", 4) == 0 && prop.pay_len >= 4u) {
                    const uint8_t *ct = data + prop.pay_off;
                    if (memcmp(ct, "prof", 4) == 0 ||
                        memcmp(ct, "rICC", 4) == 0) {
                        uint64_t icc_sz = (uint64_t)(prop.pay_len - 4u);
                        if (!avif_add_u64(*io_icc_bytes, icc_sz, io_icc_bytes) ||
                            *io_icc_bytes > (uint64_t)meta_cap) {
                            st = RFC_ERR_LIMIT_EXCEEDED;
                            goto done;
                        }
                    }
                } else if (memcmp(prop.type, "mdcv", 4) == 0) {
                    if (prop.pay_len != 24u) {
                        st = RFC_ERR_INVALID_ARGUMENT;
                        goto done;
                    }
                    if (!avif_add_u64(*io_opaque_alloc_bytes, 24ull,
                                      io_opaque_alloc_bytes)) {
                        st = RFC_ERR_LIMIT_EXCEEDED;
                        goto done;
                    }
                    if (mdcv_decl_cnt == 0u) {
                        tag = 24u | 0x80000000u;
                    } else {
                        if (!avif_add_u64(*io_unknown_opaque_bytes, 24ull,
                                          io_unknown_opaque_bytes) ||
                            *io_unknown_opaque_bytes > unknown_cap) {
                            st = RFC_ERR_LIMIT_EXCEEDED;
                            goto done;
                        }
                        tag = 24u;
                    }
                    mdcv_decl_cnt++;
                } else if (!isobmff_is_known_non_opaque_prop(prop.type)) {
                    if ((uint64_t)prop.pay_len > unknown_cap ||
                        !avif_add_u64(*io_opaque_alloc_bytes,
                                      (uint64_t)prop.pay_len,
                                      io_opaque_alloc_bytes) ||
                        !avif_add_u64(*io_unknown_opaque_bytes,
                                      (uint64_t)prop.pay_len,
                                      io_unknown_opaque_bytes) ||
                        *io_unknown_opaque_bytes > unknown_cap) {
                        st = RFC_ERR_LIMIT_EXCEEDED;
                        goto done;
                    }
                    tag = (uint32_t)prop.pay_len;
                }
                st = avif_u32_vec_push(&prop_opaque_tags, tag);
                if (st != RFC_OK) {
                    goto done;
                }
                cpos = prop.next_off;
            }
        } else if (memcmp(b.type, "ipma", 4) == 0) {
            if (b.pay_len < 8u) {
                st = RFC_ERR_TRUNCATED_INPUT;
                goto done;
            }
            const uint8_t *mp = data + b.pay_off;
            uint8_t ver = mp[0];
            uint32_t flags = ((uint32_t)mp[1] << 16) |
                             ((uint32_t)mp[2] << 8) | (uint32_t)mp[3];
            uint32_t entry_cnt = avif_rd32be(mp + 4u);
            size_t id_sz = (ver < 1u) ? 2u : 4u;
            size_t assoc_sz = (flags & 1u) ? 2u : 1u;
            size_t mcur = 8u;
            if ((uint64_t)entry_cnt * (uint64_t)(id_sz + 1u) >
                (uint64_t)(b.pay_len - mcur)) {
                st = RFC_ERR_TRUNCATED_INPUT;
                goto done;
            }
            for (uint32_t ei = 0u; ei < entry_cnt; ei++) {
                if ((ei & 63u) == 0u && avif_is_cancelled(session, exec)) {
                    st = RFC_ERR_CANCELLED;
                    goto done;
                }
                if (b.pay_len - mcur < id_sz + 1u) {
                    st = RFC_ERR_TRUNCATED_INPUT;
                    goto done;
                }
                uint32_t iid = (ver < 1u) ? (uint32_t)avif_rd16be(mp + mcur)
                                          : avif_rd32be(mp + mcur);
                st = avif_u32_vec_push_bounded_id(io_item_ids, iid);
                if (st != RFC_OK) {
                    goto done;
                }
                mcur += id_sz;
                uint8_t acnt = mp[mcur++];
                if ((uint64_t)acnt * (uint64_t)assoc_sz >
                    (uint64_t)(b.pay_len - mcur)) {
                    st = RFC_ERR_TRUNCATED_INPUT;
                    goto done;
                }
                if (!avif_add_u64(*io_ipma_assoc_count, (uint64_t)acnt,
                                  io_ipma_assoc_count) ||
                    *io_ipma_assoc_count > AVIF_PREFLIGHT_MAX_ASSOCS) {
                    st = RFC_ERR_LIMIT_EXCEEDED;
                    goto done;
                }
                for (uint8_t ai = 0u; ai < acnt; ai++) {
                    uint32_t pidx = (assoc_sz == 2u)
                                        ? (uint32_t)(avif_rd16be(mp + mcur) &
                                                     0x7FFFu)
                                        : (uint32_t)(mp[mcur] & 0x7Fu);
                    mcur += assoc_sz;
                    if (pidx > 0u &&
                        (size_t)(pidx - 1u) < prop_opaque_tags.count) {
                        uint32_t tag =
                            avif_u32_vec_cdata(&prop_opaque_tags)[pidx - 1u];
                        if (tag != 0u) {
                            uint64_t psz = (uint64_t)(tag & 0x7FFFFFFFu);
                            int is_mdcv = (tag & 0x80000000u) != 0u;
                            if (!avif_add_u64(*io_opaque_alloc_bytes, psz,
                                              io_opaque_alloc_bytes)) {
                                st = RFC_ERR_LIMIT_EXCEEDED;
                                goto done;
                            }
                            if (!is_mdcv || mdcv_assoc_cnt > 0u) {
                                if (!avif_add_u64(*io_unknown_opaque_bytes, psz,
                                                  io_unknown_opaque_bytes) ||
                                    *io_unknown_opaque_bytes > unknown_cap) {
                                    st = RFC_ERR_LIMIT_EXCEEDED;
                                    goto done;
                                }
                            }
                            if (is_mdcv) {
                                mdcv_assoc_cnt++;
                            }
                        }
                    }
                }
            }
        }
        pos = b.next_off;
    }

done:
    avif_u32_vec_free(&prop_opaque_tags);
    return st;
}

static const char *isobmff_cstr_bounded(const uint8_t *p,
                                        size_t len,
                                        size_t *out_next) {
    for (size_t i = 0u; i < len; i++) {
        if (p[i] == '\0') {
            *out_next = i + 1u;
            return (const char *)p;
        }
    }
    return NULL;
}

static rfc_status avif_preflight_isobmff(const void *session,
                                         const rfc_exec_context *exec,
                                         const rfc_allocator *alloc,
                                         const uint8_t *data,
                                         size_t len,
                                         size_t meta_cap,
                                         size_t *out_est_meta_bytes) {
    if (out_est_meta_bytes) {
        *out_est_meta_bytes = 0u;
    }
    if (!data && len > 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (len < 12u) {
        return len == 0u ? RFC_ERR_INVALID_ARGUMENT : RFC_ERR_TRUNCATED_INPUT;
    }
    if (avif_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    isobmff_box_view first;
    rfc_status st = isobmff_next_box(data, len, 0u, &first);
    if (st != RFC_OK) {
        return st;
    }
    if (memcmp(first.type, "ftyp", 4) != 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (first.pay_len < 8u || (first.pay_len % 4u) != 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const uint8_t *ft = data + first.pay_off;
    if (memcmp(ft, "avis", 4) == 0) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    int has_avif_brand = (memcmp(ft, "avif", 4) == 0 ||
                          memcmp(ft, "mif1", 4) == 0 ||
                          memcmp(ft, "miaf", 4) == 0);
    for (size_t off = 8u; off + 4u <= first.pay_len; off += 4u) {
        if (memcmp(ft + off, "avis", 4) == 0) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
        if (memcmp(ft + off, "avif", 4) == 0 ||
            memcmp(ft + off, "mif1", 4) == 0 ||
            memcmp(ft + off, "miaf", 4) == 0) {
            has_avif_brand = 1;
        }
    }
    if (!has_avif_brand) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    size_t pos = first.next_off;
    int saw_meta = 0;
    size_t meta_off = 0u, meta_len = 0u;
    size_t top_idx = 0u;
    while (pos < len) {
        if ((top_idx & 63u) == 0u && avif_is_cancelled(session, exec)) {
            return RFC_ERR_CANCELLED;
        }
        top_idx++;
        isobmff_box_view top;
        st = isobmff_next_box(data, len, pos, &top);
        if (st != RFC_OK) {
            return st;
        }
        if (memcmp(top.type, "moov", 4) == 0 ||
            memcmp(top.type, "mini", 4) == 0) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
        if (memcmp(top.type, "meta", 4) == 0 && !saw_meta) {
            saw_meta = 1;
            meta_off = top.pay_off;
            meta_len = top.pay_len;
        }
        pos = top.next_off;
    }
    if (!saw_meta) {
        return RFC_OK;
    }
    if (meta_len < 4u) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    avif_u32_vec item_ids;
    avif_u32_vec altr_siblings;
    avif_u32_vec cdsc_primary_ids;
    avif_u32_vec_init(&item_ids, alloc);
    avif_u32_vec_init(&altr_siblings, alloc);
    avif_u32_vec_init(&cdsc_primary_ids, alloc);

    int has_pitm = 0, has_iinf = 0, has_iref = 0, has_iloc = 0, has_grpl = 0,
        has_idat = 0;
    size_t pitm_off = 0u, pitm_len = 0u;
    size_t iinf_off = 0u, iinf_len = 0u;
    size_t iref_off = 0u, iref_len = 0u;
    size_t iloc_off = 0u, iloc_len = 0u;
    size_t grpl_off = 0u, grpl_len = 0u;
    uint64_t icc_bytes = 0u;
    uint64_t opaque_alloc_bytes = 0u;
    uint64_t unknown_opaque_bytes = 0u;
    uint64_t prop_count = 0u;
    uint64_t ipma_assoc_count = 0u;
    uint64_t iloc_extent_count = 0u;
    uint64_t idat_bytes = 0u;

    size_t mpos = meta_off + 4u;
    size_t mend = meta_off + meta_len;
    size_t m_idx = 0u;
    while (mpos < mend) {
        if ((m_idx & 63u) == 0u && avif_is_cancelled(session, exec)) {
            st = RFC_ERR_CANCELLED;
            goto cleanup;
        }
        m_idx++;
        isobmff_box_view sub;
        st = isobmff_next_box(data, mend, mpos, &sub);
        if (st != RFC_OK) {
            goto cleanup;
        }
        if (memcmp(sub.type, "pitm", 4) == 0 && !has_pitm) {
            has_pitm = 1;
            pitm_off = sub.pay_off;
            pitm_len = sub.pay_len;
        } else if (memcmp(sub.type, "iinf", 4) == 0 && !has_iinf) {
            has_iinf = 1;
            iinf_off = sub.pay_off;
            iinf_len = sub.pay_len;
        } else if (memcmp(sub.type, "iref", 4) == 0 && !has_iref) {
            has_iref = 1;
            iref_off = sub.pay_off;
            iref_len = sub.pay_len;
        } else if (memcmp(sub.type, "iloc", 4) == 0 && !has_iloc) {
            has_iloc = 1;
            iloc_off = sub.pay_off;
            iloc_len = sub.pay_len;
        } else if (memcmp(sub.type, "grpl", 4) == 0 && !has_grpl) {
            has_grpl = 1;
            grpl_off = sub.pay_off;
            grpl_len = sub.pay_len;
        } else if (memcmp(sub.type, "idat", 4) == 0 && !has_idat) {
            has_idat = 1;
            idat_bytes = (uint64_t)sub.pay_len;
            if (idat_bytes > AVIF_PREFLIGHT_MAX_IDAT_BYTES) {
                st = RFC_ERR_LIMIT_EXCEEDED;
                goto cleanup;
            }
        } else if (memcmp(sub.type, "iprp", 4) == 0) {
            st = isobmff_check_iprp(
                session, exec, alloc, data, sub.pay_off, sub.pay_len, meta_cap,
                &item_ids, &icc_bytes, &opaque_alloc_bytes,
                &unknown_opaque_bytes, &prop_count, &ipma_assoc_count);
            if (st != RFC_OK) {
                goto cleanup;
            }
        }
        mpos = sub.next_off;
    }

    uint32_t primary_id = 0u;
    if (has_pitm) {
        if (pitm_len < 6u) {
            st = RFC_ERR_TRUNCATED_INPUT;
            goto cleanup;
        }
        uint8_t ver = data[pitm_off];
        if (ver == 0u) {
            primary_id = avif_rd16be(data + pitm_off + 4u);
        } else {
            if (pitm_len < 8u) {
                st = RFC_ERR_TRUNCATED_INPUT;
                goto cleanup;
            }
            primary_id = avif_rd32be(data + pitm_off + 4u);
        }
        if (primary_id != 0u) {
            st = avif_u32_vec_push_bounded_id(&item_ids, primary_id);
            if (st != RFC_OK) {
                goto cleanup;
            }
        }
    }

    int primary_in_altr = 0;
    if (has_grpl && primary_id != 0u) {
        st = isobmff_collect_grpl_altr_siblings(
            session, exec, data, grpl_off, grpl_len, primary_id,
            &primary_in_altr, &altr_siblings);
        if (st != RFC_OK) {
            goto cleanup;
        }
    }
    if (has_iref) {
        st = isobmff_collect_iref_items_and_cdsc(
            session, exec, data, iref_off, iref_len, primary_id, &item_ids,
            &cdsc_primary_ids);
        if (st != RFC_OK) {
            goto cleanup;
        }
    }
    if (has_iloc) {
        st = isobmff_scan_iloc(session, exec, data, iloc_off, iloc_len,
                               &item_ids, &iloc_extent_count);
        if (st != RFC_OK) {
            goto cleanup;
        }
    }

    uint32_t exif_count = 0u;
    uint32_t xmp_count = 0u;
    uint64_t total_meta_bytes = icc_bytes;
    if (has_iinf) {
        if (iinf_len < 6u) {
            st = RFC_ERR_TRUNCATED_INPUT;
            goto cleanup;
        }
        uint8_t ver = data[iinf_off];
        size_t cur = iinf_off + 4u;
        uint32_t cnt = 0u;
        if (ver == 0u) {
            if (iinf_off + iinf_len - cur < 2u) {
                st = RFC_ERR_TRUNCATED_INPUT;
                goto cleanup;
            }
            cnt = avif_rd16be(data + cur);
            cur += 2u;
        } else {
            if (iinf_off + iinf_len - cur < 4u) {
                st = RFC_ERR_TRUNCATED_INPUT;
                goto cleanup;
            }
            cnt = avif_rd32be(data + cur);
            cur += 4u;
        }
        size_t iend = iinf_off + iinf_len;
        if ((uint64_t)cnt * 12ull > (uint64_t)(iend - cur)) {
            st = RFC_ERR_TRUNCATED_INPUT;
            goto cleanup;
        }
        for (uint32_t i = 0u; i < cnt && cur < iend; i++) {
            if ((i & 63u) == 0u && avif_is_cancelled(session, exec)) {
                st = RFC_ERR_CANCELLED;
                goto cleanup;
            }
            isobmff_box_view infe;
            st = isobmff_next_box(data, iend, cur, &infe);
            if (st != RFC_OK) {
                goto cleanup;
            }
            cur = infe.next_off;
            if (memcmp(infe.type, "infe", 4) != 0 || infe.pay_len < 4u) {
                continue;
            }
            const uint8_t *ip = data + infe.pay_off;
            uint8_t iver = ip[0];
            if (iver < 2u) {
                continue;
            }
            size_t id_sz = (iver == 2u) ? 2u : 4u;
            if (infe.pay_len < 4u + id_sz + 2u + 4u) {
                st = RFC_ERR_TRUNCATED_INPUT;
                goto cleanup;
            }
            uint32_t iid = (iver == 2u) ? (uint32_t)avif_rd16be(ip + 4u)
                                        : avif_rd32be(ip + 4u);
            st = avif_u32_vec_push_bounded_id(&item_ids, iid);
            if (st != RFC_OK) {
                goto cleanup;
            }
            const uint8_t *itype = ip + 4u + id_sz + 2u;
            size_t name_off = 4u + id_sz + 2u + 4u;
            size_t rem = infe.pay_len - name_off;
            size_t next_str = 0u;
            if (!isobmff_cstr_bounded(ip + name_off, rem, &next_str)) {
                continue;
            }
            int is_tmap_or_sato = (memcmp(itype, "tmap", 4) == 0 ||
                                   memcmp(itype, "sato", 4) == 0);
            if (is_tmap_or_sato) {
                if (iid == primary_id ||
                    (primary_in_altr &&
                     avif_u32_vec_contains(&altr_siblings, iid))) {
                    st = RFC_ERR_UNSUPPORTED_COMBINATION;
                    goto cleanup;
                }
            }

            int is_exif = (memcmp(itype, "Exif", 4) == 0);
            int is_xmp = 0;
            if (memcmp(itype, "mime", 4) == 0 && next_str < rem) {
                size_t dummy = 0u;
                const char *ctype = isobmff_cstr_bounded(
                    ip + name_off + next_str, rem - next_str, &dummy);
                if (ctype && strcmp(ctype, "application/rdf+xml") == 0) {
                    is_xmp = 1;
                }
            }
            if (!is_exif && !is_xmp) {
                continue;
            }

            int attached = 1;
            if (primary_id != 0u && has_iref) {
                attached = avif_u32_vec_contains(&cdsc_primary_ids, iid);
            } else if (primary_id != 0u && !has_iref) {
                attached = 0;
            }
            if (!attached) {
                continue;
            }

            if (is_exif) {
                exif_count++;
                if (exif_count > 1u) {
                    st = RFC_ERR_UNSUPPORTED_COMBINATION;
                    goto cleanup;
                }
            } else {
                xmp_count++;
                if (xmp_count > 1u) {
                    st = RFC_ERR_UNSUPPORTED_COMBINATION;
                    goto cleanup;
                }
            }
            if (has_iloc) {
                int found = 0;
                uint64_t item_sz = 0u;
                st = isobmff_iloc_item_extent_sum(
                    session, exec, data, iloc_off, iloc_len, iid, &found,
                    &item_sz);
                if (st != RFC_OK) {
                    goto cleanup;
                }
                if (found) {
                    uint64_t eff_sz = item_sz;
                    if (is_exif && eff_sz >= 4u) {
                        eff_sz -= 4u;
                    }
                    if (eff_sz > (uint64_t)meta_cap ||
                        !avif_add_u64(total_meta_bytes, eff_sz,
                                      &total_meta_bytes) ||
                        total_meta_bytes > (uint64_t)meta_cap) {
                        st = RFC_ERR_LIMIT_EXCEEDED;
                        goto cleanup;
                    }
                }
            }
        }
    }

    uint64_t distinct_items = avif_u32_vec_count_distinct(&item_ids);
    if (distinct_items > (uint64_t)AVIF_PREFLIGHT_MAX_ITEMS) {
        st = RFC_ERR_LIMIT_EXCEEDED;
        goto cleanup;
    }

    {
        uint64_t item_bytes = 0u;
        uint64_t assoc_bytes = 0u;
        uint64_t extent_bytes = 0u;
        uint64_t prop_bytes = 0u;
        uint64_t struct_est = 0u;
        uint64_t est = 0u;
        if (!avif_mul_u64(distinct_items, 2048ull, &item_bytes) ||
            !avif_mul_u64(ipma_assoc_count, 192ull, &assoc_bytes) ||
            !avif_mul_u64(iloc_extent_count, 32ull, &extent_bytes) ||
            !avif_mul_u64(prop_count, 256ull, &prop_bytes) ||
            !avif_add_u64(opaque_alloc_bytes, idat_bytes, &struct_est) ||
            !avif_add_u64(struct_est, item_bytes, &struct_est) ||
            !avif_add_u64(struct_est, assoc_bytes, &struct_est) ||
            !avif_add_u64(struct_est, extent_bytes, &struct_est) ||
            !avif_add_u64(struct_est, prop_bytes, &struct_est) ||
            struct_est > AVIF_PREFLIGHT_MAX_STRUCT_META_BYTES ||
            !avif_add_u64(total_meta_bytes, struct_est, &est) ||
            est > (uint64_t)SIZE_MAX) {
            st = RFC_ERR_LIMIT_EXCEEDED;
            goto cleanup;
        }
        if (out_est_meta_bytes) {
            *out_est_meta_bytes = (size_t)est;
        }
    }
    st = RFC_OK;

cleanup:
    avif_u32_vec_free(&cdsc_primary_ids);
    avif_u32_vec_free(&altr_siblings);
    avif_u32_vec_free(&item_ids);
    return st;
}

/* ------------------------------------------------------------------------ */
/* Orientation, EXIF, and Colorimetry helpers                               */
/* ------------------------------------------------------------------------ */

static int avif_exif_magic_strict(const uint8_t *data, size_t len) {
    if (!data || len < 8u) {
        return 0;
    }
    size_t off = 0u;
    if (len >= 14u && memcmp(data, "Exif\x00\x00", 6) == 0) {
        off = 6u;
    }
    const uint8_t *t = data + off;
    return (t[0] == 'I' && t[1] == 'I' && t[2] == 0x2Au && t[3] == 0x00u) ||
           (t[0] == 'M' && t[1] == 'M' && t[2] == 0x00u && t[3] == 0x2Au);
}

static rfc_orientation avif_ori_from_irot_imir(const avifImage *img) {
    int has_irot = (img->transformFlags & AVIF_TRANSFORM_IROT) != 0u;
    int has_imir = (img->transformFlags & AVIF_TRANSFORM_IMIR) != 0u;
    uint8_t angle = has_irot ? (img->irot.angle & 3u) : 0u;
    uint8_t axis = img->imir.axis & 1u;

    if (angle == 0u) {
        if (!has_imir) {
            return RFC_ORIENTATION_NORMAL;
        }
        return axis == 0u ? RFC_ORIENTATION_FLIP_VERTICAL
                          : RFC_ORIENTATION_FLIP_HORIZONTAL;
    }
    if (angle == 1u) {
        if (!has_imir) {
            return RFC_ORIENTATION_ROTATE_270;
        }
        return axis == 0u ? RFC_ORIENTATION_TRANSPOSE
                          : RFC_ORIENTATION_TRANSVERSE;
    }
    if (angle == 2u) {
        if (!has_imir) {
            return RFC_ORIENTATION_ROTATE_180;
        }
        return axis == 0u ? RFC_ORIENTATION_FLIP_HORIZONTAL
                          : RFC_ORIENTATION_FLIP_VERTICAL;
    }
    if (!has_imir) {
        return RFC_ORIENTATION_ROTATE_90;
    }
    return axis == 0u ? RFC_ORIENTATION_TRANSVERSE : RFC_ORIENTATION_TRANSPOSE;
}

static void avif_apply_ori_to_image(avifImage *img, rfc_orientation ori) {
    img->transformFlags &= ~(avifTransformFlags)(AVIF_TRANSFORM_IROT |
                                                 AVIF_TRANSFORM_IMIR);
    img->irot.angle = 0u;
    img->imir.axis = 0u;
    switch (ori) {
        case RFC_ORIENTATION_FLIP_HORIZONTAL:
            img->transformFlags |= AVIF_TRANSFORM_IMIR;
            img->imir.axis = 1u;
            break;
        case RFC_ORIENTATION_ROTATE_180:
            img->transformFlags |= AVIF_TRANSFORM_IROT;
            img->irot.angle = 2u;
            break;
        case RFC_ORIENTATION_FLIP_VERTICAL:
            img->transformFlags |= AVIF_TRANSFORM_IMIR;
            img->imir.axis = 0u;
            break;
        case RFC_ORIENTATION_TRANSPOSE:
            img->transformFlags |= AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR;
            img->irot.angle = 1u;
            img->imir.axis = 0u;
            break;
        case RFC_ORIENTATION_ROTATE_90:
            img->transformFlags |= AVIF_TRANSFORM_IROT;
            img->irot.angle = 3u;
            break;
        case RFC_ORIENTATION_TRANSVERSE:
            img->transformFlags |= AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR;
            img->irot.angle = 3u;
            img->imir.axis = 0u;
            break;
        case RFC_ORIENTATION_ROTATE_270:
            img->transformFlags |= AVIF_TRANSFORM_IROT;
            img->irot.angle = 1u;
            break;
        default:
            break;
    }
}

static rfc_status avif_extract_metadata_from_image(const avifImage *img,
                                                   const rfc_limits *limits,
                                                   const rfc_allocator *alloc,
                                                   rfc_metadata *out_meta) {
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = RFC_ORIENTATION_NORMAL;

    rfc_orientation exif_ori = RFC_ORIENTATION_UNSPECIFIED;
    if (img->exif.size > 0u) {
        if (!avif_exif_magic_strict(img->exif.data, img->exif.size)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        rfc_status st = rfc_exif_parse_orientation(
            img->exif.data, img->exif.size, &exif_ori);
        if (st != RFC_OK) {
            return st;
        }
    }

    int has_t_ori =
        (img->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR)) != 0u;
    rfc_orientation prop_ori = avif_ori_from_irot_imir(img);
    if (has_t_ori && exif_ori != RFC_ORIENTATION_UNSPECIFIED &&
        exif_ori != prop_ori) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (has_t_ori) {
        out_meta->orientation = prop_ori;
    } else if (exif_ori != RFC_ORIENTATION_UNSPECIFIED) {
        out_meta->orientation = exif_ori;
    }

    /* Non-ICC colorimetry extraction: ICC profile takes precedence over
     * colr nclx CICP per HEIF/AVIF rules; plain unspecified (2, 2) leaves
     * has_cicp = 0; cLLI scales 1 cd/m^2 <-> 10000; mDCV permutes G,B,R,W
     * ISOBMFF wire order to R,G,B,W PNG order. */
    if (img->icc.size == 0u &&
        (img->colorPrimaries != AVIF_COLOR_PRIMARIES_UNSPECIFIED ||
         img->transferCharacteristics !=
             AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED)) {
        if (img->colorPrimaries > 255u || img->transferCharacteristics > 255u) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
        out_meta->color.has_cicp = 1u;
        out_meta->color.cicp[0] = (uint8_t)img->colorPrimaries;
        out_meta->color.cicp[1] = (uint8_t)img->transferCharacteristics;
        out_meta->color.cicp[2] = 0u;
        out_meta->color.cicp[3] = 1u;
    }
    if (img->clli.maxCLL != 0u || img->clli.maxPALL != 0u) {
        out_meta->color.has_clli = 1u;
        out_meta->color.clli_max_cll = (uint32_t)img->clli.maxCLL * 10000u;
        out_meta->color.clli_max_fall = (uint32_t)img->clli.maxPALL * 10000u;
    }

    int mdcv_seen = 0;
    for (size_t i = 0u; i < img->numProperties; i++) {
        const avifImageItemProperty *prop = &img->properties[i];
        if (memcmp(prop->boxtype, "mdcv", 4) == 0) {
            if (mdcv_seen) {
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
            mdcv_seen = 1;
            if (prop->boxPayload.size != 24u || !prop->boxPayload.data) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            const uint8_t *mp = prop->boxPayload.data;
            uint32_t max_dl = avif_rd32be(mp + 16u);
            uint32_t min_dl = avif_rd32be(mp + 20u);
            if (max_dl > 0x7FFFFFFFu || min_dl > 0x7FFFFFFFu) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            out_meta->color.has_mdcv = 1u;
            out_meta->color.mdcv_xy[2] = avif_rd16be(mp + 0u);  /* Gx */
            out_meta->color.mdcv_xy[3] = avif_rd16be(mp + 2u);  /* Gy */
            out_meta->color.mdcv_xy[4] = avif_rd16be(mp + 4u);  /* Bx */
            out_meta->color.mdcv_xy[5] = avif_rd16be(mp + 6u);  /* By */
            out_meta->color.mdcv_xy[0] = avif_rd16be(mp + 8u);  /* Rx */
            out_meta->color.mdcv_xy[1] = avif_rd16be(mp + 10u); /* Ry */
            out_meta->color.mdcv_xy[6] = avif_rd16be(mp + 12u); /* Wx */
            out_meta->color.mdcv_xy[7] = avif_rd16be(mp + 14u); /* Wy */
            out_meta->color.mdcv_max_dl = max_dl;
            out_meta->color.mdcv_min_dl = min_dl;
        }
    }

    rfc_metadata cap_chk = {
        img->icc.data, img->icc.size, out_meta->orientation, 0u, out_meta->color,
        img->exif.data, img->exif.size, img->xmp.data, img->xmp.size
    };
    rfc_status st = rfc_limits_check_metadata(limits, &cap_chk);
    if (st != RFC_OK) {
        return st;
    }

    if (img->icc.size > 0u) {
        uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, img->icc.size);
        if (!buf) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(buf, img->icc.data, img->icc.size);
        out_meta->icc = buf;
        out_meta->icc_len = img->icc.size;
    }
    if (img->exif.size > 0u) {
        uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, img->exif.size);
        if (!buf) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(buf, img->exif.data, img->exif.size);
        out_meta->exif = buf;
        out_meta->exif_len = img->exif.size;
    }
    if (img->xmp.size > 0u) {
        uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, img->xmp.size);
        if (!buf) {
            rfc_metadata_free(out_meta, alloc);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(buf, img->xmp.data, img->xmp.size);
        out_meta->xmp = buf;
        out_meta->xmp_len = img->xmp.size;
    }
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Probe & Decode implementation                                            */
/* ------------------------------------------------------------------------ */

static rfc_status map_avif_decode_error(avifResult res, int was_cancelled) {
    if (was_cancelled) {
        return RFC_ERR_CANCELLED;
    }
    switch (res) {
        case AVIF_RESULT_OK:
            return RFC_OK;
        case AVIF_RESULT_OUT_OF_MEMORY:
            return RFC_ERR_OUT_OF_MEMORY;
        case AVIF_RESULT_TRUNCATED_DATA:
        case AVIF_RESULT_WAITING_ON_IO:
            return RFC_ERR_TRUNCATED_INPUT;
        case AVIF_RESULT_NOT_IMPLEMENTED:
        case AVIF_RESULT_NO_CODEC_AVAILABLE:
        case AVIF_RESULT_INVALID_TONE_MAPPED_IMAGE:
        case AVIF_RESULT_DECODE_SAMPLE_TRANSFORM_FAILED:
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        default:
            return RFC_ERR_INVALID_ARGUMENT;
    }
}

static rfc_status avif_validate_decoded_header(const avifDecoder *dec,
                                               const rfc_limits *limits,
                                               size_t src_len,
                                               rfc_layout *out_lay) {
    if (!dec || !dec->image) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (dec->imageSequenceTrackPresent || dec->imageCount != 1 ||
        dec->progressiveState == AVIF_PROGRESSIVE_STATE_ACTIVE) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    const avifImage *img = dec->image;
    if (img->width == 0u || img->height == 0u ||
        img->width > 65536u || img->height > 65536u) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (img->depth != 8u && img->depth != 10u && img->depth != 12u) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (img->alphaPremultiplied) {
        return RFC_ERR_UNSUPPORTED_ALPHA;
    }
    if ((img->transformFlags & AVIF_TRANSFORM_CLAP) != 0u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if ((img->transformFlags & AVIF_TRANSFORM_PASP) != 0u) {
        if (img->pasp.hSpacing == 0u || img->pasp.vSpacing == 0u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (img->pasp.hSpacing != img->pasp.vSpacing) {
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
    }

    int has_alpha = dec->alphaPresent ? 1 : 0;
    rfc_sample_format fmt;
    if (img->depth == 8u) {
        fmt = has_alpha ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8;
    } else {
        fmt = has_alpha ? RFC_SAMPLE_RGBA16 : RFC_SAMPLE_RGB16;
    }

    rfc_layout lay = {
        fmt,
        has_alpha ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE,
        img->width,
        img->height,
        0u,
        img->depth
    };
    rfc_layout norm;
    rfc_status st = rfc_layout_normalize_stride(&lay, &norm);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_limits_check_image(limits, &norm, 1u);
    if (st != RFC_OK) {
        return st;
    }
    uint64_t raw_bytes = 0u;
    st = rfc_limits_compute_frame_bytes(&norm, 1u, &raw_bytes);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_limits_check_decompression(limits, (uint64_t)src_len, raw_bytes);
    if (st != RFC_OK) {
        return st;
    }
    *out_lay = norm;
    return RFC_OK;
}

static rfc_status avif_extract_pixels_to_frame(const avifImage *img,
                                               int has_alpha,
                                               uint32_t threads,
                                               rfc_frame *frame) {
    uint32_t w = img->width;
    uint32_t h = img->height;
    uint32_t depth = img->depth;
    uint32_t ch = has_alpha ? 4u : 3u;
    size_t bpc = (depth > 8u) ? 2u : 1u;
    size_t row_bytes = (size_t)w * (size_t)ch * bpc;
    if (frame->layout.stride_bytes < row_bytes ||
        frame->data_len < row_bytes * (size_t)h) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (has_alpha && !img->alphaPlane) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    /* Fast exact path: full-range YUV444 IDENTITY encodes R=V, G=Y, B=U directly
     * without rounding error. */
    if (img->yuvFormat == AVIF_PIXEL_FORMAT_YUV444 &&
        img->yuvRange == AVIF_RANGE_FULL &&
        img->matrixCoefficients == AVIF_MATRIX_COEFFICIENTS_IDENTITY) {
        if (!img->yuvPlanes[AVIF_CHAN_Y] || !img->yuvPlanes[AVIF_CHAN_U] ||
            !img->yuvPlanes[AVIF_CHAN_V]) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        uint32_t shift = (depth > 8u) ? (16u - depth) : 0u;
        uint16_t mask = (depth > 8u) ? (uint16_t)((1u << depth) - 1u) : 0xffu;
        for (uint32_t y = 0u; y < h; y++) {
            const uint8_t *py =
                img->yuvPlanes[AVIF_CHAN_Y] + (size_t)y * img->yuvRowBytes[AVIF_CHAN_Y];
            const uint8_t *pu =
                img->yuvPlanes[AVIF_CHAN_U] + (size_t)y * img->yuvRowBytes[AVIF_CHAN_U];
            const uint8_t *pv =
                img->yuvPlanes[AVIF_CHAN_V] + (size_t)y * img->yuvRowBytes[AVIF_CHAN_V];
            const uint8_t *pa =
                has_alpha ? (img->alphaPlane + (size_t)y * img->alphaRowBytes)
                          : NULL;
            uint8_t *dst = frame->data + (size_t)y * frame->layout.stride_bytes;
            if (depth == 8u) {
                for (uint32_t x = 0u; x < w; x++) {
                    dst[(size_t)x * ch + 0u] = pv[x];
                    dst[(size_t)x * ch + 1u] = py[x];
                    dst[(size_t)x * ch + 2u] = pu[x];
                    if (has_alpha) {
                        dst[(size_t)x * ch + 3u] = pa[x];
                    }
                }
            } else {
                const uint16_t *wy = (const uint16_t *)py;
                const uint16_t *wu = (const uint16_t *)pu;
                const uint16_t *wv = (const uint16_t *)pv;
                const uint16_t *wa = (const uint16_t *)pa;
                uint16_t *wdst = (uint16_t *)dst;
                for (uint32_t x = 0u; x < w; x++) {
                    uint16_t r = (uint16_t)((wv[x] & mask) << shift);
                    uint16_t g = (uint16_t)((wy[x] & mask) << shift);
                    uint16_t b = (uint16_t)((wu[x] & mask) << shift);
                    wdst[(size_t)x * ch + 0u] = r;
                    wdst[(size_t)x * ch + 1u] = g;
                    wdst[(size_t)x * ch + 2u] = b;
                    if (has_alpha) {
                        uint16_t a = (uint16_t)((wa[x] & mask) << shift);
                        wdst[(size_t)x * ch + 3u] = a;
                    }
                }
            }
        }
        return RFC_OK;
    }

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, img);
    rgb.format = has_alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
    rgb.depth = depth;
    rgb.avoidLibYUV = AVIF_TRUE;
    rgb.alphaPremultiplied = AVIF_FALSE;
    rgb.maxThreads = (int)(threads ? (threads > 64u ? 64u : threads) : 1u);
    rgb.pixels = frame->data;
    rgb.rowBytes = (uint32_t)frame->layout.stride_bytes;
    avifResult res = avifImageYUVToRGB(img, &rgb);
    if (res != AVIF_RESULT_OK) {
        return map_avif_decode_error(res, 0);
    }

    if (depth > 8u) {
        uint32_t shift = 16u - depth;
        uint16_t mask = (uint16_t)((1u << depth) - 1u);
        for (uint32_t y = 0u; y < h; y++) {
            uint16_t *wdst =
                (uint16_t *)(frame->data + (size_t)y * frame->layout.stride_bytes);
            size_t row_s = (size_t)w * (size_t)ch;
            for (size_t k = 0u; k < row_s; k++) {
                wdst[k] = (uint16_t)((wdst[k] & mask) << shift);
            }
        }
    }
    return RFC_OK;
}

static rfc_status avif_decode_common(void *session,
                                     const rfc_exec_context *exec,
                                     const uint8_t *bytes,
                                     size_t size,
                                     int want_pixels,
                                     rfc_image_probe *out_probe,
                                     rfc_frame **out_frames,
                                     size_t *out_frame_count,
                                     rfc_metadata *out_meta) {
    if (want_pixels) {
        if (!out_frames || !out_frame_count || !out_meta) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        *out_frames = NULL;
        *out_frame_count = 0u;
        memset(out_meta, 0, sizeof(*out_meta));
    } else {
        if (!out_probe) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        memset(out_probe, 0, sizeof(*out_probe));
    }
    if (!bytes && size > 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (avif_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_limits lim_def;
    const rfc_limits *limits = avif_limits_of(exec, &lim_def);
    const rfc_allocator *alloc = avif_alloc_of(exec);

    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }

    size_t meta_cap = avif_meta_cap_of(limits);
    size_t est_meta = 0u;
    st = avif_preflight_isobmff(session, exec, alloc, bytes, size, meta_cap,
                                &est_meta);
    if (st != RFC_OK) {
        return st;
    }
    if (avif_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    uint32_t eff_thr = exec ? rfc_exec_threads(exec) : 1u;
    if (eff_thr == 0u) {
        eff_thr = 1u;
    } else if (eff_thr > 64u) {
        eff_thr = 64u;
    }

    avif_budget_guard bg;
    memset(&bg, 0, sizeof(bg));
    size_t parse_res_bytes = 0u;
    if (!avif_stage_work_bytes(1024ull, 8u, 0, 1u, 0, est_meta, &parse_res_bytes)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    st = avif_reserve_budget(exec, parse_res_bytes, &bg);
    if (st != RFC_OK) {
        return st;
    }

    avifDecoder *dec = avifDecoderCreate();
    if (!dec) {
        avif_release_budget(&bg);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    dec->codecChoice = AVIF_CODEC_CHOICE_AOM;
    dec->maxThreads = (int)eff_thr;
    dec->strictFlags = AVIF_STRICT_ENABLED;
    dec->imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA;
    dec->imageCountLimit = 1u;
    dec->imageSizeLimit =
        (limits->max_pixels < (uint64_t)AVIF_DEFAULT_IMAGE_SIZE_LIMIT)
            ? (uint32_t)limits->max_pixels
            : AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
    uint32_t max_dim = limits->max_width > limits->max_height
                           ? limits->max_width
                           : limits->max_height;
    dec->imageDimensionLimit =
        (max_dim < AVIF_DEFAULT_IMAGE_DIMENSION_LIMIT)
            ? max_dim
            : AVIF_DEFAULT_IMAGE_DIMENSION_LIMIT;

    avifResult ares = avifDecoderSetIOMemory(dec, bytes, size);
    if (ares != AVIF_RESULT_OK) {
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return map_avif_decode_error(ares, avif_is_cancelled(session, exec));
    }

    ares = avifDecoderParse(dec);
    if (ares != AVIF_RESULT_OK) {
        st = map_avif_decode_error(ares, avif_is_cancelled(session, exec));
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return st;
    }
    if (avif_is_cancelled(session, exec)) {
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return RFC_ERR_CANCELLED;
    }

    rfc_layout lay;
    st = avif_validate_decoded_header(dec, limits, size, &lay);
    if (st != RFC_OK) {
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return st;
    }

    rfc_metadata meta;
    st = avif_extract_metadata_from_image(dec->image, limits, alloc, &meta);
    if (st != RFC_OK) {
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return st;
    }

    if (!want_pixels) {
        uint64_t total_px = (uint64_t)lay.width * (uint64_t)lay.height;
        uint64_t frame_bytes = 0u;
        st = rfc_limits_compute_frame_bytes(&lay, 1u, &frame_bytes);
        if (st == RFC_OK) {
            memset(out_probe, 0, sizeof(*out_probe));
            out_probe->frame_count = 1u;
            out_probe->first_frame = lay;
            out_probe->total_pixels = total_px;
            out_probe->total_decoded_bytes = frame_bytes;
            out_probe->container_meta = meta;
            st = rfc_limits_check_probe(limits, out_probe, (uint64_t)size, 1);
            if (st != RFC_OK) {
                rfc_metadata_free(&meta, alloc);
                memset(out_probe, 0, sizeof(*out_probe));
            }
        } else {
            rfc_metadata_free(&meta, alloc);
        }
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return st;
    }

    uint64_t pixels = (uint64_t)lay.width * (uint64_t)lay.height;
    size_t full_dec_bytes = 0u;
    size_t meta_total = meta.icc_len + meta.exif_len + meta.xmp_len;
    size_t dec_meta_res = (est_meta > meta_total) ? est_meta : meta_total;
    if (!avif_stage_work_bytes(pixels, lay.bits_per_sample,
                               dec->alphaPresent ? 1 : 0, eff_thr, 0,
                               dec_meta_res, &full_dec_bytes)) {
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    avif_release_budget(&bg);
    st = avif_reserve_budget(exec, full_dec_bytes, &bg);
    if (st != RFC_OK) {
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        return st;
    }

    if (avif_is_cancelled(session, exec)) {
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return RFC_ERR_CANCELLED;
    }

    ares = avifDecoderNextImage(dec);
    if (ares != AVIF_RESULT_OK) {
        st = map_avif_decode_error(ares, avif_is_cancelled(session, exec));
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return st;
    }
    if (avif_is_cancelled(session, exec)) {
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return RFC_ERR_CANCELLED;
    }

    /* Re-validate header invariants against decoded AV1 OBU frame properties. */
    rfc_layout dec_lay;
    st = avif_validate_decoded_header(dec, limits, size, &dec_lay);
    if (st != RFC_OK) {
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return st;
    }

    rfc_frame *frame = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(*frame));
    if (!frame) {
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(frame, 0, sizeof(*frame));
    st = rfc_frame_alloc(frame, &dec_lay, &meta, alloc);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, frame);
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return st;
    }

    st = avif_extract_pixels_to_frame(dec->image, dec->alphaPresent ? 1 : 0,
                                      eff_thr, frame);
    if (st != RFC_OK) {
        rfc_frame_free(frame);
        alloc->free(alloc->ctx, frame);
        rfc_metadata_free(&meta, alloc);
        avifDecoderDestroy(dec);
        avif_release_budget(&bg);
        return st;
    }

    *out_frames = frame;
    *out_frame_count = 1u;
    *out_meta = meta;
    avifDecoderDestroy(dec);
    avif_release_budget(&bg);
    return RFC_OK;
}

static rfc_status avif_probe_session(void *session,
                                     const rfc_exec_context *exec,
                                     const uint8_t *bytes,
                                     size_t size,
                                     rfc_image_probe *out_probe) {
    return avif_decode_common(session, exec, bytes, size, 0, out_probe, NULL,
                              NULL, NULL);
}

static rfc_status avif_decode_session(void *session,
                                      const rfc_exec_context *exec,
                                      const uint8_t *bytes,
                                      size_t size,
                                      const rfc_setting_value *settings,
                                      size_t setting_count,
                                      rfc_frame **out_frames,
                                      size_t *out_frame_count,
                                      rfc_metadata *out_meta) {
    if (settings && setting_count > 0u) {
        const char *reason = NULL;
        rfc_status st = rfc_registry_validate_settings(
            RFC_FORMAT_AVIF, settings, setting_count, &reason);
        if (st != RFC_OK) {
            return st;
        }
    }
    return avif_decode_common(session, exec, bytes, size, 1, NULL, out_frames,
                              out_frame_count, out_meta);
}

/* ------------------------------------------------------------------------ */
/* Encode implementation + verified decoded native RGB equality self-check  */
/* ------------------------------------------------------------------------ */

static rfc_status avif_validate_encode_input(
    const rfc_frame *fr,
    const rfc_metadata *meta,
    rfc_metadata_policy meta_policy,
    const rfc_limits *limits,
    int *out_has_alpha,
    int *out_all_opaque_alpha,
    uint32_t *out_bps,
    const uint8_t **out_icc,
    size_t *out_icc_len,
    rfc_orientation *out_ori,
    rfc_colorimetry *out_color,
    const uint8_t **out_exif,
    size_t *out_exif_len,
    const uint8_t **out_xmp,
    size_t *out_xmp_len) {
    if (!fr || !fr->data) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_layout norm;
    rfc_status st = rfc_layout_normalize_stride(&fr->layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    if (fr->layout.width == 0u || fr->layout.height == 0u ||
        fr->layout.width > 65536u || fr->layout.height > 65536u) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    int has_alpha = 0;
    uint32_t sbits = 0u;
    switch (fr->layout.format) {
        case RFC_SAMPLE_RGB8:
            has_alpha = 0;
            sbits = 8u;
            break;
        case RFC_SAMPLE_RGBA8:
            has_alpha = 1;
            sbits = 8u;
            break;
        case RFC_SAMPLE_RGB16:
            has_alpha = 0;
            sbits = 16u;
            break;
        case RFC_SAMPLE_RGBA16:
            has_alpha = 1;
            sbits = 16u;
            break;
        default:
            return RFC_ERR_UNSUPPORTED_SAMPLE;
    }

    uint32_t b = norm.bits_per_sample;
    if ((sbits == 8u && b != 8u) ||
        (sbits == 16u && b != 10u && b != 12u)) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    if (has_alpha) {
        if (fr->layout.alpha != RFC_ALPHA_STRAIGHT) {
            return RFC_ERR_UNSUPPORTED_ALPHA;
        }
    } else if (fr->layout.alpha != RFC_ALPHA_NONE) {
        return RFC_ERR_UNSUPPORTED_ALPHA;
    }

    uint32_t ch = has_alpha ? 4u : 3u;
    size_t min_row = (size_t)fr->layout.width * (size_t)ch * (size_t)(sbits / 8u);
    size_t stride = fr->layout.stride_bytes ? fr->layout.stride_bytes : norm.stride_bytes;
    if (stride < min_row) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (fr->layout.height > 0u) {
        size_t tail = (size_t)(fr->layout.height - 1u) * stride + min_row;
        if (fr->data_len < tail) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
    }

    uint32_t shift = sbits - b;
    uint16_t pad_mask = shift ? (uint16_t)((1u << shift) - 1u) : 0u;
    uint16_t max_val = (uint16_t)((1u << b) - 1u);
    int all_opaque = 1;
    for (uint32_t y = 0u; y < fr->layout.height; y++) {
        const uint8_t *row = fr->data + (size_t)y * stride;
        if (sbits == 8u) {
            if (has_alpha && all_opaque) {
                for (uint32_t x = 0u; x < fr->layout.width; x++) {
                    if (row[(size_t)x * 4u + 3u] != 0xffu) {
                        all_opaque = 0;
                        break;
                    }
                }
            }
        } else {
            size_t row_s = (size_t)fr->layout.width * (size_t)ch;
            for (size_t k = 0u; k < row_s; k++) {
                uint16_t v = 0u;
                memcpy(&v, row + k * 2u, 2u);
                if ((v & pad_mask) != 0u) {
                    return RFC_ERR_UNSUPPORTED_SAMPLE;
                }
                if (has_alpha && all_opaque && (k & 3u) == 3u) {
                    if ((uint16_t)(v >> shift) != max_val) {
                        all_opaque = 0;
                    }
                }
            }
        }
    }

    const uint8_t *eff_icc = NULL;
    size_t eff_icc_len = 0u;
    const uint8_t *eff_exif = NULL;
    size_t eff_exif_len = 0u;
    const uint8_t *eff_xmp = NULL;
    size_t eff_xmp_len = 0u;
    rfc_orientation eff_ori = RFC_ORIENTATION_UNSPECIFIED;
    rfc_colorimetry eff_color;
    memset(&eff_color, 0, sizeof(eff_color));

    if (fr->metadata.icc_len > 0u && fr->metadata.icc != NULL) {
        eff_icc = fr->metadata.icc;
        eff_icc_len = fr->metadata.icc_len;
    } else if (meta && meta->icc_len > 0u && meta->icc != NULL) {
        eff_icc = meta->icc;
        eff_icc_len = meta->icc_len;
    }
    if (fr->metadata.exif_len > 0u && fr->metadata.exif != NULL) {
        eff_exif = fr->metadata.exif;
        eff_exif_len = fr->metadata.exif_len;
    } else if (meta && meta->exif_len > 0u && meta->exif != NULL) {
        eff_exif = meta->exif;
        eff_exif_len = meta->exif_len;
    }
    if (fr->metadata.xmp_len > 0u && fr->metadata.xmp != NULL) {
        eff_xmp = fr->metadata.xmp;
        eff_xmp_len = fr->metadata.xmp_len;
    } else if (meta && meta->xmp_len > 0u && meta->xmp != NULL) {
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

    rfc_orientation resolved_ori =
        (eff_ori == RFC_ORIENTATION_UNSPECIFIED) ? RFC_ORIENTATION_NORMAL : eff_ori;
    if (meta_policy == RFC_META_POLICY_PRESERVE && eff_exif_len > 0u) {
        if (!avif_exif_magic_strict(eff_exif, eff_exif_len)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        rfc_orientation exif_ori = RFC_ORIENTATION_UNSPECIFIED;
        st = rfc_exif_parse_orientation(eff_exif, eff_exif_len, &exif_ori);
        if (st != RFC_OK) {
            return st;
        }
        if (exif_ori != RFC_ORIENTATION_UNSPECIFIED) {
            if (eff_ori != RFC_ORIENTATION_UNSPECIFIED && eff_ori != exif_ori) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            resolved_ori = exif_ori;
        }
    }

    *out_has_alpha = has_alpha;
    *out_all_opaque_alpha = has_alpha ? all_opaque : 0;
    *out_bps = b;
    *out_icc = eff_icc;
    *out_icc_len = eff_icc_len;
    *out_ori = resolved_ori;
    *out_color = eff_color;
    *out_exif = eff_exif;
    *out_exif_len = eff_exif_len;
    *out_xmp = eff_xmp;
    *out_xmp_len = eff_xmp_len;
    return RFC_OK;
}

static rfc_status avif_fill_yuv_from_frame(const rfc_frame *fr,
                                           int has_alpha,
                                           uint32_t bps,
                                           avifImage *img) {
    avifResult res = avifImageAllocatePlanes(
        img, has_alpha ? AVIF_PLANES_ALL : AVIF_PLANES_YUV);
    if (res != AVIF_RESULT_OK) {
        return res == AVIF_RESULT_OUT_OF_MEMORY ? RFC_ERR_OUT_OF_MEMORY
                                                : RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t w = fr->layout.width;
    uint32_t h = fr->layout.height;
    uint32_t ch = has_alpha ? 4u : 3u;
    size_t bpc = (bps > 8u) ? 2u : 1u;
    size_t min_row = (size_t)w * (size_t)ch * bpc;
    size_t stride = fr->layout.stride_bytes ? fr->layout.stride_bytes : min_row;
    uint32_t shift = (bps > 8u) ? (16u - bps) : 0u;

    for (uint32_t y = 0u; y < h; y++) {
        const uint8_t *src = fr->data + (size_t)y * stride;
        uint8_t *py = img->yuvPlanes[AVIF_CHAN_Y] + (size_t)y * img->yuvRowBytes[AVIF_CHAN_Y];
        uint8_t *pu = img->yuvPlanes[AVIF_CHAN_U] + (size_t)y * img->yuvRowBytes[AVIF_CHAN_U];
        uint8_t *pv = img->yuvPlanes[AVIF_CHAN_V] + (size_t)y * img->yuvRowBytes[AVIF_CHAN_V];
        uint8_t *pa = has_alpha ? (img->alphaPlane + (size_t)y * img->alphaRowBytes) : NULL;
        if (bps == 8u) {
            for (uint32_t x = 0u; x < w; x++) {
                pv[x] = src[(size_t)x * ch + 0u];
                py[x] = src[(size_t)x * ch + 1u];
                pu[x] = src[(size_t)x * ch + 2u];
                if (has_alpha) {
                    pa[x] = src[(size_t)x * ch + 3u];
                }
            }
        } else {
            uint16_t *wy = (uint16_t *)py;
            uint16_t *wu = (uint16_t *)pu;
            uint16_t *wv = (uint16_t *)pv;
            uint16_t *wa = (uint16_t *)pa;
            for (uint32_t x = 0u; x < w; x++) {
                uint16_t r = 0u, g = 0u, b = 0u;
                memcpy(&r, src + ((size_t)x * ch + 0u) * 2u, 2u);
                memcpy(&g, src + ((size_t)x * ch + 1u) * 2u, 2u);
                memcpy(&b, src + ((size_t)x * ch + 2u) * 2u, 2u);
                wv[x] = (uint16_t)(r >> shift);
                wy[x] = (uint16_t)(g >> shift);
                wu[x] = (uint16_t)(b >> shift);
                if (has_alpha) {
                    uint16_t a = 0u;
                    memcpy(&a, src + ((size_t)x * ch + 3u) * 2u, 2u);
                    wa[x] = (uint16_t)(a >> shift);
                }
            }
        }
    }
    return RFC_OK;
}

static rfc_status avif_self_verify_encoded(void *session,
                                           const rfc_exec_context *exec,
                                           const rfc_frame *src_frame,
                                           const rfc_metadata *src_meta,
                                           rfc_metadata_policy meta_policy,
                                           const uint8_t *enc_bytes,
                                           size_t enc_len) {
    rfc_frame *dec_frames = NULL;
    size_t dec_count = 0u;
    rfc_metadata dec_meta;
    memset(&dec_meta, 0, sizeof(dec_meta));
    rfc_status st = avif_decode_common(session, exec, enc_bytes, enc_len, 1,
                                       NULL, &dec_frames, &dec_count, &dec_meta);
    if (st != RFC_OK) {
        return (st == RFC_ERR_CANCELLED || st == RFC_ERR_OUT_OF_MEMORY ||
                st == RFC_ERR_LIMIT_EXCEEDED)
                   ? st
                   : RFC_ERR_VERIFY_MISMATCH;
    }

    const rfc_allocator *alloc = avif_alloc_of(exec);
    rfc_sample_mismatch mm;
    memset(&mm, 0, sizeof(mm));
    st = rfc_verify_frames_equal(src_frame, 1u, dec_frames, dec_count, &mm);
    if (st == RFC_OK) {
        rfc_metadata_report rep;
        memset(&rep, 0, sizeof(rep));
        const rfc_metadata *eff_meta = src_meta ? src_meta : &src_frame->metadata;
        st = rfc_metadata_verify(
            RFC_FORMAT_AVIF, meta_policy, RFC_EXACT_PIXEL_EXACT, eff_meta,
            src_frame, 1u, &dec_meta, dec_frames, dec_count,
            (exec && exec->job) ? rfc_job_acknowledged_metadata(exec->job) : 0u,
            &rep, &mm);
        if (st == RFC_ERR_METADATA_POLICY) {
            /* Standalone direct calls reject unacknowledged unsupported items
             * upfront; if acknowledged via job, preserve report pass above. */
        }
    }

    if (dec_frames) {
        int aliased_icc =
            (dec_count == 1u && dec_meta.icc == dec_frames[0].metadata.icc);
        int aliased_exif =
            (dec_count == 1u && dec_meta.exif == dec_frames[0].metadata.exif);
        int aliased_xmp =
            (dec_count == 1u && dec_meta.xmp == dec_frames[0].metadata.xmp);
        if (aliased_icc) {
            dec_meta.icc = NULL;
            dec_meta.icc_len = 0u;
        }
        if (aliased_exif) {
            dec_meta.exif = NULL;
            dec_meta.exif_len = 0u;
        }
        if (aliased_xmp) {
            dec_meta.xmp = NULL;
            dec_meta.xmp_len = 0u;
        }
        for (size_t i = 0u; i < dec_count; i++) {
            rfc_frame_free(&dec_frames[i]);
        }
        alloc->free(alloc->ctx, dec_frames);
    }
    rfc_metadata_free(&dec_meta, alloc);
    return st;
}

/* WORKAROUND: frozen libavif 1.4.2 rejects "mdcv" in avifImageAddOpaqueProperty
 * via knownProperties[] while write.c omits native mDCV serialization; emit as
 * "MDCV" opaque property and patch the 4CC inside meta/iprp/ipco in place. */
static rfc_status avif_patch_mdcv_fourcc_in_ipco(uint8_t *data,
                                                 size_t len,
                                                 const uint8_t mdcv_box[24]) {
    size_t pos = 0u;
    int patched = 0;
    while (pos < len) {
        isobmff_box_view top;
        rfc_status st = isobmff_next_box(data, len, pos, &top);
        if (st != RFC_OK) {
            return st;
        }
        if (memcmp(top.type, "meta", 4) == 0 && top.pay_len >= 4u) {
            size_t mpos = top.pay_off + 4u;
            size_t mend = top.pay_off + top.pay_len;
            while (mpos < mend) {
                isobmff_box_view sub;
                st = isobmff_next_box(data, mend, mpos, &sub);
                if (st != RFC_OK) {
                    return st;
                }
                if (memcmp(sub.type, "iprp", 4) == 0) {
                    size_t ipos = sub.pay_off;
                    size_t iend = sub.pay_off + sub.pay_len;
                    while (ipos < iend) {
                        isobmff_box_view ib;
                        st = isobmff_next_box(data, iend, ipos, &ib);
                        if (st != RFC_OK) {
                            return st;
                        }
                        if (memcmp(ib.type, "ipco", 4) == 0) {
                            size_t cpos = ib.pay_off;
                            size_t cend = ib.pay_off + ib.pay_len;
                            while (cpos < cend) {
                                isobmff_box_view prop;
                                st = isobmff_next_box(data, cend, cpos, &prop);
                                if (st != RFC_OK) {
                                    return st;
                                }
                                if (memcmp(prop.type, "MDCV", 4) == 0 &&
                                    prop.pay_len == 24u && prop.pay_off >= 4u &&
                                    memcmp(data + prop.pay_off, mdcv_box,
                                           24u) == 0) {
                                    memcpy(data + prop.pay_off - 4u, "mdcv", 4u);
                                    patched++;
                                }
                                cpos = prop.next_off;
                            }
                        }
                        ipos = ib.next_off;
                    }
                }
                mpos = sub.next_off;
            }
        }
        pos = top.next_off;
    }
    return patched == 1 ? RFC_OK : RFC_ERR_INVALID_ARGUMENT;
}

static rfc_status avif_encode_session(void *session,
                                      const rfc_exec_context *exec,
                                      const rfc_frame *frames,
                                      size_t frame_count,
                                      const rfc_metadata *meta,
                                      const rfc_setting_value *settings,
                                      size_t setting_count,
                                      rfc_encoded *out_encoded) {
    if (!out_encoded) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_encoded, 0, sizeof(*out_encoded));
    if (avif_is_cancelled(session, exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_status st = rfc_registry_validate_settings(
        RFC_FORMAT_AVIF, setting_count > 0u ? settings : NULL, setting_count,
        NULL);
    if (st != RFC_OK) {
        return st;
    }

    rfc_metadata_policy meta_policy = RFC_META_POLICY_PRESERVE;
    rfc_exactness_class exactness = RFC_EXACT_PIXEL_EXACT;
    for (size_t i = 0u; i < setting_count; i++) {
        if (!settings[i].id || !settings[i].value) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(settings[i].id, "metadata") == 0) {
            st = rfc_metadata_policy_parse(settings[i].value, &meta_policy);
            if (st != RFC_OK) {
                return st;
            }
        } else if (strcmp(settings[i].id, "exactness") == 0) {
            st = rfc_exactness_class_parse(settings[i].value, &exactness);
            if (st != RFC_OK) {
                return st;
            }
        }
    }
    if (exactness == RFC_EXACT_BYTE_IDENTICAL && (!exec || !exec->job)) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }

    if (frame_count == 0u || !frames) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count != 1u) {
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }

    rfc_limits lim_def;
    const rfc_limits *limits = avif_limits_of(exec, &lim_def);
    const rfc_allocator *alloc = avif_alloc_of(exec);

    st = rfc_limits_check_frames(limits, frames, frame_count, meta, 0u, 0);
    if (st != RFC_OK) {
        return st;
    }

    const rfc_frame *fr = &frames[0];
    int has_alpha = 0, all_opaque_alpha = 0;
    uint32_t eff_bps = 0u;
    const uint8_t *eff_icc = NULL, *eff_exif = NULL, *eff_xmp = NULL;
    size_t eff_icc_len = 0u, eff_exif_len = 0u, eff_xmp_len = 0u;
    rfc_orientation eff_ori = RFC_ORIENTATION_NORMAL;
    rfc_colorimetry eff_color;
    st = avif_validate_encode_input(
        fr, meta, meta_policy, limits, &has_alpha, &all_opaque_alpha, &eff_bps,
        &eff_icc, &eff_icc_len, &eff_ori, &eff_color, &eff_exif, &eff_exif_len,
        &eff_xmp, &eff_xmp_len);
    if (st != RFC_OK) {
        return st;
    }

    if (!exec || !exec->job) {
        st = rfc_metadata_check_source_preflight(
            RFC_FORMAT_AVIF, meta_policy, exactness, meta, frames, frame_count,
            0u, NULL);
        if (st != RFC_OK) {
            return st;
        }
    }

    uint32_t eff_thr = exec ? rfc_exec_threads(exec) : 1u;
    if (eff_thr == 0u) {
        eff_thr = 1u;
    } else if (eff_thr > 64u) {
        eff_thr = 64u;
    }

    uint64_t pixels = (uint64_t)fr->layout.width * (uint64_t)fr->layout.height;
    size_t enc_work = 0u;
    if (!avif_stage_work_bytes(pixels, eff_bps, has_alpha, eff_thr, 1,
                               eff_icc_len + eff_exif_len + eff_xmp_len,
                               &enc_work)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    avif_budget_guard bg;
    st = avif_reserve_budget(exec, enc_work, &bg);
    if (st != RFC_OK) {
        return st;
    }

    avifImage *img = avifImageCreate(fr->layout.width, fr->layout.height,
                                     eff_bps, AVIF_PIXEL_FORMAT_YUV444);
    if (!img) {
        avif_release_budget(&bg);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    img->yuvRange = AVIF_RANGE_FULL;
    img->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
    img->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    img->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    img->alphaPremultiplied = AVIF_FALSE;

    uint8_t mdcv_box[24];
    int wrote_mdcv = 0;
    if (meta_policy == RFC_META_POLICY_PRESERVE) {
        if (eff_icc && eff_icc_len > 0u) {
            if (avifImageSetProfileICC(img, eff_icc, eff_icc_len) !=
                AVIF_RESULT_OK) {
                avifImageDestroy(img);
                avif_release_budget(&bg);
                return RFC_ERR_OUT_OF_MEMORY;
            }
        } else if (eff_color.has_cicp && eff_color.cicp[2] == 0u &&
                   eff_color.cicp[3] == 1u &&
                   (eff_color.cicp[0] != 2u || eff_color.cicp[1] != 2u)) {
            img->colorPrimaries = (avifColorPrimaries)eff_color.cicp[0];
            img->transferCharacteristics =
                (avifTransferCharacteristics)eff_color.cicp[1];
        }
        if (eff_color.has_clli && (eff_color.clli_max_cll % 10000u) == 0u &&
            (eff_color.clli_max_fall % 10000u) == 0u &&
            eff_color.clli_max_cll <= 655350000u &&
            eff_color.clli_max_fall <= 655350000u &&
            (eff_color.clli_max_cll != 0u || eff_color.clli_max_fall != 0u)) {
            img->clli.maxCLL = (uint16_t)(eff_color.clli_max_cll / 10000u);
            img->clli.maxPALL = (uint16_t)(eff_color.clli_max_fall / 10000u);
        }
        if (eff_color.has_mdcv) {
            avif_wr16be(mdcv_box + 0u, eff_color.mdcv_xy[2]);  /* Gx */
            avif_wr16be(mdcv_box + 2u, eff_color.mdcv_xy[3]);  /* Gy */
            avif_wr16be(mdcv_box + 4u, eff_color.mdcv_xy[4]);  /* Bx */
            avif_wr16be(mdcv_box + 6u, eff_color.mdcv_xy[5]);  /* By */
            avif_wr16be(mdcv_box + 8u, eff_color.mdcv_xy[0]);  /* Rx */
            avif_wr16be(mdcv_box + 10u, eff_color.mdcv_xy[1]); /* Ry */
            avif_wr16be(mdcv_box + 12u, eff_color.mdcv_xy[6]); /* Wx */
            avif_wr16be(mdcv_box + 14u, eff_color.mdcv_xy[7]); /* Wy */
            avif_wr32be(mdcv_box + 16u, eff_color.mdcv_max_dl);
            avif_wr32be(mdcv_box + 20u, eff_color.mdcv_min_dl);
            if (avifImageAddOpaqueProperty(img, (const uint8_t *)"MDCV",
                                           mdcv_box, sizeof(mdcv_box)) !=
                AVIF_RESULT_OK) {
                avifImageDestroy(img);
                avif_release_budget(&bg);
                return RFC_ERR_OUT_OF_MEMORY;
            }
            wrote_mdcv = 1;
        }
        if (eff_xmp && eff_xmp_len > 0u) {
            if (avifImageSetMetadataXMP(img, eff_xmp, eff_xmp_len) !=
                AVIF_RESULT_OK) {
                avifImageDestroy(img);
                avif_release_budget(&bg);
                return RFC_ERR_OUT_OF_MEMORY;
            }
        }
        if (eff_exif && eff_exif_len > 0u) {
            if (avifImageSetMetadataExif(img, eff_exif, eff_exif_len) !=
                AVIF_RESULT_OK) {
                avifImageDestroy(img);
                avif_release_budget(&bg);
                return RFC_ERR_OUT_OF_MEMORY;
            }
        }
        avif_apply_ori_to_image(img, eff_ori);
    }

    st = avif_fill_yuv_from_frame(fr, has_alpha, eff_bps, img);
    if (st != RFC_OK) {
        avifImageDestroy(img);
        avif_release_budget(&bg);
        return st;
    }
    if (avif_is_cancelled(session, exec)) {
        avifImageDestroy(img);
        avif_release_budget(&bg);
        return RFC_ERR_CANCELLED;
    }

    avifEncoder *enc = avifEncoderCreate();
    if (!enc) {
        avifImageDestroy(img);
        avif_release_budget(&bg);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    enc->codecChoice = AVIF_CODEC_CHOICE_AOM;
    enc->maxThreads = (int)eff_thr;
    enc->speed = 6;
    enc->quality = AVIF_QUALITY_LOSSLESS;
    enc->qualityAlpha = AVIF_QUALITY_LOSSLESS;
    enc->minQuantizer = AVIF_QUANTIZER_LOSSLESS;
    enc->maxQuantizer = AVIF_QUANTIZER_LOSSLESS;
    enc->minQuantizerAlpha = AVIF_QUANTIZER_LOSSLESS;
    enc->maxQuantizerAlpha = AVIF_QUANTIZER_LOSSLESS;
    /* libavif omits the auxiliary alpha item under AVIF_ADD_IMAGE_FLAG_SINGLE
     * when every alpha pixel is opaque; pass FORCE_KEYFRAME without SINGLE on
     * all-opaque RGBA inputs so RGBA8/RGBA16 retains its alpha plane. */
    avifAddImageFlags add_flags =
        (has_alpha && all_opaque_alpha) ? AVIF_ADD_IMAGE_FLAG_FORCE_KEYFRAME
                                        : AVIF_ADD_IMAGE_FLAG_SINGLE;

    avifRWData raw = AVIF_DATA_EMPTY;
    avifResult eres = avifEncoderAddImage(enc, img, 1u, add_flags);
    if (eres == AVIF_RESULT_OK) {
        if (avif_is_cancelled(session, exec)) {
            avifEncoderDestroy(enc);
            avifImageDestroy(img);
            avif_release_budget(&bg);
            return RFC_ERR_CANCELLED;
        }
        eres = avifEncoderFinish(enc, &raw);
    }
    avifEncoderDestroy(enc);
    avifImageDestroy(img);
    if (eres != AVIF_RESULT_OK) {
        avifRWDataFree(&raw);
        avif_release_budget(&bg);
        if (avif_is_cancelled(session, exec)) {
            return RFC_ERR_CANCELLED;
        }
        return eres == AVIF_RESULT_OUT_OF_MEMORY ? RFC_ERR_OUT_OF_MEMORY
                                                 : RFC_ERR_INVALID_ARGUMENT;
    }

    if (avif_is_cancelled(session, exec)) {
        avifRWDataFree(&raw);
        avif_release_budget(&bg);
        return RFC_ERR_CANCELLED;
    }
    if (wrote_mdcv) {
        st = avif_patch_mdcv_fourcc_in_ipco(raw.data, raw.size, mdcv_box);
        if (st != RFC_OK) {
            avifRWDataFree(&raw);
            avif_release_budget(&bg);
            return st;
        }
    }
    st = rfc_limits_check_compressed(limits, (uint64_t)raw.size);
    if (st != RFC_OK) {
        avifRWDataFree(&raw);
        avif_release_budget(&bg);
        return st;
    }

    avif_release_budget(&bg);
    st = avif_self_verify_encoded(session, exec, fr, meta, meta_policy,
                                  raw.data, raw.size);
    if (st != RFC_OK) {
        avifRWDataFree(&raw);
        return st;
    }

    size_t out_len = raw.size;
    uint8_t *out_buf = (uint8_t *)alloc->alloc(alloc->ctx, out_len ? out_len : 1u);
    if (!out_buf) {
        avifRWDataFree(&raw);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (out_len > 0u) {
        memcpy(out_buf, raw.data, out_len);
    }
    avifRWDataFree(&raw);

    out_encoded->data = out_buf;
    out_encoded->len = out_len;
    out_encoded->allocator = alloc;
    return RFC_OK;
}

/* ------------------------------------------------------------------------ */
/* Session lifecycle and registry wiring                                    */
/* ------------------------------------------------------------------------ */

static rfc_status avif_open_session(const rfc_exec_context *exec,
                                    void **out_session) {
    (void)exec;
    if (!out_session) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_avif_session_box *s = (rfc_avif_session_box *)calloc(1u, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&s->cancelled, 0);
    *out_session = s;
    return RFC_OK;
}

static void avif_close_session(void *session) {
    free(session);
}

static void avif_cancel_session(void *session) {
    if (!session) {
        return;
    }
    rfc_avif_session_box *s = (rfc_avif_session_box *)session;
    atomic_store_explicit(&s->cancelled, 1, memory_order_release);
}

static const rfc_session_adapter k_avif_session_adapter = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_AVIF,
    avif_open_session,
    avif_close_session,
    avif_probe_session,
    avif_encode_session,
    avif_decode_session,
    avif_cancel_session,
    NULL
};

rfc_status rfc_codecs_register_avif(void) {
    return rfc_registry_register_session(&k_avif_session_adapter);
}

rfc_status rfc_codecs_unregister_avif(void) {
    return rfc_registry_unregister_session(RFC_FORMAT_AVIF);
}
