#include "job_internal.h"
#include "rfc_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

static void (*g_cmp_hook_fn)(rfc_compare_hook_phase phase,
                             size_t candidate_index,
                             uint32_t iteration_index,
                             rfc_compare_token *token,
                             void *user) = NULL;
static void *g_cmp_hook_user = NULL;

void rfc_compare_set_hook_for_testing(
    void (*fn)(rfc_compare_hook_phase phase,
               size_t candidate_index,
               uint32_t iteration_index,
               rfc_compare_token *token,
               void *user),
    void *user) {
    g_cmp_hook_fn = fn;
    g_cmp_hook_user = user;
}

static void invoke_cmp_hook(rfc_compare_hook_phase phase,
                            size_t cand_idx,
                            uint32_t iter_idx,
                            rfc_compare_token *tok) {
    if (g_cmp_hook_fn) {
        g_cmp_hook_fn(phase, cand_idx, iter_idx, tok, g_cmp_hook_user);
    }
}

/* ----------------------------------------------------------------
 * Thread-safe comparison cancellation token
 * ---------------------------------------------------------------- */

struct rfc_compare_token {
    rfc_mutex lock;
    atomic_int cancelled;
    rfc_job *active_job;
};

rfc_status rfc_compare_token_create(rfc_compare_token **out_token) {
    if (out_token) {
        *out_token = NULL;
    }
    if (!out_token) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_compare_token *tok = (rfc_compare_token *)calloc(1u, sizeof(*tok));
    if (!tok) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (!rfc_mutex_init(&tok->lock)) {
        free(tok);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&tok->cancelled, 0);
    *out_token = tok;
    return RFC_OK;
}

void rfc_compare_token_destroy(rfc_compare_token *token) {
    if (!token) {
        return;
    }
    rfc_mutex_lock(&token->lock);
    rfc_job *j = token->active_job;
    token->active_job = NULL;
    rfc_mutex_unlock(&token->lock);
    if (j) {
        rfc_job_release(j);
    }
    rfc_mutex_destroy(&token->lock);
    free(token);
}

void rfc_compare_cancel(rfc_compare_token *token) {
    if (!token) {
        return;
    }
    atomic_store_explicit(&token->cancelled, 1, memory_order_release);
    rfc_mutex_lock(&token->lock);
    rfc_job *j = token->active_job;
    if (j) {
        rfc_job_retain(j);
    }
    rfc_mutex_unlock(&token->lock);
    if (j) {
        rfc_job_cancel(j);
        rfc_job_release(j);
    }
}

int rfc_compare_is_cancelled(const rfc_compare_token *token) {
    if (!token) {
        return 0;
    }
    return atomic_load_explicit(&token->cancelled, memory_order_acquire) != 0;
}

static void token_bind_job(rfc_compare_token *token, rfc_job *job) {
    if (!token) {
        return;
    }
    if (job) {
        rfc_job_retain(job);
    }
    rfc_mutex_lock(&token->lock);
    rfc_job *old = token->active_job;
    token->active_job = job;
    rfc_mutex_unlock(&token->lock);
    if (old) {
        rfc_job_release(old);
    }
    if (job && rfc_compare_is_cancelled(token)) {
        rfc_job_cancel(job);
    }
}

/* ----------------------------------------------------------------
 * Internal verified in-memory publisher for non-final callback iterations
 * ---------------------------------------------------------------- */

typedef struct cmp_mem_stage {
    uint8_t *buf;
    size_t len;
    size_t cap;
} cmp_mem_stage;

typedef struct cmp_mem_pub_ctx {
    uint8_t *published;
    size_t published_len;
    rfc_job_id published_job;
    uint64_t published_attempt;
    size_t reader_pos;
} cmp_mem_pub_ctx;

typedef struct cmp_mem_rdr_ctx {
    const uint8_t *bytes;
    size_t len;
    size_t pos;
} cmp_mem_rdr_ctx;

static rfc_status cmp_rdr_size(void *raw, int *out_known, uint64_t *out_sz) {
    cmp_mem_rdr_ctx *r = (cmp_mem_rdr_ctx *)raw;
    *out_known = 1;
    *out_sz = (uint64_t)r->len;
    return RFC_OK;
}

static rfc_status cmp_rdr_seek(void *raw, uint64_t off) {
    cmp_mem_rdr_ctx *r = (cmp_mem_rdr_ctx *)raw;
    if (off > (uint64_t)r->len) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    r->pos = (size_t)off;
    return RFC_OK;
}

static rfc_status cmp_rdr_read(void *raw,
                               uint8_t *buf,
                               size_t max_bytes,
                               size_t *out_read) {
    cmp_mem_rdr_ctx *r = (cmp_mem_rdr_ctx *)raw;
    size_t rem = r->len - r->pos;
    size_t take = rem < max_bytes ? rem : max_bytes;
    if (take > 0u) {
        memcpy(buf, r->bytes + r->pos, take);
        r->pos += take;
    }
    *out_read = take;
    return RFC_OK;
}

static void cmp_rdr_close(void *raw) {
    free(raw);
}

static rfc_status cmp_mem_begin(void *raw_ctx,
                                rfc_job_id job_id,
                                uint64_t attempt_number,
                                uint64_t expected_bytes,
                                const rfc_file_identity *source_id,
                                void **out_handle) {
    (void)raw_ctx;
    (void)job_id;
    (void)attempt_number;
    (void)expected_bytes;
    (void)source_id;
    cmp_mem_stage *sh = (cmp_mem_stage *)calloc(1u, sizeof(*sh));
    if (!sh) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    *out_handle = sh;
    return RFC_OK;
}

static rfc_status cmp_mem_write(void *raw_ctx,
                                void *raw_sh,
                                const uint8_t *data,
                                size_t len) {
    (void)raw_ctx;
    cmp_mem_stage *sh = (cmp_mem_stage *)raw_sh;
    if (len == 0u) {
        return RFC_OK;
    }
    if (len > SIZE_MAX - sh->len) {
        return RFC_ERR_OVERFLOW;
    }
    size_t need = sh->len + len;
    if (need > sh->cap) {
        size_t ncap = sh->cap ? sh->cap : 4096u;
        while (ncap < need) {
            ncap = (ncap > SIZE_MAX / 2u) ? need : (ncap * 2u);
        }
        uint8_t *nb = (uint8_t *)realloc(sh->buf, ncap);
        if (!nb) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        sh->buf = nb;
        sh->cap = ncap;
    }
    memcpy(sh->buf + sh->len, data, len);
    sh->len += len;
    return RFC_OK;
}

static rfc_status cmp_mem_flush(void *raw_ctx, void *raw_sh) {
    (void)raw_ctx;
    (void)raw_sh;
    return RFC_OK;
}

static rfc_status cmp_mem_commit(void *raw_ctx,
                                 void *raw_sh,
                                 int *out_cleaned) {
    cmp_mem_pub_ctx *ctx = (cmp_mem_pub_ctx *)raw_ctx;
    cmp_mem_stage *sh = (cmp_mem_stage *)raw_sh;
    free(ctx->published);
    ctx->published = sh->buf;
    ctx->published_len = sh->len;
    free(sh);
    if (out_cleaned) {
        *out_cleaned = 1;
    }
    return RFC_OK;
}

static rfc_status cmp_mem_abort(void *raw_ctx, void *raw_sh) {
    (void)raw_ctx;
    cmp_mem_stage *sh = (cmp_mem_stage *)raw_sh;
    if (sh) {
        free(sh->buf);
        free(sh);
    }
    return RFC_OK;
}

static rfc_status cmp_mem_open_reader(void *raw_ctx,
                                      rfc_job_id job_id,
                                      uint64_t attempt_number,
                                      rfc_io_reader *out_reader) {
    (void)job_id;
    (void)attempt_number;
    cmp_mem_pub_ctx *ctx = (cmp_mem_pub_ctx *)raw_ctx;
    cmp_mem_rdr_ctx *r = (cmp_mem_rdr_ctx *)calloc(1u, sizeof(*r));
    if (!r) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    r->bytes = ctx->published;
    r->len = ctx->published_len;
    r->pos = 0u;
    memset(out_reader, 0, sizeof(*out_reader));
    out_reader->ctx = r;
    out_reader->size_fn = cmp_rdr_size;
    out_reader->read_fn = cmp_rdr_read;
    out_reader->seek_fn = cmp_rdr_seek;
    out_reader->close_fn = cmp_rdr_close;
    return RFC_OK;
}

typedef struct cmp_cancel_pub_ctx {
    const rfc_io_publisher *inner;
    rfc_compare_token *token;
} cmp_cancel_pub_ctx;

static rfc_status cmp_cpub_begin(void *raw_ctx,
                                 rfc_job_id job_id,
                                 uint64_t attempt_number,
                                 uint64_t expected_bytes,
                                 const rfc_file_identity *source_id,
                                 void **out_handle) {
    cmp_cancel_pub_ctx *w = (cmp_cancel_pub_ctx *)raw_ctx;
    if (w->token && rfc_compare_is_cancelled(w->token)) {
        return RFC_ERR_CANCELLED;
    }
    return w->inner->begin_stage(w->inner->ctx, job_id, attempt_number,
                                 expected_bytes, source_id, out_handle);
}

static rfc_status cmp_cpub_write(void *raw_ctx,
                                 void *sh,
                                 const uint8_t *data,
                                 size_t len) {
    cmp_cancel_pub_ctx *w = (cmp_cancel_pub_ctx *)raw_ctx;
    if (w->token && rfc_compare_is_cancelled(w->token)) {
        return RFC_ERR_CANCELLED;
    }
    return w->inner->write_stage(w->inner->ctx, sh, data, len);
}

static rfc_status cmp_cpub_flush(void *raw_ctx, void *sh) {
    cmp_cancel_pub_ctx *w = (cmp_cancel_pub_ctx *)raw_ctx;
    if (w->token && rfc_compare_is_cancelled(w->token)) {
        return RFC_ERR_CANCELLED;
    }
    return w->inner->flush_stage(w->inner->ctx, sh);
}

static rfc_status cmp_cpub_commit(void *raw_ctx, void *sh, int *out_cleaned) {
    cmp_cancel_pub_ctx *w = (cmp_cancel_pub_ctx *)raw_ctx;
    return w->inner->commit_stage(w->inner->ctx, sh, out_cleaned);
}

static rfc_status cmp_cpub_abort(void *raw_ctx, void *sh) {
    cmp_cancel_pub_ctx *w = (cmp_cancel_pub_ctx *)raw_ctx;
    return w->inner->abort_stage(w->inner->ctx, sh);
}

static rfc_status cmp_cpub_open_reader(void *raw_ctx,
                                       rfc_job_id job_id,
                                       uint64_t attempt_number,
                                       rfc_io_reader *out_reader) {
    cmp_cancel_pub_ctx *w = (cmp_cancel_pub_ctx *)raw_ctx;
    return w->inner->open_published_reader(w->inner->ctx, job_id,
                                           attempt_number, out_reader);
}

static void release_candidate_contexts(const rfc_compare_spec *spec,
                                       uint8_t *released,
                                       size_t idx) {
    if (!spec || !released || idx >= spec->candidate_count || released[idx]) {
        return;
    }
    released[idx] = 1u;
    const rfc_compare_candidate_spec *cs = &spec->candidates[idx];
    void *eff_user_ctx = cs->user_ctx ? cs->user_ctx : cs->stage_ops.user_ctx;
    if (cs->has_stage_ops && cs->stage_ops.destroy_user_ctx) {
        cs->stage_ops.destroy_user_ctx(eff_user_ctx);
    }
    if (cs->publisher && cs->publisher->release_ctx) {
        cs->publisher->release_ctx(cs->publisher->ctx);
    }
}

/* ----------------------------------------------------------------
 * Statistics & helpers
 * ---------------------------------------------------------------- */

static void *cmp_default_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void cmp_default_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static void format_utc_now(char out_utc[RFC_REPORT_UTC_MAX]) {
    time_t now = time(NULL);
    struct tm tm_utc;
    memset(&tm_utc, 0, sizeof(tm_utc));
#ifdef _WIN32
    if (gmtime_s(&tm_utc, &now) != 0) {
        rfc_report_internal_copy_utf8(out_utc, RFC_REPORT_UTC_MAX,
                                      "2026-11-16T00:00:00Z");
        return;
    }
#else
    if (!gmtime_r(&now, &tm_utc)) {
        rfc_report_internal_copy_utf8(out_utc, RFC_REPORT_UTC_MAX,
                                      "2026-11-16T00:00:00Z");
        return;
    }
#endif
    char tmp_utc[64];
    snprintf(tmp_utc, sizeof(tmp_utc),
             "%04u-%02u-%02uT%02u:%02u:%02uZ",
             (unsigned)((tm_utc.tm_year + 1900) % 10000),
             (unsigned)((tm_utc.tm_mon + 1) & 0xffu),
             (unsigned)(tm_utc.tm_mday & 0xffu),
             (unsigned)(tm_utc.tm_hour & 0xffu),
             (unsigned)(tm_utc.tm_min & 0xffu),
             (unsigned)(tm_utc.tm_sec & 0xffu));
    rfc_report_internal_copy_utf8(out_utc, RFC_REPORT_UTC_MAX, tmp_utc);
}

static int is_unsupported_error(rfc_status st) {
    return st == RFC_ERR_PLANNED || st == RFC_ERR_NO_ADAPTER ||
           st == RFC_ERR_UNSUPPORTED_SAMPLE ||
           st == RFC_ERR_UNSUPPORTED_ALPHA ||
           st == RFC_ERR_UNSUPPORTED_COMBINATION ||
           st == RFC_ERR_METADATA_POLICY;
}

static void sort_u64_asc(uint64_t *arr, uint32_t n) {
    for (uint32_t i = 1u; i < n; i++) {
        uint64_t key = arr[i];
        uint32_t j = i;
        while (j > 0u && arr[j - 1u] > key) {
            arr[j] = arr[j - 1u];
            j--;
        }
        arr[j] = key;
    }
}

static uint64_t median_sorted_u64(const uint64_t *sorted, uint32_t n) {
    if (n == 0u) {
        return 0u;
    }
    if ((n & 1u) != 0u) {
        return sorted[n / 2u];
    }
    uint64_t a = sorted[n / 2u - 1u];
    uint64_t b = sorted[n / 2u];
    return a / 2u + b / 2u + ((a & 1u) & (b & 1u));
}

static void compute_min_median_timings(
    const rfc_stage_timings iters[RFC_COMPARE_MAX_ITERATIONS],
    uint32_t count,
    rfc_stage_timings *out_min,
    rfc_stage_timings *out_med) {
    memset(out_min, 0, sizeof(*out_min));
    memset(out_med, 0, sizeof(*out_med));
    if (count == 0u || count > RFC_COMPARE_MAX_ITERATIONS) {
        return;
    }
    uint64_t imp[RFC_COMPARE_MAX_ITERATIONS];
    uint64_t enc[RFC_COMPARE_MAX_ITERATIONS];
    uint64_t ver[RFC_COMPARE_MAX_ITERATIONS];
    uint64_t exp[RFC_COMPARE_MAX_ITERATIONS];
    uint64_t wal[RFC_COMPARE_MAX_ITERATIONS];
    for (uint32_t i = 0u; i < count; i++) {
        imp[i] = iters[i].import_ns;
        enc[i] = iters[i].encode_ns;
        ver[i] = iters[i].verify_ns;
        exp[i] = iters[i].export_ns;
        wal[i] = iters[i].wall_ns;
        if (iters[i].import_ran) {
            out_min->import_ran = 1;
            out_med->import_ran = 1;
        }
        if (iters[i].encode_ran) {
            out_min->encode_ran = 1;
            out_med->encode_ran = 1;
        }
        if (iters[i].verify_ran) {
            out_min->verify_ran = 1;
            out_med->verify_ran = 1;
        }
        if (iters[i].export_ran) {
            out_min->export_ran = 1;
            out_med->export_ran = 1;
        }
    }
    sort_u64_asc(imp, count);
    sort_u64_asc(enc, count);
    sort_u64_asc(ver, count);
    sort_u64_asc(exp, count);
    sort_u64_asc(wal, count);

    out_min->valid = 1;
    out_min->import_ns = imp[0];
    out_min->encode_ns = enc[0];
    out_min->verify_ns = ver[0];
    out_min->export_ns = exp[0];
    out_min->wall_ns = wal[0];

    out_med->valid = 1;
    out_med->import_ns = median_sorted_u64(imp, count);
    out_med->encode_ns = median_sorted_u64(enc, count);
    out_med->verify_ns = median_sorted_u64(ver, count);
    out_med->export_ns = median_sorted_u64(exp, count);
    out_med->wall_ns = median_sorted_u64(wal, count);
}

static rfc_status format_stage_iter_path(char out_path[4096],
                                         const char *stage_dir,
                                         char phase_ch,
                                         uint32_t iter_idx) {
    if (!stage_dir || stage_dir[0] == '\0') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t dlen = strlen(stage_dir);
    int has_sep = (dlen > 0u && (stage_dir[dlen - 1u] == '/' ||
                                 stage_dir[dlen - 1u] == '\\'));
    int n = snprintf(out_path, 4096u, "%s%siter.%c%u", stage_dir,
                     has_sep ? "" : "/", phase_ch, (unsigned)iter_idx);
    if (n <= 0 || (size_t)n >= 3800u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

static void mark_remaining_cancelled(rfc_comparison_report *rep,
                                     size_t start_idx,
                                     rfc_compare_candidate_result *first_active) {
    for (size_t j = start_idx; j < rep->candidate_count; j++) {
        rfc_compare_candidate_result *cj = &rep->candidates[j];
        cj->outcome = RFC_CANDIDATE_CANCELLED;
        cj->status = RFC_ERR_CANCELLED;
        cj->rank = 0u;
        cj->has_produced_output = 0;
        cj->measurement_only = 0;
        cj->size.outcome = RFC_SIZE_NOT_APPLICABLE;
        if (cj != first_active) {
            cj->was_started = 0;
            rfc_report_internal_copy_utf8(cj->detail, sizeof(cj->detail),
                                          "cancelled before start");
        }
        rep->cancelled_count++;
    }
}

rfc_status rfc_compare_run(const rfc_compare_spec *spec,
                           rfc_comparison_report *out_report) {
    if (out_report) {
        memset(out_report, 0, sizeof(*out_report));
    }
    if (!spec || !out_report || !spec->candidates ||
        spec->candidate_count == 0u ||
        spec->candidate_count > RFC_COMPARE_MAX_CANDIDATES ||
        spec->measured_iterations == 0u ||
        spec->measured_iterations > RFC_COMPARE_MAX_ITERATIONS ||
        spec->warmup_iterations > RFC_COMPARE_MAX_ITERATIONS) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (spec->run_id && spec->run_id[0] != '\0' &&
        !rfc_report_internal_is_valid_utf8_bounded(
            spec->run_id, RFC_REPORT_RUN_ID_MAX, 1, NULL)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (spec->recorded_utc && spec->recorded_utc[0] != '\0' &&
        !rfc_report_internal_is_valid_utf8_bounded(
            spec->recorded_utc, RFC_REPORT_UTC_MAX, 1, NULL)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    int has_io_src = (spec->io_source != NULL);
    int has_mem_src = (spec->source_bytes != NULL && spec->source_size > 0u);
    if (has_io_src == has_mem_src) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (has_mem_src && !rfc_registry_find(spec->source_format)) {
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    for (size_t i = 0u; i < spec->candidate_count; i++) {
        const rfc_compare_candidate_spec *cs = &spec->candidates[i];
        if (!rfc_registry_find(cs->target_format)) {
            return RFC_ERR_UNKNOWN_FORMAT;
        }
        if (cs->dest_path_utf8 && cs->publisher) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (cs->has_stage_ops && cs->stage_ops.on_export != NULL &&
            (cs->dest_path_utf8 != NULL || cs->publisher != NULL)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (!cs->dest_path_utf8 && !cs->publisher &&
            (!cs->has_stage_ops || cs->stage_ops.on_export == NULL) &&
            (!spec->staging_dir_utf8 || spec->staging_dir_utf8[0] == '\0')) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (cs->setting_count > RFC_REPORT_MAX_SETTINGS ||
            (cs->setting_count > 0u && !cs->settings)) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        for (size_t s = 0u; s < cs->setting_count; s++) {
            if (!rfc_report_internal_is_valid_utf8_bounded(
                    cs->settings[s].id, 48u, 1, NULL) ||
                !rfc_report_internal_is_valid_utf8_bounded(
                    cs->settings[s].value, 64u, 1, NULL)) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
    }

    rfc_limits eff_limits;
    if (spec->limits) {
        eff_limits = *spec->limits;
        rfc_status lst = rfc_limits_validate(&eff_limits);
        if (lst != RFC_OK) {
            return lst;
        }
    } else {
        rfc_limits_default(&eff_limits);
    }

    rfc_metadata_policy eff_meta = spec->metadata_policy
                                       ? spec->metadata_policy
                                       : RFC_META_POLICY_PRESERVE;
    if (eff_meta != RFC_META_POLICY_PRESERVE &&
        eff_meta != RFC_META_POLICY_STRIP) {
        return RFC_ERR_SETTING_VALUE;
    }

    rfc_scheduler_config scfg;
    rfc_scheduler_config_default(&scfg);
    scfg.max_workers = 1u;
    scfg.default_limits = eff_limits;
    uint32_t eff_threads = spec->requested_threads
                               ? spec->requested_threads
                               : scfg.total_cpu_threads;
    if (eff_threads > RFC_SCHED_MAX_CPU_THREADS) {
        return RFC_ERR_SETTING_VALUE;
    }
    if (eff_threads > scfg.total_cpu_threads) {
        scfg.total_cpu_threads = eff_threads;
    }

    out_report->schema_version = RFC_REPORT_SCHEMA_VERSION;
    out_report->requested_threads = spec->requested_threads;
    out_report->effective_threads = eff_threads;
    out_report->warmup_iterations = spec->warmup_iterations;
    out_report->measured_iterations = spec->measured_iterations;
    out_report->metadata_policy = eff_meta;
    out_report->acknowledged_unsupported_metadata =
        spec->acknowledged_unsupported_metadata;
    out_report->limits = eff_limits;
    out_report->candidate_count = spec->candidate_count;
    for (size_t i = 0u; i < spec->candidate_count; i++) {
        out_report->candidates[i].target_format =
            spec->candidates[i].target_format;
        out_report->candidates[i].size.outcome = RFC_SIZE_NOT_APPLICABLE;
    }
    if (spec->run_id && spec->run_id[0] != '\0') {
        rfc_report_internal_copy_utf8(
            out_report->run_id, sizeof(out_report->run_id), spec->run_id);
    } else {
        snprintf(out_report->run_id, sizeof(out_report->run_id), "cmp-%llu",
                 (unsigned long long)rfc_output_internal_next_seq());
    }
    if (spec->recorded_utc && spec->recorded_utc[0] != '\0') {
        rfc_report_internal_copy_utf8(out_report->recorded_utc,
                                      sizeof(out_report->recorded_utc),
                                      spec->recorded_utc);
    } else {
        format_utc_now(out_report->recorded_utc);
    }

    /* Read and hash the canonical source once before comparing candidates. */
    rfc_allocator def_alloc = {cmp_default_alloc, cmp_default_free, NULL};
    rfc_imported_input imp;
    memset(&imp, 0, sizeof(imp));
    const uint8_t *src_bytes = spec->source_bytes;
    size_t src_size = spec->source_size;
    const char *src_path_for_alias = NULL;
    const rfc_file_identity *src_id_for_alias = NULL;
    rfc_status st = RFC_OK;

    if (has_io_src) {
        if (spec->io_source->path_utf8 && spec->io_source->reader) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (spec->io_source->path_utf8) {
            src_path_for_alias = spec->io_source->path_utf8;
            st = rfc_input_read_path(
                spec->io_source->path_utf8, spec->io_source->import_flags,
                &eff_limits, &def_alloc, NULL, &imp);
        } else if (spec->io_source->reader) {
            if (!spec->io_source->reader->seek_fn) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            st = spec->io_source->reader->seek_fn(spec->io_source->reader->ctx,
                                                  0u);
            if (st == RFC_OK) {
                st = rfc_input_read_bounded(
                    spec->io_source->reader,
                    spec->io_source->virtual_name_utf8,
                    spec->io_source->import_flags, &eff_limits, &def_alloc,
                    NULL, &imp);
            }
        } else {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (st != RFC_OK) {
            rfc_imported_input_free(&imp);
            return st;
        }
        if (spec->source_format != (rfc_format_id)0 &&
            spec->source_format != imp.sniff.content_format) {
            rfc_imported_input_free(&imp);
            return RFC_ERR_FORMAT_DISAGREEMENT;
        }
        out_report->source_format = imp.sniff.content_format;
        src_bytes = imp.bytes;
        src_size = imp.size;
        if (imp.identity.valid) {
            src_id_for_alias = &imp.identity;
        }
    } else {
        st = rfc_limits_check_compressed(&eff_limits,
                                         (uint64_t)spec->source_size);
        if (st != RFC_OK) {
            return st;
        }
        out_report->source_format = spec->source_format;
    }

    out_report->source_file_bytes = (uint64_t)src_size;
    rfc_bytes_hash_sha256(src_bytes, src_size, out_report->source_sha256);

    /* Probe/decode source once to get layout + uncompressed raw_frame_bytes. */
    rfc_layout src_layout;
    int has_src_layout = 0;
    rfc_metadata src_meta;
    memset(&src_meta, 0, sizeof(src_meta));
    int has_src_meta = 0;
    const rfc_session_adapter *src_sadp =
        rfc_registry_acquire_session(out_report->source_format);
    if (src_sadp) {
        rfc_frame *probe_frames = NULL;
        size_t probe_count = 0u;
        rfc_status dst = rfc_job_internal_run_standalone_decode(
            out_report->source_format, src_sadp, &eff_limits, &def_alloc,
            src_bytes, src_size, NULL, 0u, &probe_frames, &probe_count,
            &src_meta);
        if (dst == RFC_OK && probe_frames && probe_count > 0u) {
            has_src_layout = 1;
            src_layout = probe_frames[0].layout;
            has_src_meta = 1;
            uint64_t raw_b = 0u;
            if (rfc_metrics_compute_raw_frame_bytes(probe_frames, probe_count,
                                                    &raw_b) == RFC_OK) {
                out_report->raw_frame_bytes = raw_b;
            }
        }
        if (src_meta.icc && (!probe_frames || probe_count == 0u ||
                             probe_frames[0].metadata.icc != src_meta.icc)) {
            free((void *)src_meta.icc);
        }
        src_meta.icc = NULL;
        src_meta.icc_len = 0u;
        if (src_meta.exif && (!probe_frames || probe_count == 0u ||
                              probe_frames[0].metadata.exif != src_meta.exif)) {
            free((void *)src_meta.exif);
        }
        src_meta.exif = NULL;
        src_meta.exif_len = 0u;
        if (src_meta.xmp && (!probe_frames || probe_count == 0u ||
                             probe_frames[0].metadata.xmp != src_meta.xmp)) {
            free((void *)src_meta.xmp);
        }
        src_meta.xmp = NULL;
        src_meta.xmp_len = 0u;
        if (probe_frames) {
            for (size_t i = 0u; i < probe_count; i++) {
                rfc_frame_free(&probe_frames[i]);
            }
            free(probe_frames);
        }
    }

    rfc_scheduler *sched = rfc_scheduler_create(&scfg);
    if (!sched) {
        rfc_imported_input_free(&imp);
        return RFC_ERR_OUT_OF_MEMORY;
    }

    uint8_t released_ctx[RFC_COMPARE_MAX_CANDIDATES];
    memset(released_ctx, 0, sizeof(released_ctx));

    for (size_t i = 0u; i < spec->candidate_count; i++) {
        const rfc_compare_candidate_spec *cs = &spec->candidates[i];
        rfc_compare_candidate_result *cr = &out_report->candidates[i];
        cr->target_format = cs->target_format;
        cr->size.outcome = RFC_SIZE_NOT_APPLICABLE;
        cr->effective_threads = eff_threads;

        invoke_cmp_hook(RFC_COMPARE_HOOK_BEFORE_CANDIDATE, i, 0u,
                        spec->cancel_token);

        if (rfc_compare_is_cancelled(spec->cancel_token)) {
            out_report->cancelled = 1;
            mark_remaining_cancelled(out_report, i, NULL);
            break;
        }

        if (cs->has_build_config_override) {
            cr->build_config = cs->build_config;
        } else {
            (void)rfc_report_default_provider_config(cs->target_format,
                                                     &cr->build_config);
        }

        cr->was_started = 1;

        rfc_status cfg_st =
            rfc_provider_build_config_validate(&cr->build_config);
        if (cfg_st != RFC_OK) {
            cr->outcome = RFC_CANDIDATE_FAILED;
            cr->status = cfg_st;
            cr->failing_stage = RFC_STAGE_PREFLIGHT;
            rfc_report_internal_copy_utf8(
                cr->detail, sizeof(cr->detail),
                "missing provider build configuration provenance");
            out_report->failed_count++;
            release_candidate_contexts(spec, released_ctx, i);
            continue;
        }

        const rfc_capability *tcap = rfc_registry_find(cs->target_format);
        if (!tcap->can_write) {
            cr->outcome = RFC_CANDIDATE_UNSUPPORTED;
            cr->status = RFC_ERR_UNSUPPORTED_COMBINATION;
            cr->failing_stage = RFC_STAGE_PREFLIGHT;
            rfc_report_internal_copy_utf8(
                cr->detail, sizeof(cr->detail),
                tcap->unavailable_reason ? tcap->unavailable_reason
                                         : "target format is import-only");
            out_report->unsupported_count++;
            release_candidate_contexts(spec, released_ctx, i);
            continue;
        }

        int has_custom_encode =
            (cs->has_stage_ops && cs->stage_ops.on_encode != NULL);
        if (!rfc_registry_session_adapter(cs->target_format) &&
            !has_custom_encode) {
            cr->outcome = RFC_CANDIDATE_UNSUPPORTED;
            cr->status = (tcap->availability == RFC_AVAILABILITY_PLANNED)
                             ? RFC_ERR_PLANNED
                             : RFC_ERR_NO_ADAPTER;
            cr->failing_stage = RFC_STAGE_PREFLIGHT;
            rfc_report_internal_copy_utf8(
                cr->detail, sizeof(cr->detail),
                tcap->unavailable_reason ? tcap->unavailable_reason
                                         : rfc_status_name(cr->status));
            out_report->unsupported_count++;
            release_candidate_contexts(spec, released_ctx, i);
            continue;
        }

        if (cs->publisher && !cs->publisher->open_published_reader) {
            cr->outcome = RFC_CANDIDATE_FAILED;
            cr->status = RFC_ERR_NO_ADAPTER;
            cr->failing_stage = RFC_STAGE_PREFLIGHT;
            rfc_report_internal_copy_utf8(
                cr->detail, sizeof(cr->detail),
                "callback sink missing open_published_reader");
            out_report->failed_count++;
            release_candidate_contexts(spec, released_ctx, i);
            continue;
        }

        if (cs->dest_path_utf8) {
            rfc_status dst_st = rfc_output_check_destination_path(
                cs->dest_path_utf8, src_path_for_alias, src_id_for_alias,
                RFC_EXPORT_REFUSE_EXISTING);
            if (dst_st != RFC_OK) {
                cr->outcome = RFC_CANDIDATE_FAILED;
                cr->status = dst_st;
                cr->failing_stage = RFC_STAGE_PREFLIGHT;
                rfc_report_internal_copy_utf8(
                    cr->detail, sizeof(cr->detail), rfc_status_name(dst_st));
                out_report->failed_count++;
                release_candidate_contexts(spec, released_ctx, i);
                continue;
            }
        }

        /* Prepare effective job settings (including comparison metadata policy). */
        rfc_setting_value job_settings[RFC_REPORT_MAX_SETTINGS];
        size_t job_setting_count = 0u;
        int saw_metadata_setting = 0;
        int meta_conflict = 0;
        const char *meta_str = rfc_metadata_policy_name(eff_meta);
        for (size_t s = 0u; s < cs->setting_count; s++) {
            job_settings[job_setting_count++] = cs->settings[s];
            if (strcmp(cs->settings[s].id, "metadata") == 0) {
                saw_metadata_setting = 1;
                if (strcmp(cs->settings[s].value, meta_str) != 0) {
                    meta_conflict = 1;
                }
            }
        }
        if (meta_conflict) {
            cr->outcome = RFC_CANDIDATE_FAILED;
            cr->status = RFC_ERR_INVALID_ARGUMENT;
            cr->failing_stage = RFC_STAGE_PREFLIGHT;
            rfc_report_internal_copy_utf8(
                cr->detail, sizeof(cr->detail),
                "candidate metadata setting conflicts with policy");
            out_report->failed_count++;
            release_candidate_contexts(spec, released_ctx, i);
            continue;
        }
        if (!saw_metadata_setting &&
            job_setting_count < RFC_REPORT_MAX_SETTINGS) {
            job_settings[job_setting_count].id = "metadata";
            job_settings[job_setting_count].value = meta_str;
            job_setting_count++;
        }
        for (size_t s = 0u;
             s < job_setting_count && s < RFC_REPORT_MAX_SETTINGS; s++) {
            rfc_report_internal_copy_utf8(cr->settings[s].id,
                                          sizeof(cr->settings[s].id),
                                          job_settings[s].id);
            rfc_report_internal_copy_utf8(cr->settings[s].value,
                                          sizeof(cr->settings[s].value),
                                          job_settings[s].value);
            cr->setting_count++;
        }

        int attach_sink =
            (!cs->has_stage_ops || cs->stage_ops.on_export == NULL);
        int use_path_sink = (attach_sink && cs->dest_path_utf8 != NULL);
        int use_cb_sink = (attach_sink && cs->publisher != NULL);
        int measurement_only = (!use_path_sink && !use_cb_sink);

        char stage_dir[4096];
        stage_dir[0] = '\0';
        int has_stage_dir = 0;
        if (attach_sink && !use_cb_sink) {
            const char *anchor =
                use_path_sink ? cs->dest_path_utf8 : spec->staging_dir_utf8;
            int parent_of_target = use_path_sink ? 1 : 0;
            rfc_status sdt = rfc_output_internal_create_staging_subdir(
                anchor, parent_of_target, stage_dir);
            if (sdt != RFC_OK) {
                cr->outcome = RFC_CANDIDATE_FAILED;
                cr->status = sdt;
                cr->failing_stage = RFC_STAGE_PREFLIGHT;
                rfc_report_internal_copy_utf8(
                    cr->detail, sizeof(cr->detail), rfc_status_name(sdt));
                out_report->failed_count++;
                release_candidate_contexts(spec, released_ctx, i);
                continue;
            }
            has_stage_dir = 1;
        }

        void *eff_user_ctx =
            cs->user_ctx ? cs->user_ctx : cs->stage_ops.user_ctx;

        /* Execute untimed warmups followed by measured iterations strictly sequentially. */
        uint32_t total_runs =
            spec->warmup_iterations + spec->measured_iterations;
        int any_iter_succeeded = 0;
        rfc_status iter_fail_st = RFC_OK;
        rfc_job_stage iter_fail_stage = RFC_STAGE_NONE;
        char iter_detail[RFC_JOB_DETAIL_MAX];
        iter_detail[0] = '\0';
        int cand_cancelled = 0;
        rfc_verify_report final_vrep;
        memset(&final_vrep, 0, sizeof(final_vrep));
        rfc_size_metrics final_smetrics;
        memset(&final_smetrics, 0, sizeof(final_smetrics));
        char final_temp_path[4096];
        final_temp_path[0] = '\0';
        rfc_file_identity final_staged_id;
        memset(&final_staged_id, 0, sizeof(final_staged_id));
        uint8_t *final_pub_bytes = NULL;
        size_t final_pub_len = 0u;
        rfc_job_id final_job_id = 0u;

        for (uint32_t r = 0u; r < total_runs; r++) {
            int is_warmup = (r < spec->warmup_iterations);
            uint32_t iter_num =
                is_warmup ? r : (r - spec->warmup_iterations);
            int is_final_measured =
                (!is_warmup && iter_num + 1u == spec->measured_iterations);

            invoke_cmp_hook(
                is_warmup ? RFC_COMPARE_HOOK_WARMUP_ITER
                          : RFC_COMPARE_HOOK_MEASURED_ITER,
                i, iter_num, spec->cancel_token);

            if (rfc_compare_is_cancelled(spec->cancel_token)) {
                cand_cancelled = 1;
                iter_fail_st = RFC_ERR_CANCELLED;
                break;
            }

            char temp_path[4096];
            temp_path[0] = '\0';
            const char *active_path = NULL;
            cmp_mem_pub_ctx mpub_ctx;
            memset(&mpub_ctx, 0, sizeof(mpub_ctx));
            rfc_io_publisher mpub;
            memset(&mpub, 0, sizeof(mpub));
            mpub.ctx = &mpub_ctx;
            mpub.begin_stage = cmp_mem_begin;
            mpub.write_stage = cmp_mem_write;
            mpub.flush_stage = cmp_mem_flush;
            mpub.commit_stage = cmp_mem_commit;
            mpub.abort_stage = cmp_mem_abort;
            mpub.open_published_reader = cmp_mem_open_reader;

            rfc_io_sink sink;
            memset(&sink, 0, sizeof(sink));
            if (attach_sink) {
                if (use_cb_sink) {
                    rfc_io_sink_init_callbacks(
                        &sink, &mpub, RFC_EXPORT_REFUSE_EXISTING);
                } else {
                    rfc_status pst = format_stage_iter_path(
                        temp_path, stage_dir, is_warmup ? 'w' : 'm', iter_num);
                    if (pst != RFC_OK) {
                        iter_fail_st = pst;
                        iter_fail_stage = RFC_STAGE_EXPORT;
                        break;
                    }
                    active_path = temp_path;
                    rfc_io_sink_init_path(&sink, active_path,
                                          RFC_EXPORT_REFUSE_EXISTING);
                }
            }

            rfc_job_spec jspec;
            memset(&jspec, 0, sizeof(jspec));
            jspec.source_format = out_report->source_format;
            jspec.target_format = cs->target_format;
            jspec.source_bytes = src_bytes;
            jspec.source_size = src_size;
            if (has_src_layout) {
                jspec.source_layout = &src_layout;
            }
            if (has_src_meta && src_meta.icc == NULL &&
                src_meta.exif == NULL && src_meta.xmp == NULL) {
                jspec.source_meta = &src_meta;
            }
            if (!has_src_layout) {
                uint64_t ws = cs->expected_working_set_bytes
                                  ? cs->expected_working_set_bytes
                                  : ((uint64_t)src_size * 4ull + 4096ull);
                jspec.expected_working_set_bytes = ws;
            } else {
                jspec.expected_working_set_bytes =
                    cs->expected_working_set_bytes;
            }
            jspec.settings = job_setting_count ? job_settings : NULL;
            jspec.setting_count = job_setting_count;
            jspec.requested_codec_threads = eff_threads;
            jspec.limits_override = &eff_limits;
            jspec.has_stage_ops = cs->has_stage_ops;
            if (cs->has_stage_ops) {
                jspec.stage_ops = cs->stage_ops;
                jspec.stage_ops.user_ctx = eff_user_ctx;
                jspec.stage_ops.destroy_user_ctx = NULL;
            }
            jspec.acknowledged_unsupported_metadata =
                spec->acknowledged_unsupported_metadata;
            if (attach_sink) {
                jspec.io_sink = &sink;
            }

            rfc_job *job = NULL;
            rfc_status sst = rfc_scheduler_submit(sched, NULL, &jspec, &job);
            if (sst != RFC_OK) {
                free(mpub_ctx.published);
                if (sst == RFC_ERR_CANCELLED ||
                    rfc_compare_is_cancelled(spec->cancel_token)) {
                    cand_cancelled = 1;
                    iter_fail_st = RFC_ERR_CANCELLED;
                } else {
                    iter_fail_st = sst;
                    iter_fail_stage = RFC_STAGE_PREFLIGHT;
                }
                break;
            }

            token_bind_job(spec->cancel_token, job);
            rfc_status wst = rfc_job_wait(job, 0xFFFFFFFFu);
            token_bind_job(spec->cancel_token, NULL);

            rfc_job_snapshot snap;
            memset(&snap, 0, sizeof(snap));
            (void)rfc_job_snapshot_get(job, &snap);
            if (snap.admitted_threads > 0u) {
                cr->effective_threads = snap.admitted_threads;
            }

            if (out_report->raw_frame_bytes == 0u) {
                uint64_t job_raw = rfc_job_internal_raw_frame_bytes(job);
                if (job_raw > 0u) {
                    out_report->raw_frame_bytes = job_raw;
                }
            }

            rfc_export_report erep;
            memset(&erep, 0, sizeof(erep));
            (void)rfc_job_export_report_get(job, &erep);

            if (wst != RFC_OK || snap.state != RFC_JOB_SUCCEEDED ||
                snap.status != RFC_OK ||
                rfc_compare_is_cancelled(spec->cancel_token)) {
                if (snap.state == RFC_JOB_CANCELLED ||
                    snap.status == RFC_ERR_CANCELLED ||
                    rfc_compare_is_cancelled(spec->cancel_token)) {
                    cand_cancelled = 1;
                    iter_fail_st = RFC_ERR_CANCELLED;
                } else {
                    iter_fail_st =
                        (snap.status != RFC_OK) ? snap.status : RFC_ERR_IO;
                    iter_fail_stage = snap.failing_stage;
                    rfc_report_internal_copy_utf8(
                        iter_detail, sizeof(iter_detail), snap.detail);
                }
                rfc_job_release(job);
                free(mpub_ctx.published);
                if (active_path && erep.published && erep.has_output_identity) {
                    (void)rfc_output_internal_unlink_owned_path(
                        active_path, &erep.output_identity);
                }
                break;
            }

            rfc_verify_report vrep;
            rfc_status vst = rfc_job_verify_report_get(job, &vrep);
            if (vst != RFC_OK || !vrep.valid || vrep.status != RFC_OK ||
                vrep.decoder_provenance[0] == '\0') {
                iter_fail_st = (vst != RFC_OK) ? vst : RFC_ERR_VERIFY_MISMATCH;
                iter_fail_stage = RFC_STAGE_VERIFY;
                rfc_report_internal_copy_utf8(
                    iter_detail, sizeof(iter_detail),
                    "missing or invalid verify report");
                rfc_job_release(job);
                free(mpub_ctx.published);
                if (active_path && erep.published && erep.has_output_identity) {
                    (void)rfc_output_internal_unlink_owned_path(
                        active_path, &erep.output_identity);
                }
                break;
            }

            rfc_size_metrics smetrics;
            rfc_status mst = rfc_metrics_verify_job_output(
                job, active_path, out_report->source_file_bytes,
                out_report->raw_frame_bytes, out_report->source_sha256, 1,
                &smetrics);

            rfc_stage_timings stimes;
            rfc_status tst = rfc_job_stage_timings_get(job, &stimes);

            rfc_job_release(job);

            if (mst != RFC_OK || !smetrics.valid ||
                smetrics.produced_file_bytes == 0u ||
                tst != RFC_OK || !stimes.valid) {
                free(mpub_ctx.published);
                if (active_path && erep.published && erep.has_output_identity) {
                    (void)rfc_output_internal_unlink_owned_path(
                        active_path, &erep.output_identity);
                }
                iter_fail_st =
                    (mst != RFC_OK) ? mst
                                    : ((tst != RFC_OK) ? tst : RFC_ERR_IO);
                iter_fail_stage = RFC_STAGE_EXPORT;
                rfc_report_internal_copy_utf8(iter_detail, sizeof(iter_detail),
                                              rfc_status_name(iter_fail_st));
                break;
            }

            int keep_staged_for_final =
                (is_final_measured && use_path_sink);
            if (active_path && !keep_staged_for_final && erep.published &&
                erep.has_output_identity) {
                (void)rfc_output_internal_unlink_owned_path(
                    active_path, &erep.output_identity);
            } else if (keep_staged_for_final) {
                if (!erep.has_output_identity) {
                    if (erep.published) {
                        (void)rfc_output_internal_unlink_path(active_path);
                    }
                    free(mpub_ctx.published);
                    iter_fail_st = RFC_ERR_VERIFY_MISMATCH;
                    iter_fail_stage = RFC_STAGE_EXPORT;
                    rfc_report_internal_copy_utf8(
                        iter_detail, sizeof(iter_detail),
                        "staged file missing verified output identity");
                    break;
                }
                rfc_report_internal_copy_utf8(
                    final_temp_path, sizeof(final_temp_path), active_path);
                final_staged_id = erep.output_identity;
            }

            if (is_final_measured && use_cb_sink) {
                final_pub_bytes = mpub_ctx.published;
                final_pub_len = mpub_ctx.published_len;
                final_job_id = snap.job_id;
                mpub_ctx.published = NULL;
            } else {
                free(mpub_ctx.published);
            }

            any_iter_succeeded = 1;
            if (is_warmup) {
                cr->warmup_completed++;
            } else {
                cr->per_iter_timings[iter_num] = stimes;
                cr->measured_completed++;
                final_vrep = vrep;
                final_smetrics = smetrics;
            }
        }

        /* Promote/publish verified final artifact only after all iterations pass. */
        if (!cand_cancelled && iter_fail_st == RFC_OK) {
            invoke_cmp_hook(RFC_COMPARE_HOOK_BEFORE_FINAL_PUBLISH, i,
                            spec->measured_iterations - 1u, spec->cancel_token);
            if (rfc_compare_is_cancelled(spec->cancel_token)) {
                cand_cancelled = 1;
                iter_fail_st = RFC_ERR_CANCELLED;
            }
        }

        int artifact_published = 0;

        if (!cand_cancelled && iter_fail_st == RFC_OK) {
            if (use_path_sink) {
                uint64_t pub_t0 = rfc_monotonic_now_ns();
                rfc_status pst = rfc_output_internal_promote_staged_path(
                    final_temp_path, cs->dest_path_utf8, src_path_for_alias,
                    src_id_for_alias, &final_staged_id);
                uint64_t pub_t1 = rfc_monotonic_now_ns();
                cr->final_publish_ns =
                    (pub_t1 >= pub_t0) ? (pub_t1 - pub_t0) : 0u;
                if (pst != RFC_OK) {
                    (void)rfc_output_internal_unlink_owned_path(
                        final_temp_path, &final_staged_id);
                    final_temp_path[0] = '\0';
                    iter_fail_st = pst;
                    iter_fail_stage = RFC_STAGE_EXPORT;
                    rfc_report_internal_copy_utf8(iter_detail,
                                                  sizeof(iter_detail),
                                                  rfc_status_name(pst));
                } else {
                    artifact_published = 1;
                    final_temp_path[0] = '\0';
                    if (has_stage_dir) {
                        (void)rfc_output_internal_remove_staging_subdir(
                            stage_dir);
                        has_stage_dir = 0;
                    }
                    rfc_export_report prep;
                    memset(&prep, 0, sizeof(prep));
                    prep.valid = 1;
                    prep.published = 1;
                    prep.status = RFC_OK;
                    prep.bytes_written = final_smetrics.produced_file_bytes;
                    memcpy(prep.output_sha256, final_smetrics.produced_sha256,
                           32u);
                    rfc_size_metrics post_sm;
                    rfc_status vmst = rfc_metrics_verify_produced_path(
                        cs->dest_path_utf8, out_report->source_file_bytes,
                        out_report->raw_frame_bytes, out_report->source_sha256,
                        1, &prep, &post_sm);
                    if (vmst != RFC_OK) {
                        iter_fail_st = vmst;
                        iter_fail_stage = RFC_STAGE_VERIFY;
                        rfc_report_internal_copy_utf8(
                            iter_detail, sizeof(iter_detail),
                            rfc_status_name(vmst));
                    } else {
                        final_smetrics = post_sm;
                    }
                }
            } else if (use_cb_sink) {
                cmp_cancel_pub_ctx wctx = {cs->publisher, spec->cancel_token};
                rfc_io_publisher wrap_pub;
                memset(&wrap_pub, 0, sizeof(wrap_pub));
                wrap_pub.ctx = &wctx;
                wrap_pub.begin_stage = cmp_cpub_begin;
                wrap_pub.write_stage = cmp_cpub_write;
                wrap_pub.flush_stage = cmp_cpub_flush;
                wrap_pub.commit_stage = cmp_cpub_commit;
                wrap_pub.abort_stage = cmp_cpub_abort;
                wrap_pub.open_published_reader = cmp_cpub_open_reader;

                rfc_export_report pub_erep;
                memset(&pub_erep, 0, sizeof(pub_erep));
                uint64_t pub_t0 = rfc_monotonic_now_ns();
                rfc_status pst = rfc_output_publish_staged(
                    &wrap_pub, NULL, final_job_id, 1u, src_id_for_alias,
                    final_pub_bytes, final_pub_len, NULL, &pub_erep);
                uint64_t pub_t1 = rfc_monotonic_now_ns();
                cr->final_publish_ns =
                    (pub_t1 >= pub_t0) ? (pub_t1 - pub_t0) : 0u;
                free(final_pub_bytes);
                final_pub_bytes = NULL;
                final_pub_len = 0u;

                if (pub_erep.published) {
                    artifact_published = 1;
                }
                if (pst != RFC_OK) {
                    if (pst == RFC_ERR_CANCELLED && !pub_erep.published) {
                        cand_cancelled = 1;
                        iter_fail_st = RFC_ERR_CANCELLED;
                    } else {
                        iter_fail_st = pst;
                        iter_fail_stage = RFC_STAGE_EXPORT;
                        rfc_report_internal_copy_utf8(
                            iter_detail, sizeof(iter_detail),
                            pub_erep.detail[0] ? pub_erep.detail
                                               : rfc_status_name(pst));
                    }
                } else {
                    artifact_published = 1;
                    rfc_size_metrics post_sm;
                    rfc_status vmst = rfc_metrics_verify_produced_callback(
                        &wrap_pub, final_job_id, 1u,
                        out_report->source_file_bytes,
                        out_report->raw_frame_bytes, out_report->source_sha256,
                        1, &pub_erep, &post_sm);
                    if (vmst != RFC_OK) {
                        iter_fail_st = vmst;
                        iter_fail_stage = RFC_STAGE_VERIFY;
                        rfc_report_internal_copy_utf8(
                            iter_detail, sizeof(iter_detail),
                            rfc_status_name(vmst));
                    } else {
                        final_smetrics = post_sm;
                    }
                }
            }
        }

        if (final_temp_path[0] != '\0') {
            (void)rfc_output_internal_unlink_owned_path(final_temp_path,
                                                        &final_staged_id);
            final_temp_path[0] = '\0';
        }
        if (has_stage_dir) {
            (void)rfc_output_internal_remove_staging_subdir(stage_dir);
            has_stage_dir = 0;
        }
        free(final_pub_bytes);
        final_pub_bytes = NULL;

        if (cand_cancelled) {
            out_report->cancelled = 1;
            cr->outcome = RFC_CANDIDATE_CANCELLED;
            cr->status = RFC_ERR_CANCELLED;
            cr->failing_stage = iter_fail_stage;
            cr->has_produced_output = 0;
            cr->measurement_only = 0;
            cr->rank = 0u;
            cr->size.outcome = RFC_SIZE_NOT_APPLICABLE;
            rfc_report_internal_copy_utf8(cr->detail, sizeof(cr->detail),
                                          "cancelled");
            mark_remaining_cancelled(out_report, i, cr);
            break;
        }

        if (iter_fail_st != RFC_OK) {
            if (!any_iter_succeeded && is_unsupported_error(iter_fail_st)) {
                cr->outcome = RFC_CANDIDATE_UNSUPPORTED;
                out_report->unsupported_count++;
            } else {
                cr->outcome = RFC_CANDIDATE_FAILED;
                out_report->failed_count++;
            }
            cr->status = iter_fail_st;
            cr->failing_stage = iter_fail_stage;
            cr->has_produced_output = artifact_published ? 1 : 0;
            cr->measurement_only = 0;
            cr->rank = 0u;
            cr->size.outcome = RFC_SIZE_NOT_APPLICABLE;
            rfc_report_internal_copy_utf8(
                cr->detail, sizeof(cr->detail),
                iter_detail[0] ? iter_detail : rfc_status_name(iter_fail_st));
            release_candidate_contexts(spec, released_ctx, i);
            continue;
        }

        compute_min_median_timings(cr->per_iter_timings, cr->measured_completed,
                                   &cr->min_timings, &cr->median_timings);
        cr->verify_report = final_vrep;
        cr->size = final_smetrics;
        cr->measurement_only = measurement_only ? 1 : 0;
        cr->has_produced_output = measurement_only ? 0 : 1;
        cr->outcome = RFC_CANDIDATE_COMPLETED;
        cr->status = RFC_OK;
        cr->failing_stage = RFC_STAGE_NONE;
        rfc_report_internal_copy_utf8(
            cr->detail, sizeof(cr->detail),
            measurement_only ? "ok (measurement-only)" : "ok");
        out_report->completed_count++;
        release_candidate_contexts(spec, released_ctx, i);

        invoke_cmp_hook(RFC_COMPARE_HOOK_AFTER_FINAL_PUBLISH, i,
                        spec->measured_iterations - 1u, spec->cancel_token);
        if (rfc_compare_is_cancelled(spec->cancel_token)) {
            out_report->cancelled = 1;
            if (i + 1u < spec->candidate_count) {
                mark_remaining_cancelled(out_report, i + 1u, NULL);
            }
            break;
        }
    }

    for (size_t k = 0u; k < spec->candidate_count; k++) {
        release_candidate_contexts(spec, released_ctx, k);
    }

    rfc_scheduler_destroy(sched);

    /* Confirm file-backed source bytes remained unmodified across comparison. */
    if (has_io_src && spec->io_source->path_utf8) {
        uint64_t check_sz = 0u;
        uint8_t check_sha[32];
        rfc_status hst = rfc_output_internal_hash_regular_path(
            spec->io_source->path_utf8, &check_sz, check_sha);
        if (hst != RFC_OK || check_sz != out_report->source_file_bytes ||
            memcmp(check_sha, out_report->source_sha256, 32u) != 0) {
            rfc_imported_input_free(&imp);
            return RFC_ERR_VERIFY_MISMATCH;
        }
    }
    rfc_imported_input_free(&imp);

    rfc_report_internal_finalize_configs(out_report);

    if (out_report->cancelled || rfc_compare_is_cancelled(spec->cancel_token)) {
        out_report->cancelled = 1;
        out_report->run_status = RFC_ERR_CANCELLED;
        out_report->ranking_complete = 0;
        for (size_t i = 0u; i < out_report->candidate_count; i++) {
            out_report->candidates[i].rank = 0u;
        }
    } else {
        out_report->run_status = RFC_OK;
        out_report->ranking_complete = 1;

        size_t order[RFC_COMPARE_MAX_CANDIDATES];
        size_t n_done = 0u;
        for (size_t i = 0u; i < out_report->candidate_count; i++) {
            out_report->candidates[i].rank = 0u;
            if (out_report->candidates[i].outcome == RFC_CANDIDATE_COMPLETED) {
                order[n_done++] = i;
            }
        }
        for (size_t a = 0u; a < n_done; a++) {
            for (size_t b = a + 1u; b < n_done; b++) {
                const rfc_compare_candidate_result *ca =
                    &out_report->candidates[order[a]];
                const rfc_compare_candidate_result *cb =
                    &out_report->candidates[order[b]];
                int swap = 0;
                if (cb->size.produced_file_bytes <
                    ca->size.produced_file_bytes) {
                    swap = 1;
                } else if (cb->size.produced_file_bytes ==
                           ca->size.produced_file_bytes) {
                    if (cb->median_timings.wall_ns <
                        ca->median_timings.wall_ns) {
                        swap = 1;
                    } else if (cb->median_timings.wall_ns ==
                                   ca->median_timings.wall_ns &&
                               cb->target_format < ca->target_format) {
                        swap = 1;
                    }
                }
                if (swap) {
                    size_t tmp = order[a];
                    order[a] = order[b];
                    order[b] = tmp;
                }
            }
        }
        for (size_t a = 0u; a < n_done; a++) {
            out_report->candidates[order[a]].rank = (uint32_t)(a + 1u);
        }
    }

    if (spec->history_path_utf8 && spec->history_path_utf8[0] != '\0') {
        rfc_status hst =
            rfc_history_append_path(spec->history_path_utf8, out_report);
        out_report->history_status = hst;
        out_report->history_recorded = (hst == RFC_OK) ? 1 : 0;
        if (out_report->run_status == RFC_OK && hst != RFC_OK) {
            return hst;
        }
    }

    return out_report->run_status;
}
