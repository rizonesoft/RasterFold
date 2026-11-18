#ifndef RFC_JOB_INTERNAL_H
#define RFC_JOB_INTERNAL_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "rfc_job.h"
#include "rfc_verify.h"
#include "rfc_io.h"
#include "rfc_report.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

typedef struct rfc_mutex {
#ifdef _WIN32
    SRWLOCK lock;
#else
    pthread_mutex_t lock;
#endif
} rfc_mutex;

typedef struct rfc_cond {
#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t cv;
#endif
} rfc_cond;

typedef struct rfc_thread_start {
    void (*fn)(void *);
    void *arg;
} rfc_thread_start;

typedef struct rfc_thread {
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t thread;
#endif
    int started;
} rfc_thread;

static inline int rfc_mutex_init(rfc_mutex *m) {
#ifdef _WIN32
    InitializeSRWLock(&m->lock);
    return 1;
#else
    return pthread_mutex_init(&m->lock, NULL) == 0;
#endif
}

static inline void rfc_mutex_destroy(rfc_mutex *m) {
#ifdef _WIN32
    (void)m;
#else
    pthread_mutex_destroy(&m->lock);
#endif
}

static inline void rfc_mutex_lock(rfc_mutex *m) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&m->lock);
#else
    pthread_mutex_lock(&m->lock);
#endif
}

static inline void rfc_mutex_unlock(rfc_mutex *m) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&m->lock);
#else
    pthread_mutex_unlock(&m->lock);
#endif
}

static inline int rfc_cond_init(rfc_cond *c) {
#ifdef _WIN32
    InitializeConditionVariable(&c->cv);
    return 1;
#else
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        return 0;
    }
    int rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) {
        rc = pthread_cond_init(&c->cv, &attr);
    }
    pthread_condattr_destroy(&attr);
    return rc == 0;
#endif
}

static inline void rfc_cond_destroy(rfc_cond *c) {
#ifdef _WIN32
    (void)c;
#else
    pthread_cond_destroy(&c->cv);
#endif
}

static inline void rfc_cond_signal(rfc_cond *c) {
#ifdef _WIN32
    WakeConditionVariable(&c->cv);
#else
    pthread_cond_signal(&c->cv);
#endif
}

static inline void rfc_cond_broadcast(rfc_cond *c) {
#ifdef _WIN32
    WakeAllConditionVariable(&c->cv);
#else
    pthread_cond_broadcast(&c->cv);
#endif
}

static inline void rfc_cond_wait(rfc_cond *c, rfc_mutex *m) {
#ifdef _WIN32
    SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0);
#else
    pthread_cond_wait(&c->cv, &m->lock);
#endif
}

static inline int rfc_cond_timedwait(rfc_cond *c, rfc_mutex *m, uint32_t timeout_ms) {
#ifdef _WIN32
    if (timeout_ms == UINT32_MAX) {
        SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0);
        return 1;
    }
    DWORD wait_ms = (DWORD)timeout_ms;
    if (timeout_ms == INFINITE) {
        wait_ms = INFINITE - 1u;
    }
    if (!SleepConditionVariableSRW(&c->cv, &m->lock, wait_ms, 0)) {
        if (GetLastError() == ERROR_TIMEOUT) {
            return 0;
        }
        return 0;
    }
    return 1;
#else
    if (timeout_ms == UINT32_MAX) {
        pthread_cond_wait(&c->cv, &m->lock);
        return 1;
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    uint64_t add_ns = (uint64_t)timeout_ms * 1000000ull;
    uint64_t sec = (uint64_t)ts.tv_sec + add_ns / 1000000000ull;
    uint64_t nsec = (uint64_t)ts.tv_nsec + add_ns % 1000000000ull;
    if (nsec >= 1000000000ull) {
        sec += 1ull;
        nsec -= 1000000000ull;
    }
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)nsec;
    int rc = pthread_cond_timedwait(&c->cv, &m->lock, &ts);
    return rc == 0;
#endif
}

#ifdef _WIN32
static inline unsigned __stdcall rfc_win_thread_trampoline(void *param) {
    rfc_thread_start *st = (rfc_thread_start *)param;
    void (*fn)(void *) = st->fn;
    void *arg = st->arg;
    free(st);
    fn(arg);
    return 0;
}
#else
static inline void *rfc_posix_thread_trampoline(void *param) {
    rfc_thread_start *st = (rfc_thread_start *)param;
    void (*fn)(void *) = st->fn;
    void *arg = st->arg;
    free(st);
    fn(arg);
    return NULL;
}
#endif

static inline int rfc_thread_start_fn(rfc_thread *t, void (*fn)(void *), void *arg) {
    memset(t, 0, sizeof(*t));
    rfc_thread_start *st = (rfc_thread_start *)malloc(sizeof(*st));
    if (!st) {
        return 0;
    }
    st->fn = fn;
    st->arg = arg;
#ifdef _WIN32
    uintptr_t h = _beginthreadex(NULL, 0, rfc_win_thread_trampoline, st, 0, NULL);
    if (h == 0) {
        free(st);
        return 0;
    }
    t->handle = (HANDLE)h;
    t->started = 1;
    return 1;
#else
    if (pthread_create(&t->thread, NULL, rfc_posix_thread_trampoline, st) != 0) {
        free(st);
        return 0;
    }
    t->started = 1;
    return 1;
#endif
}

static inline void rfc_thread_join(rfc_thread *t) {
    if (!t || !t->started) {
        return;
    }
#ifdef _WIN32
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    t->handle = NULL;
#else
    pthread_join(t->thread, NULL);
#endif
    t->started = 0;
}

typedef struct rfc_thread_id_t {
#ifdef _WIN32
    DWORD id;
#else
    pthread_t id;
#endif
} rfc_thread_id_t;

static inline rfc_thread_id_t rfc_thread_id_current(void) {
    rfc_thread_id_t t;
#ifdef _WIN32
    t.id = GetCurrentThreadId();
#else
    t.id = pthread_self();
#endif
    return t;
}

static inline int rfc_thread_id_equal(rfc_thread_id_t a, rfc_thread_id_t b) {
#ifdef _WIN32
    return a.id == b.id;
#else
    return pthread_equal(a.id, b.id) != 0;
#endif
}

typedef struct rfc_user_ctx_box {
    atomic_size_t refs;
    void *user_ctx;
    void (*destroy_fn)(void *);
} rfc_user_ctx_box;

static inline rfc_user_ctx_box *rfc_user_ctx_box_create(void *user_ctx,
                                                        void (*destroy_fn)(void *)) {
    rfc_user_ctx_box *box = (rfc_user_ctx_box *)malloc(sizeof(*box));
    if (!box) {
        return NULL;
    }
    atomic_init(&box->refs, 1u);
    box->user_ctx = user_ctx;
    box->destroy_fn = destroy_fn;
    return box;
}

static inline void rfc_user_ctx_box_retain(rfc_user_ctx_box *box) {
    if (box) {
        atomic_fetch_add_explicit(&box->refs, 1u, memory_order_relaxed);
    }
}

static inline void rfc_user_ctx_box_release(rfc_user_ctx_box *box) {
    if (!box) {
        return;
    }
    if (atomic_fetch_sub_explicit(&box->refs, 1u, memory_order_acq_rel) == 1u) {
        if (box->destroy_fn && box->user_ctx) {
            box->destroy_fn(box->user_ctx);
        }
        free(box);
    }
}

typedef struct rfc_slot_state {
    atomic_size_t refs;
    rfc_mutex lock;
    rfc_job_id current_job_id;
} rfc_slot_state;

typedef struct rfc_io_reader_box {
    atomic_size_t refs;
    rfc_mutex lock;
    int ownership_transferred;
    rfc_io_reader reader;
} rfc_io_reader_box;

typedef struct rfc_io_publisher_box {
    atomic_size_t refs;
    rfc_mutex lock;
    int ownership_transferred;
    rfc_io_publisher publisher;
} rfc_io_publisher_box;

typedef struct rfc_job_inputs {
    atomic_size_t refs;
    rfc_scheduler *sched;
    uint64_t charged_bytes;
    uint8_t *source_bytes;
    size_t source_size;
    rfc_layout source_layout;
    int has_layout;
    rfc_metadata source_meta;
    int has_meta;
    uint8_t *owned_icc;
    uint8_t *owned_exif;
    uint8_t *owned_xmp;
    rfc_setting_value *settings;
    size_t setting_count;
    uint32_t acknowledged_unsupported_metadata;
    uint64_t retained_bytes;

    int has_io_source;
    char *io_source_path;
    char *io_source_virtual_name;
    uint32_t io_import_flags;
    rfc_io_reader_box *io_reader_box;
    uint64_t effective_source_size;

    int has_io_sink;
    char *io_sink_path;
    uint32_t io_export_flags;
    rfc_io_publisher_box *io_publisher_box;
} rfc_job_inputs;

typedef struct rfc_partial_entry {
    void *ptr;
    size_t byte_size;
    void (*free_fn)(void *ctx, void *ptr);
    void *free_ctx;
    struct rfc_partial_entry *next;
} rfc_partial_entry;

typedef struct rfc_cancel_snapshot {
    const rfc_session_adapter *sess_adapter;
    const rfc_jpeg_reconstruct_adapter *jrec_adapter;
    const rfc_jpeg_transcode_adapter *jtrans_adapter;
    void *session;
} rfc_cancel_snapshot;

struct rfc_job {
    rfc_mutex cb_lock;
    rfc_mutex lock;
    rfc_cond done_cond;

    atomic_size_t refs;
    rfc_scheduler *sched;
    rfc_slot_state *slot_state;
    rfc_job_inputs *inputs;

    rfc_job_id job_id;
    rfc_job_id superseded_by_id;
    uint64_t attempt_number;

    rfc_job_state state;
    rfc_status status;
    rfc_job_stage stage;
    rfc_job_stage failing_stage;
    char detail[RFC_JOB_DETAIL_MAX];

    int cancel_requested;
    int superseded;
    int suppress_callbacks;
    int published;
    uint32_t last_percent;
    int cb_owner_valid;
    rfc_thread_id_t cb_owner;

    rfc_format_id source_format;
    rfc_format_id target_format;
    rfc_limits limits;
    int has_limits_override;

    uint32_t requested_threads;
    uint32_t admitted_threads;
    uint64_t reserved_memory_bytes;
    uint64_t exec_working_bytes;
    uint64_t budget_limit_bytes;

    int has_stage_ops;
    rfc_pipeline_stage_ops stage_ops;
    rfc_user_ctx_box *user_ctx_box;

    rfc_progress_fn progress_fn;
    void *progress_user;

    rfc_budget *budget;
    rfc_allocator allocator;
    rfc_allocator frame_allocator;

    rfc_partial_entry *partials;
    uint64_t live_partial_bytes;
    uint64_t attached_partial_bytes;
    uint64_t staged_partial_bytes;

    /* Owned decoded frames and encoded output passed between pipeline stages. */
    rfc_frame *decoded_frames;
    size_t decoded_frame_count;
    rfc_metadata decoded_meta;
    uint8_t *decoded_meta_icc;
    uint8_t *decoded_meta_exif;
    uint8_t *decoded_meta_xmp;
    int has_decoded_meta;
    const char *adapter_detail; /* rfc_exec_set_detail; guarded by lock */
    const rfc_allocator *decoded_frames_alloc;

    rfc_encoded encoded_output;
    int has_encoded_output;
    const rfc_session_adapter *encoded_owner_sess;
    void *encoded_owner_handle;
    rfc_format_id encoded_owner_fmt;

    /* Guarded by job->lock while active session executes so cancel can notify it. */
    uint32_t active_cancel_calls;
    const rfc_session_adapter *active_sess_adapter;
    const rfc_jpeg_reconstruct_adapter *active_jrec_adapter;
    const rfc_jpeg_transcode_adapter *active_jtrans_adapter;
    void *active_session;

    /* Streaming per-frame pre-allocation counters guarded by job->lock. */
    int in_decode_session;
    int decode_probe_exact;
    uint32_t decode_probe_frames;
    uint64_t decode_probe_pixels;
    uint64_t decode_probe_bytes;
    uint32_t decode_credit_frames;
    uint64_t decode_last_checked_fbytes;
    size_t decode_last_checked_icc_len;
    size_t decode_last_checked_exif_len;
    size_t decode_last_checked_xmp_len;
    rfc_layout decode_last_checked_layout;
    int decode_unvalidated_alloc;
    rfc_status decode_guard_failure;
    uint32_t stream_frame_count;
    uint64_t stream_total_pixels;
    uint64_t stream_total_bytes;
    uint64_t decode_compressed_bytes;
    int has_decode_compressed_bytes;

    /* Verification evidence supplied by ENCODE stages and published report. */
    rfc_verify_report verify_report;
    rfc_jpeg_coeff_image *verify_src_coeffs;
    rfc_jpeg_coeff_image *verify_dst_coeffs;
    uint8_t *verify_reconstructed_bytes;
    size_t verify_reconstructed_len;
    rfc_metadata verify_reconstructed_meta;
    uint8_t *verify_reconstructed_icc;
    uint8_t *verify_reconstructed_exif;
    uint8_t *verify_reconstructed_xmp;
    int has_verify_reconstructed;
    int has_verify_reconstructed_meta;

    /* Safe file/callback import & export state (D10 T01 s5). */
    uint8_t *imported_source_bytes;
    size_t imported_source_size;
    rfc_sniff_result sniff_result;
    int has_sniff_result;
    rfc_file_identity source_identity;
    rfc_export_report export_report;
    rfc_stage_timings stage_timings;
    uint64_t raw_frame_bytes;
    int publishing;
    int publish_decided;

    /* Intrusive cancel dispatcher queue fields guarded by sched->lock. */
    rfc_cancel_snapshot pending_cancel;
    int cancel_queued;
    struct rfc_job *cancel_next;
};

struct rfc_job_slot {
    rfc_mutex lock;
    rfc_slot_state *state;
    uint64_t attempt_count;
    rfc_job *current_job;

    int has_last_spec;
    rfc_format_id source_format;
    rfc_format_id target_format;
    rfc_job_inputs *inputs;
    uint32_t requested_codec_threads;
    uint64_t expected_working_set_bytes;
    uint64_t job_memory_limit_bytes;
    rfc_limits owned_limits;
    int has_limits;
    int has_stage_ops;
    rfc_pipeline_stage_ops stage_ops;
    rfc_user_ctx_box *user_ctx_box;
    rfc_progress_fn progress_fn;
    void *progress_user;
};

/* Internal helpers shared cleanly between job.c and scheduler.c */
void rfc_scheduler_internal_retain(rfc_scheduler *sched);
void rfc_scheduler_internal_release(rfc_scheduler *sched);
void rfc_scheduler_internal_note_submit_alloc(rfc_scheduler *sched);
void rfc_scheduler_internal_unreserve_retained(rfc_scheduler *sched,
                                               uint64_t bytes);
void rfc_scheduler_internal_on_cancel_count(rfc_scheduler *sched);
void rfc_scheduler_internal_on_publish_cancelled(rfc_scheduler *sched);
void rfc_scheduler_internal_adjust_live_partials(rfc_scheduler *sched,
                                                 int64_t delta_bytes);
void rfc_scheduler_internal_adjust_live_partials_locked(rfc_scheduler *sched,
                                                        int64_t delta_bytes);
void rfc_scheduler_internal_lock(rfc_scheduler *sched);
void rfc_scheduler_internal_unlock(rfc_scheduler *sched);
int rfc_scheduler_internal_is_destroying_locked(rfc_scheduler *sched);
void rfc_scheduler_internal_callback_enter_locked(rfc_scheduler *sched);
void rfc_scheduler_internal_callback_leave(rfc_scheduler *sched);

rfc_slot_state *rfc_slot_state_create(void);
void rfc_slot_state_retain(rfc_slot_state *state);
void rfc_slot_state_release(rfc_slot_state *state);
rfc_job_id rfc_slot_state_get_id(rfc_slot_state *state);
void rfc_slot_state_set_id(rfc_slot_state *state, rfc_job_id id);

rfc_status rfc_job_internal_resolve_exactness(
    rfc_format_id target_format,
    const rfc_setting_value *settings,
    size_t setting_count,
    rfc_exactness_class *out_exact,
    rfc_metadata_policy *out_meta);
rfc_status rfc_job_internal_validate_settings(
    rfc_format_id source_format,
    rfc_format_id target_format,
    const rfc_setting_value *settings,
    size_t setting_count,
    const rfc_pipeline_stage_ops *stage_ops,
    const char **out_detail);
rfc_status rfc_job_internal_compute_retained_bytes(
    const uint8_t *src_bytes,
    size_t src_size,
    const rfc_metadata *src_meta,
    const rfc_setting_value *settings,
    size_t setting_count,
    const rfc_io_source *io_source,
    const rfc_io_sink *io_sink,
    uint64_t *out_bytes);
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
                                      uint64_t effective_source_size);
void rfc_job_inputs_arm_io_ownership(rfc_job_inputs *inputs);
void rfc_job_inputs_retain(rfc_job_inputs *inputs);
void rfc_job_inputs_release(rfc_job_inputs *inputs);

rfc_status rfc_job_internal_begin_publish(rfc_job *job, rfc_job_id expected_id);
void rfc_job_internal_finish_publish(rfc_job *job,
                                     rfc_job_id expected_id,
                                     int succeeded);
void rfc_job_internal_note_partial_bytes(rfc_job *job, int64_t delta_bytes);

/* Frame geometry the sniff read cheaply (PNG IHDR, JPEG SOF, TIFF first
 * IFD, GIF screen and first image), so the scheduler can size a job before
 * IMPORT. format is the sample format the adapter decodes the header to.
 * Internal: the public rfc_sniff_result keeps its layout. */
typedef struct rfc_sniff_geometry {
    int has_geometry;
    uint32_t width;
    uint32_t height;
    rfc_sample_format format;
} rfc_sniff_geometry;

rfc_status rfc_input_internal_sniff_bytes_geometry(
    const uint8_t *bytes, size_t len, const char *name_utf8,
    rfc_sniff_result *out_result, rfc_sniff_geometry *out_geometry);

/* out_geometry may be NULL. */
rfc_status rfc_input_sniff_source_preflight(
    const rfc_limits *limits,
    const rfc_io_source *io_source,
    rfc_format_id caller_source_format,
    rfc_sniff_result *out_sniff,
    uint64_t *out_source_size,
    rfc_file_identity *out_identity,
    rfc_sniff_geometry *out_geometry);
rfc_status rfc_input_read_job_source(
    rfc_job *job,
    const rfc_exec_context *exec,
    const rfc_io_source *io_source,
    rfc_io_reader_box *reader_box,
    uint32_t import_flags,
    rfc_format_id expected_format,
    const char **out_err_detail);
rfc_status rfc_output_validate_sink_preflight(
    const rfc_io_sink *io_sink,
    const rfc_io_source *io_source,
    const rfc_file_identity *source_identity);
rfc_status rfc_output_publish_job(
    rfc_job *job,
    const rfc_exec_context *exec,
    const char **out_err_detail);

typedef struct rfc_sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buf_len;
} rfc_sha256_ctx;

void rfc_sha256_init(rfc_sha256_ctx *ctx);
void rfc_sha256_update(rfc_sha256_ctx *ctx, const void *data, size_t len);
void rfc_sha256_final(rfc_sha256_ctx *ctx, uint8_t out_hash[32]);

typedef rfc_status (*rfc_history_build_line_fn)(const uint8_t *existing,
                                                size_t existing_len,
                                                int exists,
                                                void *cb_ctx,
                                                char **out_line,
                                                size_t *out_line_len);

rfc_status rfc_output_internal_hash_regular_path(const char *path_utf8,
                                                 uint64_t *out_bytes,
                                                 uint8_t out_sha256[32]);
rfc_status rfc_output_internal_read_regular_path(const char *path_utf8,
                                                 size_t max_bytes,
                                                 uint8_t **out_bytes,
                                                 size_t *out_size,
                                                 int *out_exists);
rfc_status rfc_output_internal_append_history_locked(
    const char *path_utf8,
    size_t max_file_bytes,
    rfc_history_build_line_fn build_fn,
    void *cb_ctx,
    int fault_stage,
    rfc_status fault_status);
rfc_status rfc_output_internal_unlink_owned_path(
    const char *path_utf8,
    const rfc_file_identity *expected_id);
rfc_status rfc_output_internal_unlink_path(const char *path_utf8);
rfc_status rfc_output_internal_create_staging_subdir(const char *path_utf8,
                                                     int parent_of_target,
                                                     char out_dir[4096]);
rfc_status rfc_output_internal_remove_staging_subdir(const char *dir_utf8);
rfc_status rfc_output_internal_promote_staged_path(
    const char *staged_path_utf8,
    const char *dest_path_utf8,
    const char *source_path_utf8,
    const rfc_file_identity *source_id,
    const rfc_file_identity *expected_staged_id);
uint64_t rfc_output_internal_next_seq(void);
void rfc_report_internal_finalize_configs(rfc_comparison_report *report);
int rfc_report_internal_is_valid_utf8_bounded(const char *buf,
                                              size_t cap,
                                              int require_nonempty,
                                              size_t *out_len);
void rfc_report_internal_copy_utf8(char *dst, size_t cap, const char *src);
uint64_t rfc_job_internal_raw_frame_bytes(const rfc_job *job);

typedef struct rfc_job_exec_result {
    rfc_status st;
    rfc_job_stage stage_at;
    const char *err_detail;
} rfc_job_exec_result;

rfc_job *rfc_job_internal_create(void);
void rfc_job_internal_bind_budget(rfc_job *job, rfc_budget *budget);
void rfc_job_internal_execute(rfc_job *job, rfc_job_exec_result *out_res);
void rfc_job_internal_set_detail(char dst[RFC_JOB_DETAIL_MAX], const char *src);
void rfc_job_internal_release_stage_outputs(rfc_job *job);
void rfc_job_internal_set_progress_hook(
    void (*fn)(rfc_job_id job_id, int phase, void *user),
    void *user);
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
    rfc_metadata *out_meta);

#endif /* RFC_JOB_INTERNAL_H */
