#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "rfc_job.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK test_mutex_t;
typedef CONDITION_VARIABLE test_cond_t;
static void test_mutex_init(test_mutex_t *m) { InitializeSRWLock(m); }
static void test_mutex_destroy(test_mutex_t *m) { (void)m; }
static void test_mutex_lock(test_mutex_t *m) { AcquireSRWLockExclusive(m); }
static void test_mutex_unlock(test_mutex_t *m) { ReleaseSRWLockExclusive(m); }
static void test_cond_init(test_cond_t *c) { InitializeConditionVariable(c); }
static void test_cond_destroy(test_cond_t *c) { (void)c; }
static void test_cond_wait(test_cond_t *c, test_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static void test_cond_broadcast(test_cond_t *c) { WakeAllConditionVariable(c); }
#else
#include <pthread.h>
typedef pthread_mutex_t test_mutex_t;
typedef pthread_cond_t test_cond_t;
static void test_mutex_init(test_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void test_mutex_destroy(test_mutex_t *m) { pthread_mutex_destroy(m); }
static void test_mutex_lock(test_mutex_t *m) { pthread_mutex_lock(m); }
static void test_mutex_unlock(test_mutex_t *m) { pthread_mutex_unlock(m); }
static void test_cond_init(test_cond_t *c) { pthread_cond_init(c, NULL); }
static void test_cond_destroy(test_cond_t *c) { pthread_cond_destroy(c); }
static void test_cond_wait(test_cond_t *c, test_mutex_t *m) {
    pthread_cond_wait(c, m);
}
static void test_cond_broadcast(test_cond_t *c) { pthread_cond_broadcast(c); }
#endif

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

typedef struct barrier_state {
    test_mutex_t lock;
    test_cond_t cond;
    uint32_t arrived;
    uint32_t release_flag;
    uint32_t active_now;
    uint32_t max_concurrent;
    uint32_t total_runs;
    uint32_t last_seen_threads;
} barrier_state;

static void barrier_init(barrier_state *b) {
    memset(b, 0, sizeof(*b));
    test_mutex_init(&b->lock);
    test_cond_init(&b->cond);
}

static void barrier_destroy(barrier_state *b) {
    test_cond_destroy(&b->cond);
    test_mutex_destroy(&b->lock);
}

static rfc_status ok_noop_stage(rfc_job *job,
                                const rfc_exec_context *exec,
                                void *user_ctx) {
    (void)job;
    (void)exec;
    (void)user_ctx;
    return RFC_OK;
}

static rfc_status export_mark_ok(rfc_job *job,
                                 const rfc_exec_context *exec,
                                 void *user_ctx) {
    (void)user_ctx;
    return rfc_job_mark_published(job, exec->job_id);
}

static void init_basic_spec(rfc_job_spec *spec,
                            const uint8_t *src,
                            size_t src_len,
                            const rfc_layout *layout) {
    memset(spec, 0, sizeof(*spec));
    spec->source_format = RFC_FORMAT_PNG;
    spec->target_format = RFC_FORMAT_QOI;
    spec->source_bytes = src;
    spec->source_size = src_len;
    spec->source_layout = layout;
    spec->requested_codec_threads = 1;
    spec->expected_working_set_bytes = 4096u;
    spec->has_stage_ops = 1;
    spec->stage_ops.on_import = ok_noop_stage;
    spec->stage_ops.on_encode = ok_noop_stage;
    spec->stage_ops.on_verify = ok_noop_stage;
    spec->stage_ops.on_export = export_mark_ok;
}

/* 1. Configuration defaults, ranges, runtime reconfigure, effective read-back */
static void test_config_defaults_and_reconfigure(void) {
    rfc_scheduler_config def;
    rfc_scheduler_config_default(&def);
    CHECK(def.max_workers == RFC_SCHED_DEFAULT_WORKERS);
    CHECK(def.total_cpu_threads == RFC_SCHED_DEFAULT_CPU_THREADS);
    CHECK(def.max_queue_depth == RFC_SCHED_DEFAULT_QUEUE_DEPTH);
    CHECK(def.memory_budget_bytes == RFC_SCHED_DEFAULT_MEMORY_BUDGET);
    CHECK(rfc_scheduler_config_validate(&def) == RFC_OK);
    CHECK(rfc_scheduler_config_validate(NULL) == RFC_ERR_INVALID_ARGUMENT);

    rfc_scheduler_config bad = def;
    bad.max_workers = 0;
    CHECK(rfc_scheduler_config_validate(&bad) == RFC_ERR_SETTING_VALUE);
    bad = def;
    bad.max_workers = RFC_SCHED_MAX_WORKERS + 1u;
    CHECK(rfc_scheduler_config_validate(&bad) == RFC_ERR_SETTING_VALUE);
    bad = def;
    bad.total_cpu_threads = 0;
    CHECK(rfc_scheduler_config_validate(&bad) == RFC_ERR_SETTING_VALUE);
    bad = def;
    bad.total_cpu_threads = RFC_SCHED_MAX_CPU_THREADS + 1u;
    CHECK(rfc_scheduler_config_validate(&bad) == RFC_ERR_SETTING_VALUE);
    bad = def;
    bad.max_queue_depth = 0;
    CHECK(rfc_scheduler_config_validate(&bad) == RFC_ERR_SETTING_VALUE);
    bad = def;
    bad.max_queue_depth = RFC_SCHED_MAX_QUEUE_DEPTH + 1u;
    CHECK(rfc_scheduler_config_validate(&bad) == RFC_ERR_SETTING_VALUE);
    bad = def;
    bad.memory_budget_bytes = RFC_SCHED_MIN_MEMORY_BUDGET - 1u;
    CHECK(rfc_scheduler_config_validate(&bad) == RFC_ERR_SETTING_VALUE);
    bad = def;
    bad.default_limits.max_width = 0;
    CHECK(rfc_scheduler_config_validate(&bad) == RFC_ERR_SETTING_VALUE);

    rfc_scheduler *sched = rfc_scheduler_create(&def);
    CHECK(sched != NULL);

    rfc_scheduler_config readback;
    memset(&readback, 0, sizeof(readback));
    CHECK(rfc_scheduler_get_config(sched, &readback) == RFC_OK);
    CHECK(readback.max_workers == def.max_workers);
    CHECK(readback.total_cpu_threads == def.total_cpu_threads);
    CHECK(readback.max_queue_depth == def.max_queue_depth);
    CHECK(readback.memory_budget_bytes == def.memory_budget_bytes);

    CHECK(rfc_scheduler_reconfigure(sched, &bad) == RFC_ERR_SETTING_VALUE);
    CHECK(rfc_scheduler_get_config(sched, &readback) == RFC_OK);
    CHECK(readback.max_workers == def.max_workers);

    rfc_scheduler_config updated = def;
    updated.max_workers = 2u;
    updated.total_cpu_threads = 6u;
    updated.max_queue_depth = 128u;
    updated.memory_budget_bytes = 64ull * 1024ull * 1024ull;
    CHECK(rfc_scheduler_reconfigure(sched, &updated) == RFC_OK);
    CHECK(rfc_scheduler_get_config(sched, &readback) == RFC_OK);
    CHECK(readback.max_workers == 2u);
    CHECK(readback.total_cpu_threads == 6u);
    CHECK(readback.max_queue_depth == 128u);
    CHECK(readback.memory_budget_bytes == 64ull * 1024ull * 1024ull);

    printf("jobs: config read-back default=%uw/%ut/%uq/%lluB "
           "reconfigure=%uw/%ut/%uq/%lluB\n",
           def.max_workers, def.total_cpu_threads, def.max_queue_depth,
           (unsigned long long)def.memory_budget_bytes, readback.max_workers,
           readback.total_cpu_threads, readback.max_queue_depth,
           (unsigned long long)readback.memory_budget_bytes);

    rfc_scheduler_destroy(sched);
}

/* 2. Hostile limits, integer overflow, memory contract, unknown reservation = 0 alloc */
static void test_hostile_limits_and_unknown_reservation(void) {
    rfc_limits lim;
    rfc_limits_default(&lim);

    rfc_layout hostile;
    memset(&hostile, 0, sizeof(hostile));
    hostile.format = RFC_SAMPLE_RGBA16;
    hostile.alpha = RFC_ALPHA_STRAIGHT;
    hostile.width = 16777216u;
    hostile.height = 16777216u;
    hostile.stride_bytes = 0u;
    hostile.bits_per_sample = 16u;
    CHECK(rfc_limits_check_image(&lim, &hostile, 1u) == RFC_ERR_LIMIT_EXCEEDED);

    rfc_layout normal;
    memset(&normal, 0, sizeof(normal));
    normal.format = RFC_SAMPLE_RGB8;
    normal.alpha = RFC_ALPHA_NONE;
    normal.width = 64u;
    normal.height = 64u;
    normal.stride_bytes = 64u * 3u;
    normal.bits_per_sample = 8u;
    CHECK(rfc_limits_check_image(&lim, &normal, lim.max_frames + 1u) ==
          RFC_ERR_LIMIT_EXCEEDED);

    rfc_metadata hostile_meta;
    memset(&hostile_meta, 0, sizeof(hostile_meta));
    hostile_meta.icc_len = lim.max_metadata_bytes + 1u;
    CHECK(rfc_limits_check_metadata(&lim, &hostile_meta) ==
          RFC_ERR_LIMIT_EXCEEDED);

    CHECK(rfc_limits_check_decompression(&lim, 128u, 128ull * 2048ull) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(rfc_limits_check_decompression(&lim, 0u, 1u) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(rfc_limits_check_decompression(&lim, 1024u, 4096u) == RFC_OK);

    rfc_provider_memory_info minfo;
    CHECK(rfc_query_provider_memory_contract(RFC_FORMAT_PNG, &minfo) == RFC_OK);
    CHECK(minfo.mode == RFC_MEM_HOOK_ENFORCED);
    CHECK(minfo.limitation_reason != NULL && minfo.limitation_reason[0] != '\0');
    CHECK(rfc_query_provider_memory_contract(RFC_FORMAT_JPEG_XL, &minfo) ==
          RFC_OK);
    CHECK(minfo.mode == RFC_MEM_HOOK_ENFORCED);
    CHECK(rfc_query_provider_memory_contract(RFC_FORMAT_CAMERA_RAW, &minfo) ==
          RFC_OK);
    CHECK(minfo.mode == RFC_MEM_RESERVATION_AND_LIMITS);
    CHECK(minfo.working_set_multiplier >= 1u);
    CHECK(rfc_query_provider_memory_contract((rfc_format_id)9999, &minfo) ==
          RFC_ERR_UNKNOWN_FORMAT);

    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.memory_budget_bytes = 256u * 1024u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    rfc_job_spec spec;
    rfc_job *job = NULL;

    /* Unknown reservation: expected_working_set_bytes=0 and source_layout=NULL */
    init_basic_spec(&spec, NULL, 0, NULL);
    spec.expected_working_set_bytes = 0;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(job == NULL);

    /* Over-budget reservation */
    init_basic_spec(&spec, NULL, 0, &normal);
    spec.expected_working_set_bytes = cfg.memory_budget_bytes + 4096u;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(job == NULL);

    /* Hostile layout on submit */
    init_basic_spec(&spec, NULL, 0, &hostile);
    spec.expected_working_set_bytes = 4096u;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(job == NULL);

    /* Decompression expansion ratio exceeded in submit preflight */
    uint8_t tiny[4] = {1, 2, 3, 4};
    rfc_limits tight = cfg.default_limits;
    tight.max_expansion_ratio = 2u;
    init_basic_spec(&spec, tiny, sizeof(tiny), &normal); /* 12288 > 4*2 */
    spec.limits_override = &tight;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(job == NULL);

    /* Derive reservation cleanly from source_layout + provider memory contract */
    rfc_layout big_layout = normal;
    big_layout.width = 512u;
    big_layout.height = 512u;
    big_layout.stride_bytes = 512u * 3u; /* ~786KB * mult > 256KB */
    init_basic_spec(&spec, NULL, 0, &big_layout);
    spec.expected_working_set_bytes = 0u;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(job == NULL);

    rfc_scheduler_stats stats;
    rfc_scheduler_stats_get(sched, &stats);
    CHECK(stats.total_submitted == 0u);
    CHECK(stats.active_reserved_bytes == 0u);

    rfc_budget *b = rfc_budget_create(1024u);
    CHECK(b != NULL);
    CHECK(rfc_budget_alloc(b, SIZE_MAX) == NULL);
    rfc_budget_stats bstats;
    rfc_budget_stats_get(b, &bstats);
    CHECK(bstats.rejected_count == 1u && bstats.current_allocated_bytes == 0u);
    rfc_budget_destroy(b);

    printf("jobs: hostile/unknown preflight refused total_submitted=%llu "
           "peak_alloc=0\n",
           (unsigned long long)stats.total_submitted);

    rfc_scheduler_destroy(sched);
}

/* 3. Bounded concurrent CPU threads and memory reservations */
static rfc_status concurrency_barrier_stage(rfc_job *job,
                                            const rfc_exec_context *exec,
                                            void *user_ctx) {
    (void)job;
    barrier_state *b = (barrier_state *)user_ctx;
    test_mutex_lock(&b->lock);
    b->active_now++;
    if (b->active_now > b->max_concurrent) {
        b->max_concurrent = b->active_now;
    }
    b->arrived++;
    b->total_runs++;
    b->last_seen_threads = rfc_exec_threads(exec);
    test_cond_broadcast(&b->cond);
    while (!b->release_flag && !rfc_exec_is_cancelled(exec)) {
        test_cond_wait(&b->cond, &b->lock);
    }
    b->active_now--;
    test_mutex_unlock(&b->lock);
    return rfc_exec_is_cancelled(exec) ? RFC_ERR_CANCELLED : RFC_OK;
}

static void test_bounded_cpu_and_memory_concurrency(void) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 4u;
    cfg.total_cpu_threads = 4u;
    cfg.memory_budget_bytes = 100u * 1024u;

    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    barrier_state bar;
    barrier_init(&bar);

    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 16u, 16u, 48u, 8u};
    rfc_job *jobs[6] = {NULL};

    for (size_t i = 0; i < 6; i++) {
        rfc_job_spec spec;
        init_basic_spec(&spec, NULL, 0, &lay);
        spec.requested_codec_threads = 2u;
        spec.expected_working_set_bytes = 40u * 1024u;
        spec.stage_ops.on_encode = concurrency_barrier_stage;
        spec.stage_ops.user_ctx = &bar;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &jobs[i]) == RFC_OK);
    }

    /* Wait until the 2 admissible concurrent slots have arrived at encode barrier. */
    test_mutex_lock(&bar.lock);
    while (bar.arrived < 2u) {
        test_cond_wait(&bar.cond, &bar.lock);
    }
    rfc_scheduler_stats live_stats;
    rfc_scheduler_stats_get(sched, &live_stats);
    CHECK(live_stats.active_workers <= cfg.max_workers);
    CHECK(live_stats.active_cpu_threads == 4u);
    CHECK(live_stats.active_reserved_bytes == 80u * 1024u);
    CHECK(live_stats.queued_jobs == 4u);
    bar.release_flag = 1u;
    test_cond_broadcast(&bar.cond);
    test_mutex_unlock(&bar.lock);

    for (size_t i = 0; i < 6; i++) {
        CHECK(rfc_job_wait(jobs[i], UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(jobs[i], &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED);
        CHECK(snap.admitted_threads == 2u);
        CHECK(snap.reserved_memory_bytes == 40u * 1024u);
        rfc_job_release(jobs[i]);
    }

    /* Oversized CPU request clamps safely to total_cpu_threads without deadlock. */
    barrier_destroy(&bar);
    barrier_init(&bar);
    bar.release_flag = 1u;
    rfc_job_spec clamp_spec;
    init_basic_spec(&clamp_spec, NULL, 0, &lay);
    clamp_spec.requested_codec_threads = 16u;
    clamp_spec.expected_working_set_bytes = 16u * 1024u;
    clamp_spec.stage_ops.on_encode = concurrency_barrier_stage;
    clamp_spec.stage_ops.user_ctx = &bar;
    rfc_job *clamped = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &clamp_spec, &clamped) == RFC_OK);
    CHECK(rfc_job_wait(clamped, UINT32_MAX) == RFC_OK);
    rfc_job_snapshot csnap;
    CHECK(rfc_job_snapshot_get(clamped, &csnap) == RFC_OK);
    CHECK(csnap.admitted_threads == cfg.total_cpu_threads);
    CHECK(bar.last_seen_threads == cfg.total_cpu_threads);
    rfc_job_release(clamped);

    rfc_scheduler_stats final_stats;
    rfc_scheduler_stats_get(sched, &final_stats);
    CHECK(final_stats.peak_workers <= cfg.max_workers);
    CHECK(final_stats.peak_cpu_threads <= cfg.total_cpu_threads);
    CHECK(final_stats.peak_reserved_bytes <= cfg.memory_budget_bytes);
    CHECK(final_stats.active_cpu_threads == 0u);
    CHECK(final_stats.active_reserved_bytes == 0u);
    CHECK(final_stats.total_completed == 7u);

    printf("jobs: concurrency peak_workers=%u/%u peak_cpu=%u/%u "
           "peak_mem=%llu/%lluB completed=%llu\n",
           final_stats.peak_workers, cfg.max_workers,
           final_stats.peak_cpu_threads, cfg.total_cpu_threads,
           (unsigned long long)final_stats.peak_reserved_bytes,
           (unsigned long long)cfg.memory_budget_bytes,
           (unsigned long long)final_stats.total_completed);

    barrier_destroy(&bar);
    rfc_scheduler_destroy(sched);
}

/* 4. OOM hook cap enforcement & partial allocation cleanup */
static rfc_status oom_hook_stage(rfc_job *job,
                                 const rfc_exec_context *exec,
                                 void *user_ctx) {
    (void)user_ctx;
    void *p1 = rfc_job_alloc_partial(job, 8192u);
    if (!p1) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_budget *b = rfc_exec_budget(exec);
    void *pz = rfc_budget_zlib_alloc(b, 1u, 4096u);
    if (!pz) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    /* Exceeds 16 KiB limit (8192 + 4096 + 8192 = 20480 > 16384): must refuse. */
    void *pj = rfc_budget_jxl_alloc(b, 8192u);
    rfc_budget_zlib_free(b, pz);
    if (!pj) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_budget_jxl_free(b, pj);
    return RFC_OK;
}

static void test_oom_hook_cap(void) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    cfg.total_cpu_threads = 2u;
    cfg.memory_budget_bytes = 64u * 1024u;

    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 16u, 16u, 48u, 8u};
    rfc_job_spec spec;
    init_basic_spec(&spec, NULL, 0, &lay);
    spec.expected_working_set_bytes = 16384u;
    spec.job_memory_limit_bytes = 16384u;
    spec.stage_ops.on_encode = oom_hook_stage;

    rfc_job *job = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);

    rfc_job_snapshot snap;
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_FAILED);
    CHECK(snap.status == RFC_ERR_OUT_OF_MEMORY);
    CHECK(snap.failing_stage == RFC_STAGE_ENCODE);
    CHECK(snap.budget.limit_bytes == 16384u);
    CHECK(snap.budget.peak_allocated_bytes == 12288u);
    CHECK(snap.budget.peak_allocated_bytes <= snap.budget.limit_bytes);
    CHECK(snap.budget.current_allocated_bytes == 0u);
    CHECK(snap.budget.rejected_count >= 1u);
    CHECK(snap.live_partial_bytes == 0u);

    printf("jobs: oom_cap limit=%llu peak_allocated_bytes=%llu rejected=%llu "
           "live=%llu\n",
           (unsigned long long)snap.budget.limit_bytes,
           (unsigned long long)snap.budget.peak_allocated_bytes,
           (unsigned long long)snap.budget.rejected_count,
           (unsigned long long)snap.budget.current_allocated_bytes);

    rfc_job_release(job);
    rfc_scheduler_destroy(sched);
}

/* 5. Same-format session isolation, BUSY unregister, and missing stage refusals */
typedef struct sess_tracker {
    test_mutex_t lock;
    test_cond_t cond;
    uint32_t open_count;
    uint32_t close_count;
    uint32_t in_decode;
    uint32_t release_decode;
    rfc_job_id seen_ids[2];
    void *seen_handles[2];
    uint32_t distinct_count;
} sess_tracker;

static sess_tracker g_sess_tracker;

typedef struct mock_session {
    rfc_job_id job_id;
    uint32_t threads;
    const rfc_allocator *alloc;
    uint32_t encode_calls;
} mock_session;

static rfc_status mock_open_session(const rfc_exec_context *exec,
                                    void **out_sess) {
    CHECK(exec != NULL);
    CHECK(rfc_exec_threads(exec) >= 1u);
    CHECK(rfc_exec_limits(exec) != NULL);
    CHECK(rfc_exec_allocator(exec) != NULL);
    CHECK(rfc_exec_budget(exec) != NULL);

    mock_session *s = (mock_session *)exec->allocator->alloc(
        exec->allocator->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(s, 0, sizeof(*s));
    s->job_id = exec->job_id;
    s->threads = exec->codec_threads;
    s->alloc = exec->allocator;

    test_mutex_lock(&g_sess_tracker.lock);
    uint32_t idx = g_sess_tracker.open_count++;
    if (idx < 2u) {
        g_sess_tracker.seen_ids[idx] = exec->job_id;
        g_sess_tracker.seen_handles[idx] = s;
        g_sess_tracker.distinct_count++;
    }
    test_mutex_unlock(&g_sess_tracker.lock);

    *out_sess = s;
    return RFC_OK;
}

static void mock_close_session(void *session) {
    mock_session *s = (mock_session *)session;
    if (!s) {
        return;
    }
    test_mutex_lock(&g_sess_tracker.lock);
    g_sess_tracker.close_count++;
    test_mutex_unlock(&g_sess_tracker.lock);
    const rfc_allocator *a = s->alloc;
    a->free(a->ctx, s);
}

static rfc_status mock_decode_session(void *session,
                                      const rfc_exec_context *exec,
                                      const uint8_t *bytes,
                                      size_t size,
                                      const rfc_setting_value *settings,
                                      size_t setting_count,
                                      rfc_frame **out_frames,
                                      size_t *out_frame_count,
                                      rfc_metadata *out_meta) {
    (void)bytes;
    (void)size;
    (void)settings;
    (void)setting_count;
    mock_session *s = (mock_session *)session;
    CHECK(s->job_id == exec->job_id);

    test_mutex_lock(&g_sess_tracker.lock);
    g_sess_tracker.in_decode++;
    test_cond_broadcast(&g_sess_tracker.cond);
    while (!g_sess_tracker.release_decode && !rfc_exec_is_cancelled(exec)) {
        test_cond_wait(&g_sess_tracker.cond, &g_sess_tracker.lock);
    }
    test_mutex_unlock(&g_sess_tracker.lock);

    if (rfc_exec_is_cancelled(exec)) {
        return RFC_ERR_CANCELLED;
    }

    rfc_frame *frames = (rfc_frame *)exec->allocator->alloc(
        exec->allocator->ctx, sizeof(*frames));
    if (!frames) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(frames, 0, sizeof(*frames));
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
    rfc_metadata meta;
    memset(&meta, 0, sizeof(meta));
    rfc_status st = rfc_frame_alloc(&frames[0], &lay, &meta, exec->allocator);
    if (st != RFC_OK) {
        exec->allocator->free(exec->allocator->ctx, frames);
        return st;
    }
    memset(frames[0].data, 0x42, frames[0].data_len);
    *out_frames = frames;
    *out_frame_count = 1u;
    *out_meta = meta;
    return RFC_OK;
}

static rfc_status mock_encode_session(void *session,
                                      const rfc_exec_context *exec,
                                      const rfc_frame *frames,
                                      size_t frame_count,
                                      const rfc_metadata *meta,
                                      const rfc_setting_value *settings,
                                      size_t setting_count,
                                      rfc_encoded *out_encoded) {
    (void)meta;
    (void)settings;
    (void)setting_count;
    mock_session *s = (mock_session *)session;
    CHECK(s->job_id == exec->job_id);
    CHECK(frames != NULL && frame_count == 1u);
    s->encode_calls++;

    uint8_t *buf = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx, 32u);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(buf, 0x77, 32u);
    out_encoded->data = buf;
    out_encoded->len = 32u;
    out_encoded->allocator = exec->allocator;
    return RFC_OK;
}

static void mock_cancel_session(void *session) {
    (void)session;
    test_mutex_lock(&g_sess_tracker.lock);
    test_cond_broadcast(&g_sess_tracker.cond);
    test_mutex_unlock(&g_sess_tracker.lock);
}

static void test_same_format_sessions_and_missing_stages(void) {
    memset(&g_sess_tracker, 0, sizeof(g_sess_tracker));
    test_mutex_init(&g_sess_tracker.lock);
    test_cond_init(&g_sess_tracker.cond);

    rfc_session_adapter qoi_sess;
    memset(&qoi_sess, 0, sizeof(qoi_sess));
    qoi_sess.abi_version = RFC_CODEC_ABI_VERSION;
    qoi_sess.format = RFC_FORMAT_QOI;
    qoi_sess.open_session = mock_open_session;
    qoi_sess.close_session = mock_close_session;
    qoi_sess.decode_session = mock_decode_session;
    qoi_sess.encode_session = mock_encode_session;
    qoi_sess.cancel_session = mock_cancel_session;
    CHECK(rfc_registry_register_session(&qoi_sess) == RFC_OK);
    CHECK(rfc_registry_register_session(&qoi_sess) == RFC_ERR_DUPLICATE);

    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 2u;
    cfg.total_cpu_threads = 2u;
    cfg.memory_budget_bytes = 2u * 1024u * 1024u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    uint8_t dummy_src[16] = {1, 2, 3, 4};
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
    rfc_job_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.source_format = RFC_FORMAT_QOI;
    spec.target_format = RFC_FORMAT_QOI;
    spec.source_bytes = dummy_src;
    spec.source_size = sizeof(dummy_src);
    spec.source_layout = &lay;
    spec.requested_codec_threads = 1u;
    spec.expected_working_set_bytes = 16384u;
    spec.has_stage_ops = 1;
    spec.stage_ops.on_verify = ok_noop_stage;
    spec.stage_ops.on_export = export_mark_ok;

    rfc_job *j1 = NULL;
    rfc_job *j2 = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j1) == RFC_OK);
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j2) == RFC_OK);

    test_mutex_lock(&g_sess_tracker.lock);
    while (g_sess_tracker.in_decode < 2u) {
        test_cond_wait(&g_sess_tracker.cond, &g_sess_tracker.lock);
    }
    test_mutex_unlock(&g_sess_tracker.lock);

    /* Registry unregister must refuse with RFC_ERR_BUSY while active. */
    CHECK(rfc_registry_unregister_session(RFC_FORMAT_QOI) == RFC_ERR_BUSY);

    test_mutex_lock(&g_sess_tracker.lock);
    g_sess_tracker.release_decode = 1u;
    test_cond_broadcast(&g_sess_tracker.cond);
    test_mutex_unlock(&g_sess_tracker.lock);

    CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_wait(j2, UINT32_MAX) == RFC_OK);
    rfc_job_snapshot s1, s2;
    CHECK(rfc_job_snapshot_get(j1, &s1) == RFC_OK);
    CHECK(rfc_job_snapshot_get(j2, &s2) == RFC_OK);
    CHECK(s1.state == RFC_JOB_SUCCEEDED && s2.state == RFC_JOB_SUCCEEDED);
    CHECK(g_sess_tracker.distinct_count == 2u);
    CHECK(g_sess_tracker.seen_ids[0] != g_sess_tracker.seen_ids[1]);
    CHECK(g_sess_tracker.seen_handles[0] != g_sess_tracker.seen_handles[1]);
    CHECK(g_sess_tracker.open_count == g_sess_tracker.close_count);

    rfc_job_release(j1);
    rfc_job_release(j2);

    /* Unregister succeeds cleanly once sessions close and references release. */
    CHECK(rfc_registry_unregister_session(RFC_FORMAT_QOI) == RFC_OK);

    /* Missing required stages refuse loudly with RFC_ERR_NO_ADAPTER / RFC_ERR_PLANNED. */
    rfc_job_spec miss_ver;
    init_basic_spec(&miss_ver, dummy_src, sizeof(dummy_src), &lay);
    miss_ver.stage_ops.on_verify = NULL;
    rfc_job *mj = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &miss_ver, &mj) == RFC_OK);
    CHECK(rfc_job_wait(mj, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(mj, &s1) == RFC_OK);
    CHECK(s1.state == RFC_JOB_FAILED);
    CHECK(s1.status == RFC_ERR_NO_ADAPTER);
    CHECK(s1.failing_stage == RFC_STAGE_VERIFY);
    rfc_job_release(mj);

    rfc_job_spec miss_exp;
    init_basic_spec(&miss_exp, dummy_src, sizeof(dummy_src), &lay);
    miss_exp.stage_ops.on_export = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &miss_exp, &mj) == RFC_OK);
    CHECK(rfc_job_wait(mj, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_snapshot_get(mj, &s1) == RFC_OK);
    CHECK(s1.state == RFC_JOB_FAILED);
    CHECK(s1.status == RFC_ERR_NO_ADAPTER);
    CHECK(s1.failing_stage == RFC_STAGE_EXPORT);
    rfc_job_release(mj);

    for (size_t i = 0; i < rfc_registry_count(); i++) {
        const rfc_capability *cap = rfc_registry_at(i);
        if (cap && cap->availability == RFC_AVAILABILITY_PLANNED) {
            rfc_job_spec plan_spec;
            init_basic_spec(&plan_spec, dummy_src, sizeof(dummy_src), &lay);
            plan_spec.source_format = cap->id;
            plan_spec.stage_ops.on_import = NULL;
            CHECK(rfc_scheduler_submit(sched, NULL, &plan_spec, &mj) ==
                  RFC_OK);
            CHECK(rfc_job_wait(mj, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(mj, &s1) == RFC_OK);
            CHECK(s1.state == RFC_JOB_FAILED);
            CHECK(s1.status == RFC_ERR_PLANNED);
            CHECK(s1.failing_stage == RFC_STAGE_IMPORT);
            rfc_job_release(mj);
            break;
        }
    }

    printf("jobs: same_format_sessions distinct=%u open_close=%u/%u "
           "missing_verify=%s missing_export=%s\n",
           g_sess_tracker.distinct_count, g_sess_tracker.open_count,
           g_sess_tracker.close_count, rfc_status_name(RFC_ERR_NO_ADAPTER),
           rfc_status_name(RFC_ERR_NO_ADAPTER));

    rfc_scheduler_destroy(sched);
    test_cond_destroy(&g_sess_tracker.cond);
    test_mutex_destroy(&g_sess_tracker.lock);
}

/* 6. Deep copy, monotonic progress outside locks, retry, stale post-retry reject */
typedef struct retry_ctx {
    rfc_scheduler *sched;
    test_mutex_t lock;
    test_cond_t cond;
    int wiped_by_caller;
    uint32_t import_runs;
    uint32_t fail_first_encode;
    int deep_copy_ok;
    int progress_monotonic;
    int lockfree_cb_ok;
    uint32_t last_pct_for_id1;
    uint32_t stale_cb_after_supersede;
    rfc_job_id first_id;
    rfc_job_id second_id;
    int superseded_set;
} retry_ctx;

static void retry_progress_cb(const rfc_job_progress *p, void *user) {
    retry_ctx *rc = (retry_ctx *)user;
    test_mutex_lock(&rc->lock);
    if (rc->superseded_set && p->job_id == rc->first_id) {
        rc->stale_cb_after_supersede++;
    }
    if (p->job_id == rc->first_id) {
        if (p->percent < rc->last_pct_for_id1) {
            rc->progress_monotonic = 0;
        }
        rc->last_pct_for_id1 = p->percent;
    }
    test_mutex_unlock(&rc->lock);

    /* Must not deadlock calling stats/current slot queries from inside callback. */
    rfc_scheduler_stats st;
    rfc_scheduler_stats_get(rc->sched, &st);
    rc->lockfree_cb_ok = 1;
}

static rfc_status retry_import_stage(rfc_job *job,
                                     const rfc_exec_context *exec,
                                     void *user_ctx) {
    retry_ctx *rc = (retry_ctx *)user_ctx;
    test_mutex_lock(&rc->lock);
    while (!rc->wiped_by_caller) {
        test_cond_wait(&rc->cond, &rc->lock);
    }
    rc->import_runs++;
    test_mutex_unlock(&rc->lock);

    /* Verify deep-copied source bytes, metadata ICC, and settings survive caller wipe and retry. */
    size_t slen = 0;
    const uint8_t *sbytes = rfc_job_source_bytes(job, &slen);
    const uint8_t expected_src[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    if (!sbytes || slen != 16u || memcmp(sbytes, expected_src, 8u) != 0) {
        rc->deep_copy_ok = 0;
        return RFC_ERR_INVALID_ARGUMENT;
    }
    const rfc_metadata *smeta = rfc_job_source_metadata(job);
    const uint8_t expected_icc[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    if (!smeta || !smeta->icc || smeta->icc_len != 8u ||
        memcmp(smeta->icc, expected_icc, 4u) != 0) {
        rc->deep_copy_ok = 0;
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t scount = 0;
    const rfc_setting_value *sset = rfc_job_settings(job, &scount);
    if (!sset || scount != 1u || !sset[0].id || !sset[0].value ||
        strcmp(sset[0].id, "quality") != 0 ||
        strcmp(sset[0].value, "95") != 0) {
        rc->deep_copy_ok = 0;
        return RFC_ERR_SETTING_VALUE;
    }

    /* Reject invalid out-of-order transition right inside running job. */
    if (rfc_job_advance_stage(job, exec->job_id, RFC_STAGE_IMPORT,
                              RFC_STAGE_EXPORT) != RFC_ERR_INVALID_ARGUMENT) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    CHECK(rfc_job_report_progress(job, exec->job_id, RFC_STAGE_IMPORT, 1u, 4u,
                                  "importing") == RFC_OK);
    CHECK(rfc_job_report_progress(job, exec->job_id, RFC_STAGE_IMPORT, 0u, 4u,
                                  "regression clamped") == RFC_OK);
    return RFC_OK;
}

static rfc_status retry_encode_stage(rfc_job *job,
                                     const rfc_exec_context *exec,
                                     void *user_ctx) {
    retry_ctx *rc = (retry_ctx *)user_ctx;
    CHECK(rfc_job_report_progress(job, exec->job_id, RFC_STAGE_ENCODE, 2u, 4u,
                                  "encoding") == RFC_OK);
    if (rc->fail_first_encode) {
        rc->fail_first_encode = 0u;
        CHECK(rfc_job_complete_stage(job, exec->job_id, RFC_STAGE_ENCODE,
                                     RFC_ERR_SETTING_VALUE,
                                     "injected transient encode failure") ==
              RFC_OK);
        return RFC_ERR_SETTING_VALUE;
    }
    CHECK(rfc_job_report_progress(job, exec->job_id, RFC_STAGE_ENCODE, 4u, 4u,
                                  "encoded") == RFC_OK);
    return RFC_OK;
}

static void test_progress_retry_and_stale_rejection(void) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    rfc_job_slot *slot = rfc_job_slot_create();
    CHECK(slot != NULL);

    retry_ctx rctx;
    memset(&rctx, 0, sizeof(rctx));
    rctx.sched = sched;
    rctx.fail_first_encode = 1u;
    rctx.progress_monotonic = 1;
    rctx.deep_copy_ok = 1;
    test_mutex_init(&rctx.lock);
    test_cond_init(&rctx.cond);

    uint8_t src_buf[16] = {9, 8, 7, 6, 5, 4, 3, 2};
    uint8_t icc_buf[8] = {0xAA, 0xBB, 0xCC, 0xDD};
    char k1[] = "quality";
    char v1[] = "95";
    rfc_setting_value sets[1] = {{k1, v1}};
    rfc_metadata meta = {icc_buf, sizeof(icc_buf), RFC_ORIENTATION_NORMAL, 0u};
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 24u, 8u};

    rfc_job_spec spec;
    init_basic_spec(&spec, src_buf, sizeof(src_buf), &lay);
    spec.source_meta = &meta;
    spec.settings = sets;
    spec.setting_count = 1u;
    spec.stage_ops.on_import = retry_import_stage;
    spec.stage_ops.on_encode = retry_encode_stage;
    spec.stage_ops.user_ctx = &rctx;
    spec.progress_fn = retry_progress_cb;
    spec.progress_user = &rctx;

    rfc_job *j1 = NULL;
    CHECK(rfc_scheduler_submit(sched, slot, &spec, &j1) == RFC_OK);
    rctx.first_id = rfc_job_id_get(j1);
    CHECK(rfc_job_slot_current_id(slot) == rctx.first_id);

    /* Immediately wipe caller input buffers BEFORE unblocking import stage. */
    memset(src_buf, 0x00, sizeof(src_buf));
    memset(icc_buf, 0x00, sizeof(icc_buf));
    memset(k1, 'X', sizeof(k1) - 1u);
    memset(v1, 'X', sizeof(v1) - 1u);
    test_mutex_lock(&rctx.lock);
    rctx.wiped_by_caller = 1;
    test_cond_broadcast(&rctx.cond);
    test_mutex_unlock(&rctx.lock);

    CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);
    rfc_job_snapshot s1;
    CHECK(rfc_job_snapshot_get(j1, &s1) == RFC_OK);
    CHECK(s1.state == RFC_JOB_FAILED);
    CHECK(s1.status == RFC_ERR_SETTING_VALUE);
    CHECK(s1.failing_stage == RFC_STAGE_ENCODE);
    CHECK(s1.attempt_number == 1u);
    CHECK(strcmp(s1.detail, "injected transient encode failure") == 0);
    CHECK(rctx.progress_monotonic == 1 && rctx.lockfree_cb_ok == 1 &&
          rctx.deep_copy_ok == 1);

    /* Retry preserves source/settings cleanly under a strictly higher immutable ID. */
    rfc_job *j2 = NULL;
    test_mutex_lock(&rctx.lock);
    rctx.superseded_set = 1;
    test_mutex_unlock(&rctx.lock);
    CHECK(rfc_scheduler_retry(sched, slot, &j2) == RFC_OK);
    rctx.second_id = rfc_job_id_get(j2);
    CHECK(rctx.second_id > rctx.first_id);
    CHECK(rfc_job_slot_current_id(slot) == rctx.second_id);

    /* Stale operations on j1 after retry MUST fail with RFC_ERR_STALE_JOB. */
    rfc_status stale_prog = rfc_job_report_progress(
        j1, rctx.first_id, RFC_STAGE_VERIFY, 3u, 4u, "stale callback");
    rfc_status stale_adv = rfc_job_advance_stage(
        j1, rctx.first_id, RFC_STAGE_ENCODE, RFC_STAGE_VERIFY);
    rfc_status stale_comp = rfc_job_complete_stage(
        j1, rctx.first_id, RFC_STAGE_ENCODE, RFC_OK, "stale complete");
    rfc_status stale_pub = rfc_job_mark_published(j1, rctx.first_id);
    CHECK(stale_prog == RFC_ERR_STALE_JOB);
    CHECK(stale_adv == RFC_ERR_STALE_JOB);
    CHECK(stale_comp == RFC_ERR_STALE_JOB);
    CHECK(stale_pub == RFC_ERR_STALE_JOB);
    CHECK(rctx.stale_cb_after_supersede == 0u);

    CHECK(rfc_job_wait(j2, UINT32_MAX) == RFC_OK);
    rfc_job_snapshot s2;
    CHECK(rfc_job_snapshot_get(j2, &s2) == RFC_OK);
    CHECK(s2.state == RFC_JOB_SUCCEEDED);
    CHECK(s2.status == RFC_OK);
    CHECK(s2.attempt_number == 2u);
    CHECK(rctx.import_runs == 2u && rctx.deep_copy_ok == 1);

    rfc_scheduler_stats rstats;
    rfc_scheduler_stats_get(sched, &rstats);
    CHECK(rstats.total_submitted == rstats.total_completed + rstats.total_failed +
                                        rstats.total_cancelled +
                                        rstats.total_superseded);

    printf("jobs: retry id1=%llu id2=%llu stale_prog=%s stale_pub=%s "
           "attempt=%llu deep_copy=%d\n",
           (unsigned long long)rctx.first_id,
           (unsigned long long)rctx.second_id,
           rfc_status_name(stale_prog), rfc_status_name(stale_pub),
           (unsigned long long)s2.attempt_number, rctx.deep_copy_ok);

    rfc_job_release(j1);
    rfc_job_release(j2);
    rfc_job_slot_destroy(slot);
    rfc_scheduler_destroy(sched);
    test_cond_destroy(&rctx.cond);
    test_mutex_destroy(&rctx.lock);
}

/* 7. All-stage cancellation, 64x rapid restart, and shutdown callback safety */
typedef struct stage_cancel_ctx {
    test_mutex_t lock;
    test_cond_t cond;
    rfc_job_stage stop_at;
    int reached;
    int proceed;
    uint32_t export_published;
} stage_cancel_ctx;

static rfc_status cancel_stage_hook(rfc_job *job,
                                    const rfc_exec_context *exec,
                                    stage_cancel_ctx *cc,
                                    rfc_job_stage cur,
                                    int is_export) {
    if (cc->stop_at == cur) {
        void *p = rfc_job_alloc_partial(job, 4096u);
        CHECK(p != NULL);
        test_mutex_lock(&cc->lock);
        cc->reached = 1;
        test_cond_broadcast(&cc->cond);
        while (!cc->proceed) {
            test_cond_wait(&cc->cond, &cc->lock);
        }
        test_mutex_unlock(&cc->lock);
    }
    if (is_export) {
        if (rfc_exec_is_cancelled(exec)) {
            return RFC_ERR_CANCELLED;
        }
        rfc_status pst = rfc_job_mark_published(job, exec->job_id);
        if (pst == RFC_OK) {
            cc->export_published++;
        }
        return pst;
    }
    return RFC_OK;
}

static rfc_status cc_import(rfc_job *j, const rfc_exec_context *e, void *u) {
    return cancel_stage_hook(j, e, (stage_cancel_ctx *)u, RFC_STAGE_IMPORT, 0);
}
static rfc_status cc_encode(rfc_job *j, const rfc_exec_context *e, void *u) {
    return cancel_stage_hook(j, e, (stage_cancel_ctx *)u, RFC_STAGE_ENCODE, 0);
}
static rfc_status cc_verify(rfc_job *j, const rfc_exec_context *e, void *u) {
    return cancel_stage_hook(j, e, (stage_cancel_ctx *)u, RFC_STAGE_VERIFY, 0);
}
static rfc_status cc_export(rfc_job *j, const rfc_exec_context *e, void *u) {
    return cancel_stage_hook(j, e, (stage_cancel_ctx *)u, RFC_STAGE_EXPORT, 1);
}

typedef struct shutdown_ctx {
    test_mutex_t lock;
    test_cond_t cond;
    uint32_t running;
    int stop_loop;
    int destroyed;
    uint32_t callbacks_after_destroy;
} shutdown_ctx;

static void shutdown_cb(const rfc_job_progress *p, void *user) {
    (void)p;
    shutdown_ctx *sc = (shutdown_ctx *)user;
    test_mutex_lock(&sc->lock);
    if (sc->destroyed) {
        sc->callbacks_after_destroy++;
    }
    test_mutex_unlock(&sc->lock);
}

static rfc_status shutdown_worker_stage(rfc_job *job,
                                        const rfc_exec_context *exec,
                                        void *user) {
    shutdown_ctx *sc = (shutdown_ctx *)user;
    test_mutex_lock(&sc->lock);
    sc->running++;
    test_cond_broadcast(&sc->cond);
    test_mutex_unlock(&sc->lock);

    uint32_t iter = 0;
    while (!rfc_exec_is_cancelled(exec)) {
        (void)rfc_job_report_progress(job, exec->job_id, RFC_STAGE_ENCODE,
                                      iter % 100u, 100u, "running");
        iter++;
    }
    return RFC_ERR_CANCELLED;
}

static void test_cancellation_64x_and_shutdown(void) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    cfg.total_cpu_threads = 1u;
    cfg.memory_budget_bytes = 512u * 1024u;

    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 24u, 8u};

    /* A. Queued cancellation behind a blocker */
    stage_cancel_ctx block_cc;
    memset(&block_cc, 0, sizeof(block_cc));
    test_mutex_init(&block_cc.lock);
    test_cond_init(&block_cc.cond);
    block_cc.stop_at = RFC_STAGE_IMPORT;

    rfc_job_spec bspec;
    init_basic_spec(&bspec, NULL, 0, &lay);
    bspec.stage_ops.on_import = cc_import;
    bspec.stage_ops.on_encode = cc_encode;
    bspec.stage_ops.on_verify = cc_verify;
    bspec.stage_ops.on_export = cc_export;
    bspec.stage_ops.user_ctx = &block_cc;

    rfc_job *blocker = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &bspec, &blocker) == RFC_OK);
    test_mutex_lock(&block_cc.lock);
    while (!block_cc.reached) {
        test_cond_wait(&block_cc.cond, &block_cc.lock);
    }
    test_mutex_unlock(&block_cc.lock);

    stage_cancel_ctx queued_cc;
    memset(&queued_cc, 0, sizeof(queued_cc));
    test_mutex_init(&queued_cc.lock);
    test_cond_init(&queued_cc.cond);
    rfc_job_spec qspec;
    init_basic_spec(&qspec, NULL, 0, &lay);
    qspec.stage_ops.on_export = cc_export;
    qspec.stage_ops.user_ctx = &queued_cc;
    rfc_job *queued_job = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &qspec, &queued_job) == RFC_OK);
    CHECK(rfc_job_cancel(queued_job) == RFC_OK);

    test_mutex_lock(&block_cc.lock);
    block_cc.proceed = 1;
    test_cond_broadcast(&block_cc.cond);
    test_mutex_unlock(&block_cc.lock);

    CHECK(rfc_job_wait(queued_job, UINT32_MAX) == RFC_OK);
    CHECK(rfc_job_wait(blocker, UINT32_MAX) == RFC_OK);
    rfc_job_snapshot qsnap;
    CHECK(rfc_job_snapshot_get(queued_job, &qsnap) == RFC_OK);
    CHECK(qsnap.state == RFC_JOB_CANCELLED);
    CHECK(queued_cc.export_published == 0u);
    rfc_job_release(queued_job);
    rfc_job_release(blocker);
    test_cond_destroy(&queued_cc.cond);
    test_mutex_destroy(&queued_cc.lock);
    test_cond_destroy(&block_cc.cond);
    test_mutex_destroy(&block_cc.lock);

    /* B. Active cancellation at every stage (IMPORT, ENCODE, VERIFY, EXPORT) */
    const rfc_job_stage stages[4] = {RFC_STAGE_IMPORT, RFC_STAGE_ENCODE,
                                     RFC_STAGE_VERIFY, RFC_STAGE_EXPORT};
    for (size_t i = 0; i < 4; i++) {
        stage_cancel_ctx cc;
        memset(&cc, 0, sizeof(cc));
        test_mutex_init(&cc.lock);
        test_cond_init(&cc.cond);
        cc.stop_at = stages[i];

        rfc_job_spec sspec;
        init_basic_spec(&sspec, NULL, 0, &lay);
        sspec.stage_ops.on_import = cc_import;
        sspec.stage_ops.on_encode = cc_encode;
        sspec.stage_ops.on_verify = cc_verify;
        sspec.stage_ops.on_export = cc_export;
        sspec.stage_ops.user_ctx = &cc;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sspec, &job) == RFC_OK);

        test_mutex_lock(&cc.lock);
        while (!cc.reached) {
            test_cond_wait(&cc.cond, &cc.lock);
        }
        test_mutex_unlock(&cc.lock);

        CHECK(rfc_job_cancel(job) == RFC_OK);

        test_mutex_lock(&cc.lock);
        cc.proceed = 1;
        test_cond_broadcast(&cc.cond);
        test_mutex_unlock(&cc.lock);

        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_CANCELLED);
        CHECK(snap.status == RFC_ERR_CANCELLED);
        CHECK(snap.live_partial_bytes == 0u);
        CHECK(snap.budget.current_allocated_bytes == 0u);
        CHECK(cc.export_published == 0u);

        rfc_job_release(job);
        test_cond_destroy(&cc.cond);
        test_mutex_destroy(&cc.lock);
    }

    /* C. 64x rapid start/cancel and retry cycle */
    rfc_job_slot *rslot = rfc_job_slot_create();
    CHECK(rslot != NULL);
    for (size_t k = 0; k < 64u; k++) {
        rfc_job_spec rspec;
        init_basic_spec(&rspec, NULL, 0, &lay);
        rfc_job *rj = NULL;
        CHECK(rfc_scheduler_submit(sched, rslot, &rspec, &rj) == RFC_OK);
        if ((k & 1u) == 0u) {
            CHECK(rfc_job_cancel(rj) == RFC_OK);
        } else {
            rfc_job *rj2 = NULL;
            CHECK(rfc_scheduler_retry(sched, rslot, &rj2) == RFC_OK);
            CHECK(rfc_job_cancel(rj2) == RFC_OK);
            CHECK(rfc_job_wait(rj2, UINT32_MAX) == RFC_OK);
            rfc_job_release(rj2);
        }
        CHECK(rfc_job_wait(rj, UINT32_MAX) == RFC_OK);
        rfc_job_release(rj);
    }
    rfc_job_slot_destroy(rslot);

    rfc_scheduler_stats mid;
    rfc_scheduler_stats_get(sched, &mid);
    CHECK(mid.live_partial_bytes == 0u);
    CHECK(mid.published_cancelled_count == 0u);
    CHECK(mid.total_submitted == mid.total_completed + mid.total_failed +
                                   mid.total_cancelled + mid.total_superseded);
    rfc_scheduler_destroy(sched);

    /* D. Safe rfc_scheduler_destroy with active progress callbacks and queued jobs */
    rfc_scheduler_config scfg;
    rfc_scheduler_config_default(&scfg);
    scfg.max_workers = 2u;
    scfg.total_cpu_threads = 2u;
    scfg.memory_budget_bytes = 512u * 1024u;
    rfc_scheduler *dsched = rfc_scheduler_create(&scfg);
    CHECK(dsched != NULL);

    shutdown_ctx sctx;
    memset(&sctx, 0, sizeof(sctx));
    test_mutex_init(&sctx.lock);
    test_cond_init(&sctx.cond);

    rfc_job *sjobs[10] = {NULL};
    for (size_t i = 0; i < 10u; i++) {
        rfc_job_spec sspec;
        init_basic_spec(&sspec, NULL, 0, &lay);
        sspec.stage_ops.on_encode = shutdown_worker_stage;
        sspec.stage_ops.user_ctx = &sctx;
        sspec.progress_fn = shutdown_cb;
        sspec.progress_user = &sctx;
        CHECK(rfc_scheduler_submit(dsched, NULL, &sspec, &sjobs[i]) == RFC_OK);
    }

    test_mutex_lock(&sctx.lock);
    while (sctx.running < 2u) {
        test_cond_wait(&sctx.cond, &sctx.lock);
    }
    test_mutex_unlock(&sctx.lock);

    /* Destroy cancels active & queued jobs, drains callbacks, and joins workers. */
    rfc_scheduler_destroy(dsched);
    test_mutex_lock(&sctx.lock);
    sctx.destroyed = 1;
    uint32_t late_cbs = sctx.callbacks_after_destroy;
    test_mutex_unlock(&sctx.lock);

    CHECK(late_cbs == 0u);
    for (size_t i = 0; i < 10u; i++) {
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(sjobs[i], &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_CANCELLED);
        CHECK(snap.live_partial_bytes == 0u);
        rfc_job_release(sjobs[i]);
    }
    test_cond_destroy(&sctx.cond);
    test_mutex_destroy(&sctx.lock);

    printf("jobs: cancel_all_stages live_partial_bytes=%llu "
           "published_cancelled=%llu shutdown_late_callbacks=%u 64x_restart=ok\n",
           (unsigned long long)mid.live_partial_bytes,
           (unsigned long long)mid.published_cancelled_count, late_cbs);
}

int main(void) {
    test_config_defaults_and_reconfigure();
    test_hostile_limits_and_unknown_reservation();
    test_bounded_cpu_and_memory_concurrency();
    test_oom_hook_cap();
    test_same_format_sessions_and_missing_stages();
    test_progress_retry_and_stale_rejection();
    test_cancellation_64x_and_shutdown();

    if (g_failures > 0) {
        fprintf(stderr, "jobs: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("jobs: all checks passed\n");
    return 0;
}
