#include "job_internal.h"
#include "rfc_report.h"

#include <limits.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#define RFC_METRICS_CHUNK_BYTES (64u * 1024u)

uint64_t rfc_monotonic_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER freq;
    LARGE_INTEGER cnt;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 ||
        !QueryPerformanceCounter(&cnt) || cnt.QuadPart < 0) {
        return 0u;
    }
    uint64_t c = (uint64_t)cnt.QuadPart;
    uint64_t f = (uint64_t)freq.QuadPart;
    uint64_t sec = c / f;
    uint64_t rem = c % f;
    uint64_t ns_part = 0u;
    if (rem <= UINT64_MAX / 1000000000ull) {
        ns_part = (rem * 1000000000ull) / f;
    } else {
        uint64_t div_k = f / 1000ull;
        ns_part = div_k ? ((rem * 1000000ull) / div_k) : 0u;
    }
    return sec * 1000000000ull + ns_part;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 ||
        ts.tv_nsec < 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

const char *rfc_size_outcome_name(rfc_size_outcome outcome) {
    switch (outcome) {
        case RFC_SIZE_NOT_APPLICABLE:
            return "not-applicable";
        case RFC_SIZE_SMALLER:
            return "smaller";
        case RFC_SIZE_EQUAL_SIZE:
            return "equal-size";
        case RFC_SIZE_IDENTICAL_BYTES:
            return "identical-bytes";
        case RFC_SIZE_GROWTH:
            return "growth";
        default:
            return "unknown-size-outcome";
    }
}

const char *rfc_candidate_outcome_name(rfc_candidate_outcome outcome) {
    switch (outcome) {
        case RFC_CANDIDATE_COMPLETED:
            return "completed";
        case RFC_CANDIDATE_UNSUPPORTED:
            return "unsupported";
        case RFC_CANDIDATE_FAILED:
            return "failed";
        case RFC_CANDIDATE_CANCELLED:
            return "cancelled";
        default:
            return "unknown-candidate-outcome";
    }
}

static int mul_u64_safe(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0u && b > UINT64_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static int add_u64_safe(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

rfc_status rfc_metrics_compute_raw_frame_bytes(const rfc_frame *frames,
                                               size_t frame_count,
                                               uint64_t *out_raw_bytes) {
    if (out_raw_bytes) {
        *out_raw_bytes = 0u;
    }
    if (!frames || frame_count == 0u || !out_raw_bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t total = 0u;
    for (size_t i = 0u; i < frame_count; i++) {
        const rfc_layout *lay = &frames[i].layout;
        if (lay->width == 0u || lay->height == 0u) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        size_t bpp = rfc_sample_bytes(lay->format);
        if (bpp == 0u) {
            return RFC_ERR_UNSUPPORTED_SAMPLE;
        }
        uint64_t row_bytes = 0u;
        uint64_t frame_bytes = 0u;
        if (!mul_u64_safe((uint64_t)lay->width, (uint64_t)bpp, &row_bytes) ||
            !mul_u64_safe(row_bytes, (uint64_t)lay->height, &frame_bytes) ||
            !add_u64_safe(total, frame_bytes, &total)) {
            return RFC_ERR_OVERFLOW;
        }
    }
    *out_raw_bytes = total;
    return RFC_OK;
}

rfc_status rfc_metrics_classify_size(uint64_t source_file_bytes,
                                     uint64_t raw_frame_bytes,
                                     uint64_t produced_file_bytes,
                                     const uint8_t source_sha256[32],
                                     int has_source_sha256,
                                     const uint8_t produced_sha256[32],
                                     int has_produced_sha256,
                                     rfc_size_metrics *out_metrics) {
    if (!out_metrics) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_metrics, 0, sizeof(*out_metrics));
    out_metrics->source_file_bytes = source_file_bytes;
    out_metrics->raw_frame_bytes = raw_frame_bytes;
    out_metrics->produced_file_bytes = produced_file_bytes;
    if (has_source_sha256 && source_sha256) {
        memcpy(out_metrics->source_sha256, source_sha256, 32u);
        out_metrics->has_source_sha256 = 1;
    }
    if (has_produced_sha256 && produced_sha256) {
        memcpy(out_metrics->produced_sha256, produced_sha256, 32u);
        out_metrics->has_produced_sha256 = 1;
    }

    if (produced_file_bytes < source_file_bytes) {
        uint64_t diff = source_file_bytes - produced_file_bytes;
        if (diff > (uint64_t)INT64_MAX) {
            return RFC_ERR_OVERFLOW;
        }
        out_metrics->savings_bytes = diff;
        out_metrics->growth_bytes = 0u;
        out_metrics->signed_delta_bytes = -(int64_t)diff;
        out_metrics->outcome = RFC_SIZE_SMALLER;
    } else if (produced_file_bytes > source_file_bytes) {
        uint64_t diff = produced_file_bytes - source_file_bytes;
        if (diff > (uint64_t)INT64_MAX) {
            return RFC_ERR_OVERFLOW;
        }
        out_metrics->savings_bytes = 0u;
        out_metrics->growth_bytes = diff;
        out_metrics->signed_delta_bytes = (int64_t)diff;
        out_metrics->outcome = RFC_SIZE_GROWTH;
    } else {
        out_metrics->savings_bytes = 0u;
        out_metrics->growth_bytes = 0u;
        out_metrics->signed_delta_bytes = 0;
        if (out_metrics->has_source_sha256 &&
            out_metrics->has_produced_sha256 &&
            memcmp(out_metrics->source_sha256, out_metrics->produced_sha256,
                   32u) == 0) {
            out_metrics->outcome = RFC_SIZE_IDENTICAL_BYTES;
        } else {
            out_metrics->outcome = RFC_SIZE_EQUAL_SIZE;
        }
    }

    out_metrics->valid = 1;
    return RFC_OK;
}

static rfc_status check_digest_and_bytes_match(
    uint64_t actual_bytes,
    const uint8_t actual_sha256[32],
    const rfc_export_report *export_rep,
    uint64_t source_file_bytes,
    uint64_t raw_frame_bytes,
    const uint8_t source_sha256[32],
    int has_source_sha256,
    rfc_size_metrics *out_metrics) {
    if (!export_rep || !export_rep->valid || !export_rep->published ||
        export_rep->status != RFC_OK) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (actual_bytes != export_rep->bytes_written ||
        memcmp(actual_sha256, export_rep->output_sha256, 32u) != 0) {
        return RFC_ERR_VERIFY_MISMATCH;
    }
    return rfc_metrics_classify_size(source_file_bytes, raw_frame_bytes,
                                     actual_bytes, source_sha256,
                                     has_source_sha256, actual_sha256, 1,
                                     out_metrics);
}

rfc_status rfc_metrics_verify_produced_path(const char *path_utf8,
                                            uint64_t source_file_bytes,
                                            uint64_t raw_frame_bytes,
                                            const uint8_t source_sha256[32],
                                            int has_source_sha256,
                                            const rfc_export_report *export_rep,
                                            rfc_size_metrics *out_metrics) {
    if (out_metrics) {
        memset(out_metrics, 0, sizeof(*out_metrics));
    }
    if (!path_utf8 || !export_rep || !out_metrics) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (!export_rep->valid || !export_rep->published ||
        export_rep->status != RFC_OK) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t actual_bytes = 0u;
    uint8_t actual_sha256[32];
    rfc_status st = rfc_output_internal_hash_regular_path(
        path_utf8, &actual_bytes, actual_sha256);
    if (st != RFC_OK) {
        return st;
    }
    return check_digest_and_bytes_match(actual_bytes, actual_sha256, export_rep,
                                        source_file_bytes, raw_frame_bytes,
                                        source_sha256, has_source_sha256,
                                        out_metrics);
}

static rfc_status verify_reader_against_export(
    const rfc_io_reader *reader,
    uint64_t source_file_bytes,
    uint64_t raw_frame_bytes,
    const uint8_t source_sha256[32],
    int has_source_sha256,
    const rfc_export_report *export_rep,
    rfc_size_metrics *out_metrics) {
    if (!reader || !reader->read_fn || !reader->seek_fn || !export_rep ||
        !out_metrics) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (!export_rep->valid || !export_rep->published ||
        export_rep->status != RFC_OK) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_status st = reader->seek_fn(reader->ctx, 0u);
    if (st != RFC_OK) {
        return st;
    }
    if (reader->size_fn) {
        int known = 0;
        uint64_t declared_sz = 0u;
        st = reader->size_fn(reader->ctx, &known, &declared_sz);
        if (st != RFC_OK) {
            return st;
        }
        if (known && declared_sz != export_rep->bytes_written) {
            return RFC_ERR_VERIFY_MISMATCH;
        }
    }

    rfc_sha256_ctx ctx;
    rfc_sha256_init(&ctx);
    uint64_t actual_bytes = 0u;
    uint8_t chunk[RFC_METRICS_CHUNK_BYTES];
    for (;;) {
        size_t got = 0u;
        st = reader->read_fn(reader->ctx, chunk, sizeof(chunk), &got);
        if (st != RFC_OK) {
            return st;
        }
        if (got > sizeof(chunk)) {
            return RFC_ERR_IO;
        }
        if (got == 0u) {
            break;
        }
        if ((uint64_t)got > UINT64_MAX - actual_bytes) {
            return RFC_ERR_OVERFLOW;
        }
        actual_bytes += (uint64_t)got;
        if (actual_bytes > export_rep->bytes_written) {
            return RFC_ERR_VERIFY_MISMATCH;
        }
        rfc_sha256_update(&ctx, chunk, got);
    }

    uint8_t actual_sha256[32];
    rfc_sha256_final(&ctx, actual_sha256);
    return check_digest_and_bytes_match(actual_bytes, actual_sha256, export_rep,
                                        source_file_bytes, raw_frame_bytes,
                                        source_sha256, has_source_sha256,
                                        out_metrics);
}

rfc_status rfc_metrics_verify_produced_callback(
    const rfc_io_publisher *publisher,
    rfc_job_id job_id,
    uint64_t attempt_number,
    uint64_t source_file_bytes,
    uint64_t raw_frame_bytes,
    const uint8_t source_sha256[32],
    int has_source_sha256,
    const rfc_export_report *export_rep,
    rfc_size_metrics *out_metrics) {
    if (out_metrics) {
        memset(out_metrics, 0, sizeof(*out_metrics));
    }
    if (!publisher || !export_rep || !out_metrics) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (!export_rep->valid || !export_rep->published ||
        export_rep->status != RFC_OK) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (!publisher->open_published_reader) {
        return RFC_ERR_NO_ADAPTER;
    }

    rfc_io_reader rdr;
    memset(&rdr, 0, sizeof(rdr));
    rfc_status st = publisher->open_published_reader(
        publisher->ctx, job_id, attempt_number, &rdr);
    if (st == RFC_OK && (!rdr.read_fn || !rdr.seek_fn)) {
        st = RFC_ERR_INVALID_ARGUMENT;
    }
    if (st == RFC_OK) {
        st = verify_reader_against_export(&rdr, source_file_bytes,
                                          raw_frame_bytes, source_sha256,
                                          has_source_sha256, export_rep,
                                          out_metrics);
    }
    if (rdr.close_fn) {
        rdr.close_fn(rdr.ctx);
    }
    return st;
}

rfc_status rfc_metrics_verify_job_output(const rfc_job *job,
                                         const char *path_if_any_utf8,
                                         uint64_t source_file_bytes,
                                         uint64_t raw_frame_bytes,
                                         const uint8_t source_sha256[32],
                                         int has_source_sha256,
                                         rfc_size_metrics *out_metrics) {
    if (out_metrics) {
        memset(out_metrics, 0, sizeof(*out_metrics));
    }
    if (!job || !out_metrics) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_export_report exp_rep;
    rfc_status st = rfc_job_export_report_get(job, &exp_rep);
    if (st != RFC_OK) {
        return st;
    }
    if (!exp_rep.valid || !exp_rep.published || exp_rep.status != RFC_OK) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    if (path_if_any_utf8) {
        return rfc_metrics_verify_produced_path(
            path_if_any_utf8, source_file_bytes, raw_frame_bytes, source_sha256,
            has_source_sha256, &exp_rep, out_metrics);
    }

    rfc_job *mj = (rfc_job *)job;
    if (mj->inputs && mj->inputs->io_sink_path) {
        return rfc_metrics_verify_produced_path(
            mj->inputs->io_sink_path, source_file_bytes, raw_frame_bytes,
            source_sha256, has_source_sha256, &exp_rep, out_metrics);
    }

    rfc_job_retain(mj);
    rfc_io_reader rdr;
    memset(&rdr, 0, sizeof(rdr));
    st = rfc_job_open_published_reader(mj, &rdr);
    if (st == RFC_OK) {
        st = verify_reader_against_export(&rdr, source_file_bytes,
                                          raw_frame_bytes, source_sha256,
                                          has_source_sha256, &exp_rep,
                                          out_metrics);
    }
    if (rdr.close_fn) {
        rdr.close_fn(rdr.ctx);
    }
    rfc_job_release(mj);
    return st;
}
