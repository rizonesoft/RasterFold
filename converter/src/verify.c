#include "rfc_verify.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void *vf_default_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void vf_default_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static void copy_cstr_bounded(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_cap) {
        len = dst_cap - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

const char *rfc_exactness_class_name(rfc_exactness_class cls) {
    switch (cls) {
        case RFC_EXACT_PIXEL_EXACT: return "pixel-exact";
        case RFC_EXACT_BYTE_IDENTICAL: return "byte-identical";
        case RFC_EXACT_COEFFICIENT_EXACT: return "coefficient-exact";
        case RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION:
            return "original-byte-reconstruction";
        default: return "unknown-exactness";
    }
}

rfc_status rfc_exactness_class_parse(const char *str, rfc_exactness_class *out) {
    if (!str || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(str, "pixel-exact") == 0) {
        *out = RFC_EXACT_PIXEL_EXACT;
        return RFC_OK;
    }
    if (strcmp(str, "byte-identical") == 0) {
        *out = RFC_EXACT_BYTE_IDENTICAL;
        return RFC_OK;
    }
    if (strcmp(str, "coefficient-exact") == 0) {
        *out = RFC_EXACT_COEFFICIENT_EXACT;
        return RFC_OK;
    }
    if (strcmp(str, "original-byte-reconstruction") == 0) {
        *out = RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION;
        return RFC_OK;
    }
    return RFC_ERR_SETTING_VALUE;
}

const char *rfc_mismatch_kind_name(rfc_mismatch_kind kind) {
    switch (kind) {
        case RFC_MISMATCH_NONE: return "none";
        case RFC_MISMATCH_FRAME_COUNT: return "frame-count";
        case RFC_MISMATCH_DIMENSIONS: return "dimensions";
        case RFC_MISMATCH_SAMPLE_FORMAT: return "sample-format";
        case RFC_MISMATCH_BITS_PER_SAMPLE: return "bits-per-sample";
        case RFC_MISMATCH_ALPHA_MODE: return "alpha-mode";
        case RFC_MISMATCH_DURATION_MS: return "duration-ms";
        case RFC_MISMATCH_LOW_PADDING_BITS: return "low-padding-bits";
        case RFC_MISMATCH_SAMPLE_VALUE: return "sample-value";
        case RFC_MISMATCH_TRANSPARENT_RGB: return "transparent-rgb";
        case RFC_MISMATCH_FLOAT_BIT_PATTERN: return "float-bit-pattern";
        case RFC_MISMATCH_BYTE_HASH: return "byte-hash";
        case RFC_MISMATCH_JPEG_DIMENSIONS: return "jpeg-dimensions";
        case RFC_MISMATCH_JPEG_CODING_PARAMS: return "jpeg-coding-params";
        case RFC_MISMATCH_JPEG_COMPONENT_SPEC: return "jpeg-component-spec";
        case RFC_MISMATCH_JPEG_QUANT_TABLE: return "jpeg-quant-table";
        case RFC_MISMATCH_JPEG_DCT_COEFF: return "jpeg-dct-coeff";
        case RFC_MISMATCH_METADATA_LOST: return "metadata-lost";
        case RFC_MISMATCH_METADATA_RETAINED: return "metadata-retained";
        case RFC_MISMATCH_METADATA_UNSUPPORTED: return "metadata-unsupported";
        default: return "unknown-mismatch";
    }
}

/* FIPS 180-4 SHA-256 implementation without external dependencies. */
typedef struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buf_len;
} sha256_ctx;

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(sha256_ctx *ctx, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t w[64];
    for (size_t i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)block[i * 4u] << 24) |
               ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) |
               ((uint32_t)block[i * 4u + 3u]);
    }
    for (size_t i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^ rotr32(w[i - 15u], 18u) ^
                      (w[i - 15u] >> 3);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^ rotr32(w[i - 2u], 19u) ^
                      (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (size_t i = 0; i < 64u; i++) {
        uint32_t S1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0u;
    ctx->buf_len = 0u;
}

static void sha256_update(sha256_ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (!p || len == 0u) {
        return;
    }
    ctx->bitlen += (uint64_t)len * 8ull;
    while (len > 0u) {
        size_t take = 64u - ctx->buf_len;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len == 64u) {
            sha256_transform(ctx, ctx->buf);
            ctx->buf_len = 0u;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, uint8_t out_hash[32]) {
    size_t i = ctx->buf_len;
    ctx->buf[i++] = 0x80u;
    if (i > 56u) {
        while (i < 64u) {
            ctx->buf[i++] = 0u;
        }
        sha256_transform(ctx, ctx->buf);
        i = 0u;
    }
    while (i < 56u) {
        ctx->buf[i++] = 0u;
    }
    for (int shift = 56, idx = 56; idx < 64; idx++, shift -= 8) {
        ctx->buf[idx] = (uint8_t)((ctx->bitlen >> shift) & 0xffu);
    }
    sha256_transform(ctx, ctx->buf);
    for (size_t s = 0; s < 8u; s++) {
        out_hash[s * 4u] = (uint8_t)((ctx->state[s] >> 24) & 0xffu);
        out_hash[s * 4u + 1u] = (uint8_t)((ctx->state[s] >> 16) & 0xffu);
        out_hash[s * 4u + 2u] = (uint8_t)((ctx->state[s] >> 8) & 0xffu);
        out_hash[s * 4u + 3u] = (uint8_t)(ctx->state[s] & 0xffu);
    }
}

static void sha256_update_u32_le(sha256_ctx *ctx, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xffu),
        (uint8_t)((v >> 8) & 0xffu),
        (uint8_t)((v >> 16) & 0xffu),
        (uint8_t)((v >> 24) & 0xffu)
    };
    sha256_update(ctx, b, 4u);
}

static void sha256_update_u64_le(sha256_ctx *ctx, uint64_t v) {
    uint8_t b[8] = {
        (uint8_t)(v & 0xffu),
        (uint8_t)((v >> 8) & 0xffu),
        (uint8_t)((v >> 16) & 0xffu),
        (uint8_t)((v >> 24) & 0xffu),
        (uint8_t)((v >> 32) & 0xffu),
        (uint8_t)((v >> 40) & 0xffu),
        (uint8_t)((v >> 48) & 0xffu),
        (uint8_t)((v >> 56) & 0xffu)
    };
    sha256_update(ctx, b, 8u);
}

void rfc_bytes_hash_sha256(const uint8_t *data, size_t len, uint8_t out_hash[32]) {
    if (!out_hash) {
        return;
    }
    sha256_ctx ctx;
    sha256_init(&ctx);
    if (len > 0u && data != NULL) {
        sha256_update(&ctx, data, len);
    }
    sha256_final(&ctx, out_hash);
}

static int sample_is_float(rfc_sample_format fmt) {
    return fmt == RFC_SAMPLE_RGB_HALF || fmt == RFC_SAMPLE_RGBA_HALF ||
           fmt == RFC_SAMPLE_RGB_FLOAT || fmt == RFC_SAMPLE_RGBA_FLOAT;
}

static uint32_t sample_channels(rfc_sample_format fmt) {
    switch (fmt) {
        case RFC_SAMPLE_GRAY8:
        case RFC_SAMPLE_GRAY16:
            return 1u;
        case RFC_SAMPLE_RGB8:
        case RFC_SAMPLE_RGB16:
        case RFC_SAMPLE_RGB_HALF:
        case RFC_SAMPLE_RGB_FLOAT:
            return 3u;
        case RFC_SAMPLE_RGBA8:
        case RFC_SAMPLE_RGBA16:
        case RFC_SAMPLE_RGBA_HALF:
        case RFC_SAMPLE_RGBA_FLOAT:
            return 4u;
        default:
            return 0u;
    }
}

static uint32_t sample_channel_bytes(rfc_sample_format fmt) {
    switch (fmt) {
        case RFC_SAMPLE_GRAY8:
        case RFC_SAMPLE_RGB8:
        case RFC_SAMPLE_RGBA8:
            return 1u;
        case RFC_SAMPLE_GRAY16:
        case RFC_SAMPLE_RGB16:
        case RFC_SAMPLE_RGBA16:
        case RFC_SAMPLE_RGB_HALF:
        case RFC_SAMPLE_RGBA_HALF:
            return 2u;
        case RFC_SAMPLE_RGB_FLOAT:
        case RFC_SAMPLE_RGBA_FLOAT:
            return 4u;
        default:
            return 0u;
    }
}

static uint16_t read_u16_le(const uint8_t *p) {
    uint16_t v = 0u;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t read_u32_le(const uint8_t *p) {
    uint32_t v = 0u;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t read_sample_bits(const uint8_t *row,
                                 uint32_t x,
                                 uint32_t c,
                                 uint32_t channels,
                                 uint32_t ch_bytes) {
    const uint8_t *p = row + ((size_t)x * (size_t)channels + (size_t)c) *
                                 (size_t)ch_bytes;
    if (ch_bytes == 1u) {
        return (uint32_t)p[0];
    }
    if (ch_bytes == 2u) {
        return (uint32_t)read_u16_le(p);
    }
    return read_u32_le(p);
}

static uint32_t low_padding_mask(const rfc_layout *layout) {
    if (sample_is_float(layout->format)) {
        return 0u;
    }
    uint32_t storage_bits = sample_channel_bytes(layout->format) * 8u;
    if (storage_bits == 0u || layout->bits_per_sample >= storage_bits) {
        return 0u;
    }
    uint32_t pad_bits = storage_bits - layout->bits_per_sample;
    if (pad_bits >= 32u) {
        return 0xffffffffu;
    }
    return (1u << pad_bits) - 1u;
}

static void set_mismatch(rfc_sample_mismatch *out,
                         rfc_mismatch_kind kind,
                         size_t frame_index,
                         uint32_t x,
                         uint32_t y,
                         uint32_t channel,
                         uint32_t expected_bits,
                         uint32_t actual_bits,
                         const char *detail) {
    if (!out) {
        return;
    }
    out->kind = kind;
    out->frame_index = frame_index;
    out->x = x;
    out->y = y;
    out->channel = channel;
    out->expected_bits = expected_bits;
    out->actual_bits = actual_bits;
    copy_cstr_bounded(out->detail, sizeof(out->detail), detail);
}

rfc_status rfc_frames_hash_sha256(const rfc_frame *frames,
                                  size_t frame_count,
                                  uint8_t out_hash[32]) {
    if (!out_hash || (frame_count > 0u && !frames)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_hash, 0, 32u);

    sha256_ctx ctx;
    sha256_init(&ctx);
    static const uint8_t tag[14] = "RFC-FRAMES-V1";
    sha256_update(&ctx, tag, sizeof(tag));
    sha256_update_u64_le(&ctx, (uint64_t)frame_count);

    for (size_t i = 0; i < frame_count; i++) {
        rfc_status st = rfc_layout_validate(&frames[i].layout, NULL);
        if (st != RFC_OK) {
            return st;
        }
        if (!frames[i].data) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        const rfc_layout *lay = &frames[i].layout;
        size_t bpp = rfc_sample_bytes(lay->format);
        size_t active_row = (size_t)lay->width * bpp;
        if (frames[i].data_len <
            (size_t)(lay->height - 1u) * lay->stride_bytes + active_row) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        uint32_t pad_mask = low_padding_mask(lay);
        uint32_t channels = sample_channels(lay->format);
        uint32_t ch_bytes = sample_channel_bytes(lay->format);

        sha256_update_u32_le(&ctx, lay->width);
        sha256_update_u32_le(&ctx, lay->height);
        sha256_update_u32_le(&ctx, (uint32_t)lay->format);
        sha256_update_u32_le(&ctx, (uint32_t)lay->alpha);
        sha256_update_u32_le(&ctx, lay->bits_per_sample);
        sha256_update_u32_le(&ctx, frames[i].metadata.duration_ms);

        for (uint32_t y = 0; y < lay->height; y++) {
            const uint8_t *row = frames[i].data + (size_t)y * lay->stride_bytes;
            if (pad_mask != 0u) {
                for (uint32_t x = 0; x < lay->width; x++) {
                    for (uint32_t c = 0; c < channels; c++) {
                        uint32_t v =
                            read_sample_bits(row, x, c, channels, ch_bytes);
                        if ((v & pad_mask) != 0u) {
                            return RFC_ERR_VERIFY_MISMATCH;
                        }
                    }
                }
            }
            sha256_update(&ctx, row, active_row);
        }
    }

    sha256_final(&ctx, out_hash);
    return RFC_OK;
}

rfc_status rfc_verify_frames_equal(const rfc_frame *expected,
                                   size_t expected_count,
                                   const rfc_frame *actual,
                                   size_t actual_count,
                                   rfc_sample_mismatch *out_mismatch) {
    if (out_mismatch) {
        memset(out_mismatch, 0, sizeof(*out_mismatch));
    }
    if ((expected_count > 0u && !expected) || (actual_count > 0u && !actual)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (expected_count != actual_count) {
        set_mismatch(out_mismatch, RFC_MISMATCH_FRAME_COUNT, 0u, 0u, 0u, 0u,
                     (uint32_t)expected_count, (uint32_t)actual_count,
                     "frame count differs");
        return RFC_ERR_VERIFY_MISMATCH;
    }

    for (size_t i = 0; i < expected_count; i++) {
        const rfc_layout *el = &expected[i].layout;
        const rfc_layout *al = &actual[i].layout;
        rfc_status st = rfc_layout_validate(el, NULL);
        if (st != RFC_OK) {
            return st;
        }
        st = rfc_layout_validate(al, NULL);
        if (st != RFC_OK) {
            return st;
        }
        if (!expected[i].data || !actual[i].data) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        size_t ebpp = rfc_sample_bytes(el->format);
        size_t abpp = rfc_sample_bytes(al->format);
        if (expected[i].data_len <
                (size_t)(el->height - 1u) * el->stride_bytes +
                    (size_t)el->width * ebpp ||
            actual[i].data_len <
                (size_t)(al->height - 1u) * al->stride_bytes +
                    (size_t)al->width * abpp) {
            return RFC_ERR_INVALID_ARGUMENT;
        }

        if (el->width != al->width || el->height != al->height) {
            set_mismatch(out_mismatch, RFC_MISMATCH_DIMENSIONS, i, el->width,
                         el->height, 0u,
                         el->width != al->width ? el->width : el->height,
                         el->width != al->width ? al->width : al->height,
                         "frame dimensions differ");
            return RFC_ERR_VERIFY_MISMATCH;
        }
        if (el->format != al->format) {
            set_mismatch(out_mismatch, RFC_MISMATCH_SAMPLE_FORMAT, i, 0u, 0u,
                         0u, (uint32_t)el->format, (uint32_t)al->format,
                         "sample format differs");
            return RFC_ERR_VERIFY_MISMATCH;
        }
        if (el->bits_per_sample != al->bits_per_sample) {
            set_mismatch(out_mismatch, RFC_MISMATCH_BITS_PER_SAMPLE, i, 0u, 0u,
                         0u, el->bits_per_sample, al->bits_per_sample,
                         "native bits_per_sample differs");
            return RFC_ERR_VERIFY_MISMATCH;
        }
        if (el->alpha != al->alpha) {
            set_mismatch(out_mismatch, RFC_MISMATCH_ALPHA_MODE, i, 0u, 0u, 0u,
                         (uint32_t)el->alpha, (uint32_t)al->alpha,
                         "alpha mode differs");
            return RFC_ERR_VERIFY_MISMATCH;
        }
        if (expected[i].metadata.duration_ms != actual[i].metadata.duration_ms) {
            set_mismatch(out_mismatch, RFC_MISMATCH_DURATION_MS, i, 0u, 0u, 0u,
                         expected[i].metadata.duration_ms,
                         actual[i].metadata.duration_ms,
                         "frame duration_ms differs");
            return RFC_ERR_VERIFY_MISMATCH;
        }

        uint32_t channels = sample_channels(el->format);
        uint32_t ch_bytes = sample_channel_bytes(el->format);
        uint32_t pad_mask = low_padding_mask(el);
        int is_float = sample_is_float(el->format);
        uint32_t float_mag_mask = (ch_bytes == 2u) ? 0x7fffu : 0x7fffffffu;

        for (uint32_t y = 0; y < el->height; y++) {
            const uint8_t *erow =
                expected[i].data + (size_t)y * el->stride_bytes;
            const uint8_t *arow =
                actual[i].data + (size_t)y * al->stride_bytes;

            for (uint32_t x = 0; x < el->width; x++) {
                if (pad_mask != 0u) {
                    for (uint32_t c = 0; c < channels; c++) {
                        uint32_t ev =
                            read_sample_bits(erow, x, c, channels, ch_bytes);
                        uint32_t av =
                            read_sample_bits(arow, x, c, channels, ch_bytes);
                        if ((ev & pad_mask) != 0u || (av & pad_mask) != 0u) {
                            set_mismatch(out_mismatch,
                                         RFC_MISMATCH_LOW_PADDING_BITS, i, x, y,
                                         c, ev, av,
                                         "non-zero low padding bits in "
                                         "sub-16 integer sample");
                            return RFC_ERR_VERIFY_MISMATCH;
                        }
                    }
                }

                if (channels == 4u && el->alpha != RFC_ALPHA_NONE) {
                    uint32_t ea =
                        read_sample_bits(erow, x, 3u, channels, ch_bytes);
                    uint32_t aa =
                        read_sample_bits(arow, x, 3u, channels, ch_bytes);
                    if (ea != aa) {
                        set_mismatch(out_mismatch,
                                     is_float ? RFC_MISMATCH_FLOAT_BIT_PATTERN
                                              : RFC_MISMATCH_SAMPLE_VALUE,
                                     i, x, y, 3u, ea, aa,
                                     is_float
                                         ? "alpha float bit pattern differs"
                                         : "alpha sample differs");
                        return RFC_ERR_VERIFY_MISMATCH;
                    }
                    int alpha_zero =
                        is_float ? ((ea & float_mag_mask) == 0u) : (ea == 0u);
                    for (uint32_t c = 0; c < 3u; c++) {
                        uint32_t ev =
                            read_sample_bits(erow, x, c, channels, ch_bytes);
                        uint32_t av =
                            read_sample_bits(arow, x, c, channels, ch_bytes);
                        if (ev != av) {
                            if (alpha_zero) {
                                set_mismatch(out_mismatch,
                                             RFC_MISMATCH_TRANSPARENT_RGB, i, x,
                                             y, c, ev, av,
                                             "RGB under transparent pixel "
                                             "differs");
                            } else {
                                set_mismatch(
                                    out_mismatch,
                                    is_float ? RFC_MISMATCH_FLOAT_BIT_PATTERN
                                             : RFC_MISMATCH_SAMPLE_VALUE,
                                    i, x, y, c, ev, av,
                                    is_float ? "float sample bit pattern "
                                               "differs"
                                             : "sample value differs");
                            }
                            return RFC_ERR_VERIFY_MISMATCH;
                        }
                    }
                } else {
                    for (uint32_t c = 0; c < channels; c++) {
                        uint32_t ev =
                            read_sample_bits(erow, x, c, channels, ch_bytes);
                        uint32_t av =
                            read_sample_bits(arow, x, c, channels, ch_bytes);
                        if (ev != av) {
                            set_mismatch(
                                out_mismatch,
                                is_float ? RFC_MISMATCH_FLOAT_BIT_PATTERN
                                         : RFC_MISMATCH_SAMPLE_VALUE,
                                i, x, y, c, ev, av,
                                is_float ? "float sample bit pattern differs"
                                         : "sample value differs");
                            return RFC_ERR_VERIFY_MISMATCH;
                        }
                    }
                }
            }
        }
    }

    return RFC_OK;
}

rfc_status rfc_verify_bytes_exact(const uint8_t *expected_bytes,
                                  size_t expected_len,
                                  const uint8_t *actual_bytes,
                                  size_t actual_len,
                                  rfc_exactness_class exactness,
                                  const char *decoder_provenance,
                                  int independent_decoder,
                                  rfc_verify_report *out_report) {
    if (!out_report) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_report, 0, sizeof(*out_report));
    out_report->exactness = exactness;
    out_report->meta_outcome = RFC_VERIFY_META_PRESERVED;
    out_report->metadata.outcome = RFC_VERIFY_META_PRESERVED;
    out_report->independent_decoder = independent_decoder ? 1 : 0;
    copy_cstr_bounded(out_report->decoder_provenance,
                      sizeof(out_report->decoder_provenance),
                      decoder_provenance ? decoder_provenance
                                         : "byte-digest-comparison");

    if ((expected_len > 0u && !expected_bytes) ||
        (actual_len > 0u && !actual_bytes)) {
        out_report->status = RFC_ERR_INVALID_ARGUMENT;
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_bytes_hash_sha256(expected_bytes, expected_len,
                          out_report->source_hash);
    rfc_bytes_hash_sha256(actual_bytes, actual_len, out_report->verified_hash);

    if (expected_len != actual_len ||
        memcmp(out_report->source_hash, out_report->verified_hash, 32u) != 0) {
        set_mismatch(&out_report->mismatch, RFC_MISMATCH_BYTE_HASH, 0u, 0u, 0u,
                     0u, (uint32_t)(expected_len & 0xffffffffu),
                     (uint32_t)(actual_len & 0xffffffffu),
                     exactness == RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION
                         ? "reconstructed original bytes SHA-256 differs"
                         : "output bytes SHA-256 differs from source bytes");
        out_report->status = RFC_ERR_VERIFY_MISMATCH;
        out_report->valid = 1;
        return RFC_ERR_VERIFY_MISMATCH;
    }

    out_report->status = RFC_OK;
    out_report->valid = 1;
    return RFC_OK;
}

rfc_status rfc_verify_original_byte_reconstruction(
    const uint8_t *original_jpeg,
    size_t original_len,
    const uint8_t *reconstructed_jpeg,
    size_t reconstructed_len,
    rfc_verify_report *out_report) {
    if (!original_jpeg || original_len == 0u || !reconstructed_jpeg ||
        reconstructed_len == 0u || !out_report) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return rfc_verify_bytes_exact(original_jpeg, original_len,
                                  reconstructed_jpeg, reconstructed_len,
                                  RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION,
                                  "same-provider-fresh-session", 0, out_report);
}

rfc_status rfc_verify_jpeg_coefficients(const rfc_jpeg_coeff_image *expected,
                                        const rfc_jpeg_coeff_image *actual,
                                        rfc_sample_mismatch *out_mismatch) {
    if (out_mismatch) {
        memset(out_mismatch, 0, sizeof(*out_mismatch));
    }
    if (!expected || !actual) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (expected->width == 0u || expected->height == 0u ||
        expected->component_count == 0u || expected->component_count > 4u ||
        actual->component_count == 0u || actual->component_count > 4u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    if (expected->width != actual->width ||
        expected->height != actual->height) {
        set_mismatch(out_mismatch, RFC_MISMATCH_JPEG_DIMENSIONS, 0u,
                     expected->width, expected->height, 0u,
                     expected->width != actual->width ? expected->width
                                                      : expected->height,
                     expected->width != actual->width ? actual->width
                                                      : actual->height,
                     "JPEG coefficient image dimensions differ");
        return RFC_ERR_VERIFY_MISMATCH;
    }

    if (expected->progressive != actual->progressive ||
        expected->arithmetic_coding != actual->arithmetic_coding ||
        expected->baseline != actual->baseline ||
        expected->component_count != actual->component_count) {
        uint32_t eflags = (uint32_t)(expected->progressive ? 1u : 0u) |
                          (uint32_t)(expected->arithmetic_coding ? 2u : 0u) |
                          (uint32_t)(expected->baseline ? 4u : 0u) |
                          (expected->component_count << 8);
        uint32_t aflags = (uint32_t)(actual->progressive ? 1u : 0u) |
                          (uint32_t)(actual->arithmetic_coding ? 2u : 0u) |
                          (uint32_t)(actual->baseline ? 4u : 0u) |
                          (actual->component_count << 8);
        set_mismatch(out_mismatch, RFC_MISMATCH_JPEG_CODING_PARAMS, 0u, 0u, 0u,
                     0u, eflags, aflags,
                     "JPEG coding parameters or component count differ");
        return RFC_ERR_VERIFY_MISMATCH;
    }

    for (uint32_t t = 0u; t < 4u; t++) {
        if (expected->quant_tables[t].populated !=
            actual->quant_tables[t].populated) {
            set_mismatch(out_mismatch, RFC_MISMATCH_JPEG_QUANT_TABLE, 0u, t,
                         0u, 0u, (uint32_t)expected->quant_tables[t].populated,
                         (uint32_t)actual->quant_tables[t].populated,
                         "JPEG quantization table presence differs");
            return RFC_ERR_VERIFY_MISMATCH;
        }
        if (expected->quant_tables[t].populated) {
            for (uint32_t k = 0u; k < 64u; k++) {
                uint16_t ev = expected->quant_tables[t].values[k];
                uint16_t av = actual->quant_tables[t].values[k];
                if (ev != av) {
                    set_mismatch(out_mismatch, RFC_MISMATCH_JPEG_QUANT_TABLE,
                                 0u, k, t, 0u, (uint32_t)ev, (uint32_t)av,
                                 "JPEG quantization table entry differs");
                    return RFC_ERR_VERIFY_MISMATCH;
                }
            }
        }
    }

    for (uint32_t c = 0u; c < expected->component_count; c++) {
        const rfc_jpeg_component_spec *ec = &expected->components[c];
        const rfc_jpeg_component_spec *ac = &actual->components[c];
        if (ec->blocks_wide == 0u || ec->blocks_high == 0u ||
            ec->quant_table_idx >= 4u || ac->quant_table_idx >= 4u ||
            !expected->quant_tables[ec->quant_table_idx].populated ||
            !actual->quant_tables[ac->quant_table_idx].populated ||
            !ec->dct_blocks || !ac->dct_blocks) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (ec->blocks_wide > SIZE_MAX / ec->blocks_high ||
            (size_t)ec->blocks_wide * (size_t)ec->blocks_high >
                SIZE_MAX / 64u) {
            return RFC_ERR_OVERFLOW;
        }
        if (ec->component_id != ac->component_id ||
            ec->h_samp_factor != ac->h_samp_factor ||
            ec->v_samp_factor != ac->v_samp_factor ||
            ec->quant_table_idx != ac->quant_table_idx ||
            ec->dc_table_idx != ac->dc_table_idx ||
            ec->ac_table_idx != ac->ac_table_idx ||
            ec->blocks_wide != ac->blocks_wide ||
            ec->blocks_high != ac->blocks_high) {
            uint32_t esig = ((uint32_t)ec->component_id << 24) |
                            ((uint32_t)ec->h_samp_factor << 16) |
                            ((uint32_t)ec->v_samp_factor << 8) |
                            (uint32_t)ec->quant_table_idx;
            uint32_t asig = ((uint32_t)ac->component_id << 24) |
                            ((uint32_t)ac->h_samp_factor << 16) |
                            ((uint32_t)ac->v_samp_factor << 8) |
                            (uint32_t)ac->quant_table_idx;
            set_mismatch(out_mismatch, RFC_MISMATCH_JPEG_COMPONENT_SPEC, 0u,
                         ec->blocks_wide, ec->blocks_high, c, esig, asig,
                         "JPEG component sampling, table selector, or block "
                         "grid differs");
            return RFC_ERR_VERIFY_MISMATCH;
        }

        size_t block_count = (size_t)ec->blocks_wide * (size_t)ec->blocks_high;
        for (size_t b = 0u; b < block_count; b++) {
            const int16_t *eb = ec->dct_blocks + b * 64u;
            const int16_t *ab = ac->dct_blocks + b * 64u;
            for (uint32_t k = 0u; k < 64u; k++) {
                if (eb[k] != ab[k]) {
                    set_mismatch(out_mismatch, RFC_MISMATCH_JPEG_DCT_COEFF, b,
                                 k, 0u, c, (uint32_t)(uint16_t)eb[k],
                                 (uint32_t)(uint16_t)ab[k],
                                 "JPEG quantized DCT coefficient differs");
                    return RFC_ERR_VERIFY_MISMATCH;
                }
            }
        }
    }

    return RFC_OK;
}

static void free_frames_with_alloc(rfc_frame **frames_ptr,
                                   size_t *count_ptr,
                                   rfc_metadata *meta,
                                   const rfc_allocator *alloc) {
    if (meta && meta->icc) {
        if (!frames_ptr || !*frames_ptr || !count_ptr || *count_ptr == 0u ||
            (*frames_ptr)[0].metadata.icc != meta->icc) {
            if (alloc && alloc->free) {
                alloc->free(alloc->ctx, (void *)meta->icc);
            } else {
                free((void *)meta->icc);
            }
        }
        meta->icc = NULL;
        meta->icc_len = 0u;
    }
    if (!frames_ptr || !*frames_ptr) {
        if (count_ptr) {
            *count_ptr = 0u;
        }
        return;
    }
    size_t n = count_ptr ? *count_ptr : 0u;
    for (size_t i = 0u; i < n; i++) {
        rfc_frame_free(&(*frames_ptr)[i]);
    }
    if (alloc && alloc->free) {
        alloc->free(alloc->ctx, *frames_ptr);
    } else {
        free(*frames_ptr);
    }
    *frames_ptr = NULL;
    if (count_ptr) {
        *count_ptr = 0u;
    }
}

rfc_status rfc_verify_encoded_fresh(rfc_format_id target_format,
                                    const rfc_encoded *encoded,
                                    const rfc_frame *expected_frames,
                                    size_t expected_count,
                                    const rfc_metadata *expected_meta,
                                    rfc_metadata_policy meta_policy,
                                    uint32_t acknowledged_unsupported,
                                    const rfc_setting_value *decode_settings,
                                    size_t decode_setting_count,
                                    const rfc_limits *limits,
                                    const rfc_allocator *alloc,
                                    rfc_verify_report *out_report) {
    if (!out_report) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_report, 0, sizeof(*out_report));
    out_report->exactness = RFC_EXACT_PIXEL_EXACT;
    out_report->independent_decoder = 0;
    copy_cstr_bounded(out_report->decoder_provenance,
                      sizeof(out_report->decoder_provenance),
                      "same-provider-fresh-session");

    if (!encoded || !encoded->data || encoded->len == 0u || !expected_frames ||
        expected_count == 0u) {
        out_report->status = RFC_ERR_INVALID_ARGUMENT;
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_limits eff_limits;
    if (limits) {
        eff_limits = *limits;
        rfc_status lst = rfc_limits_validate(&eff_limits);
        if (lst != RFC_OK) {
            out_report->status = lst;
            return lst;
        }
    } else {
        rfc_limits_default(&eff_limits);
    }

    rfc_allocator eff_alloc = {vf_default_alloc, vf_default_free, NULL};
    if (alloc) {
        eff_alloc = *alloc;
    }
    if (!eff_alloc.alloc || !eff_alloc.free) {
        out_report->status = RFC_ERR_INVALID_ARGUMENT;
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_status st = rfc_frames_hash_sha256(expected_frames, expected_count,
                                           out_report->source_hash);
    if (st != RFC_OK) {
        out_report->status = st;
        return st;
    }

    uint32_t present_meta = rfc_metadata_source_items(
        expected_meta, expected_frames, expected_count);
    st = rfc_metadata_check_preflight(
        target_format, meta_policy, RFC_EXACT_PIXEL_EXACT, present_meta,
        acknowledged_unsupported, &out_report->metadata);
    if (st != RFC_OK) {
        if (st == RFC_ERR_METADATA_POLICY) {
            set_mismatch(&out_report->mismatch, RFC_MISMATCH_METADATA_UNSUPPORTED,
                         0u, 0u, 0u, 0u, present_meta,
                         out_report->metadata.unsupported_items,
                         out_report->metadata.detail);
        }
        out_report->status = st;
        out_report->valid = 1;
        return st;
    }

    const rfc_session_adapter *sadp =
        rfc_registry_acquire_session(target_format);
    if (!sadp) {
        out_report->status = RFC_ERR_NO_ADAPTER;
        return RFC_ERR_NO_ADAPTER;
    }

    rfc_exec_context exec;
    memset(&exec, 0, sizeof(exec));
    exec.codec_threads = 1u;
    exec.limits = eff_limits;
    exec.allocator = &eff_alloc;

    void *sess = NULL;
    st = sadp->open_session(&exec, &sess);
    if (st != RFC_OK) {
        rfc_registry_release_session(target_format);
        out_report->status = st;
        return st;
    }

    rfc_image_probe probe;
    memset(&probe, 0, sizeof(probe));
    rfc_frame *decoded_frames = NULL;
    size_t decoded_count = 0u;
    rfc_metadata decoded_meta;
    memset(&decoded_meta, 0, sizeof(decoded_meta));

    st = rfc_limits_check_compressed(&eff_limits, (uint64_t)encoded->len);
    if (st == RFC_OK) {
        st = sadp->probe_session(sess, &exec, encoded->data, encoded->len,
                                 &probe);
        if (st == RFC_OK) {
            st = rfc_limits_check_probe(&eff_limits, &probe,
                                        (uint64_t)encoded->len, 1);
        }
        if (probe.container_meta.icc) {
            eff_alloc.free(eff_alloc.ctx, (void *)probe.container_meta.icc);
            probe.container_meta.icc = NULL;
        }
    }
    if (st == RFC_OK) {
        st = sadp->decode_session(sess, &exec, encoded->data, encoded->len,
                                  decode_settings, decode_setting_count,
                                  &decoded_frames, &decoded_count,
                                  &decoded_meta);
    }

    sadp->close_session(sess);
    rfc_registry_release_session(target_format);

    if (st == RFC_OK) {
        if (!decoded_frames || decoded_count == 0u) {
            st = RFC_ERR_INVALID_ARGUMENT;
        } else {
            st = rfc_limits_check_frames(&eff_limits, decoded_frames,
                                         decoded_count, &decoded_meta,
                                         (uint64_t)encoded->len, 1);
        }
    }
    if (st == RFC_OK) {
        st = rfc_frames_hash_sha256(decoded_frames, decoded_count,
                                    out_report->verified_hash);
    }
    if (st == RFC_OK) {
        st = rfc_verify_frames_equal(expected_frames, expected_count,
                                     decoded_frames, decoded_count,
                                     &out_report->mismatch);
    }
    if (st == RFC_OK) {
        st = rfc_metadata_verify(
            target_format, meta_policy, RFC_EXACT_PIXEL_EXACT, expected_meta,
            expected_frames, expected_count, &decoded_meta, decoded_frames,
            decoded_count, acknowledged_unsupported, &out_report->metadata,
            &out_report->mismatch);
        out_report->meta_outcome = out_report->metadata.outcome;
    }

    free_frames_with_alloc(&decoded_frames, &decoded_count, &decoded_meta,
                           &eff_alloc);

    out_report->status = st;
    out_report->valid = 1;
    return st;
}
