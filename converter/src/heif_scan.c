/* Bounded ISOBMFF reader for HEIF and HEIC (see rfc_heif_scan.h).
 * ISO/IEC 14496-12 boxes (ftyp, meta, hdlr, pitm, iinf/infe, iref, iprp,
 * ipco, ipma) and ISO/IEC 23008-12 item properties (ispe, pixi, irot, imir,
 * colr, auxC, hvcC, av1C). Every query walks the boxes again through
 * positional reads; no table is built and nothing is allocated. */

#include "rfc_heif_scan.h"
#include "rfc_job.h"

#include <string.h>

#define FOURCC(a, b, c, d)                                                     \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) |        \
     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

#define MAX_BOXES 4096u

typedef struct hrd {
    rfc_heif_read_fn read_at;
    void *ctx;
    uint64_t size;
    int read_failed;
} hrd;

typedef struct range {
    uint64_t start, end; /* box payload, after the header */
    int present;
} range;

typedef struct heif_ctx {
    hrd r;
    range meta;   /* after the FullBox header */
    range iinf, iref, ipco, ipma;
    uint32_t iinf_version, iref_version, ipma_version, ipma_flags;
    uint32_t primary;
    const char *detail;
} heif_ctx;

static rfc_status rd(hrd *r, uint64_t off, uint8_t *dst, size_t n) {
    if (off > r->size || n > r->size - off) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    size_t got = 0u;
    rfc_status st = r->read_at(r->ctx, off, dst, n, &got);
    if (st != RFC_OK) {
        r->read_failed = 1;
        return st;
    }
    return got == n ? RFC_OK : RFC_ERR_TRUNCATED_INPUT;
}

static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static rfc_status rd16(hrd *r, uint64_t off, uint32_t *v) {
    uint8_t b[2];
    rfc_status st = rd(r, off, b, 2u);
    if (st == RFC_OK) {
        *v = be16(b);
    }
    return st;
}

static rfc_status rd32(hrd *r, uint64_t off, uint32_t *v) {
    uint8_t b[4];
    rfc_status st = rd(r, off, b, 4u);
    if (st == RFC_OK) {
        *v = be32(b);
    }
    return st;
}

/* The box at pos inside [pos, end): its type and payload range. */
static rfc_status box_at(hrd *r, uint64_t pos, uint64_t end, uint32_t *type,
                         range *payload) {
    uint8_t h[16];
    if (end - pos < 8u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_status st = rd(r, pos, h, 8u);
    if (st != RFC_OK) {
        return st;
    }
    uint64_t size = be32(h);
    *type = be32(h + 4);
    uint64_t hdr = 8u;
    if (size == 1u) {
        if (end - pos < 16u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        st = rd(r, pos + 8u, h + 8, 8u);
        if (st != RFC_OK) {
            return st;
        }
        size = ((uint64_t)be32(h + 8) << 32) | be32(h + 12);
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
    payload->start = pos + hdr;
    payload->end = pos + size;
    payload->present = 1;
    return RFC_OK;
}

/* Visit the boxes in [start, end); cb returns 1 to stop. */
typedef int (*box_fn)(heif_ctx *c, uint32_t type, const range *p, void *u,
                      rfc_status *st);

static rfc_status each_box(heif_ctx *c, uint64_t start, uint64_t end,
                           box_fn cb, void *u) {
    uint64_t pos = start;
    for (uint32_t n = 0u; pos < end; n++) {
        if (n >= MAX_BOXES) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        uint32_t type = 0u;
        range p;
        rfc_status st = box_at(&c->r, pos, end, &type, &p);
        if (st != RFC_OK) {
            return st;
        }
        st = RFC_OK;
        if (cb && cb(c, type, &p, u, &st)) {
            return st;
        }
        if (st != RFC_OK) {
            return st;
        }
        pos = p.end;
    }
    return RFC_OK;
}

/* ---- items (iinf / infe) ---------------------------------------------- */

typedef struct item {
    uint32_t id, type;
    int is_xmp;
} item;

typedef int (*item_fn)(heif_ctx *c, const item *it, void *u, rfc_status *st);

static rfc_status read_cstr_end(hrd *r, uint64_t pos, uint64_t end,
                                uint64_t *after) {
    for (uint64_t p = pos; p < end; p++) {
        uint8_t ch = 0u;
        rfc_status st = rd(r, p, &ch, 1u);
        if (st != RFC_OK) {
            return st;
        }
        if (ch == 0u) {
            *after = p + 1u;
            return RFC_OK;
        }
    }
    *after = end; /* an unterminated string runs to the end of the box */
    return RFC_OK;
}

static rfc_status infe_parse(heif_ctx *c, const range *p, item *it) {
    memset(it, 0, sizeof(*it));
    uint8_t vf[4];
    rfc_status st = rd(&c->r, p->start, vf, 4u);
    if (st != RFC_OK) {
        return st;
    }
    uint32_t version = vf[0];
    uint64_t q = p->start + 4u;
    if (version < 2u) {
        uint32_t id = 0u;
        st = rd16(&c->r, q, &id);
        if (st != RFC_OK) {
            return st;
        }
        it->id = id;
        q += 4u; /* item_ID, item_protection_index */
        st = read_cstr_end(&c->r, q, p->end, &q); /* item_name */
        if (st != RFC_OK) {
            return st;
        }
        it->type = FOURCC('m', 'i', 'm', 'e');
    } else {
        uint32_t id = 0u;
        if (version == 2u) {
            st = rd16(&c->r, q, &id);
            q += 2u;
        } else {
            st = rd32(&c->r, q, &id);
            q += 4u;
        }
        if (st != RFC_OK) {
            return st;
        }
        it->id = id;
        q += 2u; /* item_protection_index */
        st = rd32(&c->r, q, &it->type);
        if (st != RFC_OK) {
            return st;
        }
        q += 4u;
        if (it->type != FOURCC('m', 'i', 'm', 'e')) {
            return RFC_OK;
        }
        st = read_cstr_end(&c->r, q, p->end, &q); /* item_name */
        if (st != RFC_OK) {
            return st;
        }
    }
    /* content_type of a MIME item */
    static const char xmp[] = "application/rdf+xml";
    uint8_t ct[sizeof(xmp)];
    if (p->end - q >= sizeof(xmp)) {
        st = rd(&c->r, q, ct, sizeof(xmp));
        if (st != RFC_OK) {
            return st;
        }
        it->is_xmp = memcmp(ct, xmp, sizeof(xmp)) == 0;
    }
    return RFC_OK;
}

typedef struct items_walk {
    item_fn fn;
    void *u;
    uint32_t count;
    int stopped;
} items_walk;

static int iinf_child(heif_ctx *c, uint32_t type, const range *p, void *u,
                      rfc_status *st) {
    items_walk *w = (items_walk *)u;
    if (type != FOURCC('i', 'n', 'f', 'e')) {
        return 0;
    }
    if (++w->count > RFC_HEIF_MAX_ITEMS) {
        *st = RFC_ERR_LIMIT_EXCEEDED;
        return 1;
    }
    item it;
    *st = infe_parse(c, p, &it);
    if (*st != RFC_OK) {
        return 1;
    }
    if (w->fn(c, &it, w->u, st)) {
        w->stopped = 1;
        return 1;
    }
    return 0;
}

static rfc_status each_item(heif_ctx *c, item_fn fn, void *u) {
    if (!c->iinf.present) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t q = c->iinf.start + 4u + (c->iinf_version == 0u ? 2u : 4u);
    items_walk w = {fn, u, 0u, 0};
    return each_box(c, q, c->iinf.end, iinf_child, &w);
}

typedef struct find_item {
    uint32_t id;
    item found;
    int ok;
} find_item;

static int find_item_cb(heif_ctx *c, const item *it, void *u, rfc_status *st) {
    (void)c;
    (void)st;
    find_item *f = (find_item *)u;
    if (it->id == f->id) {
        f->found = *it;
        f->ok = 1;
        return 1;
    }
    return 0;
}

static rfc_status item_by_id(heif_ctx *c, uint32_t id, item *out, int *ok) {
    find_item f = {id, {0u, 0u, 0}, 0};
    rfc_status st = each_item(c, find_item_cb, &f);
    *out = f.found;
    *ok = f.ok;
    return st;
}

/* ---- references (iref) ------------------------------------------------- */

/* Visit every (type, from, to) reference. */
typedef int (*ref_fn)(heif_ctx *c, uint32_t type, uint32_t from, uint32_t to,
                      void *u, rfc_status *st);

typedef struct refs_walk {
    ref_fn fn;
    void *u;
} refs_walk;

static int iref_child(heif_ctx *c, uint32_t type, const range *p, void *u,
                      rfc_status *st) {
    refs_walk *w = (refs_walk *)u;
    unsigned idsz = c->iref_version == 0u ? 2u : 4u;
    uint64_t q = p->start;
    uint8_t b[4];
    if (p->end - q < idsz + 2u) {
        *st = RFC_ERR_INVALID_ARGUMENT;
        return 1;
    }
    *st = rd(&c->r, q, b, idsz);
    if (*st != RFC_OK) {
        return 1;
    }
    uint32_t from = idsz == 2u ? be16(b) : be32(b);
    q += idsz;
    uint32_t count = 0u;
    *st = rd16(&c->r, q, &count);
    if (*st != RFC_OK) {
        return 1;
    }
    q += 2u;
    if (count > RFC_HEIF_MAX_ITEMS || (uint64_t)count * idsz > p->end - q) {
        *st = RFC_ERR_INVALID_ARGUMENT;
        return 1;
    }
    for (uint32_t i = 0u; i < count; i++) {
        *st = rd(&c->r, q + (uint64_t)i * idsz, b, idsz);
        if (*st != RFC_OK) {
            return 1;
        }
        uint32_t to = idsz == 2u ? be16(b) : be32(b);
        if (w->fn(c, type, from, to, w->u, st)) {
            return 1;
        }
    }
    return 0;
}

static rfc_status each_ref(heif_ctx *c, ref_fn fn, void *u) {
    if (!c->iref.present) {
        return RFC_OK;
    }
    refs_walk w = {fn, u};
    return each_box(c, c->iref.start + 4u, c->iref.end, iref_child, &w);
}

typedef struct ref_query {
    uint32_t type, from, to; /* 0 is a wildcard for from / to */
    uint32_t hits;
    uint32_t last_from, last_to;
} ref_query;

static int ref_query_cb(heif_ctx *c, uint32_t type, uint32_t from, uint32_t to,
                        void *u, rfc_status *st) {
    (void)c;
    (void)st;
    ref_query *q = (ref_query *)u;
    if (type == q->type && (q->from == 0u || q->from == from) &&
        (q->to == 0u || q->to == to)) {
        q->hits++;
        q->last_from = from;
        q->last_to = to;
    }
    return 0;
}

static rfc_status count_refs(heif_ctx *c, uint32_t type, uint32_t from,
                             uint32_t to, uint32_t *hits) {
    ref_query q = {type, from, to, 0u, 0u, 0u};
    rfc_status st = each_ref(c, ref_query_cb, &q);
    *hits = q.hits;
    return st;
}

/* ---- properties (ipco / ipma) ----------------------------------------- */

typedef struct nth_prop {
    uint32_t want, seen, type;
    range p;
} nth_prop;

static int nth_prop_cb(heif_ctx *c, uint32_t type, const range *p, void *u,
                       rfc_status *st) {
    (void)c;
    (void)st;
    nth_prop *n = (nth_prop *)u;
    if (++n->seen == n->want) {
        n->type = type;
        n->p = *p;
        return 1;
    }
    return 0;
}

/* Visit the properties associated with `id`, in association order. */
typedef int (*prop_fn)(heif_ctx *c, uint32_t type, const range *p, void *u,
                       rfc_status *st);

static rfc_status each_prop_of(heif_ctx *c, uint32_t id, prop_fn fn, void *u) {
    if (!c->ipma.present || !c->ipco.present) {
        return RFC_OK;
    }
    uint64_t q = c->ipma.start + 4u;
    uint32_t entries = 0u;
    rfc_status st = rd32(&c->r, q, &entries);
    if (st != RFC_OK) {
        return st;
    }
    q += 4u;
    if (entries > RFC_HEIF_MAX_ITEMS) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    unsigned idsz = c->ipma_version < 1u ? 2u : 4u;
    unsigned asz = (c->ipma_flags & 1u) ? 2u : 1u;
    for (uint32_t e = 0u; e < entries; e++) {
        uint8_t b[4];
        st = rd(&c->r, q, b, idsz + 1u);
        if (st != RFC_OK) {
            return st;
        }
        uint32_t item_id = idsz == 2u ? be16(b) : be32(b);
        uint32_t n = b[idsz];
        q += idsz + 1u;
        for (uint32_t a = 0u; a < n; a++) {
            st = rd(&c->r, q, b, asz);
            if (st != RFC_OK) {
                return st;
            }
            q += asz;
            if (item_id != id) {
                continue;
            }
            uint32_t index = asz == 2u ? (be16(b) & 0x7FFFu) : (b[0] & 0x7Fu);
            if (index == 0u) {
                continue; /* no property */
            }
            if (index > RFC_HEIF_MAX_PROPERTIES) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            nth_prop np = {index, 0u, 0u, {0u, 0u, 0}};
            st = each_box(c, c->ipco.start, c->ipco.end, nth_prop_cb, &np);
            if (st != RFC_OK) {
                return st;
            }
            if (np.seen != index) {
                return RFC_ERR_INVALID_ARGUMENT; /* index past ipco */
            }
            st = RFC_OK;
            if (fn(c, np.type, &np.p, u, &st)) {
                return st;
            }
            if (st != RFC_OK) {
                return st;
            }
        }
        if (q > c->ipma.end) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
    }
    return RFC_OK;
}

typedef struct first_prop {
    uint32_t type;
    range p;
    int found;
} first_prop;

static int first_prop_cb(heif_ctx *c, uint32_t type, const range *p, void *u,
                         rfc_status *st) {
    (void)c;
    (void)st;
    first_prop *f = (first_prop *)u;
    if (type == f->type) {
        f->p = *p;
        f->found = 1;
        return 1;
    }
    return 0;
}

static rfc_status prop_of(heif_ctx *c, uint32_t id, uint32_t type, range *p,
                          int *found) {
    first_prop f = {type, {0u, 0u, 0}, 0};
    rfc_status st = each_prop_of(c, id, first_prop_cb, &f);
    *p = f.p;
    *found = f.found;
    return st;
}

/* ---- the scan ----------------------------------------------------------- */

static int is_image_type(uint32_t t) {
    return t == FOURCC('h', 'v', 'c', '1') || t == FOURCC('a', 'v', '0', '1') ||
           t == FOURCC('g', 'r', 'i', 'd') || t == FOURCC('i', 'd', 'e', 'n') ||
           t == FOURCC('i', 'o', 'v', 'l') || t == FOURCC('j', 'p', 'e', 'g') ||
           t == FOURCC('a', 'v', 'c', '1') || t == FOURCC('u', 'n', 'c', 'i');
}

typedef struct top_level {
    uint32_t count;
    uint32_t first_id;
    rfc_status st;
} top_level;

static int top_level_cb(heif_ctx *c, const item *it, void *u, rfc_status *st) {
    top_level *t = (top_level *)u;
    if (!is_image_type(it->type)) {
        return 0;
    }
    uint32_t hits = 0u, all = 0u;
    static const uint32_t from_kinds[2] = {FOURCC('t', 'h', 'm', 'b'),
                                           FOURCC('a', 'u', 'x', 'l')};
    for (int k = 0; k < 2; k++) {
        *st = count_refs(c, from_kinds[k], it->id, 0u, &hits);
        if (*st != RFC_OK) {
            return 1;
        }
        all += hits;
    }
    *st = count_refs(c, FOURCC('d', 'i', 'm', 'g'), 0u, it->id, &hits);
    if (*st != RFC_OK) {
        return 1;
    }
    all += hits;
    if (all == 0u) {
        if (t->count++ == 0u) {
            t->first_id = it->id;
        }
    }
    return 0;
}

typedef struct meta_child {
    int hdlr_pict, pitm_seen;
} meta_child;

static int meta_child_cb(heif_ctx *c, uint32_t type, const range *p, void *u,
                         rfc_status *st) {
    meta_child *m = (meta_child *)u;
    uint8_t vf[4];
    if (type == FOURCC('h', 'd', 'l', 'r')) {
        uint8_t h[12];
        *st = rd(&c->r, p->start, h, 12u);
        if (*st != RFC_OK) {
            return 1;
        }
        m->hdlr_pict = be32(h + 8) == FOURCC('p', 'i', 'c', 't');
    } else if (type == FOURCC('p', 'i', 't', 'm')) {
        *st = rd(&c->r, p->start, vf, 4u);
        if (*st != RFC_OK) {
            return 1;
        }
        *st = vf[0] == 0u ? rd16(&c->r, p->start + 4u, &c->primary)
                          : rd32(&c->r, p->start + 4u, &c->primary);
        if (*st != RFC_OK) {
            return 1;
        }
        m->pitm_seen = 1;
    } else if (type == FOURCC('i', 'i', 'n', 'f') ||
               type == FOURCC('i', 'r', 'e', 'f') ||
               type == FOURCC('i', 'p', 'm', 'a')) {
        *st = rd(&c->r, p->start, vf, 4u);
        if (*st != RFC_OK) {
            return 1;
        }
        if (type == FOURCC('i', 'i', 'n', 'f')) {
            c->iinf = *p;
            c->iinf_version = vf[0];
        } else if (type == FOURCC('i', 'r', 'e', 'f')) {
            c->iref = *p;
            c->iref_version = vf[0];
        } else {
            c->ipma = *p;
            c->ipma_version = vf[0];
            c->ipma_flags = ((uint32_t)vf[1] << 16) | ((uint32_t)vf[2] << 8) | vf[3];
        }
    } else if (type == FOURCC('i', 'p', 'r', 'p')) {
        /* ipco and ipma live inside iprp */
        *st = each_box(c, p->start, p->end, meta_child_cb, u);
        if (*st != RFC_OK) {
            return 1;
        }
    } else if (type == FOURCC('i', 'p', 'c', 'o')) {
        c->ipco = *p;
    }
    return 0;
}

typedef struct top_box {
    int ftyp_first, moov;
    uint32_t n;
} top_box;

static int top_box_cb(heif_ctx *c, uint32_t type, const range *p, void *u,
                      rfc_status *st) {
    top_box *t = (top_box *)u;
    if (t->n++ == 0u) {
        t->ftyp_first = type == FOURCC('f', 't', 'y', 'p');
    }
    if (type == FOURCC('m', 'o', 'o', 'v')) {
        t->moov = 1;
    } else if (type == FOURCC('m', 'e', 't', 'a') && !c->meta.present) {
        if (p->end - p->start < 4u) {
            *st = RFC_ERR_INVALID_ARGUMENT;
            return 1;
        }
        c->meta.start = p->start + 4u;
        c->meta.end = p->end;
        c->meta.present = 1;
    }
    return 0;
}

typedef struct aux_scan {
    uint32_t primary;
    int alpha, other;
} aux_scan;

static int aux_ref_cb(heif_ctx *c, uint32_t type, uint32_t from, uint32_t to,
                      void *u, rfc_status *st) {
    aux_scan *a = (aux_scan *)u;
    if (type != FOURCC('a', 'u', 'x', 'l') || to != a->primary) {
        return 0;
    }
    range p;
    int found = 0;
    *st = prop_of(c, from, FOURCC('a', 'u', 'x', 'C'), &p, &found);
    if (*st != RFC_OK) {
        return 1;
    }
    static const char a1[] = "urn:mpeg:hevc:2015:auxid:1";
    static const char a2[] = "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
    char urn[sizeof(a2)];
    memset(urn, 0, sizeof(urn));
    if (found && p.end - p.start > 4u) {
        uint64_t n = p.end - p.start - 4u;
        if (n > sizeof(urn) - 1u) {
            n = sizeof(urn) - 1u;
        }
        *st = rd(&c->r, p.start + 4u, (uint8_t *)urn, (size_t)n);
        if (*st != RFC_OK) {
            return 1;
        }
    }
    if (strcmp(urn, a1) == 0 || strcmp(urn, a2) == 0) {
        a->alpha = 1;
    } else {
        a->other = 1;
    }
    return 0;
}

typedef struct tile_scan {
    uint32_t grid;
    uint32_t tiles, hevc_tiles;
    uint32_t first_tile;
} tile_scan;

static int tile_ref_cb(heif_ctx *c, uint32_t type, uint32_t from, uint32_t to,
                       void *u, rfc_status *st) {
    tile_scan *t = (tile_scan *)u;
    if (type != FOURCC('d', 'i', 'm', 'g') || from != t->grid) {
        return 0;
    }
    item it;
    int ok = 0;
    *st = item_by_id(c, to, &it, &ok);
    if (*st != RFC_OK) {
        return 1;
    }
    if (!ok) {
        *st = RFC_ERR_INVALID_ARGUMENT;
        return 1;
    }
    if (t->tiles++ == 0u) {
        t->first_tile = to;
    }
    if (it.type == FOURCC('h', 'v', 'c', '1')) {
        t->hevc_tiles++;
    }
    return 0;
}

typedef struct exif_scan {
    int exif, xmp;
} exif_scan;

static int exif_cb(heif_ctx *c, const item *it, void *u, rfc_status *st) {
    (void)c;
    (void)st;
    exif_scan *e = (exif_scan *)u;
    if (it->type == FOURCC('E', 'x', 'i', 'f')) {
        e->exif = 1;
    } else if (it->is_xmp) {
        e->xmp = 1;
    }
    return 0;
}

typedef struct colr_scan {
    rfc_heif_info *out;
} colr_scan;

static int colr_cb(heif_ctx *c, uint32_t type, const range *p, void *u,
                   rfc_status *st) {
    rfc_heif_info *o = ((colr_scan *)u)->out;
    if (type != FOURCC('c', 'o', 'l', 'r') || p->end - p->start < 4u) {
        return 0;
    }
    uint32_t ct = 0u;
    *st = rd32(&c->r, p->start, &ct);
    if (*st != RFC_OK) {
        return 1;
    }
    if (ct == FOURCC('p', 'r', 'o', 'f') || ct == FOURCC('r', 'I', 'C', 'C')) {
        uint64_t n = p->end - p->start - 4u;
        if (n == 0u || n > 0xFFFFFFFFu) {
            *st = RFC_ERR_INVALID_ARGUMENT;
            return 1;
        }
        o->has_icc = 1;
        o->icc_offset = p->start + 4u;
        o->icc_len = (uint32_t)n;
    } else if (ct == FOURCC('n', 'c', 'l', 'x')) {
        uint8_t b[7];
        if (p->end - p->start < 11u) {
            *st = RFC_ERR_INVALID_ARGUMENT;
            return 1;
        }
        *st = rd(&c->r, p->start + 4u, b, 7u);
        if (*st != RFC_OK) {
            return 1;
        }
        o->has_nclx = 1;
        o->primaries = (uint16_t)be16(b);
        o->transfer = (uint16_t)be16(b + 2);
        o->matrix = (uint16_t)be16(b + 4);
        o->full_range = (uint8_t)(b[6] >> 7);
    }
    return 0;
}

static rfc_status scan_depth(heif_ctx *c, uint32_t id, uint32_t type,
                             uint32_t *depth) {
    range p;
    int found = 0;
    rfc_status st = prop_of(c, id, FOURCC('p', 'i', 'x', 'i'), &p, &found);
    if (st != RFC_OK) {
        return st;
    }
    if (found && p.end - p.start >= 6u) {
        uint8_t b[2];
        st = rd(&c->r, p.start + 4u, b, 2u); /* num_channels, bits[0] */
        if (st != RFC_OK) {
            return st;
        }
        if (b[0] > 0u) {
            *depth = b[1];
            return RFC_OK;
        }
    }
    if (type == FOURCC('h', 'v', 'c', '1')) {
        st = prop_of(c, id, FOURCC('h', 'v', 'c', 'C'), &p, &found);
        if (st == RFC_OK && found && p.end - p.start >= 18u) {
            uint8_t b = 0u;
            st = rd(&c->r, p.start + 17u, &b, 1u);
            if (st == RFC_OK) {
                *depth = 8u + (b & 7u);
            }
        }
        return st;
    }
    if (type == FOURCC('a', 'v', '0', '1')) {
        st = prop_of(c, id, FOURCC('a', 'v', '1', 'C'), &p, &found);
        if (st == RFC_OK && found && p.end - p.start >= 3u) {
            uint8_t b = 0u;
            st = rd(&c->r, p.start + 2u, &b, 1u);
            if (st == RFC_OK) {
                *depth = (b & 0x40u) ? ((b & 0x20u) ? 12u : 10u) : 8u;
            }
        }
    }
    return st;
}

static rfc_status refuse(heif_ctx *c, rfc_status st, const char *detail) {
    c->detail = detail;
    return st;
}

static rfc_status heif_scan_core(heif_ctx *c, rfc_heif_info *out) {
    top_box tb = {0, 0, 0u};
    rfc_status st = each_box(c, 0u, c->r.size, top_box_cb, &tb);
    if (st != RFC_OK) {
        return st;
    }
    if (!tb.ftyp_first || !c->meta.present) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    meta_child mc = {0, 0};
    st = each_box(c, c->meta.start, c->meta.end, meta_child_cb, &mc);
    if (st != RFC_OK) {
        return st;
    }
    if (!mc.hdlr_pict || !mc.pitm_seen || !c->iinf.present) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    item prim;
    int ok = 0;
    st = item_by_id(c, c->primary, &prim, &ok);
    if (st != RFC_OK) {
        return st;
    }
    if (!ok) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    out->primary_type = prim.type;
    /* geometry first, so a caller can size even a file refused below */
    range p;
    int found = 0;
    st = prop_of(c, c->primary, FOURCC('i', 's', 'p', 'e'), &p, &found);
    if (st != RFC_OK) {
        return st;
    }
    if (!found || p.end - p.start < 12u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint8_t wh[8];
    st = rd(&c->r, p.start + 4u, wh, 8u);
    if (st != RFC_OK) {
        return st;
    }
    uint32_t w = be32(wh), h = be32(wh + 4);
    if (w == 0u || h == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    /* clean aperture (applied before irot and imir, ISO/IEC 23008-12 6.5):
     * the picture the system decoder returns is cropped to it */
    st = prop_of(c, c->primary, FOURCC('c', 'l', 'a', 'p'), &p, &found);
    if (st != RFC_OK) {
        return st;
    }
    if (found) {
        uint8_t cl[32];
        if (p.end - p.start < 32u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        st = rd(&c->r, p.start, cl, 32u);
        if (st != RFC_OK) {
            return st;
        }
        uint32_t wn = be32(cl), wd = be32(cl + 4), hn = be32(cl + 8), hd = be32(cl + 12);
        if (wd == 0u || hd == 0u || wn % wd != 0u || hn % hd != 0u) {
            return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                          "heif: a fractional clean aperture");
        }
        uint32_t cw = wn / wd, ch = hn / hd;
        if (cw == 0u || ch == 0u || cw > w || ch > h) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        w = cw;
        h = ch;
    }
    st = prop_of(c, c->primary, FOURCC('i', 'r', 'o', 't'), &p, &found);
    if (st != RFC_OK) {
        return st;
    }
    if (found && p.end > p.start) {
        uint8_t a = 0u;
        st = rd(&c->r, p.start, &a, 1u);
        if (st != RFC_OK) {
            return st;
        }
        out->irot = a & 3u;
    }
    st = prop_of(c, c->primary, FOURCC('i', 'm', 'i', 'r'), &p, &found);
    if (st != RFC_OK) {
        return st;
    }
    if (found && p.end > p.start) {
        uint8_t a = 0u;
        st = rd(&c->r, p.start, &a, 1u);
        if (st != RFC_OK) {
            return st;
        }
        out->has_imir = 1;
        out->imir_axis = a & 1u;
    }
    out->width = (out->irot & 1u) ? h : w;
    out->height = (out->irot & 1u) ? w : h;
    out->dims_known = 1;

    if (out->has_imir && out->irot != 0u) {
        /* the host's WIC HEIF encoder and decoder disagree on a mirror
         * combined with a rotation (EXIF orientations 5 and 7 come back
         * swapped), so such a picture's displayed orientation is unverified */
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: a mirrored and rotated image (the system decoder's combined mirror and rotation is not verified)");
    }

    if (tb.moov) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: an image sequence track (sequences are not supported)");
    }
    top_level tl = {0u, 0u, RFC_OK};
    st = each_item(c, top_level_cb, &tl);
    if (st != RFC_OK) {
        return st;
    }
    if (tl.count > 1u) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: more than one top-level image");
    }
    if (tl.count == 0u || tl.first_id != c->primary) {
        return RFC_ERR_INVALID_ARGUMENT; /* the primary is not a picture */
    }
    if (prim.type == FOURCC('i', 'd', 'e', 'n') ||
        prim.type == FOURCC('i', 'o', 'v', 'l')) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: a derived (iden) or overlay (iovl) primary image");
    }
    uint32_t coded_id = c->primary, coded_type = prim.type;
    if (prim.type == FOURCC('g', 'r', 'i', 'd')) {
        tile_scan ts = {c->primary, 0u, 0u, 0u};
        st = each_ref(c, tile_ref_cb, &ts);
        if (st != RFC_OK) {
            return st;
        }
        if (ts.tiles == 0u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        out->hevc = ts.hevc_tiles > 0u;
        coded_id = ts.first_tile;
        item tile;
        st = item_by_id(c, coded_id, &tile, &ok);
        if (st != RFC_OK) {
            return st;
        }
        coded_type = tile.type;
    } else if (prim.type == FOURCC('h', 'v', 'c', '1')) {
        out->hevc = 1;
    } else if (prim.type != FOURCC('a', 'v', '0', '1')) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: the primary image coding is not HEVC, AV1 or a grid of them");
    }
    out->bit_depth = 8u;
    st = scan_depth(c, c->primary, prim.type, &out->bit_depth);
    if (st == RFC_OK && coded_id != c->primary) {
        uint32_t td = out->bit_depth;
        st = scan_depth(c, coded_id, coded_type, &td);
        if (td > out->bit_depth) {
            out->bit_depth = td;
        }
    }
    if (st != RFC_OK) {
        return st;
    }
    aux_scan ax = {c->primary, 0, 0};
    st = each_ref(c, aux_ref_cb, &ax);
    if (st != RFC_OK) {
        return st;
    }
    if (ax.other) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: an auxiliary image (depth map or gain map) cannot be carried");
    }
    if (ax.alpha) {
        return refuse(c, RFC_ERR_UNSUPPORTED_ALPHA,
                      "heif: an alpha auxiliary image (alpha HEIF import is not verified)");
    }
    if (out->bit_depth > 8u) {
        return refuse(c, RFC_ERR_UNSUPPORTED_SAMPLE,
                      "heif: a 10- or 12-bit image (the system decoder's high-depth output is not verified)");
    }
    colr_scan cs = {out};
    st = each_prop_of(c, c->primary, colr_cb, &cs);
    if (st != RFC_OK) {
        return st;
    }
    if (out->has_icc) {
        out->has_nclx = 0; /* ICC takes precedence */
    } else if (out->has_nclx && (out->transfer == 16u || out->transfer == 18u)) {
        return refuse(c, RFC_ERR_UNSUPPORTED_COMBINATION,
                      "heif: a PQ or HLG transfer (the system decoder's HDR output is not specified)");
    }
    exif_scan ex = {0, 0};
    st = each_item(c, exif_cb, &ex);
    if (st != RFC_OK) {
        return st;
    }
    out->has_exif = ex.exif;
    out->has_xmp = ex.xmp;
    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = RFC_SAMPLE_RGB8;
    lay.alpha = RFC_ALPHA_NONE;
    lay.width = out->width;
    lay.height = out->height;
    lay.bits_per_sample = 8u;
    return rfc_layout_normalize_stride(&lay, &out->layout);
}

rfc_status rfc_heif_scan(rfc_heif_read_fn read_at, void *ctx, uint64_t size,
                         rfc_heif_info *out, const char **detail,
                         int *read_failed) {
    if (detail) {
        *detail = NULL;
    }
    if (read_failed) {
        *read_failed = 0;
    }
    if (!read_at || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    heif_ctx c;
    memset(&c, 0, sizeof(c));
    c.r.read_at = read_at;
    c.r.ctx = ctx;
    c.r.size = size;
    rfc_status st = heif_scan_core(&c, out);
    if (detail) {
        *detail = c.detail;
    }
    if (read_failed) {
        *read_failed = c.r.read_failed;
    }
    return st;
}
