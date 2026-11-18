/* GIF structural walk shared by the submit-time sniff and the GIF session
 * adapter (D10 T02 s6), so both reach the same decision about a file under
 * the same limits: the header and colour tables, every extension (bounded
 * count and payload, one four-byte Graphics Control Extension per graphic,
 * Plain Text refused), and the first image descriptor with the layout it
 * decodes to. Reads go through a positional callback, so a file on disk and
 * bytes in memory are walked identically. */
#ifndef RFC_GIF_SCAN_H
#define RFC_GIF_SCAN_H

#include "rfc_codec.h"
#include "rfc_job.h"

#include <stddef.h>
#include <stdint.h>

#define RFC_GIF_MAX_EXTENSIONS 64u
#define RFC_GIF_WINDOW 4096u

/* Positional read: up to n bytes at off into dst, the count in *got (fewer
 * only at the end of the input). */
typedef rfc_status (*rfc_gif_read_fn)(void *ctx, uint64_t off, uint8_t *dst,
                                      size_t n, size_t *got);

typedef struct rfc_gif_walk {
    rfc_gif_read_fn read_at;
    void *ctx;
    const rfc_exec_context *exec;   /* refusal details; may be NULL */
    const rfc_limits *limits;
    int (*cancelled)(void *cctx);   /* may be NULL */
    void *cancel_ctx;
    /* state carried across records */
    uint32_t extensions;
    uint64_t ext_bytes;
    int gce_seen;
    uint32_t images;
    uint32_t blocks;
    /* the Graphics Control Extension that governs the first image */
    int has_transparent;
    uint8_t transparent;
    uint32_t delay_cs;
    /* a read window inside the walk: no allocation */
    uint64_t win_base;
    size_t win_len;
    uint8_t win[RFC_GIF_WINDOW];
} rfc_gif_walk;

typedef struct rfc_gif_image {
    uint32_t width, height;   /* the logical screen; the image covers it */
    uint32_t colors;          /* active colour table size */
    int interlaced;
    int has_transparent;      /* a transparent index inside the table */
    uint8_t transparent;
    uint32_t delay_cs;
    rfc_layout layout;        /* rgb8, or rgba8 with transparency */
    uint64_t lzw_pos;         /* offset of the LZW minimum code size byte */
} rfc_gif_image;

/* Prepare a walk; it allocates nothing (rfc_gif_walk_end is a no-op kept
 * for symmetry). A read error from read_at is returned as it is. */
rfc_status rfc_gif_walk_begin(rfc_gif_walk *w, rfc_gif_read_fn read_at,
                              void *ctx, const rfc_exec_context *exec,
                              const rfc_limits *limits,
                              int (*cancelled)(void *), void *cancel_ctx);
void rfc_gif_walk_end(rfc_gif_walk *w);

/* Header, global colour table, extensions and the first image descriptor
 * with its local colour table. The layout it decodes to is reported, not
 * checked: the adapter applies the image limits to it after the walk, and
 * admission applies them at submit, so both enforce the same layout. */
rfc_status rfc_gif_scan_first_image(rfc_gif_walk *w, rfc_gif_image *out);

/* From *pos, walk extensions to the next record: *out_kind is 0x2C (an image
 * descriptor at *pos) or 0x3B (the trailer, *pos after it). */
rfc_status rfc_gif_next_record(rfc_gif_walk *w, uint64_t *pos,
                               uint8_t *out_kind);

/* One byte at pos through the window; RFC_ERR_TRUNCATED_INPUT past the end. */
rfc_status rfc_gif_byte(rfc_gif_walk *w, uint64_t pos, uint8_t *out);

#endif /* RFC_GIF_SCAN_H */
