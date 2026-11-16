#ifndef RFC_REPORT_H
#define RFC_REPORT_H

#include "rfc_codec.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RFC_REPORT_SCHEMA_NAME "rasterfold.converter.report"
#define RFC_REPORT_SCHEMA_VERSION 1u
#define RFC_HISTORY_SCHEMA_NAME "rasterfold.converter.history.entry"
#define RFC_HISTORY_SCHEMA_VERSION 1u

#define RFC_COMPARE_MAX_CANDIDATES 32u
#define RFC_COMPARE_MAX_ITERATIONS 32u
#define RFC_REPORT_MAX_SETTINGS 16u
#define RFC_REPORT_STR_MAX 96u
#define RFC_REPORT_FLAGS_MAX 256u
#define RFC_REPORT_NOTES_MAX 512u
#define RFC_REPORT_RUN_ID_MAX 64u
#define RFC_REPORT_UTC_MAX 32u
#define RFC_HISTORY_MAX_BYTES (64u * 1024u * 1024u)

typedef struct rfc_stage_timings {
    int valid;
    uint64_t import_ns;
    uint64_t encode_ns;
    uint64_t verify_ns;
    uint64_t export_ns;
    uint64_t wall_ns;
    int import_ran;
    int encode_ran;
    int verify_ran;
    int export_ran;
} rfc_stage_timings;

typedef enum rfc_size_outcome {
    RFC_SIZE_NOT_APPLICABLE = 0,
    RFC_SIZE_SMALLER = 1,
    RFC_SIZE_EQUAL_SIZE = 2,
    RFC_SIZE_IDENTICAL_BYTES = 3,
    RFC_SIZE_GROWTH = 4
} rfc_size_outcome;

typedef struct rfc_size_metrics {
    int valid;
    uint64_t source_file_bytes;
    uint64_t raw_frame_bytes;
    uint64_t produced_file_bytes;
    uint64_t savings_bytes;
    uint64_t growth_bytes;
    int64_t signed_delta_bytes;
    rfc_size_outcome outcome;
    uint8_t source_sha256[32];
    int has_source_sha256;
    uint8_t produced_sha256[32];
    int has_produced_sha256;
} rfc_size_metrics;

typedef struct rfc_provider_build_config {
    char provider_name[RFC_REPORT_STR_MAX];
    char codec_version[RFC_REPORT_STR_MAX];
    char compiler_id[RFC_REPORT_STR_MAX];
    char target_abi[RFC_REPORT_STR_MAX];
    char build_flags[RFC_REPORT_FLAGS_MAX];
    char simd_features[RFC_REPORT_STR_MAX];
    char threading_features[RFC_REPORT_STR_MAX];
} rfc_provider_build_config;

typedef enum rfc_candidate_outcome {
    RFC_CANDIDATE_COMPLETED = 0,
    RFC_CANDIDATE_UNSUPPORTED = 1,
    RFC_CANDIDATE_FAILED = 2,
    RFC_CANDIDATE_CANCELLED = 3
} rfc_candidate_outcome;

typedef struct rfc_report_setting_kv {
    char id[48];
    char value[64];
} rfc_report_setting_kv;

typedef struct rfc_compare_token rfc_compare_token;

typedef struct rfc_compare_candidate_spec {
    rfc_format_id target_format;
    const rfc_setting_value *settings;
    size_t setting_count;
    const char *dest_path_utf8;
    const rfc_io_publisher *publisher;
    int has_build_config_override;
    rfc_provider_build_config build_config;
    uint64_t expected_working_set_bytes;
    int has_stage_ops;
    rfc_pipeline_stage_ops stage_ops;
    void *user_ctx;
} rfc_compare_candidate_spec;

typedef struct rfc_compare_candidate_result {
    rfc_format_id target_format;
    rfc_candidate_outcome outcome;
    rfc_status status;
    rfc_job_stage failing_stage;
    int was_started;
    int has_produced_output;
    int measurement_only;
    uint32_t rank;
    uint32_t warmup_completed;
    uint32_t measured_completed;
    rfc_stage_timings per_iter_timings[RFC_COMPARE_MAX_ITERATIONS];
    rfc_stage_timings min_timings;
    rfc_stage_timings median_timings;
    uint64_t final_publish_ns;
    rfc_size_metrics size;
    rfc_verify_report verify_report;
    rfc_provider_build_config build_config;
    uint32_t effective_threads;
    rfc_report_setting_kv settings[RFC_REPORT_MAX_SETTINGS];
    size_t setting_count;
    char detail[RFC_JOB_DETAIL_MAX];
} rfc_compare_candidate_result;

typedef struct rfc_compare_spec {
    const rfc_io_source *io_source;
    const uint8_t *source_bytes;
    size_t source_size;
    rfc_format_id source_format;
    const char *staging_dir_utf8;
    uint32_t requested_threads;
    rfc_metadata_policy metadata_policy;
    uint32_t acknowledged_unsupported_metadata;
    const rfc_limits *limits;
    uint32_t warmup_iterations;
    uint32_t measured_iterations;
    const rfc_compare_candidate_spec *candidates;
    size_t candidate_count;
    rfc_compare_token *cancel_token;
    const char *history_path_utf8;
    const char *run_id;
    const char *recorded_utc;
} rfc_compare_spec;

typedef struct rfc_comparison_report {
    uint32_t schema_version;
    rfc_status run_status;
    rfc_status history_status;
    int history_recorded;
    int ranking_complete;
    int cancelled;
    rfc_format_id source_format;
    uint64_t source_file_bytes;
    uint64_t raw_frame_bytes;
    uint8_t source_sha256[32];
    uint32_t requested_threads;
    uint32_t effective_threads;
    uint32_t warmup_iterations;
    uint32_t measured_iterations;
    rfc_metadata_policy metadata_policy;
    uint32_t acknowledged_unsupported_metadata;
    rfc_limits limits;
    int mixed_provider_configurations;
    char configuration_notes[RFC_REPORT_NOTES_MAX];
    char run_id[RFC_REPORT_RUN_ID_MAX];
    char recorded_utc[RFC_REPORT_UTC_MAX];
    size_t candidate_count;
    uint32_t completed_count;
    uint32_t unsupported_count;
    uint32_t failed_count;
    uint32_t cancelled_count;
    rfc_compare_candidate_result candidates[RFC_COMPARE_MAX_CANDIDATES];
} rfc_comparison_report;

typedef struct rfc_history_candidate_summary {
    rfc_format_id target_format;
    rfc_candidate_outcome outcome;
    rfc_status status;
    uint32_t rank;
    int has_produced_output;
    int measurement_only;
    rfc_size_outcome size_outcome;
    uint64_t produced_file_bytes;
    int64_t signed_delta_bytes;
    uint64_t median_wall_ns;
    char codec_version[RFC_REPORT_STR_MAX];
    rfc_verify_meta_outcome meta_outcome;
} rfc_history_candidate_summary;

typedef struct rfc_history_entry_summary {
    uint64_t sequence;
    char run_id[RFC_REPORT_RUN_ID_MAX];
    char recorded_utc[RFC_REPORT_UTC_MAX];
    rfc_status run_status;
    int ranking_complete;
    rfc_format_id source_format;
    uint64_t source_file_bytes;
    uint64_t raw_frame_bytes;
    uint8_t source_sha256[32];
    uint32_t requested_threads;
    uint32_t effective_threads;
    uint32_t warmup_iterations;
    uint32_t measured_iterations;
    rfc_metadata_policy metadata_policy;
    rfc_limits limits;
    int mixed_provider_configurations;
    uint32_t completed_count;
    uint32_t unsupported_count;
    uint32_t failed_count;
    uint32_t cancelled_count;
    size_t candidate_count;
    rfc_history_candidate_summary candidates[RFC_COMPARE_MAX_CANDIDATES];
    uint8_t prev_entry_sha256[32];
    uint8_t entry_sha256[32];
} rfc_history_entry_summary;

typedef struct rfc_history_log {
    rfc_history_entry_summary *entries;
    size_t entry_count;
} rfc_history_log;

uint64_t rfc_monotonic_now_ns(void);

rfc_status rfc_job_stage_timings_get(const rfc_job *job,
                                     rfc_stage_timings *out_timings);

const char *rfc_size_outcome_name(rfc_size_outcome outcome);
const char *rfc_candidate_outcome_name(rfc_candidate_outcome outcome);

rfc_status rfc_metrics_compute_raw_frame_bytes(const rfc_frame *frames,
                                               size_t frame_count,
                                               uint64_t *out_raw_bytes);

rfc_status rfc_metrics_classify_size(uint64_t source_file_bytes,
                                     uint64_t raw_frame_bytes,
                                     uint64_t produced_file_bytes,
                                     const uint8_t source_sha256[32],
                                     int has_source_sha256,
                                     const uint8_t produced_sha256[32],
                                     int has_produced_sha256,
                                     rfc_size_metrics *out_metrics);

rfc_status rfc_metrics_verify_produced_path(const char *path_utf8,
                                            uint64_t source_file_bytes,
                                            uint64_t raw_frame_bytes,
                                            const uint8_t source_sha256[32],
                                            int has_source_sha256,
                                            const rfc_export_report *export_rep,
                                            rfc_size_metrics *out_metrics);

rfc_status rfc_metrics_verify_produced_callback(
    const rfc_io_publisher *publisher,
    rfc_job_id job_id,
    uint64_t attempt_number,
    uint64_t source_file_bytes,
    uint64_t raw_frame_bytes,
    const uint8_t source_sha256[32],
    int has_source_sha256,
    const rfc_export_report *export_rep,
    rfc_size_metrics *out_metrics);

rfc_status rfc_metrics_verify_job_output(const rfc_job *job,
                                         const char *path_if_any_utf8,
                                         uint64_t source_file_bytes,
                                         uint64_t raw_frame_bytes,
                                         const uint8_t source_sha256[32],
                                         int has_source_sha256,
                                         rfc_size_metrics *out_metrics);

rfc_status rfc_compare_token_create(rfc_compare_token **out_token);
void rfc_compare_token_destroy(rfc_compare_token *token);
void rfc_compare_cancel(rfc_compare_token *token);
int rfc_compare_is_cancelled(const rfc_compare_token *token);

rfc_status rfc_compare_run(const rfc_compare_spec *spec,
                           rfc_comparison_report *out_report);

rfc_status rfc_report_default_provider_config(
    rfc_format_id fmt,
    rfc_provider_build_config *out_config);

rfc_status rfc_report_register_provider_config(
    rfc_format_id fmt,
    const rfc_provider_build_config *config);

void rfc_report_clear_provider_configs(void);

rfc_status rfc_provider_build_config_validate(
    const rfc_provider_build_config *config);

rfc_status rfc_report_validate(const rfc_comparison_report *report);

rfc_status rfc_report_format_json(const rfc_comparison_report *report,
                                  char **out_json_utf8,
                                  size_t *out_len);

rfc_status rfc_report_write_json_path(const char *dest_path_utf8,
                                      const rfc_comparison_report *report);

void rfc_report_free(void *ptr);

rfc_status rfc_history_append_path(const char *path_utf8,
                                   const rfc_comparison_report *report);

rfc_status rfc_history_verify_bytes(const uint8_t *bytes,
                                    size_t len,
                                    rfc_history_log *out_log);

rfc_status rfc_history_read_path(const char *path_utf8,
                                 rfc_history_log *out_log);

void rfc_history_log_free(rfc_history_log *log);

/* Deterministic test hooks for cancellation barriers and history persistence faults. */
typedef enum rfc_compare_hook_phase {
    RFC_COMPARE_HOOK_BEFORE_CANDIDATE = 1,
    RFC_COMPARE_HOOK_WARMUP_ITER = 2,
    RFC_COMPARE_HOOK_MEASURED_ITER = 3,
    RFC_COMPARE_HOOK_BEFORE_FINAL_PUBLISH = 4,
    RFC_COMPARE_HOOK_AFTER_FINAL_PUBLISH = 5
} rfc_compare_hook_phase;

void rfc_compare_set_hook_for_testing(
    void (*fn)(rfc_compare_hook_phase phase,
               size_t candidate_index,
               uint32_t iteration_index,
               rfc_compare_token *token,
               void *user),
    void *user);

typedef enum rfc_history_fault_stage {
    RFC_HIST_FAULT_NONE = 0,
    RFC_HIST_FAULT_WRITE_DISK_FULL = 1,
    RFC_HIST_FAULT_WRITE_IO = 2,
    RFC_HIST_FAULT_FLUSH_IO = 3
} rfc_history_fault_stage;

void rfc_history_set_fault_for_testing(rfc_history_fault_stage stage,
                                       rfc_status status);

void rfc_history_set_max_bytes_for_testing(size_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* RFC_REPORT_H */
