#include "job_internal.h"
#include "rasterfold.h"
#include "rfc_report.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rfc_history_fault_stage g_hist_fault_stage = RFC_HIST_FAULT_NONE;
static rfc_status g_hist_fault_status = RFC_OK;

void rfc_history_set_fault_for_testing(rfc_history_fault_stage stage,
                                       rfc_status status) {
    g_hist_fault_stage = stage;
    g_hist_fault_status = status;
}

void rfc_report_free(void *ptr) {
    free(ptr);
}

void rfc_history_log_free(rfc_history_log *log) {
    if (!log) {
        return;
    }
    free(log->entries);
    log->entries = NULL;
    log->entry_count = 0u;
}

static const char *format_id_name(rfc_format_id fmt) {
    const rfc_capability *cap = rfc_registry_find(fmt);
    return (cap && cap->name) ? cap->name : "unknown-format";
}

static void copy_str_safe(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void hex_encode_lower(const uint8_t bytes[32], char out_hex[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; i++) {
        out_hex[i * 2u] = hex[(bytes[i] >> 4) & 0x0fu];
        out_hex[i * 2u + 1u] = hex[bytes[i] & 0x0fu];
    }
    out_hex[64] = '\0';
}

static int hex_decode_lower64(const char *s, size_t len, uint8_t out_bytes[32]) {
    if (!s || len != 64u) {
        return 0;
    }
    for (size_t i = 0u; i < 32u; i++) {
        uint8_t vals[2];
        for (int k = 0; k < 2; k++) {
            unsigned char c = (unsigned char)s[i * 2u + (size_t)k];
            if (c >= '0' && c <= '9') {
                vals[k] = (uint8_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                vals[k] = (uint8_t)(10 + (c - 'a'));
            } else {
                return 0;
            }
        }
        out_bytes[i] = (uint8_t)((vals[0] << 4) | vals[1]);
    }
    return 1;
}

static void detect_compiler_id(char out_str[RFC_REPORT_STR_MAX]) {
#if defined(__clang__) && defined(__clang_version__)
    snprintf(out_str, RFC_REPORT_STR_MAX, "clang %s", __clang_version__);
#elif defined(__GNUC__) && defined(__VERSION__)
    snprintf(out_str, RFC_REPORT_STR_MAX, "gcc %s", __VERSION__);
#else
    copy_str_safe(out_str, RFC_REPORT_STR_MAX, "c11-compiler");
#endif
}

static const char *detect_target_abi(void) {
#if defined(_WIN32)
    return "windows-x64";
#else
    return "linux-x64";
#endif
}

static void detect_build_flags(char out_flags[RFC_REPORT_FLAGS_MAX]) {
    const char *opt =
#if defined(__OPTIMIZE__)
        "-O2+";
#else
        "-O0";
#endif
    const char *portable =
#if defined(RF_PORTABLE_SIMD)
        "-DRF_PORTABLE_SIMD";
#else
        "-march=x86-64-v2";
#endif
    snprintf(out_flags, RFC_REPORT_FLAGS_MAX,
             "-std=c11 %s %s -Wall -Wextra -Werror", opt, portable);
}

rfc_status rfc_report_default_provider_config(
    rfc_format_id fmt,
    rfc_provider_build_config *out_config) {
    if (!out_config) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_config, 0, sizeof(*out_config));
    const rfc_capability *cap = rfc_registry_find(fmt);
    if (!cap) {
        return RFC_ERR_UNKNOWN_FORMAT;
    }

    copy_str_safe(out_config->provider_name, sizeof(out_config->provider_name),
                  cap->provider);
    detect_compiler_id(out_config->compiler_id);
    copy_str_safe(out_config->target_abi, sizeof(out_config->target_abi),
                  detect_target_abi());
    detect_build_flags(out_config->build_flags);

    switch (fmt) {
        case RFC_FORMAT_RASTERFOLD:
            snprintf(out_config->codec_version,
                     sizeof(out_config->codec_version), "%u.%u",
                     (unsigned)RF_API_VERSION_MAJOR,
                     (unsigned)RF_API_VERSION_MINOR);
#ifdef RF_PORTABLE_SIMD
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "portable-c11 (RF_PORTABLE_SIMD)");
#else
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "x86-64-v2 + SSE2/SIMD paths");
#endif
#ifdef _WIN32
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "win32-threads (strip/channel pool)");
#else
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "pthreads (strip/channel pool)");
#endif
            break;
        case RFC_FORMAT_BMP:
        case RFC_FORMAT_PNM:
        case RFC_FORMAT_PAM:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "builtin-c11");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "scalar-c11 (x86-64-v2)");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "single-threaded");
            break;
        case RFC_FORMAT_PNG:
        case RFC_FORMAT_APNG:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "1.6.59");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "SSE2 hardware filter optimizations");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "single-threaded");
            break;
        case RFC_FORMAT_WEBP:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "1.6.0");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "SSE2/SSE4.1/AVX2 intrinsics (WEBP_ENABLE_SIMD=ON)");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "threaded-budgeted");
            break;
        case RFC_FORMAT_JPEG_XL:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "0.13.0");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "Google Highway runtime SIMD dispatch");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "jxl_threads (budgeted)");
            break;
        case RFC_FORMAT_AVIF:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version),
                          "1.4.2+aom-3.15.3");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "libaom NASM 3.02 + sharpyuv SIMD");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "libaom threads (budgeted)");
            break;
        case RFC_FORMAT_JPEG:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "3.2.1");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "x86-64 SSE2/AVX2 NASM 3.02 (REQUIRE_SIMD=ON)");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "single-threaded");
            break;
        case RFC_FORMAT_GIF:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "6.1.3");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "scalar LZW (x86-64-v2)");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "single-threaded");
            break;
        case RFC_FORMAT_TIFF:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "4.7.2");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "libdeflate/webp/jpeg-turbo SIMD");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "single-threaded");
            break;
        case RFC_FORMAT_HEIF:
        case RFC_FORMAT_HEIC:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "system-wic");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "Windows Imaging Component system codec");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "os-managed (import-only)");
            break;
        case RFC_FORMAT_JPEG2000:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "2.5.4");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "x86-64-v2 MCT/DWT auto-vectorized");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "openjp2 threads (budgeted)");
            break;
        case RFC_FORMAT_QOI:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version),
                          "0.0.0-ffb2d2cb");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "scalar C (x86-64-v2)");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "single-threaded");
            break;
        case RFC_FORMAT_OPENEXR:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "3.5.4");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "SSE2/F16C + OpenJPH + libdeflate SIMD");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "OpenEXRCore threads (budgeted)");
            break;
        case RFC_FORMAT_CAMERA_RAW:
        case RFC_FORMAT_DNG:
            copy_str_safe(out_config->codec_version,
                          sizeof(out_config->codec_version), "0.22.3");
            copy_str_safe(out_config->simd_features,
                          sizeof(out_config->simd_features),
                          "x86-64-v2 baseline (no OpenMP)");
            copy_str_safe(out_config->threading_features,
                          sizeof(out_config->threading_features),
                          "serial-deterministic (import-only)");
            break;
        default:
            return RFC_ERR_UNKNOWN_FORMAT;
    }
    return RFC_OK;
}

rfc_status rfc_provider_build_config_validate(
    const rfc_provider_build_config *config) {
    if (!config) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (config->provider_name[0] == '\0' || config->codec_version[0] == '\0' ||
        config->compiler_id[0] == '\0' || config->target_abi[0] == '\0' ||
        config->build_flags[0] == '\0' || config->simd_features[0] == '\0' ||
        config->threading_features[0] == '\0') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

static void detect_mixed_configs(const rfc_comparison_report *report,
                                 int *out_mixed,
                                 char notes[RFC_REPORT_NOTES_MAX]) {
    *out_mixed = 0;
    notes[0] = '\0';
    int diff_compiler = 0;
    int diff_abi = 0;
    int diff_flags = 0;
    int diff_simd = 0;
    int diff_threads = 0;
    const rfc_compare_candidate_result *base = NULL;

    for (size_t i = 0u; i < report->candidate_count; i++) {
        const rfc_compare_candidate_result *c = &report->candidates[i];
        if (rfc_provider_build_config_validate(&c->build_config) != RFC_OK) {
            continue;
        }
        if (c->outcome == RFC_CANDIDATE_UNSUPPORTED ||
            c->outcome == RFC_CANDIDATE_CANCELLED) {
            continue;
        }
        if (!base) {
            base = c;
            continue;
        }
        if (strcmp(base->build_config.compiler_id,
                   c->build_config.compiler_id) != 0) {
            diff_compiler = 1;
        }
        if (strcmp(base->build_config.target_abi,
                   c->build_config.target_abi) != 0) {
            diff_abi = 1;
        }
        if (strcmp(base->build_config.build_flags,
                   c->build_config.build_flags) != 0) {
            diff_flags = 1;
        }
        if (strcmp(base->build_config.simd_features,
                   c->build_config.simd_features) != 0) {
            diff_simd = 1;
        }
        if (strcmp(base->build_config.threading_features,
                   c->build_config.threading_features) != 0) {
            diff_threads = 1;
        }
    }

    if (diff_compiler || diff_abi || diff_flags || diff_simd || diff_threads) {
        *out_mixed = 1;
        snprintf(notes, RFC_REPORT_NOTES_MAX,
                 "providers differ across:%s%s%s%s%s",
                 diff_compiler ? " compiler" : "", diff_abi ? " target_abi" : "",
                 diff_flags ? " build_flags" : "",
                 diff_simd ? " simd_features" : "",
                 diff_threads ? " threading_features" : "");
    } else {
        copy_str_safe(notes, RFC_REPORT_NOTES_MAX,
                      "uniform provider configuration");
    }
}

void rfc_report_internal_finalize_configs(rfc_comparison_report *report) {
    if (!report) {
        return;
    }
    detect_mixed_configs(report, &report->mixed_provider_configurations,
                         report->configuration_notes);
}

static rfc_status validate_size_consistency(const rfc_compare_candidate_result *c,
                                            uint64_t report_source_bytes) {
    if (!c->size.valid || c->size.produced_file_bytes == 0u ||
        !c->size.has_produced_sha256) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t src_b = c->size.source_file_bytes;
    uint64_t prod_b = c->size.produced_file_bytes;
    if (src_b != report_source_bytes) {
        return RFC_ERR_VERIFY_MISMATCH;
    }
    switch (c->size.outcome) {
        case RFC_SIZE_SMALLER:
            if (!(prod_b < src_b)) {
                return RFC_ERR_VERIFY_MISMATCH;
            }
            break;
        case RFC_SIZE_EQUAL_SIZE:
            if (prod_b != src_b) {
                return RFC_ERR_VERIFY_MISMATCH;
            }
            break;
        case RFC_SIZE_IDENTICAL_BYTES:
            if (prod_b != src_b || !c->size.has_source_sha256 ||
                memcmp(c->size.source_sha256, c->size.produced_sha256, 32u) !=
                    0) {
                return RFC_ERR_VERIFY_MISMATCH;
            }
            break;
        case RFC_SIZE_GROWTH:
            if (!(prod_b > src_b) || c->size.growth_bytes != (prod_b - src_b)) {
                return RFC_ERR_VERIFY_MISMATCH;
            }
            break;
        default:
            return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

rfc_status rfc_report_validate(const rfc_comparison_report *report) {
    if (!report) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (report->schema_version != RFC_REPORT_SCHEMA_VERSION ||
        report->candidate_count > RFC_COMPARE_MAX_CANDIDATES ||
        report->run_id[0] == '\0' || report->recorded_utc[0] == '\0' ||
        !rfc_registry_find(report->source_format)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (report->cancelled && report->ranking_complete) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint32_t c_done = 0u;
    uint32_t c_unsup = 0u;
    uint32_t c_fail = 0u;
    uint32_t c_can = 0u;

    for (size_t i = 0u; i < report->candidate_count; i++) {
        const rfc_compare_candidate_result *c = &report->candidates[i];
        if (!rfc_registry_find(c->target_format)) {
            return RFC_ERR_UNKNOWN_FORMAT;
        }
        if (c->setting_count > RFC_REPORT_MAX_SETTINGS) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        if (c->outcome == RFC_CANDIDATE_COMPLETED) {
            c_done++;
            if (!c->was_started || c->status != RFC_OK ||
                !c->has_produced_output) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            rfc_status cst = rfc_provider_build_config_validate(&c->build_config);
            if (cst != RFC_OK) {
                return cst;
            }
            if (!c->verify_report.valid || c->verify_report.status != RFC_OK ||
                c->verify_report.decoder_provenance[0] == '\0') {
                return RFC_ERR_VERIFY_MISMATCH;
            }
            cst = validate_size_consistency(c, report->source_file_bytes);
            if (cst != RFC_OK) {
                return cst;
            }
            if (report->measured_iterations == 0u ||
                c->measured_completed != report->measured_iterations) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (report->ranking_complete && c->rank == 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (!report->ranking_complete && c->rank != 0u) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        } else {
            if (c->rank != 0u || c->has_produced_output != 0 ||
                c->size.outcome != RFC_SIZE_NOT_APPLICABLE) {
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (c->outcome == RFC_CANDIDATE_UNSUPPORTED) {
                c_unsup++;
            } else if (c->outcome == RFC_CANDIDATE_FAILED) {
                c_fail++;
            } else if (c->outcome == RFC_CANDIDATE_CANCELLED) {
                c_can++;
            } else {
                return RFC_ERR_INVALID_ARGUMENT;
            }
        }
    }

    if (c_done != report->completed_count ||
        c_unsup != report->unsupported_count ||
        c_fail != report->failed_count || c_can != report->cancelled_count ||
        (size_t)(c_done + c_unsup + c_fail + c_can) != report->candidate_count) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    int expect_mixed = 0;
    char notes[RFC_REPORT_NOTES_MAX];
    detect_mixed_configs(report, &expect_mixed, notes);
    if (expect_mixed && (!report->mixed_provider_configurations ||
                         report->configuration_notes[0] == '\0')) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    return RFC_OK;
}

/* ----------------------------------------------------------------
 * Growable UTF-8 JSON buffer builder
 * ---------------------------------------------------------------- */

typedef struct json_buf {
    char *data;
    size_t len;
    size_t cap;
    int oom;
} json_buf;

static void jb_init(json_buf *b) {
    memset(b, 0, sizeof(*b));
}

static void jb_free(json_buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0u;
    b->cap = 0u;
}

static int jb_reserve(json_buf *b, size_t extra) {
    if (b->oom) {
        return 0;
    }
    if (extra > SIZE_MAX - b->len - 1u) {
        b->oom = 1;
        return 0;
    }
    size_t need = b->len + extra + 1u;
    if (need <= b->cap) {
        return 1;
    }
    size_t ncap = b->cap ? b->cap : 1024u;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2u) {
            ncap = need;
            break;
        }
        ncap *= 2u;
    }
    char *nb = (char *)realloc(b->data, ncap);
    if (!nb) {
        b->oom = 1;
        return 0;
    }
    b->data = nb;
    b->cap = ncap;
    return 1;
}

static void jb_raw(json_buf *b, const char *s, size_t n) {
    if (!jb_reserve(b, n)) {
        return;
    }
    if (n > 0u) {
        memcpy(b->data + b->len, s, n);
        b->len += n;
    }
    b->data[b->len] = '\0';
}

static void jb_cstr(json_buf *b, const char *s) {
    jb_raw(b, s, strlen(s));
}

static void jb_u64(json_buf *b, uint64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    if (n > 0) {
        jb_raw(b, tmp, (size_t)n);
    }
}

static void jb_i64(json_buf *b, int64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    if (n > 0) {
        jb_raw(b, tmp, (size_t)n);
    }
}

static void jb_bool(json_buf *b, int v) {
    jb_cstr(b, v ? "true" : "false");
}

static void jb_json_str(json_buf *b, const char *s) {
    jb_raw(b, "\"", 1u);
    if (s) {
        for (size_t i = 0u; s[i] != '\0'; i++) {
            unsigned char c = (unsigned char)s[i];
            switch (c) {
                case '\"':
                    jb_cstr(b, "\\\"");
                    break;
                case '\\':
                    jb_cstr(b, "\\\\");
                    break;
                case '\b':
                    jb_cstr(b, "\\b");
                    break;
                case '\f':
                    jb_cstr(b, "\\f");
                    break;
                case '\n':
                    jb_cstr(b, "\\n");
                    break;
                case '\r':
                    jb_cstr(b, "\\r");
                    break;
                case '\t':
                    jb_cstr(b, "\\t");
                    break;
                default:
                    if (c < 0x20u) {
                        char esc[7];
                        snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                        jb_raw(b, esc, 6u);
                    } else {
                        char ch = (char)c;
                        jb_raw(b, &ch, 1u);
                    }
                    break;
            }
        }
    }
    jb_raw(b, "\"", 1u);
}

static void jb_timings_obj(json_buf *b, const rfc_stage_timings *t) {
    jb_cstr(b, "{\"valid\":");
    jb_bool(b, t->valid);
    jb_cstr(b, ",\"import_ns\":");
    jb_u64(b, t->import_ns);
    jb_cstr(b, ",\"encode_ns\":");
    jb_u64(b, t->encode_ns);
    jb_cstr(b, ",\"verify_ns\":");
    jb_u64(b, t->verify_ns);
    jb_cstr(b, ",\"export_ns\":");
    jb_u64(b, t->export_ns);
    jb_cstr(b, ",\"wall_ns\":");
    jb_u64(b, t->wall_ns);
    jb_cstr(b, "}");
}

rfc_status rfc_report_format_json(const rfc_comparison_report *report,
                                  char **out_json_utf8,
                                  size_t *out_len) {
    if (out_json_utf8) {
        *out_json_utf8 = NULL;
    }
    if (out_len) {
        *out_len = 0u;
    }
    if (!report || !out_json_utf8 || !out_len) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_status st = rfc_report_validate(report);
    if (st != RFC_OK) {
        return st;
    }

    char src_hex[65];
    hex_encode_lower(report->source_sha256, src_hex);

    json_buf b;
    jb_init(&b);
    jb_cstr(&b, "{\"schema\":");
    jb_json_str(&b, RFC_REPORT_SCHEMA_NAME);
    jb_cstr(&b, ",\"schema_version\":");
    jb_u64(&b, report->schema_version);
    jb_cstr(&b, ",\"run_id\":");
    jb_json_str(&b, report->run_id);
    jb_cstr(&b, ",\"recorded_utc\":");
    jb_json_str(&b, report->recorded_utc);
    jb_cstr(&b, ",\"run_status\":");
    jb_json_str(&b, rfc_status_name(report->run_status));
    jb_cstr(&b, ",\"history_status\":");
    jb_json_str(&b, rfc_status_name(report->history_status));
    jb_cstr(&b, ",\"history_recorded\":");
    jb_bool(&b, report->history_recorded);
    jb_cstr(&b, ",\"ranking_complete\":");
    jb_bool(&b, report->ranking_complete);
    jb_cstr(&b, ",\"cancelled\":");
    jb_bool(&b, report->cancelled);

    jb_cstr(&b, ",\"source\":{\"format\":");
    jb_json_str(&b, format_id_name(report->source_format));
    jb_cstr(&b, ",\"file_bytes\":");
    jb_u64(&b, report->source_file_bytes);
    jb_cstr(&b, ",\"raw_frame_bytes\":");
    jb_u64(&b, report->raw_frame_bytes);
    jb_cstr(&b, ",\"sha256\":");
    jb_json_str(&b, src_hex);
    jb_cstr(&b, "}");

    jb_cstr(&b, ",\"measurement_conditions\":{\"requested_threads\":");
    jb_u64(&b, report->requested_threads);
    jb_cstr(&b, ",\"effective_threads\":");
    jb_u64(&b, report->effective_threads);
    jb_cstr(&b, ",\"warmup_iterations\":");
    jb_u64(&b, report->warmup_iterations);
    jb_cstr(&b, ",\"measured_iterations\":");
    jb_u64(&b, report->measured_iterations);
    jb_cstr(&b, ",\"metadata_policy\":");
    jb_json_str(&b, rfc_metadata_policy_name(report->metadata_policy));
    jb_cstr(&b, ",\"acknowledged_unsupported_metadata\":");
    jb_u64(&b, report->acknowledged_unsupported_metadata);
    jb_cstr(&b, ",\"sequential_execution\":true}");

    jb_cstr(&b, ",\"mixed_provider_configurations\":");
    jb_bool(&b, report->mixed_provider_configurations);
    jb_cstr(&b, ",\"configuration_notes\":");
    jb_json_str(&b, report->configuration_notes);

    jb_cstr(&b, ",\"counts\":{\"completed\":");
    jb_u64(&b, report->completed_count);
    jb_cstr(&b, ",\"unsupported\":");
    jb_u64(&b, report->unsupported_count);
    jb_cstr(&b, ",\"failed\":");
    jb_u64(&b, report->failed_count);
    jb_cstr(&b, ",\"cancelled\":");
    jb_u64(&b, report->cancelled_count);
    jb_cstr(&b, "}");

    jb_cstr(&b, ",\"candidates\":[");
    for (size_t i = 0u; i < report->candidate_count; i++) {
        const rfc_compare_candidate_result *c = &report->candidates[i];
        if (i > 0u) {
            jb_raw(&b, ",", 1u);
        }
        jb_cstr(&b, "{\"target_format\":");
        jb_json_str(&b, format_id_name(c->target_format));
        jb_cstr(&b, ",\"outcome\":");
        jb_json_str(&b, rfc_candidate_outcome_name(c->outcome));
        jb_cstr(&b, ",\"status\":");
        jb_json_str(&b, rfc_status_name(c->status));
        jb_cstr(&b, ",\"failing_stage\":");
        jb_json_str(&b, rfc_job_stage_name(c->failing_stage));
        jb_cstr(&b, ",\"was_started\":");
        jb_bool(&b, c->was_started);
        jb_cstr(&b, ",\"has_produced_output\":");
        jb_bool(&b, c->has_produced_output);
        jb_cstr(&b, ",\"rank\":");
        jb_u64(&b, c->rank);
        jb_cstr(&b, ",\"warmup_completed\":");
        jb_u64(&b, c->warmup_completed);
        jb_cstr(&b, ",\"measured_completed\":");
        jb_u64(&b, c->measured_completed);
        jb_cstr(&b, ",\"effective_threads\":");
        jb_u64(&b, c->effective_threads);

        jb_cstr(&b, ",\"settings\":[");
        for (size_t s = 0u; s < c->setting_count; s++) {
            if (s > 0u) {
                jb_raw(&b, ",", 1u);
            }
            jb_cstr(&b, "{\"id\":");
            jb_json_str(&b, c->settings[s].id);
            jb_cstr(&b, ",\"value\":");
            jb_json_str(&b, c->settings[s].value);
            jb_cstr(&b, "}");
        }
        jb_cstr(&b, "]");

        jb_cstr(&b, ",\"provider_config\":{\"provider_name\":");
        jb_json_str(&b, c->build_config.provider_name);
        jb_cstr(&b, ",\"codec_version\":");
        jb_json_str(&b, c->build_config.codec_version);
        jb_cstr(&b, ",\"compiler_id\":");
        jb_json_str(&b, c->build_config.compiler_id);
        jb_cstr(&b, ",\"target_abi\":");
        jb_json_str(&b, c->build_config.target_abi);
        jb_cstr(&b, ",\"build_flags\":");
        jb_json_str(&b, c->build_config.build_flags);
        jb_cstr(&b, ",\"simd_features\":");
        jb_json_str(&b, c->build_config.simd_features);
        jb_cstr(&b, ",\"threading_features\":");
        jb_json_str(&b, c->build_config.threading_features);
        jb_cstr(&b, "}");

        char vsrc[65];
        char vdst[65];
        hex_encode_lower(c->verify_report.source_hash, vsrc);
        hex_encode_lower(c->verify_report.verified_hash, vdst);
        jb_cstr(&b, ",\"verification\":{\"valid\":");
        jb_bool(&b, c->verify_report.valid);
        jb_cstr(&b, ",\"status\":");
        jb_json_str(&b, rfc_status_name(c->verify_report.status));
        jb_cstr(&b, ",\"exactness\":");
        jb_json_str(&b,
                    c->verify_report.exactness
                        ? rfc_exactness_class_name(c->verify_report.exactness)
                        : "none");
        jb_cstr(&b, ",\"meta_outcome\":");
        jb_json_str(
            &b,
            c->verify_report.meta_outcome
                ? rfc_verify_meta_outcome_name(c->verify_report.meta_outcome)
                : "none");
        jb_cstr(&b, ",\"independent_decoder\":");
        jb_bool(&b, c->verify_report.independent_decoder);
        jb_cstr(&b, ",\"decoder_provenance\":");
        jb_json_str(&b, c->verify_report.decoder_provenance);
        jb_cstr(&b, ",\"source_hash\":");
        jb_json_str(&b, vsrc);
        jb_cstr(&b, ",\"verified_hash\":");
        jb_json_str(&b, vdst);
        jb_cstr(&b, "}");

        char pdst[65];
        hex_encode_lower(c->size.produced_sha256, pdst);
        jb_cstr(&b, ",\"size\":{\"valid\":");
        jb_bool(&b, c->size.valid);
        jb_cstr(&b, ",\"outcome\":");
        jb_json_str(&b, rfc_size_outcome_name(c->size.outcome));
        jb_cstr(&b, ",\"source_file_bytes\":");
        jb_u64(&b, c->size.source_file_bytes);
        jb_cstr(&b, ",\"raw_frame_bytes\":");
        jb_u64(&b, c->size.raw_frame_bytes);
        jb_cstr(&b, ",\"produced_file_bytes\":");
        jb_u64(&b, c->size.produced_file_bytes);
        jb_cstr(&b, ",\"savings_bytes\":");
        jb_u64(&b, c->size.savings_bytes);
        jb_cstr(&b, ",\"growth_bytes\":");
        jb_u64(&b, c->size.growth_bytes);
        jb_cstr(&b, ",\"signed_delta_bytes\":");
        jb_i64(&b, c->size.signed_delta_bytes);
        jb_cstr(&b, ",\"produced_sha256\":");
        jb_json_str(&b, c->size.has_produced_sha256 ? pdst : "");
        jb_cstr(&b, "}");

        jb_cstr(&b, ",\"timings_ns\":{\"min\":");
        jb_timings_obj(&b, &c->min_timings);
        jb_cstr(&b, ",\"median\":");
        jb_timings_obj(&b, &c->median_timings);
        jb_cstr(&b, ",\"per_iteration\":[");
        for (uint32_t m = 0u;
             m < c->measured_completed && m < RFC_COMPARE_MAX_ITERATIONS; m++) {
            if (m > 0u) {
                jb_raw(&b, ",", 1u);
            }
            jb_timings_obj(&b, &c->per_iter_timings[m]);
        }
        jb_cstr(&b, "]}");

        jb_cstr(&b, ",\"detail\":");
        jb_json_str(&b, c->detail);
        jb_cstr(&b, "}");
    }
    jb_cstr(&b, "]}\n");

    if (b.oom || !b.data) {
        jb_free(&b);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    *out_json_utf8 = b.data;
    *out_len = b.len;
    return RFC_OK;
}

rfc_status rfc_report_write_json_path(const char *dest_path_utf8,
                                      const rfc_comparison_report *report) {
    if (!dest_path_utf8 || !report) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    char *json = NULL;
    size_t len = 0u;
    rfc_status st = rfc_report_format_json(report, &json, &len);
    if (st != RFC_OK) {
        return st;
    }
    rfc_export_report rep;
    memset(&rep, 0, sizeof(rep));
    st = rfc_output_write_path(dest_path_utf8, NULL, NULL,
                               RFC_EXPORT_REFUSE_EXISTING, NULL, 0u,
                               (const uint8_t *)json, len, NULL, &rep);
    free(json);
    return st;
}

/* ----------------------------------------------------------------
 * Auditable tamper-evident JSONL history
 * ---------------------------------------------------------------- */

static const char *parse_status_by_name(const char *s, rfc_status *out) {
    static const rfc_status all_st[] = {
        RFC_OK,
        RFC_ERR_INVALID_ARGUMENT,
        RFC_ERR_OVERFLOW,
        RFC_ERR_UNSUPPORTED_SAMPLE,
        RFC_ERR_UNSUPPORTED_ALPHA,
        RFC_ERR_UNSUPPORTED_COMBINATION,
        RFC_ERR_UNKNOWN_FORMAT,
        RFC_ERR_UNKNOWN_SETTING,
        RFC_ERR_SETTING_VALUE,
        RFC_ERR_PLANNED,
        RFC_ERR_NO_ADAPTER,
        RFC_ERR_DUPLICATE,
        RFC_ERR_ABI_VERSION,
        RFC_ERR_OUT_OF_MEMORY,
        RFC_ERR_CANCELLED,
        RFC_ERR_LIMIT_EXCEEDED,
        RFC_ERR_STALE_JOB,
        RFC_ERR_BUSY,
        RFC_ERR_VERIFY_MISMATCH,
        RFC_ERR_METADATA_POLICY,
        RFC_ERR_IO,
        RFC_ERR_PERMISSION_DENIED,
        RFC_ERR_DESTINATION_EXISTS,
        RFC_ERR_PATH_ALIAS,
        RFC_ERR_DISK_FULL,
        RFC_ERR_FORMAT_DISAGREEMENT,
        RFC_ERR_TRUNCATED_INPUT,
    };
    for (size_t i = 0u; i < sizeof(all_st) / sizeof(all_st[0]); i++) {
        const char *name = rfc_status_name(all_st[i]);
        if (strcmp(s, name) == 0) {
            *out = all_st[i];
            return name;
        }
    }
    return NULL;
}

static int parse_format_by_name(const char *s, rfc_format_id *out) {
    const rfc_capability *cap = rfc_registry_find_name(s);
    if (!cap) {
        return 0;
    }
    *out = cap->id;
    return 1;
}

static int parse_candidate_outcome_name(const char *s,
                                        rfc_candidate_outcome *out) {
    for (int v = 0; v <= 3; v++) {
        rfc_candidate_outcome o = (rfc_candidate_outcome)v;
        if (strcmp(s, rfc_candidate_outcome_name(o)) == 0) {
            *out = o;
            return 1;
        }
    }
    return 0;
}

static int parse_size_outcome_name(const char *s, rfc_size_outcome *out) {
    for (int v = 0; v <= 4; v++) {
        rfc_size_outcome o = (rfc_size_outcome)v;
        if (strcmp(s, rfc_size_outcome_name(o)) == 0) {
            *out = o;
            return 1;
        }
    }
    return 0;
}

static int parse_meta_outcome_name(const char *s, rfc_verify_meta_outcome *out) {
    if (strcmp(s, "none") == 0) {
        *out = (rfc_verify_meta_outcome)0;
        return 1;
    }
    if (strcmp(s, rfc_verify_meta_outcome_name(RFC_VERIFY_META_PRESERVED)) == 0) {
        *out = RFC_VERIFY_META_PRESERVED;
        return 1;
    }
    if (strcmp(s, rfc_verify_meta_outcome_name(
                      RFC_VERIFY_META_STRIPPED_EXPLICITLY)) == 0) {
        *out = RFC_VERIFY_META_STRIPPED_EXPLICITLY;
        return 1;
    }
    if (strcmp(s, rfc_verify_meta_outcome_name(
                      RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED)) == 0) {
        *out = RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED;
        return 1;
    }
    return 0;
}

typedef struct jscan {
    const char *p;
    const char *end;
    int truncated;
} jscan;

static void js_skip_ws(jscan *js) {
    while (js->p < js->end && (*js->p == ' ' || *js->p == '\t' ||
                               *js->p == '\r' || *js->p == '\n')) {
        js->p++;
    }
}

static int js_expect(jscan *js, char ch) {
    js_skip_ws(js);
    if (js->p >= js->end) {
        js->truncated = 1;
        return 0;
    }
    if (*js->p != ch) {
        return 0;
    }
    js->p++;
    return 1;
}

static int js_read_str(jscan *js, char *out, size_t cap) {
    js_skip_ws(js);
    if (js->p >= js->end) {
        js->truncated = 1;
        return 0;
    }
    if (*js->p != '\"') {
        return 0;
    }
    js->p++;
    size_t pos = 0u;
    while (js->p < js->end) {
        unsigned char c = (unsigned char)*js->p++;
        if (c == '\"') {
            if (out && cap > 0u) {
                out[pos < cap ? pos : (cap - 1u)] = '\0';
            }
            return 1;
        }
        if (c < 0x20u) {
            return 0;
        }
        if (c == '\\') {
            if (js->p >= js->end) {
                js->truncated = 1;
                return 0;
            }
            unsigned char e = (unsigned char)*js->p++;
            unsigned char dec = e;
            switch (e) {
                case '\"':
                case '\\':
                case '/':
                    dec = e;
                    break;
                case 'b':
                    dec = '\b';
                    break;
                case 'f':
                    dec = '\f';
                    break;
                case 'n':
                    dec = '\n';
                    break;
                case 'r':
                    dec = '\r';
                    break;
                case 't':
                    dec = '\t';
                    break;
                case 'u': {
                    if (js->end - js->p < 4) {
                        js->truncated = 1;
                        return 0;
                    }
                    uint32_t cp = 0u;
                    for (int k = 0; k < 4; k++) {
                        unsigned char h = (unsigned char)js->p[k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') {
                            cp |= (uint32_t)(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            cp |= (uint32_t)(10 + h - 'a');
                        } else if (h >= 'A' && h <= 'F') {
                            cp |= (uint32_t)(10 + h - 'A');
                        } else {
                            return 0;
                        }
                    }
                    js->p += 4;
                    dec = (cp < 0x80u) ? (unsigned char)cp : '?';
                    break;
                }
                default:
                    return 0;
            }
            if (out && pos + 1u < cap) {
                out[pos++] = (char)dec;
            } else if (out) {
                return 0;
            }
        } else {
            if (out && pos + 1u < cap) {
                out[pos++] = (char)c;
            } else if (out) {
                return 0;
            }
        }
    }
    js->truncated = 1;
    return 0;
}

static int js_read_u64(jscan *js, uint64_t *out) {
    js_skip_ws(js);
    if (js->p >= js->end) {
        js->truncated = 1;
        return 0;
    }
    if (*js->p < '0' || *js->p > '9') {
        return 0;
    }
    if (*js->p == '0' && js->p + 1 < js->end && js->p[1] >= '0' &&
        js->p[1] <= '9') {
        return 0;
    }
    uint64_t v = 0u;
    while (js->p < js->end && *js->p >= '0' && *js->p <= '9') {
        uint64_t d = (uint64_t)(*js->p - '0');
        if (v > (UINT64_MAX - d) / 10ull) {
            return 0;
        }
        v = v * 10ull + d;
        js->p++;
    }
    *out = v;
    return 1;
}

static int js_read_i64(jscan *js, int64_t *out) {
    js_skip_ws(js);
    if (js->p >= js->end) {
        js->truncated = 1;
        return 0;
    }
    int neg = 0;
    if (*js->p == '-') {
        neg = 1;
        js->p++;
    }
    uint64_t mag = 0u;
    if (!js_read_u64(js, &mag)) {
        return 0;
    }
    if (neg) {
        if (mag == (uint64_t)INT64_MAX + 1ull) {
            *out = INT64_MIN;
            return 1;
        }
        if (mag > (uint64_t)INT64_MAX) {
            return 0;
        }
        *out = -(int64_t)mag;
    } else {
        if (mag > (uint64_t)INT64_MAX) {
            return 0;
        }
        *out = (int64_t)mag;
    }
    return 1;
}

static int js_read_bool(jscan *js, int *out) {
    js_skip_ws(js);
    if (js->p >= js->end) {
        js->truncated = 1;
        return 0;
    }
    if (js->end - js->p >= 4 && memcmp(js->p, "true", 4u) == 0) {
        js->p += 4u;
        *out = 1;
        return 1;
    }
    if (js->end - js->p >= 5 && memcmp(js->p, "false", 5u) == 0) {
        js->p += 5u;
        *out = 0;
        return 1;
    }
    if ((js->end - js->p < 4 && memcmp(js->p, "true", (size_t)(js->end - js->p)) == 0) ||
        (js->end - js->p < 5 && memcmp(js->p, "false", (size_t)(js->end - js->p)) == 0)) {
        js->truncated = 1;
    }
    return 0;
}

static int js_expect_key(jscan *js, const char *key, int need_comma) {
    if (need_comma && !js_expect(js, ',')) {
        return 0;
    }
    char kbuf[64];
    if (!js_read_str(js, kbuf, sizeof(kbuf))) {
        return 0;
    }
    if (strcmp(kbuf, key) != 0) {
        return 0;
    }
    return js_expect(js, ':');
}

static rfc_status format_history_line(const rfc_comparison_report *report,
                                      uint64_t sequence,
                                      const uint8_t prev_sha256[32],
                                      uint8_t out_entry_sha256[32],
                                      char **out_line,
                                      size_t *out_len) {
    char src_hex[65];
    char prev_hex[65];
    hex_encode_lower(report->source_sha256, src_hex);
    hex_encode_lower(prev_sha256, prev_hex);

    json_buf b;
    jb_init(&b);
    jb_cstr(&b, "{\"schema\":");
    jb_json_str(&b, RFC_HISTORY_SCHEMA_NAME);
    jb_cstr(&b, ",\"schema_version\":");
    jb_u64(&b, RFC_HISTORY_SCHEMA_VERSION);
    jb_cstr(&b, ",\"sequence\":");
    jb_u64(&b, sequence);
    jb_cstr(&b, ",\"run_id\":");
    jb_json_str(&b, report->run_id);
    jb_cstr(&b, ",\"recorded_utc\":");
    jb_json_str(&b, report->recorded_utc);
    jb_cstr(&b, ",\"run_status\":");
    jb_json_str(&b, rfc_status_name(report->run_status));
    jb_cstr(&b, ",\"ranking_complete\":");
    jb_bool(&b, report->ranking_complete);
    jb_cstr(&b, ",\"source_format\":");
    jb_json_str(&b, format_id_name(report->source_format));
    jb_cstr(&b, ",\"source_file_bytes\":");
    jb_u64(&b, report->source_file_bytes);
    jb_cstr(&b, ",\"raw_frame_bytes\":");
    jb_u64(&b, report->raw_frame_bytes);
    jb_cstr(&b, ",\"source_sha256\":");
    jb_json_str(&b, src_hex);
    jb_cstr(&b, ",\"requested_threads\":");
    jb_u64(&b, report->requested_threads);
    jb_cstr(&b, ",\"effective_threads\":");
    jb_u64(&b, report->effective_threads);
    jb_cstr(&b, ",\"warmup_iterations\":");
    jb_u64(&b, report->warmup_iterations);
    jb_cstr(&b, ",\"measured_iterations\":");
    jb_u64(&b, report->measured_iterations);
    jb_cstr(&b, ",\"metadata_policy\":");
    jb_json_str(&b, rfc_metadata_policy_name(report->metadata_policy));
    jb_cstr(&b, ",\"mixed_provider_configurations\":");
    jb_bool(&b, report->mixed_provider_configurations);

    jb_cstr(&b, ",\"counts\":{\"completed\":");
    jb_u64(&b, report->completed_count);
    jb_cstr(&b, ",\"unsupported\":");
    jb_u64(&b, report->unsupported_count);
    jb_cstr(&b, ",\"failed\":");
    jb_u64(&b, report->failed_count);
    jb_cstr(&b, ",\"cancelled\":");
    jb_u64(&b, report->cancelled_count);
    jb_cstr(&b, "}");

    jb_cstr(&b, ",\"candidates\":[");
    for (size_t i = 0u; i < report->candidate_count; i++) {
        const rfc_compare_candidate_result *c = &report->candidates[i];
        if (i > 0u) {
            jb_raw(&b, ",", 1u);
        }
        jb_cstr(&b, "{\"target_format\":");
        jb_json_str(&b, format_id_name(c->target_format));
        jb_cstr(&b, ",\"outcome\":");
        jb_json_str(&b, rfc_candidate_outcome_name(c->outcome));
        jb_cstr(&b, ",\"status\":");
        jb_json_str(&b, rfc_status_name(c->status));
        jb_cstr(&b, ",\"rank\":");
        jb_u64(&b, c->rank);
        jb_cstr(&b, ",\"has_produced_output\":");
        jb_bool(&b, c->has_produced_output);
        jb_cstr(&b, ",\"size_outcome\":");
        jb_json_str(&b, rfc_size_outcome_name(c->size.outcome));
        jb_cstr(&b, ",\"produced_file_bytes\":");
        jb_u64(&b, c->size.produced_file_bytes);
        jb_cstr(&b, ",\"signed_delta_bytes\":");
        jb_i64(&b, c->size.signed_delta_bytes);
        jb_cstr(&b, ",\"median_wall_ns\":");
        jb_u64(&b, c->median_timings.wall_ns);
        jb_cstr(&b, ",\"codec_version\":");
        jb_json_str(&b, c->build_config.codec_version);
        jb_cstr(&b, ",\"meta_outcome\":");
        jb_json_str(
            &b,
            c->verify_report.meta_outcome
                ? rfc_verify_meta_outcome_name(c->verify_report.meta_outcome)
                : "none");
        jb_cstr(&b, "}");
    }
    jb_cstr(&b, "]");

    jb_cstr(&b, ",\"prev_entry_sha256\":");
    jb_json_str(&b, prev_hex);
    /* Hash canonical object with entry_sha256 field omitted: prefix + "}". */
    jb_cstr(&b, "}");
    if (b.oom || !b.data) {
        jb_free(&b);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_bytes_hash_sha256((const uint8_t *)b.data, b.len, out_entry_sha256);

    b.len--;
    b.data[b.len] = '\0';

    char entry_hex[65];
    hex_encode_lower(out_entry_sha256, entry_hex);
    jb_cstr(&b, ",\"entry_sha256\":");
    jb_json_str(&b, entry_hex);
    jb_cstr(&b, "}\n");
    if (b.oom || !b.data) {
        jb_free(&b);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    *out_line = b.data;
    *out_len = b.len;
    return RFC_OK;
}

static rfc_status parse_and_verify_history_line(
    const char *line,
    size_t line_len,
    uint64_t expected_sequence,
    const uint8_t expected_prev[32],
    rfc_history_entry_summary *out_entry) {
    memset(out_entry, 0, sizeof(*out_entry));
    jscan js;
    js.p = line;
    js.end = line + line_len;
    js.truncated = 0;

    char str_buf[128];
    uint64_t uval = 0u;
    int bval = 0;

    if (!js_expect(&js, '{')) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }

    if (!js_expect_key(&js, "schema", 0) ||
        !js_read_str(&js, str_buf, sizeof(str_buf)) ||
        strcmp(str_buf, RFC_HISTORY_SCHEMA_NAME) != 0) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "schema_version", 1) || !js_read_u64(&js, &uval) ||
        uval != RFC_HISTORY_SCHEMA_VERSION) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "sequence", 1) || !js_read_u64(&js, &uval) ||
        uval != expected_sequence) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->sequence = uval;

    if (!js_expect_key(&js, "run_id", 1) ||
        !js_read_str(&js, out_entry->run_id, sizeof(out_entry->run_id)) ||
        out_entry->run_id[0] == '\0') {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "recorded_utc", 1) ||
        !js_read_str(&js, out_entry->recorded_utc,
                     sizeof(out_entry->recorded_utc)) ||
        out_entry->recorded_utc[0] == '\0') {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "run_status", 1) ||
        !js_read_str(&js, str_buf, sizeof(str_buf)) ||
        !parse_status_by_name(str_buf, &out_entry->run_status)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "ranking_complete", 1) ||
        !js_read_bool(&js, &out_entry->ranking_complete)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "source_format", 1) ||
        !js_read_str(&js, str_buf, sizeof(str_buf)) ||
        !parse_format_by_name(str_buf, &out_entry->source_format)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "source_file_bytes", 1) ||
        !js_read_u64(&js, &out_entry->source_file_bytes)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "raw_frame_bytes", 1) ||
        !js_read_u64(&js, &out_entry->raw_frame_bytes)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "source_sha256", 1) ||
        !js_read_str(&js, str_buf, sizeof(str_buf)) ||
        !hex_decode_lower64(str_buf, strlen(str_buf), out_entry->source_sha256)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "requested_threads", 1) ||
        !js_read_u64(&js, &uval) || uval > UINT32_MAX) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->requested_threads = (uint32_t)uval;
    if (!js_expect_key(&js, "effective_threads", 1) ||
        !js_read_u64(&js, &uval) || uval > UINT32_MAX) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->effective_threads = (uint32_t)uval;
    if (!js_expect_key(&js, "warmup_iterations", 1) ||
        !js_read_u64(&js, &uval) || uval > UINT32_MAX) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->warmup_iterations = (uint32_t)uval;
    if (!js_expect_key(&js, "measured_iterations", 1) ||
        !js_read_u64(&js, &uval) || uval > UINT32_MAX) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->measured_iterations = (uint32_t)uval;
    if (!js_expect_key(&js, "metadata_policy", 1) ||
        !js_read_str(&js, str_buf, sizeof(str_buf)) ||
        rfc_metadata_policy_parse(str_buf, &out_entry->metadata_policy) !=
            RFC_OK) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "mixed_provider_configurations", 1) ||
        !js_read_bool(&js, &out_entry->mixed_provider_configurations)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }

    if (!js_expect_key(&js, "counts", 1) || !js_expect(&js, '{')) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect_key(&js, "completed", 0) || !js_read_u64(&js, &uval) ||
        uval > UINT32_MAX) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->completed_count = (uint32_t)uval;
    if (!js_expect_key(&js, "unsupported", 1) || !js_read_u64(&js, &uval) ||
        uval > UINT32_MAX) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->unsupported_count = (uint32_t)uval;
    if (!js_expect_key(&js, "failed", 1) || !js_read_u64(&js, &uval) ||
        uval > UINT32_MAX) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->failed_count = (uint32_t)uval;
    if (!js_expect_key(&js, "cancelled", 1) || !js_read_u64(&js, &uval) ||
        uval > UINT32_MAX || !js_expect(&js, '}')) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    out_entry->cancelled_count = (uint32_t)uval;

    if (!js_expect_key(&js, "candidates", 1) || !js_expect(&js, '[')) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    js_skip_ws(&js);
    if (js.p < js.end && *js.p == ']') {
        js.p++;
    } else {
        for (;;) {
            if (out_entry->candidate_count >= RFC_COMPARE_MAX_CANDIDATES) {
                return RFC_ERR_VERIFY_MISMATCH;
            }
            rfc_history_candidate_summary *hc =
                &out_entry->candidates[out_entry->candidate_count++];
            if (!js_expect(&js, '{')) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "target_format", 0) ||
                !js_read_str(&js, str_buf, sizeof(str_buf)) ||
                !parse_format_by_name(str_buf, &hc->target_format)) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "outcome", 1) ||
                !js_read_str(&js, str_buf, sizeof(str_buf)) ||
                !parse_candidate_outcome_name(str_buf, &hc->outcome)) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "status", 1) ||
                !js_read_str(&js, str_buf, sizeof(str_buf)) ||
                !parse_status_by_name(str_buf, &hc->status)) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "rank", 1) || !js_read_u64(&js, &uval) ||
                uval > UINT32_MAX) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            hc->rank = (uint32_t)uval;
            if (!js_expect_key(&js, "has_produced_output", 1) ||
                !js_read_bool(&js, &bval)) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            hc->has_produced_output = bval;
            if (!js_expect_key(&js, "size_outcome", 1) ||
                !js_read_str(&js, str_buf, sizeof(str_buf)) ||
                !parse_size_outcome_name(str_buf, &hc->size_outcome)) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "produced_file_bytes", 1) ||
                !js_read_u64(&js, &hc->produced_file_bytes)) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "signed_delta_bytes", 1) ||
                !js_read_i64(&js, &hc->signed_delta_bytes)) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "median_wall_ns", 1) ||
                !js_read_u64(&js, &hc->median_wall_ns)) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "codec_version", 1) ||
                !js_read_str(&js, hc->codec_version,
                             sizeof(hc->codec_version))) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            if (!js_expect_key(&js, "meta_outcome", 1) ||
                !js_read_str(&js, str_buf, sizeof(str_buf)) ||
                !parse_meta_outcome_name(str_buf, &hc->meta_outcome) ||
                !js_expect(&js, '}')) {
                return js.truncated ? RFC_ERR_TRUNCATED_INPUT
                                    : RFC_ERR_VERIFY_MISMATCH;
            }
            js_skip_ws(&js);
            if (js.p >= js.end) {
                return RFC_ERR_TRUNCATED_INPUT;
            }
            if (*js.p == ',') {
                js.p++;
                continue;
            }
            if (*js.p == ']') {
                js.p++;
                break;
            }
            return RFC_ERR_VERIFY_MISMATCH;
        }
    }

    if ((size_t)(out_entry->completed_count + out_entry->unsupported_count +
                 out_entry->failed_count + out_entry->cancelled_count) !=
        out_entry->candidate_count) {
        return RFC_ERR_VERIFY_MISMATCH;
    }

    if (!js_expect_key(&js, "prev_entry_sha256", 1) ||
        !js_read_str(&js, str_buf, sizeof(str_buf)) ||
        !hex_decode_lower64(str_buf, strlen(str_buf),
                            out_entry->prev_entry_sha256)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (memcmp(out_entry->prev_entry_sha256, expected_prev, 32u) != 0) {
        return RFC_ERR_VERIFY_MISMATCH;
    }

    /* Record byte offset right before trailing ,"entry_sha256":"<64 hex>"}. */
    js_skip_ws(&js);
    if (js.p >= js.end) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    if (*js.p != ',') {
        return RFC_ERR_VERIFY_MISMATCH;
    }
    size_t prefix_len = (size_t)(js.p - line);
    if (!js_expect_key(&js, "entry_sha256", 1) ||
        !js_read_str(&js, str_buf, sizeof(str_buf)) ||
        !hex_decode_lower64(str_buf, strlen(str_buf), out_entry->entry_sha256)) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    if (!js_expect(&js, '}')) {
        return js.truncated ? RFC_ERR_TRUNCATED_INPUT : RFC_ERR_VERIFY_MISMATCH;
    }
    js_skip_ws(&js);
    if (js.p != js.end) {
        return RFC_ERR_VERIFY_MISMATCH;
    }

    /* Hash exact prefix + "}" and match entry_sha256. */
    rfc_sha256_ctx ctx;
    rfc_sha256_init(&ctx);
    rfc_sha256_update(&ctx, line, prefix_len);
    rfc_sha256_update(&ctx, "}", 1u);
    uint8_t calc[32];
    rfc_sha256_final(&ctx, calc);
    if (memcmp(calc, out_entry->entry_sha256, 32u) != 0) {
        return RFC_ERR_VERIFY_MISMATCH;
    }
    return RFC_OK;
}

rfc_status rfc_history_verify_bytes(const uint8_t *bytes,
                                    size_t len,
                                    rfc_history_log *out_log) {
    if (out_log) {
        memset(out_log, 0, sizeof(*out_log));
    }
    if (len > 0u && !bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (len == 0u) {
        return RFC_OK;
    }
    if (bytes[len - 1u] != (uint8_t)'\n') {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    size_t cap = 8u;
    size_t count = 0u;
    rfc_history_entry_summary *entries = NULL;
    if (out_log) {
        entries = (rfc_history_entry_summary *)calloc(cap, sizeof(*entries));
        if (!entries) {
            return RFC_ERR_OUT_OF_MEMORY;
        }
    }

    uint8_t expected_prev[32];
    memset(expected_prev, 0, sizeof(expected_prev));
    size_t pos = 0u;
    uint64_t seq = 1u;

    while (pos < len) {
        const uint8_t *nl = (const uint8_t *)memchr(bytes + pos, '\n', len - pos);
        if (!nl) {
            free(entries);
            return RFC_ERR_TRUNCATED_INPUT;
        }
        size_t line_len = (size_t)(nl - (bytes + pos));
        if (line_len == 0u) {
            free(entries);
            return RFC_ERR_VERIFY_MISMATCH;
        }
        rfc_history_entry_summary parsed;
        rfc_status st = parse_and_verify_history_line(
            (const char *)(bytes + pos), line_len, seq, expected_prev, &parsed);
        if (st != RFC_OK) {
            free(entries);
            return st;
        }
        memcpy(expected_prev, parsed.entry_sha256, 32u);
        if (out_log) {
            if (count == cap) {
                size_t ncap = cap * 2u;
                rfc_history_entry_summary *ne =
                    (rfc_history_entry_summary *)realloc(
                        entries, ncap * sizeof(*ne));
                if (!ne) {
                    free(entries);
                    return RFC_ERR_OUT_OF_MEMORY;
                }
                entries = ne;
                cap = ncap;
            }
            entries[count++] = parsed;
        }
        pos = (size_t)(nl - bytes) + 1u;
        seq++;
    }

    if (out_log) {
        out_log->entries = entries;
        out_log->entry_count = count;
    }
    return RFC_OK;
}

rfc_status rfc_history_read_path(const char *path_utf8,
                                 rfc_history_log *out_log) {
    if (out_log) {
        memset(out_log, 0, sizeof(*out_log));
    }
    if (!path_utf8 || !out_log) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint8_t *bytes = NULL;
    size_t len = 0u;
    int exists = 0;
    rfc_status st = rfc_output_internal_read_regular_path(path_utf8, &bytes,
                                                          &len, &exists);
    if (st != RFC_OK) {
        free(bytes);
        return st;
    }
    if (!exists) {
        free(bytes);
        return RFC_ERR_IO;
    }
    st = rfc_history_verify_bytes(bytes, len, out_log);
    free(bytes);
    return st;
}

rfc_status rfc_history_append_path(const char *path_utf8,
                                   const rfc_comparison_report *report) {
    if (!path_utf8 || !report) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_status st = rfc_report_validate(report);
    if (st != RFC_OK) {
        return st;
    }

    uint8_t *existing = NULL;
    size_t existing_len = 0u;
    int exists = 0;
    st = rfc_output_internal_read_regular_path(path_utf8, &existing,
                                               &existing_len, &exists);
    if (st != RFC_OK) {
        free(existing);
        return st;
    }

    uint64_t next_seq = 1u;
    uint8_t prev_sha[32];
    memset(prev_sha, 0, sizeof(prev_sha));

    if (exists && existing_len > 0u) {
        rfc_history_log log;
        memset(&log, 0, sizeof(log));
        st = rfc_history_verify_bytes(existing, existing_len, &log);
        if (st != RFC_OK) {
            rfc_history_log_free(&log);
            free(existing);
            return st;
        }
        if (log.entry_count > 0u) {
            const rfc_history_entry_summary *last =
                &log.entries[log.entry_count - 1u];
            next_seq = last->sequence + 1u;
            memcpy(prev_sha, last->entry_sha256, 32u);
        }
        rfc_history_log_free(&log);
    }

    uint8_t entry_sha[32];
    char *line = NULL;
    size_t line_len = 0u;
    st = format_history_line(report, next_seq, prev_sha, entry_sha, &line,
                             &line_len);
    if (st != RFC_OK) {
        free(existing);
        return st;
    }

    st = rfc_output_internal_replace_regular_path(
        path_utf8, existing, existing_len, (const uint8_t *)line, line_len,
        (int)g_hist_fault_stage, g_hist_fault_status);
    free(line);
    free(existing);
    return st;
}
