/* Uncompressed 24-bit BMP subset. Pixels cross the boundary as packed top-down RGB. */
#include "rf.h"
#include <tmmintrin.h>

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* Reverse the byte order of each 3-byte pixel: BGR <-> RGB. Four pixels per
   shuffle; each 16-byte load and store stays inside the row because at least
   six pixels remain, and the next step or the tail rewrites bytes 12..15. */
static void swap_rb(uint8_t *dst, const uint8_t *src, uint32_t n) {
    const __m128i shuf = _mm_setr_epi8(2, 1, 0, 5, 4, 3, 8, 7, 6, 11, 10, 9, 12, 13, 14, 15);
    uint32_t i = 0;
    for (; i + 6 <= n; i += 4, src += 12, dst += 12)
        _mm_storeu_si128((__m128i *)dst, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)src), shuf));
    for (; i < n; i++, src += 3, dst += 3) {
        uint8_t b = src[0], g = src[1], r = src[2];
        dst[0] = r; dst[1] = g; dst[2] = b;
    }
}

int rf_bmp_load(const uint8_t *f, size_t size, rf_image *out) {
    out->rgb = 0;
    if (size < 54) return rf_fail(RF_ERR_FORMAT, "invalid BMP: truncated");
    if (rd16(f) != 0x4D42) return rf_fail(RF_ERR_FORMAT, "invalid BMP: signature");
    if (rd32(f + 14) != 40) return rf_fail(RF_ERR_UNSUPPORTED, "unsupported BMP: header");
    int32_t sw = (int32_t)rd32(f + 18);
    if (sw < 1) return rf_fail(RF_ERR_FORMAT, "invalid BMP: dimensions");
    if ((uint32_t)sw > RF_MAX_DIM) return rf_fail(RF_ERR_LIMIT, "image exceeds size limit");
    int32_t sh = (int32_t)rd32(f + 22);
    if (sh == 0 || sh == INT32_MIN) return rf_fail(RF_ERR_FORMAT, "invalid BMP: dimensions");
    int topdown = sh < 0;
    uint32_t w = (uint32_t)sw, h = (uint32_t)(sh < 0 ? -sh : sh);
    if (h > RF_MAX_DIM) return rf_fail(RF_ERR_LIMIT, "image exceeds size limit");
    if (rd16(f + 26) != 1) return rf_fail(RF_ERR_UNSUPPORTED, "unsupported BMP: planes");
    if (rd16(f + 28) != 24) return rf_fail(RF_ERR_UNSUPPORTED, "unsupported BMP: bit count");
    if (rd32(f + 30) != 0) return rf_fail(RF_ERR_UNSUPPORTED, "unsupported BMP: compression");
    if ((uint64_t)w * h > RF_MAX_PIXELS) return rf_fail(RF_ERR_LIMIT, "image exceeds size limit");
    uint64_t off = rd32(f + 10);
    if (off < 54) return rf_fail(RF_ERR_FORMAT, "invalid BMP: truncated");
    uint64_t stride = ((uint64_t)w * 3 + 3) & ~(uint64_t)3;
    uint64_t pixel_end = off + stride * h;
    if (pixel_end > size) return rf_fail(RF_ERR_FORMAT, "invalid BMP: truncated");
    uint64_t bf_size = rd32(f + 2);
    if (bf_size != 0 && (bf_size > size || bf_size < pixel_end)) return rf_fail(RF_ERR_FORMAT, "invalid BMP: file size");
    uint64_t image_size = rd32(f + 34);
    if (image_size != 0 && image_size < stride * h) return rf_fail(RF_ERR_FORMAT, "invalid BMP: image size");
    uint8_t *rgb = rf_alloc((size_t)w * h * 3);
    if (!rgb) return rf_fail(RF_ERR_LIMIT, "out of memory");
    for (uint32_t y = 0; y < h; y++) {
        uint32_t file_row = topdown ? y : h - 1 - y;
        swap_rb(rgb + (size_t)y * w * 3, f + off + stride * file_row, w);
    }
    out->width = w;
    out->height = h;
    out->rgb = rgb;
    return RF_OK;
}

/* Canonical bottom-up 24-bit BMP with zero padding and 3780 pixels per metre. */
int rf_bmp_store(const rf_image *img, uint8_t **file, size_t *size) {
    uint32_t w = img->width, h = img->height;
    uint64_t stride = ((uint64_t)w * 3 + 3) & ~(uint64_t)3;
    uint64_t total = 54 + stride * h;
    if (total > RF_MAX_FILE) return rf_fail(RF_ERR_LIMIT, "image exceeds size limit");
    uint8_t *b = rf_alloc((size_t)total);
    if (!b) return rf_fail(RF_ERR_LIMIT, "out of memory");
    wr16(b, 0x4D42);
    wr32(b + 2, (uint32_t)total);
    wr32(b + 10, 54);
    wr32(b + 14, 40);
    wr32(b + 18, w);
    wr32(b + 22, h);
    wr16(b + 26, 1);
    wr16(b + 28, 24);
    wr32(b + 34, (uint32_t)(stride * h));
    wr32(b + 38, 3780);
    wr32(b + 42, 3780);
    for (uint32_t y = 0; y < h; y++)
        swap_rb(b + 54 + stride * (h - 1 - y), img->rgb + (size_t)y * w * 3, w);
    *file = b;
    *size = (size_t)total;
    return RF_OK;
}
