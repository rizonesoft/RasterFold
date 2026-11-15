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
#include <sched.h>
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

typedef struct test_thread_start_arg {
    void (*fn)(void *);
    void *arg;
} test_thread_start_arg;

#ifdef _WIN32
typedef struct test_thread_t {
    HANDLE handle;
} test_thread_t;

static DWORD WINAPI test_thread_trampoline(LPVOID raw) {
    test_thread_start_arg *sa = (test_thread_start_arg *)raw;
    void (*fn)(void *) = sa->fn;
    void *arg = sa->arg;
    free(sa);
    fn(arg);
    return 0;
}

static int test_thread_start(test_thread_t *t, void (*fn)(void *), void *arg) {
    test_thread_start_arg *sa =
        (test_thread_start_arg *)malloc(sizeof(*sa));
    if (!sa) {
        return 0;
    }
    sa->fn = fn;
    sa->arg = arg;
    t->handle = CreateThread(NULL, 0, test_thread_trampoline, sa, 0, NULL);
    if (!t->handle) {
        free(sa);
        return 0;
    }
    return 1;
}

static void test_thread_join(test_thread_t *t) {
    if (t->handle) {
        WaitForSingleObject(t->handle, INFINITE);
        CloseHandle(t->handle);
        t->handle = NULL;
    }
}
#else
typedef struct test_thread_t {
    pthread_t th;
} test_thread_t;

static void *test_thread_trampoline(void *raw) {
    test_thread_start_arg *sa = (test_thread_start_arg *)raw;
    void (*fn)(void *) = sa->fn;
    void *arg = sa->arg;
    free(sa);
    fn(arg);
    return NULL;
}

static int test_thread_start(test_thread_t *t, void (*fn)(void *), void *arg) {
    test_thread_start_arg *sa =
        (test_thread_start_arg *)malloc(sizeof(*sa));
    if (!sa) {
        return 0;
    }
    sa->fn = fn;
    sa->arg = arg;
    if (pthread_create(&t->th, NULL, test_thread_trampoline, sa) != 0) {
        free(sa);
        return 0;
    }
    return 1;
}

static void test_thread_join(test_thread_t *t) {
    pthread_join(t->th, NULL);
}
#endif

static void test_thread_yield(void) {
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

extern void rfc_job_internal_set_progress_hook(
    void (*fn)(rfc_job_id job_id, int phase, void *user),
    void *user);

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

    /* Layout validator parity: invalid sample depth, invalid alpha, normalized zero stride */
    rfc_layout bad_float = {RFC_SAMPLE_RGBA_FLOAT, RFC_ALPHA_STRAIGHT, 4u, 4u, 0u, 16u};
    CHECK(rfc_limits_check_image(&lim, &bad_float, 1u) ==
          RFC_ERR_UNSUPPORTED_SAMPLE);
    rfc_layout bad_alpha = {RFC_SAMPLE_RGB8, RFC_ALPHA_STRAIGHT, 4u, 4u, 0u, 8u};
    CHECK(rfc_limits_check_image(&lim, &bad_alpha, 1u) ==
          RFC_ERR_UNSUPPORTED_ALPHA);
    rfc_layout zero_stride = normal;
    zero_stride.stride_bytes = 0u;
    uint64_t norm_bytes = 0;
    CHECK(rfc_limits_compute_frame_bytes(&zero_stride, 1u, &norm_bytes) ==
          RFC_OK);
    CHECK(norm_bytes == 64ull * 64ull * 3ull);

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

    /* Budget reservation must cover job_memory_limit_bytes + retained input bytes */
    uint8_t small_src[16];
    memset(small_src, 0x5A, sizeof(small_src));
    init_basic_spec(&spec, small_src, sizeof(small_src), &normal);
    spec.expected_working_set_bytes = 4096u;
    spec.job_memory_limit_bytes = cfg.memory_budget_bytes;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
          RFC_ERR_LIMIT_EXCEEDED);
    CHECK(job == NULL);

    /* Compressed input limit enforced even when source_layout == NULL */
    uint8_t oversized_comp[32];
    memset(oversized_comp, 0x33, sizeof(oversized_comp));
    rfc_limits small_comp_lim = cfg.default_limits;
    small_comp_lim.max_compressed_bytes = 16u;
    init_basic_spec(&spec, oversized_comp, sizeof(oversized_comp), NULL);
    spec.expected_working_set_bytes = 4096u;
    spec.limits_override = &small_comp_lim;
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
    CHECK(stats.submission_alloc_count == 0u);

    rfc_budget *b = rfc_budget_create(1024u);
    CHECK(b != NULL);
    CHECK(rfc_budget_alloc(b, SIZE_MAX) == NULL);
    rfc_budget_stats bstats;
    rfc_budget_stats_get(b, &bstats);
    CHECK(bstats.rejected_count == 1u && bstats.current_allocated_bytes == 0u);
    rfc_budget_destroy(b);

    printf("jobs: hostile/unknown preflight refused total_submitted=%llu "
           "peak_alloc=0 submit_allocs=%llu\n",
           (unsigned long long)stats.total_submitted,
           (unsigned long long)stats.submission_alloc_count);

    /* Verify positive submit DOES advance submission_alloc_count. */
    init_basic_spec(&spec, NULL, 0, &normal);
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
    rfc_job_release(job);
    rfc_scheduler_stats_get(sched, &stats);
    CHECK(stats.submission_alloc_count > 0u);

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

static rfc_status mock_probe_session(void *session,
                                     const rfc_exec_context *exec,
                                     const uint8_t *bytes,
                                     size_t size,
                                     rfc_image_probe *out_probe) {
    (void)bytes;
    (void)size;
    mock_session *s = (mock_session *)session;
    CHECK(s != NULL && exec != NULL && s->job_id == exec->job_id);
    if (!out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = 4u;
    out_probe->first_frame.height = 4u;
    out_probe->first_frame.stride_bytes = 12u;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    return RFC_OK;
}

static rfc_status session_verify_stage(rfc_job *job,
                                       const rfc_exec_context *exec,
                                       void *user_ctx) {
    (void)exec;
    (void)user_ctx;
    size_t fc = 0;
    const rfc_metadata *dm = NULL;
    const rfc_frame *frames = rfc_job_decoded_frames(job, &fc, &dm);
    CHECK(frames != NULL && fc == 1u && dm != NULL);
    if (frames && fc == 1u) {
        CHECK(frames[0].data != NULL && frames[0].data_len == 48u);
        if (frames[0].data && frames[0].data_len == 48u) {
            CHECK(frames[0].data[0] == 0x42u && frames[0].data[47] == 0x42u);
        }
    }
    const rfc_encoded *enc = rfc_job_encoded_output(job);
    CHECK(enc != NULL && enc->data != NULL && enc->len == 32u);
    if (enc && enc->data && enc->len == 32u) {
        CHECK(enc->data[0] == 0x77u && enc->data[31] == 0x77u);
    }
    return RFC_OK;
}

static rfc_status session_export_stage(rfc_job *job,
                                       const rfc_exec_context *exec,
                                       void *user_ctx) {
    (void)user_ctx;
    size_t fc = 0;
    const rfc_frame *frames = rfc_job_decoded_frames(job, &fc, NULL);
    CHECK(frames != NULL && fc == 1u);
    const rfc_encoded *enc = rfc_job_encoded_output(job);
    CHECK(enc != NULL && enc->data != NULL && enc->len == 32u);
    return rfc_job_mark_published(job, exec->job_id);
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

    rfc_session_adapter missing_probe;
    memset(&missing_probe, 0, sizeof(missing_probe));
    missing_probe.abi_version = RFC_CODEC_ABI_VERSION;
    missing_probe.format = RFC_FORMAT_QOI;
    missing_probe.open_session = mock_open_session;
    missing_probe.close_session = mock_close_session;
    missing_probe.decode_session = mock_decode_session;
    missing_probe.encode_session = mock_encode_session;
    missing_probe.cancel_session = mock_cancel_session;
    CHECK(rfc_registry_register_session(&missing_probe) ==
          RFC_ERR_INVALID_ARGUMENT);

    rfc_session_adapter qoi_sess = missing_probe;
    qoi_sess.probe_session = mock_probe_session;
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
    spec.stage_ops.on_verify = session_verify_stage;
    spec.stage_ops.on_export = session_export_stage;

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
        strcmp(sset[0].id, "metadata") != 0 ||
        strcmp(sset[0].value, "preserve") != 0) {
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
    char k1[] = "metadata";
    char v1[] = "preserve";
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

/* 8. Regression tests covering Round 1 review findings (#1, #4, #5, #7, #8, #9, #10) */
typedef struct r1_block_ctx {
    test_mutex_t lock;
    test_cond_t cond;
    uint32_t entered;
    int release;
    int saw_progress;
    rfc_status report_st;
    uint8_t seen_bytes[16];
    size_t seen_len;
} r1_block_ctx;

static rfc_status r1_block_import_stage(rfc_job *job,
                                        const rfc_exec_context *exec,
                                        void *user_ctx) {
    r1_block_ctx *bc = (r1_block_ctx *)user_ctx;
    size_t n = 0;
    const uint8_t *b = rfc_job_source_bytes(job, &n);
    test_mutex_lock(&bc->lock);
    bc->entered++;
    if (b && n <= sizeof(bc->seen_bytes)) {
        memcpy(bc->seen_bytes, b, n);
        bc->seen_len = n;
    }
    test_cond_broadcast(&bc->cond);
    while (!bc->release && !rfc_exec_is_cancelled(exec)) {
        test_cond_wait(&bc->cond, &bc->lock);
    }
    test_mutex_unlock(&bc->lock);
    bc->report_st = rfc_job_report_progress(job, exec->job_id,
                                            RFC_STAGE_IMPORT, 1u, 2u, "r1");
    bc->saw_progress = 1;
    return RFC_OK;
}

static uint32_t g_r1_leg_probe_calls = 0;
static uint32_t g_r1_leg_encode_calls = 0;
static uint32_t g_r1_leg_decode_calls = 0;

static rfc_status r1_leg_probe(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx; (void)data; (void)len;
    g_r1_leg_probe_calls++;
    return RFC_OK;
}

static rfc_status r1_leg_encode(void *ctx, const rfc_frame *in,
                                const rfc_setting_value *settings,
                                size_t setting_count,
                                const rfc_allocator *allocator,
                                rfc_encoded *out) {
    (void)ctx; (void)in; (void)settings; (void)setting_count;
    (void)allocator; (void)out;
    g_r1_leg_encode_calls++;
    return RFC_OK;
}

static rfc_status r1_leg_decode(void *ctx, const rfc_encoded *in,
                                const rfc_setting_value *settings,
                                size_t setting_count,
                                const rfc_allocator *allocator,
                                rfc_frame *out) {
    (void)ctx; (void)in; (void)settings; (void)setting_count;
    (void)allocator; (void)out;
    g_r1_leg_decode_calls++;
    return RFC_OK;
}

static rfc_status r1_leg_cancel(void *ctx) {
    (void)ctx;
    return RFC_OK;
}

static void r1_leg_free(void *ctx) {
    (void)ctx;
}

typedef struct r1_cb_gate_ctx {
    test_mutex_t lock;
    test_cond_t cond;
    rfc_job_id target_job_id;
    int pause_phase;
    int entered_phase;
    int release_phase;
    uint32_t cb_calls;
    uint32_t cb_completed;
    int self_cancel_in_cb;
    rfc_job *self_job;
    rfc_status self_cancel_st;
} r1_cb_gate_ctx;

static void r1_progress_hook_cb(rfc_job_id job_id, int phase, void *user) {
    r1_cb_gate_ctx *gc = (r1_cb_gate_ctx *)user;
    test_mutex_lock(&gc->lock);
    if (gc->target_job_id != RFC_JOB_ID_NONE &&
        job_id == gc->target_job_id && phase == gc->pause_phase) {
        gc->entered_phase = 1;
        test_cond_broadcast(&gc->cond);
        while (!gc->release_phase) {
            test_cond_wait(&gc->cond, &gc->lock);
        }
    }
    test_mutex_unlock(&gc->lock);
}

static void r1_gated_progress_fn(const rfc_job_progress *p, void *user) {
    r1_cb_gate_ctx *gc = (r1_cb_gate_ctx *)user;
    test_mutex_lock(&gc->lock);
    if (gc->target_job_id == RFC_JOB_ID_NONE || p->job_id == gc->target_job_id) {
        gc->cb_calls++;
    }
    int do_self_cancel = gc->self_cancel_in_cb;
    rfc_job *sj = gc->self_job;
    test_mutex_unlock(&gc->lock);

    if (do_self_cancel && sj) {
        gc->self_cancel_st = rfc_job_cancel(sj);
    }

    test_mutex_lock(&gc->lock);
    gc->cb_completed++;
    test_cond_broadcast(&gc->cond);
    test_mutex_unlock(&gc->lock);
}

static rfc_status r1_report_stage(rfc_job *job,
                                  const rfc_exec_context *exec,
                                  void *user_ctx) {
    (void)user_ctx;
    (void)rfc_job_report_progress(job, exec->job_id, RFC_STAGE_IMPORT, 1u, 2u,
                                  "gate-check");
    return RFC_OK;
}

typedef struct r1_retry_thread_arg {
    rfc_scheduler *sched;
    rfc_job_slot *slot;
    rfc_job *out_job;
    rfc_status st;
    int done;
    test_mutex_t *lock;
} r1_retry_thread_arg;

static void r1_retry_worker(void *raw) {
    r1_retry_thread_arg *a = (r1_retry_thread_arg *)raw;
    a->st = rfc_scheduler_retry(a->sched, a->slot, &a->out_job);
    test_mutex_lock(a->lock);
    a->done = 1;
    test_mutex_unlock(a->lock);
}

typedef struct r1_probe_ctrl {
    test_mutex_t lock;
    test_cond_t cond;
    int mode; /* 0: hostile probe, 1: streaming frame-1 limit, 2: cancel, 3: multi-frame probe limit */
    uint32_t decode_calls;
    uint32_t encode_calls;
    int in_decode;
    int allow_decode_finish;
    int cancel_entered;
    int caller_cancel_returned;
} r1_probe_ctrl;

static r1_probe_ctrl g_r1_probe;

typedef struct r1_psess {
    rfc_job_id job_id;
    const rfc_allocator *alloc;
} r1_psess;

static rfc_status r1_p_open(const rfc_exec_context *exec, void **out_sess) {
    r1_psess *s =
        (r1_psess *)exec->allocator->alloc(exec->allocator->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    s->job_id = exec->job_id;
    s->alloc = exec->allocator;
    *out_sess = s;
    return RFC_OK;
}

static void r1_p_close(void *session) {
    r1_psess *s = (r1_psess *)session;
    if (s) {
        const rfc_allocator *a = s->alloc;
        a->free(a->ctx, s);
    }
}

static rfc_status r1_p_probe(void *session,
                             const rfc_exec_context *exec,
                             const uint8_t *bytes,
                             size_t size,
                             rfc_image_probe *out_probe) {
    (void)session; (void)exec; (void)bytes; (void)size;
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.bits_per_sample = 8u;
    if (g_r1_probe.mode == 0) {
        out_probe->first_frame.width = 16384u;
        out_probe->first_frame.height = 16384u;
        out_probe->first_frame.stride_bytes = 16384u * 3u;
        out_probe->frame_count = 1u;
    } else if (g_r1_probe.mode == 1) {
        out_probe->first_frame.width = 4u;
        out_probe->first_frame.height = 4u;
        out_probe->first_frame.stride_bytes = 12u;
        out_probe->frame_count = 2u;
    } else if (g_r1_probe.mode == 3) {
        out_probe->first_frame.width = 4u;
        out_probe->first_frame.height = 4u;
        out_probe->first_frame.stride_bytes = 12u;
        out_probe->frame_count = 2u;
        out_probe->total_pixels = 2000u;
        out_probe->total_decoded_bytes = 64u * 1024u;
    } else if (g_r1_probe.mode == 4) {
        out_probe->first_frame.width = 4u;
        out_probe->first_frame.height = 4u;
        out_probe->first_frame.stride_bytes = 12u;
        out_probe->frame_count = 2u;
        out_probe->total_pixels = 0u;
        out_probe->total_decoded_bytes = 0u;
    } else if (g_r1_probe.mode == 5 || g_r1_probe.mode == 7 ||
               g_r1_probe.mode == 8 || g_r1_probe.mode == 9 ||
               g_r1_probe.mode == 10) {
        out_probe->first_frame.width = 16u;
        out_probe->first_frame.height = 16u;
        out_probe->first_frame.stride_bytes = 64u;
        out_probe->first_frame.format = RFC_SAMPLE_RGBA8;
        out_probe->first_frame.alpha = RFC_ALPHA_STRAIGHT;
        out_probe->frame_count = 0u;
        out_probe->total_pixels = 0u;
        out_probe->total_decoded_bytes = 0u;
    } else if (g_r1_probe.mode == 6) {
        out_probe->first_frame.width = 16u;
        out_probe->first_frame.height = 16u;
        out_probe->first_frame.stride_bytes = 64u;
        out_probe->first_frame.format = RFC_SAMPLE_RGBA8;
        out_probe->first_frame.alpha = RFC_ALPHA_STRAIGHT;
        out_probe->frame_count = 2u;
        out_probe->total_pixels = 0u;
        out_probe->total_decoded_bytes = 0u;
    } else {
        out_probe->first_frame.width = 4u;
        out_probe->first_frame.height = 4u;
        out_probe->first_frame.stride_bytes = 12u;
        out_probe->frame_count = 1u;
    }
    return RFC_OK;
}

static rfc_status r1_p_decode(void *session,
                              const rfc_exec_context *exec,
                              const uint8_t *bytes,
                              size_t size,
                              const rfc_setting_value *settings,
                              size_t setting_count,
                              rfc_frame **out_frames,
                              size_t *out_frame_count,
                              rfc_metadata *out_meta) {
    (void)session; (void)bytes; (void)size; (void)settings;
    (void)setting_count;
    test_mutex_lock(&g_r1_probe.lock);
    g_r1_probe.decode_calls++;
    if (g_r1_probe.mode == 2) {
        g_r1_probe.in_decode = 1;
        test_cond_broadcast(&g_r1_probe.cond);
        while (!g_r1_probe.allow_decode_finish) {
            test_cond_wait(&g_r1_probe.cond, &g_r1_probe.lock);
        }
        test_mutex_unlock(&g_r1_probe.lock);
        return RFC_ERR_CANCELLED;
    }
    test_mutex_unlock(&g_r1_probe.lock);

    memset(out_meta, 0, sizeof(*out_meta));
    if (g_r1_probe.mode == 1) {
        rfc_frame *frames = (rfc_frame *)exec->allocator->alloc(
            exec->allocator->ctx, 2u * sizeof(*frames));
        if (!frames) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memset(frames, 0, 2u * sizeof(*frames));
        rfc_layout l0 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_layout l1 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 128u, 4u, 384u, 8u};
        rfc_status st = rfc_exec_alloc_frame(exec, &frames[0], &l0, NULL);
        if (st == RFC_OK) {
            st = rfc_exec_alloc_frame(exec, &frames[1], &l1, NULL);
        }
        if (st != RFC_OK) {
            rfc_frame_free(&frames[0]);
            rfc_frame_free(&frames[1]);
            exec->allocator->free(exec->allocator->ctx, frames);
            return st;
        }
        *out_frames = frames;
        *out_frame_count = 2u;
        return RFC_OK;
    }
    if (g_r1_probe.mode == 4) {
        /* Hostile adapter bypasses incremental check when probe omitted totals. */
        rfc_frame *frames = (rfc_frame *)exec->allocator->alloc(
            exec->allocator->ctx, 2u * sizeof(*frames));
        if (!frames) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memset(frames, 0, 2u * sizeof(*frames));
        rfc_layout l0 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_status st = rfc_frame_alloc(&frames[0], &l0, NULL, exec->allocator);
        if (st != RFC_OK) {
            exec->allocator->free(exec->allocator->ctx, frames);
            return st;
        }
        *out_frames = frames;
        *out_frame_count = 1u;
        return RFC_OK;
    }
    if (g_r1_probe.mode == 5) {
        /* Unknown frame count: frame 0 validated, frame 1 tries unvalidated rfc_frame_alloc. */
        rfc_frame *frames = (rfc_frame *)exec->allocator->alloc(
            exec->allocator->ctx, 2u * sizeof(*frames));
        if (!frames) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memset(frames, 0, 2u * sizeof(*frames));
        rfc_layout l0 = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 16u, 16u, 64u, 8u};
        rfc_layout l1 = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 16u, 16u, 64u, 8u};
        rfc_status st = rfc_exec_alloc_frame(exec, &frames[0], &l0, NULL);
        if (st != RFC_OK) {
            exec->allocator->free(exec->allocator->ctx, frames);
            return st;
        }
        st = rfc_frame_alloc(&frames[1], &l1, NULL, exec->allocator);
        if (st != RFC_OK) {
            rfc_frame_free(&frames[0]);
            exec->allocator->free(exec->allocator->ctx, frames);
            return st;
        }
        *out_frames = frames;
        *out_frame_count = 2u;
        return RFC_OK;
    }
    if (g_r1_probe.mode == 6) {
        /* Compliant scratch allocations + free/realloc + incremental frame checks. */
        uint8_t *scratch =
            (uint8_t *)exec->allocator->alloc(exec->allocator->ctx, 4096u);
        uint8_t *zscratch =
            (uint8_t *)rfc_budget_zlib_alloc(rfc_exec_budget(exec), 1u, 4096u);
        if (!scratch || !zscratch) {
            rfc_budget_zlib_free(rfc_exec_budget(exec), zscratch);
            exec->allocator->free(exec->allocator->ctx, scratch);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memset(scratch, 0x3C, 4096u);
        memset(zscratch, 0x4D, 4096u);
        for (int pass = 0; pass < 6; pass++) {
            exec->allocator->free(exec->allocator->ctx, scratch);
            scratch =
                (uint8_t *)exec->allocator->alloc(exec->allocator->ctx, 4096u);
            if (!scratch) {
                rfc_budget_zlib_free(rfc_exec_budget(exec), zscratch);
                return RFC_ERR_OUT_OF_MEMORY;
            }
            scratch[0] = (uint8_t)pass;
        }

        rfc_frame *frames = (rfc_frame *)exec->allocator->alloc(
            exec->allocator->ctx, 2u * sizeof(*frames));
        if (!frames) {
            rfc_budget_zlib_free(rfc_exec_budget(exec), zscratch);
            exec->allocator->free(exec->allocator->ctx, scratch);
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memset(frames, 0, 2u * sizeof(*frames));
        rfc_layout flay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 16u, 16u, 64u, 8u};
        rfc_status st = rfc_exec_check_next_frame(exec, &flay, NULL);
        if (st == RFC_OK) {
            st = rfc_frame_alloc(&frames[0], &flay, NULL, exec->allocator);
        }
        if (st == RFC_OK) {
            st = rfc_exec_alloc_frame(exec, &frames[1], &flay, NULL);
        }
        rfc_budget_zlib_free(rfc_exec_budget(exec), zscratch);
        exec->allocator->free(exec->allocator->ctx, scratch);
        if (st != RFC_OK) {
            rfc_frame_free(&frames[0]);
            rfc_frame_free(&frames[1]);
            exec->allocator->free(exec->allocator->ctx, frames);
            return st;
        }
        memset(frames[0].data, 0x10, frames[0].data_len);
        memset(frames[1].data, 0x11, frames[1].data_len);
        *out_frames = frames;
        *out_frame_count = 2u;
        return RFC_OK;
    }
    if (g_r1_probe.mode == 7) {
        /* Over-budget scratch allocation under unknown totals still enforces job budget. */
        uint8_t *big_scratch =
            (uint8_t *)exec->allocator->alloc(exec->allocator->ctx, 32768u);
        if (big_scratch) {
            exec->allocator->free(exec->allocator->ctx, big_scratch);
            return RFC_OK;
        }
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (g_r1_probe.mode >= 8 && g_r1_probe.mode <= 10) {
        /* Validate a 1024-byte 16x16 layout, then attempt a 1024-byte layout swap. */
        rfc_frame *frames = (rfc_frame *)exec->allocator->alloc(
            exec->allocator->ctx, sizeof(*frames));
        if (!frames) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memset(frames, 0, sizeof(*frames));
        rfc_layout checked = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 16u, 16u, 64u, 8u};
        rfc_layout wide = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 256u, 1u, 1024u, 8u};
        rfc_layout swapped = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 32u, 8u, 128u, 8u};
        rfc_status st = rfc_exec_check_next_frame(exec, &checked, NULL);
        if (st == RFC_OK) {
            if (g_r1_probe.mode == 8) {
                st = rfc_exec_alloc_frame(exec, &frames[0], &wide, NULL);
            } else if (g_r1_probe.mode == 9) {
                st = rfc_frame_alloc(&frames[0], &wide, NULL, exec->allocator);
            } else {
                st = rfc_frame_alloc(&frames[0], &swapped, NULL, exec->allocator);
            }
        }
        if (st != RFC_OK) {
            rfc_frame_free(&frames[0]);
            exec->allocator->free(exec->allocator->ctx, frames);
            return st;
        }
        *out_frames = frames;
        *out_frame_count = 1u;
        return RFC_OK;
    }
    return RFC_ERR_INVALID_ARGUMENT;
}

static rfc_status r1_p_encode(void *session,
                              const rfc_exec_context *exec,
                              const rfc_frame *frames,
                              size_t frame_count,
                              const rfc_metadata *meta,
                              const rfc_setting_value *settings,
                              size_t setting_count,
                              rfc_encoded *out_encoded) {
    (void)session; (void)meta; (void)settings; (void)setting_count;
    test_mutex_lock(&g_r1_probe.lock);
    g_r1_probe.encode_calls++;
    test_mutex_unlock(&g_r1_probe.lock);
    if (g_r1_probe.mode == 6 && frames != NULL && frame_count == 2u) {
        uint8_t *buf = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx, 16u);
        if (!buf) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
        memset(buf, 0x55, 16u);
        out_encoded->data = buf;
        out_encoded->len = 16u;
        out_encoded->allocator = exec->allocator;
        return RFC_OK;
    }
    return RFC_ERR_INVALID_ARGUMENT;
}

static void r1_p_cancel(void *session) {
    (void)session;
    test_mutex_lock(&g_r1_probe.lock);
    g_r1_probe.cancel_entered = 1;
    test_cond_broadcast(&g_r1_probe.cond);
    while (!g_r1_probe.caller_cancel_returned) {
        test_cond_wait(&g_r1_probe.cond, &g_r1_probe.lock);
    }
    test_mutex_unlock(&g_r1_probe.lock);
}

static void test_round1_regressions(void) {
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};

    /* R1 (#1): Destroying a slot while job is active must not cause UAF. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.total_cpu_threads = 1u;
        cfg.memory_budget_bytes = 512u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        r1_block_ctx bc;
        memset(&bc, 0, sizeof(bc));
        test_mutex_init(&bc.lock);
        test_cond_init(&bc.cond);

        rfc_job_slot *slot = rfc_job_slot_create();
        CHECK(slot != NULL);

        rfc_job_spec spec;
        init_basic_spec(&spec, NULL, 0, &lay);
        spec.stage_ops.on_import = r1_block_import_stage;
        spec.stage_ops.user_ctx = &bc;

        rfc_job *j = NULL;
        CHECK(rfc_scheduler_submit(sched, slot, &spec, &j) == RFC_OK);

        test_mutex_lock(&bc.lock);
        while (bc.entered == 0u) {
            test_cond_wait(&bc.cond, &bc.lock);
        }
        test_mutex_unlock(&bc.lock);

        rfc_job_slot_destroy(slot);

        test_mutex_lock(&bc.lock);
        bc.release = 1;
        test_cond_broadcast(&bc.cond);
        test_mutex_unlock(&bc.lock);

        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED);
        CHECK(bc.saw_progress == 1 && bc.report_st == RFC_OK);

        rfc_job_release(j);
        test_cond_destroy(&bc.cond);
        test_mutex_destroy(&bc.lock);
        rfc_scheduler_destroy(sched);
    }

    /* R2 (#5): Legacy rfc_adapter-only registration is refused cleanly in bounded jobs. */
    {
        g_r1_leg_probe_calls = 0;
        g_r1_leg_encode_calls = 0;
        g_r1_leg_decode_calls = 0;
        rfc_adapter leg = {RFC_CODEC_ABI_VERSION, RFC_FORMAT_QOI, r1_leg_probe,
                           r1_leg_encode, r1_leg_decode, r1_leg_cancel,
                           r1_leg_free, NULL};
        CHECK(rfc_registry_register(&leg) == RFC_OK);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.memory_budget_bytes = 512u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        uint8_t dummy[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_QOI;
        spec.target_format = RFC_FORMAT_QOI;
        spec.source_bytes = dummy;
        spec.source_size = sizeof(dummy);
        spec.source_layout = &lay;
        spec.expected_working_set_bytes = 8192u;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_verify = ok_noop_stage;
        spec.stage_ops.on_export = export_mark_ok;

        rfc_job *j = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(j, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED);
        CHECK(snap.status == RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(g_r1_leg_probe_calls == 0u && g_r1_leg_decode_calls == 0u &&
              g_r1_leg_encode_calls == 0u);

        rfc_job_release(j);
        rfc_scheduler_destroy(sched);
        CHECK(rfc_registry_unregister(RFC_FORMAT_QOI) == RFC_OK);
    }

    /* R3 (#4): Atomic callback gate across cancel, retry, and self-cancel. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.total_cpu_threads = 1u;
        cfg.memory_budget_bytes = 512u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        r1_cb_gate_ctx gc;
        memset(&gc, 0, sizeof(gc));
        test_mutex_init(&gc.lock);
        test_cond_init(&gc.cond);
        rfc_job_internal_set_progress_hook(r1_progress_hook_cb, &gc);

        /* Subcase A: pause before cb_lock (phase 0), cancel, resume -> 0 callbacks. */
        gc.pause_phase = 0;
        gc.target_job_id = 1u;
        rfc_job_spec spec;
        init_basic_spec(&spec, NULL, 0, &lay);
        spec.stage_ops.on_import = r1_report_stage;
        spec.progress_fn = r1_gated_progress_fn;
        spec.progress_user = &gc;

        rfc_job *j1 = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j1) == RFC_OK);
        test_mutex_lock(&gc.lock);
        while (!gc.entered_phase) {
            test_cond_wait(&gc.cond, &gc.lock);
        }
        test_mutex_unlock(&gc.lock);

        CHECK(rfc_job_cancel(j1) == RFC_OK);
        test_mutex_lock(&gc.lock);
        gc.release_phase = 1;
        test_cond_broadcast(&gc.cond);
        test_mutex_unlock(&gc.lock);

        CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);
        CHECK(gc.cb_calls == 0u);
        rfc_job_release(j1);

        /* Subcase B: pause inside cb_lock (phase 1) on slot job; retry blocks until cb finishes. */
        test_mutex_lock(&gc.lock);
        gc.entered_phase = 0;
        gc.release_phase = 0;
        gc.cb_calls = 0;
        gc.cb_completed = 0;
        gc.pause_phase = 1;
        gc.target_job_id = RFC_JOB_ID_NONE;
        test_mutex_unlock(&gc.lock);

        rfc_job_slot *slot = rfc_job_slot_create();
        CHECK(slot != NULL);
        CHECK(rfc_scheduler_submit(sched, slot, &spec, &j1) == RFC_OK);
        rfc_job_id j1_id = rfc_job_id_get(j1);
        test_mutex_lock(&gc.lock);
        gc.target_job_id = j1_id;
        while (!gc.entered_phase) {
            test_cond_wait(&gc.cond, &gc.lock);
        }
        test_mutex_unlock(&gc.lock);

        r1_retry_thread_arg rarg;
        memset(&rarg, 0, sizeof(rarg));
        rarg.sched = sched;
        rarg.slot = slot;
        rarg.lock = &gc.lock;
        test_thread_t rth;
        CHECK(test_thread_start(&rth, r1_retry_worker, &rarg));

        /* Once j1.superseded_by_id is non-zero, retry has committed and is waiting on cb_lock. */
        for (;;) {
            rfc_job_snapshot sn;
            CHECK(rfc_job_snapshot_get(j1, &sn) == RFC_OK);
            if (sn.superseded_by_id != RFC_JOB_ID_NONE) {
                break;
            }
            test_thread_yield();
        }
        test_mutex_lock(&gc.lock);
        CHECK(gc.cb_completed == 0u);
        CHECK(rarg.done == 0);
        gc.release_phase = 1;
        test_cond_broadcast(&gc.cond);
        test_mutex_unlock(&gc.lock);

        test_thread_join(&rth);
        CHECK(rarg.st == RFC_OK && rarg.out_job != NULL);
        test_mutex_lock(&gc.lock);
        CHECK(gc.cb_completed >= 1u);
        test_mutex_unlock(&gc.lock);

        CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_wait(rarg.out_job, UINT32_MAX) == RFC_OK);
        rfc_job_release(j1);
        rfc_job_release(rarg.out_job);
        rfc_job_slot_destroy(slot);

        /* Subcase C: self-cancel directly inside progress_fn must not deadlock. */
        test_mutex_lock(&gc.lock);
        gc.target_job_id = RFC_JOB_ID_NONE;
        gc.pause_phase = -1;
        gc.self_cancel_in_cb = 1;
        gc.self_cancel_st = RFC_ERR_INVALID_ARGUMENT;
        gc.self_job = NULL;
        test_mutex_unlock(&gc.lock);
        rfc_job_internal_set_progress_hook(NULL, NULL);

        rfc_job *sj = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &sj) == RFC_OK);
        test_mutex_lock(&gc.lock);
        gc.self_job = sj;
        test_mutex_unlock(&gc.lock);
        CHECK(rfc_job_wait(sj, UINT32_MAX) == RFC_OK);
        CHECK(gc.self_cancel_st == RFC_OK);
        rfc_job_release(sj);

        test_cond_destroy(&gc.cond);
        test_mutex_destroy(&gc.lock);
        rfc_scheduler_destroy(sched);
    }

    /* R4 (#7, #8): Incompatible reconfigure refuses with RFC_ERR_BUSY; busy submit preserves slot. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.total_cpu_threads = 1u;
        cfg.max_queue_depth = 1u;
        cfg.memory_budget_bytes = 256u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        rfc_job_slot *slot = rfc_job_slot_create();
        CHECK(slot != NULL);

        r1_block_ctx slot_bc;
        memset(&slot_bc, 0, sizeof(slot_bc));
        test_mutex_init(&slot_bc.lock);
        test_cond_init(&slot_bc.cond);
        slot_bc.release = 1;

        const uint8_t data_a[4] = {'A', 'A', 'A', 'A'};
        const uint8_t data_b[4] = {'B', 'B', 'B', 'B'};
        rfc_job_spec spec_a;
        init_basic_spec(&spec_a, data_a, sizeof(data_a), &lay);
        spec_a.expected_working_set_bytes = 64u * 1024u;
        spec_a.stage_ops.on_import = r1_block_import_stage;
        spec_a.stage_ops.user_ctx = &slot_bc;

        rfc_job *ja0 = NULL;
        CHECK(rfc_scheduler_submit(sched, slot, &spec_a, &ja0) == RFC_OK);
        CHECK(rfc_job_wait(ja0, UINT32_MAX) == RFC_OK);
        rfc_job_release(ja0);

        r1_block_ctx hold_bc;
        memset(&hold_bc, 0, sizeof(hold_bc));
        test_mutex_init(&hold_bc.lock);
        test_cond_init(&hold_bc.cond);

        rfc_job_spec hold_spec;
        init_basic_spec(&hold_spec, NULL, 0, &lay);
        hold_spec.expected_working_set_bytes = 160u * 1024u;
        hold_spec.stage_ops.on_import = r1_block_import_stage;
        hold_spec.stage_ops.user_ctx = &hold_bc;

        rfc_job *j_act = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &hold_spec, &j_act) == RFC_OK);
        test_mutex_lock(&hold_bc.lock);
        while (hold_bc.entered == 0u) {
            test_cond_wait(&hold_bc.cond, &hold_bc.lock);
        }
        test_mutex_unlock(&hold_bc.lock);

        rfc_job_spec q_spec;
        init_basic_spec(&q_spec, NULL, 0, &lay);
        q_spec.expected_working_set_bytes = 128u * 1024u;
        rfc_job *j_q = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &q_spec, &j_q) == RFC_OK);

        rfc_scheduler_config lower_budget = cfg;
        lower_budget.memory_budget_bytes = 96u * 1024u;
        CHECK(rfc_scheduler_reconfigure(sched, &lower_budget) == RFC_ERR_BUSY);

        rfc_scheduler_config tighter_lim = cfg;
        tighter_lim.default_limits.max_width = 32u;
        CHECK(rfc_scheduler_reconfigure(sched, &tighter_lim) == RFC_ERR_BUSY);

        rfc_job_spec spec_b;
        init_basic_spec(&spec_b, data_b, sizeof(data_b), &lay);
        rfc_job *j_busy = NULL;
        CHECK(rfc_scheduler_submit(sched, slot, &spec_b, &j_busy) == RFC_ERR_BUSY);
        CHECK(j_busy == NULL);

        test_mutex_lock(&hold_bc.lock);
        hold_bc.release = 1;
        test_cond_broadcast(&hold_bc.cond);
        test_mutex_unlock(&hold_bc.lock);
        CHECK(rfc_job_wait(j_act, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_wait(j_q, UINT32_MAX) == RFC_OK);
        rfc_job_release(j_act);
        rfc_job_release(j_q);

        rfc_job *j_retry = NULL;
        CHECK(rfc_scheduler_retry(sched, slot, &j_retry) == RFC_OK);
        CHECK(rfc_job_wait(j_retry, UINT32_MAX) == RFC_OK);
        CHECK(slot_bc.seen_len == 4u &&
              memcmp(slot_bc.seen_bytes, data_a, 4u) == 0);
        rfc_job_release(j_retry);

        rfc_job_slot_destroy(slot);
        test_cond_destroy(&hold_bc.cond);
        test_mutex_destroy(&hold_bc.lock);
        test_cond_destroy(&slot_bc.cond);
        test_mutex_destroy(&slot_bc.lock);
        rfc_scheduler_destroy(sched);
    }

    /* R5 & R6 (#9, #10): Probe pre-check before decode, frame-1 limit check, non-blocking cancel. */
    {
        memset(&g_r1_probe, 0, sizeof(g_r1_probe));
        test_mutex_init(&g_r1_probe.lock);
        test_cond_init(&g_r1_probe.cond);

        rfc_session_adapter psess;
        memset(&psess, 0, sizeof(psess));
        psess.abi_version = RFC_CODEC_ABI_VERSION;
        psess.format = RFC_FORMAT_QOI;
        psess.open_session = r1_p_open;
        psess.close_session = r1_p_close;
        psess.probe_session = r1_p_probe;
        psess.decode_session = r1_p_decode;
        psess.encode_session = r1_p_encode;
        psess.cancel_session = r1_p_cancel;
        CHECK(rfc_registry_register_session(&psess) == RFC_OK);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.memory_budget_bytes = 512u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        rfc_limits lim = cfg.default_limits;
        lim.max_width = 64u;

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_QOI;
        spec.target_format = RFC_FORMAT_QOI;
        spec.source_bytes = src;
        spec.source_size = sizeof(src);
        spec.source_layout = &lay;
        spec.expected_working_set_bytes = 32768u;
        spec.limits_override = &lim;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_verify = ok_noop_stage;
        spec.stage_ops.on_export = export_mark_ok;

        /* Mode 0: hostile probe dimensions fail BEFORE decode_session is ever called. */
        g_r1_probe.mode = 0;
        rfc_job *j0 = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j0) == RFC_OK);
        CHECK(rfc_job_wait(j0, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot sn;
        CHECK(rfc_job_snapshot_get(j0, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(g_r1_probe.decode_calls == 0u);
        rfc_job_release(j0);

        /* Mode 1: multi-frame decode where frame 1 exceeds limits fails before frame 1 allocation or encode. */
        g_r1_probe.mode = 1;
        rfc_job *j1 = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j1) == RFC_OK);
        CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j1, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(sn.budget.peak_allocated_bytes > 0u &&
              sn.budget.peak_allocated_bytes < 512u);
        CHECK(g_r1_probe.encode_calls == 0u);
        rfc_job_release(j1);

        /* Mode 2: blocking cancel_session must not block caller of rfc_job_cancel. */
        g_r1_probe.mode = 2;
        rfc_job *j2 = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j2) == RFC_OK);
        test_mutex_lock(&g_r1_probe.lock);
        while (!g_r1_probe.in_decode) {
            test_cond_wait(&g_r1_probe.cond, &g_r1_probe.lock);
        }
        test_mutex_unlock(&g_r1_probe.lock);

        CHECK(rfc_job_cancel(j2) == RFC_OK);
        test_mutex_lock(&g_r1_probe.lock);
        g_r1_probe.caller_cancel_returned = 1;
        g_r1_probe.allow_decode_finish = 1;
        test_cond_broadcast(&g_r1_probe.cond);
        test_mutex_unlock(&g_r1_probe.lock);

        CHECK(rfc_job_wait(j2, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j2, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_CANCELLED);
        CHECK(g_r1_probe.cancel_entered == 1);
        rfc_job_release(j2);

        rfc_scheduler_destroy(sched);
        CHECK(rfc_registry_unregister_session(RFC_FORMAT_QOI) == RFC_OK);
        test_cond_destroy(&g_r1_probe.cond);
        test_mutex_destroy(&g_r1_probe.lock);
    }

    printf("jobs: round 1 regressions ok (slot_uaf=0 legacy_refused=%s "
           "probe_precheck=%s async_cancel=ok cb_gate=ok)\n",
           rfc_status_name(RFC_ERR_UNSUPPORTED_COMBINATION),
           rfc_status_name(RFC_ERR_LIMIT_EXCEEDED));
}

typedef struct gap_sess_ctrl {
    size_t decode_setting_count;
    int decode_saw_frames;
    int decode_saw_metadata;
    size_t encode_setting_count;
    int encode_saw_frames;
    int encode_saw_metadata;
} gap_sess_ctrl;

static gap_sess_ctrl g_gap_sess;

static rfc_status gap_s_open(const rfc_exec_context *exec, void **out_sess) {
    r1_psess *s =
        (r1_psess *)exec->allocator->alloc(exec->allocator->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    s->job_id = exec->job_id;
    s->alloc = exec->allocator;
    *out_sess = s;
    return RFC_OK;
}

static void gap_s_close(void *session) {
    r1_psess *s = (r1_psess *)session;
    if (s) {
        const rfc_allocator *a = s->alloc;
        a->free(a->ctx, s);
    }
}

static rfc_status gap_s_probe(void *session,
                              const rfc_exec_context *exec,
                              const uint8_t *bytes,
                              size_t size,
                              rfc_image_probe *out_probe) {
    (void)session;
    (void)exec;
    (void)bytes;
    (void)size;
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = 4u;
    out_probe->first_frame.height = 4u;
    out_probe->first_frame.stride_bytes = 12u;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    return RFC_OK;
}

static rfc_status gap_s_decode(void *session,
                               const rfc_exec_context *exec,
                               const uint8_t *bytes,
                               size_t size,
                               const rfc_setting_value *settings,
                               size_t setting_count,
                               rfc_frame **out_frames,
                               size_t *out_frame_count,
                               rfc_metadata *out_meta) {
    (void)session;
    (void)bytes;
    (void)size;
    g_gap_sess.decode_setting_count = setting_count;
    for (size_t i = 0; i < setting_count; i++) {
        if (settings[i].id && strcmp(settings[i].id, "frames") == 0) {
            g_gap_sess.decode_saw_frames = 1;
        }
        if (settings[i].id && strcmp(settings[i].id, "metadata") == 0) {
            g_gap_sess.decode_saw_metadata = 1;
        }
    }
    rfc_frame *frames = (rfc_frame *)exec->allocator->alloc(
        exec->allocator->ctx, sizeof(*frames));
    if (!frames) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(frames, 0, sizeof(*frames));
    rfc_layout l0 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
    rfc_status st = rfc_exec_alloc_frame(exec, &frames[0], &l0, NULL);
    if (st != RFC_OK) {
        exec->allocator->free(exec->allocator->ctx, frames);
        return st;
    }
    memset(out_meta, 0, sizeof(*out_meta));
    *out_frames = frames;
    *out_frame_count = 1u;
    return RFC_OK;
}

static rfc_status gap_s_encode(void *session,
                               const rfc_exec_context *exec,
                               const rfc_frame *frames,
                               size_t frame_count,
                               const rfc_metadata *meta,
                               const rfc_setting_value *settings,
                               size_t setting_count,
                               rfc_encoded *out_encoded) {
    (void)session;
    (void)frames;
    (void)frame_count;
    (void)meta;
    g_gap_sess.encode_setting_count = setting_count;
    for (size_t i = 0; i < setting_count; i++) {
        if (settings[i].id && strcmp(settings[i].id, "frames") == 0) {
            g_gap_sess.encode_saw_frames = 1;
        }
        if (settings[i].id && strcmp(settings[i].id, "metadata") == 0) {
            g_gap_sess.encode_saw_metadata = 1;
        }
    }
    uint8_t *buf = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx, 16u);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(buf, 0x33, 16u);
    out_encoded->data = buf;
    out_encoded->len = 16u;
    out_encoded->allocator = exec->allocator;
    return RFC_OK;
}

typedef struct gap_sat_barrier {
    test_mutex_t lock;
    test_cond_t cond;
    uint32_t ready;
    int go;
} gap_sat_barrier;

typedef struct gap_sat_arg {
    rfc_scheduler *sched;
    gap_sat_barrier *bar;
    const uint8_t *src;
    size_t src_len;
    const uint8_t *icc;
    size_t icc_len;
    rfc_status st;
    rfc_job *job;
} gap_sat_arg;

static void gap_sat_worker(void *raw) {
    gap_sat_arg *a = (gap_sat_arg *)raw;
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
    rfc_metadata meta = {a->icc, a->icc_len, RFC_ORIENTATION_NORMAL, 0u};
    rfc_setting_value s = {"metadata", "preserve"};

    rfc_job_spec spec;
    init_basic_spec(&spec, a->src, a->src_len, &lay);
    spec.source_meta = &meta;
    spec.settings = &s;
    spec.setting_count = 1u;
    spec.expected_working_set_bytes = 64u * 1024u;

    test_mutex_lock(&a->bar->lock);
    a->bar->ready++;
    test_cond_broadcast(&a->bar->cond);
    while (!a->bar->go) {
        test_cond_wait(&a->bar->cond, &a->bar->lock);
    }
    test_mutex_unlock(&a->bar->lock);

    a->st = rfc_scheduler_submit(a->sched, NULL, &spec, &a->job);
}

static void test_completion_gap_regressions(void) {
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};

    /* Gap 1: Enforce registry settings + format read/write capability in preflight (0 allocs) and stage filtering. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.memory_budget_bytes = 512u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        rfc_scheduler_stats st_before;
        rfc_scheduler_stats st_after;
        rfc_scheduler_stats_get(sched, &st_before);

        rfc_job_spec spec;
        init_basic_spec(&spec, src, sizeof(src), &lay);
        rfc_job *j = NULL;

        rfc_setting_value unknown_s[1] = {{"quality", "95"}};
        spec.settings = unknown_s;
        spec.setting_count = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j) ==
              RFC_ERR_UNKNOWN_SETTING);
        CHECK(j == NULL);

        rfc_setting_value bad_val_s[1] = {{"metadata", "bogus"}};
        spec.settings = bad_val_s;
        spec.setting_count = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j) ==
              RFC_ERR_SETTING_VALUE);

        rfc_setting_value combo_s[2] = {{"metadata", "strip"},
                                        {"exactness", "byte-identical"}};
        spec.settings = combo_s;
        spec.setting_count = 2u;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);

        rfc_setting_value frames_s[1] = {{"frames", "all"}};
        spec.settings = frames_s;
        spec.setting_count = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &j) ==
              RFC_ERR_UNKNOWN_SETTING);

        /* Default encode refuses import-only target formats upfront in preflight. */
        rfc_job_spec ro_target;
        memset(&ro_target, 0, sizeof(ro_target));
        ro_target.source_format = RFC_FORMAT_QOI;
        ro_target.target_format = RFC_FORMAT_JPEG;
        ro_target.source_bytes = src;
        ro_target.source_size = sizeof(src);
        ro_target.source_layout = &lay;
        ro_target.expected_working_set_bytes = 8192u;
        CHECK(rfc_scheduler_submit(sched, NULL, &ro_target, &j) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);

        rfc_scheduler_stats_get(sched, &st_after);
        CHECK(st_after.submission_alloc_count == st_before.submission_alloc_count);
        CHECK(st_after.total_submitted == 0u);

        /* Cross-format conversion (APNG -> QOI) filters stage settings cleanly per format. */
        memset(&g_gap_sess, 0, sizeof(g_gap_sess));
        rfc_session_adapter apng_s;
        memset(&apng_s, 0, sizeof(apng_s));
        apng_s.abi_version = RFC_CODEC_ABI_VERSION;
        apng_s.format = RFC_FORMAT_APNG;
        apng_s.open_session = gap_s_open;
        apng_s.close_session = gap_s_close;
        apng_s.probe_session = gap_s_probe;
        apng_s.decode_session = gap_s_decode;
        apng_s.encode_session = gap_s_encode;
        rfc_session_adapter qoi_s = apng_s;
        qoi_s.format = RFC_FORMAT_QOI;
        CHECK(rfc_registry_register_session(&apng_s) == RFC_OK);
        CHECK(rfc_registry_register_session(&qoi_s) == RFC_OK);

        rfc_setting_value cross_s[2] = {{"frames", "all"},
                                        {"metadata", "preserve"}};
        rfc_job_spec xspec;
        memset(&xspec, 0, sizeof(xspec));
        xspec.source_format = RFC_FORMAT_APNG;
        xspec.target_format = RFC_FORMAT_QOI;
        xspec.source_bytes = src;
        xspec.source_size = sizeof(src);
        xspec.source_layout = &lay;
        xspec.settings = cross_s;
        xspec.setting_count = 2u;
        xspec.expected_working_set_bytes = 32u * 1024u;
        xspec.has_stage_ops = 1;
        xspec.stage_ops.on_verify = ok_noop_stage;
        xspec.stage_ops.on_export = export_mark_ok;
        CHECK(rfc_scheduler_submit(sched, NULL, &xspec, &j) == RFC_OK);
        CHECK(rfc_job_wait(j, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot xsn;
        CHECK(rfc_job_snapshot_get(j, &xsn) == RFC_OK);
        CHECK(xsn.state == RFC_JOB_SUCCEEDED);
        CHECK(g_gap_sess.decode_setting_count == 2u &&
              g_gap_sess.decode_saw_frames == 1 &&
              g_gap_sess.decode_saw_metadata == 1);
        CHECK(g_gap_sess.encode_setting_count == 1u &&
              g_gap_sess.encode_saw_frames == 0 &&
              g_gap_sess.encode_saw_metadata == 1);
        rfc_job_release(j);

        rfc_scheduler_destroy(sched);
        CHECK(rfc_registry_unregister_session(RFC_FORMAT_APNG) == RFC_OK);
        CHECK(rfc_registry_unregister_session(RFC_FORMAT_QOI) == RFC_OK);
    }

    /* Gap 2: Completed/cancelled slot retained buffers remain charged against memory budget until slot destruction. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.memory_budget_bytes = 64u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        size_t big_sz = 48u * 1024u;
        size_t med_sz = 24u * 1024u;
        uint8_t *big_buf = (uint8_t *)calloc(1u, big_sz);
        uint8_t *med_buf = (uint8_t *)calloc(1u, med_sz);
        CHECK(big_buf != NULL && med_buf != NULL);

        /* Subcase A: Completed slot retains 48 KB after job release. */
        rfc_job_slot *slot1 = rfc_job_slot_create();
        CHECK(slot1 != NULL);
        rfc_job_spec sp1;
        init_basic_spec(&sp1, big_buf, big_sz, &lay);
        sp1.expected_working_set_bytes = 8192u;
        rfc_job *j1 = NULL;
        CHECK(rfc_scheduler_submit(sched, slot1, &sp1, &j1) == RFC_OK);
        CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);
        rfc_job_release(j1);

        rfc_scheduler_config lower = cfg;
        lower.memory_budget_bytes = 32u * 1024u;
        CHECK(rfc_scheduler_reconfigure(sched, &lower) == RFC_ERR_BUSY);

        rfc_job_spec sp_med;
        init_basic_spec(&sp_med, med_buf, med_sz, &lay);
        sp_med.expected_working_set_bytes = 4096u;
        rfc_job *j_busy = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp_med, &j_busy) == RFC_ERR_BUSY);
        CHECK(j_busy == NULL);

        rfc_job_slot_destroy(slot1);
        rfc_job *j_fit = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp_med, &j_fit) == RFC_OK);
        CHECK(rfc_job_wait(j_fit, UINT32_MAX) == RFC_OK);
        rfc_job_release(j_fit);

        /* Subcase B: Cancelled slot retains 48 KB until slot destruction. */
        r1_block_ctx bc;
        memset(&bc, 0, sizeof(bc));
        test_mutex_init(&bc.lock);
        test_cond_init(&bc.cond);

        rfc_job_slot *slot2 = rfc_job_slot_create();
        CHECK(slot2 != NULL);
        rfc_job_spec sp2;
        init_basic_spec(&sp2, big_buf, big_sz, &lay);
        sp2.expected_working_set_bytes = 8192u;
        sp2.stage_ops.on_import = r1_block_import_stage;
        sp2.stage_ops.user_ctx = &bc;

        rfc_job *j2 = NULL;
        CHECK(rfc_scheduler_submit(sched, slot2, &sp2, &j2) == RFC_OK);
        test_mutex_lock(&bc.lock);
        while (bc.entered == 0u) {
            test_cond_wait(&bc.cond, &bc.lock);
        }
        test_mutex_unlock(&bc.lock);

        CHECK(rfc_job_cancel(j2) == RFC_OK);
        test_mutex_lock(&bc.lock);
        bc.release = 1;
        test_cond_broadcast(&bc.cond);
        test_mutex_unlock(&bc.lock);
        CHECK(rfc_job_wait(j2, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot sn2;
        CHECK(rfc_job_snapshot_get(j2, &sn2) == RFC_OK);
        CHECK(sn2.state == RFC_JOB_CANCELLED);
        rfc_job_release(j2);

        CHECK(rfc_scheduler_reconfigure(sched, &lower) == RFC_ERR_BUSY);
        CHECK(rfc_scheduler_submit(sched, NULL, &sp_med, &j_busy) == RFC_ERR_BUSY);
        rfc_job_slot_destroy(slot2);
        CHECK(rfc_scheduler_reconfigure(sched, &lower) == RFC_OK);

        test_cond_destroy(&bc.cond);
        test_mutex_destroy(&bc.lock);
        free(big_buf);
        free(med_buf);
        rfc_scheduler_destroy(sched);
    }

    /* Gap 3: Pre-copy atomic queue/budget check performs 0 allocations on concurrent saturated rejects. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.max_queue_depth = 2u;
        cfg.memory_budget_bytes = 1024u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        r1_block_ctx bc;
        memset(&bc, 0, sizeof(bc));
        test_mutex_init(&bc.lock);
        test_cond_init(&bc.cond);

        rfc_job_spec bspec;
        init_basic_spec(&bspec, NULL, 0, &lay);
        bspec.expected_working_set_bytes = 16u * 1024u;
        bspec.stage_ops.on_import = r1_block_import_stage;
        bspec.stage_ops.user_ctx = &bc;

        rfc_job *blocker = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &bspec, &blocker) == RFC_OK);
        test_mutex_lock(&bc.lock);
        while (bc.entered == 0u) {
            test_cond_wait(&bc.cond, &bc.lock);
        }
        test_mutex_unlock(&bc.lock);

        rfc_job_spec qspec;
        init_basic_spec(&qspec, NULL, 0, &lay);
        qspec.expected_working_set_bytes = 16u * 1024u;
        rfc_job *qa = NULL;
        rfc_job *qb = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &qspec, &qa) == RFC_OK);
        CHECK(rfc_scheduler_submit(sched, NULL, &qspec, &qb) == RFC_OK);

        rfc_scheduler_stats st_before;
        rfc_scheduler_stats_get(sched, &st_before);

        size_t src_sz = 64u * 1024u;
        uint8_t *src = (uint8_t *)calloc(1u, src_sz);
        uint8_t icc[128];
        memset(icc, 0x5A, sizeof(icc));
        CHECK(src != NULL);

        gap_sat_barrier bar;
        memset(&bar, 0, sizeof(bar));
        test_mutex_init(&bar.lock);
        test_cond_init(&bar.cond);

        gap_sat_arg args[4];
        test_thread_t ths[4];
        for (int i = 0; i < 4; i++) {
            args[i].sched = sched;
            args[i].bar = &bar;
            args[i].src = src;
            args[i].src_len = src_sz;
            args[i].icc = icc;
            args[i].icc_len = sizeof(icc);
            args[i].st = RFC_OK;
            args[i].job = NULL;
            CHECK(test_thread_start(&ths[i], gap_sat_worker, &args[i]));
        }

        test_mutex_lock(&bar.lock);
        while (bar.ready < 4u) {
            test_cond_wait(&bar.cond, &bar.lock);
        }
        bar.go = 1;
        test_cond_broadcast(&bar.cond);
        test_mutex_unlock(&bar.lock);

        for (int i = 0; i < 4; i++) {
            test_thread_join(&ths[i]);
            CHECK(args[i].st == RFC_ERR_BUSY);
            CHECK(args[i].job == NULL);
        }

        rfc_scheduler_stats st_after;
        rfc_scheduler_stats_get(sched, &st_after);
        CHECK(st_after.submission_alloc_count == st_before.submission_alloc_count);

        test_mutex_lock(&bc.lock);
        bc.release = 1;
        test_cond_broadcast(&bc.cond);
        test_mutex_unlock(&bc.lock);

        CHECK(rfc_job_wait(blocker, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_wait(qa, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_wait(qb, UINT32_MAX) == RFC_OK);
        rfc_job_release(blocker);
        rfc_job_release(qa);
        rfc_job_release(qb);

        free(src);
        test_cond_destroy(&bar.cond);
        test_mutex_destroy(&bar.lock);
        test_cond_destroy(&bc.cond);
        test_mutex_destroy(&bc.lock);
        rfc_scheduler_destroy(sched);
    }

    /* Gap 3b: Memory-budget saturation with queue capacity available performs 0 submission allocations. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.max_queue_depth = 16u;
        cfg.memory_budget_bytes = 256u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        r1_block_ctx bc;
        memset(&bc, 0, sizeof(bc));
        test_mutex_init(&bc.lock);
        test_cond_init(&bc.cond);

        rfc_job_spec bspec;
        init_basic_spec(&bspec, NULL, 0, &lay);
        bspec.expected_working_set_bytes = 224u * 1024u;
        bspec.stage_ops.on_import = r1_block_import_stage;
        bspec.stage_ops.user_ctx = &bc;

        rfc_job *blocker = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &bspec, &blocker) == RFC_OK);
        test_mutex_lock(&bc.lock);
        while (bc.entered == 0u) {
            test_cond_wait(&bc.cond, &bc.lock);
        }
        test_mutex_unlock(&bc.lock);

        rfc_scheduler_stats st_before;
        rfc_scheduler_stats_get(sched, &st_before);
        CHECK(st_before.queued_jobs == 0u);

        size_t src_sz = 64u * 1024u;
        uint8_t *src = (uint8_t *)calloc(1u, src_sz);
        uint8_t icc[128];
        memset(icc, 0x5B, sizeof(icc));
        CHECK(src != NULL);

        gap_sat_barrier bar;
        memset(&bar, 0, sizeof(bar));
        test_mutex_init(&bar.lock);
        test_cond_init(&bar.cond);

        gap_sat_arg args[4];
        test_thread_t ths[4];
        for (int i = 0; i < 4; i++) {
            args[i].sched = sched;
            args[i].bar = &bar;
            args[i].src = src;
            args[i].src_len = src_sz;
            args[i].icc = icc;
            args[i].icc_len = sizeof(icc);
            args[i].st = RFC_OK;
            args[i].job = NULL;
            CHECK(test_thread_start(&ths[i], gap_sat_worker, &args[i]));
        }

        test_mutex_lock(&bar.lock);
        while (bar.ready < 4u) {
            test_cond_wait(&bar.cond, &bar.lock);
        }
        bar.go = 1;
        test_cond_broadcast(&bar.cond);
        test_mutex_unlock(&bar.lock);

        for (int i = 0; i < 4; i++) {
            test_thread_join(&ths[i]);
            CHECK(args[i].st == RFC_ERR_BUSY);
            CHECK(args[i].job == NULL);
        }

        rfc_scheduler_stats st_after;
        rfc_scheduler_stats_get(sched, &st_after);
        CHECK(st_after.submission_alloc_count == st_before.submission_alloc_count);

        test_mutex_lock(&bc.lock);
        bc.release = 1;
        test_cond_broadcast(&bc.cond);
        test_mutex_unlock(&bc.lock);

        CHECK(rfc_job_wait(blocker, UINT32_MAX) == RFC_OK);
        rfc_job_release(blocker);

        /* Also verify memory saturation solely from retained slot inputs while active=0, queue=0. */
        size_t hold_sz = 224u * 1024u;
        uint8_t *hold_buf = (uint8_t *)calloc(1u, hold_sz);
        CHECK(hold_buf != NULL);
        rfc_job_slot *slot_hold = rfc_job_slot_create();
        CHECK(slot_hold != NULL);
        rfc_job_spec hspec;
        init_basic_spec(&hspec, hold_buf, hold_sz, &lay);
        hspec.expected_working_set_bytes = 4096u;
        rfc_job *jhold = NULL;
        CHECK(rfc_scheduler_submit(sched, slot_hold, &hspec, &jhold) == RFC_OK);
        CHECK(rfc_job_wait(jhold, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot hsn;
        CHECK(rfc_job_snapshot_get(jhold, &hsn) == RFC_OK);
        CHECK(hsn.state == RFC_JOB_SUCCEEDED);
        rfc_job_release(jhold);

        rfc_scheduler_stats_get(sched, &st_before);
        CHECK(st_before.active_workers == 0u);
        CHECK(st_before.queued_jobs == 0u);

        test_cond_destroy(&bar.cond);
        test_mutex_destroy(&bar.lock);
        memset(&bar, 0, sizeof(bar));
        test_mutex_init(&bar.lock);
        test_cond_init(&bar.cond);
        for (int i = 0; i < 4; i++) {
            args[i].st = RFC_OK;
            args[i].job = NULL;
            CHECK(test_thread_start(&ths[i], gap_sat_worker, &args[i]));
        }
        test_mutex_lock(&bar.lock);
        while (bar.ready < 4u) {
            test_cond_wait(&bar.cond, &bar.lock);
        }
        bar.go = 1;
        test_cond_broadcast(&bar.cond);
        test_mutex_unlock(&bar.lock);
        for (int i = 0; i < 4; i++) {
            test_thread_join(&ths[i]);
            CHECK(args[i].st == RFC_ERR_BUSY);
            CHECK(args[i].job == NULL);
        }
        rfc_scheduler_stats_get(sched, &st_after);
        CHECK(st_after.submission_alloc_count == st_before.submission_alloc_count);

        rfc_job_slot_destroy(slot_hold);
        free(hold_buf);
        free(src);
        test_cond_destroy(&bar.cond);
        test_mutex_destroy(&bar.lock);
        test_cond_destroy(&bc.cond);
        test_mutex_destroy(&bc.lock);
        rfc_scheduler_destroy(sched);
    }

    /* Gap 4: Hostile multi-frame container header probe and mandatory pre-allocation frame checks. */
    {
        memset(&g_r1_probe, 0, sizeof(g_r1_probe));
        test_mutex_init(&g_r1_probe.lock);
        test_cond_init(&g_r1_probe.cond);

        rfc_session_adapter psess;
        memset(&psess, 0, sizeof(psess));
        psess.abi_version = RFC_CODEC_ABI_VERSION;
        psess.format = RFC_FORMAT_QOI;
        psess.open_session = r1_p_open;
        psess.close_session = r1_p_close;
        psess.probe_session = r1_p_probe;
        psess.decode_session = r1_p_decode;
        psess.encode_session = r1_p_encode;
        psess.cancel_session = r1_p_cancel;
        CHECK(rfc_registry_register_session(&psess) == RFC_OK);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.memory_budget_bytes = 512u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        uint8_t src[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        rfc_limits lim = cfg.default_limits;
        lim.max_decoded_bytes = 4096u;

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_QOI;
        spec.target_format = RFC_FORMAT_QOI;
        spec.source_bytes = src;
        spec.source_size = sizeof(src);
        spec.source_layout = &lay;
        spec.expected_working_set_bytes = 32768u;
        spec.limits_override = &lim;
        spec.has_stage_ops = 1;
        spec.stage_ops.on_verify = ok_noop_stage;
        spec.stage_ops.on_export = export_mark_ok;

        /* Subcase A: probe supplies aggregate total_decoded_bytes exceeding limits -> 0 decode_calls. */
        g_r1_probe.mode = 3;
        rfc_job *jm = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &jm) == RFC_OK);
        CHECK(rfc_job_wait(jm, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot sn;
        CHECK(rfc_job_snapshot_get(jm, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(g_r1_probe.decode_calls == 0u);
        rfc_job_release(jm);

        /* Subcase B: probe omits totals -> rfc_frame_alloc without prior incremental check fails before alloc. */
        g_r1_probe.mode = 4;
        rfc_job *ju = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &ju) == RFC_OK);
        CHECK(rfc_job_wait(ju, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(ju, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_INVALID_ARGUMENT);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(g_r1_probe.encode_calls == 0u);
        CHECK(sn.budget.peak_allocated_bytes < 512u);
        rfc_job_release(ju);

        /* Subcase C: unknown frame count -> frame 1 rfc_frame_alloc without prior check fails before alloc. */
        g_r1_probe.mode = 5;
        rfc_job *jr = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &jr) == RFC_OK);
        CHECK(rfc_job_wait(jr, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(jr, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_INVALID_ARGUMENT);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(g_r1_probe.encode_calls == 0u);
        CHECK(sn.budget.peak_allocated_bytes < 2048u);
        rfc_job_release(jr);

        /* Subcase D: unknown multi-frame totals with compliant 4 KiB scratch, free/realloc, and hook buffers succeeds. */
        g_r1_probe.mode = 6;
        uint32_t enc_before = g_r1_probe.encode_calls;
        rfc_job *jok = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &jok) == RFC_OK);
        CHECK(rfc_job_wait(jok, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(jok, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_SUCCEEDED);
        CHECK(sn.status == RFC_OK);
        CHECK(sn.budget.rejected_count == 0u);
        CHECK(g_r1_probe.encode_calls == enc_before + 1u);
        rfc_job_release(jok);

        /* Subcase E: unknown totals still govern scratch allocations strictly under job budget. */
        g_r1_probe.mode = 7;
        rfc_job_spec cap_spec = spec;
        cap_spec.expected_working_set_bytes = 16384u;
        cap_spec.job_memory_limit_bytes = 16384u;
        rfc_job *jcap = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &cap_spec, &jcap) == RFC_OK);
        CHECK(rfc_job_wait(jcap, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(jcap, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_OUT_OF_MEMORY);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(sn.budget.rejected_count >= 1u);
        CHECK(sn.budget.peak_allocated_bytes <= 16384u);
        rfc_job_release(jcap);

        /* Subcase F/G/H: credit binds to full validated layout, not just byte count. */
        rfc_limits tight_lim = lim;
        tight_lim.max_width = 128u;
        rfc_job_spec swap_spec = spec;
        swap_spec.limits_override = &tight_lim;

        g_r1_probe.mode = 8;
        enc_before = g_r1_probe.encode_calls;
        rfc_job *j8 = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &swap_spec, &j8) == RFC_OK);
        CHECK(rfc_job_wait(j8, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j8, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(g_r1_probe.encode_calls == enc_before);
        CHECK(sn.budget.peak_allocated_bytes < 512u);
        rfc_job_release(j8);

        g_r1_probe.mode = 9;
        rfc_job *j9 = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &swap_spec, &j9) == RFC_OK);
        CHECK(rfc_job_wait(j9, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j9, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(g_r1_probe.encode_calls == enc_before);
        CHECK(sn.budget.peak_allocated_bytes < 512u);
        rfc_job_release(j9);

        g_r1_probe.mode = 10;
        rfc_job *j10 = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &swap_spec, &j10) == RFC_OK);
        CHECK(rfc_job_wait(j10, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j10, &sn) == RFC_OK);
        CHECK(sn.state == RFC_JOB_FAILED);
        CHECK(sn.status == RFC_ERR_INVALID_ARGUMENT);
        CHECK(sn.failing_stage == RFC_STAGE_IMPORT);
        CHECK(g_r1_probe.encode_calls == enc_before);
        CHECK(sn.budget.peak_allocated_bytes < 512u);
        rfc_job_release(j10);

        rfc_scheduler_destroy(sched);
        CHECK(rfc_registry_unregister_session(RFC_FORMAT_QOI) == RFC_OK);
        test_cond_destroy(&g_r1_probe.cond);
        test_mutex_destroy(&g_r1_probe.lock);
    }

    printf("jobs: completion gap regressions ok (settings_gate=1 "
           "slot_retained=ok sat_allocs=0 multiframe_probe=ok "
           "credit_layout=bound)\n");
}

int main(void) {
    test_config_defaults_and_reconfigure();
    test_hostile_limits_and_unknown_reservation();
    test_bounded_cpu_and_memory_concurrency();
    test_oom_hook_cap();
    test_same_format_sessions_and_missing_stages();
    test_progress_retry_and_stale_rejection();
    test_cancellation_64x_and_shutdown();
    test_round1_regressions();
    test_completion_gap_regressions();

    if (g_failures > 0) {
        fprintf(stderr, "jobs: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("jobs: all checks passed\n");
    return 0;
}
