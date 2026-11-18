/* The GIF structural walk shared by the submit sniff and the GIF adapter
 * (see rfc_gif_scan.h). GIF89a: header, logical screen descriptor, colour
 * tables, extension blocks (appendix 15-24), image descriptor. */

#include "rfc_gif_scan.h"

#include <stdlib.h>
#include <string.h>

#define GIF_WINDOW (64u * 1024u)

rfc_status rfc_gif_walk_begin(rfc_gif_walk *w, rfc_gif_read_fn read_at,
                              void *ctx, const rfc_exec_context *exec,
                              const rfc_limits *limits,
                              int (*cancelled)(void *), void *cancel_ctx) {
    memset(w, 0, sizeof(*w));
    if (!read_at || !limits) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    w->win = (uint8_t *)malloc(GIF_WINDOW);
    if (!w->win) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    w->read_at = read_at;
    w->ctx = ctx;
    w->exec = exec;
    w->limits = limits;
    w->cancelled = cancelled;
    w->cancel_ctx = cancel_ctx;
    return RFC_OK;
}

void rfc_gif_walk_end(rfc_gif_walk *w) {
    free(w->win);
    w->win = NULL;
}

static void gif_detail(rfc_gif_walk *w, const char *d) {
    if (w->exec) {
        rfc_exec_set_detail(w->exec, d);
    }
}

rfc_status rfc_gif_byte(rfc_gif_walk *w, uint64_t pos, uint8_t *out) {
    if (pos < w->win_base || pos >= w->win_base + w->win_len) {
        size_t got = 0u;
        rfc_status st = w->read_at(w->ctx, pos, w->win, GIF_WINDOW, &got);
        if (st != RFC_OK) {
            return st;
        }
        if (got == 0u) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        w->win_base = pos;
        w->win_len = got;
    }
    *out = w->win[pos - w->win_base];
    return RFC_OK;
}

static rfc_status gif_bytes(rfc_gif_walk *w, uint64_t pos, uint8_t *dst,
                            size_t n) {
    for (size_t i = 0u; i < n; i++) {
        rfc_status st = rfc_gif_byte(w, pos + i, &dst[i]);
        if (st != RFC_OK) {
            return st;
        }
    }
    return RFC_OK;
}

static int gif_is_cancelled(rfc_gif_walk *w) {
    return w->cancelled && w->cancelled(w->cancel_ctx);
}

rfc_status rfc_gif_next_record(rfc_gif_walk *w, uint64_t *pos,
                               uint8_t *out_kind) {
    uint64_t p = *pos;
    for (;;) {
        if (gif_is_cancelled(w)) {
            return RFC_ERR_CANCELLED;
        }
        uint8_t blk = 0u;
        rfc_status st = rfc_gif_byte(w, p, &blk);
        if (st != RFC_OK) {
            return st;
        }
        if (blk == 0x3Bu) {
            *pos = p + 1u;
            *out_kind = blk;
            return RFC_OK;
        }
        if (blk == 0x2Cu) {
            *pos = p;
            *out_kind = blk;
            return RFC_OK;
        }
        if (blk != 0x21u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        uint8_t label = 0u;
        st = rfc_gif_byte(w, p + 1u, &label);
        if (st != RFC_OK) {
            return st;
        }
        p += 2u;
        if (++w->extensions > RFC_GIF_MAX_EXTENSIONS) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        if (label == 0x01u) {
            /* Plain Text: a graphic rendering block needing composition */
            gif_detail(w, "gif: a Plain Text Extension needs composition");
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
        if (label == 0xF9u && w->gce_seen) {
            return RFC_ERR_INVALID_ARGUMENT; /* one GCE per graphic */
        }
        int first = 1;
        for (;;) {
            if ((++w->blocks & 255u) == 0u && gif_is_cancelled(w)) {
                return RFC_ERR_CANCELLED;
            }
            uint8_t n = 0u;
            st = rfc_gif_byte(w, p, &n);
            if (st != RFC_OK) {
                return st;
            }
            p += 1u;
            if (n == 0u) {
                break;
            }
            w->ext_bytes += n;
            /* the limit applies as written, 0 included */
            if (w->ext_bytes > (uint64_t)w->limits->max_metadata_bytes) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            uint8_t last = 0u;
            st = rfc_gif_byte(w, p + n - 1u, &last); /* the block is present */
            if (st != RFC_OK) {
                return st;
            }
            if (label == 0xF9u) {
                /* exactly one four-byte block, then the terminator */
                if (n != 4u || !first) {
                    return RFC_ERR_INVALID_ARGUMENT;
                }
                w->gce_seen = 1;
                if (w->images == 0u) {
                    uint8_t g[4];
                    st = gif_bytes(w, p, g, 4u);
                    if (st != RFC_OK) {
                        return st;
                    }
                    w->has_transparent = (g[0] & 1u) != 0u;
                    w->delay_cs = (uint32_t)g[1] | ((uint32_t)g[2] << 8);
                    w->transparent = g[3];
                }
            }
            first = 0;
            p += n;
        }
        if (label == 0xF9u && first) {
            return RFC_ERR_INVALID_ARGUMENT; /* zero-length GCE */
        }
    }
}

rfc_status rfc_gif_scan_first_image(rfc_gif_walk *w, rfc_gif_image *out) {
    memset(out, 0, sizeof(*out));
    uint8_t h[13];
    size_t got = 0u;
    rfc_status st = w->read_at(w->ctx, 0u, h, sizeof(h), &got);
    if (st != RFC_OK) {
        return st;
    }
    if (got < 6u ||
        (memcmp(h, "GIF87a", 6) != 0 && memcmp(h, "GIF89a", 6) != 0)) {
        return got < 6u ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_INVALID_ARGUMENT;
    }
    if (got < sizeof(h)) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    out->width = (uint32_t)h[6] | ((uint32_t)h[7] << 8);
    out->height = (uint32_t)h[8] | ((uint32_t)h[9] << 8);
    if (out->width == 0u || out->height == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t gct = (h[10] & 0x80u) ? (2u << (h[10] & 7u)) : 0u;
    uint64_t pos = 13u + 3u * (uint64_t)gct;
    if (gct) {
        uint8_t last = 0u;
        st = rfc_gif_byte(w, pos - 1u, &last);
        if (st != RFC_OK) {
            return st;
        }
    }
    uint8_t kind = 0u;
    st = rfc_gif_next_record(w, &pos, &kind);
    if (st != RFC_OK) {
        return st;
    }
    if (kind != 0x2Cu) {
        return RFC_ERR_INVALID_ARGUMENT; /* a trailer before any image */
    }
    uint8_t d[10];
    st = gif_bytes(w, pos, d, sizeof(d));
    if (st != RFC_OK) {
        return st;
    }
    w->gce_seen = 0; /* the next graphic may carry its own GCE */
    w->images = 1u;
    uint32_t x = (uint32_t)d[1] | ((uint32_t)d[2] << 8);
    uint32_t y = (uint32_t)d[3] | ((uint32_t)d[4] << 8);
    uint32_t iw = (uint32_t)d[5] | ((uint32_t)d[6] << 8);
    uint32_t ih = (uint32_t)d[7] | ((uint32_t)d[8] << 8);
    uint8_t fl = d[9];
    pos += 10u;
    if (x != 0u || y != 0u || iw != out->width || ih != out->height) {
        gif_detail(w, "gif: the image does not cover the logical screen at the origin");
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    out->interlaced = (fl & 0x40u) != 0u;
    out->colors = gct;
    if (fl & 0x80u) {
        out->colors = 2u << (fl & 7u);
        pos += 3u * (uint64_t)out->colors;
        uint8_t last = 0u;
        st = rfc_gif_byte(w, pos - 1u, &last);
        if (st != RFC_OK) {
            return st;
        }
    }
    if (out->colors == 0u) {
        return RFC_ERR_INVALID_ARGUMENT; /* no colour table */
    }
    out->delay_cs = w->delay_cs;
    out->transparent = w->transparent;
    out->has_transparent = w->has_transparent && w->transparent < out->colors;
    rfc_layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.format = out->has_transparent ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGB8;
    lay.alpha = out->has_transparent ? RFC_ALPHA_STRAIGHT : RFC_ALPHA_NONE;
    lay.width = out->width;
    lay.height = out->height;
    lay.bits_per_sample = 8u;
    st = rfc_layout_normalize_stride(&lay, &out->layout);
    if (st != RFC_OK) {
        return st;
    }
    out->lzw_pos = pos;
    return RFC_OK;
}
