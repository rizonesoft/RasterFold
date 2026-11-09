/* Versioned adapter lifecycle, capability registry and result codes.
 * The registry is data (converter/codecs.json) mirrored by
 * converter/src/registry.c; this header is the only C contract. */
#ifndef RFC_CODEC_H
#define RFC_CODEC_H

#include "rfc_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RFC_CODEC_ABI_VERSION 1u

typedef enum rfc_status {
    RFC_OK = 0,
    RFC_ERR_INVALID_ARGUMENT = 1,
    RFC_ERR_OVERFLOW = 2,
    RFC_ERR_UNSUPPORTED_SAMPLE = 3,
    RFC_ERR_UNSUPPORTED_ALPHA = 4,
    RFC_ERR_UNSUPPORTED_COMBINATION = 5,
    RFC_ERR_UNKNOWN_FORMAT = 6,
    RFC_ERR_UNKNOWN_SETTING = 7,
    RFC_ERR_SETTING_VALUE = 8,
    RFC_ERR_PLANNED = 9,       /* visible in the registry, not yet shipped */
    RFC_ERR_NO_ADAPTER = 10,   /* usable format with no registered adapter */
    RFC_ERR_DUPLICATE = 11,
    RFC_ERR_ABI_VERSION = 12,
    RFC_ERR_OUT_OF_MEMORY = 13
} rfc_status;

typedef enum rfc_format_id {
    RFC_FORMAT_RASTERFOLD = 1,
    RFC_FORMAT_BMP = 2,
    RFC_FORMAT_PNG = 3,
    RFC_FORMAT_APNG = 4,
    RFC_FORMAT_PNM = 5,
    RFC_FORMAT_PAM = 6,
    RFC_FORMAT_WEBP = 7,
    RFC_FORMAT_JPEG_XL = 8,
    RFC_FORMAT_AVIF = 9,
    RFC_FORMAT_JPEG = 10,
    RFC_FORMAT_GIF = 11,
    RFC_FORMAT_TIFF = 12,
    RFC_FORMAT_HEIF = 13,
    RFC_FORMAT_HEIC = 14,
    RFC_FORMAT_JPEG2000 = 15,
    RFC_FORMAT_QOI = 16,
    RFC_FORMAT_OPENEXR = 17,
    RFC_FORMAT_CAMERA_RAW = 18,
    RFC_FORMAT_DNG = 19
} rfc_format_id;

typedef enum rfc_availability {
    RFC_AVAILABILITY_PLANNED = 0, /* listed with a reason; never selectable */
    RFC_AVAILABILITY_USABLE = 1   /* provider section shipped; selectable */
} rfc_availability;

typedef enum rfc_mode { RFC_MODE_READ = 1, RFC_MODE_WRITE = 2 } rfc_mode;

typedef struct rfc_setting_def {
    const char *id;
    const char *type; /* "enum", "int" or "bool" */
    const char *default_value;
    const char *const *allowed; /* enum values, NULL-terminated; NULL otherwise */
    size_t allowed_count;
    int64_t min_value; /* int range; ignored for other types */
    int64_t max_value;
    const char *refusal_reason;
} rfc_setting_def;

/* A discoverable combination constraint: when setting_a has value_a and
 * (setting_b is NULL or setting_b has value_b), the combination is
 * refused with reason. Rules are data, mirrored in converter/codecs.json. */
typedef struct rfc_combination_rule {
    const char *setting_a;
    const char *value_a;
    const char *setting_b; /* NULL means: value_a alone is refused */
    const char *value_b;
    const char *reason;
} rfc_combination_rule;

typedef struct rfc_capability {
    rfc_format_id id;
    const char *name;
    const char *provider;
    const char *owner; /* owning section, e.g. "D10 T02 \u00a71" */
    const char *extension_owner; /* nullable: owner of animation/multi-page extensions */
    rfc_availability availability;
    const char *unavailable_reason; /* required when availability is planned */
    int can_read;
    int can_write;
    const rfc_sample_format *samples;
    size_t sample_count;
    const rfc_setting_def *settings;
    size_t setting_count;
    const rfc_combination_rule *rules;
    size_t rule_count;
} rfc_capability;

typedef struct rfc_setting_value {
    const char *id;
    const char *value; /* textual form; validated against the definition */
} rfc_setting_value;

/* Adapter lifecycle. All entry points are required; cancel and free must
 * tolerate a NULL handle. encode/decode exchange owned rfc_frame values. */
typedef struct rfc_adapter {
    uint32_t abi_version; /* must equal RFC_CODEC_ABI_VERSION */
    rfc_format_id format;
    rfc_status (*probe)(void *ctx, const uint8_t *data, size_t len);
    /* Settings are the registry-validated rfc_setting_value list; adapters
     * never receive unvalidated settings. encode allocates out through
     * the supplied allocator; decode allocates the frame the same way. */
    rfc_status (*encode)(void *ctx, const rfc_frame *in,
                         const rfc_setting_value *settings, size_t setting_count,
                         const rfc_allocator *allocator, rfc_encoded *out);
    rfc_status (*decode)(void *ctx, const rfc_encoded *in,
                         const rfc_setting_value *settings, size_t setting_count,
                         const rfc_allocator *allocator, rfc_frame *out);
    rfc_status (*cancel)(void *ctx);
    void (*free_adapter)(void *ctx);
    void *ctx;
} rfc_adapter;

/* Validate a layout before allocation. On RFC_OK, *total_out (when not
 * NULL) receives stride_bytes * height. Never allocates. */
rfc_status rfc_layout_validate(const rfc_layout *layout, size_t *total_out);

/* Allocate an owned frame after validation. Refusals return before the
 * allocator is called. metadata.icc is copied when present. */
rfc_status rfc_frame_alloc(rfc_frame *frame, const rfc_layout *layout,
                           const rfc_metadata *metadata,
                           const rfc_allocator *allocator);
void rfc_frame_free(rfc_frame *frame);

/* Registry enumeration and queries. Never allocate. */
size_t rfc_registry_count(void);
const rfc_capability *rfc_registry_at(size_t index);
const rfc_capability *rfc_registry_find(rfc_format_id id);
const rfc_capability *rfc_registry_find_name(const char *name);

/* Validate setting values against a format. On refusal the status names
 * the cause and *reason_out (when not NULL) explains it in words. */
rfc_status rfc_registry_validate_settings(rfc_format_id id,
                                          const rfc_setting_value *values,
                                          size_t value_count,
                                          const char **reason_out);

/* Select a format for execution in a mode. Planned formats return
 * RFC_ERR_PLANNED; usable formats without an adapter return
 * RFC_ERR_NO_ADAPTER. */
rfc_status rfc_registry_select(rfc_format_id id, rfc_mode mode);

/* Register an adapter (one per format). The registry borrows the struct;
 * the caller keeps it alive until rfc_registry_unregister. */
rfc_status rfc_registry_register(const rfc_adapter *adapter);
rfc_status rfc_registry_unregister(rfc_format_id id);
const rfc_adapter *rfc_registry_adapter(rfc_format_id id);

const char *rfc_status_name(rfc_status status);
const char *rfc_sample_name(rfc_sample_format format);

#ifdef __cplusplus
}
#endif
#endif
