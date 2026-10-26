/* The .fold container, version 2. spec/FOLD.md defines every field. */
#include "rf.h"

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

static int check_dims(uint32_t w, uint32_t h, const char *bad, rf_error *err) {
    if (w == 0 || h == 0) return rf_fail(err, RF_ERR_FORMAT, bad);
    if (w > RF_MAX_DIM || h > RF_MAX_DIM || (uint64_t)w * h > RF_MAX_PIXELS)
        return rf_fail(err, RF_ERR_LIMIT, "image exceeds size limit");
    return RF_OK;
}

/* The smallest of raw (0), CM (1), and palette (3, <= 256 colours) is written; ties: 0, then 1, then 3. */
int rf_encode(const rf_image *img, uint8_t **file, size_t *size, int threads, rf_error *err) {
    *file = 0;
    *size = 0;
    int rc = check_dims(img->width, img->height, "invalid image: dimensions", err);
    if (rc) return rc;
    size_t raw = (size_t)img->width * img->height * 3;
    uint8_t *f = rf_alloc(RF_HEADER_SIZE + raw + 256);
    if (!f) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    size_t n = 0;
    rc = rf_cm_encode(img->rgb, img->width, img->height, f + RF_HEADER_SIZE, raw - 1, &n, threads, err);
    if (rc) { rf_free(f); return rc; }
    uint32_t method = RF_METHOD_CM;
    if (n == 0) {
        memcpy(f + RF_HEADER_SIZE, img->rgb, raw);
        n = raw;
        method = RF_METHOD_RAW;
    }
    if (n > 0) {
        size_t pal_n = 0;
        rc = rf_pal_encode(img->rgb, img->width, img->height, f + RF_HEADER_SIZE, n - 1, &pal_n, err);
        if (rc) { rf_free(f); return rc; }
        if (pal_n > 0) {
            n = pal_n;
            method = RF_METHOD_PAL2;
        }
    }
    f[0] = 'R'; f[1] = 'F'; f[2] = 'I'; f[3] = (uint8_t)RF_VERSION;
    wr16(f + 4, RF_VERSION);
    wr16(f + 6, RF_HEADER_SIZE);
    wr32(f + 8, img->width);
    wr32(f + 12, img->height);
    f[16] = (uint8_t)method;
    f[17] = 0;
    wr16(f + 18, RF_FORMAT_RGB8);
    wr32(f + 20, (uint32_t)n);
    wr32(f + 24, rf_crc32(f + RF_HEADER_SIZE, n));
    wr32(f + 28, rf_crc32(f, 28));
    *file = f;
    *size = RF_HEADER_SIZE + n;
    return RF_OK;
}

static int parse_fold(const uint8_t *f, size_t size, rf_info *info, const uint8_t **payload_out, rf_error *err) {
    if (size < RF_HEADER_SIZE) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: truncated");
    if (f[0] != 'R' || f[1] != 'F' || f[2] != 'I') return rf_fail(err, RF_ERR_FORMAT, "invalid fold: magic");
    if (f[3] != RF_VERSION || rd16(f + 4) != RF_VERSION) return rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported fold: version");
    if (rd16(f + 6) != RF_HEADER_SIZE) return rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported fold: header size");
    if (rd16(f + 18) != RF_FORMAT_RGB8) return rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported fold: pixel format");
    if (rf_crc32(f, 28) != rd32(f + 28)) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: header checksum");
    uint32_t method = f[16];
    if (method > RF_METHOD_PAL2) return rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported fold: method");
    if (f[17] != 0) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: reserved byte");
    uint32_t w = rd32(f + 8), h = rd32(f + 12);
    int rc = check_dims(w, h, "invalid fold: dimensions", err);
    if (rc) return rc;
    uint32_t n = rd32(f + 20);
    if ((uint64_t)n != (uint64_t)size - RF_HEADER_SIZE) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: payload length");
    const uint8_t *payload = f + RF_HEADER_SIZE;
    if (rf_crc32(payload, n) != rd32(f + 24)) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: payload checksum");
    size_t raw = (size_t)w * h * 3;
    if (method == RF_METHOD_RAW && n != raw) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: raw length");
    info->width = w; info->height = h; info->method = method;
    info->payload_size = n; info->payload_crc = rd32(f + 24); info->header_crc = rd32(f + 28);
    for (int c = 0; c < 3; c++) info->stream_size[c] = (method == RF_METHOD_CM && n >= 12) ? rd32(payload + 4 * c) : 0;
    *payload_out = payload;
    return RF_OK;
}

int rf_decode(const uint8_t *f, size_t size, rf_image *out, rf_info *info, int threads, rf_error *err) {
    out->rgb = 0;
    rf_info hdr = {0};
    const uint8_t *payload = 0;
    int rc = parse_fold(f, size, &hdr, &payload, err);
    if (rc || !payload) return rc;
    if (info) *info = hdr;
    size_t raw = (size_t)hdr.width * hdr.height * 3;
    uint8_t *rgb = rf_alloc(raw);
    if (!rgb) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    if (hdr.method == RF_METHOD_RAW) memcpy(rgb, payload, raw);
    else if (hdr.method == RF_METHOD_PAL || hdr.method == RF_METHOD_PAL2) {
        if ((rc = rf_pal_decode(payload, hdr.payload_size, rgb, 0, hdr.width, hdr.height, hdr.method, err)) != RF_OK) { rf_free(rgb); return rc; }
    } else if ((rc = rf_cm_decode(payload, hdr.payload_size, rgb, 0, hdr.width, hdr.height, threads, err)) != RF_OK) { rf_free(rgb); return rc; }
    out->width = hdr.width;
    out->height = hdr.height;
    out->rgb = rgb;
    return RF_OK;
}

int rf_decode_bmp(const uint8_t *f, size_t size, uint8_t **bmp, size_t *bmp_size, int threads, rf_error *err) {
    *bmp = 0;
    *bmp_size = 0;
    rf_info hdr = {0};
    const uint8_t *payload = 0;
    int rc = parse_fold(f, size, &hdr, &payload, err);
    if (rc || !payload) return rc;
    size_t stride = 0;
    rc = rf_bmp_alloc(hdr.width, hdr.height, bmp, bmp_size, &stride, err);
    if (rc || !*bmp) return rc;
    if (hdr.method == RF_METHOD_RAW) rf_bmp_write_raw(*bmp, stride, payload, hdr.width, hdr.height);
    else if (hdr.method == RF_METHOD_PAL || hdr.method == RF_METHOD_PAL2) {
        if ((rc = rf_pal_decode(payload, hdr.payload_size, *bmp, stride, hdr.width, hdr.height, hdr.method, err)) != RF_OK) {
            rf_free(*bmp);
            *bmp = 0;
            *bmp_size = 0;
            return rc;
        }
    } else if ((rc = rf_cm_decode(payload, hdr.payload_size, *bmp, stride, hdr.width, hdr.height, threads, err)) != RF_OK) {
        rf_free(*bmp);
        *bmp = 0;
        *bmp_size = 0;
        return rc;
    }
    return RF_OK;
}
