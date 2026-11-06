#include "rf.h"

uint32_t rf_version(void) { return RF_API_VERSION; }

static void clear_error(rf_error *err) {
    if (err) { err->code = RF_OK; err->message = 0; }
}

static int options_threads(const rf_options *options, int *threads, rf_error *err) {
    uint32_t n = 0;
    if (options) {
        if (options->size < 8) return rf_fail(err, RF_ERR_USAGE, "options: size too small");
        if (options->size > sizeof *options) return rf_fail(err, RF_ERR_UNSUPPORTED, "options: size not supported");
        if (options->size % 4) return rf_fail(err, RF_ERR_USAGE, "options: invalid size alignment");
        n = options->threads;
        if (n > 9) return rf_fail(err, RF_ERR_USAGE, "options: threads must be 0..9");
        size_t words = (options->size < sizeof *options ? options->size : sizeof *options) / 4;
        for (size_t i = 2; i < words; i++)
            if (options->reserved[i - 2]) return rf_fail(err, RF_ERR_UNSUPPORTED, "options: reserved field");
    }
    int ncpu = rf_cpu_count();
    int def_t = ncpu < 1 ? 1 : (ncpu > 9 ? 9 : ncpu);
    *threads = n ? (int)n : def_t;
    return RF_OK;
}

int rf_encode_image(const rf_pixels *pixels, const rf_options *options, rf_buffer *out, rf_error *err) {
    clear_error(err);
    if (!out) return rf_fail(err, RF_ERR_USAGE, "output is required");
    out->data = 0; out->size = 0;
    if (!pixels || !pixels->data) return rf_fail(err, RF_ERR_USAGE, "pixels are required");
    if (pixels->layout != RF_LAYOUT_RGB8) return rf_fail(err, RF_ERR_UNSUPPORTED, "unsupported pixel layout");
    uint32_t w = pixels->width, h = pixels->height;
    if (!w || !h) return rf_fail(err, RF_ERR_FORMAT, "invalid image: dimensions");
    if (w > RF_MAX_DIM || h > RF_MAX_DIM || (uint64_t)w * h > RF_MAX_PIXELS)
        return rf_fail(err, RF_ERR_LIMIT, "image exceeds size limit");
    size_t row = (size_t)w * 3;
    if (pixels->stride < row || (h > 1 && pixels->stride > (SIZE_MAX - row) / (h - 1)) ||
        pixels->size < (h - 1) * pixels->stride + row)
        return rf_fail(err, RF_ERR_USAGE, "invalid pixel stride or buffer size");
    int threads;
    int rc = options_threads(options, &threads, err);
    if (rc) return rc;
    rf_image img = {w, h, pixels->data};
    uint8_t *packed = 0;
    if (pixels->stride != row) {
        packed = rf_alloc(row * h);
        if (!packed) return rf_fail(err, RF_ERR_LIMIT, "out of memory");
        for (uint32_t y = 0; y < h; y++) memcpy(packed + y * row, pixels->data + y * pixels->stride, row);
        img.rgb = packed;
    }
    rc = rf_encode(&img, &out->data, &out->size, threads, err);
    rf_free(packed);
    return rc;
}

int rf_decode_image(const void *bytes, size_t size, const rf_options *options, rf_pixels *out, rf_error *err) {
    clear_error(err);
    if (!out) return rf_fail(err, RF_ERR_USAGE, "output is required");
    memset(out, 0, sizeof *out);
    if (!bytes) return rf_fail(err, RF_ERR_USAGE, "input is required");
    if (size > RF_MAX_FILE) return rf_fail(err, RF_ERR_LIMIT, "input exceeds size limit");
    int threads;
    int rc = options_threads(options, &threads, err);
    if (rc) return rc;
    rf_image img = {0, 0, 0};
    rc = rf_decode(bytes, size, &img, 0, threads, err);
    if (rc) return rc;
    out->width = img.width; out->height = img.height;
    out->stride = (size_t)img.width * 3; out->layout = RF_LAYOUT_RGB8;
    out->data = img.rgb; out->size = out->stride * img.height;
    return RF_OK;
}

int rf_inspect(const void *bytes, size_t size, rf_info *info, rf_error *err) {
    clear_error(err);
    if (!info) return rf_fail(err, RF_ERR_USAGE, "info is required");
    memset(info, 0, sizeof *info);
    if (!bytes) return rf_fail(err, RF_ERR_USAGE, "input is required");
    if (size > RF_MAX_FILE) return rf_fail(err, RF_ERR_LIMIT, "input exceeds size limit");
    rf_image img = {0, 0, 0};
    int rc = rf_decode(bytes, size, &img, info, 1, err);
    rf_free(img.rgb);
    if (rc) memset(info, 0, sizeof *info);
    return rc;
}
