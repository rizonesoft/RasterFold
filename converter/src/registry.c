#include "rfc_codec.h"
#include "codec_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic(rfc_frame_alloc_guard_fn) g_frame_alloc_guard = NULL;

void rfc_internal_set_frame_alloc_guard(rfc_frame_alloc_guard_fn fn) {
    atomic_store_explicit(&g_frame_alloc_guard, fn, memory_order_release);
}

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

int rfc_colorimetry_has_any(const rfc_colorimetry *c) {
    if (!c) {
        return 0;
    }
    return c->has_srgb || c->has_gama || c->has_chrm || c->has_cicp ||
           c->has_mdcv || c->has_clli;
}

int rfc_colorimetry_is_valid(const rfc_colorimetry *c) {
    if (!c) {
        return 1;
    }
    if (c->has_srgb > 1u || c->has_gama > 1u || c->has_chrm > 1u ||
        c->has_cicp > 1u || c->has_mdcv > 1u || c->has_clli > 1u) {
        return 0;
    }
    if (c->has_srgb && c->srgb_intent > 3u) {
        return 0;
    }
    if (c->has_gama &&
        (c->gama_scaled == 0u || c->gama_scaled > 0x7FFFFFFFu)) {
        return 0;
    }
    if (c->has_chrm) {
        for (size_t i = 0u; i < 8u; i++) {
            if (c->chrm_xy[i] > 0x7FFFFFFFu) {
                return 0;
            }
        }
    }
    if (c->has_cicp && (c->cicp[2] != 0u || c->cicp[3] > 1u)) {
        return 0;
    }
    if (c->has_mdcv &&
        (c->mdcv_max_dl > 0x7FFFFFFFu || c->mdcv_min_dl > 0x7FFFFFFFu)) {
        return 0;
    }
    if (c->has_clli &&
        (c->clli_max_cll > 0x7FFFFFFFu || c->clli_max_fall > 0x7FFFFFFFu)) {
        return 0;
    }
    return 1;
}

rfc_status rfc_frame_alloc(rfc_frame *frame, const rfc_layout *layout,
                           const rfc_metadata *metadata,
                           const rfc_allocator *allocator) {
    if (!frame || !allocator || !allocator->alloc || !allocator->free)
        return RFC_ERR_INVALID_ARGUMENT;
    size_t total = 0;
    rfc_status st = rfc_layout_validate(layout, &total);
    if (st != RFC_OK) return st; /* refusal happens before any allocation */
    if (metadata) {
        if (metadata->icc_len > RFC_MAX_ICC_BYTES ||
            metadata->exif_len > RFC_MAX_ICC_BYTES ||
            metadata->xmp_len > RFC_MAX_ICC_BYTES ||
            metadata->icc_len + metadata->exif_len > RFC_MAX_ICC_BYTES ||
            metadata->icc_len + metadata->exif_len + metadata->xmp_len >
                RFC_MAX_ICC_BYTES)
            return RFC_ERR_INVALID_ARGUMENT;
        if ((metadata->icc_len && !metadata->icc) ||
            (metadata->exif_len && !metadata->exif) ||
            (metadata->xmp_len && !metadata->xmp))
            return RFC_ERR_INVALID_ARGUMENT;
        if (metadata->orientation > RFC_ORIENTATION_ROTATE_270)
            return RFC_ERR_INVALID_ARGUMENT;
        if (!rfc_colorimetry_is_valid(&metadata->color))
            return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_frame_alloc_guard_fn guard =
        atomic_load_explicit(&g_frame_alloc_guard, memory_order_acquire);
    if (guard) {
        st = guard(allocator, layout, metadata);
        if (st != RFC_OK) return st;
    }
    uint8_t *data = (uint8_t *)allocator->alloc(allocator->ctx, total);
    if (!data) return RFC_ERR_OUT_OF_MEMORY;
    uint8_t *icc = NULL;
    uint8_t *exif = NULL;
    uint8_t *xmp = NULL;
    if (metadata && metadata->icc_len) {
        icc = (uint8_t *)allocator->alloc(allocator->ctx, metadata->icc_len);
        if (!icc) {
            allocator->free(allocator->ctx, data);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(icc, metadata->icc, metadata->icc_len);
    }
    if (metadata && metadata->exif_len) {
        exif = (uint8_t *)allocator->alloc(allocator->ctx, metadata->exif_len);
        if (!exif) {
            if (icc) allocator->free(allocator->ctx, icc);
            allocator->free(allocator->ctx, data);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(exif, metadata->exif, metadata->exif_len);
    }
    if (metadata && metadata->xmp_len) {
        xmp = (uint8_t *)allocator->alloc(allocator->ctx, metadata->xmp_len);
        if (!xmp) {
            if (exif) allocator->free(allocator->ctx, exif);
            if (icc) allocator->free(allocator->ctx, icc);
            allocator->free(allocator->ctx, data);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(xmp, metadata->xmp, metadata->xmp_len);
    }
    memset(frame, 0, sizeof(*frame));
    frame->layout = *layout;
    frame->data = data;
    frame->data_len = total;
    frame->allocator = allocator;
    if (metadata) frame->metadata = *metadata;
    frame->metadata.icc = icc;
    frame->metadata.exif = exif;
    frame->metadata.xmp = xmp;
    return RFC_OK;
}

void rfc_frame_free(rfc_frame *frame) {
    if (!frame || !frame->allocator) return;
    if (frame->metadata.icc)
        frame->allocator->free(frame->allocator->ctx, (void *)frame->metadata.icc);
    if (frame->metadata.exif && frame->metadata.exif != frame->metadata.icc)
        frame->allocator->free(frame->allocator->ctx, (void *)frame->metadata.exif);
    if (frame->metadata.xmp && frame->metadata.xmp != frame->metadata.icc &&
        frame->metadata.xmp != frame->metadata.exif)
        frame->allocator->free(frame->allocator->ctx, (void *)frame->metadata.xmp);
    if (frame->data) frame->allocator->free(frame->allocator->ctx, frame->data);
    memset(frame, 0, sizeof(*frame));
}

void rfc_metadata_free(rfc_metadata *meta, const rfc_allocator *allocator) {
    if (!meta) return;
    const void *p_icc = meta->icc;
    const void *p_exif = meta->exif;
    const void *p_xmp = meta->xmp;
    if (p_icc) {
        if (allocator && allocator->free)
            allocator->free(allocator->ctx, (void *)p_icc);
        else
            free((void *)p_icc);
    }
    if (p_exif && p_exif != p_icc) {
        if (allocator && allocator->free)
            allocator->free(allocator->ctx, (void *)p_exif);
        else
            free((void *)p_exif);
    }
    if (p_xmp && p_xmp != p_icc && p_xmp != p_exif) {
        if (allocator && allocator->free)
            allocator->free(allocator->ctx, (void *)p_xmp);
        else
            free((void *)p_xmp);
    }
    meta->icc = NULL;
    meta->icc_len = 0u;
    meta->exif = NULL;
    meta->exif_len = 0u;
    meta->xmp = NULL;
    meta->xmp_len = 0u;
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
    {"exactness", "enum", "coefficient-exact", k_exactness_jpeg, 3, 0, 0,
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
#define USABLE RFC_AVAILABILITY_USABLE

#define META_NONE RFC_META_ITEM_NONE
#define META_ICC RFC_META_ITEM_ICC
#define META_ORI RFC_META_ITEM_ORIENTATION
#define META_TIM RFC_META_ITEM_FRAME_TIMING
#define META_EXIF RFC_META_ITEM_EXIF
#define META_XMP RFC_META_ITEM_XMP

static const rfc_capability k_registry[] = {
    {RFC_FORMAT_RASTERFOLD, "rasterfold", "RasterFold (this repository)", "D10 T02 \xc2\xa7" "1", NULL, USABLE, NULL, 1, 1, k_rgb8, 1, COMMON, COMMON_RULES, META_NONE},
    {RFC_FORMAT_BMP, "bmp", "in-tree converter code", "D10 T02 \xc2\xa7" "1", NULL, USABLE, NULL, 1, 1, k_bmp, 3, COMMON, COMMON_RULES, META_NONE},
    {RFC_FORMAT_PNG, "png", "libpng (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "1", NULL, USABLE, NULL, 1, 1, k_png, 6, COMMON, COMMON_RULES, META_ICC},
    {RFC_FORMAT_APNG, "apng", "libpng (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "1", "D10 T02 \xc2\xa7" "12", PLANNED, "APNG multi-frame sequence composition and export owned by D10 T02 \xc2\xa7" "12", 1, 1, k_png, 6, ANIM, ANIM_RULES, META_ICC | META_TIM},
    {RFC_FORMAT_PNM, "pnm", "in-tree converter code", "D10 T02 \xc2\xa7" "1", NULL, USABLE, NULL, 1, 1, k_pnm, 4, COMMON, COMMON_RULES, META_NONE},
    {RFC_FORMAT_PAM, "pam", "in-tree converter code", "D10 T02 \xc2\xa7" "1", NULL, USABLE, NULL, 1, 1, k_pam, 6, COMMON, COMMON_RULES, META_NONE},
    {RFC_FORMAT_WEBP, "webp", "libwebp (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "2", NULL, USABLE, NULL, 1, 1, k_rgba8, 2, COMMON, COMMON_RULES, META_ICC | META_ORI | META_EXIF | META_XMP},
    {RFC_FORMAT_JPEG_XL, "jpeg-xl", "libjxl (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "3", "D10 T02 \xc2\xa7" "12", USABLE, NULL, 1, 1, k_png, 6, JXLSET, JXL_RULES, META_ICC | META_ORI | META_TIM | META_EXIF | META_XMP},
    {RFC_FORMAT_AVIF, "avif", "libavif (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "4", NULL, USABLE, NULL, 1, 1, k_10bit, 4, COMMON, COMMON_RULES, META_ICC | META_ORI | META_EXIF | META_XMP},
    {RFC_FORMAT_JPEG, "jpeg", "libjpeg-turbo (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "5", NULL, USABLE, NULL, 1, 1, k_jpeg, 2, JPEGSET, COMMON_RULES, META_ICC | META_ORI | META_EXIF | META_XMP},
    {RFC_FORMAT_GIF, "gif", "giflib (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "6", "D10 T02 \xc2\xa7" "12", USABLE, NULL, 1, 1, k_rgba8, 2, ANIM, ANIM_RULES, META_TIM},
    {RFC_FORMAT_TIFF, "tiff", "libtiff (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "6", "D10 T02 \xc2\xa7" "12", USABLE, NULL, 1, 1, k_png, 6, ANIM, ANIM_RULES, META_ICC | META_ORI | META_XMP},
    {RFC_FORMAT_HEIF, "heif", "Windows Imaging Component (system codec, not bundled)", "D10 T02 \xc2\xa7" "7", NULL, USABLE, NULL, 1, 0, k_rgb8, 1, COMMON, COMMON_RULES, META_ICC},
    {RFC_FORMAT_HEIC, "heic", "Windows Imaging Component (system codec, not bundled)", "D10 T02 \xc2\xa7" "7", NULL, USABLE, NULL, 1, 0, k_rgb8, 1, COMMON, COMMON_RULES, META_ICC},
    {RFC_FORMAT_JPEG2000, "jpeg-2000", "OpenJPEG (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "9", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_png, 6, COMMON, COMMON_RULES, META_ICC},
    {RFC_FORMAT_QOI, "qoi", "in-tree converter code", "D10 T02 \xc2\xa7" "8", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_rgba8, 2, COMMON, COMMON_RULES, META_NONE},
    {RFC_FORMAT_OPENEXR, "openexr", "OpenEXR (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "10", NULL, PLANNED, "converter adapter not yet shipped", 1, 1, k_exr, 4, COMMON, COMMON_RULES, META_ICC},
    {RFC_FORMAT_CAMERA_RAW, "camera-raw", "LibRaw (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "11", NULL, PLANNED, "import-only adapter not yet shipped", 1, 0, k_png, 6, COMMON, COMMON_RULES, META_ICC | META_ORI},
    {RFC_FORMAT_DNG, "dng", "LibRaw (bundled, pinned by D10 T01 \xc2\xa7" "2)", "D10 T02 \xc2\xa7" "11", NULL, PLANNED, "import-only adapter not yet shipped", 1, 0, k_png, 6, COMMON, COMMON_RULES, META_ICC | META_ORI},
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

const char *rfc_registry_setting_default(rfc_format_id id, const char *setting_id) {
    const rfc_capability *cap = rfc_registry_find(id);
    if (!cap || !setting_id) return NULL;
    for (size_t j = 0; j < cap->setting_count; j++) {
        if (strcmp(cap->settings[j].id, setting_id) == 0) return cap->settings[j].default_value;
    }
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

#include <stdatomic.h>

static atomic_int k_reg_lock = 0;
static const rfc_adapter *k_adapters[REGISTRY_COUNT];
static size_t k_adapter_active[REGISTRY_COUNT];
static const rfc_session_adapter *k_session_adapters[REGISTRY_COUNT];
static size_t k_session_active[REGISTRY_COUNT];
static const rfc_jpeg_reconstruct_adapter *k_jrec_adapter = NULL;
static size_t k_jrec_active = 0;
static const rfc_jpeg_transcode_adapter *k_jtrans_adapter = NULL;
static size_t k_jtrans_active = 0;

static void reg_lock(void) {
    while (atomic_exchange_explicit(&k_reg_lock, 1, memory_order_acquire) != 0) {
    }
}

static void reg_unlock(void) {
    atomic_store_explicit(&k_reg_lock, 0, memory_order_release);
}

static int reg_find_index(rfc_format_id id) {
    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        if (k_registry[i].id == id) return (int)i;
    }
    return -1;
}
rfc_status rfc_registry_select(rfc_format_id id, rfc_mode mode) {
    const rfc_capability *cap = rfc_registry_find(id);
    if (!cap) return RFC_ERR_UNKNOWN_FORMAT;
    if (mode == RFC_MODE_READ && !cap->can_read) return RFC_ERR_UNSUPPORTED_COMBINATION;
    if (mode == RFC_MODE_WRITE && !cap->can_write) return RFC_ERR_UNSUPPORTED_COMBINATION;
    if (cap->availability == RFC_AVAILABILITY_PLANNED) return RFC_ERR_PLANNED;
    if (!rfc_registry_adapter(id) && !rfc_registry_session_adapter(id)) return RFC_ERR_NO_ADAPTER;
    return RFC_OK;
}
rfc_status rfc_registry_register(const rfc_adapter *adapter) {
    if (!adapter) return RFC_ERR_INVALID_ARGUMENT;
    if (adapter->abi_version != RFC_CODEC_ABI_VERSION) return RFC_ERR_ABI_VERSION;
    if (!adapter->probe || !adapter->encode || !adapter->decode || !adapter->cancel || !adapter->free_adapter)
        return RFC_ERR_INVALID_ARGUMENT;
    reg_lock();
    int idx = reg_find_index(adapter->format);
    if (idx < 0) {
        reg_unlock();
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (k_adapters[idx]) {
        reg_unlock();
        return RFC_ERR_DUPLICATE;
    }
    k_adapters[idx] = adapter;
    reg_unlock();
    return RFC_OK;
}
rfc_status rfc_registry_unregister(rfc_format_id id) {
    reg_lock();
    int idx = reg_find_index(id);
    if (idx < 0) {
        reg_unlock();
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (k_adapter_active[idx] > 0) {
        reg_unlock();
        return RFC_ERR_BUSY;
    }
    k_adapters[idx] = NULL;
    reg_unlock();
    return RFC_OK;
}
const rfc_adapter *rfc_registry_adapter(rfc_format_id id) {
    reg_lock();
    int idx = reg_find_index(id);
    const rfc_adapter *out = (idx >= 0) ? k_adapters[idx] : NULL;
    reg_unlock();
    return out;
}
const rfc_adapter *rfc_registry_acquire_adapter(rfc_format_id id) {
    reg_lock();
    int idx = reg_find_index(id);
    const rfc_adapter *out = NULL;
    if (idx >= 0 && k_adapters[idx] != NULL) {
        k_adapter_active[idx]++;
        out = k_adapters[idx];
    }
    reg_unlock();
    return out;
}
void rfc_registry_release_adapter(rfc_format_id id) {
    reg_lock();
    int idx = reg_find_index(id);
    if (idx >= 0 && k_adapter_active[idx] > 0) {
        k_adapter_active[idx]--;
    }
    reg_unlock();
}
rfc_status rfc_registry_register_session(const rfc_session_adapter *adapter) {
    if (!adapter || !adapter->open_session || !adapter->close_session ||
        !adapter->probe_session || !adapter->encode_session ||
        !adapter->decode_session) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (adapter->abi_version != RFC_CODEC_ABI_VERSION) {
        return RFC_ERR_ABI_VERSION;
    }
    reg_lock();
    int idx = reg_find_index(adapter->format);
    if (idx < 0) {
        reg_unlock();
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (k_session_adapters[idx] != NULL) {
        reg_unlock();
        return RFC_ERR_DUPLICATE;
    }
    k_session_adapters[idx] = adapter;
    reg_unlock();
    return RFC_OK;
}
rfc_status rfc_registry_unregister_session(rfc_format_id id) {
    reg_lock();
    int idx = reg_find_index(id);
    if (idx < 0) {
        reg_unlock();
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (k_session_active[idx] > 0) {
        reg_unlock();
        return RFC_ERR_BUSY;
    }
    k_session_adapters[idx] = NULL;
    reg_unlock();
    return RFC_OK;
}
const rfc_session_adapter *rfc_registry_session_adapter(rfc_format_id id) {
    reg_lock();
    int idx = reg_find_index(id);
    const rfc_session_adapter *out = (idx >= 0) ? k_session_adapters[idx] : NULL;
    reg_unlock();
    return out;
}
const rfc_session_adapter *rfc_registry_acquire_session(rfc_format_id id) {
    reg_lock();
    int idx = reg_find_index(id);
    const rfc_session_adapter *out = NULL;
    if (idx >= 0 && k_session_adapters[idx] != NULL) {
        k_session_active[idx]++;
        out = k_session_adapters[idx];
    }
    reg_unlock();
    return out;
}
void rfc_registry_release_session(rfc_format_id id) {
    reg_lock();
    int idx = reg_find_index(id);
    if (idx >= 0 && k_session_active[idx] > 0) {
        k_session_active[idx]--;
    }
    reg_unlock();
}
rfc_status rfc_registry_register_jpeg_reconstruct(
    const rfc_jpeg_reconstruct_adapter *adapter) {
    if (!adapter || !adapter->open_session || !adapter->close_session ||
        !adapter->probe_jpeg || !adapter->transcode_jpeg_to_jxl ||
        !adapter->reconstruct_jpeg_from_jxl) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (adapter->abi_version != RFC_CODEC_ABI_VERSION) {
        return RFC_ERR_ABI_VERSION;
    }
    reg_lock();
    if (k_jrec_adapter != NULL) {
        reg_unlock();
        return RFC_ERR_DUPLICATE;
    }
    k_jrec_adapter = adapter;
    reg_unlock();
    return RFC_OK;
}
rfc_status rfc_registry_unregister_jpeg_reconstruct(void) {
    reg_lock();
    if (k_jrec_active > 0) {
        reg_unlock();
        return RFC_ERR_BUSY;
    }
    k_jrec_adapter = NULL;
    reg_unlock();
    return RFC_OK;
}
const rfc_jpeg_reconstruct_adapter *rfc_registry_jpeg_reconstruct_adapter(void) {
    reg_lock();
    const rfc_jpeg_reconstruct_adapter *out = k_jrec_adapter;
    reg_unlock();
    return out;
}
const rfc_jpeg_reconstruct_adapter *rfc_registry_acquire_jpeg_reconstruct(void) {
    reg_lock();
    const rfc_jpeg_reconstruct_adapter *out = NULL;
    if (k_jrec_adapter != NULL) {
        k_jrec_active++;
        out = k_jrec_adapter;
    }
    reg_unlock();
    return out;
}
void rfc_registry_release_jpeg_reconstruct(void) {
    reg_lock();
    if (k_jrec_active > 0) {
        k_jrec_active--;
    }
    reg_unlock();
}
rfc_status rfc_registry_register_jpeg_transcode(
    const rfc_jpeg_transcode_adapter *adapter) {
    if (!adapter || !adapter->open_session || !adapter->close_session ||
        !adapter->preflight_optimize ||
        !adapter->optimize_jpeg || !adapter->read_coefficients ||
        !adapter->free_coefficients || !adapter->compare_markers) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (adapter->abi_version != RFC_CODEC_ABI_VERSION) {
        return RFC_ERR_ABI_VERSION;
    }
    reg_lock();
    if (k_jtrans_adapter != NULL) {
        reg_unlock();
        return RFC_ERR_DUPLICATE;
    }
    k_jtrans_adapter = adapter;
    reg_unlock();
    return RFC_OK;
}
rfc_status rfc_registry_unregister_jpeg_transcode(void) {
    reg_lock();
    if (k_jtrans_active > 0) {
        reg_unlock();
        return RFC_ERR_BUSY;
    }
    k_jtrans_adapter = NULL;
    reg_unlock();
    return RFC_OK;
}
const rfc_jpeg_transcode_adapter *rfc_registry_acquire_jpeg_transcode(void) {
    reg_lock();
    const rfc_jpeg_transcode_adapter *out = NULL;
    if (k_jtrans_adapter != NULL) {
        k_jtrans_active++;
        out = k_jtrans_adapter;
    }
    reg_unlock();
    return out;
}
void rfc_registry_release_jpeg_transcode(void) {
    reg_lock();
    if (k_jtrans_active > 0) {
        k_jtrans_active--;
    }
    reg_unlock();
}
static int session_adapter_valid(const rfc_session_adapter *a) {
    return a && a->open_session && a->close_session && a->probe_session &&
           a->encode_session && a->decode_session;
}
static int jtrans_adapter_valid(const rfc_jpeg_transcode_adapter *a) {
    return a && a->open_session && a->close_session && a->preflight_optimize &&
           a->optimize_jpeg && a->read_coefficients && a->free_coefficients &&
           a->compare_markers;
}
rfc_status rfc_registry_register_session_pair(const rfc_session_adapter *a,
                                              const rfc_session_adapter *b) {
    if (!session_adapter_valid(a) || !session_adapter_valid(b) ||
        a->format == b->format) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (a->abi_version != RFC_CODEC_ABI_VERSION ||
        b->abi_version != RFC_CODEC_ABI_VERSION) {
        return RFC_ERR_ABI_VERSION;
    }
    reg_lock();
    int ia = reg_find_index(a->format);
    int ib = reg_find_index(b->format);
    if (ia < 0 || ib < 0) {
        reg_unlock();
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (k_session_adapters[ia] != NULL || k_session_adapters[ib] != NULL) {
        reg_unlock();
        return RFC_ERR_DUPLICATE;
    }
    k_session_adapters[ia] = a;
    k_session_adapters[ib] = b;
    reg_unlock();
    return RFC_OK;
}
rfc_status rfc_registry_unregister_session_pair(rfc_format_id a,
                                                rfc_format_id b) {
    reg_lock();
    int ia = reg_find_index(a);
    int ib = reg_find_index(b);
    if (ia < 0 || ib < 0) {
        reg_unlock();
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (k_session_active[ia] > 0 || k_session_active[ib] > 0) {
        reg_unlock();
        return RFC_ERR_BUSY;
    }
    k_session_adapters[ia] = NULL;
    k_session_adapters[ib] = NULL;
    reg_unlock();
    return RFC_OK;
}
rfc_status rfc_registry_register_jpeg_pair(
    const rfc_session_adapter *session_adapter,
    const rfc_jpeg_transcode_adapter *transcode_adapter) {
    if (!session_adapter_valid(session_adapter) ||
        !jtrans_adapter_valid(transcode_adapter) ||
        session_adapter->format != RFC_FORMAT_JPEG) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (session_adapter->abi_version != RFC_CODEC_ABI_VERSION ||
        transcode_adapter->abi_version != RFC_CODEC_ABI_VERSION) {
        return RFC_ERR_ABI_VERSION;
    }
    reg_lock();
    int idx = reg_find_index(RFC_FORMAT_JPEG);
    if (idx < 0) {
        reg_unlock();
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (k_session_adapters[idx] != NULL || k_jtrans_adapter != NULL) {
        reg_unlock();
        return RFC_ERR_DUPLICATE;
    }
    k_session_adapters[idx] = session_adapter;
    k_jtrans_adapter = transcode_adapter;
    reg_unlock();
    return RFC_OK;
}
rfc_status rfc_registry_unregister_jpeg_pair(void) {
    reg_lock();
    int idx = reg_find_index(RFC_FORMAT_JPEG);
    if (idx < 0) {
        reg_unlock();
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (k_session_active[idx] > 0 || k_jtrans_active > 0) {
        reg_unlock();
        return RFC_ERR_BUSY;
    }
    k_session_adapters[idx] = NULL;
    k_jtrans_adapter = NULL;
    reg_unlock();
    return RFC_OK;
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
        case RFC_ERR_CANCELLED: return "cancelled";
        case RFC_ERR_LIMIT_EXCEEDED: return "limit-exceeded";
        case RFC_ERR_STALE_JOB: return "stale-job";
        case RFC_ERR_BUSY: return "busy";
        case RFC_ERR_VERIFY_MISMATCH: return "verify-mismatch";
        case RFC_ERR_METADATA_POLICY: return "metadata-policy";
        case RFC_ERR_IO: return "io-error";
        case RFC_ERR_PERMISSION_DENIED: return "permission-denied";
        case RFC_ERR_DESTINATION_EXISTS: return "destination-exists";
        case RFC_ERR_PATH_ALIAS: return "path-alias";
        case RFC_ERR_DISK_FULL: return "disk-full";
        case RFC_ERR_FORMAT_DISAGREEMENT: return "format-disagreement";
        case RFC_ERR_TRUNCATED_INPUT: return "truncated-input";
        default: return "unknown-status";
    }
}
