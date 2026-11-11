#include "rfc_codec.h"

#include <stdlib.h>
#include <string.h>

size_t rfc_sample_bytes(rfc_sample_format format) {
    switch (format) {
        case RFC_SAMPLE_GRAY8: return 1;
        case RFC_SAMPLE_GRAY16: return 2;
        case RFC_SAMPLE_RGB8: return 3;
        case RFC_SAMPLE_RGBA8: return 4;
        case RFC_SAMPLE_RGB16: case RFC_SAMPLE_RGB_HALF: return 6;
        case RFC_SAMPLE_RGBA16: case RFC_SAMPLE_RGBA_HALF: return 8;
        case RFC_SAMPLE_RGB_FLOAT: return 12;
        case RFC_SAMPLE_RGBA_FLOAT: return 16;
        default: return 0;
    }
}

static uint32_t channel_bits(rfc_sample_format f) {
    switch (f) {
        case RFC_SAMPLE_GRAY8: case RFC_SAMPLE_RGB8: case RFC_SAMPLE_RGBA8: return 8;
        case RFC_SAMPLE_GRAY16: case RFC_SAMPLE_RGB16: case RFC_SAMPLE_RGBA16:
        case RFC_SAMPLE_RGB_HALF: case RFC_SAMPLE_RGBA_HALF: return 16;
        case RFC_SAMPLE_RGB_FLOAT: case RFC_SAMPLE_RGBA_FLOAT: return 32;
        default: return 0;
    }
}
static int has_alpha(rfc_sample_format f) {
    return f == RFC_SAMPLE_RGBA8 || f == RFC_SAMPLE_RGBA16 ||
           f == RFC_SAMPLE_RGBA_HALF || f == RFC_SAMPLE_RGBA_FLOAT;
}

rfc_status rfc_layout_validate(const rfc_layout *layout, size_t *total_out) {
    if (!layout) return RFC_ERR_INVALID_ARGUMENT;
    size_t bpp = rfc_sample_bytes(layout->format);
    if (bpp == 0) return RFC_ERR_UNSUPPORTED_SAMPLE;
    if (layout->width == 0 || layout->height == 0 ||
        layout->width > RFC_MAX_DIMENSION || layout->height > RFC_MAX_DIMENSION)
        return RFC_ERR_INVALID_ARGUMENT;
    if (layout->bits_per_sample == 0 || layout->bits_per_sample > channel_bits(layout->format))
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    if ((layout->format == RFC_SAMPLE_RGB_HALF || layout->format == RFC_SAMPLE_RGBA_HALF) &&
        layout->bits_per_sample != 16)
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    if ((layout->format == RFC_SAMPLE_RGB_FLOAT || layout->format == RFC_SAMPLE_RGBA_FLOAT) &&
        layout->bits_per_sample != 32)
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    if (layout->alpha < RFC_ALPHA_NONE || layout->alpha > RFC_ALPHA_PREMULTIPLIED)
        return RFC_ERR_UNSUPPORTED_ALPHA;
    if (has_alpha(layout->format) && layout->alpha == RFC_ALPHA_NONE)
        return RFC_ERR_UNSUPPORTED_ALPHA;
    if (!has_alpha(layout->format) && layout->alpha != RFC_ALPHA_NONE &&
        layout->alpha != RFC_ALPHA_OPAQUE)
        return RFC_ERR_UNSUPPORTED_ALPHA;
    if ((size_t)layout->width > SIZE_MAX / bpp) return RFC_ERR_OVERFLOW;
    size_t row = (size_t)layout->width * bpp;
    if (layout->stride_bytes < row) return RFC_ERR_INVALID_ARGUMENT;
    if (layout->stride_bytes != 0 && (size_t)layout->height > SIZE_MAX / layout->stride_bytes)
        return RFC_ERR_OVERFLOW;
    if (total_out) *total_out = layout->stride_bytes * (size_t)layout->height;
    return RFC_OK;
}

rfc_status rfc_frame_alloc(rfc_frame *frame, const rfc_layout *layout,
                           const rfc_metadata *metadata,
                           const rfc_allocator *allocator) {
    if (!frame || !allocator || !allocator->alloc || !allocator->free)
        return RFC_ERR_INVALID_ARGUMENT;
    size_t total = 0;
    rfc_status st = rfc_layout_validate(layout, &total);
    if (st != RFC_OK) return st; /* refusal happens before any allocation */
    if (metadata && metadata->icc_len > RFC_MAX_ICC_BYTES)
        return RFC_ERR_INVALID_ARGUMENT;
    if (metadata && metadata->icc_len && !metadata->icc)
        return RFC_ERR_INVALID_ARGUMENT;
    if (metadata && metadata->orientation > RFC_ORIENTATION_ROTATE_270)
        return RFC_ERR_INVALID_ARGUMENT;
    uint8_t *data = (uint8_t *)allocator->alloc(allocator->ctx, total);
    if (!data) return RFC_ERR_OUT_OF_MEMORY;
    uint8_t *icc = NULL;
    if (metadata && metadata->icc_len) {
        icc = (uint8_t *)allocator->alloc(allocator->ctx, metadata->icc_len);
        if (!icc) { allocator->free(allocator->ctx, data); return RFC_ERR_OUT_OF_MEMORY; }
        memcpy(icc, metadata->icc, metadata->icc_len);
    }
    memset(frame, 0, sizeof(*frame));
    frame->layout = *layout;
    frame->data = data;
    frame->data_len = total;
    frame->allocator = allocator;
    if (metadata) frame->metadata = *metadata;
    frame->metadata.icc = icc;
    return RFC_OK;
}

void rfc_frame_free(rfc_frame *frame) {
    if (!frame || !frame->allocator) return;
    if (frame->metadata.icc)
        frame->allocator->free(frame->allocator->ctx, (void *)frame->metadata.icc);
    if (frame->data) frame->allocator->free(frame->allocator->ctx, frame->data);
    memset(frame, 0, sizeof(*frame));
}

void rfc_encoded_free(rfc_encoded *encoded) {
    if (!encoded) return;
    if (encoded->data && encoded->allocator)
        encoded->allocator->free(encoded->allocator->ctx, encoded->data);
    encoded->data = NULL; encoded->len = 0; encoded->allocator = NULL;
}

const char *rfc_sample_name(rfc_sample_format f) {
    switch (f) {
        case RFC_SAMPLE_GRAY8: return "gray8";
        case RFC_SAMPLE_GRAY16: return "gray16";
        case RFC_SAMPLE_RGB8: return "rgb8";
        case RFC_SAMPLE_RGBA8: return "rgba8";
        case RFC_SAMPLE_RGB16: return "rgb16";
        case RFC_SAMPLE_RGBA16: return "rgba16";
        case RFC_SAMPLE_RGB_HALF: return "rgb-half";
        case RFC_SAMPLE_RGBA_HALF: return "rgba-half";
        case RFC_SAMPLE_RGB_FLOAT: return "rgb-float";
        case RFC_SAMPLE_RGBA_FLOAT: return "rgba-float";
        default: return "unknown";
    }
}

/* Setting definitions shared by formats. */
static const char *const k_metadata_values[] = {"preserve", "strip", NULL};
static const char *const k_exactness_values[] = {"pixel-exact", "byte-identical", NULL};
static const char *const k_exactness_jpeg[] = {"pixel-exact", "byte-identical", "coefficient-exact", NULL};
static const char *const k_exactness_jxl[] = {"pixel-exact", "byte-identical", "coefficient-exact", "original-byte-reconstruction", NULL};
static const char *const k_frames_values[] = {"all", "first-only", NULL};
static const rfc_setting_def k_common_settings[] = {
    {"metadata", "enum", "preserve", k_metadata_values, 2, 0, 0,
     "metadata=strip is explicit; metadata is never dropped silently"},
    {"exactness", "enum", "pixel-exact", k_exactness_values, 2, 0, 0,
     "exactness supports pixel-exact and byte-identical only: coefficient-exact needs JPEG DCT coefficients and original-byte-reconstruction needs JPEG XL reconstruction data, neither of which this format carries"},
};
static const rfc_setting_def k_anim_settings[] = {
    {"metadata", "enum", "preserve", k_metadata_values, 2, 0, 0,
     "metadata=strip is explicit; metadata is never dropped silently"},
    {"exactness", "enum", "pixel-exact", k_exactness_values, 2, 0, 0,
     "exactness supports pixel-exact and byte-identical only: coefficient-exact needs JPEG DCT coefficients and original-byte-reconstruction needs JPEG XL reconstruction data, neither of which this format carries"},
    {"frames", "enum", "all", k_frames_values, 2, 0, 0,
     "frames=first-only discards frames and is refused for exact conversion"},
};
static const rfc_setting_def k_jpeg_settings[] = {
    {"metadata", "enum", "preserve", k_metadata_values, 2, 0, 0,
     "metadata=strip is explicit; metadata is never dropped silently"},
    {"exactness", "enum", "pixel-exact", k_exactness_jpeg, 3, 0, 0,
     "exactness supports pixel-exact, byte-identical, and coefficient-exact only: original-byte-reconstruction needs JPEG XL reconstruction data, which JPEG does not carry"},
};
static const rfc_setting_def k_jxl_settings[] = {
    {"metadata", "enum", "preserve", k_metadata_values, 2, 0, 0,
     "metadata=strip is explicit; metadata is never dropped silently"},
    {"exactness", "enum", "pixel-exact", k_exactness_jxl, 4, 0, 0,
     "exactness supports pixel-exact, byte-identical, coefficient-exact, and original-byte-reconstruction; any other label is not an exactness class"},
    {"frames", "enum", "all", k_frames_values, 2, 0, 0,
     "frames=first-only discards frames and is refused for exact conversion"},
};
#define COMMON k_common_settings, 2
#define JPEGSET k_jpeg_settings, 2
#define JXLSET k_jxl_settings, 3
#define ANIM k_anim_settings, 3
static const rfc_combination_rule k_common_rules[] = {
    {"metadata", "strip", "exactness", "byte-identical",
     "stripping metadata changes the bytes, so it cannot claim byte-identical exactness"},
};
static const rfc_combination_rule k_anim_rules[] = {
    {"metadata", "strip", "exactness", "byte-identical",
     "stripping metadata changes the bytes, so it cannot claim byte-identical exactness"},
    {"frames", "first-only", NULL, NULL,
     "frames=first-only discards frames and is refused for exact conversion"},
};
#define COMMON_RULES k_common_rules, 1
static const rfc_combination_rule k_jxl_rules[] = {
    {"metadata", "strip", "exactness", "byte-identical",
     "stripping metadata changes the bytes, so it cannot claim byte-identical exactness"},
    {"metadata", "strip", "exactness", "original-byte-reconstruction",
     "stripping metadata changes the reconstructed bytes, so it cannot claim original-byte-reconstruction"},
    {"frames", "first-only", NULL, NULL,
     "frames=first-only discards frames and is refused for exact conversion"},
};
#define JXL_RULES k_jxl_rules, 3
#define ANIM_RULES k_anim_rules, 2

static const rfc_sample_format k_rgb8[] = {RFC_SAMPLE_RGB8};
static const rfc_sample_format k_bmp[] = {RFC_SAMPLE_GRAY8, RFC_SAMPLE_RGB8, RFC_SAMPLE_RGBA8};
static const rfc_sample_format k_png[] = {RFC_SAMPLE_GRAY8, RFC_SAMPLE_GRAY16, RFC_SAMPLE_RGB8,
                                          RFC_SAMPLE_RGBA8, RFC_SAMPLE_RGB16, RFC_SAMPLE_RGBA16};
static const rfc_sample_format k_pnm[] = {RFC_SAMPLE_GRAY8, RFC_SAMPLE_GRAY16,
                                          RFC_SAMPLE_RGB8, RFC_SAMPLE_RGB16};
static const rfc_sample_format k_pam[] = {RFC_SAMPLE_GRAY8, RFC_SAMPLE_GRAY16, RFC_SAMPLE_RGB8,
                                          RFC_SAMPLE_RGBA8, RFC_SAMPLE_RGB16, RFC_SAMPLE_RGBA16};
static const rfc_sample_format k_rgba8[] = {RFC_SAMPLE_RGB8, RFC_SAMPLE_RGBA8};
static const rfc_sample_format k_jpeg[] = {RFC_SAMPLE_GRAY8, RFC_SAMPLE_RGB8};
static const rfc_sample_format k_10bit[] = {RFC_SAMPLE_RGB8, RFC_SAMPLE_RGBA8,
                                            RFC_SAMPLE_RGB16, RFC_SAMPLE_RGBA16};
static const rfc_sample_format k_exr[] = {RFC_SAMPLE_RGB_HALF, RFC_SAMPLE_RGBA_HALF,
                                          RFC_SAMPLE_RGB_FLOAT, RFC_SAMPLE_RGBA_FLOAT};
#define PLANNED RFC_AVAILABILITY_PLANNED

static const rfc_capability k_registry[] = {
    {RFC_FORMAT_RASTERFOLD, "rasterfold", "RasterFold (this repository)", "D10 T02 \xc2\xa7" "1", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_rgb8, 1, COMMON, COMMON_RULES},
    {RFC_FORMAT_BMP, "bmp", "in-tree converter code", "D10 T02 \xc2\xa7" "1", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_bmp, 3, COMMON, COMMON_RULES},
    {RFC_FORMAT_PNG, "png", "libpng (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "1", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_png, 6, COMMON, COMMON_RULES},
    {RFC_FORMAT_APNG, "apng", "libpng (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "1", "D10 T02 \xc2\xa7" "12", PLANNED, "converter adapter not yet shipped; animation extensions owned by D10 T02 \xc2\xa7" "12", 1, 1, k_png, 6, ANIM, ANIM_RULES},
    {RFC_FORMAT_PNM, "pnm", "in-tree converter code", "D10 T02 \xc2\xa7" "1", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_pnm, 4, COMMON, COMMON_RULES},
    {RFC_FORMAT_PAM, "pam", "in-tree converter code", "D10 T02 \xc2\xa7" "1", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_pam, 6, COMMON, COMMON_RULES},
    {RFC_FORMAT_WEBP, "webp", "libwebp (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "2", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_rgba8, 2, COMMON, COMMON_RULES},
    {RFC_FORMAT_JPEG_XL, "jpeg-xl", "libjxl (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "3", "D10 T02 \xc2\xa7" "12", PLANNED, "converter adapter not yet shipped", 1, 1, k_png, 6, JXLSET, JXL_RULES},
    {RFC_FORMAT_AVIF, "avif", "libavif (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "4", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_10bit, 4, COMMON, COMMON_RULES},
    {RFC_FORMAT_JPEG, "jpeg", "libjpeg-turbo (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "5", NULL, PLANNED, "converter adapter not yet shipped", 1, 0, k_jpeg, 2, JPEGSET, COMMON_RULES},
    {RFC_FORMAT_GIF, "gif", "in-tree converter code", "D10 T02 \xc2\xa7" "6", "D10 T02 \xc2\xa7" "12", PLANNED, "converter adapter not yet shipped", 1, 1, k_rgba8, 2, ANIM, ANIM_RULES},
    {RFC_FORMAT_TIFF, "tiff", "libtiff (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "6", "D10 T02 \xc2\xa7" "12", PLANNED, "converter adapter not yet shipped", 1, 1, k_png, 6, ANIM, ANIM_RULES},
    {RFC_FORMAT_HEIF, "heif", "Windows Imaging Component (system codec, not bundled)", "D10 T02 \xc2\xa7" "7", NULL, PLANNED, "import-only system-codec adapter not yet shipped; decision recorded in docs/codec-distribution.md", 1, 0, k_10bit, 4, COMMON, COMMON_RULES},
    {RFC_FORMAT_HEIC, "heic", "Windows Imaging Component (system codec, not bundled)", "D10 T02 \xc2\xa7" "7", NULL, PLANNED, "import-only system-codec adapter not yet shipped; decision recorded in docs/codec-distribution.md", 1, 0, k_10bit, 4, COMMON, COMMON_RULES},
    {RFC_FORMAT_JPEG2000, "jpeg-2000", "OpenJPEG (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "9", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_png, 6, COMMON, COMMON_RULES},
    {RFC_FORMAT_QOI, "qoi", "in-tree converter code", "D10 T02 \xc2\xa7" "8", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_rgba8, 2, COMMON, COMMON_RULES},
    {RFC_FORMAT_OPENEXR, "openexr", "OpenEXR (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "10", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_exr, 4, COMMON, COMMON_RULES},
    {RFC_FORMAT_CAMERA_RAW, "camera-raw", "LibRaw (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "11", NULL, PLANNED, "import-only adapter not yet shipped", 1, 0, k_png, 6, COMMON, COMMON_RULES},
    {RFC_FORMAT_DNG, "dng", "LibRaw (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "11", NULL, PLANNED, "import-only adapter not yet shipped", 1, 0, k_png, 6, COMMON, COMMON_RULES},
};
#define REGISTRY_COUNT (sizeof(k_registry) / sizeof(k_registry[0]))

size_t rfc_registry_count(void) { return REGISTRY_COUNT; }
const rfc_capability *rfc_registry_at(size_t i) { return i < REGISTRY_COUNT ? &k_registry[i] : NULL; }
const rfc_capability *rfc_registry_find(rfc_format_id id) {
    for (size_t i = 0; i < REGISTRY_COUNT; i++) if (k_registry[i].id == id) return &k_registry[i];
    return NULL;
}
const rfc_capability *rfc_registry_find_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < REGISTRY_COUNT; i++) if (strcmp(k_registry[i].name, name) == 0) return &k_registry[i];
    return NULL;
}

rfc_status rfc_registry_validate_settings(rfc_format_id id, const rfc_setting_value *values,
                                          size_t value_count, const char **reason_out) {
    const rfc_capability *cap = rfc_registry_find(id);
    if (!cap) return RFC_ERR_UNKNOWN_FORMAT;
    if (value_count && !values) return RFC_ERR_INVALID_ARGUMENT;
    const char *metadata = NULL, *exactness = NULL;
    for (size_t i = 0; i < value_count; i++) {
        if (!values[i].id || !values[i].value) return RFC_ERR_INVALID_ARGUMENT;
        for (size_t k = 0; k < i; k++) {
            if (strcmp(values[k].id, values[i].id) == 0) {
                if (reason_out) *reason_out = "duplicate setting; deliver one value per setting";
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
        const rfc_setting_def *def = NULL;
        for (size_t j = 0; j < cap->setting_count; j++)
            if (strcmp(cap->settings[j].id, values[i].id) == 0) def = &cap->settings[j];
        if (!def) { if (reason_out) *reason_out = "unknown setting for this format"; return RFC_ERR_UNKNOWN_SETTING; }
        if (strcmp(def->type, "enum") == 0) {
            int ok = 0;
            for (size_t j = 0; j < def->allowed_count; j++) if (strcmp(def->allowed[j], values[i].value) == 0) ok = 1;
            if (!ok) { if (reason_out) *reason_out = def->refusal_reason; return RFC_ERR_SETTING_VALUE; }
        }
        (void)metadata; (void)exactness;
    }
    for (size_t r = 0; r < cap->rule_count; r++) {
        const rfc_combination_rule *rule = &cap->rules[r];
        const char *va = NULL, *vb = NULL;
        for (size_t i = 0; i < value_count; i++) {
            if (strcmp(values[i].id, rule->setting_a) == 0) va = values[i].value;
            if (rule->setting_b && strcmp(values[i].id, rule->setting_b) == 0) vb = values[i].value;
        }
        if (rule->setting_b && !rule->value_b) continue; /* malformed rule: never dereference */
        if (va && strcmp(va, rule->value_a) == 0 &&
            (!rule->setting_b || (vb && strcmp(vb, rule->value_b) == 0))) {
            if (reason_out) *reason_out = rule->reason;
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
    }
    return RFC_OK;
}

static const rfc_adapter *k_adapters[REGISTRY_COUNT];
rfc_status rfc_registry_select(rfc_format_id id, rfc_mode mode) {
    const rfc_capability *cap = rfc_registry_find(id);
    if (!cap) return RFC_ERR_UNKNOWN_FORMAT;
    if (mode == RFC_MODE_READ && !cap->can_read) return RFC_ERR_UNSUPPORTED_COMBINATION;
    if (mode == RFC_MODE_WRITE && !cap->can_write) return RFC_ERR_UNSUPPORTED_COMBINATION;
    if (cap->availability == RFC_AVAILABILITY_PLANNED) return RFC_ERR_PLANNED;
    if (!rfc_registry_adapter(id)) return RFC_ERR_NO_ADAPTER;
    return RFC_OK;
}
rfc_status rfc_registry_register(const rfc_adapter *adapter) {
    if (!adapter) return RFC_ERR_INVALID_ARGUMENT;
    if (adapter->abi_version != RFC_CODEC_ABI_VERSION) return RFC_ERR_ABI_VERSION;
    if (!adapter->probe || !adapter->encode || !adapter->decode || !adapter->cancel || !adapter->free_adapter)
        return RFC_ERR_INVALID_ARGUMENT;
    const rfc_capability *cap = rfc_registry_find(adapter->format);
    if (!cap) return RFC_ERR_UNKNOWN_FORMAT;
    for (size_t i = 0; i < REGISTRY_COUNT; i++) if (k_registry[i].id == adapter->format) {
        if (k_adapters[i]) return RFC_ERR_DUPLICATE;
        k_adapters[i] = adapter; return RFC_OK;
    }
    return RFC_ERR_UNKNOWN_FORMAT;
}
rfc_status rfc_registry_unregister(rfc_format_id id) {
    for (size_t i = 0; i < REGISTRY_COUNT; i++) if (k_registry[i].id == id) { k_adapters[i] = NULL; return RFC_OK; }
    return RFC_ERR_UNKNOWN_FORMAT;
}
const rfc_adapter *rfc_registry_adapter(rfc_format_id id) {
    for (size_t i = 0; i < REGISTRY_COUNT; i++) if (k_registry[i].id == id) return k_adapters[i];
    return NULL;
}
const char *rfc_status_name(rfc_status s) {
    switch (s) {
        case RFC_OK: return "ok";
        case RFC_ERR_INVALID_ARGUMENT: return "invalid-argument";
        case RFC_ERR_OVERFLOW: return "overflow";
        case RFC_ERR_UNSUPPORTED_SAMPLE: return "unsupported-sample";
        case RFC_ERR_UNSUPPORTED_ALPHA: return "unsupported-alpha";
        case RFC_ERR_UNSUPPORTED_COMBINATION: return "unsupported-combination";
        case RFC_ERR_UNKNOWN_FORMAT: return "unknown-format";
        case RFC_ERR_UNKNOWN_SETTING: return "unknown-setting";
        case RFC_ERR_SETTING_VALUE: return "setting-value";
        case RFC_ERR_PLANNED: return "planned";
        case RFC_ERR_NO_ADAPTER: return "no-adapter";
        case RFC_ERR_DUPLICATE: return "duplicate";
        case RFC_ERR_ABI_VERSION: return "abi-version";
        case RFC_ERR_OUT_OF_MEMORY: return "out-of-memory";
        default: return "unknown-status";
    }
}
