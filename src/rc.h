/* Binary range coder shared by method 1 ("CM"), the palette methods (2, 3, 4), and screen content (5, 6).
   spec/FOLD.md ("Range coder") is the normative description. */
#ifndef RF_RC_H
#define RF_RC_H

#include "rf.h"

typedef struct {
    uint64_t low;
    uint32_t range;
    uint32_t cache;
    uint64_t pending;
    uint8_t *out;
    size_t pos, cap;
} renc;

static void renc_init(renc *e, uint8_t *out, size_t cap) {
    e->low = 0; e->range = 0xFFFFFFFFu; e->cache = 0; e->pending = 1;
    e->out = out; e->pos = 0; e->cap = cap;
}
static RF_NOINLINE void renc_shift(renc *e) {
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint32_t carry = (uint32_t)(e->low >> 32), b = e->cache;
        do {
            if (e->pos < e->cap) e->out[e->pos] = (uint8_t)(b + carry);
            e->pos++;
            b = 0xFF;
        } while (--e->pending != 0);
        e->cache = (uint32_t)(e->low >> 24) & 0xFF;
    }
    e->pending++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}
RF_INLINE void renc_bit(renc *e, int bit, uint32_t p1) {
    uint32_t bound = (e->range >> 12) * p1;
    if (bit) e->range = bound;
    else { e->low += bound; e->range -= bound; }
    while (e->range < (1u << 24)) { e->range <<= 8; renc_shift(e); }
}
/* Method 4 codes with a 16-bit probability and an exact 64-bit split: with
   range >= 2^24 and 1 <= p1 <= 65535 both parts stay >= 2^8, so at most two
   bytes restore range >= 2^24, as for the 12-bit split above. */
RF_INLINE void renc_bit16(renc *e, int bit, uint32_t p1) {
    uint32_t bound = (uint32_t)(((uint64_t)e->range * p1) >> 16);
    if (bit) e->range = bound;
    else { e->low += bound; e->range -= bound; }
    while (e->range < (1u << 24)) { e->range <<= 8; renc_shift(e); }
}
static void renc_flush(renc *e) { for (int i = 0; i < 5; i++) renc_shift(e); }

/* The decoder reads its stream straight from the input buffer, switching to a
   128-byte zero-padded tail buffer for the last 40 bytes so one sample (up to
   16 decisions / 32 stream bytes plus 2-byte lookahead) can decode before
   checking the tail boundary. A stream that needs more bytes than it has ends
   with in > end, which the caller rejects. */
typedef struct {
    uint32_t range, code;
    const uint8_t *in, *end, *lim;      /* lim = end - 40 before tail, then end + 2 */
    uint8_t tail[128];
} rdec;

static void rdec_init(rdec *d, const uint8_t *in, size_t n) {
    d->range = 0xFFFFFFFFu; d->code = 0;
    for (int i = 0; i < 5; i++) d->code = (d->code << 8) | in[i];
    memset(d->tail, 0, sizeof d->tail);
    size_t rem = n - 5;
    if (rem <= 48) {
        if (rem) memcpy(d->tail, in + 5, rem);
        d->in = d->tail; d->end = d->tail + rem; d->lim = d->end + 2;
    } else {
        memcpy(d->tail, in + n - 48, 48);
        d->in = in + 5; d->end = in + n; d->lim = in + n - 40;
    }
}
static RF_NOINLINE void rdec_tail(rdec *d) {
    if (d->lim < d->end) {
        d->in = d->tail + 48 - (d->end - d->in);
        d->end = d->tail + 48;
        d->lim = d->tail + 50;
    }
    d->in = d->in > d->lim ? d->lim : d->in;
}
/* After any decision range >= 2^12, so at most two bytes restore range >= 2^24. */
RF_INLINE int rdec_bit(rdec *d, uint32_t p1) {
    uint32_t bound = (d->range >> 12) * p1;
    int bit = d->code < bound;
    uint32_t r0 = d->range - bound, c0 = d->code - bound;
    uint32_t range = bit ? bound : r0;
    uint32_t code = bit ? d->code : c0;
    uint32_t n = (range < (1u << 24)) + (range < (1u << 16));
    uint32_t next = ((uint32_t)d->in[0] << 8) | d->in[1];
    uint64_t c64 = ((uint64_t)code << 16 | next) >> (16 - 8 * n);
    d->code = (uint32_t)c64;
    d->range = range << (8 * n);
    d->in += n;
    return bit;
}
RF_INLINE int rdec_bit16(rdec *d, uint32_t p1) {
    uint32_t bound = (uint32_t)(((uint64_t)d->range * p1) >> 16);
    int bit = d->code < bound;
    uint32_t r0 = d->range - bound, c0 = d->code - bound;
    uint32_t range = bit ? bound : r0;
    uint32_t code = bit ? d->code : c0;
    uint32_t n = (range < (1u << 24)) + (range < (1u << 16));
    uint32_t next = ((uint32_t)d->in[0] << 8) | d->in[1];
    uint64_t c64 = ((uint64_t)code << 16 | next) >> (16 - 8 * n);
    d->code = (uint32_t)c64;
    d->range = range << (8 * n);
    d->in += n;
    return bit;
}

#endif
