#include "job_internal.h"
#include "codec_internal.h"

#define RFC_ALLOC_MAGIC 0x52464247544D414Cull

typedef union rfc_alloc_hdr {
    struct {
        uint64_t magic;
        size_t size;
    } info;
    max_align_t align_dummy;
} rfc_alloc_hdr;

struct rfc_budget {
    rfc_mutex lock;
    uint64_t limit_bytes;
    uint64_t current_allocated_bytes;
    uint64_t peak_allocated_bytes;
    uint64_t allocation_count;
    uint64_t rejected_count;
};

static void *budget_alloc_cb(void *ctx, size_t size) {
    return rfc_budget_alloc((rfc_budget *)ctx, size);
}

static void budget_free_cb(void *ctx, void *ptr) {
    rfc_budget_free((rfc_budget *)ctx, ptr);
}

static _Thread_local rfc_job *g_tl_decode_job = NULL;

static void *job_alloc_cb(void *ctx, size_t size);
static void job_free_cb(void *ctx, void *ptr);
static rfc_status job_frame_alloc_guard(const rfc_allocator *allocator,
                                        const rfc_layout *layout,
                                        const rfc_metadata *metadata);

rfc_budget *rfc_budget_create(uint64_t limit_bytes) {
    rfc_budget *b = (rfc_budget *)calloc(1u, sizeof(*b));
    if (!b) {
        return NULL;
    }
    if (!rfc_mutex_init(&b->lock)) {
        free(b);
        return NULL;
    }
    b->limit_bytes = limit_bytes;
    return b;
}

void rfc_budget_destroy(rfc_budget *budget) {
    if (!budget) {
        return;
    }
    rfc_mutex_destroy(&budget->lock);
    free(budget);
}

void rfc_budget_stats_get(const rfc_budget *budget, rfc_budget_stats *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!budget) {
        return;
    }
    rfc_budget *mutable_b = (rfc_budget *)budget;
    rfc_mutex_lock(&mutable_b->lock);
    out->limit_bytes = mutable_b->limit_bytes;
    out->current_allocated_bytes = mutable_b->current_allocated_bytes;
    out->peak_allocated_bytes = mutable_b->peak_allocated_bytes;
    out->allocation_count = mutable_b->allocation_count;
    out->rejected_count = mutable_b->rejected_count;
    rfc_mutex_unlock(&mutable_b->lock);
}

void rfc_budget_allocator(rfc_budget *budget, rfc_allocator *out_alloc) {
    if (!out_alloc) {
        return;
    }
    out_alloc->alloc = budget_alloc_cb;
    out_alloc->free = budget_free_cb;
    out_alloc->ctx = budget;
}

void *rfc_budget_alloc(rfc_budget *budget, size_t size) {
    if (!budget) {
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(rfc_alloc_hdr) - 1u) {
        rfc_mutex_lock(&budget->lock);
        budget->rejected_count++;
        rfc_mutex_unlock(&budget->lock);
        return NULL;
    }

    rfc_mutex_lock(&budget->lock);
    if ((uint64_t)size > budget->limit_bytes - budget->current_allocated_bytes) {
        budget->rejected_count++;
        rfc_mutex_unlock(&budget->lock);
        return NULL;
    }
    budget->current_allocated_bytes += (uint64_t)size;
    rfc_mutex_unlock(&budget->lock);

    size_t raw_size = sizeof(rfc_alloc_hdr) + (size ? size : 1u);
    rfc_alloc_hdr *hdr = (rfc_alloc_hdr *)malloc(raw_size);
    if (!hdr) {
        rfc_mutex_lock(&budget->lock);
        budget->current_allocated_bytes -= (uint64_t)size;
        budget->rejected_count++;
        rfc_mutex_unlock(&budget->lock);
        return NULL;
    }

    rfc_mutex_lock(&budget->lock);
    if (budget->current_allocated_bytes > budget->peak_allocated_bytes) {
        budget->peak_allocated_bytes = budget->current_allocated_bytes;
    }
    budget->allocation_count++;
    rfc_mutex_unlock(&budget->lock);

    hdr->info.magic = RFC_ALLOC_MAGIC;
    hdr->info.size = size;
    return (void *)(hdr + 1);
}

void *rfc_budget_realloc(rfc_budget *budget, void *ptr, size_t new_size) {
    if (!budget) {
        return NULL;
    }
    if (!ptr) {
        return rfc_budget_alloc(budget, new_size);
    }
    if (new_size == 0) {
        rfc_budget_free(budget, ptr);
        return NULL;
    }
    if (new_size > SIZE_MAX - sizeof(rfc_alloc_hdr) - 1u) {
        rfc_mutex_lock(&budget->lock);
        budget->rejected_count++;
        rfc_mutex_unlock(&budget->lock);
        return NULL;
    }

    rfc_alloc_hdr *hdr = ((rfc_alloc_hdr *)ptr) - 1;
    if (hdr->info.magic != RFC_ALLOC_MAGIC) {
        return NULL;
    }
    size_t old_size = hdr->info.size;

    if (new_size > old_size) {
        size_t delta = new_size - old_size;
        rfc_mutex_lock(&budget->lock);
        if ((uint64_t)delta > budget->limit_bytes - budget->current_allocated_bytes) {
            budget->rejected_count++;
            rfc_mutex_unlock(&budget->lock);
            return NULL;
        }
        budget->current_allocated_bytes += (uint64_t)delta;
        rfc_mutex_unlock(&budget->lock);

        rfc_alloc_hdr *new_hdr =
            (rfc_alloc_hdr *)realloc(hdr, sizeof(rfc_alloc_hdr) + new_size);
        if (!new_hdr) {
            rfc_mutex_lock(&budget->lock);
            budget->current_allocated_bytes -= (uint64_t)delta;
            budget->rejected_count++;
            rfc_mutex_unlock(&budget->lock);
            return NULL;
        }
        new_hdr->info.size = new_size;
        rfc_mutex_lock(&budget->lock);
        if (budget->current_allocated_bytes > budget->peak_allocated_bytes) {
            budget->peak_allocated_bytes = budget->current_allocated_bytes;
        }
        budget->allocation_count++;
        rfc_mutex_unlock(&budget->lock);
        return (void *)(new_hdr + 1);
    }

    size_t delta = old_size - new_size;
    rfc_alloc_hdr *new_hdr =
        (rfc_alloc_hdr *)realloc(hdr, sizeof(rfc_alloc_hdr) + new_size);
    if (!new_hdr) {
        return ptr;
    }
    new_hdr->info.size = new_size;
    rfc_mutex_lock(&budget->lock);
    budget->current_allocated_bytes -= (uint64_t)delta;
    budget->allocation_count++;
    rfc_mutex_unlock(&budget->lock);
    return (void *)(new_hdr + 1);
}

void rfc_budget_free(rfc_budget *budget, void *ptr) {
    if (!ptr) {
        return;
    }
    rfc_alloc_hdr *hdr = ((rfc_alloc_hdr *)ptr) - 1;
    if (hdr->info.magic != RFC_ALLOC_MAGIC) {
        return;
    }
    size_t sz = hdr->info.size;
    hdr->info.magic = 0;
    hdr->info.size = 0;
    if (budget) {
        rfc_mutex_lock(&budget->lock);
        if (budget->current_allocated_bytes >= (uint64_t)sz) {
            budget->current_allocated_bytes -= (uint64_t)sz;
        } else {
            budget->current_allocated_bytes = 0;
        }
        rfc_mutex_unlock(&budget->lock);
    }
    free(hdr);
}

rfc_status rfc_budget_reserve(rfc_budget *budget, size_t size) {
    if (!budget) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_mutex_lock(&budget->lock);
    if (budget->current_allocated_bytes > budget->limit_bytes ||
        (uint64_t)size > budget->limit_bytes - budget->current_allocated_bytes) {
        budget->rejected_count++;
        rfc_mutex_unlock(&budget->lock);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    budget->current_allocated_bytes += (uint64_t)size;
    if (budget->current_allocated_bytes > budget->peak_allocated_bytes) {
        budget->peak_allocated_bytes = budget->current_allocated_bytes;
    }
    budget->allocation_count++;
    rfc_mutex_unlock(&budget->lock);
    return RFC_OK;
}

void rfc_budget_release(rfc_budget *budget, size_t size) {
    if (!budget) {
        return;
    }
    rfc_mutex_lock(&budget->lock);
    if (budget->current_allocated_bytes >= (uint64_t)size) {
        budget->current_allocated_bytes -= (uint64_t)size;
    } else {
        budget->current_allocated_bytes = 0;
    }
    rfc_mutex_unlock(&budget->lock);
}

void *rfc_budget_jxl_alloc(void *opaque, size_t size) {
    return rfc_budget_alloc((rfc_budget *)opaque, size);
}

void rfc_budget_jxl_free(void *opaque, void *address) {
    rfc_budget_free((rfc_budget *)opaque, address);
}

void *rfc_budget_zlib_alloc(void *opaque, unsigned int items, unsigned int size) {
    rfc_budget *b = (rfc_budget *)opaque;
    if (!b) {
        return NULL;
    }
    if (items != 0 && (size_t)size > SIZE_MAX / (size_t)items) {
        rfc_mutex_lock(&b->lock);
        b->rejected_count++;
        rfc_mutex_unlock(&b->lock);
        return NULL;
    }
    return rfc_budget_alloc(b, (size_t)items * (size_t)size);
}

void rfc_budget_zlib_free(void *opaque, void *address) {
    rfc_budget_free((rfc_budget *)opaque, address);
}

static void (*g_progress_hook)(rfc_job_id job_id, int phase, void *user) = NULL;
static void *g_progress_hook_user = NULL;

void rfc_job_internal_set_progress_hook(
    void (*fn)(rfc_job_id job_id, int phase, void *user),
    void *user) {
    g_progress_hook = fn;
    g_progress_hook_user = user;
}

static int add_u64_job(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static int mul_u64_job(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

void rfc_job_internal_set_detail(char dst[RFC_JOB_DETAIL_MAX], const char *src) {
    if (!dst) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= RFC_JOB_DETAIL_MAX) {
        len = RFC_JOB_DETAIL_MAX - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int format_defines_setting(const rfc_capability *cap, const char *id) {
    if (!cap || !id) {
        return 0;
    }
    for (size_t i = 0; i < cap->setting_count; i++) {
        if (cap->settings[i].id && strcmp(cap->settings[i].id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static size_t filter_format_settings(const rfc_capability *cap,
                                     const rfc_setting_value *settings,
                                     size_t setting_count,
                                     rfc_setting_value *out_vals,
                                     size_t max_out,
                                     int include_exactness) {
    size_t n = 0;
    if (!cap || !settings || !out_vals || max_out == 0) {
        return 0u;
    }
    for (size_t i = 0; i < setting_count && n < max_out; i++) {
        if ((include_exactness || strcmp(settings[i].id, "exactness") != 0) &&
            format_defines_setting(cap, settings[i].id)) {
            out_vals[n++] = settings[i];
        }
    }
    return n;
}

#define RFC_MAX_FORMAT_SETTINGS 64u

rfc_status rfc_job_internal_validate_settings(
    rfc_format_id source_format,
    rfc_format_id target_format,
    const rfc_setting_value *settings,
    size_t setting_count,
    const rfc_pipeline_stage_ops *stage_ops,
    const char **out_detail) {
    if (out_detail) {
        *out_detail = NULL;
    }
    const rfc_capability *src_cap = rfc_registry_find(source_format);
    if (!src_cap) {
        if (out_detail) {
            *out_detail = "unknown source format";
        }
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    const rfc_capability *dst_cap = rfc_registry_find(target_format);
    if (!dst_cap) {
        if (out_detail) {
            *out_detail = "unknown target format";
        }
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if ((!stage_ops || !stage_ops->on_import) && !src_cap->can_read) {
        if (out_detail) {
            *out_detail = "source format does not support decoding";
        }
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if ((!stage_ops || !stage_ops->on_encode) && !dst_cap->can_write) {
        if (out_detail) {
            *out_detail = "target format does not support encoding";
        }
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (setting_count > 0u && !settings) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_exactness_class eff_exact = RFC_EXACT_PIXEL_EXACT;
    rfc_metadata_policy eff_policy = RFC_META_POLICY_PRESERVE;
    if (rfc_job_internal_resolve_exactness(target_format, settings,
                                           setting_count, &eff_exact,
                                           &eff_policy) == RFC_OK) {
        /* JPEG output is never a lossy re-encode of pixels (D10 T02 s5). */
        if (target_format == RFC_FORMAT_JPEG &&
            (!stage_ops || !stage_ops->on_encode) &&
            !(source_format == RFC_FORMAT_JPEG &&
              (eff_exact == RFC_EXACT_COEFFICIENT_EXACT ||
               eff_exact == RFC_EXACT_BYTE_IDENTICAL))) {
            if (out_detail) {
                *out_detail =
                    "JPEG output is limited to lossless entropy optimization "
                    "(exactness=coefficient-exact) or a byte-identical copy of "
                    "a JPEG source; RGB to baseline JPEG is lossy";
            }
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
        if (eff_exact == RFC_EXACT_COEFFICIENT_EXACT &&
            source_format == RFC_FORMAT_JPEG &&
            target_format != RFC_FORMAT_JPEG &&
            target_format != RFC_FORMAT_JPEG_XL &&
            (!stage_ops || (!stage_ops->on_import && !stage_ops->on_encode))) {
            if (out_detail) {
                *out_detail =
                    "coefficient-exact needs a target that stores JPEG DCT "
                    "coefficients (JPEG or JPEG XL)";
            }
            return RFC_ERR_UNSUPPORTED_COMBINATION;
        }
    }
    if (setting_count == 0) {
        return RFC_OK;
    }
    for (size_t i = 0; i < setting_count; i++) {
        if (!settings[i].id || !settings[i].value) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        for (size_t k = 0; k < i; k++) {
            if (strcmp(settings[k].id, settings[i].id) == 0) {
                if (out_detail) {
                    *out_detail = "duplicate setting; deliver one value per setting";
                }
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
        if (!format_defines_setting(src_cap, settings[i].id) &&
            !format_defines_setting(dst_cap, settings[i].id)) {
            if (out_detail) {
                *out_detail = "unknown setting for source and target formats";
            }
            return RFC_ERR_UNKNOWN_SETTING;
        }
    }
    if (setting_count > RFC_MAX_FORMAT_SETTINGS) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_setting_value src_vals[RFC_MAX_FORMAT_SETTINGS];
    size_t src_n = filter_format_settings(src_cap, settings, setting_count,
                                          src_vals, RFC_MAX_FORMAT_SETTINGS, 0);
    rfc_status st = rfc_registry_validate_settings(
        source_format, src_n > 0 ? src_vals : NULL, src_n, out_detail);
    if (st != RFC_OK) {
        return st;
    }

    rfc_setting_value dst_vals[RFC_MAX_FORMAT_SETTINGS];
    size_t dst_n = filter_format_settings(dst_cap, settings, setting_count,
                                          dst_vals, RFC_MAX_FORMAT_SETTINGS, 1);
    st = rfc_registry_validate_settings(
        target_format, dst_n > 0 ? dst_vals : NULL, dst_n, out_detail);
    if (st != RFC_OK) {
        return st;
    }

    for (size_t i = 0; i < setting_count; i++) {
        if (strcmp(settings[i].id, "exactness") == 0) {
            if (strcmp(settings[i].value, "byte-identical") == 0 &&
                source_format != target_format) {
                if (out_detail) {
                    *out_detail =
                        "byte-identical requires matching source and target formats";
                }
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
            if (strcmp(settings[i].value, "coefficient-exact") == 0) {
                if (source_format != RFC_FORMAT_JPEG &&
                    target_format != RFC_FORMAT_JPEG) {
                    if (out_detail) {
                        *out_detail =
                            "coefficient-exact requires a JPEG source or target";
                    }
                    return RFC_ERR_UNSUPPORTED_COMBINATION;
                }
                if ((source_format == RFC_FORMAT_JPEG_XL ||
                     target_format == RFC_FORMAT_JPEG_XL) &&
                    (!stage_ops ||
                     (!stage_ops->on_import && !stage_ops->on_encode))) {
                    if (out_detail) {
                        *out_detail =
                            "coefficient-exact with JPEG XL is not implemented";
                    }
                    return RFC_ERR_UNSUPPORTED_COMBINATION;
                }
            }
            if (strcmp(settings[i].value, "original-byte-reconstruction") == 0 &&
                (source_format != RFC_FORMAT_JPEG ||
                 target_format != RFC_FORMAT_JPEG_XL)) {
                if (out_detail) {
                    *out_detail = "original-byte-reconstruction requires JPEG source and JPEG XL target";
                }
                return RFC_ERR_UNSUPPORTED_COMBINATION;
            }
        }
    }
    return RFC_OK;
}

rfc_status rfc_job_validate_combination(rfc_format_id source_format,
                                        rfc_format_id target_format,
                                        const rfc_setting_value *settings,
                                        size_t setting_count,
                                        const char **out_reason) {
    const char *reason = NULL;
    rfc_status st = rfc_job_internal_validate_settings(
        source_format, target_format, settings, setting_count, NULL, &reason);
    if (out_reason) {
        *out_reason = (st != RFC_OK) ? (reason ? reason : rfc_status_name(st))
                                     : NULL;
    }
    return st;
}

static char *dup_cstr_counted(const char *s, rfc_scheduler *alloc_counter) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1u;
    rfc_scheduler_internal_note_submit_alloc(alloc_counter);
    char *copy = (char *)malloc(n);
    if (copy) {
        memcpy(copy, s, n);
    }
    return copy;
}

static rfc_io_reader_box *rfc_io_reader_box_create(const rfc_io_reader *reader,
                                                   rfc_scheduler *alloc_counter) {
    if (!reader) {
        return NULL;
    }
    rfc_scheduler_internal_note_submit_alloc(alloc_counter);
    rfc_io_reader_box *box = (rfc_io_reader_box *)calloc(1u, sizeof(*box));
    if (!box) {
        return NULL;
    }
    if (!rfc_mutex_init(&box->lock)) {
        free(box);
        return NULL;
    }
    atomic_init(&box->refs, 1u);
    box->ownership_transferred = 0;
    box->reader = *reader;
    return box;
}

static void rfc_io_reader_box_release(rfc_io_reader_box *box) {
    if (!box) {
        return;
    }
    if (atomic_fetch_sub_explicit(&box->refs, 1u, memory_order_acq_rel) == 1u) {
        if (box->ownership_transferred && box->reader.close_fn) {
            box->reader.close_fn(box->reader.ctx);
        }
        rfc_mutex_destroy(&box->lock);
        free(box);
    }
}

static rfc_io_publisher_box *rfc_io_publisher_box_create(
    const rfc_io_publisher *publisher,
    rfc_scheduler *alloc_counter) {
    if (!publisher) {
        return NULL;
    }
    rfc_scheduler_internal_note_submit_alloc(alloc_counter);
    rfc_io_publisher_box *box =
        (rfc_io_publisher_box *)calloc(1u, sizeof(*box));
    if (!box) {
        return NULL;
    }
    if (!rfc_mutex_init(&box->lock)) {
        free(box);
        return NULL;
    }
    atomic_init(&box->refs, 1u);
    box->ownership_transferred = 0;
    box->publisher = *publisher;
    return box;
}

static void rfc_io_publisher_box_release(rfc_io_publisher_box *box) {
    if (!box) {
        return;
    }
    if (atomic_fetch_sub_explicit(&box->refs, 1u, memory_order_acq_rel) == 1u) {
        if (box->ownership_transferred && box->publisher.release_ctx) {
            box->publisher.release_ctx(box->publisher.ctx);
        }
        rfc_mutex_destroy(&box->lock);
        free(box);
    }
}

rfc_status rfc_job_internal_compute_retained_bytes(
    const uint8_t *src_bytes,
    size_t src_size,
    const rfc_metadata *src_meta,
    const rfc_setting_value *settings,
    size_t setting_count,
    const rfc_io_source *io_source,
    const rfc_io_sink *io_sink,
    uint64_t *out_bytes) {
    if (!out_bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t total = 0;
    if (src_size > 0 && src_bytes != NULL) {
        if (!add_u64_job(total, (uint64_t)src_size, &total)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }
    if (src_meta && src_meta->icc_len > 0 && src_meta->icc != NULL) {
        if (!add_u64_job(total, (uint64_t)src_meta->icc_len, &total)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }
    if (src_meta && src_meta->exif_len > 0 && src_meta->exif != NULL) {
        if (!add_u64_job(total, (uint64_t)src_meta->exif_len, &total)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }
    if (src_meta && src_meta->xmp_len > 0 && src_meta->xmp != NULL) {
        if (!add_u64_job(total, (uint64_t)src_meta->xmp_len, &total)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }
    if (setting_count > 0 && settings != NULL) {
        uint64_t arr_bytes = 0;
        if (!mul_u64_job((uint64_t)setting_count,
                         (uint64_t)sizeof(rfc_setting_value), &arr_bytes) ||
            !add_u64_job(total, arr_bytes, &total)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        for (size_t i = 0; i < setting_count; i++) {
            if (settings[i].id) {
                uint64_t sl = (uint64_t)strlen(settings[i].id) + 1ull;
                if (!add_u64_job(total, sl, &total)) {
                    return RFC_ERR_LIMIT_EXCEEDED;
                }
            }
            if (settings[i].value) {
                uint64_t sl = (uint64_t)strlen(settings[i].value) + 1ull;
                if (!add_u64_job(total, sl, &total)) {
                    return RFC_ERR_LIMIT_EXCEEDED;
                }
            }
        }
    }
    if (io_source) {
        if (io_source->path_utf8) {
            uint64_t sl = (uint64_t)strlen(io_source->path_utf8) + 1ull;
            if (!add_u64_job(total, sl, &total)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
        }
        if (io_source->virtual_name_utf8) {
            uint64_t sl = (uint64_t)strlen(io_source->virtual_name_utf8) + 1ull;
            if (!add_u64_job(total, sl, &total)) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
        }
    }
    if (io_sink && io_sink->path_utf8) {
        uint64_t sl = (uint64_t)strlen(io_sink->path_utf8) + 1ull;
        if (!add_u64_job(total, sl, &total)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
    }
    *out_bytes = total;
    return RFC_OK;
}

static void inputs_free_contents(rfc_job_inputs *in) {
    if (!in) {
        return;
    }
    rfc_scheduler *sched = in->sched;
    uint64_t charged = in->charged_bytes;
    free(in->source_bytes);
    free(in->owned_icc);
    free(in->owned_exif);
    free(in->owned_xmp);
    if (in->settings) {
        for (size_t i = 0; i < in->setting_count; i++) {
            free((void *)in->settings[i].id);
            free((void *)in->settings[i].value);
        }
        free(in->settings);
    }
    free(in->io_source_path);
    free(in->io_source_virtual_name);
    free(in->io_sink_path);
    rfc_io_reader_box_release(in->io_reader_box);
    rfc_io_publisher_box_release(in->io_publisher_box);
    free(in);
    if (sched && charged > 0) {
        rfc_scheduler_internal_unreserve_retained(sched, charged);
    }
    if (sched) {
        rfc_scheduler_internal_release(sched);
    }
}

rfc_job_inputs *rfc_job_inputs_create(rfc_scheduler *sched,
                                      uint64_t *reserved_bytes_inout,
                                      const uint8_t *src_bytes,
                                      size_t src_size,
                                      const rfc_layout *src_layout,
                                      const rfc_metadata *src_meta,
                                      const rfc_setting_value *settings,
                                      size_t setting_count,
                                      uint32_t acknowledged_unsupported_metadata,
                                      const rfc_io_source *io_source,
                                      const rfc_io_sink *io_sink,
                                      uint64_t effective_source_size) {
    uint64_t retained = 0;
    if (rfc_job_internal_compute_retained_bytes(src_bytes, src_size, src_meta,
                                                settings, setting_count,
                                                io_source, io_sink,
                                                &retained) != RFC_OK) {
        return NULL;
    }

    rfc_scheduler_internal_note_submit_alloc(sched);
    rfc_job_inputs *in = (rfc_job_inputs *)calloc(1u, sizeof(*in));
    if (!in) {
        return NULL;
    }
    atomic_init(&in->refs, 1u);
    if (sched) {
        in->sched = sched;
        rfc_scheduler_internal_retain(sched);
        if (reserved_bytes_inout && *reserved_bytes_inout > 0) {
            in->charged_bytes = *reserved_bytes_inout;
            *reserved_bytes_inout = 0;
        }
    }
    in->source_size = src_size;
    in->setting_count = setting_count;
    in->acknowledged_unsupported_metadata = acknowledged_unsupported_metadata;
    in->retained_bytes = retained;
    in->effective_source_size =
        effective_source_size > 0u ? effective_source_size : (uint64_t)src_size;

    if (src_size > 0 && src_bytes != NULL) {
        rfc_scheduler_internal_note_submit_alloc(sched);
        in->source_bytes = (uint8_t *)malloc(src_size);
        if (!in->source_bytes) {
            inputs_free_contents(in);
            return NULL;
        }
        memcpy(in->source_bytes, src_bytes, src_size);
    }

    if (src_layout) {
        if (rfc_layout_normalize_stride(src_layout, &in->source_layout) !=
            RFC_OK) {
            inputs_free_contents(in);
            return NULL;
        }
        in->has_layout = 1;
    }

    if (src_meta) {
        in->source_meta = *src_meta;
        in->source_meta.icc = NULL;
        in->source_meta.exif = NULL;
        in->source_meta.xmp = NULL;
        in->has_meta = 1;
        if (src_meta->icc_len > 0 && src_meta->icc != NULL) {
            rfc_scheduler_internal_note_submit_alloc(sched);
            in->owned_icc = (uint8_t *)malloc(src_meta->icc_len);
            if (!in->owned_icc) {
                inputs_free_contents(in);
                return NULL;
            }
            memcpy(in->owned_icc, src_meta->icc, src_meta->icc_len);
            in->source_meta.icc = in->owned_icc;
        }
        if (src_meta->exif_len > 0 && src_meta->exif != NULL) {
            rfc_scheduler_internal_note_submit_alloc(sched);
            in->owned_exif = (uint8_t *)malloc(src_meta->exif_len);
            if (!in->owned_exif) {
                inputs_free_contents(in);
                return NULL;
            }
            memcpy(in->owned_exif, src_meta->exif, src_meta->exif_len);
            in->source_meta.exif = in->owned_exif;
        }
        if (src_meta->xmp_len > 0 && src_meta->xmp != NULL) {
            rfc_scheduler_internal_note_submit_alloc(sched);
            in->owned_xmp = (uint8_t *)malloc(src_meta->xmp_len);
            if (!in->owned_xmp) {
                inputs_free_contents(in);
                return NULL;
            }
            memcpy(in->owned_xmp, src_meta->xmp, src_meta->xmp_len);
            in->source_meta.xmp = in->owned_xmp;
        }
    }

    if (setting_count > 0 && settings != NULL) {
        rfc_scheduler_internal_note_submit_alloc(sched);
        in->settings =
            (rfc_setting_value *)calloc(setting_count, sizeof(*in->settings));
        if (!in->settings) {
            inputs_free_contents(in);
            return NULL;
        }
        for (size_t i = 0; i < setting_count; i++) {
            in->settings[i].id =
                settings[i].id ? dup_cstr_counted(settings[i].id, sched)
                               : NULL;
            in->settings[i].value =
                settings[i].value
                    ? dup_cstr_counted(settings[i].value, sched)
                    : NULL;
            if ((settings[i].id && !in->settings[i].id) ||
                (settings[i].value && !in->settings[i].value)) {
                inputs_free_contents(in);
                return NULL;
            }
        }
    }

    if (io_source) {
        in->has_io_source = 1;
        in->io_import_flags = io_source->import_flags;
        if (io_source->path_utf8) {
            in->io_source_path = dup_cstr_counted(io_source->path_utf8, sched);
            if (!in->io_source_path) {
                inputs_free_contents(in);
                return NULL;
            }
        }
        if (io_source->virtual_name_utf8) {
            in->io_source_virtual_name =
                dup_cstr_counted(io_source->virtual_name_utf8, sched);
            if (!in->io_source_virtual_name) {
                inputs_free_contents(in);
                return NULL;
            }
        }
        if (io_source->reader) {
            in->io_reader_box = rfc_io_reader_box_create(io_source->reader, sched);
            if (!in->io_reader_box) {
                inputs_free_contents(in);
                return NULL;
            }
        }
    }

    if (io_sink) {
        in->has_io_sink = 1;
        in->io_export_flags = io_sink->export_flags;
        if (io_sink->path_utf8) {
            in->io_sink_path = dup_cstr_counted(io_sink->path_utf8, sched);
            if (!in->io_sink_path) {
                inputs_free_contents(in);
                return NULL;
            }
        }
        if (io_sink->publisher) {
            in->io_publisher_box =
                rfc_io_publisher_box_create(io_sink->publisher, sched);
            if (!in->io_publisher_box) {
                inputs_free_contents(in);
                return NULL;
            }
        }
    }

    return in;
}

void rfc_job_inputs_arm_io_ownership(rfc_job_inputs *inputs) {
    if (!inputs) {
        return;
    }
    if (inputs->io_reader_box) {
        inputs->io_reader_box->ownership_transferred = 1;
    }
    if (inputs->io_publisher_box) {
        inputs->io_publisher_box->ownership_transferred = 1;
    }
}

void rfc_job_inputs_retain(rfc_job_inputs *inputs) {
    if (inputs) {
        atomic_fetch_add_explicit(&inputs->refs, 1u, memory_order_relaxed);
    }
}

void rfc_job_inputs_release(rfc_job_inputs *inputs) {
    if (!inputs) {
        return;
    }
    if (atomic_fetch_sub_explicit(&inputs->refs, 1u, memory_order_acq_rel) ==
        1u) {
        inputs_free_contents(inputs);
    }
}

rfc_slot_state *rfc_slot_state_create(void) {
    rfc_slot_state *st = (rfc_slot_state *)calloc(1u, sizeof(*st));
    if (!st) {
        return NULL;
    }
    if (!rfc_mutex_init(&st->lock)) {
        free(st);
        return NULL;
    }
    atomic_init(&st->refs, 1u);
    st->current_job_id = RFC_JOB_ID_NONE;
    return st;
}

void rfc_slot_state_retain(rfc_slot_state *state) {
    if (state) {
        atomic_fetch_add_explicit(&state->refs, 1u, memory_order_relaxed);
    }
}

void rfc_slot_state_release(rfc_slot_state *state) {
    if (!state) {
        return;
    }
    if (atomic_fetch_sub_explicit(&state->refs, 1u, memory_order_acq_rel) ==
        1u) {
        rfc_mutex_destroy(&state->lock);
        free(state);
    }
}

rfc_job_id rfc_slot_state_get_id(rfc_slot_state *state) {
    if (!state) {
        return RFC_JOB_ID_NONE;
    }
    rfc_mutex_lock(&state->lock);
    rfc_job_id id = state->current_job_id;
    rfc_mutex_unlock(&state->lock);
    return id;
}

void rfc_slot_state_set_id(rfc_slot_state *state, rfc_job_id id) {
    if (!state) {
        return;
    }
    rfc_mutex_lock(&state->lock);
    state->current_job_id = id;
    rfc_mutex_unlock(&state->lock);
}

rfc_job_slot *rfc_job_slot_create(void) {
    rfc_job_slot *slot = (rfc_job_slot *)calloc(1u, sizeof(*slot));
    if (!slot) {
        return NULL;
    }
    if (!rfc_mutex_init(&slot->lock)) {
        free(slot);
        return NULL;
    }
    slot->state = rfc_slot_state_create();
    if (!slot->state) {
        rfc_mutex_destroy(&slot->lock);
        free(slot);
        return NULL;
    }
    return slot;
}

void rfc_job_slot_destroy(rfc_job_slot *slot) {
    if (!slot) {
        return;
    }
    rfc_mutex_lock(&slot->lock);
    rfc_job *cj = slot->current_job;
    slot->current_job = NULL;
    rfc_job_inputs *in = slot->inputs;
    slot->inputs = NULL;
    rfc_user_ctx_box *box = slot->user_ctx_box;
    slot->user_ctx_box = NULL;
    rfc_slot_state *st = slot->state;
    slot->state = NULL;
    rfc_mutex_unlock(&slot->lock);

    if (cj) {
        rfc_job_release(cj);
    }
    rfc_job_inputs_release(in);
    rfc_user_ctx_box_release(box);
    rfc_slot_state_release(st);
    rfc_mutex_destroy(&slot->lock);
    free(slot);
}

rfc_job_id rfc_job_slot_current_id(const rfc_job_slot *slot) {
    if (!slot || !slot->state) {
        return RFC_JOB_ID_NONE;
    }
    return rfc_slot_state_get_id(slot->state);
}

rfc_job *rfc_job_internal_create(void) {
    rfc_internal_set_frame_alloc_guard(job_frame_alloc_guard);
    rfc_job *job = (rfc_job *)calloc(1u, sizeof(*job));
    if (!job) {
        return NULL;
    }
    if (!rfc_mutex_init(&job->cb_lock)) {
        free(job);
        return NULL;
    }
    if (!rfc_mutex_init(&job->lock)) {
        rfc_mutex_destroy(&job->cb_lock);
        free(job);
        return NULL;
    }
    if (!rfc_cond_init(&job->done_cond)) {
        rfc_mutex_destroy(&job->lock);
        rfc_mutex_destroy(&job->cb_lock);
        free(job);
        return NULL;
    }
    atomic_init(&job->refs, 1u);
    job->state = RFC_JOB_QUEUED;
    job->stage = RFC_STAGE_PREFLIGHT;
    job->status = RFC_OK;
    return job;
}

static void job_free_cb(void *ctx, void *ptr) {
    rfc_job *job = (rfc_job *)ctx;
    rfc_budget_free(job ? job->budget : NULL, ptr);
}

static void *job_alloc_cb(void *ctx, size_t size) {
    rfc_job *job = (rfc_job *)ctx;
    if (!job || !job->budget) {
        return NULL;
    }
    return rfc_budget_alloc(job->budget, size);
}

void rfc_job_internal_bind_budget(rfc_job *job, rfc_budget *budget) {
    if (!job) {
        return;
    }
    rfc_budget_allocator(budget, &job->frame_allocator);
    job->allocator.alloc = job_alloc_cb;
    job->allocator.free = job_free_cb;
    job->allocator.ctx = job;
}

void rfc_job_retain(rfc_job *job) {
    if (job) {
        atomic_fetch_add_explicit(&job->refs, 1u, memory_order_relaxed);
    }
}

void rfc_job_release(rfc_job *job) {
    if (!job) {
        return;
    }
    if (atomic_fetch_sub_explicit(&job->refs, 1u, memory_order_acq_rel) == 1u) {
        rfc_job_internal_release_stage_outputs(job);
        rfc_job_release_partials(job);
        rfc_job_inputs_release(job->inputs);
        rfc_slot_state_release(job->slot_state);
        rfc_user_ctx_box_release(job->user_ctx_box);
        rfc_budget_destroy(job->budget);
        rfc_scheduler_internal_release(job->sched);
        rfc_cond_destroy(&job->done_cond);
        rfc_mutex_destroy(&job->lock);
        rfc_mutex_destroy(&job->cb_lock);
        free(job);
    }
}

rfc_job_id rfc_job_id_get(const rfc_job *job) {
    return job ? job->job_id : RFC_JOB_ID_NONE;
}

rfc_format_id rfc_job_source_format(const rfc_job *job) {
    return job ? job->source_format : (rfc_format_id)0;
}

rfc_format_id rfc_job_target_format(const rfc_job *job) {
    return job ? job->target_format : (rfc_format_id)0;
}

const uint8_t *rfc_job_source_bytes(const rfc_job *job, size_t *out_size) {
    if (!job) {
        if (out_size) {
            *out_size = 0u;
        }
        return NULL;
    }
    if (job->inputs && job->inputs->source_bytes) {
        if (out_size) {
            *out_size = job->inputs->source_size;
        }
        return job->inputs->source_bytes;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    const uint8_t *imp = m->imported_source_bytes;
    size_t sz = m->imported_source_size;
    rfc_mutex_unlock(&m->lock);
    if (out_size) {
        *out_size = sz;
    }
    return imp;
}

const rfc_layout *rfc_job_source_layout(const rfc_job *job) {
    return (job && job->inputs && job->inputs->has_layout)
               ? &job->inputs->source_layout
               : NULL;
}

const rfc_metadata *rfc_job_source_metadata(const rfc_job *job) {
    return (job && job->inputs && job->inputs->has_meta)
               ? &job->inputs->source_meta
               : NULL;
}

uint32_t rfc_job_acknowledged_metadata(const rfc_job *job) {
    return (job && job->inputs) ? job->inputs->acknowledged_unsupported_metadata
                                : 0u;
}

const rfc_setting_value *rfc_job_settings(const rfc_job *job, size_t *out_count) {
    if (out_count) {
        *out_count = (job && job->inputs) ? job->inputs->setting_count : 0u;
    }
    return (job && job->inputs) ? job->inputs->settings : NULL;
}

static void free_decoded_frames_array(rfc_frame **frames_ptr,
                                      size_t *count_ptr,
                                      const rfc_allocator *alloc) {
    if (!frames_ptr || !*frames_ptr) {
        if (count_ptr) {
            *count_ptr = 0u;
        }
        return;
    }
    rfc_frame *frames = *frames_ptr;
    size_t n = count_ptr ? *count_ptr : 0u;
    for (size_t i = 0; i < n; i++) {
        rfc_frame_free(&frames[i]);
    }
    if (alloc && alloc->free) {
        alloc->free(alloc->ctx, frames);
    } else {
        free(frames);
    }
    *frames_ptr = NULL;
    if (count_ptr) {
        *count_ptr = 0u;
    }
}

static void free_verify_coeff_image(const rfc_allocator *alloc,
                                    rfc_jpeg_coeff_image **img_ptr) {
    if (!img_ptr || !*img_ptr) {
        return;
    }
    rfc_jpeg_coeff_image *img = *img_ptr;
    for (uint32_t c = 0u; c < 4u; c++) {
        if (img->components[c].dct_blocks) {
            void *p = (void *)img->components[c].dct_blocks;
            if (alloc && alloc->free) {
                alloc->free(alloc->ctx, p);
            } else {
                free(p);
            }
            img->components[c].dct_blocks = NULL;
        }
    }
    if (alloc && alloc->free) {
        alloc->free(alloc->ctx, img);
    } else {
        free(img);
    }
    *img_ptr = NULL;
}

static rfc_status clone_verify_coeff_image(const rfc_allocator *alloc,
                                           const rfc_jpeg_coeff_image *src,
                                           rfc_jpeg_coeff_image **out_img) {
    *out_img = NULL;
    if (!src || src->component_count == 0u || src->component_count > 4u ||
        !alloc || !alloc->alloc || !alloc->free) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_jpeg_coeff_image *dst =
        (rfc_jpeg_coeff_image *)alloc->alloc(alloc->ctx, sizeof(*dst));
    if (!dst) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    *dst = *src;
    for (uint32_t c = 0u; c < 4u; c++) {
        dst->components[c].dct_blocks = NULL;
    }
    for (uint32_t c = 0u; c < src->component_count; c++) {
        const rfc_jpeg_component_spec *sc = &src->components[c];
        if (sc->blocks_wide == 0u || sc->blocks_high == 0u || !sc->dct_blocks) {
            free_verify_coeff_image(alloc, &dst);
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (sc->blocks_wide > SIZE_MAX / sc->blocks_high ||
            (size_t)sc->blocks_wide * (size_t)sc->blocks_high >
                SIZE_MAX / (64u * sizeof(int16_t))) {
            free_verify_coeff_image(alloc, &dst);
            return RFC_ERR_OVERFLOW;
        }
        size_t bytes = (size_t)sc->blocks_wide * (size_t)sc->blocks_high *
                       64u * sizeof(int16_t);
        int16_t *blk = (int16_t *)alloc->alloc(alloc->ctx, bytes);
        if (!blk) {
            free_verify_coeff_image(alloc, &dst);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(blk, sc->dct_blocks, bytes);
        dst->components[c].dct_blocks = blk;
    }
    *out_img = dst;
    return RFC_OK;
}

void rfc_job_internal_release_stage_outputs(rfc_job *job) {
    if (!job) {
        return;
    }
    rfc_mutex_lock(&job->lock);
    rfc_frame *df = job->decoded_frames;
    size_t df_cnt = job->decoded_frame_count;
    const rfc_allocator *df_alloc =
        job->decoded_frames_alloc ? job->decoded_frames_alloc : &job->allocator;
    uint8_t *d_icc = job->decoded_meta_icc;
    uint8_t *d_exif = job->decoded_meta_exif;
    uint8_t *d_xmp = job->decoded_meta_xmp;
    rfc_encoded enc = job->encoded_output;
    int has_enc = job->has_encoded_output;
    const rfc_session_adapter *enc_sess = job->encoded_owner_sess;
    void *enc_handle = job->encoded_owner_handle;
    rfc_format_id enc_fmt = job->encoded_owner_fmt;
    rfc_jpeg_coeff_image *v_src_c = job->verify_src_coeffs;
    rfc_jpeg_coeff_image *v_dst_c = job->verify_dst_coeffs;
    uint8_t *v_rec = job->verify_reconstructed_bytes;
    uint8_t *v_rec_icc = job->verify_reconstructed_icc;
    uint8_t *v_rec_exif = job->verify_reconstructed_exif;
    uint8_t *v_rec_xmp = job->verify_reconstructed_xmp;
    uint8_t *imp_bytes = job->imported_source_bytes;

    job->decoded_frames = NULL;
    job->decoded_frame_count = 0;
    job->decoded_frames_alloc = NULL;
    job->decoded_meta_icc = NULL;
    job->decoded_meta_exif = NULL;
    job->decoded_meta_xmp = NULL;
    job->has_decoded_meta = 0;
    memset(&job->decoded_meta, 0, sizeof(job->decoded_meta));
    job->has_encoded_output = 0;
    memset(&job->encoded_output, 0, sizeof(job->encoded_output));
    job->encoded_owner_sess = NULL;
    job->encoded_owner_handle = NULL;
    job->encoded_owner_fmt = 0;
    job->verify_src_coeffs = NULL;
    job->verify_dst_coeffs = NULL;
    job->verify_reconstructed_bytes = NULL;
    job->verify_reconstructed_len = 0u;
    job->verify_reconstructed_icc = NULL;
    job->verify_reconstructed_exif = NULL;
    job->verify_reconstructed_xmp = NULL;
    job->has_verify_reconstructed = 0;
    job->has_verify_reconstructed_meta = 0;
    memset(&job->verify_reconstructed_meta, 0,
           sizeof(job->verify_reconstructed_meta));
    job->imported_source_bytes = NULL;
    job->imported_source_size = 0u;
    memset(&job->source_identity, 0, sizeof(job->source_identity));
    rfc_mutex_unlock(&job->lock);

    if (imp_bytes) {
        if (job->allocator.free) {
            job->allocator.free(job->allocator.ctx, imp_bytes);
        } else {
            free(imp_bytes);
        }
    }

    if (has_enc) {
        if (enc_sess && enc_sess->free_encoded) {
            enc_sess->free_encoded(enc_handle, &enc);
        } else {
            rfc_encoded_free(&enc);
        }
    }
    if (enc_sess) {
        if (enc_handle) {
            enc_sess->close_session(enc_handle);
        }
        rfc_registry_release_session(enc_fmt);
    }
    if (d_icc) {
        if (job->allocator.free) {
            job->allocator.free(job->allocator.ctx, d_icc);
        } else {
            free(d_icc);
        }
    }
    if (d_exif && d_exif != d_icc) {
        if (job->allocator.free) {
            job->allocator.free(job->allocator.ctx, d_exif);
        } else {
            free(d_exif);
        }
    }
    if (d_xmp && d_xmp != d_icc && d_xmp != d_exif) {
        if (job->allocator.free) {
            job->allocator.free(job->allocator.ctx, d_xmp);
        } else {
            free(d_xmp);
        }
    }
    free_verify_coeff_image(&job->allocator, &v_src_c);
    free_verify_coeff_image(&job->allocator, &v_dst_c);
    if (v_rec) {
        if (job->allocator.free) {
            job->allocator.free(job->allocator.ctx, v_rec);
        } else {
            free(v_rec);
        }
    }
    if (v_rec_icc) {
        if (job->allocator.free) {
            job->allocator.free(job->allocator.ctx, v_rec_icc);
        } else {
            free(v_rec_icc);
        }
    }
    if (v_rec_exif && v_rec_exif != v_rec_icc) {
        if (job->allocator.free) {
            job->allocator.free(job->allocator.ctx, v_rec_exif);
        } else {
            free(v_rec_exif);
        }
    }
    if (v_rec_xmp && v_rec_xmp != v_rec_icc && v_rec_xmp != v_rec_exif) {
        if (job->allocator.free) {
            job->allocator.free(job->allocator.ctx, v_rec_xmp);
        } else {
            free(v_rec_xmp);
        }
    }
    free_decoded_frames_array(&df, &df_cnt, df_alloc);
}

const rfc_frame *rfc_job_decoded_frames(const rfc_job *job,
                                        size_t *out_count,
                                        const rfc_metadata **out_meta) {
    if (out_count) {
        *out_count = 0u;
    }
    if (out_meta) {
        *out_meta = NULL;
    }
    if (!job) {
        return NULL;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    const rfc_frame *frames = m->decoded_frames;
    if (out_count) {
        *out_count = m->decoded_frame_count;
    }
    if (out_meta) {
        *out_meta = m->has_decoded_meta ? &m->decoded_meta : NULL;
    }
    rfc_mutex_unlock(&m->lock);
    return frames;
}

const rfc_encoded *rfc_job_encoded_output(const rfc_job *job) {
    if (!job) {
        return NULL;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    const rfc_encoded *out = m->has_encoded_output ? &m->encoded_output : NULL;
    rfc_mutex_unlock(&m->lock);
    return out;
}

static rfc_status store_decoded_frames_on_job(rfc_job *job,
                                              rfc_frame *frames,
                                              size_t frame_count,
                                              const rfc_metadata *meta,
                                              int meta_icc_owned_by_exec) {
    if (!job || (frame_count > 0 && !frames)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_metadata new_meta;
    memset(&new_meta, 0, sizeof(new_meta));
    uint8_t *owned_icc = NULL;
    uint8_t *owned_exif = NULL;
    uint8_t *owned_xmp = NULL;
    int has_meta = 0;

    if (meta) {
        new_meta = *meta;
        has_meta = 1;
        if (meta->icc && meta->icc_len > 0) {
            int aliases_frame0 =
                (frame_count > 0 && frames[0].metadata.icc == meta->icc);
            if (aliases_frame0) {
                owned_icc = NULL;
            } else if (meta_icc_owned_by_exec) {
                owned_icc = (uint8_t *)meta->icc;
            } else {
                owned_icc = (uint8_t *)job->allocator.alloc(job->allocator.ctx,
                                                            meta->icc_len);
                if (!owned_icc) {
                    return RFC_ERR_OUT_OF_MEMORY;
                }
                memcpy(owned_icc, meta->icc, meta->icc_len);
                new_meta.icc = owned_icc;
            }
        } else {
            new_meta.icc = NULL;
        }
        if (meta->exif && meta->exif_len > 0) {
            int aliases_frame0 =
                (frame_count > 0 && frames[0].metadata.exif == meta->exif);
            if (aliases_frame0) {
                owned_exif = NULL;
            } else if (meta_icc_owned_by_exec) {
                owned_exif = (uint8_t *)meta->exif;
            } else {
                owned_exif = (uint8_t *)job->allocator.alloc(job->allocator.ctx,
                                                             meta->exif_len);
                if (!owned_exif) {
                    if (owned_icc && !meta_icc_owned_by_exec) {
                        job->allocator.free(job->allocator.ctx, owned_icc);
                    }
                    return RFC_ERR_OUT_OF_MEMORY;
                }
                memcpy(owned_exif, meta->exif, meta->exif_len);
                new_meta.exif = owned_exif;
            }
        } else {
            new_meta.exif = NULL;
        }
        if (meta->xmp && meta->xmp_len > 0) {
            int aliases_frame0 =
                (frame_count > 0 && frames[0].metadata.xmp == meta->xmp);
            if (aliases_frame0) {
                owned_xmp = NULL;
            } else if (meta_icc_owned_by_exec) {
                owned_xmp = (uint8_t *)meta->xmp;
            } else {
                owned_xmp = (uint8_t *)job->allocator.alloc(job->allocator.ctx,
                                                            meta->xmp_len);
                if (!owned_xmp) {
                    if (!meta_icc_owned_by_exec) {
                        if (owned_exif) {
                            job->allocator.free(job->allocator.ctx, owned_exif);
                        }
                        if (owned_icc) {
                            job->allocator.free(job->allocator.ctx, owned_icc);
                        }
                    }
                    return RFC_ERR_OUT_OF_MEMORY;
                }
                memcpy(owned_xmp, meta->xmp, meta->xmp_len);
                new_meta.xmp = owned_xmp;
            }
        } else {
            new_meta.xmp = NULL;
        }
    } else if (frame_count > 0) {
        new_meta = frames[0].metadata;
        has_meta = 1;
    }

    rfc_mutex_lock(&job->lock);
    rfc_frame *old_df = job->decoded_frames;
    size_t old_cnt = job->decoded_frame_count;
    const rfc_allocator *old_alloc =
        job->decoded_frames_alloc ? job->decoded_frames_alloc : &job->allocator;
    uint8_t *old_icc = job->decoded_meta_icc;
    uint8_t *old_exif = job->decoded_meta_exif;
    uint8_t *old_xmp = job->decoded_meta_xmp;

    job->decoded_frames = frames;
    job->decoded_frame_count = frame_count;
    job->decoded_frames_alloc =
        (frame_count > 0 && frames[0].allocator) ? frames[0].allocator
                                                 : &job->allocator;
    job->decoded_meta = new_meta;
    job->decoded_meta_icc = owned_icc;
    job->decoded_meta_exif = owned_exif;
    job->decoded_meta_xmp = owned_xmp;
    job->has_decoded_meta = has_meta;
    rfc_mutex_unlock(&job->lock);

    if (old_icc) {
        job->allocator.free(job->allocator.ctx, old_icc);
    }
    if (old_exif && old_exif != old_icc) {
        job->allocator.free(job->allocator.ctx, old_exif);
    }
    if (old_xmp && old_xmp != old_icc && old_xmp != old_exif) {
        job->allocator.free(job->allocator.ctx, old_xmp);
    }
    free_decoded_frames_array(&old_df, &old_cnt, old_alloc);
    return RFC_OK;
}

rfc_status rfc_job_set_decoded_frames(rfc_job *job,
                                      rfc_frame *frames,
                                      size_t frame_count,
                                      const rfc_metadata *meta) {
    if (!job || (frame_count > 0 && !frames)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (frame_count > 0) {
        uint64_t comp_sz = 0ull;
        if (job->inputs && job->inputs->source_size > 0u) {
            comp_sz = (uint64_t)job->inputs->source_size;
        } else if (job->imported_source_size > 0u) {
            comp_sz = (uint64_t)job->imported_source_size;
        }
        int has_comp = (comp_sz > 0ull) ? 1 : 0;
        rfc_status st = rfc_limits_check_frames(
            &job->limits, frames, frame_count, meta, comp_sz, has_comp);
        if (st != RFC_OK) {
            return st;
        }
    } else if (meta) {
        rfc_status st = rfc_limits_check_metadata(&job->limits, meta);
        if (st != RFC_OK) {
            return st;
        }
    }
    return store_decoded_frames_on_job(job, frames, frame_count, meta, 0);
}

rfc_status rfc_job_set_encoded_output(rfc_job *job, const rfc_encoded *encoded) {
    if (!job || !encoded || (encoded->len > 0 && !encoded->data)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_encoded stored;
    memset(&stored, 0, sizeof(stored));
    if (encoded->allocator != NULL) {
        stored = *encoded;
    } else if (encoded->len > 0) {
        uint8_t *copy = (uint8_t *)job->allocator.alloc(job->allocator.ctx,
                                                        encoded->len);
        if (!copy) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, encoded->data, encoded->len);
        stored.data = copy;
        stored.len = encoded->len;
        stored.allocator = &job->allocator;
    }

    rfc_mutex_lock(&job->lock);
    rfc_encoded old_enc = job->encoded_output;
    int had_enc = job->has_encoded_output;
    const rfc_session_adapter *old_sess = job->encoded_owner_sess;
    void *old_handle = job->encoded_owner_handle;
    rfc_format_id old_fmt = job->encoded_owner_fmt;

    job->encoded_output = stored;
    job->has_encoded_output = 1;
    job->encoded_owner_sess = NULL;
    job->encoded_owner_handle = NULL;
    job->encoded_owner_fmt = 0;
    rfc_mutex_unlock(&job->lock);

    if (had_enc) {
        if (old_sess && old_sess->free_encoded) {
            old_sess->free_encoded(old_handle, &old_enc);
        } else {
            rfc_encoded_free(&old_enc);
        }
    }
    if (old_sess) {
        if (old_handle) {
            old_sess->close_session(old_handle);
        }
        rfc_registry_release_session(old_fmt);
    }
    return RFC_OK;
}

rfc_status rfc_job_verify_report_get(const rfc_job *job,
                                     rfc_verify_report *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!job || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    if (!m->verify_report.valid) {
        rfc_mutex_unlock(&m->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out = m->verify_report;
    rfc_mutex_unlock(&m->lock);
    return RFC_OK;
}

static rfc_status job_clone_verified_metadata(
    const rfc_allocator *alloc,
    const rfc_metadata *src,
    rfc_metadata *out_meta,
    uint8_t **out_icc_copy,
    uint8_t **out_exif_copy,
    uint8_t **out_xmp_copy,
    int *out_has_meta) {
    memset(out_meta, 0, sizeof(*out_meta));
    *out_icc_copy = NULL;
    *out_exif_copy = NULL;
    *out_xmp_copy = NULL;
    *out_has_meta = 0;
    if (!src) {
        return RFC_OK;
    }
    if ((src->icc_len > 0u && !src->icc) ||
        (src->exif_len > 0u && !src->exif) ||
        (src->xmp_len > 0u && !src->xmp)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_meta = *src;
    out_meta->icc = NULL;
    out_meta->exif = NULL;
    out_meta->xmp = NULL;
    if (src->icc_len > 0u || src->exif_len > 0u || src->xmp_len > 0u) {
        if (!alloc || !alloc->alloc || !alloc->free) {
            memset(out_meta, 0, sizeof(*out_meta));
            return RFC_ERR_INVALID_ARGUMENT;
        }
    }
    if (src->icc_len > 0u) {
        uint8_t *icc = (uint8_t *)alloc->alloc(alloc->ctx, src->icc_len);
        if (!icc) {
            memset(out_meta, 0, sizeof(*out_meta));
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(icc, src->icc, src->icc_len);
        out_meta->icc = icc;
        *out_icc_copy = icc;
    }
    if (src->exif_len > 0u) {
        uint8_t *exif = (uint8_t *)alloc->alloc(alloc->ctx, src->exif_len);
        if (!exif) {
            if (*out_icc_copy) {
                alloc->free(alloc->ctx, *out_icc_copy);
                *out_icc_copy = NULL;
            }
            memset(out_meta, 0, sizeof(*out_meta));
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(exif, src->exif, src->exif_len);
        out_meta->exif = exif;
        *out_exif_copy = exif;
    }
    if (src->xmp_len > 0u) {
        uint8_t *xmp = (uint8_t *)alloc->alloc(alloc->ctx, src->xmp_len);
        if (!xmp) {
            if (*out_exif_copy) {
                alloc->free(alloc->ctx, *out_exif_copy);
                *out_exif_copy = NULL;
            }
            if (*out_icc_copy) {
                alloc->free(alloc->ctx, *out_icc_copy);
                *out_icc_copy = NULL;
            }
            memset(out_meta, 0, sizeof(*out_meta));
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(xmp, src->xmp, src->xmp_len);
        out_meta->xmp = xmp;
        *out_xmp_copy = xmp;
    }
    *out_has_meta = 1;
    return RFC_OK;
}

rfc_status rfc_job_set_verified_jpeg_coeffs(
    rfc_job *job,
    const rfc_jpeg_coeff_image *source_coeffs,
    const rfc_jpeg_coeff_image *verified_coeffs,
    const rfc_metadata *verified_meta) {
    if (!job || !source_coeffs || !verified_coeffs) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_jpeg_coeff_image *src_copy = NULL;
    rfc_jpeg_coeff_image *dst_copy = NULL;
    rfc_status st =
        clone_verify_coeff_image(&job->allocator, source_coeffs, &src_copy);
    if (st != RFC_OK) {
        return st;
    }
    st = clone_verify_coeff_image(&job->allocator, verified_coeffs, &dst_copy);
    if (st != RFC_OK) {
        free_verify_coeff_image(&job->allocator, &src_copy);
        return st;
    }
    rfc_metadata meta_copy;
    uint8_t *icc_copy = NULL;
    uint8_t *exif_copy = NULL;
    uint8_t *xmp_copy = NULL;
    int has_rmeta = 0;
    st = job_clone_verified_metadata(&job->allocator, verified_meta, &meta_copy,
                                     &icc_copy, &exif_copy, &xmp_copy,
                                     &has_rmeta);
    if (st != RFC_OK) {
        free_verify_coeff_image(&job->allocator, &src_copy);
        free_verify_coeff_image(&job->allocator, &dst_copy);
        return st;
    }
    rfc_mutex_lock(&job->lock);
    rfc_jpeg_coeff_image *old_src = job->verify_src_coeffs;
    rfc_jpeg_coeff_image *old_dst = job->verify_dst_coeffs;
    uint8_t *old_icc = job->verify_reconstructed_icc;
    uint8_t *old_exif = job->verify_reconstructed_exif;
    uint8_t *old_xmp = job->verify_reconstructed_xmp;
    job->verify_src_coeffs = src_copy;
    job->verify_dst_coeffs = dst_copy;
    job->verify_reconstructed_meta = meta_copy;
    job->verify_reconstructed_icc = icc_copy;
    job->verify_reconstructed_exif = exif_copy;
    job->verify_reconstructed_xmp = xmp_copy;
    job->has_verify_reconstructed_meta = has_rmeta;
    rfc_mutex_unlock(&job->lock);
    free_verify_coeff_image(&job->allocator, &old_src);
    free_verify_coeff_image(&job->allocator, &old_dst);
    if (old_icc) {
        job->allocator.free(job->allocator.ctx, old_icc);
    }
    if (old_exif && old_exif != old_icc) {
        job->allocator.free(job->allocator.ctx, old_exif);
    }
    if (old_xmp && old_xmp != old_icc && old_xmp != old_exif) {
        job->allocator.free(job->allocator.ctx, old_xmp);
    }
    return RFC_OK;
}

rfc_status rfc_job_set_verified_reconstructed_bytes(
    rfc_job *job,
    const uint8_t *reconstructed_jpeg,
    size_t reconstructed_len,
    const rfc_metadata *reconstructed_meta) {
    if (!job || (reconstructed_len > 0u && !reconstructed_jpeg)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_metadata meta_copy;
    uint8_t *icc_copy = NULL;
    uint8_t *exif_copy = NULL;
    uint8_t *xmp_copy = NULL;
    int has_rmeta = 0;
    rfc_status st = job_clone_verified_metadata(
        &job->allocator, reconstructed_meta, &meta_copy, &icc_copy, &exif_copy,
        &xmp_copy, &has_rmeta);
    if (st != RFC_OK) {
        return st;
    }
    uint8_t *bytes_copy = NULL;
    if (reconstructed_len > 0u) {
        bytes_copy = (uint8_t *)job->allocator.alloc(job->allocator.ctx,
                                                     reconstructed_len);
        if (!bytes_copy) {
            if (xmp_copy) {
                job->allocator.free(job->allocator.ctx, xmp_copy);
            }
            if (exif_copy) {
                job->allocator.free(job->allocator.ctx, exif_copy);
            }
            if (icc_copy) {
                job->allocator.free(job->allocator.ctx, icc_copy);
            }
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memcpy(bytes_copy, reconstructed_jpeg, reconstructed_len);
    }

    rfc_mutex_lock(&job->lock);
    uint8_t *old_bytes = job->verify_reconstructed_bytes;
    uint8_t *old_icc = job->verify_reconstructed_icc;
    uint8_t *old_exif = job->verify_reconstructed_exif;
    uint8_t *old_xmp = job->verify_reconstructed_xmp;
    job->verify_reconstructed_bytes = bytes_copy;
    job->verify_reconstructed_len = reconstructed_len;
    job->verify_reconstructed_meta = meta_copy;
    job->verify_reconstructed_icc = icc_copy;
    job->verify_reconstructed_exif = exif_copy;
    job->verify_reconstructed_xmp = xmp_copy;
    job->has_verify_reconstructed = 1;
    job->has_verify_reconstructed_meta = has_rmeta;
    rfc_mutex_unlock(&job->lock);

    if (old_bytes) {
        job->allocator.free(job->allocator.ctx, old_bytes);
    }
    if (old_icc) {
        job->allocator.free(job->allocator.ctx, old_icc);
    }
    if (old_exif && old_exif != old_icc) {
        job->allocator.free(job->allocator.ctx, old_exif);
    }
    if (old_xmp && old_xmp != old_icc && old_xmp != old_exif) {
        job->allocator.free(job->allocator.ctx, old_xmp);
    }
    return RFC_OK;
}

static int job_is_terminal_state(rfc_job_state s) {
    return s == RFC_JOB_SUCCEEDED || s == RFC_JOB_FAILED ||
           s == RFC_JOB_CANCELLED || s == RFC_JOB_SUPERSEDED;
}

static int job_slot_superseded_locked(const rfc_job *job) {
    if (!job || !job->slot_state) {
        return 0;
    }
    rfc_job_id slot_id = rfc_slot_state_get_id(job->slot_state);
    return slot_id != job->job_id;
}

static int job_is_aborted_locked(const rfc_job *job) {
    return job->cancel_requested || job->superseded ||
           job->superseded_by_id != RFC_JOB_ID_NONE ||
           job->state == RFC_JOB_CANCELLED ||
           job->state == RFC_JOB_SUPERSEDED;
}

int rfc_job_is_cancelled(const rfc_job *job) {
    if (!job) {
        return 1;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    int aborted = job_is_aborted_locked(m);
    rfc_mutex_unlock(&m->lock);
    return aborted;
}

rfc_status rfc_job_cancel(rfc_job *job) {
    if (!job) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_scheduler *sched = job->sched;
    if (sched) {
        /* Delegate lock acquisition order (sched->lock -> job->lock) to scheduler. */
        rfc_scheduler_internal_retain(sched);
        rfc_scheduler_internal_on_cancel_count(sched);
    }

    extern rfc_status rfc_scheduler_internal_cancel_job(rfc_scheduler *s, rfc_job *j);
    if (sched) {
        rfc_status st = rfc_scheduler_internal_cancel_job(sched, job);
        rfc_scheduler_internal_release(sched);
        return st;
    }

    rfc_mutex_lock(&job->lock);
    if (!job->published && !job->publishing && !job->publish_decided &&
        !job_is_terminal_state(job->state)) {
        job->cancel_requested = 1;
        job->suppress_callbacks = 1;
        if (job->state == RFC_JOB_QUEUED) {
            job->state = RFC_JOB_CANCELLED;
            job->status = RFC_ERR_CANCELLED;
            job->failing_stage = job->stage;
            rfc_cond_broadcast(&job->done_cond);
        }
    }
    int same_thread =
        job->cb_owner_valid &&
        rfc_thread_id_equal(job->cb_owner, rfc_thread_id_current());
    rfc_mutex_unlock(&job->lock);

    if (!same_thread) {
        rfc_mutex_lock(&job->cb_lock);
        rfc_mutex_unlock(&job->cb_lock);
    }
    return RFC_OK;
}

static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
#endif
}

rfc_status rfc_job_wait(rfc_job *job, uint32_t timeout_ms) {
    if (!job) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_mutex_lock(&job->lock);
    if (timeout_ms == UINT32_MAX) {
        while (!job_is_terminal_state(job->state)) {
            rfc_cond_wait(&job->done_cond, &job->lock);
        }
        rfc_mutex_unlock(&job->lock);
        return RFC_OK;
    }

    uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
    while (!job_is_terminal_state(job->state)) {
        uint64_t now = monotonic_ms();
        if (now >= deadline) {
            rfc_mutex_unlock(&job->lock);
            return RFC_ERR_BUSY;
        }
        uint64_t rem = deadline - now;
        uint32_t wait_slice = rem >= UINT32_MAX ? (UINT32_MAX - 1u) : (uint32_t)rem;
        rfc_cond_timedwait(&job->done_cond, &job->lock, wait_slice);
    }
    rfc_mutex_unlock(&job->lock);
    return RFC_OK;
}

rfc_status rfc_job_snapshot_get(const rfc_job *job, rfc_job_snapshot *out) {
    if (!job || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    out->job_id = m->job_id;
    out->superseded_by_id = m->superseded_by_id;
    out->state = m->state;
    out->status = m->status;
    out->stage = m->stage;
    out->failing_stage = m->failing_stage;
    out->admitted_threads = m->admitted_threads;
    out->reserved_memory_bytes = m->reserved_memory_bytes;
    out->live_partial_bytes = m->live_partial_bytes;
    out->attempt_number = m->attempt_number;
    memcpy(out->detail, m->detail, sizeof(out->detail));
    rfc_budget *b = m->budget;
    rfc_mutex_unlock(&m->lock);
    if (b) {
        rfc_budget_stats_get(b, &out->budget);
    }
    return RFC_OK;
}

rfc_status rfc_job_report_progress(rfc_job *job,
                                   rfc_job_id expected_id,
                                   rfc_job_stage stage,
                                   uint32_t completed_units,
                                   uint32_t total_units,
                                   const char *detail) {
    if (!job || expected_id == RFC_JOB_ID_NONE) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    if (g_progress_hook) {
        g_progress_hook(job->job_id, 0, g_progress_hook_user);
    }

    /* Strict lock order: job->cb_lock -> sched->lock -> job->lock -> slot_state->lock */
    rfc_mutex_lock(&job->cb_lock);

    rfc_scheduler *sched = job->sched;
    if (sched) {
        rfc_scheduler_internal_lock(sched);
    }
    rfc_mutex_lock(&job->lock);

    job->cb_owner_valid = 1;
    job->cb_owner = rfc_thread_id_current();

    int stale = job_slot_superseded_locked(job) ||
                job->job_id != expected_id || job->superseded ||
                job->superseded_by_id != RFC_JOB_ID_NONE ||
                job->state == RFC_JOB_SUPERSEDED;
    int destroying =
        sched ? rfc_scheduler_internal_is_destroying_locked(sched) : 0;
    int cancelled = job->cancel_requested || job->state == RFC_JOB_CANCELLED ||
                    destroying || job->suppress_callbacks;

    if (stale || cancelled) {
        job->cb_owner_valid = 0;
        rfc_mutex_unlock(&job->lock);
        if (sched) {
            rfc_scheduler_internal_unlock(sched);
        }
        rfc_mutex_unlock(&job->cb_lock);
        return stale ? RFC_ERR_STALE_JOB : RFC_ERR_CANCELLED;
    }

    uint32_t pct = job->last_percent;
    if (total_units > 0) {
        uint64_t calc = ((uint64_t)completed_units * 100ull) / (uint64_t)total_units;
        if (calc > 100ull) {
            calc = 100ull;
        }
        if ((uint32_t)calc > pct) {
            pct = (uint32_t)calc;
        }
    }
    job->last_percent = pct;

    rfc_progress_fn fn = job->progress_fn;
    void *user = job->progress_user;
    rfc_job_progress msg;
    memset(&msg, 0, sizeof(msg));
    msg.job_id = job->job_id;
    msg.state = job->state;
    msg.stage = stage;
    msg.completed_units = completed_units;
    msg.total_units = total_units;
    msg.percent = pct;
    rfc_job_internal_set_detail(msg.detail, detail);

    if (fn && sched) {
        rfc_scheduler_internal_callback_enter_locked(sched);
    }
    rfc_mutex_unlock(&job->lock);
    if (sched) {
        rfc_scheduler_internal_unlock(sched);
    }

    if (fn) {
        if (g_progress_hook) {
            g_progress_hook(job->job_id, 1, g_progress_hook_user);
        }
        fn(&msg, user);
        if (sched) {
            rfc_scheduler_internal_callback_leave(sched);
        }
    }

    rfc_mutex_lock(&job->lock);
    job->cb_owner_valid = 0;
    rfc_mutex_unlock(&job->lock);
    rfc_mutex_unlock(&job->cb_lock);
    return RFC_OK;
}

static rfc_job_stage next_stage_after(rfc_job_stage s) {
    switch (s) {
        case RFC_STAGE_PREFLIGHT:
            return RFC_STAGE_IMPORT;
        case RFC_STAGE_IMPORT:
            return RFC_STAGE_ENCODE;
        case RFC_STAGE_ENCODE:
            return RFC_STAGE_VERIFY;
        case RFC_STAGE_VERIFY:
            return RFC_STAGE_EXPORT;
        case RFC_STAGE_EXPORT:
            return RFC_STAGE_DONE;
        default:
            return RFC_STAGE_NONE;
    }
}

rfc_status rfc_job_advance_stage(rfc_job *job,
                                 rfc_job_id expected_id,
                                 rfc_job_stage from_stage,
                                 rfc_job_stage to_stage) {
    if (!job || expected_id == RFC_JOB_ID_NONE) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_mutex_lock(&job->lock);
    if (job_slot_superseded_locked(job) || job->job_id != expected_id ||
        job->superseded || job->superseded_by_id != RFC_JOB_ID_NONE ||
        job->state == RFC_JOB_SUPERSEDED) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_STALE_JOB;
    }
    if (job->cancel_requested || job->state == RFC_JOB_CANCELLED) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_CANCELLED;
    }
    if (job->state != RFC_JOB_RUNNING || job->stage != from_stage ||
        to_stage == RFC_STAGE_NONE || next_stage_after(from_stage) != to_stage) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    job->stage = to_stage;
    rfc_mutex_unlock(&job->lock);
    return RFC_OK;
}

rfc_status rfc_job_complete_stage(rfc_job *job,
                                  rfc_job_id expected_id,
                                  rfc_job_stage stage,
                                  rfc_status outcome,
                                  const char *detail) {
    if (!job || expected_id == RFC_JOB_ID_NONE) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_mutex_lock(&job->lock);
    if (job_slot_superseded_locked(job) || job->job_id != expected_id ||
        job->superseded || job->superseded_by_id != RFC_JOB_ID_NONE ||
        job->state == RFC_JOB_SUPERSEDED) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_STALE_JOB;
    }
    if (job->cancel_requested || job->state == RFC_JOB_CANCELLED) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_CANCELLED;
    }
    if (job->stage != stage) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (outcome != RFC_OK) {
        job->failing_stage = stage;
        job->status = outcome;
        rfc_job_internal_set_detail(job->detail, detail);
    } else if (detail) {
        rfc_job_internal_set_detail(job->detail, detail);
    }
    rfc_mutex_unlock(&job->lock);
    return RFC_OK;
}

rfc_status rfc_job_attach_partial(rfc_job *job,
                                  void *ptr,
                                  size_t byte_size,
                                  void (*free_fn)(void *ctx, void *ptr),
                                  void *free_ctx) {
    if (!job || !ptr || !free_fn) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_partial_entry *e = (rfc_partial_entry *)malloc(sizeof(*e));
    if (!e) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    e->ptr = ptr;
    e->byte_size = byte_size;
    e->free_fn = free_fn;
    e->free_ctx = free_ctx;

    rfc_scheduler *sched = job->sched;
    if (sched) {
        rfc_scheduler_internal_lock(sched);
    }
    rfc_mutex_lock(&job->lock);
    e->next = job->partials;
    job->partials = e;
    job->attached_partial_bytes += (uint64_t)byte_size;
    job->live_partial_bytes =
        job->attached_partial_bytes + job->staged_partial_bytes;
    if (sched && byte_size > 0u) {
        uint64_t step = ((uint64_t)byte_size > (uint64_t)INT64_MAX)
                            ? (uint64_t)INT64_MAX
                            : (uint64_t)byte_size;
        rfc_scheduler_internal_adjust_live_partials_locked(sched,
                                                           (int64_t)step);
    }
    rfc_mutex_unlock(&job->lock);
    if (sched) {
        rfc_scheduler_internal_unlock(sched);
    }
    return RFC_OK;
}

void *rfc_job_alloc_partial(rfc_job *job, size_t byte_size) {
    if (!job || !job->budget) {
        return NULL;
    }
    void *ptr = rfc_budget_alloc(job->budget, byte_size);
    if (!ptr) {
        return NULL;
    }
    if (rfc_job_attach_partial(job, ptr, byte_size, budget_free_cb,
                               job->budget) != RFC_OK) {
        rfc_budget_free(job->budget, ptr);
        return NULL;
    }
    return ptr;
}

void rfc_job_release_partials(rfc_job *job) {
    if (!job) {
        return;
    }
    rfc_scheduler *sched = job->sched;
    if (sched) {
        rfc_scheduler_internal_lock(sched);
    }
    rfc_mutex_lock(&job->lock);
    rfc_partial_entry *list = job->partials;
    job->partials = NULL;
    uint64_t freed = job->attached_partial_bytes;
    job->attached_partial_bytes = 0u;
    job->live_partial_bytes = job->staged_partial_bytes;
    if (sched && freed > 0u) {
        uint64_t step =
            (freed > (uint64_t)INT64_MAX) ? (uint64_t)INT64_MAX : freed;
        rfc_scheduler_internal_adjust_live_partials_locked(sched,
                                                           -(int64_t)step);
    }
    rfc_mutex_unlock(&job->lock);
    if (sched) {
        rfc_scheduler_internal_unlock(sched);
    }

    while (list) {
        rfc_partial_entry *next = list->next;
        if (list->free_fn && list->ptr) {
            list->free_fn(list->free_ctx, list->ptr);
        }
        free(list);
        list = next;
    }
}

rfc_status rfc_job_mark_published(rfc_job *job, rfc_job_id expected_id) {
    if (!job || expected_id == RFC_JOB_ID_NONE) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_mutex_lock(&job->lock);
    if (job_slot_superseded_locked(job) || job->job_id != expected_id ||
        job->superseded || job->superseded_by_id != RFC_JOB_ID_NONE ||
        job->state == RFC_JOB_SUPERSEDED) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_STALE_JOB;
    }
    if (job->cancel_requested || job->state == RFC_JOB_CANCELLED) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_CANCELLED;
    }
    if (job->stage != RFC_STAGE_EXPORT) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    job->published = 1;
    rfc_mutex_unlock(&job->lock);
    return RFC_OK;
}

rfc_status rfc_job_internal_begin_publish(rfc_job *job, rfc_job_id expected_id) {
    if (!job || expected_id == RFC_JOB_ID_NONE) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_mutex_lock(&job->lock);
    if (job_slot_superseded_locked(job) || job->job_id != expected_id ||
        job->superseded || job->superseded_by_id != RFC_JOB_ID_NONE ||
        job->state == RFC_JOB_SUPERSEDED) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_STALE_JOB;
    }
    if (job->cancel_requested || job->state == RFC_JOB_CANCELLED) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_CANCELLED;
    }
    if (job->stage != RFC_STAGE_EXPORT) {
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    job->publishing = 1;
    rfc_mutex_unlock(&job->lock);
    return RFC_OK;
}

void rfc_job_internal_finish_publish(rfc_job *job,
                                     rfc_job_id expected_id,
                                     int succeeded) {
    if (!job) {
        return;
    }
    rfc_mutex_lock(&job->lock);
    job->publishing = 0;
    if (job->job_id == expected_id) {
        job->publish_decided = 1;
        if (succeeded) {
            job->published = 1;
        }
    }
    rfc_mutex_unlock(&job->lock);
}

void rfc_job_internal_note_partial_bytes(rfc_job *job, int64_t delta_bytes) {
    if (!job || delta_bytes == 0) {
        return;
    }
    rfc_scheduler *sched = job->sched;
    if (sched) {
        rfc_scheduler_internal_lock(sched);
    }
    rfc_mutex_lock(&job->lock);
    int64_t actual_delta = 0;
    if (delta_bytes > 0) {
        uint64_t add = (uint64_t)delta_bytes;
        job->staged_partial_bytes += add;
        actual_delta = delta_bytes;
    } else {
        uint64_t sub = (uint64_t)(-(delta_bytes + 1)) + 1u;
        if (job->staged_partial_bytes >= sub) {
            job->staged_partial_bytes -= sub;
            actual_delta = delta_bytes;
        } else {
            uint64_t clamped = job->staged_partial_bytes;
            if (clamped > (uint64_t)INT64_MAX) {
                clamped = (uint64_t)INT64_MAX;
            }
            actual_delta = -(int64_t)clamped;
            job->staged_partial_bytes = 0u;
        }
    }
    job->live_partial_bytes =
        job->attached_partial_bytes + job->staged_partial_bytes;
    if (sched && actual_delta != 0) {
        rfc_scheduler_internal_adjust_live_partials_locked(sched,
                                                           actual_delta);
    }
    rfc_mutex_unlock(&job->lock);
    if (sched) {
        rfc_scheduler_internal_unlock(sched);
    }
}

rfc_status rfc_job_sniff_result_get(const rfc_job *job,
                                    rfc_sniff_result *out_sniff) {
    if (out_sniff) {
        memset(out_sniff, 0, sizeof(*out_sniff));
    }
    if (!job || !out_sniff) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    if (!m->has_sniff_result) {
        rfc_mutex_unlock(&m->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_sniff = m->sniff_result;
    rfc_mutex_unlock(&m->lock);
    return RFC_OK;
}

rfc_status rfc_job_export_report_get(const rfc_job *job,
                                     rfc_export_report *out_report) {
    if (out_report) {
        memset(out_report, 0, sizeof(*out_report));
    }
    if (!job || !out_report) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    if (!m->export_report.valid) {
        rfc_mutex_unlock(&m->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_report = m->export_report;
    rfc_mutex_unlock(&m->lock);
    return RFC_OK;
}

rfc_status rfc_job_stage_timings_get(const rfc_job *job,
                                     rfc_stage_timings *out_timings) {
    if (out_timings) {
        memset(out_timings, 0, sizeof(*out_timings));
    }
    if (!job || !out_timings) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    if (!m->stage_timings.valid) {
        rfc_mutex_unlock(&m->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_timings = m->stage_timings;
    rfc_mutex_unlock(&m->lock);
    return RFC_OK;
}

uint64_t rfc_job_internal_raw_frame_bytes(const rfc_job *job) {
    if (!job) {
        return 0u;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    uint64_t v = m->raw_frame_bytes;
    rfc_mutex_unlock(&m->lock);
    return v;
}

rfc_status rfc_job_open_published_reader(const rfc_job *job,
                                         rfc_io_reader *out_reader) {
    if (out_reader) {
        memset(out_reader, 0, sizeof(*out_reader));
    }
    if (!job || !out_reader) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_job *m = (rfc_job *)job;
    rfc_mutex_lock(&m->lock);
    int pub_ok = (m->export_report.valid && m->export_report.published);
    rfc_io_publisher_box *pbox =
        (m->inputs && m->inputs->io_publisher_box) ? m->inputs->io_publisher_box
                                                   : NULL;
    rfc_job_id jid = m->job_id;
    uint64_t attempt = m->attempt_number;
    rfc_mutex_unlock(&m->lock);

    if (!pub_ok) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (!pbox) {
        return RFC_ERR_NO_ADAPTER;
    }

    rfc_mutex_lock(&pbox->lock);
    if (!pbox->publisher.open_published_reader) {
        rfc_mutex_unlock(&pbox->lock);
        return RFC_ERR_NO_ADAPTER;
    }
    rfc_io_reader rdr;
    memset(&rdr, 0, sizeof(rdr));
    rfc_status st = pbox->publisher.open_published_reader(
        pbox->publisher.ctx, jid, attempt, &rdr);
    rfc_mutex_unlock(&pbox->lock);
    if (st != RFC_OK) {
        if (rdr.close_fn) {
            rdr.close_fn(rdr.ctx);
        }
        return st;
    }
    if (!rdr.read_fn || !rdr.seek_fn) {
        if (rdr.close_fn) {
            rdr.close_fn(rdr.ctx);
        }
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_reader = rdr;
    return RFC_OK;
}

int rfc_exec_is_cancelled(const rfc_exec_context *exec) {
    if (!exec) {
        return 1;
    }
    if (!exec->job) {
        return 0;
    }
    return rfc_job_is_cancelled(exec->job);
}

void rfc_exec_set_detail(const rfc_exec_context *exec, const char *detail) {
    if (!exec || !exec->job) {
        return;
    }
    rfc_mutex_lock(&exec->job->lock);
    exec->job->adapter_detail = detail;
    rfc_mutex_unlock(&exec->job->lock);
}

/* The detail an adapter named for its refusal, consumed once. */
static const char *take_adapter_detail(rfc_job *job) {
    rfc_mutex_lock(&job->lock);
    const char *d = job->adapter_detail;
    job->adapter_detail = NULL;
    rfc_mutex_unlock(&job->lock);
    return d;
}

uint32_t rfc_exec_threads(const rfc_exec_context *exec) {
    return exec ? exec->codec_threads : 0u;
}

const rfc_limits *rfc_exec_limits(const rfc_exec_context *exec) {
    return exec ? &exec->limits : NULL;
}

const rfc_allocator *rfc_exec_allocator(const rfc_exec_context *exec) {
    return exec ? exec->allocator : NULL;
}

rfc_budget *rfc_exec_budget(const rfc_exec_context *exec) {
    return exec ? exec->budget : NULL;
}

static int layouts_equal_exact(const rfc_layout *a, const rfc_layout *b) {
    if (!a || !b) {
        return 0;
    }
    return a->format == b->format && a->alpha == b->alpha &&
           a->width == b->width && a->height == b->height &&
           a->stride_bytes == b->stride_bytes &&
           a->bits_per_sample == b->bits_per_sample;
}

static rfc_status consume_decode_credit_locked(rfc_job *job,
                                               const rfc_limits *limits,
                                               const rfc_layout *norm,
                                               uint64_t fbytes,
                                               size_t icc_len,
                                               size_t exif_len,
                                               size_t xmp_len) {
    if (job->decode_last_checked_fbytes != fbytes ||
        job->decode_last_checked_icc_len != icc_len ||
        job->decode_last_checked_exif_len != exif_len ||
        job->decode_last_checked_xmp_len != xmp_len ||
        !layouts_equal_exact(&job->decode_last_checked_layout, norm)) {
        rfc_status lim_st = rfc_limits_check_image(limits, norm, 1u);
        if (lim_st != RFC_OK) {
            if (job->decode_guard_failure == RFC_OK) {
                job->decode_guard_failure = lim_st;
            }
            return lim_st;
        }
        job->decode_unvalidated_alloc = 1;
        if (job->decode_guard_failure == RFC_OK) {
            job->decode_guard_failure = RFC_ERR_INVALID_ARGUMENT;
        }
        return RFC_ERR_INVALID_ARGUMENT;
    }
    job->decode_credit_frames = 0u;
    memset(&job->decode_last_checked_layout, 0,
           sizeof(job->decode_last_checked_layout));
    return RFC_OK;
}

static rfc_status internal_exec_check_next_frame(rfc_job *job,
                                                 const rfc_limits *limits,
                                                 const rfc_layout *layout,
                                                 const rfc_metadata *meta,
                                                 int grant_credit) {
    if (!limits || !layout) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (job && rfc_job_is_cancelled(job)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_layout norm;
    rfc_status st = rfc_layout_normalize_stride(layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_limits_check_image(limits, &norm, 1u);
    if (st != RFC_OK) {
        return st;
    }
    if (meta) {
        st = rfc_limits_check_metadata(limits, meta);
        if (st != RFC_OK) {
            return st;
        }
    }
    uint64_t fbytes = 0;
    st = rfc_limits_compute_frame_bytes(&norm, 1u, &fbytes);
    if (st != RFC_OK) {
        return st;
    }
    uint64_t fpixels = 0;
    if (!mul_u64_job((uint64_t)norm.width, (uint64_t)norm.height, &fpixels)) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }

    if (!job) {
        return RFC_OK;
    }

    rfc_mutex_lock(&job->lock);
    if (grant_credit && job->in_decode_session &&
        job->decode_credit_frames != 0u) {
        job->decode_unvalidated_alloc = 1;
        if (job->decode_guard_failure == RFC_OK) {
            job->decode_guard_failure = RFC_ERR_INVALID_ARGUMENT;
        }
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (job->stream_frame_count >= RFC_MAX_FRAMES ||
        job->stream_frame_count >= limits->max_frames) {
        if (job->in_decode_session && job->decode_guard_failure == RFC_OK) {
            job->decode_guard_failure = RFC_ERR_LIMIT_EXCEEDED;
        }
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint32_t next_frames = job->stream_frame_count + 1u;
    if (job->in_decode_session && job->decode_probe_frames > 0u &&
        next_frames > job->decode_probe_frames) {
        if (job->decode_guard_failure == RFC_OK) {
            job->decode_guard_failure = RFC_ERR_INVALID_ARGUMENT;
        }
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t next_pixels = 0;
    uint64_t next_bytes = 0;
    if (!add_u64_job(job->stream_total_pixels, fpixels, &next_pixels) ||
        next_pixels > limits->max_total_pixels ||
        !add_u64_job(job->stream_total_bytes, fbytes, &next_bytes) ||
        next_bytes > limits->max_decoded_bytes) {
        if (job->in_decode_session && job->decode_guard_failure == RFC_OK) {
            job->decode_guard_failure = RFC_ERR_LIMIT_EXCEEDED;
        }
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (job->in_decode_session && job->decode_probe_exact &&
        (next_pixels > job->decode_probe_pixels ||
         next_bytes > job->decode_probe_bytes)) {
        if (job->decode_guard_failure == RFC_OK) {
            job->decode_guard_failure = RFC_ERR_LIMIT_EXCEEDED;
        }
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t comp_bytes = 0ull;
    if (job->has_decode_compressed_bytes) {
        comp_bytes = job->decode_compressed_bytes;
    } else if (job->inputs && job->inputs->source_size > 0u) {
        comp_bytes = (uint64_t)job->inputs->source_size;
    } else if (job->imported_source_size > 0u) {
        comp_bytes = (uint64_t)job->imported_source_size;
    }
    int has_comp = (comp_bytes > 0ull) ? 1 : 0;
    if (has_comp) {
        st = rfc_limits_check_decompression(limits, comp_bytes, next_bytes);
        if (st != RFC_OK) {
            if (job->in_decode_session && job->decode_guard_failure == RFC_OK) {
                job->decode_guard_failure = st;
            }
            rfc_mutex_unlock(&job->lock);
            return st;
        }
    }
    job->stream_frame_count = next_frames;
    job->stream_total_pixels = next_pixels;
    job->stream_total_bytes = next_bytes;
    if (grant_credit) {
        job->decode_credit_frames = 1u;
        job->decode_last_checked_fbytes = fbytes;
        job->decode_last_checked_icc_len = meta ? meta->icc_len : 0u;
        job->decode_last_checked_exif_len = meta ? meta->exif_len : 0u;
        job->decode_last_checked_xmp_len = meta ? meta->xmp_len : 0u;
        job->decode_last_checked_layout = norm;
    }
    rfc_mutex_unlock(&job->lock);
    return RFC_OK;
}

static rfc_status job_frame_alloc_guard(const rfc_allocator *allocator,
                                        const rfc_layout *layout,
                                        const rfc_metadata *metadata) {
    if (!allocator || !layout) {
        return RFC_OK;
    }
    rfc_job *job = NULL;
    if (allocator->alloc == job_alloc_cb && allocator->ctx) {
        job = (rfc_job *)allocator->ctx;
    } else if (g_tl_decode_job &&
               allocator != &g_tl_decode_job->frame_allocator) {
        job = g_tl_decode_job;
    }
    if (!job) {
        return RFC_OK;
    }

    rfc_mutex_lock(&job->lock);
    int in_dec = job->in_decode_session;
    int probe_exact = job->decode_probe_exact;
    rfc_limits lim = job->limits;
    rfc_mutex_unlock(&job->lock);

    if (!in_dec) {
        return RFC_OK;
    }

    size_t row_total = 0;
    rfc_status st = rfc_layout_validate(layout, &row_total);
    if (st != RFC_OK) {
        return st;
    }
    rfc_layout norm;
    st = rfc_layout_normalize_stride(layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    uint64_t fbytes = (uint64_t)row_total;
    size_t icc_len = metadata ? metadata->icc_len : 0u;
    size_t exif_len = metadata ? metadata->exif_len : 0u;
    size_t xmp_len = metadata ? metadata->xmp_len : 0u;

    rfc_mutex_lock(&job->lock);
    if (!probe_exact && job->decode_credit_frames == 0u) {
        job->decode_unvalidated_alloc = 1;
        if (job->decode_guard_failure == RFC_OK) {
            job->decode_guard_failure = RFC_ERR_INVALID_ARGUMENT;
        }
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (job->decode_credit_frames > 0u) {
        st = consume_decode_credit_locked(job, &lim, &norm, fbytes, icc_len,
                                          exif_len, xmp_len);
        rfc_mutex_unlock(&job->lock);
        return st;
    }
    rfc_mutex_unlock(&job->lock);

    return internal_exec_check_next_frame(job, &lim, &norm, metadata, 0);
}

rfc_status rfc_exec_check_next_frame(const rfc_exec_context *exec,
                                     const rfc_layout *layout,
                                     const rfc_metadata *meta) {
    if (!exec || !layout) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return internal_exec_check_next_frame(exec->job, &exec->limits, layout,
                                          meta, 1);
}

rfc_status rfc_exec_alloc_frame(const rfc_exec_context *exec,
                                rfc_frame *frame,
                                const rfc_layout *layout,
                                const rfc_metadata *metadata) {
    if (!exec || !frame || !layout) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (exec->job && rfc_job_is_cancelled(exec->job)) {
        return RFC_ERR_CANCELLED;
    }
    rfc_layout norm;
    rfc_status st = rfc_layout_normalize_stride(layout, &norm);
    if (st != RFC_OK) {
        return st;
    }
    uint64_t fbytes = 0;
    uint64_t fpixels = 0;
    int have_credit = 0;
    if (exec->job) {
        st = rfc_limits_compute_frame_bytes(&norm, 1u, &fbytes);
        if (st != RFC_OK) {
            return st;
        }
        if (!mul_u64_job((uint64_t)norm.width, (uint64_t)norm.height,
                         &fpixels)) {
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        size_t icc_len = metadata ? metadata->icc_len : 0u;
        size_t exif_len = metadata ? metadata->exif_len : 0u;
        size_t xmp_len = metadata ? metadata->xmp_len : 0u;
        rfc_mutex_lock(&exec->job->lock);
        if (exec->job->decode_credit_frames > 0u) {
            st = consume_decode_credit_locked(exec->job, &exec->limits, &norm,
                                              fbytes, icc_len, exif_len,
                                              xmp_len);
            rfc_mutex_unlock(&exec->job->lock);
            if (st != RFC_OK) {
                return st;
            }
            have_credit = 1;
        } else {
            rfc_mutex_unlock(&exec->job->lock);
        }
    }
    if (!have_credit) {
        st = internal_exec_check_next_frame(exec->job, &exec->limits, &norm,
                                            metadata, 0);
        if (st != RFC_OK) {
            return st;
        }
    }
    const rfc_allocator *alloc =
        (exec->job && exec->job->frame_allocator.alloc)
            ? &exec->job->frame_allocator
            : exec->allocator;
    st = rfc_frame_alloc(frame, &norm, metadata, alloc);
    if (st != RFC_OK && exec->job) {
        rfc_mutex_lock(&exec->job->lock);
        if (exec->job->stream_frame_count > 0u) {
            exec->job->stream_frame_count--;
        }
        if (exec->job->stream_total_pixels >= fpixels) {
            exec->job->stream_total_pixels -= fpixels;
        }
        if (exec->job->stream_total_bytes >= fbytes) {
            exec->job->stream_total_bytes -= fbytes;
        }
        rfc_mutex_unlock(&exec->job->lock);
    }
    return st;
}

static rfc_status check_stage_abort(rfc_job *job) {
    rfc_mutex_lock(&job->lock);
    int sup = job_slot_superseded_locked(job) || job->superseded ||
              job->superseded_by_id != RFC_JOB_ID_NONE;
    int can = job->cancel_requested || job->state == RFC_JOB_CANCELLED;
    rfc_mutex_unlock(&job->lock);
    if (sup) {
        return RFC_ERR_STALE_JOB;
    }
    if (can) {
        return RFC_ERR_CANCELLED;
    }
    return RFC_OK;
}

rfc_status rfc_job_internal_resolve_exactness(
    rfc_format_id target_format,
    const rfc_setting_value *settings,
    size_t setting_count,
    rfc_exactness_class *out_exact,
    rfc_metadata_policy *out_meta) {
    /* The effective exactness is the explicit setting, else the target
     * format's registry default (coefficient-exact for JPEG, pixel-exact for
     * every other format). */
    *out_exact = RFC_EXACT_PIXEL_EXACT;
    *out_meta = RFC_META_POLICY_PRESERVE;
    const char *def = rfc_registry_setting_default(target_format, "exactness");
    if (def && rfc_exactness_class_parse(def, out_exact) != RFC_OK) {
        *out_exact = RFC_EXACT_PIXEL_EXACT;
    }
    if (!settings) {
        return RFC_OK;
    }
    for (size_t i = 0u; i < setting_count; i++) {
        if (!settings[i].id || !settings[i].value) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(settings[i].id, "exactness") == 0) {
            rfc_status st =
                rfc_exactness_class_parse(settings[i].value, out_exact);
            if (st != RFC_OK) {
                return st;
            }
        } else if (strcmp(settings[i].id, "metadata") == 0) {
            rfc_status st =
                rfc_metadata_policy_parse(settings[i].value, out_meta);
            if (st != RFC_OK) {
                return st;
            }
        }
    }
    return RFC_OK;
}

static rfc_status parse_job_verify_settings(const rfc_job *job,
                                          const rfc_setting_value *settings,
                                          size_t setting_count,
                                          rfc_exactness_class *out_exact,
                                          rfc_metadata_policy *out_meta) {
    return rfc_job_internal_resolve_exactness(job->target_format, settings,
                                              setting_count, out_exact,
                                              out_meta);
}

static void store_job_verify_report(rfc_job *job,
                                    const rfc_verify_report *rep) {
    if (!job || !rep) {
        return;
    }
    rfc_mutex_lock(&job->lock);
    job->verify_report = *rep;
    rfc_mutex_unlock(&job->lock);
}

static void copy_verify_detail(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0u) {
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

static void free_non_aliasing_meta(rfc_metadata *meta,
                                     const rfc_frame *frames,
                                     size_t count,
                                     const rfc_allocator *alloc) {
    if (!meta || !alloc || !alloc->free) {
        return;
    }
    const rfc_metadata *f0 = (frames && count > 0u) ? &frames[0].metadata : NULL;
    if (meta->icc && (!f0 || f0->icc != meta->icc)) {
        alloc->free(alloc->ctx, (void *)meta->icc);
    }
    if (meta->exif && meta->exif != meta->icc &&
        (!f0 || f0->exif != meta->exif)) {
        alloc->free(alloc->ctx, (void *)meta->exif);
    }
    if (meta->xmp && meta->xmp != meta->icc && meta->xmp != meta->exif &&
        (!f0 || f0->xmp != meta->xmp)) {
        alloc->free(alloc->ctx, (void *)meta->xmp);
    }
    meta->icc = NULL;
    meta->icc_len = 0u;
    meta->exif = NULL;
    meta->exif_len = 0u;
    meta->xmp = NULL;
    meta->xmp_len = 0u;
}

static rfc_status run_bounded_session_decode(
    rfc_job *job,
    const rfc_exec_context *exec,
    rfc_format_id fmt,
    const rfc_session_adapter *sadp,
    const uint8_t *in_bytes,
    size_t in_size,
    const rfc_setting_value *dec_settings,
    size_t dec_setting_count,
    int validate_dec_settings,
    rfc_frame **out_frames,
    size_t *out_count,
    rfc_metadata *out_meta,
    const char **out_err_detail) {
    *out_frames = NULL;
    *out_count = 0u;
    memset(out_meta, 0, sizeof(*out_meta));

    void *sess = NULL;
    rfc_status st = sadp->open_session(exec, &sess);
    if (st != RFC_OK) {
        rfc_registry_release_session(fmt);
        return st;
    }

    rfc_mutex_lock(&job->lock);
    job->active_sess_adapter = sadp;
    job->active_session = sess;
    job->decode_compressed_bytes = (uint64_t)in_size;
    job->has_decode_compressed_bytes = (in_size > 0u) ? 1 : 0;
    rfc_mutex_unlock(&job->lock);

    rfc_image_probe probe;
    memset(&probe, 0, sizeof(probe));
    int probe_exact = 0;
    uint32_t exp_frames = 0u;
    uint64_t exp_pixels = 0u;
    uint64_t exp_bytes = 0u;

    st = check_stage_abort(job);
    if (st == RFC_OK) {
        st = rfc_limits_check_compressed(&job->limits, (uint64_t)in_size);
    }
    if (st == RFC_OK) {
        st = sadp->probe_session(sess, exec, in_bytes, in_size, &probe);
        if (st == RFC_OK) {
            st = rfc_limits_check_probe(&job->limits, &probe, (uint64_t)in_size,
                                        in_size > 0u);
        }
        if (st == RFC_OK) {
            if (probe.frame_count == 1u) {
                uint64_t fb = 0u;
                uint64_t fp = 0u;
                st = rfc_limits_compute_frame_bytes(&probe.first_frame, 1u, &fb);
                if (st == RFC_OK &&
                    !mul_u64_job((uint64_t)probe.first_frame.width,
                                 (uint64_t)probe.first_frame.height, &fp)) {
                    st = RFC_ERR_LIMIT_EXCEEDED;
                }
                if (st == RFC_OK) {
                    probe_exact = 1;
                    exp_frames = 1u;
                    exp_pixels = fp;
                    exp_bytes = fb;
                }
            } else if (probe.frame_count > 1u && probe.total_pixels > 0u &&
                       probe.total_decoded_bytes > 0u) {
                probe_exact = 1;
                exp_frames = probe.frame_count;
                exp_pixels = probe.total_pixels;
                exp_bytes = probe.total_decoded_bytes;
            } else {
                probe_exact = 0;
                exp_frames = probe.frame_count;
                exp_pixels = probe.total_pixels;
                exp_bytes = probe.total_decoded_bytes;
            }
        }
        if (st == RFC_OK && validate_dec_settings &&
            job->stage_ops.on_encode == NULL &&
            job->target_format != (rfc_format_id)0) {
            size_t job_sc = 0u;
            const rfc_setting_value *job_sv = rfc_job_settings(job, &job_sc);
            rfc_exactness_class t_ex = RFC_EXACT_PIXEL_EXACT;
            rfc_metadata_policy t_pol = RFC_META_POLICY_PRESERVE;
            if (rfc_job_internal_resolve_exactness(
                    job->target_format, job_sv, job_sc, &t_ex, &t_pol) ==
                    RFC_OK &&
                t_ex == RFC_EXACT_PIXEL_EXACT) {
                const rfc_capability *tcap =
                    rfc_registry_find(job->target_format);
                if (tcap && probe.first_frame.format != (rfc_sample_format)0) {
                    int sample_ok = 0;
                    for (size_t si = 0u; si < tcap->sample_count; si++) {
                        if (tcap->samples[si] == probe.first_frame.format) {
                            sample_ok = 1;
                            break;
                        }
                    }
                    if (!sample_ok) {
                        st = RFC_ERR_UNSUPPORTED_SAMPLE;
                        if (out_err_detail && !*out_err_detail) {
                            *out_err_detail =
                                "target format does not support source sample format";
                        }
                    }
                }
                if (st == RFC_OK) {
                    rfc_verify_report pre_rep;
                    memset(&pre_rep, 0, sizeof(pre_rep));
                    rfc_status mst = rfc_metadata_check_source_preflight(
                        job->target_format, t_pol, RFC_EXACT_PIXEL_EXACT,
                        &probe.container_meta, NULL, 0u,
                        rfc_job_acknowledged_metadata(job), &pre_rep.metadata);
                    if (mst == RFC_ERR_METADATA_POLICY) {
                        pre_rep.exactness = RFC_EXACT_PIXEL_EXACT;
                        copy_verify_detail(pre_rep.decoder_provenance,
                                           sizeof(pre_rep.decoder_provenance),
                                           "same-provider-fresh-session");
                        pre_rep.mismatch.kind = RFC_MISMATCH_METADATA_UNSUPPORTED;
                        copy_verify_detail(pre_rep.mismatch.detail,
                                           sizeof(pre_rep.mismatch.detail),
                                           pre_rep.metadata.detail);
                        pre_rep.meta_outcome = pre_rep.metadata.outcome;
                        pre_rep.status = mst;
                        pre_rep.valid = 1;
                        store_job_verify_report(job, &pre_rep);
                        st = mst;
                        if (out_err_detail && !*out_err_detail) {
                            *out_err_detail =
                                "unacknowledged unsupported metadata";
                        }
                    }
                }
            }
        }
        rfc_metadata_free(&probe.container_meta, exec->allocator);
    }

    rfc_frame *decoded_frames = NULL;
    size_t decoded_count = 0u;
    rfc_metadata decoded_meta;
    memset(&decoded_meta, 0, sizeof(decoded_meta));

    if (st == RFC_OK) {
        st = check_stage_abort(job);
    }
    if (st == RFC_OK && validate_dec_settings) {
        st = rfc_registry_validate_settings(
            fmt, dec_setting_count > 0u ? dec_settings : NULL,
            dec_setting_count, out_err_detail);
    }

    rfc_status guard_fail = RFC_OK;
    int unval = 0;
    uint32_t stream_frames = 0u;
    if (st == RFC_OK) {
        rfc_mutex_lock(&job->lock);
        job->stream_frame_count = 0u;
        job->stream_total_pixels = 0u;
        job->stream_total_bytes = 0u;
        job->decode_probe_exact = probe_exact;
        job->decode_probe_frames = exp_frames;
        job->decode_probe_pixels = exp_pixels;
        job->decode_probe_bytes = exp_bytes;
        job->decode_credit_frames = 0u;
        job->decode_last_checked_fbytes = 0u;
        job->decode_last_checked_icc_len = 0u;
        job->decode_last_checked_exif_len = 0u;
        job->decode_last_checked_xmp_len = 0u;
        memset(&job->decode_last_checked_layout, 0,
               sizeof(job->decode_last_checked_layout));
        job->decode_unvalidated_alloc = 0;
        job->decode_guard_failure = RFC_OK;
        job->in_decode_session = 1;
        rfc_mutex_unlock(&job->lock);

        rfc_job *prev_tl_decode_job = g_tl_decode_job;
        g_tl_decode_job = job;
        st = sadp->decode_session(
            sess, exec, in_bytes, in_size,
            dec_setting_count > 0u ? dec_settings : NULL, dec_setting_count,
            &decoded_frames, &decoded_count, &decoded_meta);
        g_tl_decode_job = prev_tl_decode_job;
    }

    rfc_mutex_lock(&job->lock);
    job->in_decode_session = 0;
    {
        /* job->lock is held here */
        const char *ad = job->adapter_detail;
        job->adapter_detail = NULL;
        if (st != RFC_OK && ad && out_err_detail && !*out_err_detail) {
            *out_err_detail = ad;
        }
    }
    job->decode_compressed_bytes = 0ull;
    job->has_decode_compressed_bytes = 0;
    guard_fail = job->decode_guard_failure;
    unval = job->decode_unvalidated_alloc;
    stream_frames = job->stream_frame_count;
    job->active_sess_adapter = NULL;
    job->active_session = NULL;
    while (job->active_cancel_calls > 0u) {
        rfc_cond_wait(&job->done_cond, &job->lock);
    }
    rfc_mutex_unlock(&job->lock);

    sadp->close_session(sess);
    rfc_registry_release_session(fmt);

    if (guard_fail != RFC_OK &&
        (st == RFC_OK || st == RFC_ERR_OUT_OF_MEMORY)) {
        st = guard_fail;
    }
    if (st == RFC_OK &&
        (unval || (!probe_exact && (size_t)stream_frames != decoded_count) ||
         (probe.frame_count > 0u &&
          decoded_count != (size_t)probe.frame_count) ||
         !decoded_frames || decoded_count == 0u)) {
        st = RFC_ERR_INVALID_ARGUMENT;
    }
    if (st == RFC_OK) {
        st = rfc_limits_check_frames(&job->limits, decoded_frames,
                                     decoded_count, &decoded_meta,
                                     (uint64_t)in_size, in_size > 0u);
    }

    if (st == RFC_OK) {
        *out_frames = decoded_frames;
        *out_count = decoded_count;
        *out_meta = decoded_meta;
    } else {
        free_non_aliasing_meta(&decoded_meta, decoded_frames, decoded_count,
                               exec->allocator);
        free_decoded_frames_array(&decoded_frames, &decoded_count,
                                  exec->allocator);
    }
    return st;
}

rfc_status rfc_job_internal_run_standalone_decode(
    rfc_format_id fmt,
    const rfc_session_adapter *sadp,
    const rfc_limits *limits,
    const rfc_allocator *alloc,
    const uint8_t *in_bytes,
    size_t in_size,
    const rfc_setting_value *dec_settings,
    size_t dec_setting_count,
    rfc_frame **out_frames,
    size_t *out_count,
    rfc_metadata *out_meta) {
    if (!sadp || !limits || !alloc || !alloc->alloc || !alloc->free ||
        !out_frames || !out_count || !out_meta) {
        if (fmt != (rfc_format_id)0 && sadp) {
            rfc_registry_release_session(fmt);
        }
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_job *tmp_job = rfc_job_internal_create();
    if (!tmp_job) {
        rfc_registry_release_session(fmt);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    tmp_job->limits = *limits;
    tmp_job->frame_allocator = *alloc;

    rfc_exec_context exec;
    memset(&exec, 0, sizeof(exec));
    exec.job = tmp_job;
    exec.codec_threads = 1u;
    exec.limits = *limits;
    exec.allocator = alloc;

    const char *err_detail = NULL;
    rfc_status st = run_bounded_session_decode(
        tmp_job, &exec, fmt, sadp, in_bytes, in_size, dec_settings,
        dec_setting_count, 0, out_frames, out_count, out_meta, &err_detail);
    if (st == RFC_OK && *out_frames) {
        for (size_t i = 0u; i < *out_count; i++) {
            if ((*out_frames)[i].allocator == &tmp_job->frame_allocator) {
                (*out_frames)[i].allocator = alloc;
            }
        }
    }
    rfc_job_release(tmp_job);
    return st;
}

static rfc_status verify_reconstructed_metadata(
    const rfc_job *job,
    rfc_metadata_policy meta_policy,
    rfc_exactness_class exactness,
    const rfc_metadata *src_meta_eff,
    uint32_t ack_meta,
    rfc_verify_report *rep) {
    rfc_frame dst_meta_frame;
    memset(&dst_meta_frame, 0, sizeof(dst_meta_frame));
    dst_meta_frame.metadata = job->verify_reconstructed_meta;
    const rfc_frame *dst_frames = NULL;
    size_t dst_frame_count = 0u;
    if (job->decoded_frame_count == 1u && job->decoded_frames) {
        dst_meta_frame.layout = job->decoded_frames[0].layout;
        dst_frames = &dst_meta_frame;
        dst_frame_count = 1u;
    }
    rfc_status st = rfc_metadata_verify(
        job->target_format, meta_policy, exactness, src_meta_eff,
        job->decoded_frames, job->decoded_frame_count,
        &job->verify_reconstructed_meta, dst_frames, dst_frame_count, ack_meta,
        &rep->metadata, &rep->mismatch);
    rep->meta_outcome = rep->metadata.outcome;
    return st;
}

/* ------------------------------------------------------------------------ */
/* JPEG -> JPEG coefficient-exact entropy optimization (D10 T02 s5)          */
/* ------------------------------------------------------------------------ */

static void jtrans_set_active(rfc_job *job,
                              const rfc_jpeg_transcode_adapter *a,
                              void *sess) {
    rfc_mutex_lock(&job->lock);
    job->active_jtrans_adapter = a;
    job->active_session = sess;
    rfc_mutex_unlock(&job->lock);
}

static void jtrans_clear_active(rfc_job *job) {
    rfc_mutex_lock(&job->lock);
    job->active_jtrans_adapter = NULL;
    job->active_session = NULL;
    while (job->active_cancel_calls > 0) {
        rfc_cond_wait(&job->done_cond, &job->lock);
    }
    rfc_mutex_unlock(&job->lock);
}

/* IMPORT: bounded probe (limits, metadata) without a pixel decode. */
static rfc_status jpeg_coeff_import(rfc_job *job, const rfc_exec_context *exec,
                                    const uint8_t *src_bytes, size_t src_size,
                                    const char **err_detail) {
    const rfc_session_adapter *sadp =
        rfc_registry_acquire_session(RFC_FORMAT_JPEG);
    if (!sadp) {
        *err_detail = "JPEG optimization requires the registered JPEG adapter";
        return RFC_ERR_NO_ADAPTER;
    }
    void *sess = NULL;
    rfc_status st = sadp->open_session(exec, &sess);
    if (st != RFC_OK) {
        rfc_registry_release_session(RFC_FORMAT_JPEG);
        return st;
    }
    rfc_mutex_lock(&job->lock);
    job->active_sess_adapter = sadp;
    job->active_session = sess;
    rfc_mutex_unlock(&job->lock);
    rfc_image_probe probe;
    memset(&probe, 0, sizeof(probe));
    st = check_stage_abort(job);
    if (st == RFC_OK) {
        st = rfc_limits_check_compressed(&job->limits, (uint64_t)src_size);
    }
    if (st == RFC_OK) {
        st = sadp->probe_session(sess, exec, src_bytes, src_size, &probe);
    }
    if (st == RFC_OK) {
        st = rfc_limits_check_probe(&job->limits, &probe, (uint64_t)src_size,
                                    src_size > 0u);
    }
    if (st == RFC_OK) {
        const rfc_jpeg_transcode_adapter *jt =
            rfc_registry_acquire_jpeg_transcode();
        if (!jt) {
            *err_detail =
                "JPEG optimization requires the registered transcode adapter";
            st = RFC_ERR_NO_ADAPTER;
        } else {
            void *jsess = NULL;
            st = jt->open_session(exec, &jsess);
            if (st == RFC_OK) {
                st = jt->preflight_optimize(jsess, exec, src_bytes, src_size);
                jt->close_session(jsess);
            }
            rfc_registry_release_jpeg_transcode();
            if (st == RFC_ERR_UNSUPPORTED_COMBINATION) {
                *err_detail =
                    "the source JPEG cannot be re-encoded with identical "
                    "coefficients and colour interpretation (a quantization "
                    "table slot redefined between scans, or a JFIF/Adobe "
                    "header after the first scan)";
            }
        }
    }
    uint64_t raw_b = 0u;
    if (st == RFC_OK) {
        st = rfc_limits_compute_frame_bytes(&probe.first_frame, 1u, &raw_b);
    }
    rfc_mutex_lock(&job->lock);
    job->active_sess_adapter = NULL;
    job->active_session = NULL;
    while (job->active_cancel_calls > 0) {
        rfc_cond_wait(&job->done_cond, &job->lock);
    }
    rfc_mutex_unlock(&job->lock);
    sadp->close_session(sess);
    rfc_registry_release_session(RFC_FORMAT_JPEG);
    if (st == RFC_OK) {
        st = check_stage_abort(job);
    }
    if (st == RFC_OK) {
        int has_meta = (probe.container_meta.icc_len > 0u ||
                        probe.container_meta.exif_len > 0u ||
                        probe.container_meta.xmp_len > 0u ||
                        probe.container_meta.orientation >
                            RFC_ORIENTATION_NORMAL);
        if (has_meta || !job->inputs || !job->inputs->has_meta) {
            st = store_decoded_frames_on_job(job, NULL, 0u,
                                             &probe.container_meta, 1);
            if (st != RFC_OK) {
                rfc_metadata_free(&probe.container_meta, exec->allocator);
            }
        } else {
            rfc_metadata_free(&probe.container_meta, exec->allocator);
        }
    } else {
        rfc_metadata_free(&probe.container_meta, exec->allocator);
    }
    if (st == RFC_OK) {
        rfc_mutex_lock(&job->lock);
        job->raw_frame_bytes = raw_b;
        rfc_mutex_unlock(&job->lock);
    }
    return st;
}

/* ENCODE: entropy-optimize the source coefficients. */
static rfc_status jpeg_coeff_encode(rfc_job *job, const rfc_exec_context *exec,
                                    const rfc_setting_value *dst_settings,
                                    size_t dst_setting_count,
                                    const uint8_t *src_bytes, size_t src_size,
                                    const char **err_detail) {
    const rfc_jpeg_transcode_adapter *jt = rfc_registry_acquire_jpeg_transcode();
    if (!jt) {
        *err_detail =
            "JPEG optimization requires the registered transcode adapter";
        return RFC_ERR_NO_ADAPTER;
    }
    void *sess = NULL;
    rfc_status st = jt->open_session(exec, &sess);
    if (st != RFC_OK) {
        rfc_registry_release_jpeg_transcode();
        return st;
    }
    jtrans_set_active(job, jt, sess);
    rfc_encoded enc_out;
    memset(&enc_out, 0, sizeof(enc_out));
    st = check_stage_abort(job);
    if (st == RFC_OK) {
        st = jt->optimize_jpeg(sess, exec, src_bytes, src_size, dst_settings,
                               dst_setting_count, &enc_out);
        if (st == RFC_ERR_UNSUPPORTED_COMBINATION) {
            *err_detail = "the source JPEG cannot be re-encoded with identical "
                          "coefficients (for example a quantization table slot "
                          "redefined between scans)";
        }
    }
    jtrans_clear_active(job);
    jt->close_session(sess);
    rfc_registry_release_jpeg_transcode();
    if (st == RFC_OK) {
        st = rfc_job_set_encoded_output(job, &enc_out);
    }
    if (st != RFC_OK) {
        rfc_encoded_free(&enc_out);
    }
    return st;
}

/* VERIFY: read source and output coefficients in separate fresh sessions,
 * check the marker contract, compare the coefficient images and the modelled
 * metadata while the adapter still owns the images, then release them
 * through the adapter's own free_coefficients. */
static rfc_status jpeg_coeff_verify(rfc_job *job, const rfc_exec_context *exec,
                                    const uint8_t *src_bytes, size_t src_size,
                                    rfc_metadata_policy meta_policy,
                                    const rfc_metadata *src_meta_eff,
                                    uint32_t ack_meta, rfc_verify_report *rep,
                                    const char **err_detail) {
    if (!src_bytes || src_size == 0u || !job->has_encoded_output ||
        !job->encoded_output.data || job->encoded_output.len == 0u) {
        *err_detail = "coefficient-exact verify requires source and output "
                      "JPEG bytes";
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const rfc_jpeg_transcode_adapter *jt = rfc_registry_acquire_jpeg_transcode();
    if (!jt) {
        *err_detail =
            "coefficient-exact verify requires the registered transcode adapter";
        return RFC_ERR_NO_ADAPTER;
    }
    rfc_bytes_hash_sha256(src_bytes, src_size, rep->source_hash);
    rfc_bytes_hash_sha256(job->encoded_output.data, job->encoded_output.len,
                          rep->verified_hash);
    struct rfc_jpeg_coeff_image *img[2] = {NULL, NULL};
    void *sess[2] = {NULL, NULL};
    rfc_metadata meta[2];
    memset(meta, 0, sizeof(meta));
    const uint8_t *in[2] = {src_bytes, job->encoded_output.data};
    size_t in_len[2] = {src_size, job->encoded_output.len};
    int strip = (meta_policy == RFC_META_POLICY_STRIP);
    rfc_status st = RFC_OK;
    /* Each image is read by its own fresh session, and that session stays
     * open until the adapter has released the image it produced. */
    for (int k = 0; k < 2 && st == RFC_OK; k++) {
        st = jt->open_session(exec, &sess[k]);
        if (st != RFC_OK) {
            sess[k] = NULL;
            break;
        }
        jtrans_set_active(job, jt, sess[k]);
        st = check_stage_abort(job);
        if (st == RFC_OK) {
            st = jt->read_coefficients(sess[k], exec, in[k], in_len[k], &img[k],
                                       &meta[k]);
        }
        if (st == RFC_OK && k == 1) {
            const char *mdet = NULL;
            st = jt->compare_markers(sess[k], exec, src_bytes, src_size, in[1],
                                     in_len[1], strip, &mdet);
            if (st == RFC_ERR_VERIFY_MISMATCH) {
                rep->mismatch.kind = strip ? RFC_MISMATCH_METADATA_RETAINED
                                           : RFC_MISMATCH_METADATA_LOST;
                copy_verify_detail(rep->mismatch.detail,
                                   sizeof(rep->mismatch.detail),
                                   mdet ? mdet : "marker contract mismatch");
                *err_detail = mdet ? mdet : "marker contract mismatch";
            }
        }
        jtrans_clear_active(job);
    }
    if (st == RFC_OK) {
        st = rfc_verify_jpeg_coefficients(img[0], img[1], &rep->mismatch);
        if (st == RFC_ERR_VERIFY_MISMATCH) {
            *err_detail = "JPEG coefficient-domain mismatch";
        }
    }
    if (st == RFC_OK) {
        st = rfc_metadata_verify(job->target_format, meta_policy,
                                 RFC_EXACT_COEFFICIENT_EXACT, src_meta_eff,
                                 NULL, 0u, &meta[1], NULL, 0u, ack_meta,
                                 &rep->metadata, &rep->mismatch);
        rep->meta_outcome = rep->metadata.outcome;
        if (st == RFC_ERR_VERIFY_MISMATCH) {
            *err_detail = "metadata policy verification failed";
        }
    }
    for (int k = 0; k < 2; k++) {
        if (sess[k]) {
            if (img[k]) {
                jt->free_coefficients(sess[k], exec, img[k]);
            }
            jt->close_session(sess[k]);
        }
    }
    rfc_registry_release_jpeg_transcode();
    rfc_metadata_free(&meta[0], exec->allocator);
    rfc_metadata_free(&meta[1], exec->allocator);
    return st;
}

void rfc_job_internal_execute(rfc_job *job, rfc_job_exec_result *out_res) {
    if (!job) {
        return;
    }

    rfc_exec_context exec;
    memset(&exec, 0, sizeof(exec));
    exec.job_id = job->job_id;
    exec.codec_threads = job->admitted_threads ? job->admitted_threads : 1u;
    exec.limits = job->limits;
    exec.allocator = &job->allocator;
    exec.budget = job->budget;
    exec.job = job;

    void *user_ctx = job->user_ctx_box ? job->user_ctx_box->user_ctx : NULL;
    const uint8_t *src_bytes = job->inputs ? job->inputs->source_bytes : NULL;
    size_t src_size = job->inputs ? job->inputs->source_size : 0u;
    const rfc_setting_value *settings =
        job->inputs ? job->inputs->settings : NULL;
    size_t setting_count = job->inputs ? job->inputs->setting_count : 0u;

    rfc_exactness_class job_exactness = RFC_EXACT_PIXEL_EXACT;
    rfc_metadata_policy job_meta_policy = RFC_META_POLICY_PRESERVE;
    (void)parse_job_verify_settings(job, settings, setting_count, &job_exactness,
                                    &job_meta_policy);
    int use_jxl_jrec =
        (job->source_format == RFC_FORMAT_JPEG &&
         job->target_format == RFC_FORMAT_JPEG_XL &&
         job_exactness == RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION &&
         job->stage_ops.on_import == NULL &&
         job->stage_ops.on_encode == NULL);
    int use_jpeg_coeff =
        (job->source_format == RFC_FORMAT_JPEG &&
         job->target_format == RFC_FORMAT_JPEG &&
         job_exactness == RFC_EXACT_COEFFICIENT_EXACT &&
         job->stage_ops.on_import == NULL &&
         job->stage_ops.on_encode == NULL);

    rfc_job_stage stage_at = RFC_STAGE_IMPORT;
    rfc_status st = check_stage_abort(job);
    const char *err_detail = NULL;
    uint64_t wall_t0 = rfc_monotonic_now_ns();
    rfc_stage_timings timings;
    memset(&timings, 0, sizeof(timings));

    rfc_mutex_lock(&job->lock);
    job->stream_frame_count = 0u;
    job->stream_total_pixels = 0u;
    job->stream_total_bytes = 0u;
    rfc_mutex_unlock(&job->lock);

    if (st != RFC_OK) {
        stage_at = RFC_STAGE_PREFLIGHT;
        goto finish;
    }

    /* Stage 1: IMPORT */
    stage_at = RFC_STAGE_IMPORT;
    st = rfc_job_advance_stage(job, job->job_id, RFC_STAGE_PREFLIGHT,
                               RFC_STAGE_IMPORT);
    if (st != RFC_OK) {
        goto finish;
    }
    uint64_t imp_t0 = rfc_monotonic_now_ns();
    timings.import_ran = 1;

    if (job->stage_ops.on_import != NULL) {
        st = job->stage_ops.on_import(job, &exec, user_ctx);
    } else {
        if (job->inputs && job->inputs->has_io_source) {
            rfc_io_source io_src_desc;
            memset(&io_src_desc, 0, sizeof(io_src_desc));
            io_src_desc.path_utf8 = job->inputs->io_source_path;
            io_src_desc.virtual_name_utf8 =
                job->inputs->io_source_virtual_name;
            io_src_desc.import_flags = job->inputs->io_import_flags;
            io_src_desc.reader = job->inputs->io_reader_box
                                     ? &job->inputs->io_reader_box->reader
                                     : NULL;
            st = rfc_input_read_job_source(
                job, &exec, &io_src_desc, job->inputs->io_reader_box,
                job->inputs->io_import_flags, job->source_format, &err_detail);
            if (st == RFC_OK && job->inputs->has_io_sink) {
                rfc_io_sink io_sink_desc;
                memset(&io_sink_desc, 0, sizeof(io_sink_desc));
                io_sink_desc.path_utf8 = job->inputs->io_sink_path;
                io_sink_desc.export_flags = job->inputs->io_export_flags;
                io_sink_desc.publisher =
                    job->inputs->io_publisher_box
                        ? &job->inputs->io_publisher_box->publisher
                        : NULL;
                st = rfc_output_validate_sink_preflight(
                    &io_sink_desc, &io_src_desc,
                    job->source_identity.valid ? &job->source_identity : NULL);
                if (st != RFC_OK && !err_detail) {
                    err_detail = rfc_status_name(st);
                }
            }
            if (st == RFC_OK) {
                src_bytes = job->imported_source_bytes;
                src_size = job->imported_source_size;
            }
        }
        const rfc_capability *src_cap =
            (st == RFC_OK) ? rfc_registry_find(job->source_format) : NULL;
        if (st != RFC_OK) {
            /* preserve read/preflight error */
        } else if (!src_cap) {
            st = RFC_ERR_UNKNOWN_FORMAT;
            err_detail = "unknown source format";
        } else if (!src_cap->can_read) {
            st = RFC_ERR_UNSUPPORTED_COMBINATION;
            err_detail = "source format does not support decoding";
        } else if (use_jpeg_coeff) {
            st = jpeg_coeff_import(job, &exec, src_bytes, src_size,
                                   &err_detail);
        } else if (use_jxl_jrec) {
            const rfc_jpeg_reconstruct_adapter *jrec =
                rfc_registry_acquire_jpeg_reconstruct();
            if (jrec) {
                void *sess = NULL;
                st = jrec->open_session(&exec, &sess);
                if (st != RFC_OK) {
                    rfc_registry_release_jpeg_reconstruct();
                } else {
                    rfc_mutex_lock(&job->lock);
                    job->active_jrec_adapter = jrec;
                    job->active_session = sess;
                    rfc_mutex_unlock(&job->lock);

                    rfc_image_probe probe;
                    memset(&probe, 0, sizeof(probe));
                    st = check_stage_abort(job);
                    if (st == RFC_OK) {
                        st = rfc_limits_check_compressed(&job->limits,
                                                         (uint64_t)src_size);
                    }
                    if (st == RFC_OK) {
                        st = jrec->probe_jpeg(sess, &exec, src_bytes, src_size,
                                              &probe);
                    }
                    if (st == RFC_OK) {
                        st = rfc_limits_check_probe(&job->limits, &probe,
                                                    (uint64_t)src_size,
                                                    src_size > 0u);
                    }
                    uint64_t raw_b = 0u;
                    if (st == RFC_OK) {
                        st = rfc_limits_compute_frame_bytes(
                            &probe.first_frame, 1u, &raw_b);
                    }

                    rfc_mutex_lock(&job->lock);
                    job->active_jrec_adapter = NULL;
                    job->active_session = NULL;
                    while (job->active_cancel_calls > 0) {
                        rfc_cond_wait(&job->done_cond, &job->lock);
                    }
                    rfc_mutex_unlock(&job->lock);

                    jrec->close_session(sess);
                    rfc_registry_release_jpeg_reconstruct();

                    if (st == RFC_OK) {
                        st = check_stage_abort(job);
                    }
                    if (st == RFC_OK) {
                        int has_probed_meta =
                            (probe.container_meta.icc_len > 0u ||
                             probe.container_meta.exif_len > 0u ||
                             probe.container_meta.xmp_len > 0u ||
                             probe.container_meta.orientation >
                                 RFC_ORIENTATION_NORMAL);
                        if (has_probed_meta || !job->inputs ||
                            !job->inputs->has_meta) {
                            st = store_decoded_frames_on_job(
                                job, NULL, 0u, &probe.container_meta, 1);
                            if (st != RFC_OK) {
                                rfc_metadata_free(&probe.container_meta,
                                                  exec.allocator);
                            }
                        } else {
                            rfc_metadata_free(&probe.container_meta,
                                              exec.allocator);
                        }
                        if (st == RFC_OK) {
                            rfc_mutex_lock(&job->lock);
                            job->raw_frame_bytes = raw_b;
                            rfc_mutex_unlock(&job->lock);
                        }
                    } else {
                        rfc_metadata_free(&probe.container_meta, exec.allocator);
                    }
                }
            } else {
                const rfc_session_adapter *sadp =
                    rfc_registry_acquire_session(job->source_format);
                if (sadp) {
                    rfc_setting_value src_settings[RFC_MAX_FORMAT_SETTINGS];
                    size_t src_setting_count = filter_format_settings(
                        src_cap, settings, setting_count, src_settings,
                        RFC_MAX_FORMAT_SETTINGS, 0);
                    rfc_frame *decoded_frames = NULL;
                    size_t decoded_count = 0u;
                    rfc_metadata decoded_meta;
                    st = run_bounded_session_decode(
                        job, &exec, job->source_format, sadp, src_bytes,
                        src_size, src_setting_count > 0u ? src_settings : NULL,
                        src_setting_count, 1, &decoded_frames, &decoded_count,
                        &decoded_meta, &err_detail);
                    if (st == RFC_OK) {
                        st = store_decoded_frames_on_job(job, decoded_frames,
                                                         decoded_count,
                                                         &decoded_meta, 1);
                        if (st != RFC_OK) {
                            free_non_aliasing_meta(&decoded_meta,
                                                   decoded_frames,
                                                   decoded_count,
                                                   exec.allocator);
                            free_decoded_frames_array(&decoded_frames,
                                                      &decoded_count,
                                                      exec.allocator);
                        }
                    }
                } else {
                    st = RFC_ERR_NO_ADAPTER;
                    err_detail =
                        "JPEG reconstruction import requires a registered adapter";
                }
            }
        } else {
            const rfc_session_adapter *sadp =
                rfc_registry_acquire_session(job->source_format);
            if (sadp) {
                rfc_setting_value src_settings[RFC_MAX_FORMAT_SETTINGS];
                size_t src_setting_count = filter_format_settings(
                    src_cap, settings, setting_count, src_settings,
                    RFC_MAX_FORMAT_SETTINGS, 0);
                rfc_frame *decoded_frames = NULL;
                size_t decoded_count = 0u;
                rfc_metadata decoded_meta;
                st = run_bounded_session_decode(
                    job, &exec, job->source_format, sadp, src_bytes, src_size,
                    src_setting_count > 0u ? src_settings : NULL,
                    src_setting_count, 1, &decoded_frames, &decoded_count,
                    &decoded_meta, &err_detail);
                if (st == RFC_OK) {
                    st = store_decoded_frames_on_job(job, decoded_frames,
                                                     decoded_count,
                                                     &decoded_meta, 1);
                    if (st != RFC_OK) {
                        free_non_aliasing_meta(&decoded_meta, decoded_frames,
                                               decoded_count, exec.allocator);
                        free_decoded_frames_array(&decoded_frames,
                                                  &decoded_count,
                                                  exec.allocator);
                    }
                }
            } else if (rfc_registry_adapter(job->source_format) != NULL) {
                st = RFC_ERR_UNSUPPORTED_COMBINATION;
                err_detail =
                    "concurrent bounded jobs require rfc_session_adapter";
            } else if (src_cap->availability == RFC_AVAILABILITY_PLANNED) {
                st = RFC_ERR_PLANNED;
                err_detail = "source format is planned and has no adapter";
            } else {
                st = RFC_ERR_NO_ADAPTER;
                err_detail = "import stage requires a configured adapter";
            }
        }
    }

    if (st == RFC_OK && job->decoded_frames != NULL &&
        job->decoded_frame_count > 0) {
        st = rfc_limits_check_frames(
            &job->limits, job->decoded_frames, job->decoded_frame_count,
            job->has_decoded_meta ? &job->decoded_meta : NULL,
            (uint64_t)src_size, src_size > 0);
        if (st == RFC_OK) {
            uint64_t raw_b = 0u;
            if (rfc_metrics_compute_raw_frame_bytes(
                    job->decoded_frames, job->decoded_frame_count, &raw_b) ==
                RFC_OK) {
                rfc_mutex_lock(&job->lock);
                job->raw_frame_bytes = raw_b;
                rfc_mutex_unlock(&job->lock);
            }
        }
    }
    timings.import_ns = rfc_monotonic_now_ns() - imp_t0;

    if (st != RFC_OK) {
        goto finish;
    }
    st = check_stage_abort(job);
    if (st != RFC_OK) {
        goto finish;
    }

    /* A pixel-exact conversion whose decoded metadata the target cannot
     * carry (and the caller did not acknowledge) is refused here, before
     * ENCODE spends its work; the report is the one VERIFY would publish. */
    if (job->stage_ops.on_encode == NULL &&
        job->decoded_frames && job->decoded_frame_count > 0u) {
        rfc_exactness_class pre_ex = RFC_EXACT_PIXEL_EXACT;
        rfc_metadata_policy pre_pol = RFC_META_POLICY_PRESERVE;
        if (parse_job_verify_settings(job, settings, setting_count, &pre_ex,
                                      &pre_pol) == RFC_OK &&
            pre_ex == RFC_EXACT_PIXEL_EXACT) {
            const rfc_metadata *pre_meta =
                job->has_decoded_meta
                    ? &job->decoded_meta
                    : (job->inputs && job->inputs->has_meta
                           ? &job->inputs->source_meta
                           : NULL);
            rfc_verify_report pre_rep;
            memset(&pre_rep, 0, sizeof(pre_rep));
            rfc_status pst = rfc_metadata_check_source_preflight(
                job->target_format, pre_pol, RFC_EXACT_PIXEL_EXACT, pre_meta,
                job->decoded_frames, job->decoded_frame_count,
                rfc_job_acknowledged_metadata(job), &pre_rep.metadata);
            if (pst == RFC_ERR_METADATA_POLICY) {
                pre_rep.exactness = RFC_EXACT_PIXEL_EXACT;
                copy_verify_detail(pre_rep.decoder_provenance,
                                   sizeof(pre_rep.decoder_provenance),
                                   "same-provider-fresh-session");
                (void)rfc_frames_hash_sha256(job->decoded_frames,
                                             job->decoded_frame_count,
                                             pre_rep.source_hash);
                pre_rep.mismatch.kind = RFC_MISMATCH_METADATA_UNSUPPORTED;
                copy_verify_detail(pre_rep.mismatch.detail,
                                   sizeof(pre_rep.mismatch.detail),
                                   pre_rep.metadata.detail);
                pre_rep.meta_outcome = pre_rep.metadata.outcome;
                pre_rep.status = pst;
                pre_rep.valid = 1;
                store_job_verify_report(job, &pre_rep);
                err_detail = "unacknowledged unsupported metadata";
                st = pst;
                goto finish;
            }
        }
    }

    /* Stage 2: ENCODE */
    stage_at = RFC_STAGE_ENCODE;
    st = rfc_job_advance_stage(job, job->job_id, RFC_STAGE_IMPORT,
                               RFC_STAGE_ENCODE);
    if (st != RFC_OK) {
        goto finish;
    }
    uint64_t enc_t0 = rfc_monotonic_now_ns();
    timings.encode_ran = 1;

    if (job->stage_ops.on_encode != NULL) {
        st = job->stage_ops.on_encode(job, &exec, user_ctx);
    } else {
        const rfc_capability *dst_cap = rfc_registry_find(job->target_format);
        if (!dst_cap) {
            st = RFC_ERR_UNKNOWN_FORMAT;
            err_detail = "unknown target format";
        } else if (!dst_cap->can_write) {
            st = RFC_ERR_UNSUPPORTED_COMBINATION;
            err_detail = "target format does not support encoding";
        } else if (job->source_format == job->target_format &&
                   job_exactness == RFC_EXACT_BYTE_IDENTICAL &&
                   job_meta_policy == RFC_META_POLICY_PRESERVE) {
            rfc_setting_value dst_settings[RFC_MAX_FORMAT_SETTINGS];
            size_t dst_setting_count = filter_format_settings(
                dst_cap, settings, setting_count, dst_settings,
                RFC_MAX_FORMAT_SETTINGS, 1);
            st = check_stage_abort(job);
            if (st == RFC_OK) {
                st = rfc_registry_validate_settings(
                    job->target_format,
                    dst_setting_count > 0 ? dst_settings : NULL,
                    dst_setting_count, &err_detail);
            }
            if (st == RFC_OK && (!src_bytes || src_size == 0u)) {
                st = RFC_ERR_INVALID_ARGUMENT;
                err_detail = "byte-identical copy requires source bytes";
            }
            if (st == RFC_OK) {
                st = rfc_limits_check_compressed(&job->limits,
                                                 (uint64_t)src_size);
            }
            if (st == RFC_OK) {
                uint8_t *copy_buf = (uint8_t *)exec.allocator->alloc(
                    exec.allocator->ctx, src_size);
                if (!copy_buf) {
                    st = RFC_ERR_OUT_OF_MEMORY;
                } else {
                    memcpy(copy_buf, src_bytes, src_size);
                    rfc_encoded enc_out = {copy_buf, src_size, exec.allocator};
                    st = rfc_job_set_encoded_output(job, &enc_out);
                    if (st != RFC_OK) {
                        rfc_encoded_free(&enc_out);
                    }
                }
            }
        } else if (use_jpeg_coeff) {
            rfc_setting_value dst_settings[RFC_MAX_FORMAT_SETTINGS];
            size_t dst_setting_count = filter_format_settings(
                dst_cap, settings, setting_count, dst_settings,
                RFC_MAX_FORMAT_SETTINGS, 1);
            st = check_stage_abort(job);
            if (st == RFC_OK) {
                st = rfc_registry_validate_settings(
                    job->target_format,
                    dst_setting_count > 0 ? dst_settings : NULL,
                    dst_setting_count, &err_detail);
            }
            if (st == RFC_OK) {
                st = jpeg_coeff_encode(job, &exec,
                                       dst_setting_count > 0 ? dst_settings
                                                             : NULL,
                                       dst_setting_count, src_bytes, src_size,
                                       &err_detail);
            }
        } else if (use_jxl_jrec) {
            const rfc_jpeg_reconstruct_adapter *jrec =
                rfc_registry_acquire_jpeg_reconstruct();
            if (!jrec) {
                st = RFC_ERR_NO_ADAPTER;
                err_detail =
                    "JPEG -> JPEG XL transcode requires a registered reconstruction adapter";
            } else {
                void *sess = NULL;
                st = jrec->open_session(&exec, &sess);
                if (st != RFC_OK) {
                    rfc_registry_release_jpeg_reconstruct();
                } else {
                    rfc_mutex_lock(&job->lock);
                    job->active_jrec_adapter = jrec;
                    job->active_session = sess;
                    rfc_mutex_unlock(&job->lock);

                    rfc_encoded enc_out;
                    memset(&enc_out, 0, sizeof(enc_out));
                    rfc_metadata trans_meta;
                    memset(&trans_meta, 0, sizeof(trans_meta));

                    rfc_setting_value dst_settings[RFC_MAX_FORMAT_SETTINGS];
                    size_t dst_setting_count = filter_format_settings(
                        dst_cap, settings, setting_count, dst_settings,
                        RFC_MAX_FORMAT_SETTINGS, 1);

                    st = check_stage_abort(job);
                    if (st == RFC_OK) {
                        st = rfc_registry_validate_settings(
                            job->target_format,
                            dst_setting_count > 0 ? dst_settings : NULL,
                            dst_setting_count, &err_detail);
                    }
                    if (st == RFC_OK) {
                        st = jrec->transcode_jpeg_to_jxl(
                            sess, &exec, src_bytes, src_size,
                            dst_setting_count > 0 ? dst_settings : NULL,
                            dst_setting_count, &enc_out, &trans_meta);
                    }

                    rfc_mutex_lock(&job->lock);
                    job->active_jrec_adapter = NULL;
                    job->active_session = NULL;
                    while (job->active_cancel_calls > 0) {
                        rfc_cond_wait(&job->done_cond, &job->lock);
                    }
                    rfc_mutex_unlock(&job->lock);

                    jrec->close_session(sess);
                    rfc_registry_release_jpeg_reconstruct();

                    if (st == RFC_OK) {
                        st = rfc_job_set_encoded_output(job, &enc_out);
                        if (st != RFC_OK) {
                            rfc_encoded_free(&enc_out);
                        }
                    } else {
                        rfc_encoded_free(&enc_out);
                    }
                    if (st == RFC_OK) {
                        int has_trans_meta =
                            (trans_meta.icc_len > 0u ||
                             trans_meta.exif_len > 0u ||
                             trans_meta.xmp_len > 0u ||
                             trans_meta.orientation > RFC_ORIENTATION_NORMAL);
                        if (has_trans_meta || !job->inputs ||
                            !job->inputs->has_meta) {
                            st = store_decoded_frames_on_job(
                                job, NULL, 0u, &trans_meta, 1);
                            if (st != RFC_OK) {
                                rfc_metadata_free(&trans_meta, exec.allocator);
                            }
                        } else {
                            rfc_metadata_free(&trans_meta, exec.allocator);
                        }
                    } else {
                        rfc_metadata_free(&trans_meta, exec.allocator);
                    }
                }
            }
        } else {
            const rfc_session_adapter *sadp =
                rfc_registry_acquire_session(job->target_format);
            if (sadp) {
                if (!job->decoded_frames || job->decoded_frame_count == 0) {
                    st = RFC_ERR_INVALID_ARGUMENT;
                    rfc_registry_release_session(job->target_format);
                } else {
                    void *sess = NULL;
                    st = sadp->open_session(&exec, &sess);
                    if (st == RFC_OK) {
                        rfc_mutex_lock(&job->lock);
                        job->active_sess_adapter = sadp;
                        job->active_session = sess;
                        rfc_mutex_unlock(&job->lock);

                        rfc_encoded enc_out;
                        memset(&enc_out, 0, sizeof(enc_out));

                        rfc_setting_value dst_settings[RFC_MAX_FORMAT_SETTINGS];
                        size_t dst_setting_count = filter_format_settings(
                            dst_cap, settings, setting_count, dst_settings,
                            RFC_MAX_FORMAT_SETTINGS, 1);

                        st = check_stage_abort(job);
                        if (st == RFC_OK) {
                            st = rfc_registry_validate_settings(
                                job->target_format,
                                dst_setting_count > 0 ? dst_settings : NULL,
                                dst_setting_count, &err_detail);
                        }
                        if (st == RFC_OK) {
                            st = sadp->encode_session(
                                sess, &exec, job->decoded_frames,
                                job->decoded_frame_count,
                                job->has_decoded_meta ? &job->decoded_meta
                                                      : NULL,
                                dst_setting_count > 0 ? dst_settings : NULL,
                                dst_setting_count, &enc_out);
                        }
                        {
                            const char *ad = take_adapter_detail(job);
                            if (st != RFC_OK && !err_detail && ad) {
                                err_detail = ad;
                            }
                        }

                        rfc_mutex_lock(&job->lock);
                        job->active_sess_adapter = NULL;
                        job->active_session = NULL;
                        while (job->active_cancel_calls > 0) {
                            rfc_cond_wait(&job->done_cond, &job->lock);
                        }
                        rfc_mutex_unlock(&job->lock);

                        if (st == RFC_OK) {
                            if (sadp->free_encoded) {
                                rfc_mutex_lock(&job->lock);
                                job->encoded_output = enc_out;
                                job->has_encoded_output = 1;
                                job->encoded_owner_sess = sadp;
                                job->encoded_owner_handle = sess;
                                job->encoded_owner_fmt = job->target_format;
                                rfc_mutex_unlock(&job->lock);
                            } else {
                                st = rfc_job_set_encoded_output(job, &enc_out);
                                if (st != RFC_OK && enc_out.allocator) {
                                    rfc_encoded_free(&enc_out);
                                }
                                sadp->close_session(sess);
                                rfc_registry_release_session(job->target_format);
                            }
                        } else {
                            if (sadp->free_encoded) {
                                sadp->free_encoded(sess, &enc_out);
                            } else {
                                rfc_encoded_free(&enc_out);
                            }
                            sadp->close_session(sess);
                            rfc_registry_release_session(job->target_format);
                        }
                    } else {
                        rfc_registry_release_session(job->target_format);
                    }
                }
            } else if (rfc_registry_adapter(job->target_format) != NULL) {
                st = RFC_ERR_UNSUPPORTED_COMBINATION;
                err_detail =
                    "concurrent bounded jobs require rfc_session_adapter";
            } else if (dst_cap->availability == RFC_AVAILABILITY_PLANNED) {
                st = RFC_ERR_PLANNED;
                err_detail = "target format is planned and has no adapter";
            } else {
                st = RFC_ERR_NO_ADAPTER;
                err_detail = "encode stage requires a configured adapter";
            }
        }
    }

    timings.encode_ns = rfc_monotonic_now_ns() - enc_t0;
    if (st != RFC_OK) {
        goto finish;
    }
    st = check_stage_abort(job);
    if (st != RFC_OK) {
        goto finish;
    }

    /* Stage 3: VERIFY */
    stage_at = RFC_STAGE_VERIFY;
    st = rfc_job_advance_stage(job, job->job_id, RFC_STAGE_ENCODE,
                               RFC_STAGE_VERIFY);
    if (st != RFC_OK) {
        goto finish;
    }
    uint64_t ver_t0 = rfc_monotonic_now_ns();
    timings.verify_ran = 1;

    if (job->stage_ops.on_verify != NULL) {
        st = job->stage_ops.on_verify(job, &exec, user_ctx);
    } else {
        rfc_exactness_class exactness = RFC_EXACT_PIXEL_EXACT;
        rfc_metadata_policy meta_policy = RFC_META_POLICY_PRESERVE;
        st = parse_job_verify_settings(job, settings, setting_count, &exactness,
                                       &meta_policy);
        uint32_t ack_meta = rfc_job_acknowledged_metadata(job);
        const rfc_metadata *src_meta_eff =
            job->has_decoded_meta
                ? &job->decoded_meta
                : (job->inputs && job->inputs->has_meta
                       ? &job->inputs->source_meta
                       : NULL);
        rfc_verify_report rep;
        memset(&rep, 0, sizeof(rep));
        rep.exactness = exactness;
        rep.independent_decoder = 0;
        copy_verify_detail(rep.decoder_provenance,
                           sizeof(rep.decoder_provenance),
                           "same-provider-fresh-session");

        if (st == RFC_OK && exactness == RFC_EXACT_PIXEL_EXACT) {
            const rfc_session_adapter *sadp =
                rfc_registry_acquire_session(job->target_format);
            if (!sadp) {
                st = RFC_ERR_NO_ADAPTER;
                err_detail = "verify stage requires an exactness verifier";
            } else if (!job->decoded_frames || job->decoded_frame_count == 0u ||
                       !job->has_encoded_output || !job->encoded_output.data ||
                       job->encoded_output.len == 0u) {
                rfc_registry_release_session(job->target_format);
                st = RFC_ERR_INVALID_ARGUMENT;
                err_detail =
                    "verify stage requires decoded source and encoded output";
            } else {
                st = rfc_frames_hash_sha256(job->decoded_frames,
                                            job->decoded_frame_count,
                                            rep.source_hash);
                if (st == RFC_OK) {
                    st = rfc_metadata_check_source_preflight(
                        job->target_format, meta_policy, RFC_EXACT_PIXEL_EXACT,
                        src_meta_eff, job->decoded_frames,
                        job->decoded_frame_count, ack_meta, &rep.metadata);
                }
                if (st != RFC_OK) {
                    rfc_registry_release_session(job->target_format);
                    if (st == RFC_ERR_METADATA_POLICY) {
                        rep.mismatch.kind = RFC_MISMATCH_METADATA_UNSUPPORTED;
                        copy_verify_detail(rep.mismatch.detail,
                                           sizeof(rep.mismatch.detail),
                                           rep.metadata.detail);
                        err_detail = "unacknowledged unsupported metadata";
                    }
                    rep.meta_outcome = rep.metadata.outcome;
                    rep.status = st;
                    rep.valid = 1;
                    store_job_verify_report(job, &rep);
                } else {
                    rfc_frame *ver_frames = NULL;
                    size_t ver_count = 0u;
                    rfc_metadata ver_meta;
                    memset(&ver_meta, 0, sizeof(ver_meta));
                    st = run_bounded_session_decode(
                        job, &exec, job->target_format, sadp,
                        job->encoded_output.data, job->encoded_output.len, NULL,
                        0u, 0, &ver_frames, &ver_count, &ver_meta,
                        &err_detail);
                    if (st == RFC_OK) {
                        st = rfc_frames_hash_sha256(ver_frames, ver_count,
                                                    rep.verified_hash);
                    }
                    if (st == RFC_OK) {
                        st = rfc_verify_frames_equal(
                            job->decoded_frames, job->decoded_frame_count,
                            ver_frames, ver_count, &rep.mismatch);
                        if (st == RFC_ERR_VERIFY_MISMATCH) {
                            err_detail = "fresh decode sample or frame "
                                         "mismatch";
                        }
                    }
                    if (st == RFC_OK) {
                        st = rfc_metadata_verify(
                            job->target_format, meta_policy,
                            RFC_EXACT_PIXEL_EXACT, src_meta_eff,
                            job->decoded_frames, job->decoded_frame_count,
                            &ver_meta, ver_frames, ver_count, ack_meta,
                            &rep.metadata, &rep.mismatch);
                        rep.meta_outcome = rep.metadata.outcome;
                        if (st == RFC_ERR_VERIFY_MISMATCH) {
                            err_detail = "metadata policy verification failed";
                        }
                    }
                    free_non_aliasing_meta(&ver_meta, ver_frames, ver_count,
                                           exec.allocator);
                    free_decoded_frames_array(&ver_frames, &ver_count,
                                              exec.allocator);
                    if (st != RFC_ERR_CANCELLED && st != RFC_ERR_STALE_JOB) {
                        rep.status = st;
                        rep.valid = 1;
                        store_job_verify_report(job, &rep);
                    }
                }
            }
        } else if (st == RFC_OK && exactness == RFC_EXACT_BYTE_IDENTICAL) {
            if (meta_policy == RFC_META_POLICY_STRIP) {
                st = RFC_ERR_UNSUPPORTED_COMBINATION;
                err_detail = "metadata=strip cannot claim byte-identical";
            } else if (!src_bytes || src_size == 0u ||
                       !job->has_encoded_output) {
                st = RFC_ERR_INVALID_ARGUMENT;
                err_detail =
                    "byte-identical verify requires source and encoded bytes";
            } else {
                st = rfc_verify_bytes_exact(
                    src_bytes, src_size, job->encoded_output.data,
                    job->encoded_output.len, RFC_EXACT_BYTE_IDENTICAL,
                    "same-provider-fresh-session", 0, &rep);
                if (st == RFC_OK) {
                    st = rfc_metadata_verify(
                        job->target_format, meta_policy,
                        RFC_EXACT_BYTE_IDENTICAL, src_meta_eff,
                        job->decoded_frames, job->decoded_frame_count,
                        src_meta_eff, job->decoded_frames,
                        job->decoded_frame_count, ack_meta, &rep.metadata,
                        &rep.mismatch);
                    rep.meta_outcome = rep.metadata.outcome;
                } else {
                    err_detail = "output bytes differ from source bytes";
                }
                rep.status = st;
                rep.valid = 1;
                store_job_verify_report(job, &rep);
            }
        } else if (st == RFC_OK && exactness == RFC_EXACT_COEFFICIENT_EXACT) {
            if (use_jpeg_coeff) {
                st = jpeg_coeff_verify(job, &exec, src_bytes, src_size,
                                       meta_policy, src_meta_eff, ack_meta,
                                       &rep, &err_detail);
                if (st != RFC_ERR_CANCELLED && st != RFC_ERR_STALE_JOB) {
                    rep.status = st;
                    rep.valid = 1;
                    store_job_verify_report(job, &rep);
                }
            } else if (job->source_format != RFC_FORMAT_JPEG &&
                job->target_format != RFC_FORMAT_JPEG) {
                st = RFC_ERR_UNSUPPORTED_COMBINATION;
                err_detail = "coefficient-exact requires a JPEG source or "
                             "target";
            } else if (!job->verify_src_coeffs || !job->verify_dst_coeffs ||
                       !job->has_verify_reconstructed_meta) {
                st = RFC_ERR_NO_ADAPTER;
                err_detail =
                    "coefficient-exact verify requires JPEG coefficient and "
                    "output metadata evidence";
            } else {
                if (src_bytes && src_size > 0u) {
                    rfc_bytes_hash_sha256(src_bytes, src_size, rep.source_hash);
                }
                if (job->has_encoded_output && job->encoded_output.data) {
                    rfc_bytes_hash_sha256(job->encoded_output.data,
                                          job->encoded_output.len,
                                          rep.verified_hash);
                }
                st = rfc_verify_jpeg_coefficients(job->verify_src_coeffs,
                                                  job->verify_dst_coeffs,
                                                  &rep.mismatch);
                if (st == RFC_OK) {
                    st = verify_reconstructed_metadata(
                        job, meta_policy, RFC_EXACT_COEFFICIENT_EXACT,
                        src_meta_eff, ack_meta, &rep);
                } else {
                    err_detail = "JPEG coefficient-domain mismatch";
                }
                rep.status = st;
                rep.valid = 1;
                store_job_verify_report(job, &rep);
            }
        } else if (st == RFC_OK &&
                   exactness == RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION) {
            if (meta_policy == RFC_META_POLICY_STRIP ||
                job->source_format != RFC_FORMAT_JPEG ||
                job->target_format != RFC_FORMAT_JPEG_XL) {
                st = RFC_ERR_UNSUPPORTED_COMBINATION;
                err_detail =
                    "original-byte-reconstruction requires JPEG XL with "
                    "preserved metadata";
            } else if (use_jxl_jrec && !job->has_verify_reconstructed) {
                if (!src_bytes || src_size == 0u || !job->has_encoded_output ||
                    !job->encoded_output.data ||
                    job->encoded_output.len == 0u) {
                    st = RFC_ERR_INVALID_ARGUMENT;
                    err_detail =
                        "original-byte-reconstruction verify requires source JPEG and encoded JPEG XL bytes";
                } else {
                    st = rfc_metadata_check_source_preflight(
                        job->target_format, meta_policy,
                        RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION, src_meta_eff,
                        job->decoded_frames, job->decoded_frame_count, ack_meta,
                        &rep.metadata);
                    if (st != RFC_OK) {
                        if (st == RFC_ERR_METADATA_POLICY) {
                            rep.mismatch.kind =
                                RFC_MISMATCH_METADATA_UNSUPPORTED;
                            copy_verify_detail(rep.mismatch.detail,
                                               sizeof(rep.mismatch.detail),
                                               rep.metadata.detail);
                            err_detail = "unacknowledged unsupported metadata";
                        }
                        rep.meta_outcome = rep.metadata.outcome;
                        rep.status = st;
                        rep.valid = 1;
                        store_job_verify_report(job, &rep);
                    } else {
                        const rfc_jpeg_reconstruct_adapter *jrec =
                            rfc_registry_acquire_jpeg_reconstruct();
                        if (!jrec) {
                            st = RFC_ERR_NO_ADAPTER;
                            err_detail =
                                "original-byte-reconstruction verify requires a reconstruction adapter";
                        } else {
                            void *sess = NULL;
                            st = jrec->open_session(&exec, &sess);
                            if (st != RFC_OK) {
                                rfc_registry_release_jpeg_reconstruct();
                            } else {
                                rfc_mutex_lock(&job->lock);
                                job->active_jrec_adapter = jrec;
                                job->active_session = sess;
                                rfc_mutex_unlock(&job->lock);

                                rfc_encoded rec_jpeg;
                                memset(&rec_jpeg, 0, sizeof(rec_jpeg));
                                rfc_metadata rec_meta;
                                memset(&rec_meta, 0, sizeof(rec_meta));
                                st = check_stage_abort(job);
                                if (st == RFC_OK) {
                                    st = jrec->reconstruct_jpeg_from_jxl(
                                        sess, &exec, job->encoded_output.data,
                                        job->encoded_output.len, &rec_jpeg,
                                        &rec_meta);
                                }

                                rfc_mutex_lock(&job->lock);
                                job->active_jrec_adapter = NULL;
                                job->active_session = NULL;
                                while (job->active_cancel_calls > 0) {
                                    rfc_cond_wait(&job->done_cond, &job->lock);
                                }
                                rfc_mutex_unlock(&job->lock);

                                jrec->close_session(sess);
                                rfc_registry_release_jpeg_reconstruct();

                                if (st == RFC_OK) {
                                    st = rfc_job_set_verified_reconstructed_bytes(
                                        job, rec_jpeg.data, rec_jpeg.len,
                                        &rec_meta);
                                }
                                rfc_encoded_free(&rec_jpeg);
                                rfc_metadata_free(&rec_meta, exec.allocator);
                                if (st == RFC_OK) {
                                    st = rfc_verify_original_byte_reconstruction(
                                        src_bytes, src_size,
                                        job->verify_reconstructed_bytes,
                                        job->verify_reconstructed_len, &rep);
                                    if (st == RFC_OK) {
                                        st = verify_reconstructed_metadata(
                                            job, meta_policy,
                                            RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION,
                                            src_meta_eff, ack_meta, &rep);
                                    } else {
                                        err_detail =
                                            "reconstructed original JPEG bytes SHA-256 differs";
                                    }
                                }
                                if (st != RFC_ERR_CANCELLED &&
                                    st != RFC_ERR_STALE_JOB) {
                                    rep.status = st;
                                    rep.valid = 1;
                                    store_job_verify_report(job, &rep);
                                }
                            }
                        }
                    }
                }
            } else if (!job->has_verify_reconstructed ||
                       !job->has_verify_reconstructed_meta) {
                st = RFC_ERR_NO_ADAPTER;
                err_detail =
                    "original-byte-reconstruction requires reconstructed "
                    "original JPEG bytes and output metadata evidence";
            } else if (!src_bytes || src_size == 0u ||
                       !job->verify_reconstructed_bytes ||
                       job->verify_reconstructed_len == 0u) {
                st = RFC_ERR_INVALID_ARGUMENT;
                err_detail =
                    "original-byte-reconstruction requires non-empty JPEG "
                    "bytes";
            } else {
                st = rfc_verify_original_byte_reconstruction(
                    src_bytes, src_size, job->verify_reconstructed_bytes,
                    job->verify_reconstructed_len, &rep);
                if (st == RFC_OK) {
                    st = verify_reconstructed_metadata(
                        job, meta_policy,
                        RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION, src_meta_eff,
                        ack_meta, &rep);
                } else {
                    err_detail =
                        "reconstructed original JPEG bytes SHA-256 differs";
                }
                rep.status = st;
                rep.valid = 1;
                store_job_verify_report(job, &rep);
            }
        }
    }

    timings.verify_ns = rfc_monotonic_now_ns() - ver_t0;
    if (st != RFC_OK) {
        goto finish;
    }
    st = check_stage_abort(job);
    if (st != RFC_OK) {
        goto finish;
    }

    /* Stage 4: EXPORT (never invoked if already cancelled or superseded) */
    stage_at = RFC_STAGE_EXPORT;
    st = rfc_job_advance_stage(job, job->job_id, RFC_STAGE_VERIFY,
                               RFC_STAGE_EXPORT);
    if (st != RFC_OK) {
        goto finish;
    }
    st = check_stage_abort(job);
    if (st != RFC_OK) {
        goto finish;
    }
    uint64_t exp_t0 = rfc_monotonic_now_ns();
    timings.export_ran = 1;

    if (job->stage_ops.on_export != NULL) {
        st = job->stage_ops.on_export(job, &exec, user_ctx);
    } else if (job->inputs && job->inputs->has_io_sink) {
        st = rfc_output_publish_job(job, &exec, &err_detail);
    } else {
        st = RFC_ERR_NO_ADAPTER;
        err_detail = "export stage requires an output publisher";
    }
    timings.export_ns = rfc_monotonic_now_ns() - exp_t0;

finish:
    timings.wall_ns = rfc_monotonic_now_ns() - wall_t0;
    timings.valid = 1;
    rfc_job_internal_release_stage_outputs(job);

    /* Free all attached partials before transitioning to terminal state. */
    rfc_job_release_partials(job);

    if (out_res) {
        rfc_mutex_lock(&job->lock);
        job->stage_timings = timings;
        rfc_mutex_unlock(&job->lock);
        out_res->st = st;
        out_res->stage_at = stage_at;
        out_res->err_detail = err_detail;
        return;
    }

    rfc_mutex_lock(&job->lock);
    job->stage_timings = timings;
    int abort_sup = job->superseded || job->superseded_by_id != RFC_JOB_ID_NONE;
    int abort_can = job->cancel_requested || job->state == RFC_JOB_CANCELLED;
    if (job->publish_decided) {
        if (job->published && st == RFC_OK) {
            job->stage = RFC_STAGE_DONE;
            job->state = RFC_JOB_SUCCEEDED;
            job->status = RFC_OK;
            job->failing_stage = RFC_STAGE_NONE;
            if (job->detail[0] == '\0') {
                rfc_job_internal_set_detail(job->detail, "ok");
            }
        } else {
            job->state = RFC_JOB_FAILED;
            job->status = (st != RFC_OK) ? st : RFC_ERR_IO;
            job->failing_stage =
                (stage_at != RFC_STAGE_NONE) ? stage_at : RFC_STAGE_EXPORT;
            if (job->detail[0] == '\0') {
                rfc_job_internal_set_detail(
                    job->detail,
                    err_detail ? err_detail : rfc_status_name(job->status));
            }
        }
    } else if (job->published && st == RFC_OK) {
        job->stage = RFC_STAGE_DONE;
        job->state = RFC_JOB_SUCCEEDED;
        job->status = RFC_OK;
        job->failing_stage = RFC_STAGE_NONE;
        if (job->detail[0] == '\0') {
            rfc_job_internal_set_detail(job->detail, "ok");
        }
    } else if (abort_sup) {
        job->state = RFC_JOB_SUPERSEDED;
        job->status = RFC_ERR_STALE_JOB;
        job->failing_stage = stage_at;
        job->suppress_callbacks = 1;
        rfc_job_internal_set_detail(job->detail, "superseded by retry");
    } else if (abort_can || st == RFC_ERR_CANCELLED) {
        job->state = RFC_JOB_CANCELLED;
        job->status = RFC_ERR_CANCELLED;
        job->failing_stage = stage_at;
        job->suppress_callbacks = 1;
        rfc_job_internal_set_detail(job->detail, "cancelled");
        if (job->published && job->sched) {
            rfc_scheduler_internal_on_publish_cancelled(job->sched);
        }
    } else if (st == RFC_OK) {
        job->stage = RFC_STAGE_DONE;
        job->state = RFC_JOB_SUCCEEDED;
        job->status = RFC_OK;
        job->failing_stage = RFC_STAGE_NONE;
        if (job->detail[0] == '\0') {
            rfc_job_internal_set_detail(job->detail, "ok");
        }
    } else {
        job->state = RFC_JOB_FAILED;
        job->status = st;
        job->failing_stage = stage_at;
        if (job->detail[0] == '\0') {
            rfc_job_internal_set_detail(
                job->detail, err_detail ? err_detail : rfc_status_name(st));
        }
    }
    job->suppress_callbacks = 1;
    rfc_cond_broadcast(&job->done_cond);
    rfc_mutex_unlock(&job->lock);
}

const char *rfc_job_state_name(rfc_job_state state) {
    switch (state) {
        case RFC_JOB_QUEUED:
            return "queued";
        case RFC_JOB_RUNNING:
            return "running";
        case RFC_JOB_SUCCEEDED:
            return "succeeded";
        case RFC_JOB_FAILED:
            return "failed";
        case RFC_JOB_CANCELLED:
            return "cancelled";
        case RFC_JOB_SUPERSEDED:
            return "superseded";
        default:
            return "unknown-state";
    }
}

const char *rfc_job_stage_name(rfc_job_stage stage) {
    switch (stage) {
        case RFC_STAGE_NONE:
            return "none";
        case RFC_STAGE_PREFLIGHT:
            return "preflight";
        case RFC_STAGE_IMPORT:
            return "import";
        case RFC_STAGE_ENCODE:
            return "encode";
        case RFC_STAGE_VERIFY:
            return "verify";
        case RFC_STAGE_EXPORT:
            return "export";
        case RFC_STAGE_DONE:
            return "done";
        default:
            return "unknown-stage";
    }
}
