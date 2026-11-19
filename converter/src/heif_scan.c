/* Bounded ISOBMFF reader for HEIF and HEIC (see rfc_heif_scan.h).
 *
 * ISO/IEC 14496-12 boxes (ftyp, meta, hdlr, pitm, iinf/infe, iref, iprp,
 * ipco, ipma, moov) and ISO/IEC 23008-12 item properties (ispe, pixi, clap,
 * irot, imir, colr, auxC, hvcC, av1C). The top-level boxes are walked with
 * positional reads; the meta box is read once and parsed in memory, each
 * field checked against the box that holds it; the item, reference,
 * property and association tables are built once and searched by item ID. */

#include "rfc_heif_scan.h"
#include "rfc_job.h"

#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d)                                                     \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) |        \
     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

#define MAX_TOP_BOXES 4096u

typedef struct span {
    size_t s, e; /* payload [s, e) inside the meta buffer */
} span;

typedef struct item {
    uint32_t id, type;
    uint8_t is_xmp;
    uint8_t ref_from_thmb_auxl; /* a thumbnail or auxiliary of something */
    uint8_t ref_to_dimg;        /* an input of a derived image */
    uint32_t assoc_first, assoc_count; /* into the sorted association table */
} item;

typedef struct ref {
    uint32_t type, from, to;
} ref;

typedef struct prop {
    uint32_t type;
    span p;
} prop;

typedef struct assoc {
    uint32_t item, index; /* index: 1-based into props */
    uint32_t order;       /* stable order of appearance */
} assoc;

typedef struct heif_ctx {
    const uint8_t *m; /* the meta box payload, after its FullBox header */
    size_t mn;
    uint64_t m_file_off; /* file offset of m[0] */
    item *items;
    uint32_t n_items;
    ref *refs;
    uint32_t n_refs;
    prop *props;
    uint32_t n_props;
    assoc *assocs;
    uint32_t n_assocs;
    uint32_t primary;
    const char *detail;
} heif_ctx;

static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static rfc_status refuse(heif_ctx *c, rfc_status st, const char *detail) {
    c->detail = detail;
    return st;
}

/* ---- positional reads for the top level -------------------------------- */

typedef struct src {
    rfc_heif_read_fn read_at;
    void *ctx;
    uint64_t size; /* RFC_HEIF_SIZE_UNKNOWN when not known */
    int read_failed;
} src;

/* Read n bytes at off; *got may be short only at the end of the input. */
static rfc_status src_read(src *r, uint64_t off, uint8_t *dst, size_t n,
                           size_t *got) {
    *got = 0u;
    if (r->size != RFC_HEIF_SIZE_UNKNOWN) {
        if (off >= r->size) {
            return RFC_OK;
        }
        if (n > r->size - off) {
            n = (size_t)(r->size - off);
        }
    }
    size_t total = 0u;
    while (total < n) {
        size_t g = 0u;
        rfc_status st = r->read_at(r->ctx, off + total, dst + total, n - total, &g);
        if (st != RFC_OK) {
            r->read_failed = 1;
            return st;
        }
        if (g == 0u) {
            break;
        }
        total += g;
    }
    *got = total;
    return RFC_OK;
}

/* ---- boxes in memory ---------------------------------------------------- */

/* The box at pos in [pos, end): its type and payload span. */
static rfc_status mbox(const uint8_t *m, size_t pos, size_t end, uint32_t *type,
                       span *payload, size_t *next) {
    if (end < pos || end - pos < 8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t size = be32(m + pos);
    *type = be32(m + pos + 4);
    size_t hdr = 8u;
    if (size == 1u) {
        if (end - pos < 16u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        size = ((uint64_t)be32(m + pos + 8) << 32) | be32(m + pos + 12);
        hdr = 16u;
    } else if (size == 0u) {
        size = end - pos;
    }
    if (*type == FOURCC('u', 'u', 'i', 'd')) {
        hdr += 16u;
    }
    if (size < hdr || size > end - pos) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    payload->s = pos + hdr;
    payload->e = pos + (size_t)size;
    *next = payload->e;
    return RFC_OK;
}

/* A FullBox: version and flags, the content after them in *body. */
static rfc_status fullbox(const uint8_t *m, span p, uint32_t *version,
                          uint32_t *flags, span *body) {
    if (p.e - p.s < 4u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *version = m[p.s];
    if (flags) {
        *flags = ((uint32_t)m[p.s + 1] << 16) | ((uint32_t)m[p.s + 2] << 8) | m[p.s + 3];
    }
    body->s = p.s + 4u;
    body->e = p.e;
    return RFC_OK;
}

/* A NUL-terminated string at *pos inside [*pos, end); *pos moves past it. */
static rfc_status cstr(const uint8_t *m, size_t *pos, size_t end,
                       const uint8_t **out, size_t *len) {
    if (*pos >= end) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const uint8_t *z = (const uint8_t *)memchr(m + *pos, 0, end - *pos);
    if (!z) {
        return RFC_ERR_INVALID_ARGUMENT; /* unterminated */
    }
    *out = m + *pos;
    *len = (size_t)(z - (m + *pos));
    *pos += *len + 1u;
    return RFC_OK;
}

/* ---- tables ------------------------------------------------------------- */

static rfc_status parse_iinf(heif_ctx *c, span p) {
    uint32_t version = 0u;
    span b;
    rfc_status st = fullbox(c->m, p, &version, NULL, &b);
    if (st != RFC_OK) {
        return st;
    }
    size_t q = b.s;
    uint32_t count = 0u;
    if (version == 0u) {
        if (b.e - q < 2u) return RFC_ERR_INVALID_ARGUMENT;
        count = be16(c->m + q);
        q += 2u;
    } else {
        if (b.e - q < 4u) return RFC_ERR_INVALID_ARGUMENT;
        count = be32(c->m + q);
        q += 4u;
    }
    if (count > RFC_HEIF_MAX_ITEMS) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    while (q < b.e) {
        uint32_t type = 0u;
        span ip;
        size_t next = 0u;
        st = mbox(c->m, q, b.e, &type, &ip, &next);
        if (st != RFC_OK) {
            return st;
        }
        q = next;
        if (type != FOURCC('i', 'n', 'f', 'e')) {
            continue;
        }
        if (c->n_items >= RFC_HEIF_MAX_ITEMS) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        uint32_t iv = 0u;
        span ib;
        st = fullbox(c->m, ip, &iv, NULL, &ib);
        if (st != RFC_OK) {
            return st;
        }
        item it;
        memset(&it, 0, sizeof(it));
        size_t r = ib.s;
        const uint8_t *str = NULL;
        size_t sl = 0u;
        if (iv < 2u) {
            if (ib.e - r < 4u) return RFC_ERR_INVALID_ARGUMENT;
            it.id = be16(c->m + r);
            r += 4u; /* item_ID, item_protection_index */
            it.type = FOURCC('m', 'i', 'm', 'e');
            st = cstr(c->m, &r, ib.e, &str, &sl); /* item_name */
        } else {
            size_t idsz = iv == 2u ? 2u : 4u;
            if (ib.e - r < idsz + 6u) return RFC_ERR_INVALID_ARGUMENT;
            it.id = idsz == 2u ? be16(c->m + r) : be32(c->m + r);
            r += idsz + 2u; /* item_ID, item_protection_index */
            it.type = be32(c->m + r);
            r += 4u;
            st = cstr(c->m, &r, ib.e, &str, &sl); /* item_name */
        }
        if (st != RFC_OK) {
            return st;
        }
        if (it.type == FOURCC('m', 'i', 'm', 'e')) {
            st = cstr(c->m, &r, ib.e, &str, &sl); /* content_type */
            if (st != RFC_OK) {
                return st;
            }
            it.is_xmp = sl == 19u && memcmp(str, "application/rdf+xml", 19u) == 0;
        }
        c->items[c->n_items++] = it;
    }
    return RFC_OK;
}

static rfc_status parse_iref(heif_ctx *c, span p) {
    uint32_t version = 0u;
    span b;
    rfc_status st = fullbox(c->m, p, &version, NULL, &b);
    if (st != RFC_OK) {
        return st;
    }
    size_t idsz = version == 0u ? 2u : 4u;
    size_t q = b.s;
    while (q < b.e) {
        uint32_t type = 0u;
        span rp;
        size_t next = 0u;
        st = mbox(c->m, q, b.e, &type, &rp, &next);
        if (st != RFC_OK) {
            return st;
        }
        q = next;
        size_t r = rp.s;
        if (rp.e - r < idsz + 2u) return RFC_ERR_INVALID_ARGUMENT;
        uint32_t from = idsz == 2u ? be16(c->m + r) : be32(c->m + r);
        r += idsz;
        uint32_t count = be16(c->m + r);
        r += 2u;
        if ((uint64_t)count * idsz > rp.e - r) return RFC_ERR_INVALID_ARGUMENT;
        for (uint32_t k = 0u; k < count; k++, r += idsz) {
            if (c->n_refs >= RFC_HEIF_MAX_REFERENCES) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            uint32_t to = idsz == 2u ? be16(c->m + r) : be32(c->m + r);
            c->refs[c->n_refs++] = (ref){type, from, to};
        }
    }
    return RFC_OK;
}

static rfc_status parse_ipco(heif_ctx *c, span p) {
    size_t q = p.s;
    while (q < p.e) {
        uint32_t type = 0u;
        span pp;
        size_t next = 0u;
        rfc_status st = mbox(c->m, q, p.e, &type, &pp, &next);
        if (st != RFC_OK) {
            return st;
        }
        q = next;
        if (c->n_props >= RFC_HEIF_MAX_PROPERTIES) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        c->props[c->n_props++] = (prop){type, pp};
    }
    return RFC_OK;
}

static rfc_status parse_ipma(heif_ctx *c, span p) {
    uint32_t version = 0u, flags = 0u;
    span b;
    rfc_status st = fullbox(c->m, p, &version, &flags, &b);
    if (st != RFC_OK) {
        return st;
    }
    size_t q = b.s;
    if (b.e - q < 4u) return RFC_ERR_INVALID_ARGUMENT;
    uint32_t entries = be32(c->m + q);
    q += 4u;
    size_t idsz = version < 1u ? 2u : 4u;
    size_t asz = (flags & 1u) ? 2u : 1u;
    for (uint32_t e = 0u; e < entries; e++) {
        if (b.e - q < idsz + 1u) return RFC_ERR_INVALID_ARGUMENT;
        uint32_t id = idsz == 2u ? be16(c->m + q) : be32(c->m + q);
        q += idsz;
        uint32_t n = c->m[q++];
        if ((uint64_t)n * asz > b.e - q) return RFC_ERR_INVALID_ARGUMENT;
        for (uint32_t a = 0u; a < n; a++, q += asz) {
            uint32_t index = asz == 2u ? (be16(c->m + q) & 0x7FFFu) : (c->m[q] & 0x7Fu);
            if (index == 0u) {
                continue;
            }
            if (c->n_assocs >= RFC_HEIF_MAX_ASSOCIATIONS) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            c->assocs[c->n_assocs] = (assoc){id, index, c->n_assocs};
            c->n_assocs++;
        }
    }
    return RFC_OK;
}

static rfc_status parse_iprp(heif_ctx *c, span p) {
    size_t q = p.s;
    int ipco_seen = 0;
    while (q < p.e) {
        uint32_t type = 0u;
        span cp;
        size_t next = 0u;
        rfc_status st = mbox(c->m, q, p.e, &type, &cp, &next);
        if (st != RFC_OK) {
            return st;
        }
        q = next;
        if (type == FOURCC('i', 'p', 'c', 'o')) {
            if (ipco_seen) {
                return RFC_ERR_INVALID_ARGUMENT; /* one ipco */
            }
            ipco_seen = 1;
            st = parse_ipco(c, cp);
        } else if (type == FOURCC('i', 'p', 'm', 'a')) {
            st = parse_ipma(c, cp); /* every ipma, in order */
        }
        if (st != RFC_OK) {
            return st;
        }
    }
    return RFC_OK;
}

static int item_cmp(const void *a, const void *b) {
    uint32_t x = ((const item *)a)->id, y = ((const item *)b)->id;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int assoc_cmp(const void *a, const void *b) {
    const assoc *x = (const assoc *)a, *y = (const assoc *)b;
    if (x->item != y->item) return x->item < y->item ? -1 : 1;
    return x->order < y->order ? -1 : (x->order > y->order ? 1 : 0);
}

static item *find_item(heif_ctx *c, uint32_t id) {
    item key;
    memset(&key, 0, sizeof(key));
    key.id = id;
    return (item *)bsearch(&key, c->items, c->n_items, sizeof(item), item_cmp);
}

/* Index once: items by ID, associations by item, reference flags. */
static rfc_status index_tables(heif_ctx *c) {
    qsort(c->items, c->n_items, sizeof(item), item_cmp);
    for (uint32_t i = 1u; i < c->n_items; i++) {
        if (c->items[i].id == c->items[i - 1u].id) {
            return RFC_ERR_INVALID_ARGUMENT; /* duplicate item ID */
        }
    }
    qsort(c->assocs, c->n_assocs, sizeof(assoc), assoc_cmp);
    for (uint32_t a = 0u; a < c->n_assocs; a++) {
        if (c->assocs[a].index > c->n_props) {
            return RFC_ERR_INVALID_ARGUMENT; /* index past ipco */
        }
        item *it = find_item(c, c->assocs[a].item);
        if (!it) {
            continue; /* associations of unknown items are ignored */
        }
        if (it->assoc_count == 0u) {
            it->assoc_first = a;
        }
        it->assoc_count++;
    }
    for (uint32_t r = 0u; r < c->n_refs; r++) {
        const ref *rf = &c->refs[r];
        if (rf->type == FOURCC('t', 'h', 'm', 'b') || rf->type == FOURCC('a', 'u', 'x', 'l')) {
            item *f = find_item(c, rf->from);
            if (f) f->ref_from_thmb_auxl = 1u;
        } else if (rf->type == FOURCC('d', 'i', 'm', 'g')) {
            item *t = find_item(c, rf->to);
            if (t) t->ref_to_dimg = 1u;
        }
    }
    return RFC_OK;
}

/* The first property of `type` associated with `it`, in association order. */
static const prop *prop_of(heif_ctx *c, const item *it, uint32_t type) {
    for (uint32_t a = 0u; a < it->assoc_count; a++) {
        const prop *p = &c->props[c->assocs[it->assoc_first + a].index - 1u];
        if (p->type == type) {
            return p;
        }
    }
    return NULL;
}

/* ---- the scan ----------------------------------------------------------- */

static int is_image_type(uint32_t t) {
    return t == FOURCC('h', 'v', 'c', '1') || t == FOURCC('a', 'v', '0', '1') ||
           t == FOURCC('g', 'r', 'i', 'd') || t == FOURCC('i', 'd', 'e', 'n') ||
           t == FOURCC('i', 'o', 'v', 'l') || t == FOURCC('j', 'p', 'e', 'g') ||
           t == FOURCC('a', 'v', 'c', '1') || t == FOURCC('u', 'n', 'c', 'i');
}

/* Largest significant depth of any channel of `it` (8 when unstated). */
static rfc_status item_depth(heif_ctx *c, const item *it, uint32_t *depth) {
    uint32_t d = 0u;
    const prop *p = prop_of(c, it, FOURCC('p', 'i', 'x', 'i'));
    if (p) {
        uint32_t v = 0u;
        span b;
        rfc_status st = fullbox(c->m, p->p, &v, NULL, &b);
        if (st != RFC_OK) return st;
        if (b.e - b.s < 1u) return RFC_ERR_INVALID_ARGUMENT;
        uint32_t n = c->m[b.s];
        if (n == 0u || b.e - b.s < 1u + n) return RFC_ERR_INVALID_ARGUMENT;
        for (uint32_t k = 0u; k < n; k++) {
            uint32_t bits = c->m[b.s + 1u + k];
            if (bits > d) d = bits;
        }
    }
    if (it->type == FOURCC('h', 'v', 'c', '1')) {
        p = prop_of(c, it, FOURCC('h', 'v', 'c', 'C'));
        if (p) {
            if (p->p.e - p->p.s < 19u) return RFC_ERR_INVALID_ARGUMENT;
            uint32_t luma = 8u + (c->m[p->p.s + 17u] & 7u);
            uint32_t chroma = 8u + (c->m[p->p.s + 18u] & 7u);
            if (luma > d) d = luma;
            if (chroma > d) d = chroma;
        }
    } else if (it->type == FOURCC('a', 'v', '0', '1')) {
        p = prop_of(c, it, FOURCC('a', 'v', '1', 'C'));
        if (p) {
            if (p->p.e - p->p.s < 3u) return RFC_ERR_INVALID_ARGUMENT;
            uint8_t b = c->m[p->p.s + 2u];
            uint32_t ad = (b & 0x40u) ? ((b & 0x20u) ? 12u : 10u) : 8u;
            if (ad > d) d = ad;
        }
    }
    *depth = d ? d : 8u;
    return RFC_OK;
}

/* Clean aperture (ISO/IEC 14496-12 12.1.4.1): the crop must lie on the
 * pixel grid inside the coded w x h picture. */
static rfc_status apply_clap(heif_ctx *c, const prop *p, uint32_t *w, uint32_t *h) {
    if (p->p.e - p->p.s < 32u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const uint8_t *b = c->m + p->p.s;
    uint32_t wn = be32(b), wd = be32(b + 4), hn = be32(b + 8), hd = be32(b + 12);
    int32_t hon = (int32_t)be32(b + 16), von = (int32_t)be32(b + 24);
    uint32_t hod = be32(b + 20), vod = be32(b + 28);
    if (wd == 0u || hd == 0u || hod == 0u || vod == 0u || wn % wd != 0u || hn % hd != 0u) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: a clean aperture off the pixel grid");
    }
    uint64_t cw = wn / wd, ch = hn / hd;
    if (cw == 0u || ch == 0u || cw > *w || ch > *h) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    /* left = (W - cw) / 2 + hon / hod, which must be a whole pixel in range;
     * every factor is below 2^32, so the products below fit in 64 bits */
    uint64_t dw = (uint64_t)*w - cw, dh = (uint64_t)*h - ch;
    if (dw > 0x7FFFFFFFu || dh > 0x7FFFFFFFu) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    int64_t ln = (int64_t)(dw * (uint64_t)hod) + 2 * (int64_t)hon;
    int64_t tn = (int64_t)(dh * (uint64_t)vod) + 2 * (int64_t)von;
    int64_t ld = 2 * (int64_t)hod, td = 2 * (int64_t)vod;
    if (ln < 0 || tn < 0 || ln % ld != 0 || tn % td != 0 ||
        ln / ld > (int64_t)dw || tn / td > (int64_t)dh) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: a clean aperture off the pixel grid");
    }
    *w = (uint32_t)cw;
    *h = (uint32_t)ch;
    return RFC_OK;
}

static rfc_status scan_meta(heif_ctx *c, rfc_heif_info *out) {
    int hdlr_pict = 0, pitm_seen = 0, iinf_seen = 0;
    size_t q = 0u;
    while (q < c->mn) {
        uint32_t type = 0u;
        span p;
        size_t next = 0u;
        rfc_status st = mbox(c->m, q, c->mn, &type, &p, &next);
        if (st != RFC_OK) {
            return st;
        }
        q = next;
        uint32_t v = 0u;
        span b;
        if (type == FOURCC('h', 'd', 'l', 'r')) {
            st = fullbox(c->m, p, &v, NULL, &b);
            if (st != RFC_OK) return st;
            if (b.e - b.s < 8u) return RFC_ERR_INVALID_ARGUMENT;
            hdlr_pict = be32(c->m + b.s + 4u) == FOURCC('p', 'i', 'c', 't');
        } else if (type == FOURCC('p', 'i', 't', 'm')) {
            st = fullbox(c->m, p, &v, NULL, &b);
            if (st != RFC_OK) return st;
            size_t idsz = v == 0u ? 2u : 4u;
            if (b.e - b.s < idsz) return RFC_ERR_INVALID_ARGUMENT;
            c->primary = idsz == 2u ? be16(c->m + b.s) : be32(c->m + b.s);
            pitm_seen = 1;
        } else if (type == FOURCC('i', 'i', 'n', 'f')) {
            if (iinf_seen) return RFC_ERR_INVALID_ARGUMENT;
            iinf_seen = 1;
            st = parse_iinf(c, p);
        } else if (type == FOURCC('i', 'r', 'e', 'f')) {
            st = parse_iref(c, p);
        } else if (type == FOURCC('i', 'p', 'r', 'p')) {
            st = parse_iprp(c, p);
        }
        if (st != RFC_OK) {
            return st;
        }
    }
    if (!hdlr_pict || !pitm_seen || !iinf_seen) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_status st = index_tables(c);
    if (st != RFC_OK) {
        return st;
    }
    item *prim = find_item(c, c->primary);
    if (!prim) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    out->primary_type = prim->type;
    /* geometry first, so a caller can size even a file refused below */
    const prop *p = prop_of(c, prim, FOURCC('i', 's', 'p', 'e'));
    if (!p) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t v = 0u;
    span b;
    st = fullbox(c->m, p->p, &v, NULL, &b);
    if (st != RFC_OK) return st;
    if (b.e - b.s < 8u) return RFC_ERR_INVALID_ARGUMENT;
    uint32_t cw = be32(c->m + b.s), ch = be32(c->m + b.s + 4u);
    if (cw == 0u || ch == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t w = cw, h = ch;
    const prop *cl = prop_of(c, prim, FOURCC('c', 'l', 'a', 'p'));
    p = prop_of(c, prim, FOURCC('i', 'r', 'o', 't'));
    if (p) {
        if (p->p.e - p->p.s < 1u) return RFC_ERR_INVALID_ARGUMENT;
        out->irot = c->m[p->p.s] & 3u;
    }
    p = prop_of(c, prim, FOURCC('i', 'm', 'i', 'r'));
    if (p) {
        if (p->p.e - p->p.s < 1u) return RFC_ERR_INVALID_ARGUMENT;
        out->has_imir = 1;
    }
    int turn = out->irot & 1u;
    out->coded_width = turn ? ch : cw;
    out->coded_height = turn ? cw : ch;
    if (cl) {
        st = apply_clap(c, cl, &w, &h);
        if (st != RFC_OK) {
            return st;
        }
    }
    out->width = turn ? h : w;
    out->height = turn ? w : h;
    out->dims_known = 1;
    if (out->has_imir && out->irot != 0u) {
        /* the host's WIC HEIF encoder and decoder disagree on a mirror
         * combined with a rotation (EXIF orientations 5 and 7 come back
         * swapped), so such a picture's displayed orientation is unverified */
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: a mirrored and rotated image (the system decoder's combined mirror and rotation is not verified)");
    }
    /* exactly one top-level picture, and it is the primary */
    uint32_t top = 0u;
    for (uint32_t i = 0u; i < c->n_items; i++) {
        const item *it = &c->items[i];
        if (is_image_type(it->type) && !it->ref_from_thmb_auxl && !it->ref_to_dimg) {
            top++;
            if (it->id != c->primary) {
                return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                              "heif: more than one top-level image");
            }
        }
    }
    if (top == 0u) {
        return RFC_ERR_INVALID_ARGUMENT; /* the primary is not a picture */
    }
    if (prim->type == FOURCC('i', 'd', 'e', 'n') || prim->type == FOURCC('i', 'o', 'v', 'l')) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: a derived (iden) or overlay (iovl) primary image");
    }
    uint32_t depth = 0u;
    if (prim->type == FOURCC('a', 'v', '0', '1')) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: an AV1-coded HEIF image (not verified; AVIF files use the AVIF adapter)");
    }
    if (prim->type == FOURCC('g', 'r', 'i', 'd')) {
        /* every tile is HEVC of one size, and each depth counts; the coded
         * picture is the tile mosaic covering the grid */
        uint32_t tiles = 0u, tile_type = 0u, tw = 0u, th = 0u;
        for (uint32_t r = 0u; r < c->n_refs; r++) {
            const ref *rf = &c->refs[r];
            if (rf->type != FOURCC('d', 'i', 'm', 'g') || rf->from != c->primary) {
                continue;
            }
            const item *t = find_item(c, rf->to);
            if (!t) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (t->type == FOURCC('a', 'v', '0', '1')) {
                return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                              "heif: an AV1-coded HEIF image (not verified; AVIF files use the AVIF adapter)");
            }
            if (t->type != FOURCC('h', 'v', 'c', '1')) {
                return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                              "heif: a grid whose tiles are not all HEVC");
            }
            const prop *tp = prop_of(c, t, FOURCC('i', 's', 'p', 'e'));
            uint32_t tv = 0u;
            span tb;
            if (!tp) return RFC_ERR_INVALID_ARGUMENT;
            st = fullbox(c->m, tp->p, &tv, NULL, &tb);
            if (st != RFC_OK) return st;
            if (tb.e - tb.s < 8u) return RFC_ERR_INVALID_ARGUMENT;
            uint32_t w1 = be32(c->m + tb.s), h1 = be32(c->m + tb.s + 4u);
            if (w1 == 0u || h1 == 0u || (tiles > 0u && (w1 != tw || h1 != th))) {
                return RFC_ERR_INVALID_ARGUMENT; /* grid tiles share one size */
            }
            tw = w1;
            th = h1;
            tile_type = t->type;
            tiles++;
            uint32_t td = 8u;
            st = item_depth(c, t, &td);
            if (st != RFC_OK) return st;
            if (td > depth) depth = td;
        }
        if (tiles == 0u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        uint64_t cols = ((uint64_t)cw + tw - 1u) / tw, rows = ((uint64_t)ch + th - 1u) / th;
        if (cols * rows != tiles || cols * tw > 0xFFFFFFFFu || rows * th > 0xFFFFFFFFu) {
            return RFC_ERR_INVALID_ARGUMENT; /* the tiles do not cover the grid */
        }
        out->coded_width = turn ? (uint32_t)(rows * th) : (uint32_t)(cols * tw);
        out->coded_height = turn ? (uint32_t)(cols * tw) : (uint32_t)(rows * th);
        out->tile_width = turn ? th : tw;
        out->tile_height = turn ? tw : th;
        (void)tile_type;
        out->coding = RFC_HEIF_CODING_HEVC;
        uint32_t gd = 8u;
        st = item_depth(c, prim, &gd);
        if (st != RFC_OK) return st;
        if (gd > depth) depth = gd;
    } else if (prim->type == FOURCC('h', 'v', 'c', '1')) {
        out->coding = RFC_HEIF_CODING_HEVC;
        out->tile_width = out->coded_width;
        out->tile_height = out->coded_height;
        st = item_depth(c, prim, &depth);
        if (st != RFC_OK) return st;
    } else {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: the primary image coding is not HEVC or a grid of HEVC tiles");
    }
    out->bit_depth = depth;
    /* auxiliary images of the primary */
    int alpha = 0, other = 0;
    for (uint32_t r = 0u; r < c->n_refs; r++) {
        const ref *rf = &c->refs[r];
        if (rf->type != FOURCC('a', 'u', 'x', 'l') || rf->to != c->primary) {
            continue;
        }
        const item *a = find_item(c, rf->from);
        const prop *ac = a ? prop_of(c, a, FOURCC('a', 'u', 'x', 'C')) : NULL;
        int is_alpha = 0;
        if (ac) {
            uint32_t av = 0u;
            span ab;
            st = fullbox(c->m, ac->p, &av, NULL, &ab);
            if (st != RFC_OK) return st;
            size_t pos = ab.s;
            const uint8_t *urn = NULL;
            size_t ul = 0u;
            st = cstr(c->m, &pos, ab.e, &urn, &ul);
            if (st != RFC_OK) return st;
            is_alpha = (ul == 26u && memcmp(urn, "urn:mpeg:hevc:2015:auxid:1", 26u) == 0) ||
                       (ul == 43u && memcmp(urn, "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha", 43u) == 0);
        }
        if (is_alpha) alpha = 1; else other = 1;
    }
    if (other) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: an auxiliary image (depth map or gain map) cannot be carried");
    }
    if (alpha) {
        return refuse(c, RFC_ERR_UNSUPPORTED_ALPHA,
                      "heif: an alpha auxiliary image (alpha HEIF import is not verified)");
    }
    if (out->bit_depth > 8u) {
        return refuse(c, RFC_ERR_UNSUPPORTED_SAMPLE,
                      "heif: a 10- or 12-bit image (the system decoder's high-depth output is not verified)");
    }
    /* colour boxes of the primary, in association order */
    for (uint32_t a = 0u; a < prim->assoc_count; a++) {
        const prop *cp = &c->props[c->assocs[prim->assoc_first + a].index - 1u];
        if (cp->type != FOURCC('c', 'o', 'l', 'r')) {
            continue;
        }
        if (cp->p.e - cp->p.s < 4u) return RFC_ERR_INVALID_ARGUMENT;
        uint32_t ct = be32(c->m + cp->p.s);
        if (ct == FOURCC('p', 'r', 'o', 'f') || ct == FOURCC('r', 'I', 'C', 'C')) {
            size_t n = cp->p.e - cp->p.s - 4u;
            if (n == 0u) return RFC_ERR_INVALID_ARGUMENT;
            if (!out->has_icc) {
                out->has_icc = 1;
                out->icc_offset = c->m_file_off + cp->p.s + 4u;
                out->icc_len = (uint32_t)n;
            }
        } else if (ct == FOURCC('n', 'c', 'l', 'x')) {
            if (cp->p.e - cp->p.s < 11u) return RFC_ERR_INVALID_ARGUMENT;
            const uint8_t *nb = c->m + cp->p.s + 4u;
            out->has_nclx = 1;
            out->primaries = (uint16_t)be16(nb);
            out->transfer = (uint16_t)be16(nb + 2);
            out->matrix = (uint16_t)be16(nb + 4);
            out->full_range = (uint8_t)(nb[6] >> 7);
        }
    }
    if (out->has_icc) {
        out->has_nclx = 0; /* ICC takes precedence */
    } else if (out->has_nclx && (out->transfer == 16u || out->transfer == 18u)) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: a PQ or HLG transfer (the system decoder's HDR output is not specified)");
    }
    for (uint32_t i = 0u; i < c->n_items; i++) {
        if (c->items[i].type == FOURCC('E', 'x', 'i', 'f')) out->has_exif = 1;
        if (c->items[i].is_xmp) out->has_xmp = 1;
    }
    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = RFC_SAMPLE_RGB8;
    lay.alpha = RFC_ALPHA_NONE;
    lay.bits_per_sample = 8u;
    lay.width = out->width;
    lay.height = out->height;
    st = rfc_layout_normalize_stride(&lay, &out->layout);
    if (st != RFC_OK) return st;
    lay.width = out->coded_width;
    lay.height = out->coded_height;
    st = rfc_layout_normalize_stride(&lay, &out->coded_layout);
    if (st != RFC_OK) return st;
    lay.width = out->tile_width;
    lay.height = out->tile_height;
    return rfc_layout_normalize_stride(&lay, &out->tile_layout);
}

static void *heap_alloc(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n ? n : 1u);
}

static void heap_free(void *ctx, void *p) {
    (void)ctx;
    free(p);
}

rfc_status rfc_heif_scan(rfc_heif_read_fn read_at, void *ctx, uint64_t size,
                         const rfc_allocator *alloc, rfc_heif_info *out,
                         const char **detail, int *read_failed) {
    static const rfc_allocator k_heap = {heap_alloc, heap_free, NULL};
    if (detail) *detail = NULL;
    if (read_failed) *read_failed = 0;
    if (!read_at || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (!alloc || !alloc->alloc || !alloc->free) {
        alloc = &k_heap;
    }
    src r = {read_at, ctx, size, 0};
    heif_ctx c;
    memset(&c, 0, sizeof(c));
    rfc_status st = RFC_OK;
    /* top level: ftyp first; find meta; a moov is a sequence track */
    uint64_t pos = 0u, meta_off = 0u, meta_size = 0u;
    int meta_seen = 0, ftyp_first = 0, moov = 0, meta_to_end = 0;
    for (uint32_t n = 0u;; n++) {
        if (n >= MAX_TOP_BOXES) {
            st = RFC_ERR_LIMIT_EXCEEDED;
            break;
        }
        uint8_t h[16];
        size_t got = 0u;
        st = src_read(&r, pos, h, 16u, &got);
        if (st != RFC_OK) break;
        if (got == 0u) break; /* the end, at a box boundary */
        if (got < 8u) {
            st = RFC_ERR_TRUNCATED_INPUT;
            break;
        }
        uint64_t bsz = be32(h);
        uint32_t type = be32(h + 4);
        uint64_t hdr = 8u;
        if (bsz == 1u) {
            if (got < 16u) {
                st = RFC_ERR_TRUNCATED_INPUT;
                break;
            }
            bsz = ((uint64_t)be32(h + 8) << 32) | be32(h + 12);
            hdr = 16u;
        }
        if (n == 0u) ftyp_first = type == FOURCC('f', 't', 'y', 'p');
        if (type == FOURCC('m', 'o', 'o', 'v')) moov = 1;
        if (bsz == 0u) {
            /* runs to the end of the file */
            if (type == FOURCC('m', 'e', 't', 'a') && !meta_seen) {
                meta_seen = 1;
                meta_off = pos + hdr;
                meta_to_end = 1;
                if (size != RFC_HEIF_SIZE_UNKNOWN) {
                    meta_size = size - meta_off;
                    meta_to_end = 0;
                }
            }
            break;
        }
        if (bsz < hdr || (size != RFC_HEIF_SIZE_UNKNOWN && bsz > size - pos)) {
            st = RFC_ERR_INVALID_ARGUMENT;
            break;
        }
        if (type == FOURCC('m', 'e', 't', 'a') && !meta_seen) {
            meta_seen = 1;
            meta_off = pos + hdr;
            meta_size = bsz - hdr;
        }
        if (UINT64_MAX - pos < bsz) {
            st = RFC_ERR_INVALID_ARGUMENT;
            break;
        }
        pos += bsz;
    }
    if (st == RFC_OK && moov) {
        st = refuse(&c, RFC_ERR_UNSUPPORTED_COMBINATION,
                    "heif: an image sequence track (sequences are not supported)");
    }
    uint8_t *meta = NULL;
    if (st == RFC_OK && meta_to_end) {
        /* the size is unknown: the meta box ends where the input does; it is
         * read once, to the end, into a buffer grown within the meta bound */
        size_t cap = 65536u, len = 0u;
        meta = (uint8_t *)alloc->alloc(alloc->ctx, cap);
        if (!meta) st = RFC_ERR_OUT_OF_MEMORY;
        while (st == RFC_OK) {
            if (len == cap) {
                if ((uint64_t)cap > RFC_HEIF_MAX_META_BYTES) {
                    st = RFC_ERR_LIMIT_EXCEEDED;
                    break;
                }
                uint64_t want = (uint64_t)cap * 2u;
                size_t ncap = want > RFC_HEIF_MAX_META_BYTES + 1u
                                  ? (size_t)(RFC_HEIF_MAX_META_BYTES + 1u)
                                  : (size_t)want;
                uint8_t *nb = (uint8_t *)alloc->alloc(alloc->ctx, ncap);
                if (!nb) {
                    st = RFC_ERR_OUT_OF_MEMORY;
                    break;
                }
                memcpy(nb, meta, len);
                alloc->free(alloc->ctx, meta);
                meta = nb;
                cap = ncap;
            }
            size_t got = 0u;
            st = src_read(&r, meta_off + len, meta + len, cap - len, &got);
            if (st != RFC_OK) break;
            len += got;
            if (len > RFC_HEIF_MAX_META_BYTES) {
                st = RFC_ERR_LIMIT_EXCEEDED;
                break;
            }
            if (len < cap) break; /* the end of the input */
        }
        meta_size = len;
    }
    if (st == RFC_OK && (!ftyp_first || !meta_seen || meta_size < 4u)) {
        st = RFC_ERR_INVALID_ARGUMENT;
    }
    if (st == RFC_OK && meta_size > RFC_HEIF_MAX_META_BYTES) {
        st = RFC_ERR_LIMIT_EXCEEDED;
    }
    if (st == RFC_OK) {
        if (!meta) {
            meta = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)meta_size);
        }
        c.items = (item *)alloc->alloc(alloc->ctx, sizeof(item) * RFC_HEIF_MAX_ITEMS);
        c.refs = (ref *)alloc->alloc(alloc->ctx, sizeof(ref) * RFC_HEIF_MAX_REFERENCES);
        c.props = (prop *)alloc->alloc(alloc->ctx, sizeof(prop) * RFC_HEIF_MAX_PROPERTIES);
        c.assocs = (assoc *)alloc->alloc(alloc->ctx, sizeof(assoc) * RFC_HEIF_MAX_ASSOCIATIONS);
        if (!meta || !c.items || !c.refs || !c.props || !c.assocs) {
            st = RFC_ERR_OUT_OF_MEMORY;
        }
    }
    if (st == RFC_OK && !meta_to_end) {
        size_t got = 0u;
        st = src_read(&r, meta_off, meta, (size_t)meta_size, &got);
        if (st == RFC_OK && got != meta_size) {
            st = RFC_ERR_TRUNCATED_INPUT;
        }
    }
    if (st == RFC_OK) {
        c.m = meta + 4u; /* after the FullBox header of meta */
        c.mn = (size_t)meta_size - 4u;
        c.m_file_off = meta_off + 4u;
        memset(c.items, 0, sizeof(item) * RFC_HEIF_MAX_ITEMS);
        st = scan_meta(&c, out);
    }
    if (meta) alloc->free(alloc->ctx, meta);
    if (c.items) alloc->free(alloc->ctx, c.items);
    if (c.refs) alloc->free(alloc->ctx, c.refs);
    if (c.props) alloc->free(alloc->ctx, c.props);
    if (c.assocs) alloc->free(alloc->ctx, c.assocs);
    if (detail) *detail = c.detail;
    if (read_failed) *read_failed = r.read_failed;
    return st;
}
