/* PNG (ISO/IEC 15948) subset: 8-bit RGB and RGBA, all filters, Adam7.
   Pixels cross the boundary as packed top-down RGB, like the BMP subset.
   Transparency (a tRNS chunk, or a non-opaque alpha channel) is refused
   as unsupported until the format carries alpha. */
#include "rf.h"

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t png_crc(const uint8_t *type_and_data, size_t n) {
    /* rf_crc32 is the same CRC-32 (init and final xor included). */
    return rf_crc32(type_and_data, n);
}

static const uint8_t PNG_SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};

/* Adam7 pass geometry: x start, y start, x step, y step. */
static const uint8_t ADAM_X0[7] = {0, 4, 0, 2, 0, 1, 0};
static const uint8_t ADAM_Y0[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t ADAM_DX[7] = {8, 8, 4, 4, 2, 2, 1};
static const uint8_t ADAM_DY[7] = {8, 8, 8, 4, 4, 2, 2};

int rf_png_load(const uint8_t *f, size_t size, rf_image *out, rf_error *err) {
    out->rgb = 0;
    if (size < 8) return rf_fail(err, RF_ERR_FORMAT, "invalid PNG: truncated");
    for (int i = 0; i < 8; i++)
        if (f[i] != PNG_SIG[i]) return rf_fail(err, RF_ERR_FORMAT, "invalid PNG: signature");
    uint32_t w = 0, h = 0, channels = 0;
    int have_ihdr = 0, have_iend = 0, interlace = 0;
    int seen_idat = 0, idat_ended = 0, seen_plte = 0;
    uint8_t *idat = 0;
    size_t idat_size = 0, idat_cap = 0;
    size_t pos = 8;
    int rc = RF_OK;
    while (pos < size) {
        if (size - pos < 12) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: truncated"); goto done; }
        uint32_t length = rd_be32(f + pos);
        const uint8_t *type = f + pos + 4;
        if ((uint64_t)length + 12 > (uint64_t)(size - pos)) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: truncated"); goto done; }
        uint32_t stored_crc = rd_be32(f + pos + 8 + length);
        if (png_crc(type, 4 + (size_t)length) != stored_crc) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: chunk CRC"); goto done; }
        const uint8_t *data = f + pos + 8;
        int is_idat = type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T';
        if (seen_idat && !is_idat) idat_ended = 1;
        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            if (pos != 8 || length != 13) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: IHDR"); goto done; }
            w = rd_be32(data);
            h = rd_be32(data + 4);
            if (w == 0 || h == 0) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: dimensions"); goto done; }
            if (w > RF_MAX_DIM || h > RF_MAX_DIM || (uint64_t)w * h > RF_MAX_PIXELS) {
                rc = rf_fail(err, RF_ERR_LIMIT, "image exceeds size limit"); goto done;
            }
            /* Validate every IHDR field and the depth/colour
               combination before classifying the format: an illegal
               value or combination is malformed (exit 4), and only a
               legal PNG outside the supported subset is unsupported
               (exit 3). */
            if (data[8] != 1 && data[8] != 2 && data[8] != 4 && data[8] != 8 && data[8] != 16) {
                rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: bit depth"); goto done;
            }
            if (data[9] != 0 && data[9] != 2 && data[9] != 3 && data[9] != 4 && data[9] != 6) {
                rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: colour type"); goto done;
            }
            {
                int combo;
                switch (data[9]) {
                    case 0: combo = 1; break;
                    case 2: combo = data[8] == 8 || data[8] == 16; break;
                    case 3: combo = data[8] != 16; break;
                    default: combo = data[8] == 8 || data[8] == 16; break;
                }
                if (!combo) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: depth and colour"); goto done; }
            }
            if (data[10] != 0 || data[11] != 0) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: IHDR methods"); goto done; }
            if (data[12] > 1) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: interlace"); goto done; }
            if (data[8] != 8) { rc = rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported PNG: bit depth"); goto done; }
            if (data[9] == 2) channels = 3;
            else if (data[9] == 6) channels = 4;
            else { rc = rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported PNG: colour type"); goto done; }
            interlace = data[12];
            have_ihdr = 1;
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S') {
            rc = rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported PNG: transparency");
            goto done;
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (!have_ihdr) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: IDAT before IHDR"); goto done; }
            if (idat_ended) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: IDAT sequence"); goto done; }
            seen_idat = 1;
            if (idat_size + length > idat_cap) {
                size_t ncap = (idat_size + length) * 2 + 64;
                uint8_t *nbuf = rf_alloc(ncap);
                if (!nbuf) { rc = rf_fail(err, RF_ERR_LIMIT, "out of memory"); goto done; }
                for (size_t i = 0; i < idat_size; i++) nbuf[i] = idat[i];
                rf_free(idat);
                idat = nbuf;
                idat_cap = ncap;
            }
            for (uint32_t i = 0; i < length; i++) idat[idat_size + i] = data[i];
            idat_size += length;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            if (length != 0) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: IEND"); goto done; }
            if ((uint64_t)pos + 12 + length != (uint64_t)size) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: trailing data"); goto done; }
            have_iend = 1;
            pos += 12 + length;
            break;
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            /* A palette must precede IDAT, appear at most once, and
               hold whole RGB entries; a suggested palette on an RGB
               image is otherwise ignored. */
            if (!have_ihdr || seen_plte || seen_idat || length == 0 || length % 3 != 0 || length > 768) {
                rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: PLTE"); goto done;
            }
            seen_plte = 1;
        } else if (type[0] >= 'A' && type[0] <= 'Z' &&
                   !(type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E')) {
            /* An unknown critical chunk can change how the pixels must
               be read; only unknown ancillary chunks may be skipped. */
            rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: unknown critical chunk");
            goto done;
        }
        pos += 12 + length;
    }
    if (!have_ihdr || !have_iend) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: structure"); goto done; }
    if (idat_size == 0) { rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: no image data"); goto done; }
    {
        uint32_t passes = interlace ? 7 : 1;
        /* Compute the exact inflated size across passes; a pass with
           no pixels in either dimension contributes no filter bytes. */
        size_t raw_size = 0;
        for (uint32_t p = 0; p < passes; p++) {
            uint32_t pw = interlace ? (w > ADAM_X0[p] ? (w - ADAM_X0[p] + ADAM_DX[p] - 1) / ADAM_DX[p] : 0) : w;
            uint32_t ph = interlace ? (h > ADAM_Y0[p] ? (h - ADAM_Y0[p] + ADAM_DY[p] - 1) / ADAM_DY[p] : 0) : h;
            if (pw && ph) raw_size += (size_t)ph * ((size_t)pw * channels + 1);
        }
        uint8_t *raw = rf_alloc(raw_size ? raw_size : 1);
        uint8_t *rgb = rf_alloc((size_t)w * h * 3);
        if (!raw || !rgb) {
            rf_free(raw); rf_free(rgb);
            rc = rf_fail(err, RF_ERR_LIMIT, "out of memory");
            goto done;
        }
        rc = rf_zlib_inflate(idat, idat_size, raw, raw_size, err);
        if (rc) { rf_free(raw); rf_free(rgb); goto done; }
        size_t rp = 0;
        for (uint32_t p = 0; p < passes; p++) {
            uint32_t x0 = interlace ? ADAM_X0[p] : 0, y0 = interlace ? ADAM_Y0[p] : 0;
            uint32_t dx = interlace ? ADAM_DX[p] : 1, dy = interlace ? ADAM_DY[p] : 1;
            uint32_t pw = (w > x0) ? (w - x0 + dx - 1) / dx : 0;
            uint32_t ph = (h > y0) ? (h - y0 + dy - 1) / dy : 0;
            if (!pw || !ph) continue;
            size_t row_bytes = (size_t)pw * channels;
            const uint8_t *prev = 0;
            for (uint32_t y = 0; y < ph; y++) {
                uint32_t filter = raw[rp++];
                uint8_t *cur = raw + rp;
                if (filter > 4) { rf_free(raw); rf_free(rgb); rc = rf_fail(err, RF_ERR_FORMAT, "invalid PNG: filter"); goto done; }
                for (size_t x = 0; x < row_bytes; x++) {
                    uint32_t a = x >= channels ? cur[x - channels] : 0;
                    uint32_t b = prev ? prev[x] : 0;
                    uint32_t c = (prev && x >= channels) ? prev[x - channels] : 0;
                    uint32_t v = cur[x];
                    switch (filter) {
                        case 0: break;
                        case 1: v += a; break;
                        case 2: v += b; break;
                        case 3: v += (a + b) / 2; break;
                        default: {
                            int32_t pp = (int32_t)a + (int32_t)b - (int32_t)c;
                            int32_t pa = pp < (int32_t)a ? (int32_t)a - pp : pp - (int32_t)a;
                            int32_t pb = pp < (int32_t)b ? (int32_t)b - pp : pp - (int32_t)b;
                            int32_t pc = pp < (int32_t)c ? (int32_t)c - pp : pp - (int32_t)c;
                            v += (uint32_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
                            break;
                        }
                    }
                    cur[x] = (uint8_t)v;
                }
                for (uint32_t x = 0; x < pw; x++) {
                    uint32_t gx = x0 + x * dx, gy = y0 + y * dy;
                    const uint8_t *px = cur + (size_t)x * channels;
                    uint8_t *dst = rgb + ((size_t)gy * w + gx) * 3;
                    dst[0] = px[0]; dst[1] = px[1]; dst[2] = px[2];
                    if (channels == 4 && px[3] != 255) {
                        rf_free(raw); rf_free(rgb);
                        rc = rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported PNG: alpha");
                        goto done;
                    }
                }
                prev = cur;
                rp += row_bytes;
            }
        }
        rf_free(raw);
        out->width = w;
        out->height = h;
        out->rgb = rgb;
    }
done:
    rf_free(idat);
    if (rc) { out->rgb = 0; }
    return rc;
}

static void put_chunk(uint8_t *out, size_t *pos, const char *type, const uint8_t *data, size_t n) {
    wr_be32(out + *pos, (uint32_t)n);
    for (int i = 0; i < 4; i++) out[*pos + 4 + i] = (uint8_t)type[i];
    for (size_t i = 0; i < n; i++) out[*pos + 8 + i] = data[i];
    wr_be32(out + *pos + 8 + n, png_crc(out + *pos + 4, 4 + n));
    *pos += 12 + n;
}

int rf_png_store(const rf_image *img, uint8_t **file, size_t *size, rf_error *err) {
    *file = 0;
    *size = 0;
    uint32_t w = img->width, h = img->height;
    size_t raw_size = (size_t)h * ((size_t)w * 3 + 1);
    uint8_t *raw = rf_alloc(raw_size);
    if (!raw) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
    /* Per-row filter choice by the sum of absolute signed filtered bytes. */
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = img->rgb + (size_t)y * w * 3;
        const uint8_t *prev = y ? img->rgb + (size_t)(y - 1) * w * 3 : 0;
        size_t row_bytes = (size_t)w * 3;
        int best_filter = 0;
        uint64_t best_score = UINT64_MAX;
        uint8_t *dst = raw + (size_t)y * (row_bytes + 1);
        for (int filter = 0; filter < 5; filter++) {
            uint64_t score = 0;
            for (size_t x = 0; x < row_bytes; x++) {
                uint32_t a = x >= 3 ? row[x - 3] : 0;
                uint32_t b = prev ? prev[x] : 0;
                uint32_t c = (prev && x >= 3) ? prev[x - 3] : 0;
                uint32_t v = row[x];
                uint32_t fv;
                switch (filter) {
                    case 0: fv = v; break;
                    case 1: fv = v - a; break;
                    case 2: fv = v - b; break;
                    case 3: fv = v - (a + b) / 2; break;
                    default: {
                        int32_t pp = (int32_t)a + (int32_t)b - (int32_t)c;
                        int32_t pa = pp < (int32_t)a ? (int32_t)a - pp : pp - (int32_t)a;
                        int32_t pb = pp < (int32_t)b ? (int32_t)b - pp : pp - (int32_t)b;
                        int32_t pc = pp < (int32_t)c ? (int32_t)c - pp : pp - (int32_t)c;
                        fv = v - (uint32_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
                        break;
                    }
                }
                int8_t sv = (int8_t)(uint8_t)fv;
                score += (uint64_t)(sv < 0 ? -(int)sv : (int)sv);
            }
            if (score < best_score) { best_score = score; best_filter = filter; }
        }
        dst[0] = (uint8_t)best_filter;
        for (size_t x = 0; x < row_bytes; x++) {
            uint32_t a = x >= 3 ? row[x - 3] : 0;
            uint32_t b = prev ? prev[x] : 0;
            uint32_t c = (prev && x >= 3) ? prev[x - 3] : 0;
            uint32_t v = row[x];
            uint32_t fv;
            switch (best_filter) {
                case 0: fv = v; break;
                case 1: fv = v - a; break;
                case 2: fv = v - b; break;
                case 3: fv = v - (a + b) / 2; break;
                default: {
                    int32_t pp = (int32_t)a + (int32_t)b - (int32_t)c;
                    int32_t pa = pp < (int32_t)a ? (int32_t)a - pp : pp - (int32_t)a;
                    int32_t pb = pp < (int32_t)b ? (int32_t)b - pp : pp - (int32_t)b;
                    int32_t pc = pp < (int32_t)c ? (int32_t)c - pp : pp - (int32_t)c;
                    fv = v - (uint32_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
                    break;
                }
            }
            dst[1 + x] = (uint8_t)fv;
        }
    }
    uint8_t *zdata = 0;
    size_t zsize = 0;
    int rc = rf_zlib_deflate(raw, raw_size, &zdata, &zsize, err);
    rf_free(raw);
    if (rc) return rc;
    size_t total = 8 + (12 + 13) + (12 + zsize) + 12;
    uint8_t *out = rf_alloc(total);
    if (!out) { rf_free(zdata); return rf_fail(err, RF_ERR_LIMIT, "out of memory"); }
    for (int i = 0; i < 8; i++) out[i] = PNG_SIG[i];
    size_t pos = 8;
    uint8_t ihdr[13];
    wr_be32(ihdr, w);
    wr_be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    put_chunk(out, &pos, "IHDR", ihdr, 13);
    put_chunk(out, &pos, "IDAT", zdata, zsize);
    rf_free(zdata);
    put_chunk(out, &pos, "IEND", ihdr, 0);
    *file = out;
    *size = pos;
    return RF_OK;
}
