#include "job_internal.h"

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

rfc_status rfc_job_internal_compute_retained_bytes(
    const uint8_t *src_bytes,
    size_t src_size,
    const rfc_metadata *src_meta,
    const rfc_setting_value *settings,
    size_t setting_count,
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
    *out_bytes = total;
    return RFC_OK;
}

static void inputs_free_contents(rfc_job_inputs *in) {
    if (!in) {
        return;
    }
    free(in->source_bytes);
    free(in->owned_icc);
    if (in->settings) {
        for (size_t i = 0; i < in->setting_count; i++) {
            free((void *)in->settings[i].id);
            free((void *)in->settings[i].value);
        }
        free(in->settings);
    }
    free(in);
}

rfc_job_inputs *rfc_job_inputs_create(rfc_scheduler *alloc_counter,
                                      const uint8_t *src_bytes,
                                      size_t src_size,
                                      const rfc_layout *src_layout,
                                      const rfc_metadata *src_meta,
                                      const rfc_setting_value *settings,
                                      size_t setting_count) {
    uint64_t retained = 0;
    if (rfc_job_internal_compute_retained_bytes(src_bytes, src_size, src_meta,
                                                settings, setting_count,
                                                &retained) != RFC_OK) {
        return NULL;
    }

    rfc_scheduler_internal_note_submit_alloc(alloc_counter);
    rfc_job_inputs *in = (rfc_job_inputs *)calloc(1u, sizeof(*in));
    if (!in) {
        return NULL;
    }
    atomic_init(&in->refs, 1u);
    in->source_size = src_size;
    in->setting_count = setting_count;
    in->retained_bytes = retained;

    if (src_size > 0 && src_bytes != NULL) {
        rfc_scheduler_internal_note_submit_alloc(alloc_counter);
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
        in->has_meta = 1;
        if (src_meta->icc_len > 0 && src_meta->icc != NULL) {
            rfc_scheduler_internal_note_submit_alloc(alloc_counter);
            in->owned_icc = (uint8_t *)malloc(src_meta->icc_len);
            if (!in->owned_icc) {
                inputs_free_contents(in);
                return NULL;
            }
            memcpy(in->owned_icc, src_meta->icc, src_meta->icc_len);
            in->source_meta.icc = in->owned_icc;
        }
    }

    if (setting_count > 0 && settings != NULL) {
        rfc_scheduler_internal_note_submit_alloc(alloc_counter);
        in->settings =
            (rfc_setting_value *)calloc(setting_count, sizeof(*in->settings));
        if (!in->settings) {
            inputs_free_contents(in);
            return NULL;
        }
        for (size_t i = 0; i < setting_count; i++) {
            in->settings[i].id =
                settings[i].id ? dup_cstr_counted(settings[i].id, alloc_counter)
                               : NULL;
            in->settings[i].value =
                settings[i].value
                    ? dup_cstr_counted(settings[i].value, alloc_counter)
                    : NULL;
            if ((settings[i].id && !in->settings[i].id) ||
                (settings[i].value && !in->settings[i].value)) {
                inputs_free_contents(in);
                return NULL;
            }
        }
    }

    return in;
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
    if (out_size) {
        *out_size = (job && job->inputs) ? job->inputs->source_size : 0u;
    }
    return (job && job->inputs) ? job->inputs->source_bytes : NULL;
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
    rfc_encoded enc = job->encoded_output;
    int has_enc = job->has_encoded_output;
    const rfc_session_adapter *enc_sess = job->encoded_owner_sess;
    void *enc_handle = job->encoded_owner_handle;
    rfc_format_id enc_fmt = job->encoded_owner_fmt;

    job->decoded_frames = NULL;
    job->decoded_frame_count = 0;
    job->decoded_frames_alloc = NULL;
    job->decoded_meta_icc = NULL;
    job->has_decoded_meta = 0;
    memset(&job->decoded_meta, 0, sizeof(job->decoded_meta));
    job->has_encoded_output = 0;
    memset(&job->encoded_output, 0, sizeof(job->encoded_output));
    job->encoded_owner_sess = NULL;
    job->encoded_owner_handle = NULL;
    job->encoded_owner_fmt = 0;
    rfc_mutex_unlock(&job->lock);

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

    job->decoded_frames = frames;
    job->decoded_frame_count = frame_count;
    job->decoded_frames_alloc =
        (frame_count > 0 && frames[0].allocator) ? frames[0].allocator
                                                 : &job->allocator;
    job->decoded_meta = new_meta;
    job->decoded_meta_icc = owned_icc;
    job->has_decoded_meta = has_meta;
    rfc_mutex_unlock(&job->lock);

    if (old_icc) {
        job->allocator.free(job->allocator.ctx, old_icc);
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
        uint64_t comp_sz =
            job->inputs ? (uint64_t)job->inputs->source_size : 0ull;
        int has_comp = (job->inputs && job->inputs->source_size > 0) ? 1 : 0;
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
    if (!job->published && !job_is_terminal_state(job->state)) {
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

    rfc_mutex_lock(&job->lock);
    e->next = job->partials;
    job->partials = e;
    job->live_partial_bytes += (uint64_t)byte_size;
    rfc_scheduler *sched = job->sched;
    rfc_mutex_unlock(&job->lock);

    if (sched && byte_size > 0) {
        rfc_scheduler_internal_adjust_live_partials(sched, (int64_t)byte_size);
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
    rfc_mutex_lock(&job->lock);
    rfc_partial_entry *list = job->partials;
    job->partials = NULL;
    uint64_t freed = job->live_partial_bytes;
    job->live_partial_bytes = 0;
    rfc_scheduler *sched = job->sched;
    rfc_mutex_unlock(&job->lock);

    while (list) {
        rfc_partial_entry *next = list->next;
        if (list->free_fn && list->ptr) {
            list->free_fn(list->free_ctx, list->ptr);
        }
        free(list);
        list = next;
    }
    if (sched && freed > 0) {
        rfc_scheduler_internal_adjust_live_partials(sched, -(int64_t)freed);
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

int rfc_exec_is_cancelled(const rfc_exec_context *exec) {
    if (!exec || !exec->job) {
        return 1;
    }
    return rfc_job_is_cancelled(exec->job);
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

    rfc_job_stage stage_at = RFC_STAGE_IMPORT;
    rfc_status st = check_stage_abort(job);
    const char *err_detail = NULL;

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

    if (job->stage_ops.on_import != NULL) {
        st = job->stage_ops.on_import(job, &exec, user_ctx);
    } else {
        const rfc_capability *src_cap = rfc_registry_find(job->source_format);
        if (!src_cap) {
            st = RFC_ERR_UNKNOWN_FORMAT;
            err_detail = "unknown source format";
        } else {
            const rfc_session_adapter *sadp =
                rfc_registry_acquire_session(job->source_format);
            if (sadp) {
                void *sess = NULL;
                st = sadp->open_session(&exec, &sess);
                if (st == RFC_OK) {
                    rfc_mutex_lock(&job->lock);
                    job->active_sess_adapter = sadp;
                    job->active_session = sess;
                    rfc_mutex_unlock(&job->lock);

                    st = check_stage_abort(job);
                    if (st == RFC_OK) {
                        st = rfc_limits_check_compressed(&job->limits,
                                                         (uint64_t)src_size);
                    }
                    if (st == RFC_OK) {
                        rfc_layout probed_layout;
                        rfc_metadata probed_meta;
                        memset(&probed_layout, 0, sizeof(probed_layout));
                        memset(&probed_meta, 0, sizeof(probed_meta));
                        st = sadp->probe_session(sess, &exec, src_bytes,
                                                 src_size, &probed_layout,
                                                 &probed_meta);
                        if (st == RFC_OK) {
                            st = rfc_limits_check_image(&job->limits,
                                                        &probed_layout, 1u);
                        }
                        if (st == RFC_OK) {
                            uint64_t probed_bytes = 0;
                            st = rfc_limits_compute_frame_bytes(
                                &probed_layout, 1u, &probed_bytes);
                            if (st == RFC_OK && src_size > 0) {
                                st = rfc_limits_check_decompression(
                                    &job->limits, (uint64_t)src_size,
                                    probed_bytes);
                            }
                        }
                        if (st == RFC_OK) {
                            st = rfc_limits_check_metadata(&job->limits,
                                                           &probed_meta);
                        }
                        if (probed_meta.icc) {
                            exec.allocator->free(exec.allocator->ctx,
                                                 (void *)probed_meta.icc);
                        }
                    }

                    rfc_frame *decoded_frames = NULL;
                    size_t decoded_count = 0;
                    rfc_metadata decoded_meta;
                    memset(&decoded_meta, 0, sizeof(decoded_meta));

                    if (st == RFC_OK) {
                        st = check_stage_abort(job);
                    }
                    if (st == RFC_OK) {
                        st = sadp->decode_session(
                            sess, &exec, src_bytes, src_size, settings,
                            setting_count, &decoded_frames, &decoded_count,
                            &decoded_meta);
                    }

                    rfc_mutex_lock(&job->lock);
                    job->active_sess_adapter = NULL;
                    job->active_session = NULL;
                    while (job->active_cancel_calls > 0) {
                        rfc_cond_wait(&job->done_cond, &job->lock);
                    }
                    rfc_mutex_unlock(&job->lock);

                    sadp->close_session(sess);

                    if (st == RFC_OK) {
                        if (!decoded_frames || decoded_count == 0) {
                            st = RFC_ERR_INVALID_ARGUMENT;
                            if (decoded_meta.icc &&
                                (!decoded_frames ||
                                 decoded_frames[0].metadata.icc !=
                                     decoded_meta.icc)) {
                                exec.allocator->free(exec.allocator->ctx,
                                                     (void *)decoded_meta.icc);
                            }
                            free_decoded_frames_array(&decoded_frames,
                                                      &decoded_count,
                                                      exec.allocator);
                        } else {
                            st = store_decoded_frames_on_job(
                                job, decoded_frames, decoded_count,
                                &decoded_meta, 1);
                            if (st != RFC_OK) {
                                if (decoded_meta.icc &&
                                    decoded_frames[0].metadata.icc !=
                                        decoded_meta.icc) {
                                    exec.allocator->free(
                                        exec.allocator->ctx,
                                        (void *)decoded_meta.icc);
                                }
                                free_decoded_frames_array(&decoded_frames,
                                                          &decoded_count,
                                                          exec.allocator);
                            }
                        }
                    } else {
                        if (decoded_meta.icc &&
                            (!decoded_frames || decoded_count == 0 ||
                             decoded_frames[0].metadata.icc !=
                                 decoded_meta.icc)) {
                            exec.allocator->free(exec.allocator->ctx,
                                                 (void *)decoded_meta.icc);
                        }
                        free_decoded_frames_array(&decoded_frames,
                                                  &decoded_count,
                                                  exec.allocator);
                    }
                }
                rfc_registry_release_session(job->source_format);
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
    }

    if (st != RFC_OK) {
        goto finish;
    }
    st = check_stage_abort(job);
    if (st != RFC_OK) {
        goto finish;
    }

    /* Stage 2: ENCODE */
    stage_at = RFC_STAGE_ENCODE;
    st = rfc_job_advance_stage(job, job->job_id, RFC_STAGE_IMPORT,
                               RFC_STAGE_ENCODE);
    if (st != RFC_OK) {
        goto finish;
    }

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

                        st = check_stage_abort(job);
                        if (st == RFC_OK) {
                            st = sadp->encode_session(
                                sess, &exec, job->decoded_frames,
                                job->decoded_frame_count,
                                job->has_decoded_meta ? &job->decoded_meta
                                                      : NULL,
                                settings, setting_count, &enc_out);
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

    if (job->stage_ops.on_verify != NULL) {
        st = job->stage_ops.on_verify(job, &exec, user_ctx);
    } else {
        st = RFC_ERR_NO_ADAPTER;
        err_detail = "verify stage requires an exactness verifier";
    }

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

    if (job->stage_ops.on_export != NULL) {
        st = job->stage_ops.on_export(job, &exec, user_ctx);
    } else {
        st = RFC_ERR_NO_ADAPTER;
        err_detail = "export stage requires an output publisher";
    }

finish:
    rfc_job_internal_release_stage_outputs(job);

    /* Free all attached partials before transitioning to terminal state. */
    rfc_job_release_partials(job);

    if (out_res) {
        out_res->st = st;
        out_res->stage_at = stage_at;
        out_res->err_detail = err_detail;
        return;
    }

    rfc_mutex_lock(&job->lock);
    int abort_sup = job->superseded || job->superseded_by_id != RFC_JOB_ID_NONE;
    int abort_can = job->cancel_requested || job->state == RFC_JOB_CANCELLED;
    if (job->published && st == RFC_OK) {
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
