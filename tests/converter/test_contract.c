/* Contract tests for the converter pixel/registry ABI (D10 T01 section 1).
 * Built and run by scripts/check-converter.py --case contract. */
#include "rfc_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static size_t alloc_calls = 0;
static void *counting_alloc(void *ctx, size_t size) {
    (void)ctx; alloc_calls++; return malloc(size ? size : 1);
}
static void counting_free(void *ctx, void *ptr) { (void)ctx; free(ptr); }

static rfc_status fake_probe(void *ctx, const uint8_t *d, size_t n) {
    (void)ctx; (void)d; (void)n; return RFC_OK;
}
static rfc_status fake_encode(void *ctx, const rfc_frame *in,
                              const rfc_setting_value *s, size_t n,
                              const rfc_allocator *a, rfc_encoded *out) {
    (void)ctx; (void)in; (void)s; (void)n; (void)a; (void)out; return RFC_OK;
}
static rfc_status fake_decode(void *ctx, const rfc_encoded *in,
                              const rfc_setting_value *s, size_t n,
                              const rfc_allocator *a, rfc_frame *out) {
    (void)ctx; (void)in; (void)s; (void)n; (void)a; (void)out; return RFC_OK;
}
static rfc_status fake_cancel(void *ctx) { (void)ctx; return RFC_OK; }
static void fake_free(void *ctx) { (void)ctx; }

static void dump_registry(void) {
    for (size_t i = 0; i < rfc_registry_count(); i++) {
        const rfc_capability *c = rfc_registry_at(i);
        printf("%s|%s|%s|%s|%s|%d|%d|", c->name, c->provider, c->owner,
               c->extension_owner ? c->extension_owner : "-",
               c->availability == RFC_AVAILABILITY_USABLE ? "usable" : "planned",
               c->can_read, c->can_write);
        for (size_t j = 0; j < c->sample_count; j++) printf("%s,", rfc_sample_name(c->samples[j]));
        printf("|");
        for (size_t j = 0; j < c->setting_count; j++) {
            const rfc_setting_def *s = &c->settings[j];
            printf("%s:%s:%s:", s->id, s->type, s->default_value);
            for (size_t k = 0; k < s->allowed_count; k++) printf("%s,", s->allowed[k]);
            printf(";");
        }
        printf("|");
        for (size_t j = 0; j < c->rule_count; j++) {
            const rfc_combination_rule *r = &c->rules[j];
            printf("%s=%s&%s=%s;", r->setting_a, r->value_a,
                   r->setting_b ? r->setting_b : "-", r->value_b ? r->value_b : "-");
        }
        printf("\n");
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "dump") == 0) { dump_registry(); return 0; }
    rfc_allocator alloc = {counting_alloc, counting_free, NULL};

    /* Valid layout succeeds and reports the total. */
    rfc_layout good = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4, 3, 12};
    size_t total = 0;
    CHECK(rfc_layout_validate(&good, &total) == RFC_OK && total == 36);

    /* Refusals happen before allocation: the counting allocator must
     * record zero calls across every refusal below. */
    alloc_calls = 0;
    rfc_frame frame;
    rfc_layout bad_stride = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4, 3, 11};
    CHECK(rfc_frame_alloc(&frame, &bad_stride, NULL, &alloc) == RFC_ERR_INVALID_ARGUMENT);
    rfc_layout overflow = {RFC_SAMPLE_RGBA16, RFC_ALPHA_STRAIGHT, 10, 3, SIZE_MAX / 2};
    CHECK(rfc_frame_alloc(&frame, &overflow, NULL, &alloc) == RFC_ERR_OVERFLOW);
    rfc_layout bad_alpha_value = {RFC_SAMPLE_RGBA8, (rfc_alpha_mode)99, 2, 2, 8};
    CHECK(rfc_frame_alloc(&frame, &bad_alpha_value, NULL, &alloc) == RFC_ERR_UNSUPPORTED_ALPHA);
    rfc_layout bad_alpha = {RFC_SAMPLE_RGB8, RFC_ALPHA_STRAIGHT, 2, 2, 6};
    CHECK(rfc_frame_alloc(&frame, &bad_alpha, NULL, &alloc) == RFC_ERR_UNSUPPORTED_ALPHA);
    rfc_layout zero = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 0, 2, 0};
    CHECK(rfc_frame_alloc(&frame, &zero, NULL, &alloc) == RFC_ERR_INVALID_ARGUMENT);
    CHECK(alloc_calls == 0);

    /* A valid frame allocates, owns a copy of the ICC bytes, and frees. */
    uint8_t icc[4] = {1, 2, 3, 4};
    rfc_metadata meta = {icc, 4, RFC_ORIENTATION_NORMAL, 0};
    CHECK(rfc_frame_alloc(&frame, &good, &meta, &alloc) == RFC_OK);
    CHECK(frame.data_len == 36 && frame.metadata.icc && frame.metadata.icc != icc);
    CHECK(alloc_calls >= 1);
    rfc_frame_free(&frame);
    CHECK(frame.data == NULL);

    /* Registry: every vision format is listed; rasterfold is findable. */
    CHECK(rfc_registry_count() == 19);
    const rfc_capability *rf = rfc_registry_find_name("rasterfold");
    CHECK(rf && rf->id == RFC_FORMAT_RASTERFOLD && rf->can_read && rf->can_write);
    CHECK(rfc_registry_find(RFC_FORMAT_QOI) != NULL);
    CHECK(rfc_registry_find_name("no-such-format") == NULL);

    /* Planned formats are visible but cannot be selected for execution. */
    CHECK(rfc_registry_select(RFC_FORMAT_PNG, RFC_MODE_READ) == RFC_ERR_PLANNED);
    const char *reason = NULL;
    rfc_setting_value ok_vals[] = {{"metadata", "preserve"}, {"exactness", "pixel-exact"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_PNG, ok_vals, 2, &reason) == RFC_OK);

    /* Unsupported combinations fail with a reason, before any work. */
    rfc_setting_value strip_bytes[] = {{"metadata", "strip"}, {"exactness", "byte-identical"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_PNG, strip_bytes, 2, &reason) ==
          RFC_ERR_UNSUPPORTED_COMBINATION && reason != NULL);
    rfc_setting_value first_frame[] = {{"frames", "first-only"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_GIF, first_frame, 1, &reason) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);
    rfc_setting_value null_id[] = {{NULL, "preserve"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_PNG, null_id, 1, &reason) ==
          RFC_ERR_INVALID_ARGUMENT);
    rfc_setting_value unknown[] = {{"bogus", "1"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_PNG, unknown, 1, &reason) ==
          RFC_ERR_UNKNOWN_SETTING);
    rfc_setting_value bad_value[] = {{"metadata", "sometimes"}};
    CHECK(rfc_registry_validate_settings(RFC_FORMAT_PNG, bad_value, 1, &reason) ==
          RFC_ERR_SETTING_VALUE);
    /* Import-only formats refuse write selection. */
    CHECK(rfc_registry_select(RFC_FORMAT_CAMERA_RAW, RFC_MODE_WRITE) ==
          RFC_ERR_UNSUPPORTED_COMBINATION);

    /* Adapter lifecycle: ABI and entry points are enforced; a registered
     * adapter is findable and a duplicate is refused. */
    rfc_adapter fake = {RFC_CODEC_ABI_VERSION, RFC_FORMAT_QOI, fake_probe,
                        fake_encode, fake_decode, fake_cancel, fake_free, NULL};
    CHECK(rfc_registry_register(&fake) == RFC_OK);
    CHECK(rfc_registry_adapter(RFC_FORMAT_QOI) == &fake);
    CHECK(rfc_registry_register(&fake) == RFC_ERR_DUPLICATE);
    rfc_adapter bad_abi = fake; bad_abi.abi_version = 999; bad_abi.format = RFC_FORMAT_BMP;
    CHECK(rfc_registry_register(&bad_abi) == RFC_ERR_ABI_VERSION);
    CHECK(rfc_registry_unregister(RFC_FORMAT_QOI) == RFC_OK);
    CHECK(rfc_registry_adapter(RFC_FORMAT_QOI) == NULL);

    if (failures) { fprintf(stderr, "%d contract check(s) failed\n", failures); return 1; }
    printf("contract: all checks passed\n");
    return 0;
}
