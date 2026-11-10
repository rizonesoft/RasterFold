/* Binary P6 PPM, maxval 255 only. Pixels cross the boundary as packed
   top-down RGB, the same shape as the BMP and PNG readers. */
#include "rf.h"

static int ppm_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

/* Skip whitespace and #-to-end-of-line comments between header tokens. */
static size_t ppm_skip(const uint8_t *f, size_t size, size_t pos) {
    for (;;) {
        while (pos < size && ppm_ws(f[pos])) pos++;
        if (pos < size && f[pos] == '#') {
            while (pos < size && f[pos] != '\n') pos++;
            continue;
        }
        return pos;
    }
}

/* One checked decimal token: at least one digit, value capped well below
   overflow, terminated by whitespace, a comment, or the end of the file. */
static int ppm_number(const uint8_t *f, size_t size, size_t *pos, uint32_t *out, rf_error *err) {
    size_t p = ppm_skip(f, size, *pos);
    if (p >= size || f[p] < '0' || f[p] > '9')
        return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: header");
    uint64_t v = 0;
    while (p < size && f[p] >= '0' && f[p] <= '9') {
        v = v * 10u + (uint64_t)(f[p] - '0');
        if (v > (1ull << 32)) return rf_fail(err, RF_ERR_LIMIT, "image exceeds size limit");
        p++;
    }
    if (p < size && !ppm_ws(f[p]) && f[p] != '#')
        return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: header");
    *pos = p;
    *out = (uint32_t)v;
    return RF_OK;
}

int rf_ppm_load(const uint8_t *f, size_t size, rf_image *out, rf_error *err) {
    out->rgb = 0;
    if (size < 2) return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: truncated");
    if (f[0] != 'P' || (f[1] != '6' && f[1] != '3'))
        return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: signature");
    if (f[1] == '3') return rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported PPM: P3");
    if (size < 3 || !ppm_ws(f[2])) return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: header");
    size_t pos = 2;
    uint32_t w = 0, h = 0, maxval = 0;
    int rc = ppm_number(f, size, &pos, &w, err);
    if (rc) return rc;
    rc = ppm_number(f, size, &pos, &h, err);
    if (rc) return rc;
    rc = ppm_number(f, size, &pos, &maxval, err);
    if (rc) return rc;
    if (w == 0 || h == 0) return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: dimensions");
    if (w > RF_MAX_DIM || h > RF_MAX_DIM) return rf_fail(err, RF_ERR_LIMIT, "image exceeds size limit");
    if ((uint64_t)w * h > RF_MAX_PIXELS) return rf_fail(err, RF_ERR_LIMIT, "image exceeds size limit");
    if (maxval != 255u) return rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported PPM: maxval");
    /* Exactly one whitespace byte separates maxval from the binary raster;
       comments are not allowed there, so a first raster byte that happens
       to be whitespace or '#' is pixel data, never header. */
    if (pos >= size || !ppm_ws(f[pos])) return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: header");
    pos++;
    uint64_t need = (uint64_t)w * h * 3u;
    if ((uint64_t)(size - pos) < need) return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: truncated");
    if ((uint64_t)(size - pos) > need) return rf_fail(err, RF_ERR_FORMAT, "invalid PPM: trailing data");
    uint8_t *rgb = rf_alloc((size_t)need);
    if (!rgb) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    memcpy(rgb, f + pos, (size_t)need);
    out->width = w;
    out->height = h;
    out->rgb = rgb;
    return RF_OK;
}

static size_t ppm_dec(char *dst, uint32_t v) {
    char rev[10];
    size_t n = 0;
    do { rev[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    for (size_t i = 0; i < n; i++) dst[i] = rev[n - 1 - i];
    return n;
}

int rf_ppm_store(const rf_image *img, uint8_t **file, size_t *size, rf_error *err) {
    char hdr[32];
    size_t hdr_len = 0;
    hdr[hdr_len++] = 'P'; hdr[hdr_len++] = '6'; hdr[hdr_len++] = '\n';
    hdr_len += ppm_dec(hdr + hdr_len, img->width);
    hdr[hdr_len++] = ' ';
    hdr_len += ppm_dec(hdr + hdr_len, img->height);
    hdr[hdr_len++] = '\n';
    hdr[hdr_len++] = '2'; hdr[hdr_len++] = '5'; hdr[hdr_len++] = '5'; hdr[hdr_len++] = '\n';
    size_t raster = (size_t)img->width * img->height * 3u;
    uint8_t *buf = rf_alloc((size_t)hdr_len + raster);
    if (!buf) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    memcpy(buf, hdr, (size_t)hdr_len);
    memcpy(buf + hdr_len, img->rgb, raster);
    *file = buf;
    *size = (size_t)hdr_len + raster;
    return RF_OK;
}
