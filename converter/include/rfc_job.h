#ifndef RFC_JOB_H
#define RFC_JOB_H

#include <stddef.h>
#include <stdint.h>

#include "rfc_image.h"
#include "rfc_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t rfc_job_id;
#define RFC_JOB_ID_NONE ((rfc_job_id)0u)

typedef struct rfc_budget rfc_budget;
typedef struct rfc_job_slot rfc_job_slot;
typedef struct rfc_job rfc_job;
typedef struct rfc_scheduler rfc_scheduler;

typedef enum rfc_job_state {
    RFC_JOB_QUEUED = 0,
    RFC_JOB_RUNNING = 1,
    RFC_JOB_SUCCEEDED = 2,
    RFC_JOB_FAILED = 3,
    RFC_JOB_CANCELLED = 4,
    RFC_JOB_SUPERSEDED = 5
} rfc_job_state;

typedef enum rfc_job_stage {
    RFC_STAGE_NONE = 0,
    RFC_STAGE_PREFLIGHT = 1,
    RFC_STAGE_IMPORT = 2,
    RFC_STAGE_ENCODE = 3,
    RFC_STAGE_VERIFY = 4,
    RFC_STAGE_EXPORT = 5,
    RFC_STAGE_DONE = 6
} rfc_job_stage;

typedef enum rfc_memory_enforcement_mode {
    RFC_MEM_RESERVATION_AND_LIMITS = 1,
    RFC_MEM_HOOK_ENFORCED = 2
} rfc_memory_enforcement_mode;

typedef struct rfc_provider_memory_info {
    rfc_format_id format;
    rfc_memory_enforcement_mode mode;
    uint32_t working_set_multiplier;
    uint64_t fixed_overhead_bytes;
    const char *limitation_reason;
} rfc_provider_memory_info;

typedef struct rfc_limits {
    uint32_t max_width;
    uint32_t max_height;
    uint64_t max_pixels;
    uint32_t max_frames;
    uint64_t max_total_pixels;
    uint64_t max_decoded_bytes;
    size_t max_metadata_bytes;
    uint64_t max_compressed_bytes;
    uint32_t max_expansion_ratio;
} rfc_limits;

typedef struct rfc_budget_stats {
    uint64_t limit_bytes;
    uint64_t current_allocated_bytes;
    uint64_t peak_allocated_bytes;
    uint64_t allocation_count;
    uint64_t rejected_count;
} rfc_budget_stats;

struct rfc_exec_context {
    rfc_job_id job_id;
    uint32_t codec_threads;
    rfc_limits limits;
    const rfc_allocator *allocator;
    rfc_budget *budget;
    rfc_job *job;
};

typedef struct rfc_pipeline_stage_ops {
    rfc_status (*on_import)(rfc_job *job, const rfc_exec_context *exec, void *user_ctx);
    rfc_status (*on_encode)(rfc_job *job, const rfc_exec_context *exec, void *user_ctx);
    rfc_status (*on_verify)(rfc_job *job, const rfc_exec_context *exec, void *user_ctx);
    rfc_status (*on_export)(rfc_job *job, const rfc_exec_context *exec, void *user_ctx);
    void *user_ctx;
    void (*destroy_user_ctx)(void *user_ctx);
} rfc_pipeline_stage_ops;

enum {
    RFC_SCHED_MIN_WORKERS = 1,
    RFC_SCHED_MAX_WORKERS = 64,
    RFC_SCHED_DEFAULT_WORKERS = 4,
    RFC_SCHED_MIN_CPU_THREADS = 1,
    RFC_SCHED_MAX_CPU_THREADS = 256,
    RFC_SCHED_DEFAULT_CPU_THREADS = 4,
    RFC_SCHED_MIN_QUEUE_DEPTH = 1,
    RFC_SCHED_MAX_QUEUE_DEPTH = 4096,
    RFC_SCHED_DEFAULT_QUEUE_DEPTH = 64
};

#define RFC_SCHED_MIN_MEMORY_BUDGET ((uint64_t)(4u * 1024u))
#define RFC_SCHED_DEFAULT_MEMORY_BUDGET ((uint64_t)(512u * 1024u * 1024u))
#define RFC_SCHED_MAX_MEMORY_BUDGET ((uint64_t)(128ull * 1024ull * 1024ull * 1024ull))

typedef struct rfc_scheduler_config {
    uint32_t max_workers;
    uint32_t total_cpu_threads;
    uint32_t max_queue_depth;
    uint64_t memory_budget_bytes;
    rfc_limits default_limits;
} rfc_scheduler_config;

#define RFC_JOB_DETAIL_MAX 256

typedef struct rfc_job_progress {
    rfc_job_id job_id;
    rfc_job_state state;
    rfc_job_stage stage;
    uint32_t completed_units;
    uint32_t total_units;
    uint32_t percent;
    char detail[RFC_JOB_DETAIL_MAX];
} rfc_job_progress;

/* Serialized per job outside scheduler/job locks; must not block or call back
 * into rfc_job_cancel / rfc_scheduler_submit / rfc_scheduler_retry while
 * holding a lock acquired on a thread calling those APIs. */
typedef void (*rfc_progress_fn)(const rfc_job_progress *progress, void *user);

typedef struct rfc_job_snapshot {
    rfc_job_id job_id;
    rfc_job_id superseded_by_id;
    rfc_job_state state;
    rfc_status status;
    rfc_job_stage stage;
    rfc_job_stage failing_stage;
    uint32_t admitted_threads;
    uint64_t reserved_memory_bytes;
    rfc_budget_stats budget;
    uint64_t live_partial_bytes;
    uint64_t attempt_number;
    char detail[RFC_JOB_DETAIL_MAX];
} rfc_job_snapshot;

typedef struct rfc_scheduler_stats {
    uint32_t active_workers;
    uint32_t peak_workers;
    uint32_t active_cpu_threads;
    uint32_t peak_cpu_threads;
    uint32_t queued_jobs;
    uint64_t active_reserved_bytes;
    uint64_t peak_reserved_bytes;
    uint64_t live_partial_bytes;
    uint64_t published_cancelled_count;
    uint64_t total_submitted;
    uint64_t total_completed;
    uint64_t total_failed;
    uint64_t total_cancelled;
    uint64_t total_superseded;
    uint64_t submission_alloc_count;
} rfc_scheduler_stats;

typedef struct rfc_job_spec {
    rfc_format_id source_format;
    rfc_format_id target_format;
    const uint8_t *source_bytes;
    size_t source_size;
    const rfc_layout *source_layout;
    const rfc_metadata *source_meta;
    /* Validated in preflight across source and target format capabilities before
     * input retention or queueing; unknown settings, invalid enum values, or
     * disallowed combinations fail with RFC_ERR_UNKNOWN_SETTING,
     * RFC_ERR_SETTING_VALUE, or RFC_ERR_UNSUPPORTED_COMBINATION. */
    const rfc_setting_value *settings;
    size_t setting_count;
    uint32_t requested_codec_threads;
    uint64_t expected_working_set_bytes;
    uint64_t job_memory_limit_bytes;
    const rfc_limits *limits_override;
    int has_stage_ops;
    rfc_pipeline_stage_ops stage_ops;
    rfc_progress_fn progress_fn;
    void *progress_user;
} rfc_job_spec;

void rfc_limits_default(rfc_limits *out);
rfc_status rfc_limits_validate(const rfc_limits *limits);
rfc_status rfc_layout_normalize_stride(const rfc_layout *layout,
                                       rfc_layout *out_normalized);
rfc_status rfc_limits_compute_frame_bytes(const rfc_layout *layout,
                                          size_t frame_count,
                                          uint64_t *out_bytes);
rfc_status rfc_limits_check_compressed(const rfc_limits *limits,
                                       uint64_t compressed_bytes);
rfc_status rfc_limits_check_image(const rfc_limits *limits,
                                  const rfc_layout *layout,
                                  size_t frame_count);
rfc_status rfc_limits_check_metadata(const rfc_limits *limits,
                                     const rfc_metadata *meta);
rfc_status rfc_limits_check_decompression(const rfc_limits *limits,
                                          uint64_t compressed_bytes,
                                          uint64_t decoded_bytes);
rfc_status rfc_limits_check_frames(const rfc_limits *limits,
                                   const rfc_frame *frames,
                                   size_t frame_count,
                                   const rfc_metadata *container_meta,
                                   uint64_t compressed_bytes,
                                   int has_compressed);
rfc_status rfc_limits_check_probe(const rfc_limits *limits,
                                  const rfc_image_probe *probe,
                                  uint64_t compressed_bytes,
                                  int has_compressed);
rfc_status rfc_query_provider_memory_contract(rfc_format_id id,
                                              rfc_provider_memory_info *out);

rfc_budget *rfc_budget_create(uint64_t limit_bytes);
void rfc_budget_destroy(rfc_budget *budget);
void rfc_budget_stats_get(const rfc_budget *budget, rfc_budget_stats *out);
void rfc_budget_allocator(rfc_budget *budget, rfc_allocator *out_alloc);
void *rfc_budget_alloc(rfc_budget *budget, size_t size);
void *rfc_budget_realloc(rfc_budget *budget, void *ptr, size_t new_size);
void rfc_budget_free(rfc_budget *budget, void *ptr);
void *rfc_budget_jxl_alloc(void *opaque, size_t size);
void rfc_budget_jxl_free(void *opaque, void *address);
void *rfc_budget_zlib_alloc(void *opaque, unsigned int items, unsigned int size);
void rfc_budget_zlib_free(void *opaque, void *address);

rfc_job_slot *rfc_job_slot_create(void);
void rfc_job_slot_destroy(rfc_job_slot *slot);
rfc_job_id rfc_job_slot_current_id(const rfc_job_slot *slot);

void rfc_job_retain(rfc_job *job);
void rfc_job_release(rfc_job *job);
rfc_job_id rfc_job_id_get(const rfc_job *job);
rfc_format_id rfc_job_source_format(const rfc_job *job);
rfc_format_id rfc_job_target_format(const rfc_job *job);
const uint8_t *rfc_job_source_bytes(const rfc_job *job, size_t *out_size);
const rfc_layout *rfc_job_source_layout(const rfc_job *job);
const rfc_metadata *rfc_job_source_metadata(const rfc_job *job);
const rfc_setting_value *rfc_job_settings(const rfc_job *job, size_t *out_count);
const rfc_frame *rfc_job_decoded_frames(const rfc_job *job,
                                        size_t *out_count,
                                        const rfc_metadata **out_meta);
const rfc_encoded *rfc_job_encoded_output(const rfc_job *job);
rfc_status rfc_job_set_decoded_frames(rfc_job *job,
                                      rfc_frame *frames,
                                      size_t frame_count,
                                      const rfc_metadata *meta);
rfc_status rfc_job_set_encoded_output(rfc_job *job,
                                      const rfc_encoded *encoded);
int rfc_job_is_cancelled(const rfc_job *job);
rfc_status rfc_job_cancel(rfc_job *job);
rfc_status rfc_job_wait(rfc_job *job, uint32_t timeout_ms);
rfc_status rfc_job_snapshot_get(const rfc_job *job, rfc_job_snapshot *out);
rfc_status rfc_job_report_progress(rfc_job *job,
                                   rfc_job_id expected_id,
                                   rfc_job_stage stage,
                                   uint32_t completed_units,
                                   uint32_t total_units,
                                   const char *detail);
rfc_status rfc_job_advance_stage(rfc_job *job,
                                 rfc_job_id expected_id,
                                 rfc_job_stage from_stage,
                                 rfc_job_stage to_stage);
rfc_status rfc_job_complete_stage(rfc_job *job,
                                  rfc_job_id expected_id,
                                  rfc_job_stage stage,
                                  rfc_status outcome,
                                  const char *detail);
rfc_status rfc_job_attach_partial(rfc_job *job,
                                  void *ptr,
                                  size_t byte_size,
                                  void (*free_fn)(void *ctx, void *ptr),
                                  void *free_ctx);
void *rfc_job_alloc_partial(rfc_job *job, size_t byte_size);
void rfc_job_release_partials(rfc_job *job);
rfc_status rfc_job_mark_published(rfc_job *job, rfc_job_id expected_id);

int rfc_exec_is_cancelled(const rfc_exec_context *exec);
uint32_t rfc_exec_threads(const rfc_exec_context *exec);
const rfc_limits *rfc_exec_limits(const rfc_exec_context *exec);
/* Job-budget allocator for codec scratch buffers, session state, frame
 * descriptor arrays, and metadata copies (governed strictly by rfc_budget).
 * Decoded rfc_frame pixel allocations must go through rfc_exec_alloc_frame or
 * rfc_frame_alloc (preceded by rfc_exec_check_next_frame when probe_session
 * omits exact aggregate bounds), where per-frame and cumulative limits are
 * enforced prior to pixel allocation. */
const rfc_allocator *rfc_exec_allocator(const rfc_exec_context *exec);
rfc_budget *rfc_exec_budget(const rfc_exec_context *exec);
rfc_status rfc_exec_check_next_frame(const rfc_exec_context *exec,
                                     const rfc_layout *layout,
                                     const rfc_metadata *meta);
rfc_status rfc_exec_alloc_frame(const rfc_exec_context *exec,
                                rfc_frame *frame,
                                const rfc_layout *layout,
                                const rfc_metadata *metadata);

void rfc_scheduler_config_default(rfc_scheduler_config *out);
rfc_status rfc_scheduler_config_validate(const rfc_scheduler_config *cfg);
rfc_scheduler *rfc_scheduler_create(const rfc_scheduler_config *cfg);
rfc_status rfc_scheduler_reconfigure(rfc_scheduler *sched,
                                     const rfc_scheduler_config *cfg);
rfc_status rfc_scheduler_get_config(const rfc_scheduler *sched,
                                    rfc_scheduler_config *out);
void rfc_scheduler_stats_get(const rfc_scheduler *sched,
                             rfc_scheduler_stats *out);
void rfc_scheduler_destroy(rfc_scheduler *sched);
rfc_status rfc_scheduler_submit(rfc_scheduler *sched,
                                rfc_job_slot *slot,
                                const rfc_job_spec *spec,
                                rfc_job **out_job);
rfc_status rfc_scheduler_retry(rfc_scheduler *sched,
                               rfc_job_slot *slot,
                               rfc_job **out_job);

const char *rfc_job_state_name(rfc_job_state state);
const char *rfc_job_stage_name(rfc_job_stage stage);

#ifdef __cplusplus
}
#endif

#endif /* RFC_JOB_H */
