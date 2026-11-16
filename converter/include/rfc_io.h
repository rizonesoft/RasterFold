/* Portable bounded input sniffing, OS file identity, and exclusive staged
 * publication contracts (D10 T01 s5). Zero window, GUI, or <windows.h>
 * dependencies in the public C11/C++17 header. */
#ifndef RFC_IO_H
#define RFC_IO_H

#include <stddef.h>
#include <stdint.h>

#include "rfc_codec.h"
#include "rfc_image.h"
#include "rfc_job.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RFC_SNIFF_DETAIL_MAX 160u

typedef struct rfc_file_identity {
    int valid;
    int is_symlink_or_reparse;
    uint64_t volume_id_hi;
    uint64_t volume_id_lo;
    uint64_t file_id_hi;
    uint64_t file_id_lo;
    uint64_t link_count;
} rfc_file_identity;

typedef enum rfc_import_flags {
    RFC_IMPORT_DEFAULT = 0u,
    RFC_IMPORT_ALLOW_DISAGREEMENT = 1u << 0
} rfc_import_flags;

/* AGENTS.md prohibits overwriting existing destinations outright: only
 * RFC_EXPORT_REFUSE_EXISTING (0) is valid; any non-zero flag is rejected
 * with RFC_ERR_INVALID_ARGUMENT before staging or allocation. */
typedef enum rfc_export_flags {
    RFC_EXPORT_REFUSE_EXISTING = 0u
} rfc_export_flags;

typedef struct rfc_sniff_result {
    rfc_format_id content_format;
    rfc_format_id extension_format;
    int extension_present;
    int extension_known;
    int disagreement;
    char detail[RFC_SNIFF_DETAIL_MAX];
} rfc_sniff_result;

typedef struct rfc_imported_input {
    uint8_t *bytes;
    size_t size;
    rfc_allocator allocator;
    rfc_sniff_result sniff;
    rfc_file_identity identity;
} rfc_imported_input;

typedef struct rfc_export_report {
    int valid;
    rfc_status status;
    uint64_t bytes_written;
    int published;
    int temp_cleaned_up;
    uint8_t output_sha256[32];
    char detail[RFC_JOB_DETAIL_MAX];
} rfc_export_report;

/* Portable bounded stream reader callbacks.
 * seek_fn and read_fn are required; size_fn, identity_fn, and close_fn are
 * optional. During job execution the engine invokes seek_fn(ctx, 0) before
 * preflight sniffing and again before each attempt's bounded read in IMPORT.
 * When ownership transfers on rfc_scheduler_submit == RFC_OK, close_fn is
 * invoked at most once after all job attempts, retries, and cancellations
 * release the shared input context; on failed submit, close_fn is never
 * called and caller retains ownership. */
typedef struct rfc_io_reader {
    void *ctx;
    rfc_status (*size_fn)(void *ctx, int *out_known, uint64_t *out_size);
    rfc_status (*read_fn)(void *ctx, uint8_t *buf, size_t max_bytes,
                          size_t *out_read);
    rfc_status (*seek_fn)(void *ctx, uint64_t offset);
    rfc_status (*identity_fn)(void *ctx, rfc_file_identity *out_id);
    void (*close_fn)(void *ctx);
} rfc_io_reader;

/* Portable staged publication callbacks.
 * begin_stage, write_stage, flush_stage, commit_stage, and abort_stage are
 * required; release_ctx is optional. Each job attempt allocates a fresh
 * stage_handle via begin_stage; any failure or cancellation before commit
 * calls abort_stage to clean up only the attempt's temporary staging artifact
 * and return RFC_OK only when the temporary artifact was actually removed.
 * commit_stage must publish atomically without replacing any existing target
 * and report through out_temp_cleaned_up (1 when no temporary artifact remains,
 * 0 if a link-fallback temporary link could not be unlinked).
 * Ownership of ctx transfers only on rfc_scheduler_submit == RFC_OK, and
 * release_ctx runs exactly once when the final reference drops. */
typedef struct rfc_io_publisher {
    void *ctx;
    rfc_status (*begin_stage)(void *ctx, rfc_job_id job_id,
                              uint64_t attempt_number, uint64_t expected_bytes,
                              const rfc_file_identity *source_id,
                              void **out_stage_handle);
    rfc_status (*write_stage)(void *ctx, void *stage_handle,
                              const uint8_t *data, size_t len);
    rfc_status (*flush_stage)(void *ctx, void *stage_handle);
    rfc_status (*commit_stage)(void *ctx, void *stage_handle,
                               int *out_temp_cleaned_up);
    rfc_status (*abort_stage)(void *ctx, void *stage_handle);
    void (*release_ctx)(void *ctx);
} rfc_io_publisher;

typedef struct rfc_io_source {
    const char *path_utf8;
    const rfc_io_reader *reader;
    const char *virtual_name_utf8; /* optional extension hint for callback readers */
    uint32_t import_flags;
} rfc_io_source;

typedef struct rfc_io_sink {
    const char *path_utf8;
    const rfc_io_publisher *publisher;
    uint32_t export_flags;
} rfc_io_sink;

void rfc_io_source_init_path(rfc_io_source *out,
                             const char *path_utf8,
                             uint32_t import_flags);
void rfc_io_source_init_callbacks(rfc_io_source *out,
                                  const rfc_io_reader *reader,
                                  const char *virtual_name_utf8,
                                  uint32_t import_flags);
void rfc_io_sink_init_path(rfc_io_sink *out,
                           const char *path_utf8,
                           uint32_t export_flags);
void rfc_io_sink_init_callbacks(rfc_io_sink *out,
                                const rfc_io_publisher *publisher,
                                uint32_t export_flags);

rfc_status rfc_input_extension_format(const char *path_or_name_utf8,
                                      rfc_format_id *out_format,
                                      int *out_present,
                                      int *out_known);

rfc_status rfc_input_sniff_bytes(const uint8_t *bytes,
                                 size_t len,
                                 const char *name_utf8,
                                 rfc_sniff_result *out_result);

rfc_status rfc_input_sniff_reader(const rfc_io_reader *reader,
                                  const char *name_utf8,
                                  rfc_sniff_result *out_result,
                                  int *out_size_known,
                                  uint64_t *out_size);

rfc_status rfc_input_sniff_path(const char *path_utf8,
                                rfc_sniff_result *out_result,
                                uint64_t *out_size,
                                rfc_file_identity *out_identity);

rfc_status rfc_input_read_bounded(const rfc_io_reader *reader,
                                  const char *name_utf8,
                                  uint32_t import_flags,
                                  const rfc_limits *limits,
                                  const rfc_allocator *alloc,
                                  const rfc_exec_context *exec,
                                  rfc_imported_input *out_input);

rfc_status rfc_input_read_path(const char *path_utf8,
                               uint32_t import_flags,
                               const rfc_limits *limits,
                               const rfc_allocator *alloc,
                               const rfc_exec_context *exec,
                               rfc_imported_input *out_input);

void rfc_imported_input_free(rfc_imported_input *input);

rfc_status rfc_file_identity_query_path(const char *path_utf8,
                                        rfc_file_identity *out_identity);

int rfc_file_identity_equal(const rfc_file_identity *a,
                            const rfc_file_identity *b);

rfc_status rfc_output_check_destination_path(const char *dest_path_utf8,
                                             const char *source_path_utf8,
                                             const rfc_file_identity *source_id,
                                             uint32_t export_flags);

rfc_status rfc_output_publish_staged(const rfc_io_publisher *publisher,
                                     rfc_job *job,
                                     rfc_job_id job_id,
                                     uint64_t attempt_number,
                                     const rfc_file_identity *source_id,
                                     const uint8_t *bytes,
                                     size_t size,
                                     const rfc_exec_context *exec,
                                     rfc_export_report *out_report);

rfc_status rfc_output_write_path(const char *dest_path_utf8,
                                 const char *source_path_utf8,
                                 const rfc_file_identity *source_id,
                                 uint32_t export_flags,
                                 rfc_job *job,
                                 rfc_job_id job_id,
                                 const uint8_t *bytes,
                                 size_t size,
                                 const rfc_exec_context *exec,
                                 rfc_export_report *out_report);

rfc_status rfc_job_sniff_result_get(const rfc_job *job,
                                    rfc_sniff_result *out_sniff);

rfc_status rfc_job_export_report_get(const rfc_job *job,
                                     rfc_export_report *out_report);

/* Deterministic test hooks for fault injection and commit/cancel barriers.
 * Production code keeps both unset (0 / NULL). */
typedef enum rfc_io_fault_stage {
    RFC_IO_FAULT_NONE = 0,
    RFC_IO_FAULT_WRITE_DISK_FULL = 1,
    RFC_IO_FAULT_WRITE_IO = 2,
    RFC_IO_FAULT_FLUSH_IO = 3,
    RFC_IO_FAULT_COMMIT_EXISTS = 4,
    RFC_IO_FAULT_COMMIT_IO = 5,
    RFC_IO_FAULT_POSIX_NO_TMPFILE = 6,
    RFC_IO_FAULT_POSIX_PROC_LINK = 7,
    RFC_IO_FAULT_POSIX_HANDLE_LINK = 8
} rfc_io_fault_stage;

void rfc_output_set_write_fault_for_testing(rfc_io_fault_stage fault_stage,
                                            uint64_t fail_after_bytes,
                                            rfc_status fail_status);

void rfc_output_set_commit_barrier_for_testing(
    void (*before_begin_publish)(rfc_job_id job_id, void *user),
    void (*after_begin_publish)(rfc_job_id job_id, void *user),
    void *user);

void rfc_output_set_posix_post_check_for_testing(
    void (*after_commit_check)(rfc_job_id job_id, void *user),
    void (*after_abort_check)(rfc_job_id job_id, void *user),
    void *user);

void rfc_output_set_posix_post_link_for_testing(
    void (*after_link)(rfc_job_id job_id, void *user),
    void *user);

void rfc_output_set_win32_unc_root_for_testing(const char *unc_share_utf8,
                                               const char *backing_dir_utf8);

#ifdef __cplusplus
}
#endif

#endif /* RFC_IO_H */
