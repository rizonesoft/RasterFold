/* File safety, sniffing, path identity, atomic no-replace publication,
 * bounded streaming, retry rewind, and commit/cancel serialization suite
 * (D10 T01 s5). */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif

#include "rasterfold.h"
#include "rfc_codec.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK fs_mutex_t;
typedef CONDITION_VARIABLE fs_cond_t;
typedef HANDLE fs_thread_t;
static void fs_mutex_init(fs_mutex_t *m) { InitializeSRWLock(m); }
static void fs_mutex_destroy(fs_mutex_t *m) { (void)m; }
static void fs_mutex_lock(fs_mutex_t *m) { AcquireSRWLockExclusive(m); }
static void fs_mutex_unlock(fs_mutex_t *m) { ReleaseSRWLockExclusive(m); }
static void fs_cond_init(fs_cond_t *c) { InitializeConditionVariable(c); }
static void fs_cond_destroy(fs_cond_t *c) { (void)c; }
static void fs_cond_wait(fs_cond_t *c, fs_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static void fs_cond_broadcast(fs_cond_t *c) { WakeAllConditionVariable(c); }
typedef DWORD fs_tid_t;
static fs_tid_t fs_tid_self(void) { return GetCurrentThreadId(); }
static int fs_tid_eq(fs_tid_t a, fs_tid_t b) { return a == b; }
typedef DWORD (WINAPI *fs_thread_fn)(void *);
#define FS_THREAD_RET DWORD WINAPI
#define FS_THREAD_RETURN return 0u
static int fs_thread_start(fs_thread_t *t, fs_thread_fn fn, void *arg) {
    HANDLE h = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (!h) {
        return 0;
    }
    *t = h;
    return 1;
}
static void fs_thread_join(fs_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
typedef pthread_mutex_t fs_mutex_t;
typedef pthread_cond_t fs_cond_t;
typedef pthread_t fs_thread_t;
static void fs_mutex_init(fs_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void fs_mutex_destroy(fs_mutex_t *m) { pthread_mutex_destroy(m); }
static void fs_mutex_lock(fs_mutex_t *m) { pthread_mutex_lock(m); }
static void fs_mutex_unlock(fs_mutex_t *m) { pthread_mutex_unlock(m); }
static void fs_cond_init(fs_cond_t *c) { pthread_cond_init(c, NULL); }
static void fs_cond_destroy(fs_cond_t *c) { pthread_cond_destroy(c); }
static void fs_cond_wait(fs_cond_t *c, fs_mutex_t *m) {
    pthread_cond_wait(c, m);
}
static void fs_cond_broadcast(fs_cond_t *c) { pthread_cond_broadcast(c); }
typedef pthread_t fs_tid_t;
static fs_tid_t fs_tid_self(void) { return pthread_self(); }
static int fs_tid_eq(fs_tid_t a, fs_tid_t b) { return pthread_equal(a, b) != 0; }
typedef void *(*fs_thread_fn)(void *);
#define FS_THREAD_RET void *
#define FS_THREAD_RETURN return NULL
static int fs_thread_start(fs_thread_t *t, fs_thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0;
}
static void fs_thread_join(fs_thread_t t) {
    pthread_join(t, NULL);
}
#endif

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond); \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ----------------------------------------------------------------
 * Portable OS filesystem helpers for standalone C tests
 * ---------------------------------------------------------------- */

static int make_temp_dir(char *out_dir, size_t cap, const char *suffix) {
#ifdef _WIN32
    wchar_t wbase[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, wbase);
    if (n == 0 || n >= MAX_PATH - 64) {
        return 0;
    }
    wchar_t wsub[128];
    swprintf(wsub, 128, L"rfc_fs_%lu_%S",
             (unsigned long)GetCurrentProcessId(), suffix ? suffix : "t");
    wchar_t wfull[MAX_PATH];
    _snwprintf(wfull, MAX_PATH, L"%ls%ls", wbase, wsub);
    wfull[MAX_PATH - 1] = L'\0';
    if (!CreateDirectoryW(wfull, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, wfull, -1, out_dir, (int)cap,
                                   NULL, NULL);
    return need > 0 && (size_t)need <= cap;
#else
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) {
        base = "/tmp";
    }
    char tmpl[512];
    int wn = snprintf(tmpl, sizeof(tmpl), "%s/rfc_fs_%s_XXXXXX",
                      base, suffix ? suffix : "t");
    if (wn <= 0 || (size_t)wn >= sizeof(tmpl)) {
        return 0;
    }
    char *made = mkdtemp(tmpl);
    if (!made || strlen(made) + 1u > cap) {
        return 0;
    }
    strcpy(out_dir, made);
    return 1;
#endif
}

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    int n = snprintf(out, cap, "%s%c%s", dir, sep, name);
    CHECK(n > 0 && (size_t)n < cap);
}

#ifdef _WIN32
static int utf8_to_wide(const char *s, wchar_t *out, size_t wcap) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, (int)wcap);
    return n > 0;
}

static int file_exists_os(const char *path) {
    wchar_t wpath[1024];
    if (!utf8_to_wide(path, wpath, 1024u)) {
        return 0;
    }
    DWORD attr = GetFileAttributesW(wpath);
    return attr != INVALID_FILE_ATTRIBUTES;
}

static void remove_file_os(const char *path) {
    wchar_t wpath[1024];
    if (utf8_to_wide(path, wpath, 1024u)) {
        SetFileAttributesW(wpath, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(wpath);
    }
}

static void remove_dir_os(const char *path) {
    wchar_t wpath[1024];
    if (utf8_to_wide(path, wpath, 1024u)) {
        SetFileAttributesW(wpath, FILE_ATTRIBUTE_NORMAL);
        RemoveDirectoryW(wpath);
    }
}
#else
static int file_exists_os(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static void remove_file_os(const char *path) {
    unlink(path);
}

static void remove_dir_os(const char *path) {
    rmdir(path);
}
#endif

static void write_file_exact(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (len > 0u) {
        CHECK(fwrite(data, 1u, len, fp) == len);
    }
    CHECK(fclose(fp) == 0);
}

static void read_file_exact(const char *path, uint8_t **out_bytes, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    CHECK(fseek(fp, 0, SEEK_END) == 0);
    long sz = ftell(fp);
    CHECK(sz >= 0);
    CHECK(fseek(fp, 0, SEEK_SET) == 0);
    size_t len = (size_t)sz;
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1u);
    CHECK(buf != NULL);
    if (len > 0u) {
        CHECK(fread(buf, 1u, len, fp) == len);
    }
    CHECK(fclose(fp) == 0);
    *out_bytes = buf;
    *out_len = len;
}

static int dir_has_prefix(const char *dir, const char *prefix) {
    size_t plen = strlen(prefix);
#ifdef _WIN32
    char pattern[1024];
    join_path(pattern, sizeof(pattern), dir, "*");
    wchar_t wp[1024];
    if (!utf8_to_wide(pattern, wp, 1024u)) {
        return 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int found = 0;
    do {
        char name[512];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name,
                                sizeof(name), NULL, NULL) > 0) {
            if (strncmp(name, prefix, plen) == 0) {
                found = 1;
                break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
#else
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, plen) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
#endif
}

/* ----------------------------------------------------------------
 * Test-only RasterFold & BMP session adapters (D10 T02 wires real adapters)
 * ---------------------------------------------------------------- */

typedef struct fs_test_sess {
    const rfc_exec_context *exec;
} fs_test_sess;

static rfc_status fs_sess_open(const rfc_exec_context *exec, void **out_sess) {
    if (!exec || !exec->allocator || !out_sess) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    fs_test_sess *s = (fs_test_sess *)exec->allocator->alloc(
        exec->allocator->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    s->exec = exec;
    *out_sess = s;
    return RFC_OK;
}

static void fs_sess_close(void *raw) {
    fs_test_sess *s = (fs_test_sess *)raw;
    if (s) {
        const rfc_allocator *a = s->exec->allocator;
        a->free(a->ctx, s);
    }
}

static void fs_sess_cancel(void *raw) {
    (void)raw;
}

static void fs_sess_free_encoded(void *raw, rfc_encoded *encoded) {
    (void)raw;
    rfc_encoded_free(encoded);
}

static rfc_status rf_sess_probe(void *raw,
                                const rfc_exec_context *exec,
                                const uint8_t *bytes,
                                size_t size,
                                rfc_image_probe *out_probe) {
    (void)raw;
    (void)exec;
    if (!bytes || size < 32u || !out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (bytes[0] != 'R' || bytes[1] != 'F' || bytes[2] != 'I' ||
        (bytes[3] != 2u && bytes[3] != 3u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t w = rd32le(bytes + 8u);
    uint32_t h = rd32le(bytes + 12u);
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.stride_bytes = (size_t)w * 3u;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)w * (uint64_t)h;
    out_probe->total_decoded_bytes = out_probe->total_pixels * 3ull;
    out_probe->container_meta.orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

static rfc_status rf_sess_encode(void *raw,
                                 const rfc_exec_context *exec,
                                 const rfc_frame *frames,
                                 size_t frame_count,
                                 const rfc_metadata *meta,
                                 const rfc_setting_value *settings,
                                 size_t setting_count,
                                 rfc_encoded *out_encoded) {
    (void)raw;
    (void)meta;
    (void)settings;
    (void)setting_count;
    if (!frames || frame_count != 1u ||
        frames[0].layout.format != RFC_SAMPLE_RGB8 || !out_encoded) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    rf_pixels px;
    px.width = frames[0].layout.width;
    px.height = frames[0].layout.height;
    px.stride = frames[0].layout.stride_bytes;
    px.layout = RF_LAYOUT_RGB8;
    px.data = frames[0].data;
    px.size = frames[0].data_len;

    rf_options opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = (uint32_t)sizeof(opt);
    opt.threads = rfc_exec_threads(exec);
    if (opt.threads > 9u) {
        opt.threads = 9u;
    }

    rf_buffer buf = {NULL, 0u};
    rf_error err = {0, NULL};
    if (rf_encode_image(&px, &opt, &buf, &err) != RF_OK) {
        rf_free(buf.data);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint8_t *copy = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx, buf.size);
    if (!copy) {
        rf_free(buf.data);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, buf.data, buf.size);
    size_t sz = buf.size;
    rf_free(buf.data);

    out_encoded->data = copy;
    out_encoded->len = sz;
    out_encoded->allocator = exec->allocator;
    return RFC_OK;
}

static rfc_status rf_sess_decode(void *raw,
                                 const rfc_exec_context *exec,
                                 const uint8_t *bytes,
                                 size_t size,
                                 const rfc_setting_value *settings,
                                 size_t setting_count,
                                 rfc_frame **out_frames,
                                 size_t *out_frame_count,
                                 rfc_metadata *out_meta) {
    (void)raw;
    (void)settings;
    (void)setting_count;
    rf_options opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = (uint32_t)sizeof(opt);
    opt.threads = rfc_exec_threads(exec);
    if (opt.threads > 9u) {
        opt.threads = 9u;
    }

    rf_pixels px;
    memset(&px, 0, sizeof(px));
    rf_error err = {0, NULL};
    if (rf_decode_image(bytes, size, &opt, &px, &err) != RF_OK) {
        rf_free(px.data);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_frame *arr = (rfc_frame *)exec->allocator->alloc(exec->allocator->ctx,
                                                         sizeof(rfc_frame));
    if (!arr) {
        rf_free(px.data);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, sizeof(*arr));
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, px.width, px.height,
                      (size_t)px.width * 3u, 8u};
    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0}};
    rfc_status st = rfc_exec_alloc_frame(exec, &arr[0], &lay, &meta);
    if (st != RFC_OK) {
        rf_free(px.data);
        exec->allocator->free(exec->allocator->ctx, arr);
        return st;
    }
    for (uint32_t y = 0; y < px.height; y++) {
        memcpy(arr[0].data + (size_t)y * arr[0].layout.stride_bytes,
               px.data + (size_t)y * px.stride, (size_t)px.width * 3u);
    }
    rf_free(px.data);

    *out_frames = arr;
    *out_frame_count = 1u;
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

static rfc_status bmp_parse_header(const uint8_t *bytes,
                                   size_t size,
                                   uint32_t *out_w,
                                   uint32_t *out_h,
                                   int *out_top_down,
                                   uint32_t *out_off) {
    if (!bytes || size < 54u || bytes[0] != 'B' || bytes[1] != 'M') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t off = rd32le(bytes + 10u);
    uint32_t dib = rd32le(bytes + 14u);
    int32_t sw = (int32_t)rd32le(bytes + 18u);
    int32_t sh = (int32_t)rd32le(bytes + 22u);
    uint16_t planes = rd16le(bytes + 26u);
    uint16_t bpp = rd16le(bytes + 28u);
    uint32_t comp = rd32le(bytes + 30u);
    if (dib < 40u || sw <= 0 || sh == 0 || planes != 1u || bpp != 24u || comp != 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int top_down = (sh < 0) ? 1 : 0;
    uint32_t w = (uint32_t)sw;
    uint32_t h = (uint32_t)(sh < 0 ? -sh : sh);
    if (off < 54u || off > size) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    uint64_t row_stride = (((uint64_t)w * 3ull) + 3ull) & ~3ull;
    uint64_t need = (uint64_t)off + row_stride * (uint64_t)h;
    if (need > (uint64_t)size) {
        return RFC_ERR_TRUNCATED_INPUT;
    }
    *out_w = w;
    *out_h = h;
    *out_top_down = top_down;
    *out_off = off;
    return RFC_OK;
}

static rfc_status bmp_sess_probe(void *raw,
                                 const rfc_exec_context *exec,
                                 const uint8_t *bytes,
                                 size_t size,
                                 rfc_image_probe *out_probe) {
    (void)raw;
    (void)exec;
    uint32_t w = 0u, h = 0u, off = 0u;
    int top_down = 0;
    rfc_status st = bmp_parse_header(bytes, size, &w, &h, &top_down, &off);
    if (st != RFC_OK) {
        return st;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.stride_bytes = (size_t)w * 3u;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)w * (uint64_t)h;
    out_probe->total_decoded_bytes = out_probe->total_pixels * 3ull;
    out_probe->container_meta.orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

static rfc_status bmp_sess_encode(void *raw,
                                  const rfc_exec_context *exec,
                                  const rfc_frame *frames,
                                  size_t frame_count,
                                  const rfc_metadata *meta,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_encoded *out_encoded) {
    (void)raw;
    (void)meta;
    (void)settings;
    (void)setting_count;
    if (!frames || frame_count != 1u ||
        frames[0].layout.format != RFC_SAMPLE_RGB8 || !out_encoded) {
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    uint32_t w = frames[0].layout.width;
    uint32_t h = frames[0].layout.height;
    size_t row_stride = (((size_t)w * 3u) + 3u) & ~(size_t)3u;
    size_t pix_bytes = row_stride * (size_t)h;
    size_t total = 54u + pix_bytes;
    uint8_t *buf = (uint8_t *)exec->allocator->alloc(exec->allocator->ctx, total);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(buf, 0, total);
    buf[0] = 'B';
    buf[1] = 'M';
    wr32le(buf + 2u, (uint32_t)total);
    wr32le(buf + 10u, 54u);
    wr32le(buf + 14u, 40u);
    wr32le(buf + 18u, w);
    wr32le(buf + 22u, h);
    wr16le(buf + 26u, 1u);
    wr16le(buf + 28u, 24u);
    wr32le(buf + 34u, (uint32_t)pix_bytes);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src_row = frames[0].data + (size_t)y * frames[0].layout.stride_bytes;
        uint8_t *dst_row = buf + 54u + (size_t)(h - 1u - y) * row_stride;
        for (uint32_t x = 0; x < w; x++) {
            dst_row[(size_t)x * 3u + 0u] = src_row[(size_t)x * 3u + 2u];
            dst_row[(size_t)x * 3u + 1u] = src_row[(size_t)x * 3u + 1u];
            dst_row[(size_t)x * 3u + 2u] = src_row[(size_t)x * 3u + 0u];
        }
    }
    out_encoded->data = buf;
    out_encoded->len = total;
    out_encoded->allocator = exec->allocator;
    return RFC_OK;
}

static rfc_status bmp_sess_decode(void *raw,
                                  const rfc_exec_context *exec,
                                  const uint8_t *bytes,
                                  size_t size,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_frame **out_frames,
                                  size_t *out_frame_count,
                                  rfc_metadata *out_meta) {
    (void)raw;
    (void)settings;
    (void)setting_count;
    uint32_t w = 0u, h = 0u, off = 0u;
    int top_down = 0;
    rfc_status st = bmp_parse_header(bytes, size, &w, &h, &top_down, &off);
    if (st != RFC_OK) {
        return st;
    }
    rfc_frame *arr = (rfc_frame *)exec->allocator->alloc(exec->allocator->ctx,
                                                         sizeof(rfc_frame));
    if (!arr) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, sizeof(*arr));
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, w, h, (size_t)w * 3u, 8u};
    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0}};
    st = rfc_exec_alloc_frame(exec, &arr[0], &lay, &meta);
    if (st != RFC_OK) {
        exec->allocator->free(exec->allocator->ctx, arr);
        return st;
    }
    size_t row_stride = (((size_t)w * 3u) + 3u) & ~(size_t)3u;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t sy = top_down ? y : (h - 1u - y);
        const uint8_t *src_row = bytes + off + (size_t)sy * row_stride;
        uint8_t *dst_row = arr[0].data + (size_t)y * arr[0].layout.stride_bytes;
        for (uint32_t x = 0; x < w; x++) {
            dst_row[(size_t)x * 3u + 0u] = src_row[(size_t)x * 3u + 2u];
            dst_row[(size_t)x * 3u + 1u] = src_row[(size_t)x * 3u + 1u];
            dst_row[(size_t)x * 3u + 2u] = src_row[(size_t)x * 3u + 0u];
        }
    }
    *out_frames = arr;
    *out_frame_count = 1u;
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

static rfc_session_adapter g_rf_test_adp = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_RASTERFOLD,
    fs_sess_open,
    fs_sess_close,
    rf_sess_probe,
    rf_sess_encode,
    rf_sess_decode,
    fs_sess_cancel,
    fs_sess_free_encoded,
};

static rfc_session_adapter g_bmp_test_adp = {
    RFC_CODEC_ABI_VERSION,
    RFC_FORMAT_BMP,
    fs_sess_open,
    fs_sess_close,
    bmp_sess_probe,
    bmp_sess_encode,
    bmp_sess_decode,
    fs_sess_cancel,
    fs_sess_free_encoded,
};

static void build_test_bmp(uint8_t *out, size_t *out_len, uint32_t w, uint32_t h,
                           uint8_t seed) {
    size_t row_stride = (((size_t)w * 3u) + 3u) & ~(size_t)3u;
    size_t pix_bytes = row_stride * (size_t)h;
    size_t total = 54u + pix_bytes;
    memset(out, 0, total);
    out[0] = 'B';
    out[1] = 'M';
    wr32le(out + 2u, (uint32_t)total);
    wr32le(out + 10u, 54u);
    wr32le(out + 14u, 40u);
    wr32le(out + 18u, w);
    wr32le(out + 22u, h);
    wr16le(out + 26u, 1u);
    wr16le(out + 28u, 24u);
    wr32le(out + 34u, (uint32_t)pix_bytes);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = out + 54u + (size_t)(h - 1u - y) * row_stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 29u + y * 13u + seed) & 0xffu);
            uint8_t g = (uint8_t)((x * 11u + y * 37u + seed * 3u) & 0xffu);
            uint8_t b = (uint8_t)((x * 43u + y * 7u + seed * 5u) & 0xffu);
            row[(size_t)x * 3u + 0u] = b;
            row[(size_t)x * 3u + 1u] = g;
            row[(size_t)x * 3u + 2u] = r;
        }
    }
    *out_len = total;
}

static void build_test_fold(uint8_t **out_bytes, size_t *out_len,
                            uint32_t w, uint32_t h, uint8_t seed) {
    size_t pix_sz = (size_t)w * (size_t)h * 3u;
    uint8_t *rgb = (uint8_t *)malloc(pix_sz);
    CHECK(rgb != NULL);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t off = ((size_t)y * (size_t)w + (size_t)x) * 3u;
            rgb[off + 0u] = (uint8_t)((x * 23u + y * 19u + seed) & 0xffu);
            rgb[off + 1u] = (uint8_t)((x * 31u + y * 17u + seed * 3u) & 0xffu);
            rgb[off + 2u] = (uint8_t)((x * 7u + y * 41u + seed * 7u) & 0xffu);
        }
    }
    rf_pixels px = {w, h, (size_t)w * 3u, RF_LAYOUT_RGB8, rgb, pix_sz};
    rf_buffer buf = {NULL, 0u};
    rf_error err = {0, NULL};
    CHECK(rf_encode_image(&px, NULL, &buf, &err) == RF_OK);
    uint8_t *copy = (uint8_t *)malloc(buf.size);
    CHECK(copy != NULL);
    memcpy(copy, buf.data, buf.size);
    *out_bytes = copy;
    *out_len = buf.size;
    rf_free(buf.data);
    free(rgb);
}

/* ----------------------------------------------------------------
 * Counting rfc_io_reader and rfc_io_publisher test doubles
 * ---------------------------------------------------------------- */

typedef struct mem_reader_ctx {
    fs_mutex_t lock;
    uint8_t *buf;
    size_t len;
    size_t cap;
    size_t pos;
    int know_size;
    uint64_t declared_size;
    size_t chunk_cap;
    rfc_status force_read_status;
    rfc_status force_seek_status;
    uint32_t size_calls;
    uint32_t read_calls;
    uint32_t seek_calls;
    uint32_t identity_calls;
    uint32_t close_calls;
    int has_identity;
    rfc_file_identity id;
    fs_tid_t submit_tid;
    int block_after_first_read;
    int read_entered;
    int release_read;
    fs_cond_t cond;
} mem_reader_ctx;

static void mem_reader_init(mem_reader_ctx *c, const uint8_t *data, size_t len) {
    memset(c, 0, sizeof(*c));
    fs_mutex_init(&c->lock);
    fs_cond_init(&c->cond);
    c->submit_tid = fs_tid_self();
    c->cap = len ? len : 64u;
    c->buf = (uint8_t *)malloc(c->cap);
    CHECK(c->buf != NULL);
    if (len > 0u) {
        memcpy(c->buf, data, len);
    }
    c->len = len;
    c->know_size = 1;
    c->declared_size = (uint64_t)len;
    c->chunk_cap = 4096u;
}

static void mem_reader_destroy(mem_reader_ctx *c) {
    free(c->buf);
    c->buf = NULL;
    fs_cond_destroy(&c->cond);
    fs_mutex_destroy(&c->lock);
}

static rfc_status mem_reader_size(void *raw, int *out_known, uint64_t *out_size) {
    mem_reader_ctx *c = (mem_reader_ctx *)raw;
    fs_mutex_lock(&c->lock);
    c->size_calls++;
    *out_known = c->know_size;
    *out_size = c->declared_size;
    fs_mutex_unlock(&c->lock);
    return RFC_OK;
}

static rfc_status mem_reader_read(void *raw, uint8_t *buf, size_t max_bytes,
                                  size_t *out_read) {
    mem_reader_ctx *c = (mem_reader_ctx *)raw;
    fs_mutex_lock(&c->lock);
    c->read_calls++;
    if (c->block_after_first_read && c->pos > 0u &&
        !fs_tid_eq(fs_tid_self(), c->submit_tid)) {
        c->read_entered = 1;
        fs_cond_broadcast(&c->cond);
        while (!c->release_read) {
            fs_cond_wait(&c->cond, &c->lock);
        }
    }
    if (c->force_read_status != RFC_OK) {
        rfc_status st = c->force_read_status;
        fs_mutex_unlock(&c->lock);
        return st;
    }
    size_t avail = (c->pos < c->len) ? (c->len - c->pos) : 0u;
    size_t n = max_bytes;
    if (c->chunk_cap > 0u && n > c->chunk_cap) {
        n = c->chunk_cap;
    }
    if (n > avail) {
        n = avail;
    }
    if (n > 0u) {
        memcpy(buf, c->buf + c->pos, n);
        c->pos += n;
    }
    *out_read = n;
    fs_mutex_unlock(&c->lock);
    return RFC_OK;
}

static rfc_status mem_reader_seek(void *raw, uint64_t offset) {
    mem_reader_ctx *c = (mem_reader_ctx *)raw;
    fs_mutex_lock(&c->lock);
    c->seek_calls++;
    if (c->force_seek_status != RFC_OK) {
        rfc_status st = c->force_seek_status;
        fs_mutex_unlock(&c->lock);
        return st;
    }
    if (offset > (uint64_t)c->len) {
        fs_mutex_unlock(&c->lock);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    c->pos = (size_t)offset;
    fs_mutex_unlock(&c->lock);
    return RFC_OK;
}

static rfc_status mem_reader_identity(void *raw, rfc_file_identity *out_id) {
    mem_reader_ctx *c = (mem_reader_ctx *)raw;
    fs_mutex_lock(&c->lock);
    c->identity_calls++;
    if (c->has_identity) {
        *out_id = c->id;
    } else {
        memset(out_id, 0, sizeof(*out_id));
    }
    fs_mutex_unlock(&c->lock);
    return RFC_OK;
}

static void mem_reader_close(void *raw) {
    mem_reader_ctx *c = (mem_reader_ctx *)raw;
    fs_mutex_lock(&c->lock);
    c->close_calls++;
    fs_mutex_unlock(&c->lock);
}

typedef struct mem_pub_stage {
    uint64_t attempt;
    uint8_t *buf;
    size_t len;
    size_t cap;
    int flushed;
    int committed;
    int aborted;
} mem_pub_stage;

typedef struct mem_pub_ctx {
    fs_mutex_t lock;
    uint32_t begin_calls;
    uint32_t write_calls;
    uint32_t flush_calls;
    uint32_t commit_calls;
    uint32_t abort_calls;
    uint32_t release_calls;
    rfc_status fail_write_status;
    rfc_status fail_flush_status;
    rfc_status fail_commit_once_status;
    rfc_status fail_commit_always_status;
    rfc_status fail_abort_status;
    int commit_not_cleaned;
    rfc_job *cancel_job_on_flush;
    uint64_t last_attempt;
    uint8_t *published_bytes;
    size_t published_len;
    int published;
    uint32_t live_handles;
} mem_pub_ctx;

static void mem_pub_init(mem_pub_ctx *p) {
    memset(p, 0, sizeof(*p));
    fs_mutex_init(&p->lock);
}

static void mem_pub_destroy(mem_pub_ctx *p) {
    free(p->published_bytes);
    p->published_bytes = NULL;
    fs_mutex_destroy(&p->lock);
}

static rfc_status mem_pub_begin(void *raw, rfc_job_id job_id,
                                uint64_t attempt_number, uint64_t expected_bytes,
                                const rfc_file_identity *source_id,
                                void **out_handle) {
    (void)job_id;
    (void)source_id;
    mem_pub_ctx *p = (mem_pub_ctx *)raw;
    mem_pub_stage *st = (mem_pub_stage *)calloc(1u, sizeof(*st));
    if (!st) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    st->attempt = attempt_number;
    st->cap = expected_bytes ? (size_t)expected_bytes : 256u;
    st->buf = (uint8_t *)malloc(st->cap);
    if (!st->buf) {
        free(st);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    fs_mutex_lock(&p->lock);
    p->begin_calls++;
    p->last_attempt = attempt_number;
    p->live_handles++;
    fs_mutex_unlock(&p->lock);
    *out_handle = st;
    return RFC_OK;
}

static rfc_status mem_pub_write(void *raw, void *handle,
                                const uint8_t *data, size_t len) {
    mem_pub_ctx *p = (mem_pub_ctx *)raw;
    mem_pub_stage *st = (mem_pub_stage *)handle;
    fs_mutex_lock(&p->lock);
    p->write_calls++;
    rfc_status fst = p->fail_write_status;
    fs_mutex_unlock(&p->lock);
    if (fst != RFC_OK) {
        return fst;
    }
    if (len > 0u) {
        if (st->len + len > st->cap) {
            size_t nc = st->cap ? st->cap : 256u;
            while (nc < st->len + len) {
                nc *= 2u;
            }
            uint8_t *nb = (uint8_t *)realloc(st->buf, nc);
            if (!nb) {
                return RFC_ERR_OUT_OF_MEMORY;
            }
            st->buf = nb;
            st->cap = nc;
        }
        memcpy(st->buf + st->len, data, len);
        st->len += len;
    }
    return RFC_OK;
}

static rfc_status mem_pub_flush(void *raw, void *handle) {
    mem_pub_ctx *p = (mem_pub_ctx *)raw;
    mem_pub_stage *st = (mem_pub_stage *)handle;
    fs_mutex_lock(&p->lock);
    p->flush_calls++;
    rfc_status fst = p->fail_flush_status;
    rfc_job *cj = p->cancel_job_on_flush;
    fs_mutex_unlock(&p->lock);
    if (cj) {
        (void)rfc_job_cancel(cj);
    }
    if (fst != RFC_OK) {
        return fst;
    }
    st->flushed = 1;
    return RFC_OK;
}

static rfc_status mem_pub_commit(void *raw, void *handle,
                                 int *out_temp_cleaned_up) {
    mem_pub_ctx *p = (mem_pub_ctx *)raw;
    mem_pub_stage *st = (mem_pub_stage *)handle;
    fs_mutex_lock(&p->lock);
    if (out_temp_cleaned_up) {
        *out_temp_cleaned_up = p->commit_not_cleaned ? 0 : 1;
    }
    p->commit_calls++;
    if (p->fail_commit_always_status != RFC_OK) {
        rfc_status fst = p->fail_commit_always_status;
        fs_mutex_unlock(&p->lock);
        return fst;
    }
    if (p->fail_commit_once_status != RFC_OK) {
        rfc_status fst = p->fail_commit_once_status;
        p->fail_commit_once_status = RFC_OK;
        fs_mutex_unlock(&p->lock);
        return fst;
    }
    free(p->published_bytes);
    p->published_bytes = (uint8_t *)malloc(st->len ? st->len : 1u);
    if (!p->published_bytes) {
        fs_mutex_unlock(&p->lock);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (st->len > 0u) {
        memcpy(p->published_bytes, st->buf, st->len);
    }
    p->published_len = st->len;
    p->published = 1;
    if (p->live_handles > 0u) {
        p->live_handles--;
    }
    fs_mutex_unlock(&p->lock);
    st->committed = 1;
    free(st->buf);
    free(st);
    return RFC_OK;
}

static rfc_status mem_pub_abort(void *raw, void *handle) {
    mem_pub_ctx *p = (mem_pub_ctx *)raw;
    mem_pub_stage *st = (mem_pub_stage *)handle;
    fs_mutex_lock(&p->lock);
    p->abort_calls++;
    rfc_status ast = p->fail_abort_status;
    if (p->live_handles > 0u) {
        p->live_handles--;
    }
    fs_mutex_unlock(&p->lock);
    if (st) {
        st->aborted = 1;
        free(st->buf);
        free(st);
    }
    return ast;
}

static void mem_pub_release(void *raw) {
    mem_pub_ctx *p = (mem_pub_ctx *)raw;
    fs_mutex_lock(&p->lock);
    p->release_calls++;
    fs_mutex_unlock(&p->lock);
}

static rfc_io_reader make_mem_reader(mem_reader_ctx *c) {
    rfc_io_reader r;
    memset(&r, 0, sizeof(r));
    r.ctx = c;
    r.size_fn = mem_reader_size;
    r.read_fn = mem_reader_read;
    r.seek_fn = mem_reader_seek;
    r.identity_fn = mem_reader_identity;
    r.close_fn = mem_reader_close;
    return r;
}

static rfc_io_publisher make_mem_publisher(mem_pub_ctx *p) {
    rfc_io_publisher pub;
    memset(&pub, 0, sizeof(pub));
    pub.ctx = p;
    pub.begin_stage = mem_pub_begin;
    pub.write_stage = mem_pub_write;
    pub.flush_stage = mem_pub_flush;
    pub.commit_stage = mem_pub_commit;
    pub.abort_stage = mem_pub_abort;
    pub.release_ctx = mem_pub_release;
    return pub;
}

/* ----------------------------------------------------------------
 * 1. Format sniffing across all 19 formats, active/script refusal,
 *    and content-vs-extension disagreement tests.
 * ---------------------------------------------------------------- */

static void assert_sniff_eq(const uint8_t *bytes, size_t len, const char *name,
                            rfc_format_id want_content,
                            rfc_format_id want_ext,
                            int want_disagree) {
    rfc_sniff_result sr;
    rfc_status st = rfc_input_sniff_bytes(bytes, len, name, &sr);
    CHECK(st == RFC_OK);
    CHECK(sr.content_format == want_content);
    if (want_ext != (rfc_format_id)0) {
        CHECK(sr.extension_present == 1);
        CHECK(sr.extension_known == 1);
        CHECK(sr.extension_format == want_ext);
    }
    CHECK(sr.disagreement == want_disagree);
}

static void test_sniffing(void) {
    /* 1. RASTERFOLD */
    {
        uint8_t *fold = NULL;
        size_t flen = 0u;
        build_test_fold(&fold, &flen, 4u, 4u, 11u);
        assert_sniff_eq(fold, flen, "pic.fold", RFC_FORMAT_RASTERFOLD,
                        RFC_FORMAT_RASTERFOLD, 0);
        /* Extension disagreement: .fold bytes named fake.png */
        assert_sniff_eq(fold, flen, "fake.png", RFC_FORMAT_RASTERFOLD,
                        RFC_FORMAT_PNG, 1);
        free(fold);
    }

    /* 2. BMP */
    {
        uint8_t bmp[256];
        size_t blen = 0u;
        build_test_bmp(bmp, &blen, 4u, 4u, 5u);
        assert_sniff_eq(bmp, blen, "img.bmp", RFC_FORMAT_BMP, RFC_FORMAT_BMP, 0);
        assert_sniff_eq(bmp, blen, "wrong.jpg", RFC_FORMAT_BMP, RFC_FORMAT_JPEG, 1);
    }

    /* 3. PNG vs 4. APNG (acTL chunk past first 4 KiB before IDAT) */
    {
        uint8_t png[48];
        memset(png, 0, sizeof(png));
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        memcpy(png, sig, 8u);
        wr32be(png + 8u, 13u);
        memcpy(png + 12u, "IHDR", 4u);
        wr32be(png + 16u, 4u);
        wr32be(png + 20u, 4u);
        wr32be(png + 33u, 4u);
        memcpy(png + 37u, "IDAT", 4u);
        assert_sniff_eq(png, sizeof(png), "a.png", RFC_FORMAT_PNG, RFC_FORMAT_PNG, 0);

        size_t large_txt = 6000u;
        size_t apng_sz = 8u + 25u + (12u + large_txt) + 20u + 16u;
        uint8_t *apng = (uint8_t *)calloc(1u, apng_sz);
        CHECK(apng != NULL);
        memcpy(apng, sig, 8u);
        size_t pos = 8u;
        wr32be(apng + pos, 13u); memcpy(apng + pos + 4u, "IHDR", 4u);
        pos += 12u + 13u;
        wr32be(apng + pos, (uint32_t)large_txt); memcpy(apng + pos + 4u, "iTXt", 4u);
        pos += 12u + large_txt;
        wr32be(apng + pos, 8u); memcpy(apng + pos + 4u, "acTL", 4u);
        wr32be(apng + pos + 8u, 2u);
        pos += 12u + 8u;
        wr32be(apng + pos, 4u); memcpy(apng + pos + 4u, "IDAT", 4u);
        assert_sniff_eq(apng, apng_sz, "anim.apng", RFC_FORMAT_APNG,
                        RFC_FORMAT_APNG, 0);
        free(apng);
    }

    /* 5. PNM & 6. PAM */
    {
        const uint8_t ppm[] = "P6\n2 2\n255\n123456123456";
        assert_sniff_eq(ppm, sizeof(ppm) - 1u, "a.ppm", RFC_FORMAT_PNM,
                        RFC_FORMAT_PNM, 0);
        const uint8_t pam[] =
            "P7\nWIDTH 2\nHEIGHT 2\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n"
            "123456123456";
        assert_sniff_eq(pam, sizeof(pam) - 1u, "a.pam", RFC_FORMAT_PAM,
                        RFC_FORMAT_PAM, 0);
    }

    /* 7. WEBP */
    {
        uint8_t webp[28];
        memset(webp, 0, sizeof(webp));
        memcpy(webp, "RIFF", 4u);
        wr32le(webp + 4u, 20u);
        memcpy(webp + 8u, "WEBP", 4u);
        memcpy(webp + 12u, "VP8 ", 4u);
        wr32le(webp + 16u, 8u);
        assert_sniff_eq(webp, sizeof(webp), "a.webp", RFC_FORMAT_WEBP,
                        RFC_FORMAT_WEBP, 0);
    }

    /* 8. JPEG XL (codestream & ISOBMFF) */
    {
        const uint8_t jxl_raw[] = {0xff, 0x0a, 0x00, 0x01, 0x02, 0x03};
        assert_sniff_eq(jxl_raw, sizeof(jxl_raw), "a.jxl",
                        RFC_FORMAT_JPEG_XL, RFC_FORMAT_JPEG_XL, 0);
        const uint8_t jxl_box[] = {0, 0, 0, 12, 'J', 'X', 'L', ' ', 0x0d, 0x0a, 0x87, 0x0a};
        assert_sniff_eq(jxl_box, sizeof(jxl_box), "b.jxl",
                        RFC_FORMAT_JPEG_XL, RFC_FORMAT_JPEG_XL, 0);
    }

    /* 9. AVIF, 13. HEIF, 14. HEIC, 18b. CR3 */
    {
        uint8_t isobmff[24];
        memset(isobmff, 0, sizeof(isobmff));
        wr32be(isobmff, 24u);
        memcpy(isobmff + 4u, "ftyp", 4u);

        memcpy(isobmff + 8u, "avif", 4u);
        assert_sniff_eq(isobmff, sizeof(isobmff), "a.avif",
                        RFC_FORMAT_AVIF, RFC_FORMAT_AVIF, 0);
        memcpy(isobmff + 8u, "mif1", 4u);
        assert_sniff_eq(isobmff, sizeof(isobmff), "a.heif",
                        RFC_FORMAT_HEIF, RFC_FORMAT_HEIF, 0);
        memcpy(isobmff + 8u, "heic", 4u);
        assert_sniff_eq(isobmff, sizeof(isobmff), "a.heic",
                        RFC_FORMAT_HEIC, RFC_FORMAT_HEIC, 0);
        memcpy(isobmff + 8u, "crx ", 4u);
        assert_sniff_eq(isobmff, sizeof(isobmff), "a.cr3",
                        RFC_FORMAT_CAMERA_RAW, RFC_FORMAT_CAMERA_RAW, 0);
    }

    /* 10. JPEG & 11. GIF & 15. JPEG2000 & 16. QOI */
    {
        const uint8_t jpg[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 0xff, 0xd9};
        assert_sniff_eq(jpg, sizeof(jpg), "a.jpg", RFC_FORMAT_JPEG, RFC_FORMAT_JPEG, 0);

        const uint8_t gif[] = "GIF89a\x01\x00\x01\x00\x00\x00\x00\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00;";
        assert_sniff_eq(gif, sizeof(gif) - 1u, "a.gif", RFC_FORMAT_GIF, RFC_FORMAT_GIF, 0);

        const uint8_t jp2[] = {0, 0, 0, 12, 'j', 'P', ' ', ' ', 0x0d, 0x0a, 0x87, 0x0a, 0, 0, 0, 12, 'f', 't', 'y', 'p', 'j', 'p', '2', ' '};
        assert_sniff_eq(jp2, sizeof(jp2), "a.jp2", RFC_FORMAT_JPEG2000, RFC_FORMAT_JPEG2000, 0);
        const uint8_t j2k[] = {0xff, 0x4f, 0xff, 0x51, 0x00, 0x2f};
        assert_sniff_eq(j2k, sizeof(j2k), "a.j2k", RFC_FORMAT_JPEG2000, RFC_FORMAT_JPEG2000, 0);

        const uint8_t qoi[14 + 8] = {'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        assert_sniff_eq(qoi, sizeof(qoi), "a.qoi", RFC_FORMAT_QOI, RFC_FORMAT_QOI, 0);
    }

    /* 12. TIFF vs 19. DNG vs 18. Camera RAW (CR2, RAF, MRW, X3F) */
    {
        uint8_t tif[40];
        memset(tif, 0, sizeof(tif));
        tif[0] = 'I'; tif[1] = 'I'; wr16le(tif + 2u, 42u); wr32le(tif + 4u, 8u);
        wr16le(tif + 8u, 1u);
        wr16le(tif + 10u, 0x0100u); /* ImageWidth */
        assert_sniff_eq(tif, sizeof(tif), "a.tif", RFC_FORMAT_TIFF, RFC_FORMAT_TIFF, 0);

        uint8_t dng[40];
        memcpy(dng, tif, sizeof(tif));
        wr16le(dng + 10u, 0xC612u); /* DNGVersion */
        assert_sniff_eq(dng, sizeof(dng), "a.dng", RFC_FORMAT_DNG, RFC_FORMAT_DNG, 0);

        uint8_t cr2[32];
        memset(cr2, 0, sizeof(cr2));
        cr2[0] = 'I'; cr2[1] = 'I'; wr16le(cr2 + 2u, 42u); wr32le(cr2 + 4u, 16u);
        cr2[8] = 'C'; cr2[9] = 'R'; cr2[10] = 2u; cr2[11] = 0u;
        assert_sniff_eq(cr2, sizeof(cr2), "a.cr2",
                        RFC_FORMAT_CAMERA_RAW, RFC_FORMAT_CAMERA_RAW, 0);

        const uint8_t raf[] = "FUJIFILMCCD-RAW 0201";
        assert_sniff_eq(raf, sizeof(raf) - 1u, "a.raf",
                        RFC_FORMAT_CAMERA_RAW, RFC_FORMAT_CAMERA_RAW, 0);
        const uint8_t mrw[] = {0x00, 'M', 'R', 'M', 0, 0, 0, 8};
        assert_sniff_eq(mrw, sizeof(mrw), "a.mrw",
                        RFC_FORMAT_CAMERA_RAW, RFC_FORMAT_CAMERA_RAW, 0);
        const uint8_t x3f[] = {'F', 'O', 'V', 'b', 0, 0, 2, 0};
        assert_sniff_eq(x3f, sizeof(x3f), "a.x3f",
                        RFC_FORMAT_CAMERA_RAW, RFC_FORMAT_CAMERA_RAW, 0);
    }

    /* 17. OpenEXR */
    {
        uint8_t exr[16];
        memset(exr, 0, sizeof(exr));
        exr[0] = 0x76u; exr[1] = 0x2fu; exr[2] = 0x31u; exr[3] = 0x01u;
        exr[4] = 0x02u;
        exr[8] = '\0'; /* end of header attributes */
        assert_sniff_eq(exr, sizeof(exr), "a.exr",
                        RFC_FORMAT_OPENEXR, RFC_FORMAT_OPENEXR, 0);
    }

    /* Unsafe SVG / XML / HTML / script masquerading as .png must be refused */
    {
        static const char *const unsafe[] = {
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>",
            "<?xml version=\"1.0\"?><svg></svg>",
            "<!DOCTYPE html><html><script>x=1</script></html>",
            "#!/bin/sh\necho hi\n",
        };
        for (size_t i = 0u; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
            rfc_sniff_result sr;
            rfc_status st = rfc_input_sniff_bytes(
                (const uint8_t *)unsafe[i], strlen(unsafe[i]), "evil.png", &sr);
            CHECK(st == RFC_ERR_UNKNOWN_FORMAT || st == RFC_ERR_FORMAT_DISAGREEMENT);
            CHECK(sr.content_format != RFC_FORMAT_PNG);
        }
    }

    /* Bounded read disagreement gate & scheduler preflight disagreement gate */
    {
        uint8_t *fold = NULL;
        size_t flen = 0u;
        build_test_fold(&fold, &flen, 4u, 4u, 21u);

        mem_reader_ctx rc;
        mem_reader_init(&rc, fold, flen);
        rfc_io_reader rdr = make_mem_reader(&rc);

        rfc_limits lim;
        rfc_limits_default(&lim);
        rfc_imported_input imp;
        memset(&imp, 0, sizeof(imp));
        CHECK(rfc_input_read_bounded(&rdr, "fake.png", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_FORMAT_DISAGREEMENT);
        CHECK(imp.bytes == NULL);

        CHECK(rfc_input_read_bounded(&rdr, "fake.png",
                                     RFC_IMPORT_ALLOW_DISAGREEMENT,
                                     &lim, NULL, NULL, &imp) == RFC_OK);
        CHECK(imp.sniff.content_format == RFC_FORMAT_RASTERFOLD);
        CHECK(imp.sniff.extension_format == RFC_FORMAT_PNG);
        CHECK(imp.sniff.disagreement == 1);
        CHECK(imp.size == flen);
        rfc_imported_input_free(&imp);
        mem_reader_destroy(&rc);

        /* Scheduler preflight: conflicting spec.source_format fails with allocs==0 even when allow_disagreement set */
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        mem_reader_init(&rc, fold, flen);
        mem_pub_ctx pc;
        mem_pub_init(&pc);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_reader rdr2 = make_mem_reader(&rc);
        rfc_io_publisher pub2 = make_mem_publisher(&pc);
        rfc_io_source_init_callbacks(&src_io, &rdr2, "fake.png",
                                     RFC_IMPORT_ALLOW_DISAGREEMENT);
        rfc_io_sink_init_callbacks(&dst_io, &pub2, RFC_EXPORT_REFUSE_EXISTING);

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_PNG; /* disagrees with sniffed RASTERFOLD */
        spec.target_format = RFC_FORMAT_BMP;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_scheduler_stats bstats, astats;
        rfc_scheduler_stats_get(sched, &bstats);
        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
              RFC_ERR_FORMAT_DISAGREEMENT);
        CHECK(job == NULL);
        rfc_scheduler_stats_get(sched, &astats);
        CHECK(astats.submission_alloc_count == bstats.submission_alloc_count);
        CHECK(rc.close_calls == 0u);
        CHECK(pc.release_calls == 0u);

        /* Now allow authoritative sniffed RASTERFOLD -> BMP via callbacks */
        spec.source_format = (rfc_format_id)0;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED);
        rfc_sniff_result jsr;
        CHECK(rfc_job_sniff_result_get(job, &jsr) == RFC_OK);
        CHECK(jsr.disagreement == 1);
        CHECK(jsr.content_format == RFC_FORMAT_RASTERFOLD);
        CHECK(pc.published == 1 && pc.published_len > 54u);
        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        CHECK(rc.close_calls == 1u);
        CHECK(pc.release_calls == 1u);
        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
        free(fold);
    }

    printf("file-safety: sniffing ok\n");
}

/* ----------------------------------------------------------------
 * 2. Conflicting job spec preflight refusals, bounded streaming limits,
 *    short reads, and container completeness checks.
 * ---------------------------------------------------------------- */

static rfc_status dummy_stage_cb(rfc_job *job, const rfc_exec_context *exec,
                                 void *user) {
    (void)job;
    (void)exec;
    (void)user;
    return RFC_OK;
}

static void test_conflicts_and_bounds(void) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    uint8_t bmp[256];
    size_t blen = 0u;
    build_test_bmp(bmp, &blen, 4u, 4u, 7u);

    /* 1-6: Conflicting inputs must all reject with submit_allocs == 0 & 0 close/release */
    {
        mem_reader_ctx rc;
        mem_pub_ctx pc;
        mem_reader_init(&rc, bmp, blen);
        mem_pub_init(&pc);
        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_io_publisher pub = make_mem_publisher(&pc);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "in.bmp", RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        for (int tc = 0; tc < 6; tc++) {
            rfc_job_spec spec;
            memset(&spec, 0, sizeof(spec));
            spec.source_format = RFC_FORMAT_BMP;
            spec.target_format = RFC_FORMAT_RASTERFOLD;
            spec.expected_working_set_bytes = 64u * 1024u;
            rfc_io_source s_bad = src_io;
            rfc_io_sink d_bad = dst_io;

            if (tc == 0) {
                spec.io_source = &src_io;
                spec.source_bytes = bmp;
                spec.source_size = blen;
            } else if (tc == 1) {
                spec.io_source = &src_io;
                spec.has_stage_ops = 1;
                spec.stage_ops.on_import = dummy_stage_cb;
            } else if (tc == 2) {
                spec.io_source = &src_io;
                spec.io_sink = &dst_io;
                spec.has_stage_ops = 1;
                spec.stage_ops.on_export = dummy_stage_cb;
            } else if (tc == 3) {
                d_bad.export_flags = 1u;
                spec.io_source = &src_io;
                spec.io_sink = &d_bad;
            } else if (tc == 4) {
                s_bad.path_utf8 = "/tmp/dual.bmp";
                spec.io_source = &s_bad;
            } else {
                d_bad.path_utf8 = "/tmp/dual.fold";
                spec.io_source = &src_io;
                spec.io_sink = &d_bad;
            }

            rfc_scheduler_stats bstats, astats;
            rfc_scheduler_stats_get(sched, &bstats);
            rfc_job *job = NULL;
            CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
                  RFC_ERR_INVALID_ARGUMENT);
            CHECK(job == NULL);
            rfc_scheduler_stats_get(sched, &astats);
            CHECK(astats.submission_alloc_count == bstats.submission_alloc_count);
            CHECK(rc.close_calls == 0u);
            CHECK(pc.release_calls == 0u);
        }

        /* Invalid source UTF-8 rejected preflight */
        {
            const uint8_t bad_utf8[4] = {(uint8_t)0xff, (uint8_t)0xfe, '.', 'b'};
            rfc_io_source s_bad;
            rfc_io_source_init_path(&s_bad, (const char *)bad_utf8, RFC_IMPORT_DEFAULT);
            rfc_job_spec spec;
            memset(&spec, 0, sizeof(spec));
            spec.source_format = RFC_FORMAT_BMP;
            spec.target_format = RFC_FORMAT_RASTERFOLD;
            spec.expected_working_set_bytes = 64u * 1024u;
            spec.io_source = &s_bad;
            rfc_scheduler_stats bstats, astats;
            rfc_scheduler_stats_get(sched, &bstats);
            rfc_job *job = NULL;
            CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
                  RFC_ERR_INVALID_ARGUMENT);
            CHECK(job == NULL);
            rfc_scheduler_stats_get(sched, &astats);
            CHECK(astats.submission_alloc_count == bstats.submission_alloc_count);
        }

        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
    }

    /* Unknown-size & declared-size max_compressed_bytes bounds */
    {
        uint8_t big_bmp[1024];
        size_t big_len = 0u;
        build_test_bmp(big_bmp, &big_len, 16u, 16u, 3u);
        CHECK(big_len > 256u);

        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_compressed_bytes = 256u;

        mem_reader_ctx rc;
        mem_reader_init(&rc, big_bmp, big_len);
        rc.know_size = 0;
        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_imported_input imp;
        memset(&imp, 0, sizeof(imp));
        CHECK(rfc_input_read_bounded(&rdr, "big.bmp", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_LIMIT_EXCEEDED);
        CHECK(imp.bytes == NULL);

        /* Declared known size > limit rejected in preflight with allocs==0 */
        rc.know_size = 1;
        rc.declared_size = (uint64_t)big_len;
        rfc_io_source src_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "big.bmp", RFC_IMPORT_DEFAULT);
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.limits_override = &lim;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        rfc_scheduler_stats bstats, astats;
        rfc_scheduler_stats_get(sched, &bstats);
        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) ==
              RFC_ERR_LIMIT_EXCEEDED);
        CHECK(job == NULL);
        rfc_scheduler_stats_get(sched, &astats);
        CHECK(astats.submission_alloc_count == bstats.submission_alloc_count);
        CHECK(rc.close_calls == 0u);

        /* Short-read vs declared size -> RFC_ERR_TRUNCATED_INPUT */
        lim.max_compressed_bytes = 1024u * 1024u;
        rc.know_size = 1;
        rc.declared_size = 2048u;
        CHECK(rfc_input_read_bounded(&rdr, "short.bmp", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_TRUNCATED_INPUT);
        CHECK(imp.bytes == NULL);
        mem_reader_destroy(&rc);
    }

    /* Container truncation checks across formats */
    {
        rfc_limits lim;
        rfc_limits_default(&lim);

        /* Truncated .fold header (< 32) and payload (< 32 + payload_size) */
        uint8_t *fold = NULL;
        size_t flen = 0u;
        build_test_fold(&fold, &flen, 4u, 4u, 9u);
        CHECK(flen > 36u);
        mem_reader_ctx rc;
        mem_reader_init(&rc, fold, 20u);
        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_imported_input imp;
        memset(&imp, 0, sizeof(imp));
        CHECK(rfc_input_read_bounded(&rdr, "t.fold", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_TRUNCATED_INPUT);
        mem_reader_destroy(&rc);

        mem_reader_init(&rc, fold, flen - 1u);
        rdr = make_mem_reader(&rc);
        CHECK(rfc_input_read_bounded(&rdr, "t.fold", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_TRUNCATED_INPUT);
        mem_reader_destroy(&rc);
        free(fold);

        /* Truncated BMP, PNG (no IEND), JPEG (no EOI), QOI (no end marker) */
        mem_reader_init(&rc, bmp, blen - 4u);
        rdr = make_mem_reader(&rc);
        CHECK(rfc_input_read_bounded(&rdr, "t.bmp", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_TRUNCATED_INPUT);
        mem_reader_destroy(&rc);

        const uint8_t png_no_iend[] = {
            0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
            0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0, 0, 0, 0, 0
        };
        mem_reader_init(&rc, png_no_iend, sizeof(png_no_iend));
        rdr = make_mem_reader(&rc);
        CHECK(rfc_input_read_bounded(&rdr, "t.png", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_TRUNCATED_INPUT);
        mem_reader_destroy(&rc);

        const uint8_t jpg_no_eoi[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1};
        mem_reader_init(&rc, jpg_no_eoi, sizeof(jpg_no_eoi));
        rdr = make_mem_reader(&rc);
        CHECK(rfc_input_read_bounded(&rdr, "t.jpg", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_TRUNCATED_INPUT);
        mem_reader_destroy(&rc);

        const uint8_t qoi_short[] = {'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0, 1, 2};
        mem_reader_init(&rc, qoi_short, sizeof(qoi_short));
        rdr = make_mem_reader(&rc);
        CHECK(rfc_input_read_bounded(&rdr, "t.qoi", RFC_IMPORT_DEFAULT,
                                     &lim, NULL, NULL, &imp) ==
              RFC_ERR_TRUNCATED_INPUT);
        mem_reader_destroy(&rc);
    }

    rfc_scheduler_destroy(sched);
    printf("file-safety: conflicts_and_bounds ok\n");
}

/* ----------------------------------------------------------------
 * 3. Path identity, alias refusals, existing destinations, and races
 * ---------------------------------------------------------------- */

static void test_path_identity_and_refusals(void) {
    char dir[512];
    CHECK(make_temp_dir(dir, sizeof(dir), "id"));

    char src_path[768], existing_dst[768], ro_dir[768], ro_dst[768];
    join_path(src_path, sizeof(src_path), dir, "src.bmp");
    join_path(existing_dst, sizeof(existing_dst), dir, "existing.fold");
    join_path(ro_dir, sizeof(ro_dir), dir, "rodir");
    join_path(ro_dst, sizeof(ro_dst), ro_dir, "out.fold");

    uint8_t bmp[256];
    size_t blen = 0u;
    build_test_bmp(bmp, &blen, 4u, 4u, 33u);
    write_file_exact(src_path, bmp, blen);

    static const uint8_t sentinel[12] = {'S', 'E', 'N', 'T', 'I', 'N', 'E', 'L', 1, 2, 3, 4};
    write_file_exact(existing_dst, sentinel, sizeof(sentinel));

    rfc_file_identity src_id;
    CHECK(rfc_file_identity_query_path(src_path, &src_id) == RFC_OK);
    CHECK(src_id.valid == 1);

    /* 1. Existing destination refused; sentinel bytes preserved */
    {
        rfc_export_report er;
        memset(&er, 0, sizeof(er));
        CHECK(rfc_output_write_path(existing_dst, src_path, &src_id,
                                    RFC_EXPORT_REFUSE_EXISTING,
                                    NULL, 10u, sentinel, sizeof(sentinel),
                                    NULL, &er) == RFC_ERR_DESTINATION_EXISTS);
        CHECK(er.published == 0);
        uint8_t *after = NULL;
        size_t alen = 0u;
        read_file_exact(existing_dst, &after, &alen);
        CHECK(alen == sizeof(sentinel) && memcmp(after, sentinel, sizeof(sentinel)) == 0);
        free(after);
        CHECK(!dir_has_prefix(dir, "existing.fold.rfc-partial."));
    }

    /* 2. Non-zero export_flags refused outright (AGENTS.md no-overwrite rule) */
    {
        char fresh_dst[768];
        join_path(fresh_dst, sizeof(fresh_dst), dir, "never_written.fold");
        rfc_export_report er;
        CHECK(rfc_output_write_path(existing_dst, src_path, &src_id, 1u,
                                    NULL, 11u, bmp, blen, NULL, &er) ==
              RFC_ERR_INVALID_ARGUMENT);
        CHECK(rfc_output_write_path(fresh_dst, src_path, &src_id, 1u,
                                    NULL, 12u, bmp, blen, NULL, &er) ==
              RFC_ERR_INVALID_ARGUMENT);
        CHECK(!file_exists_os(fresh_dst));
    }

    /* 3. Same-file input alias refused */
    {
        rfc_export_report er;
        CHECK(rfc_output_write_path(src_path, src_path, &src_id,
                                    RFC_EXPORT_REFUSE_EXISTING,
                                    NULL, 13u, sentinel, sizeof(sentinel),
                                    NULL, &er) == RFC_ERR_PATH_ALIAS);
        uint8_t *after = NULL;
        size_t alen = 0u;
        read_file_exact(src_path, &after, &alen);
        CHECK(alen == blen && memcmp(after, bmp, blen) == 0);
        free(after);
    }

#ifdef _WIN32
    /* 4W. Windows reserved device names rejected cleanly */
    {
        char nul_dst[768];
        join_path(nul_dst, sizeof(nul_dst), dir, "CON.fold");
        CHECK(rfc_output_check_destination_path(nul_dst, src_path, &src_id,
                                                RFC_EXPORT_REFUSE_EXISTING) ==
              RFC_ERR_INVALID_ARGUMENT);
    }
#else
    /* 4P. POSIX hard link, symlink, dangling symlink, symlink parent, chmod 0555 */
    {
        char hlink[768], sym_alias[768], sym_dangling[768], missing[768];
        char real_sub[768], sym_dir[768], sym_dir_dst[768];
        join_path(hlink, sizeof(hlink), dir, "hardlink.bmp");
        join_path(sym_alias, sizeof(sym_alias), dir, "sym_alias.bmp");
        join_path(missing, sizeof(missing), dir, "missing_target.fold");
        join_path(sym_dangling, sizeof(sym_dangling), dir, "sym_dangling.fold");
        join_path(real_sub, sizeof(real_sub), dir, "realsub");
        join_path(sym_dir, sizeof(sym_dir), dir, "symdir");
        join_path(sym_dir_dst, sizeof(sym_dir_dst), sym_dir, "src.bmp");

        CHECK(link(src_path, hlink) == 0);
        rfc_export_report er;
        CHECK(rfc_output_write_path(hlink, src_path, &src_id,
                                    RFC_EXPORT_REFUSE_EXISTING,
                                    NULL, 14u, sentinel, sizeof(sentinel),
                                    NULL, &er) == RFC_ERR_PATH_ALIAS);

        CHECK(symlink(src_path, sym_alias) == 0);
        CHECK(rfc_output_write_path(sym_alias, src_path, &src_id,
                                    RFC_EXPORT_REFUSE_EXISTING,
                                    NULL, 15u, sentinel, sizeof(sentinel),
                                    NULL, &er) == RFC_ERR_PATH_ALIAS);

        CHECK(symlink(missing, sym_dangling) == 0);
        CHECK(rfc_output_write_path(sym_dangling, src_path, &src_id,
                                    RFC_EXPORT_REFUSE_EXISTING,
                                    NULL, 16u, sentinel, sizeof(sentinel),
                                    NULL, &er) == RFC_ERR_PATH_ALIAS);

        CHECK(mkdir(real_sub, 0755) == 0);
        char real_sub_src[768];
        join_path(real_sub_src, sizeof(real_sub_src), real_sub, "src.bmp");
        write_file_exact(real_sub_src, bmp, blen);
        CHECK(symlink(real_sub, sym_dir) == 0);
        CHECK(rfc_output_write_path(sym_dir_dst, real_sub_src, NULL,
                                    RFC_EXPORT_REFUSE_EXISTING,
                                    NULL, 17u, sentinel, sizeof(sentinel),
                                    NULL, &er) == RFC_ERR_PATH_ALIAS);

        /* Non-root permission denial */
        if (geteuid() != 0) {
            CHECK(mkdir(ro_dir, 0755) == 0);
            CHECK(chmod(ro_dir, 0555) == 0);
            rfc_status pst = rfc_output_write_path(ro_dst, NULL, NULL,
                                                   RFC_EXPORT_REFUSE_EXISTING,
                                                   NULL, 18u, sentinel, sizeof(sentinel),
                                                   NULL, &er);
            CHECK(pst == RFC_ERR_PERMISSION_DENIED);
            CHECK(chmod(ro_dir, 0755) == 0);
            rmdir(ro_dir);
        }

        unlink(sym_dir);
        unlink(real_sub_src);
        rmdir(real_sub);
        unlink(sym_dangling);
        unlink(sym_alias);
        unlink(hlink);
    }
#endif

    /* 5. Post-preflight race destination creation before commit */
    {
        char race_dst[768];
        join_path(race_dst, sizeof(race_dst), dir, "raced.fold");
        CHECK(rfc_output_check_destination_path(race_dst, src_path, &src_id,
                                                RFC_EXPORT_REFUSE_EXISTING) == RFC_OK);
        /* Destination suddenly created before write/commit */
        rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_COMMIT_EXISTS, 0u,
                                               RFC_ERR_DESTINATION_EXISTS);
        rfc_export_report er;
        CHECK(rfc_output_write_path(race_dst, src_path, &src_id,
                                    RFC_EXPORT_REFUSE_EXISTING,
                                    NULL, 19u, bmp, blen, NULL, &er) ==
              RFC_ERR_DESTINATION_EXISTS);
        rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_NONE, 0u, RFC_OK);
        CHECK(er.published == 0 && er.temp_cleaned_up == 1);
        CHECK(!file_exists_os(race_dst));
        CHECK(!dir_has_prefix(dir, "raced.fold.rfc-partial."));
    }

    (void)ro_dir;
    (void)ro_dst;
    remove_file_os(existing_dst);
    remove_file_os(src_path);
    remove_dir_os(dir);
    printf("file-safety: path_identity_and_refusals ok\n");
}

/* ----------------------------------------------------------------
 * 4. Disk-full / flush / commit fault cleanup preserving foreign files
 * ---------------------------------------------------------------- */

static void test_disk_full_and_cleanup(void) {
    char dir[512];
    CHECK(make_temp_dir(dir, sizeof(dir), "disk"));

    char src_path[768], dst_path[768], foreign1[768], foreign2[768];
    join_path(src_path, sizeof(src_path), dir, "src.bmp");
    join_path(dst_path, sizeof(dst_path), dir, "out.fold");
    join_path(foreign1, sizeof(foreign1), dir, "out.fold.partial");
    join_path(foreign2, sizeof(foreign2), dir, "out.fold.rfc-partial.999999.77.1.1");

    uint8_t bmp[1024];
    size_t blen = 0u;
    build_test_bmp(bmp, &blen, 16u, 16u, 45u);
    write_file_exact(src_path, bmp, blen);

    static const uint8_t fmark[8] = {'F', 'O', 'R', 'E', 'I', 'G', 'N', '!'};
    write_file_exact(foreign1, fmark, sizeof(fmark));
    write_file_exact(foreign2, fmark, sizeof(fmark));

    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    static const rfc_io_fault_stage faults[4] = {
        RFC_IO_FAULT_WRITE_DISK_FULL,
        RFC_IO_FAULT_FLUSH_IO,
        RFC_IO_FAULT_COMMIT_EXISTS,
        RFC_IO_FAULT_COMMIT_IO,
    };
    static const rfc_status wants[4] = {
        RFC_ERR_DISK_FULL,
        RFC_ERR_IO,
        RFC_ERR_DESTINATION_EXISTS,
        RFC_ERR_IO,
    };

    for (size_t i = 0u; i < 4u; i++) {
        rfc_output_set_write_fault_for_testing(faults[i], 32u, wants[i]);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_path(&src_io, src_path, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&dst_io, dst_path, RFC_EXPORT_REFUSE_EXISTING);

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 128u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);

        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED);
        CHECK(snap.failing_stage == RFC_STAGE_EXPORT);
        CHECK(snap.status == wants[i]);
        CHECK(snap.live_partial_bytes == 0u);

        rfc_export_report er;
        CHECK(rfc_job_export_report_get(job, &er) == RFC_OK);
        CHECK(er.valid == 1 && er.published == 0 && er.temp_cleaned_up == 1);
        CHECK(er.status == wants[i]);
        CHECK(!file_exists_os(dst_path));
        CHECK(!dir_has_prefix(dir, "out.fold.rfc-partial.1."));
        CHECK(file_exists_os(foreign1) && file_exists_os(foreign2));

        rfc_job_release(job);
    }

    rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_NONE, 0u, RFC_OK);
    rfc_scheduler_destroy(sched);

    remove_file_os(foreign2);
    remove_file_os(foreign1);
    remove_file_os(src_path);
    remove_dir_os(dir);
    printf("file-safety: disk_full_and_cleanup ok\n");
}

/* ----------------------------------------------------------------
 * 5. Callback ownership, queued/mid-read cancel, and retry rewind
 * ---------------------------------------------------------------- */

static void test_callback_ownership_and_retry(void) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;

    /* A. Mid-read cancellation owns exactly one close/release + zero abort leak */
    {
        uint8_t bmp[256];
        size_t blen = 0u;
        build_test_bmp(bmp, &blen, 4u, 4u, 11u);
        mem_reader_ctx rc;
        mem_pub_ctx pc;
        mem_reader_init(&rc, bmp, blen);
        rc.chunk_cap = 32u;
        rc.block_after_first_read = 1;
        mem_pub_init(&pc);

        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_io_publisher pub = make_mem_publisher(&pc);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "live.bmp", RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);

        fs_mutex_lock(&rc.lock);
        while (!rc.read_entered) {
            fs_cond_wait(&rc.cond, &rc.lock);
        }
        fs_mutex_unlock(&rc.lock);

        CHECK(rfc_job_cancel(job) == RFC_OK);
        fs_mutex_lock(&rc.lock);
        rc.release_read = 1;
        fs_cond_broadcast(&rc.cond);
        fs_mutex_unlock(&rc.lock);

        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_CANCELLED && snap.status == RFC_ERR_CANCELLED);
        CHECK(pc.published == 0);
        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        CHECK(rc.close_calls == 1u);
        CHECK(pc.release_calls == 1u);
        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
    }

    /* B. Retry rewinds stream (seek_fn(0)), re-reads live updated BMP, resets reports,
     *    allocates fresh attempt handle (attempt 2), and calls close/release ONCE. */
    {
        uint8_t bmp1[256], bmp2[256];
        size_t blen1 = 0u, blen2 = 0u;
        build_test_bmp(bmp1, &blen1, 4u, 4u, 19u);
        build_test_bmp(bmp2, &blen2, 4u, 4u, 99u);
        CHECK(blen1 == blen2);

        mem_reader_ctx rc;
        mem_pub_ctx pc;
        mem_reader_init(&rc, bmp1, blen1);
        mem_pub_init(&pc);
        pc.fail_commit_once_status = RFC_ERR_IO;

        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_io_publisher pub = make_mem_publisher(&pc);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "retry.bmp", RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);
        rfc_job_slot *slot = rfc_job_slot_create();
        CHECK(slot != NULL);

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *j1 = NULL;
        CHECK(rfc_scheduler_submit(sched, slot, &spec, &j1) == RFC_OK);
        CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);

        rfc_job_snapshot s1;
        CHECK(rfc_job_snapshot_get(j1, &s1) == RFC_OK);
        CHECK(s1.state == RFC_JOB_FAILED && s1.status == RFC_ERR_IO);
        CHECK(s1.attempt_number == 1u);
        CHECK(pc.begin_calls == 1u && pc.abort_calls == 1u && pc.live_handles == 0u);
        CHECK(rc.close_calls == 0u && pc.release_calls == 0u);

        rfc_export_report er1;
        CHECK(rfc_job_export_report_get(j1, &er1) == RFC_OK);
        CHECK(er1.valid == 1 && er1.published == 0 && er1.status == RFC_ERR_IO);

        /* Mutate live reader payload before retry -> attempt 2 must decode bmp2! */
        fs_mutex_lock(&rc.lock);
        memcpy(rc.buf, bmp2, blen2);
        fs_mutex_unlock(&rc.lock);
        uint32_t seeks_before_retry = rc.seek_calls;

        rfc_job *j2 = NULL;
        CHECK(rfc_scheduler_retry(sched, slot, &j2) == RFC_OK);
        CHECK(rfc_job_wait(j2, UINT32_MAX) == RFC_OK);

        rfc_job_snapshot s2;
        CHECK(rfc_job_snapshot_get(j2, &s2) == RFC_OK);
        CHECK(s2.state == RFC_JOB_SUCCEEDED && s2.status == RFC_OK);
        CHECK(s2.attempt_number == 2u);
        CHECK(rc.seek_calls > seeks_before_retry);
        CHECK(pc.begin_calls == 2u && pc.last_attempt == 2u);
        CHECK(pc.abort_calls == 1u && pc.commit_calls == 2u && pc.published == 1);
        CHECK(pc.live_handles == 0u);
        CHECK(rc.close_calls == 0u && pc.release_calls == 0u);

        /* Decode published .fold bytes and prove pixels match bmp2 (seed 99), not bmp1 */
        rf_pixels decoded = {0};
        rf_error rf_err = {0, NULL};
        CHECK(rf_decode_image(pc.published_bytes, pc.published_len, NULL,
                              &decoded, &rf_err) == RF_OK);
        CHECK(decoded.width == 4u && decoded.height == 4u);
        /* Pixel (0,0) in bmp2(seed=99): r = 99, g = 41? let's check build_test_bmp:
         * r=(0+0+99)&255=99, g=(0+0+99*3)&255=41, b=(0+0+99*5)&255=239 */
        CHECK(decoded.data[0] == (uint8_t)(99u & 0xffu));
        CHECK(decoded.data[1] == (uint8_t)((99u * 3u) & 0xffu));
        CHECK(decoded.data[2] == (uint8_t)((99u * 5u) & 0xffu));
        rf_free(decoded.data);

        rfc_job_release(j1);
        rfc_job_release(j2);
        rfc_job_slot_destroy(slot);
        rfc_scheduler_destroy(sched);
        CHECK(rc.close_calls == 1u);
        CHECK(pc.release_calls == 1u);
        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
    }

    printf("file-safety: callback_ownership_and_retry ok\n");
}

/* ----------------------------------------------------------------
 * 6. Deterministic commit / cancel / destroy serialization barriers
 * ---------------------------------------------------------------- */

typedef struct commit_barrier_ctx {
    fs_mutex_t lock;
    fs_cond_t cond;
    int entered;
    int release;
    rfc_job *job;
    rfc_status cancel_rc;
    int cancel_done;
} commit_barrier_ctx;

static void barrier_before_cb(rfc_job_id job_id, void *user) {
    (void)job_id;
    commit_barrier_ctx *b = (commit_barrier_ctx *)user;
    fs_mutex_lock(&b->lock);
    b->entered = 1;
    fs_cond_broadcast(&b->cond);
    while (!b->release) {
        fs_cond_wait(&b->cond, &b->lock);
    }
    fs_mutex_unlock(&b->lock);
}

static void barrier_after_cb(rfc_job_id job_id, void *user) {
    (void)job_id;
    commit_barrier_ctx *b = (commit_barrier_ctx *)user;
    fs_mutex_lock(&b->lock);
    b->entered = 1;
    fs_cond_broadcast(&b->cond);
    while (!b->cancel_done) {
        fs_cond_wait(&b->cond, &b->lock);
    }
    fs_mutex_unlock(&b->lock);
}

static FS_THREAD_RET cancel_worker_fn(void *user) {
    commit_barrier_ctx *b = (commit_barrier_ctx *)user;
    rfc_status st = rfc_job_cancel(b->job);
    fs_mutex_lock(&b->lock);
    b->cancel_rc = st;
    b->cancel_done = 1;
    fs_cond_broadcast(&b->cond);
    fs_mutex_unlock(&b->lock);
    FS_THREAD_RETURN;
}

static void test_commit_cancel_serialization(void) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;

    uint8_t bmp[256];
    size_t blen = 0u;
    build_test_bmp(bmp, &blen, 4u, 4u, 77u);

    /* Case A: Cancel while paused BEFORE begin_publish -> abort & never publish */
    {
        commit_barrier_ctx b;
        memset(&b, 0, sizeof(b));
        fs_mutex_init(&b.lock);
        fs_cond_init(&b.cond);
        rfc_output_set_commit_barrier_for_testing(barrier_before_cb, NULL, &b);

        mem_reader_ctx rc;
        mem_pub_ctx pc;
        mem_reader_init(&rc, bmp, blen);
        mem_pub_init(&pc);
        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_io_publisher pub = make_mem_publisher(&pc);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "c1.bmp", RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);

        fs_mutex_lock(&b.lock);
        while (!b.entered) {
            fs_cond_wait(&b.cond, &b.lock);
        }
        fs_mutex_unlock(&b.lock);

        CHECK(rfc_job_cancel(job) == RFC_OK);
        fs_mutex_lock(&b.lock);
        b.release = 1;
        fs_cond_broadcast(&b.cond);
        fs_mutex_unlock(&b.lock);

        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_CANCELLED && snap.status == RFC_ERR_CANCELLED);
        CHECK(pc.published == 0 && pc.abort_calls == 1u);

        rfc_scheduler_stats stats;
        rfc_scheduler_stats_get(sched, &stats);
        CHECK(stats.published_cancelled_count == 0u);

        rfc_export_report er;
        CHECK(rfc_job_export_report_get(job, &er) == RFC_OK);
        CHECK(er.published == 0 && er.temp_cleaned_up == 1);

        rfc_output_set_commit_barrier_for_testing(NULL, NULL, NULL);
        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        CHECK(rc.close_calls == 1u && pc.release_calls == 1u);
        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
        fs_cond_destroy(&b.cond);
        fs_mutex_destroy(&b.lock);
    }

    /* Case B: Cancel concurrently AFTER begin_publish entered -> commit completes,
     * job stays SUCCEEDED, published_cancelled_count == 0 */
    {
        commit_barrier_ctx b;
        memset(&b, 0, sizeof(b));
        fs_mutex_init(&b.lock);
        fs_cond_init(&b.cond);
        rfc_output_set_commit_barrier_for_testing(NULL, barrier_after_cb, &b);

        mem_reader_ctx rc;
        mem_pub_ctx pc;
        mem_reader_init(&rc, bmp, blen);
        mem_pub_init(&pc);
        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_io_publisher pub = make_mem_publisher(&pc);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "c2.bmp", RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);
        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        b.job = job;

        fs_mutex_lock(&b.lock);
        while (!b.entered) {
            fs_cond_wait(&b.cond, &b.lock);
        }
        fs_mutex_unlock(&b.lock);

        fs_thread_t cth;
        CHECK(fs_thread_start(&cth, cancel_worker_fn, &b));
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        fs_thread_join(cth);

        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        CHECK(pc.published == 1 && b.cancel_rc == RFC_OK);

        rfc_scheduler_stats stats;
        rfc_scheduler_stats_get(sched, &stats);
        CHECK(stats.published_cancelled_count == 0u);

        rfc_output_set_commit_barrier_for_testing(NULL, NULL, NULL);
        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        CHECK(rc.close_calls == 1u && pc.release_calls == 1u);
        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
        fs_cond_destroy(&b.cond);
        fs_mutex_destroy(&b.lock);
    }

    printf("file-safety: commit_cancel_serialization ok\n");
}

/* ----------------------------------------------------------------
 * 7. Full .fold path-backed & callback-backed round-trip with SHA-256
 * ---------------------------------------------------------------- */

static void test_fold_roundtrip(void) {
    char dir[512];
    CHECK(make_temp_dir(dir, sizeof(dir), "rt"));
    char bmp_path[768], fold_path[768];
    join_path(bmp_path, sizeof(bmp_path), dir, "img.bmp");
    join_path(fold_path, sizeof(fold_path), dir, "img.fold");

    uint8_t bmp[1024];
    size_t blen = 0u;
    build_test_bmp(bmp, &blen, 16u, 16u, 61u);
    write_file_exact(bmp_path, bmp, blen);

    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    CHECK(sched != NULL);

    rfc_io_source src_io;
    rfc_io_sink dst_io;
    rfc_io_source_init_path(&src_io, bmp_path, RFC_IMPORT_DEFAULT);
    rfc_io_sink_init_path(&dst_io, fold_path, RFC_EXPORT_REFUSE_EXISTING);

    rfc_job_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.source_format = RFC_FORMAT_BMP;
    spec.target_format = RFC_FORMAT_RASTERFOLD;
    spec.expected_working_set_bytes = 128u * 1024u;
    spec.io_source = &src_io;
    spec.io_sink = &dst_io;

    rfc_job *job = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
    CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);

    rfc_job_snapshot snap;
    CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
    CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);

    rfc_verify_report vr;
    CHECK(rfc_job_verify_report_get(job, &vr) == RFC_OK);
    CHECK(vr.valid == 1 && vr.status == RFC_OK);

    rfc_export_report er;
    CHECK(rfc_job_export_report_get(job, &er) == RFC_OK);
    CHECK(er.valid == 1 && er.published == 1 && er.temp_cleaned_up == 1);
    CHECK(er.bytes_written > 32u);

    uint8_t *disk_fold = NULL;
    size_t dlen = 0u;
    read_file_exact(fold_path, &disk_fold, &dlen);
    CHECK(dlen == (size_t)er.bytes_written);
    uint8_t disk_sha[32];
    rfc_bytes_hash_sha256(disk_fold, dlen, disk_sha);
    CHECK(memcmp(disk_sha, er.output_sha256, 32u) == 0);

    rf_pixels px = {0};
    rf_error rf_err = {0, NULL};
    CHECK(rf_decode_image(disk_fold, dlen, NULL, &px, &rf_err) == RF_OK);
    CHECK(px.width == 16u && px.height == 16u);
    CHECK(px.data[0] == (uint8_t)(61u & 0xffu));
    CHECK(px.data[1] == (uint8_t)((61u * 3u) & 0xffu));
    CHECK(px.data[2] == (uint8_t)((61u * 5u) & 0xffu));
    rf_free(px.data);
    free(disk_fold);

    CHECK(!dir_has_prefix(dir, "img.fold.rfc-partial."));

    rfc_job_release(job);
    rfc_scheduler_destroy(sched);
    remove_file_os(fold_path);
    remove_file_os(bmp_path);
    remove_dir_os(dir);
    printf("file-safety: fold_roundtrip ok\n");
}

/* ----------------------------------------------------------------
 * CLI mode for cross-platform OS/NTFS/RLIMIT_FSIZE Python suite
 * ---------------------------------------------------------------- */

static int cmd_sniff(const char *path) {
    rfc_sniff_result sr;
    uint64_t sz = 0u;
    rfc_file_identity id;
    memset(&sr, 0, sizeof(sr));
    memset(&id, 0, sizeof(id));
    rfc_status st = rfc_input_sniff_path(path, &sr, &sz, &id);
    printf("status=%d name=%s content=%d ext=%d present=%d known=%d disagree=%d "
           "size=%llu valid_id=%d symlink=%d links=%llu\n",
           (int)st, rfc_status_name(st), (int)sr.content_format,
           (int)sr.extension_format, sr.extension_present, sr.extension_known,
           sr.disagreement, (unsigned long long)sz, id.valid,
           id.is_symlink_or_reparse, (unsigned long long)id.link_count);
    return st == RFC_OK ? 0 : (int)st;
}

static int cmd_export_refuse(const char *src_opt, const char *dst,
                             uint32_t flags) {
    const char *src = (strcmp(src_opt, "-") == 0) ? NULL : src_opt;
    rfc_file_identity src_id;
    memset(&src_id, 0, sizeof(src_id));
    if (src) {
        rfc_status qst = rfc_file_identity_query_path(src, &src_id);
        (void)qst;
    }
    static const uint8_t payload[24] = {
        'R', 'F', 'T', 'E', 'S', 'T', 1, 2, 3, 4, 5, 6,
        7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18
    };
    rfc_export_report er;
    memset(&er, 0, sizeof(er));
    rfc_status st = rfc_output_write_path(dst, src, src ? &src_id : NULL, flags,
                                          NULL, 42u, payload, sizeof(payload),
                                          NULL, &er);
    printf("status=%d name=%s published=%d cleaned=%d\n",
           (int)st, rfc_status_name(st), er.published, er.temp_cleaned_up);
    return st == RFC_OK ? 0 : (int)st;
}

static int cmd_read_check(const char *path, uint32_t flags) {
    rfc_limits lim;
    rfc_limits_default(&lim);
    rfc_imported_input imp;
    memset(&imp, 0, sizeof(imp));
    rfc_status st = rfc_input_read_path(path, flags, &lim, NULL, NULL, &imp);
    printf("status=%d name=%s content=%d ext=%d disagree=%d size=%zu\n",
           (int)st, rfc_status_name(st), (int)imp.sniff.content_format,
           (int)imp.sniff.extension_format, imp.sniff.disagreement, imp.size);
    rfc_imported_input_free(&imp);
    return st == RFC_OK ? 0 : (int)st;
}

static int cmd_pipeline_fold(const char *src_path, const char *dst_path,
                             uint32_t import_flags, int src_fmt_override) {
    rfc_scheduler_config cfg;
    rfc_scheduler_config_default(&cfg);
    cfg.max_workers = 1u;
    rfc_scheduler *sched = rfc_scheduler_create(&cfg);
    if (!sched) {
        return 99;
    }

    rfc_io_source src_io;
    rfc_io_sink dst_io;
    rfc_io_source_init_path(&src_io, src_path, import_flags);
    rfc_io_sink_init_path(&dst_io, dst_path, RFC_EXPORT_REFUSE_EXISTING);

    rfc_job_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.source_format = (rfc_format_id)src_fmt_override;
    spec.target_format = RFC_FORMAT_RASTERFOLD;
    spec.expected_working_set_bytes = 256u * 1024u;
    spec.io_source = &src_io;
    spec.io_sink = &dst_io;

    rfc_job *job = NULL;
    rfc_status sub_st = rfc_scheduler_submit(sched, NULL, &spec, &job);
    if (sub_st != RFC_OK) {
        printf("state=-1 status=%d name=%s failing_stage=0 disagree=0 "
               "published=0 cleaned=1 bytes=0\n",
               (int)sub_st, rfc_status_name(sub_st));
        rfc_scheduler_destroy(sched);
        return (int)sub_st;
    }

    rfc_job_wait(job, UINT32_MAX);
    rfc_job_snapshot snap;
    rfc_job_snapshot_get(job, &snap);
    rfc_sniff_result sr;
    memset(&sr, 0, sizeof(sr));
    rfc_job_sniff_result_get(job, &sr);
    rfc_export_report er;
    memset(&er, 0, sizeof(er));
    rfc_job_export_report_get(job, &er);

    printf("state=%d status=%d name=%s failing_stage=%d disagree=%d "
           "published=%d cleaned=%d bytes=%llu\n",
           (int)snap.state, (int)snap.status, rfc_status_name(snap.status),
           (int)snap.failing_stage, sr.disagreement, er.published,
           er.temp_cleaned_up, (unsigned long long)er.bytes_written);

    int rc = (snap.state == RFC_JOB_SUCCEEDED) ? 0
             : (snap.status ? (int)snap.status : 1);
    rfc_job_release(job);
    rfc_scheduler_destroy(sched);
    return rc;
}

static int cmd_write_fault(const char *dst_path, size_t payload_bytes,
                           int fault_stage_int) {
    if (payload_bytes == 0u || payload_bytes > (4u * 1024u * 1024u)) {
        return 2;
    }
    uint8_t *buf = (uint8_t *)malloc(payload_bytes);
    if (!buf) {
        return 3;
    }
    for (size_t i = 0u; i < payload_bytes; i++) {
        buf[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
    }

    if (fault_stage_int > 0) {
        rfc_output_set_write_fault_for_testing((rfc_io_fault_stage)fault_stage_int,
                                               payload_bytes / 2u,
                                               RFC_ERR_DISK_FULL);
    }
    rfc_export_report er;
    memset(&er, 0, sizeof(er));
    rfc_status st = rfc_output_write_path(dst_path, NULL, NULL,
                                          RFC_EXPORT_REFUSE_EXISTING, NULL, 77u,
                                          buf, payload_bytes, NULL, &er);
    rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_NONE, 0u, RFC_OK);
    free(buf);

    printf("status=%d name=%s published=%d cleaned=%d bytes=%llu\n",
           (int)st, rfc_status_name(st), er.published, er.temp_cleaned_up,
           (unsigned long long)er.bytes_written);
    return st == RFC_OK ? 0 : (int)st;
}

typedef struct hostile_read_ctx {
    const uint8_t *bmp;
    size_t blen;
    size_t pos;
    int seek_count;
    int trigger_on_seek;
    size_t oversize_plus;
} hostile_read_ctx;

static rfc_status hostile_oversize_read(void *raw, uint8_t *out, size_t cap,
                                        size_t *out_got) {
    hostile_read_ctx *c = (hostile_read_ctx *)raw;
    if (c->seek_count >= c->trigger_on_seek) {
        *out_got = cap + c->oversize_plus;
        return RFC_OK;
    }
    size_t rem = c->blen - c->pos;
    size_t take = rem < cap ? rem : cap;
    if (take > 0u) {
        memcpy(out, c->bmp + c->pos, take);
    }
    c->pos += take;
    *out_got = take;
    return RFC_OK;
}

static rfc_status hostile_oversize_seek(void *raw, uint64_t off) {
    hostile_read_ctx *c = (hostile_read_ctx *)raw;
    if (off != 0u) {
        return RFC_ERR_IO;
    }
    c->pos = 0u;
    c->seek_count++;
    return RFC_OK;
}

static void audit_partial_free(void *ctx, void *ptr) {
    int *freed = (int *)ctx;
    *freed = 1;
    free(ptr);
}

typedef struct toctou_swap_ctx {
    const uint8_t *first_buf;
    size_t first_len;
    const uint8_t *second_buf;
    size_t second_len;
    size_t pos;
    int seek_count;
} toctou_swap_ctx;

static rfc_status toctou_swap_read(void *raw, uint8_t *out, size_t cap,
                                   size_t *out_got) {
    toctou_swap_ctx *c = (toctou_swap_ctx *)raw;
    const uint8_t *src = (c->seek_count >= 3) ? c->second_buf : c->first_buf;
    size_t slen = (c->seek_count >= 3) ? c->second_len : c->first_len;
    size_t rem = (c->pos < slen) ? (slen - c->pos) : 0u;
    size_t take = (rem < cap) ? rem : cap;
    if (take > 0u) {
        memcpy(out, src + c->pos, take);
    }
    c->pos += take;
    *out_got = take;
    return RFC_OK;
}

static rfc_status toctou_swap_seek(void *raw, uint64_t off) {
    toctou_swap_ctx *c = (toctou_swap_ctx *)raw;
    if (off != 0u) {
        return RFC_ERR_IO;
    }
    c->pos = 0u;
    c->seek_count++;
    return RFC_OK;
}

typedef struct tail_io_err_ctx {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    size_t fail_from_off;
} tail_io_err_ctx;

static rfc_status tail_io_err_read(void *raw, uint8_t *out, size_t cap,
                                   size_t *out_got) {
    tail_io_err_ctx *c = (tail_io_err_ctx *)raw;
    if (c->pos >= c->fail_from_off) {
        return RFC_ERR_IO;
    }
    size_t limit = (c->len < c->fail_from_off) ? c->len : c->fail_from_off;
    size_t rem = (c->pos < limit) ? (limit - c->pos) : 0u;
    size_t take = (rem < cap) ? rem : cap;
    if (take > 0u) {
        memcpy(out, c->buf + c->pos, take);
    }
    c->pos += take;
    *out_got = take;
    return RFC_OK;
}

static rfc_status tail_io_err_seek(void *raw, uint64_t off) {
    tail_io_err_ctx *c = (tail_io_err_ctx *)raw;
    if (off >= (uint64_t)c->fail_from_off) {
        return RFC_ERR_IO;
    }
    if (off > (uint64_t)c->len) {
        return RFC_ERR_IO;
    }
    c->pos = (size_t)off;
    return RFC_OK;
}

typedef struct blocker_ctx {
    fs_mutex_t lock;
    fs_cond_t cond;
    int entered;
    int release;
} blocker_ctx;

static rfc_status blocker_export_cb(rfc_job *job, const rfc_exec_context *exec,
                                    void *user) {
    (void)job;
    (void)exec;
    blocker_ctx *bc = (blocker_ctx *)user;
    fs_mutex_lock(&bc->lock);
    bc->entered = 1;
    fs_cond_broadcast(&bc->cond);
    while (!bc->release) {
        fs_cond_wait(&bc->cond, &bc->lock);
    }
    fs_mutex_unlock(&bc->lock);
    return RFC_OK;
}

static rfc_status fs_noop_stage_cb(rfc_job *job, const rfc_exec_context *exec,
                                   void *user) {
    (void)job;
    (void)exec;
    (void)user;
    return RFC_OK;
}

typedef struct race_hook_ctx {
    char dir[1024];
    char partial_path[1200];
    char target_or_moved[1200];
    char attacker_dir[1200];
    int mode; /* 0: assert no visible partial, 2: swap dir, 3: chmod 0555, 4: win32 lock check */
    int triggered;
    int verified;
} race_hook_ctx;

#ifdef _WIN32
static void win32_make_junction(const char *junc_utf8, const char *target_utf8) {
    wchar_t sys_dir[MAX_PATH];
    UINT sn = GetSystemDirectoryW(sys_dir, MAX_PATH);
    CHECK(sn > 0u && sn < MAX_PATH - 16u);
    wchar_t cmd_exe[MAX_PATH];
    _snwprintf(cmd_exe, MAX_PATH, L"%ls\\cmd.exe", sys_dir);
    wchar_t wjunc[1200], wtarget[1200], cmd_line[3000];
    CHECK(utf8_to_wide(junc_utf8, wjunc, 1200u));
    CHECK(utf8_to_wide(target_utf8, wtarget, 1200u));
    _snwprintf(cmd_line, 3000u, L"\"%ls\" /d /c mklink /J \"%ls\" \"%ls\"",
               cmd_exe, wjunc, wtarget);

    HANDLE job = CreateJobObjectW(NULL, NULL);
    CHECK(job != NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    memset(&jeli, 0, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli,
                                  sizeof(jeli)) != FALSE);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(cmd_exe, cmd_line, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL,
                             &si, &pi);
    if (!ok) {
        CloseHandle(job);
        CHECK(ok != FALSE);
        return;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1u);
        (void)WaitForSingleObject(pi.hProcess, 5000u);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        CHECK(0);
        return;
    }
    CHECK(ResumeThread(pi.hThread) != (DWORD)-1);

    DWORD wr = WaitForSingleObject(pi.hProcess, 30000u);
    DWORD ec = 1u;
    BOOL ec_ok = FALSE;
    if (wr != WAIT_OBJECT_0) {
        (void)TerminateJobObject(job, 1u);
        (void)TerminateProcess(pi.hProcess, 1u);
        (void)WaitForSingleObject(pi.hProcess, 5000u);
    } else {
        ec_ok = GetExitCodeProcess(pi.hProcess, &ec);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    CHECK(wr == WAIT_OBJECT_0);
    CHECK(ec_ok != FALSE && ec == 0u);
}
#else
typedef struct posix_post_link_ctx {
    char src_path[1200];
    char moved_path[1200];
    fs_mutex_t lock;
    fs_cond_t cond;
    int sync_enabled;
    int entered;
    int cancel_dispatched;
    int allow_finish;
    int fired;
    rfc_job *job;
    rfc_status cancel_status;
} posix_post_link_ctx;

static void posix_post_link_cb(rfc_job_id job_id, void *user) {
    (void)job_id;
    posix_post_link_ctx *pl = (posix_post_link_ctx *)user;
    pl->fired++;
    CHECK(rename(pl->src_path, pl->moved_path) == 0);
    if (pl->sync_enabled) {
        fs_mutex_lock(&pl->lock);
        pl->entered = 1;
        fs_cond_broadcast(&pl->cond);
        while (!pl->allow_finish) {
            fs_cond_wait(&pl->cond, &pl->lock);
        }
        fs_mutex_unlock(&pl->lock);
    }
}

static void *post_link_cancel_worker(void *arg) {
    posix_post_link_ctx *pl = (posix_post_link_ctx *)arg;
    fs_mutex_lock(&pl->lock);
    pl->cancel_dispatched = 1;
    fs_cond_broadcast(&pl->cond);
    fs_mutex_unlock(&pl->lock);
    pl->cancel_status = rfc_job_cancel(pl->job);
    return NULL;
}

typedef struct posix_post_check_ctx {
    char subst_path[1200];
    char sym_path[1200];
    char sym_target[1200];
    char abort_path[1200];
    char dest_sym_path[1200];
    char dest_sym_target[1200];
    int commit_fired;
    int abort_fired;
} posix_post_check_ctx;

static void posix_post_commit_cb(rfc_job_id job_id, void *user) {
    (void)job_id;
    posix_post_check_ctx *pc = (posix_post_check_ctx *)user;
    pc->commit_fired++;
    if (pc->subst_path[0] != '\0') {
        const uint8_t att[] = "ATTACKER";
        write_file_exact(pc->subst_path, att, sizeof(att) - 1u);
    }
    if (pc->sym_path[0] != '\0' && pc->sym_target[0] != '\0') {
        CHECK(symlink(pc->sym_target, pc->sym_path) == 0);
    }
    if (pc->dest_sym_path[0] != '\0' && pc->dest_sym_target[0] != '\0') {
        CHECK(symlink(pc->dest_sym_target, pc->dest_sym_path) == 0);
    }
}

static void posix_post_abort_cb(rfc_job_id job_id, void *user) {
    (void)job_id;
    posix_post_check_ctx *pc = (posix_post_check_ctx *)user;
    pc->abort_fired++;
    if (pc->abort_path[0] != '\0') {
        const uint8_t ab[] = "FOREIGN_ABORT";
        write_file_exact(pc->abort_path, ab, sizeof(ab) - 1u);
    }
}
#endif

static void race_before_publish_cb(rfc_job_id job_id, void *user) {
    (void)job_id;
    race_hook_ctx *rc = (race_hook_ctx *)user;
    rc->triggered = 1;
#ifdef _WIN32
    if (rc->mode == 4) {
        char pat[1200];
        join_path(pat, sizeof(pat), rc->dir, "out.fold.rfc-partial.*");
        wchar_t wpat[1200];
        CHECK(utf8_to_wide(pat, wpat, 1200u));
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW(wpat, &fd);
        CHECK(hf != INVALID_HANDLE_VALUE);
        char pname[512];
        int pn = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, pname,
                                     sizeof(pname), NULL, NULL);
        FindClose(hf);
        CHECK(pn > 0);
        join_path(rc->partial_path, sizeof(rc->partial_path), rc->dir, pname);
        wchar_t wp[1200], wd[1200], wm[1200];
        CHECK(utf8_to_wide(rc->partial_path, wp, 1200u));
        CHECK(utf8_to_wide(rc->dir, wd, 1200u));
        CHECK(utf8_to_wide(rc->target_or_moved, wm, 1200u));
        BOOL del_ok = DeleteFileW(wp);
        DWORD del_err = GetLastError();
        BOOL mov_ok = MoveFileW(wd, wm);
        DWORD mov_err = GetLastError();
        if (!del_ok && del_err == ERROR_SHARING_VIOLATION &&
            !mov_ok &&
            (mov_err == ERROR_SHARING_VIOLATION ||
             mov_err == ERROR_ACCESS_DENIED)) {
            rc->verified = 1;
        }
    }
#else
    if (rc->mode == 0) {
        CHECK(dir_has_prefix(rc->dir, "out.fold.rfc-partial.") == 0);
        rc->verified = 1;
    } else if (rc->mode == 2) {
        CHECK(rename(rc->dir, rc->target_or_moved) == 0);
        CHECK(symlink(rc->attacker_dir, rc->dir) == 0);
        rc->verified = 1;
    } else if (rc->mode == 3) {
        CHECK(chmod(rc->dir, 0555) == 0);
        rc->verified = 1;
    }
#endif
}

static void test_audit_regressions(void) {
    uint8_t bmp[256];
    size_t blen = 0u;
    build_test_bmp(bmp, &blen, 4u, 4u, 77u);

    /* 1. Truncated UTF-8 sequences never over-read and fail with INVALID_ARGUMENT. */
    {
        const char bad1[] = {(char)0xE0, '\0'};
        const char bad2[] = {(char)0xF0, (char)0x90, '\0'};
        const char bad3[] = {(char)0xC2, '\0'};
        rfc_sniff_result sr;
        rfc_imported_input imp;
        memset(&imp, 0, sizeof(imp));
        CHECK(rfc_input_sniff_path(bad1, &sr, NULL, NULL) ==
              RFC_ERR_INVALID_ARGUMENT);
        CHECK(rfc_input_sniff_path(bad2, &sr, NULL, NULL) ==
              RFC_ERR_INVALID_ARGUMENT);
        CHECK(rfc_input_sniff_path(bad3, &sr, NULL, NULL) ==
              RFC_ERR_INVALID_ARGUMENT);
        CHECK(rfc_input_read_path(bad1, RFC_IMPORT_DEFAULT, NULL, NULL, NULL,
                                  &imp) == RFC_ERR_INVALID_ARGUMENT);
        CHECK(rfc_output_check_destination_path(
                  bad1, NULL, NULL, RFC_EXPORT_REFUSE_EXISTING) ==
              RFC_ERR_INVALID_ARGUMENT);
        CHECK(rfc_output_check_destination_path(
                  bad2, NULL, NULL, RFC_EXPORT_REFUSE_EXISTING) ==
              RFC_ERR_INVALID_ARGUMENT);
    }

    /* 2. Oversize read_fn return count is rejected on both sniff and bounded read,
     *    and detail reflects real failure. */
    {
        hostile_read_ctx hc_sniff = {bmp, blen, 0u, 0, 1, 128u};
        rfc_io_reader hr_sniff;
        memset(&hr_sniff, 0, sizeof(hr_sniff));
        hr_sniff.ctx = &hc_sniff;
        hr_sniff.read_fn = hostile_oversize_read;
        hr_sniff.seek_fn = hostile_oversize_seek;

        rfc_sniff_result sr_hostile;
        memset(&sr_hostile, 0, sizeof(sr_hostile));
        CHECK(rfc_input_sniff_reader(&hr_sniff, "a.bmp", &sr_hostile, NULL,
                                     NULL) == RFC_ERR_IO);

        hostile_read_ctx hc = {bmp, blen, 0u, 0, 3, 128u};
        rfc_io_reader hr;
        memset(&hr, 0, sizeof(hr));
        hr.ctx = &hc;
        hr.read_fn = hostile_oversize_read;
        hr.seek_fn = hostile_oversize_seek;

        rfc_imported_input imp;
        memset(&imp, 0, sizeof(imp));
        CHECK(rfc_input_read_bounded(&hr, "a.bmp", RFC_IMPORT_DEFAULT, NULL,
                                     NULL, NULL, &imp) == RFC_ERR_IO);
        CHECK(imp.bytes == NULL);
        CHECK(strncmp(imp.sniff.detail, "sniffed ", 8u) != 0);

        /* Format disagreement on bounded read sets disagreement detail. */
        mem_reader_ctx mrc;
        mem_reader_init(&mrc, bmp, blen);
        rfc_io_reader mr = make_mem_reader(&mrc);
        memset(&imp, 0, sizeof(imp));
        CHECK(rfc_input_read_bounded(&mr, "wrong.png", RFC_IMPORT_DEFAULT, NULL,
                                     NULL, NULL, &imp) ==
              RFC_ERR_FORMAT_DISAGREEMENT);
        CHECK(strncmp(imp.sniff.detail, "sniffed ", 8u) != 0);
        mem_reader_destroy(&mrc);

        /* Limit exceeded in worker read (size unknown at preflight) sets limit detail. */
        mem_reader_ctx slim_rc;
        mem_reader_init(&slim_rc, bmp, blen);
        rfc_io_reader slim_r = make_mem_reader(&slim_rc);
        slim_r.size_fn = NULL;
        mem_pub_ctx slim_pc;
        mem_pub_init(&slim_pc);
        rfc_io_publisher slim_p = make_mem_publisher(&slim_pc);

        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &slim_r, "s.bmp",
                                     RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &slim_p, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        rfc_limits lim;
        rfc_limits_default(&lim);
        lim.max_compressed_bytes = 32u;

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.limits_override = &lim;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);

        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED);
        CHECK(snap.status == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(strncmp(snap.detail, "sniffed ", 8u) != 0);
        CHECK(slim_pc.begin_calls == 0u);

        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        mem_pub_destroy(&slim_pc);
        mem_reader_destroy(&slim_rc);
    }

    /* 3 & 4. Concurrent release_partials + failing commit vs cancel/retry. */
    {
        commit_barrier_ctx b;
        memset(&b, 0, sizeof(b));
        fs_mutex_init(&b.lock);
        fs_cond_init(&b.cond);
        rfc_output_set_commit_barrier_for_testing(NULL, barrier_after_cb, &b);

        mem_reader_ctx rc;
        mem_pub_ctx pc;
        mem_reader_init(&rc, bmp, blen);
        mem_pub_init(&pc);
        pc.fail_commit_once_status = RFC_ERR_DESTINATION_EXISTS;
        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_io_publisher pub = make_mem_publisher(&pc);

        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "r.bmp", RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);
        rfc_job_slot *slot = rfc_job_slot_create();
        CHECK(slot != NULL);

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *j1 = NULL;
        CHECK(rfc_scheduler_submit(sched, slot, &spec, &j1) == RFC_OK);

        fs_mutex_lock(&b.lock);
        while (!b.entered) {
            fs_cond_wait(&b.cond, &b.lock);
        }
        fs_mutex_unlock(&b.lock);

        int attached_freed = 0;
        void *pbuf = malloc(128u);
        CHECK(pbuf != NULL);
        CHECK(rfc_job_attach_partial(j1, pbuf, 128u, audit_partial_free,
                                     &attached_freed) == RFC_OK);

        rfc_scheduler_stats st_mid;
        rfc_scheduler_stats_get(sched, &st_mid);
        CHECK(st_mid.live_partial_bytes > 128u);
        uint64_t expected_staged = st_mid.live_partial_bytes - 128u;

        rfc_job_release_partials(j1);
        CHECK(attached_freed == 1);

        rfc_scheduler_stats st_after_rel;
        rfc_scheduler_stats_get(sched, &st_after_rel);
        CHECK(st_after_rel.live_partial_bytes == expected_staged);

        CHECK(rfc_job_cancel(j1) == RFC_OK);
        rfc_job *j2 = NULL;
        CHECK(rfc_scheduler_retry(sched, slot, &j2) == RFC_OK);

        fs_mutex_lock(&b.lock);
        b.cancel_done = 1;
        fs_cond_broadcast(&b.cond);
        fs_mutex_unlock(&b.lock);

        CHECK(rfc_job_wait(j1, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_wait(j2, UINT32_MAX) == RFC_OK);

        rfc_job_snapshot sn1;
        rfc_job_snapshot sn2;
        CHECK(rfc_job_snapshot_get(j1, &sn1) == RFC_OK);
        CHECK(rfc_job_snapshot_get(j2, &sn2) == RFC_OK);
        CHECK(sn1.state == RFC_JOB_FAILED);
        CHECK(sn1.status == RFC_ERR_DESTINATION_EXISTS);
        CHECK(sn1.failing_stage == RFC_STAGE_EXPORT);
        CHECK(sn1.superseded_by_id == RFC_JOB_ID_NONE);
        CHECK(sn1.live_partial_bytes == 0u);

        rfc_export_report er1;
        CHECK(rfc_job_export_report_get(j1, &er1) == RFC_OK);
        CHECK(er1.valid == 1 && er1.published == 0 && er1.temp_cleaned_up == 1);
        CHECK(er1.status == RFC_ERR_DESTINATION_EXISTS);

        CHECK(sn2.state == RFC_JOB_SUCCEEDED);
        CHECK(sn2.status == RFC_OK);
        CHECK(sn2.attempt_number == 2u);
        CHECK(pc.published == 1);

        rfc_scheduler_stats st_end;
        rfc_scheduler_stats_get(sched, &st_end);
        CHECK(st_end.live_partial_bytes == 0u);
        CHECK(st_end.total_failed == 1u);
        CHECK(st_end.total_completed == 1u);
        CHECK(st_end.total_cancelled == 0u);
        CHECK(st_end.total_superseded == 0u);

        rfc_output_set_commit_barrier_for_testing(NULL, NULL, NULL);
        rfc_job_release(j1);
        rfc_job_release(j2);
        rfc_job_slot_destroy(slot);
        rfc_scheduler_destroy(sched);
        CHECK(rc.close_calls == 1u && pc.release_calls == 1u);
        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
        fs_cond_destroy(&b.cond);
        fs_mutex_destroy(&b.lock);
    }

    /* 5. Finding 3: TOCTOU content swap between sniff and bounded read fails cleanly. */
    {
        const uint8_t qoi[14 + 8] = {'q', 'o', 'i', 'f', 0, 0, 0, 1,
                                     0, 0, 0, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        for (int allow = 0; allow <= 1; allow++) {
            toctou_swap_ctx tc = {bmp, blen, qoi, sizeof(qoi), 0u, 0};
            rfc_io_reader tr;
            memset(&tr, 0, sizeof(tr));
            tr.ctx = &tc;
            tr.read_fn = toctou_swap_read;
            tr.seek_fn = toctou_swap_seek;

            rfc_imported_input imp;
            memset(&imp, 0, sizeof(imp));
            uint32_t fl = allow ? RFC_IMPORT_ALLOW_DISAGREEMENT
                                : RFC_IMPORT_DEFAULT;
            CHECK(rfc_input_read_bounded(&tr, "a.bmp", fl, NULL, NULL, NULL,
                                         &imp) == RFC_ERR_FORMAT_DISAGREEMENT);
            CHECK(imp.bytes == NULL);
        }

        toctou_swap_ctx jtc = {bmp, blen, qoi, sizeof(qoi), 0u, 0};
        rfc_io_reader jtr;
        memset(&jtr, 0, sizeof(jtr));
        jtr.ctx = &jtc;
        jtr.read_fn = toctou_swap_read;
        jtr.seek_fn = toctou_swap_seek;

        mem_pub_ctx pc;
        mem_pub_init(&pc);
        rfc_io_publisher pub = make_mem_publisher(&pc);
        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &jtr, "t.bmp",
                                     RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED);
        CHECK(snap.status == RFC_ERR_FORMAT_DISAGREEMENT);
        CHECK(snap.failing_stage == RFC_STAGE_IMPORT);
        CHECK(pc.begin_calls == 0u);

        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        mem_pub_destroy(&pc);
    }

    /* 6. Finding 7: Container-walker callback I/O error after 16 KiB header propagates. */
    {
        size_t large_txt = 18000u;
        size_t png_sz = 8u + 12u + 13u + 12u + large_txt + 12u + 8u;
        uint8_t *png = (uint8_t *)calloc(1u, png_sz);
        CHECK(png != NULL);
        memcpy(png, "\x89PNG\r\n\x1a\n", 8u);
        size_t p = 8u;
        wr32be(png + p, 13u);
        memcpy(png + p + 4u, "IHDR", 4u);
        p += 12u + 13u;
        wr32be(png + p, (uint32_t)large_txt);
        memcpy(png + p + 4u, "iTXt", 4u);
        p += 12u + large_txt;
        wr32be(png + p, 8u);
        memcpy(png + p + 4u, "acTL", 4u);

        tail_io_err_ctx pc = {png, png_sz, 0u, 16384u};
        rfc_io_reader pr;
        memset(&pr, 0, sizeof(pr));
        pr.ctx = &pc;
        pr.read_fn = tail_io_err_read;
        pr.seek_fn = tail_io_err_seek;
        rfc_sniff_result psr;
        CHECK(rfc_input_sniff_reader(&pr, "a.png", &psr, NULL, NULL) ==
              RFC_ERR_IO);
        free(png);

        uint8_t *tif = (uint8_t *)calloc(1u, 20000u);
        CHECK(tif != NULL);
        tif[0] = 'I';
        tif[1] = 'I';
        wr16le(tif + 2u, 42u);
        wr32le(tif + 4u, 19000u);
        wr16le(tif + 19000u, 1u);
        wr16le(tif + 19002u, 0xC612u);

        tail_io_err_ctx tc = {tif, 20000u, 0u, 16384u};
        rfc_io_reader tr;
        memset(&tr, 0, sizeof(tr));
        tr.ctx = &tc;
        tr.read_fn = tail_io_err_read;
        tr.seek_fn = tail_io_err_seek;
        rfc_sniff_result tsr;
        CHECK(rfc_input_sniff_reader(&tr, "a.tif", &tsr, NULL, NULL) ==
              RFC_ERR_IO);
        free(tif);
    }

    /* 7. Finding 4: Standalone publish_staged checks exec cancellation after flush_stage. */
    {
        blocker_ctx bc;
        memset(&bc, 0, sizeof(bc));
        fs_mutex_init(&bc.lock);
        fs_cond_init(&bc.cond);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_job_spec bspec;
        memset(&bspec, 0, sizeof(bspec));
        bspec.source_format = RFC_FORMAT_RASTERFOLD;
        bspec.target_format = RFC_FORMAT_RASTERFOLD;
        bspec.source_layout = &lay;
        bspec.expected_working_set_bytes = 64u * 1024u;
        bspec.has_stage_ops = 1;
        bspec.stage_ops.on_import = blocker_export_cb;
        bspec.stage_ops.on_encode = fs_noop_stage_cb;
        bspec.stage_ops.on_verify = fs_noop_stage_cb;
        bspec.stage_ops.on_export = fs_noop_stage_cb;
        bspec.stage_ops.user_ctx = &bc;

        rfc_job *j_blk = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &bspec, &j_blk) == RFC_OK);

        fs_mutex_lock(&bc.lock);
        while (!bc.entered) {
            fs_cond_wait(&bc.cond, &bc.lock);
        }
        fs_mutex_unlock(&bc.lock);

        rfc_job_spec qspec;
        memset(&qspec, 0, sizeof(qspec));
        qspec.source_format = RFC_FORMAT_RASTERFOLD;
        qspec.target_format = RFC_FORMAT_RASTERFOLD;
        qspec.source_layout = &lay;
        qspec.expected_working_set_bytes = 64u * 1024u;
        qspec.has_stage_ops = 1;
        qspec.stage_ops.on_import = fs_noop_stage_cb;
        qspec.stage_ops.on_encode = fs_noop_stage_cb;
        qspec.stage_ops.on_verify = fs_noop_stage_cb;
        qspec.stage_ops.on_export = fs_noop_stage_cb;
        rfc_job *j_exec = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &qspec, &j_exec) == RFC_OK);

        mem_pub_ctx spc;
        mem_pub_init(&spc);
        spc.cancel_job_on_flush = j_exec;
        rfc_io_publisher spub = make_mem_publisher(&spc);

        struct rfc_exec_context exec_stand;
        memset(&exec_stand, 0, sizeof(exec_stand));
        exec_stand.job = j_exec;

        rfc_export_report srep;
        memset(&srep, 0, sizeof(srep));
        CHECK(rfc_output_publish_staged(&spub, NULL, 0u, 1u, NULL, bmp, blen,
                                        &exec_stand, &srep) ==
              RFC_ERR_CANCELLED);
        CHECK(spc.flush_calls == 1u);
        CHECK(spc.commit_calls == 0u);
        CHECK(spc.abort_calls == 1u);
        CHECK(srep.published == 0 && srep.temp_cleaned_up == 1);

        fs_mutex_lock(&bc.lock);
        bc.release = 1;
        fs_cond_broadcast(&bc.cond);
        fs_mutex_unlock(&bc.lock);

        CHECK(rfc_job_wait(j_blk, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_wait(j_exec, UINT32_MAX) == RFC_OK);
        rfc_job_release(j_blk);
        rfc_job_release(j_exec);
        rfc_scheduler_destroy(sched);
        mem_pub_destroy(&spc);
        fs_cond_destroy(&bc.cond);
        fs_mutex_destroy(&bc.lock);
    }

    /* 8. Finding 5: Cleanup failure sets temp_cleaned_up=0 and retains partial bytes. */
    {
        mem_reader_ctx rc;
        mem_pub_ctx pc;
        mem_reader_init(&rc, bmp, blen);
        mem_pub_init(&pc);
        pc.fail_commit_always_status = RFC_ERR_IO;
        pc.fail_abort_status = RFC_ERR_PERMISSION_DENIED;
        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_io_publisher pub = make_mem_publisher(&pc);

        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "cf.bmp",
                                     RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);

        rfc_export_report er;
        CHECK(rfc_job_export_report_get(job, &er) == RFC_OK);
        CHECK(er.valid == 1 && er.published == 0 && er.temp_cleaned_up == 0);

        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_FAILED);
        CHECK(snap.live_partial_bytes > 0u);

        rfc_scheduler_stats stats;
        rfc_scheduler_stats_get(sched, &stats);
        CHECK(stats.live_partial_bytes == snap.live_partial_bytes);

        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
    }

    /* 8b. Commit temp_cleaned_up=0 retains staged partial bytes on success. */
    {
        mem_reader_ctx rc;
        mem_pub_ctx pc;
        mem_reader_init(&rc, bmp, blen);
        mem_pub_init(&pc);
        pc.commit_not_cleaned = 1;
        rfc_io_reader rdr = make_mem_reader(&rc);
        rfc_io_publisher pub = make_mem_publisher(&pc);

        rfc_io_source src_io;
        rfc_io_sink dst_io;
        rfc_io_source_init_callbacks(&src_io, &rdr, "c8b.bmp",
                                     RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_callbacks(&dst_io, &pub, RFC_EXPORT_REFUSE_EXISTING);

        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        rfc_job_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source_format = RFC_FORMAT_BMP;
        spec.target_format = RFC_FORMAT_RASTERFOLD;
        spec.expected_working_set_bytes = 64u * 1024u;
        spec.io_source = &src_io;
        spec.io_sink = &dst_io;

        rfc_job *job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);

        rfc_export_report er;
        CHECK(rfc_job_export_report_get(job, &er) == RFC_OK);
        CHECK(er.valid == 1 && er.published == 1 && er.temp_cleaned_up == 0);

        rfc_job_snapshot snap;
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED);
        CHECK(snap.live_partial_bytes > 0u);

        rfc_scheduler_stats stats;
        rfc_scheduler_stats_get(sched, &stats);
        CHECK(stats.live_partial_bytes == snap.live_partial_bytes);

        rfc_job_release(job);
        rfc_scheduler_destroy(sched);
        mem_pub_destroy(&pc);
        mem_reader_destroy(&rc);
    }

    /* 9. POSIX O_TMPFILE/handle-only publish & cleanup races; Win32 share/UNC pinning. */
    {
        char root[512];
        CHECK(make_temp_dir(root, sizeof(root), "race"));
#ifndef _WIN32
        /* 9a. Gap 2: O_TMPFILE leaves zero visible .rfc-partial. entries and
         * planting foreign replacement between commit validation & publish/cleanup
         * succeeds cleanly without touching the foreign file. */
        {
            char d0[700], dst0[900], subst0[900];
            join_path(d0, sizeof(d0), root, "case_otmp");
            CHECK(mkdir(d0, 0755) == 0);
            join_path(dst0, sizeof(dst0), d0, "out.fold");
            join_path(subst0, sizeof(subst0), d0, "out.fold.rfc-partial.subst");

            race_hook_ctx rc;
            memset(&rc, 0, sizeof(rc));
            snprintf(rc.dir, sizeof(rc.dir), "%s", d0);
            rc.mode = 0;

            posix_post_check_ctx pc;
            memset(&pc, 0, sizeof(pc));
            snprintf(pc.subst_path, sizeof(pc.subst_path), "%s", subst0);

            rfc_output_set_commit_barrier_for_testing(race_before_publish_cb,
                                                      NULL, &rc);
            rfc_output_set_posix_post_check_for_testing(posix_post_commit_cb,
                                                        NULL, &pc);

            rfc_export_report er;
            CHECK(rfc_output_write_path(dst0, NULL, NULL,
                                        RFC_EXPORT_REFUSE_EXISTING, NULL, 91u,
                                        bmp, blen, NULL, &er) == RFC_OK);
            rfc_output_set_posix_post_check_for_testing(NULL, NULL, NULL);
            rfc_output_set_commit_barrier_for_testing(NULL, NULL, NULL);

            CHECK(rc.verified == 1 && pc.commit_fired == 1);
            CHECK(er.published == 1 && er.temp_cleaned_up == 1);

            uint8_t *dbuf = NULL;
            size_t dlen = 0u;
            read_file_exact(dst0, &dbuf, &dlen);
            CHECK(dlen == blen && memcmp(dbuf, bmp, blen) == 0);
            free(dbuf);

            uint8_t *sbuf = NULL;
            size_t slen = 0u;
            read_file_exact(subst0, &sbuf, &slen);
            CHECK(slen == 8u && memcmp(sbuf, "ATTACKER", 8u) == 0);
            free(sbuf);

            remove_file_os(subst0);
            remove_file_os(dst0);
            remove_dir_os(d0);
        }

        /* 9b. Gap 1 & Gap 2: Forced fallback + post-check substitution (both
         * handle-link and proc-link faults), abort post-check foreign cleanup,
         * and post-check destination symlink race. */
        {
            char d1[700], dst1[900], vic[900], sub1[900], sym1[900], ab1[900];
            join_path(d1, sizeof(d1), root, "case_fallback");
            CHECK(mkdir(d1, 0755) == 0);
            join_path(dst1, sizeof(dst1), d1, "out.fold");
            join_path(vic, sizeof(vic), d1, "victim.txt");
            join_path(sub1, sizeof(sub1), d1, "out.fold.rfc-partial.subst");
            join_path(sym1, sizeof(sym1), d1, "out.fold.rfc-partial.sym");
            join_path(ab1, sizeof(ab1), d1, "out.fold.rfc-partial.abort");
            const uint8_t vdata[] = "VICTIM";
            write_file_exact(vic, vdata, sizeof(vdata) - 1u);

            /* 9b-1: Force handle-link refusal with post-commit & post-abort planting. */
            posix_post_check_ctx pc1;
            memset(&pc1, 0, sizeof(pc1));
            snprintf(pc1.subst_path, sizeof(pc1.subst_path), "%s", sub1);
            snprintf(pc1.sym_path, sizeof(pc1.sym_path), "%s", sym1);
            snprintf(pc1.sym_target, sizeof(pc1.sym_target), "%s", vic);
            snprintf(pc1.abort_path, sizeof(pc1.abort_path), "%s", ab1);

            rfc_output_set_posix_post_check_for_testing(posix_post_commit_cb,
                                                        posix_post_abort_cb,
                                                        &pc1);
            rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_POSIX_HANDLE_LINK,
                                                   0u, RFC_ERR_IO);

            rfc_export_report er1;
            CHECK(rfc_output_write_path(dst1, NULL, NULL,
                                        RFC_EXPORT_REFUSE_EXISTING, NULL, 92u,
                                        bmp, blen, NULL, &er1) == RFC_ERR_IO);
            rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_NONE, 0u,
                                                   RFC_OK);
            rfc_output_set_posix_post_check_for_testing(NULL, NULL, NULL);

            CHECK(pc1.commit_fired == 1 && pc1.abort_fired == 1);
            CHECK(er1.published == 0 && er1.temp_cleaned_up == 1);
            CHECK(!file_exists_os(dst1));

            uint8_t *sbuf = NULL;
            size_t slen = 0u;
            read_file_exact(sub1, &sbuf, &slen);
            CHECK(slen == 8u && memcmp(sbuf, "ATTACKER", 8u) == 0);
            free(sbuf);

            struct stat lst;
            CHECK(lstat(sym1, &lst) == 0 && S_ISLNK(lst.st_mode));

            uint8_t *abuf = NULL;
            size_t alen = 0u;
            read_file_exact(ab1, &abuf, &alen);
            CHECK(alen == 13u && memcmp(abuf, "FOREIGN_ABORT", 13u) == 0);
            free(abuf);

            uint8_t *vbuf = NULL;
            size_t vlen = 0u;
            read_file_exact(vic, &vbuf, &vlen);
            CHECK(vlen == 6u && memcmp(vbuf, "VICTIM", 6u) == 0);
            free(vbuf);

            remove_file_os(sub1);
            remove_file_os(sym1);
            remove_file_os(ab1);

            /* 9b-2: Force /proc/self/fd link fallback (AT_EMPTY_PATH or safe refusal). */
            posix_post_check_ctx pc2;
            memset(&pc2, 0, sizeof(pc2));
            snprintf(pc2.subst_path, sizeof(pc2.subst_path), "%s", sub1);
            snprintf(pc2.abort_path, sizeof(pc2.abort_path), "%s", ab1);

            rfc_output_set_posix_post_check_for_testing(posix_post_commit_cb,
                                                        posix_post_abort_cb,
                                                        &pc2);
            rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_POSIX_PROC_LINK,
                                                   0u, RFC_ERR_IO);

            rfc_export_report er2;
            rfc_status st2 = rfc_output_write_path(
                dst1, NULL, NULL, RFC_EXPORT_REFUSE_EXISTING, NULL, 93u, bmp,
                blen, NULL, &er2);
            rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_NONE, 0u,
                                                   RFC_OK);
            rfc_output_set_posix_post_check_for_testing(NULL, NULL, NULL);

            CHECK(pc2.commit_fired == 1);
            if (st2 == RFC_OK) {
                CHECK(er2.published == 1 && er2.temp_cleaned_up == 1);
                uint8_t *pbuf = NULL;
                size_t plen = 0u;
                read_file_exact(dst1, &pbuf, &plen);
                CHECK(plen == blen && memcmp(pbuf, bmp, blen) == 0);
                free(pbuf);
                remove_file_os(dst1);
            } else {
                CHECK(st2 == RFC_ERR_IO);
                CHECK(er2.published == 0 && er2.temp_cleaned_up == 1);
                CHECK(pc2.abort_fired == 1);
                CHECK(!file_exists_os(dst1));
                uint8_t *ab2 = NULL;
                size_t ab2l = 0u;
                read_file_exact(ab1, &ab2, &ab2l);
                CHECK(ab2l == 13u && memcmp(ab2, "FOREIGN_ABORT", 13u) == 0);
                free(ab2);
                remove_file_os(ab1);
            }
            read_file_exact(sub1, &sbuf, &slen);
            CHECK(slen == 8u && memcmp(sbuf, "ATTACKER", 8u) == 0);
            free(sbuf);
            remove_file_os(sub1);

            /* 9b-3: Post-check symlink created at destination before linkat. */
            posix_post_check_ctx pc3;
            memset(&pc3, 0, sizeof(pc3));
            snprintf(pc3.dest_sym_path, sizeof(pc3.dest_sym_path), "%s", dst1);
            snprintf(pc3.dest_sym_target, sizeof(pc3.dest_sym_target), "%s",
                     vic);
            rfc_output_set_posix_post_check_for_testing(posix_post_commit_cb,
                                                        posix_post_abort_cb,
                                                        &pc3);

            rfc_export_report er3;
            CHECK(rfc_output_write_path(dst1, NULL, NULL,
                                        RFC_EXPORT_REFUSE_EXISTING, NULL, 94u,
                                        bmp, blen, NULL, &er3) ==
                  RFC_ERR_PATH_ALIAS);
            rfc_output_set_posix_post_check_for_testing(NULL, NULL, NULL);
            CHECK(pc3.commit_fired == 1 && pc3.abort_fired == 1);
            CHECK(er3.published == 0 && er3.temp_cleaned_up == 1);
            read_file_exact(vic, &vbuf, &vlen);
            CHECK(vlen == 6u && memcmp(vbuf, "VICTIM", 6u) == 0);
            free(vbuf);

            remove_file_os(dst1); /* symlink */
            remove_file_os(vic);

            /* 9b-4: Post-link destination rename treats linkat as publication
             * boundary so reporting and cancellation stay consistent. */
            {
                char dst4[900], moved4[900];
                join_path(dst4, sizeof(dst4), d1, "postlink_direct.fold");
                join_path(moved4, sizeof(moved4), d1, "moved_direct.fold");

                posix_post_link_ctx pl;
                memset(&pl, 0, sizeof(pl));
                snprintf(pl.src_path, sizeof(pl.src_path), "%s", dst4);
                snprintf(pl.moved_path, sizeof(pl.moved_path), "%s", moved4);
                rfc_output_set_posix_post_link_for_testing(posix_post_link_cb,
                                                           &pl);

                rfc_export_report er4;
                CHECK(rfc_output_write_path(dst4, NULL, NULL,
                                            RFC_EXPORT_REFUSE_EXISTING, NULL,
                                            940u, bmp, blen, NULL,
                                            &er4) == RFC_OK);
                rfc_output_set_posix_post_link_for_testing(NULL, NULL);
                CHECK(pl.fired == 1);
                CHECK(er4.published == 1 && er4.status == RFC_OK &&
                      er4.temp_cleaned_up == 1);
                CHECK(!file_exists_os(dst4));
                uint8_t *mbuf = NULL;
                size_t mlen = 0u;
                read_file_exact(moved4, &mbuf, &mlen);
                CHECK(mlen == blen && memcmp(mbuf, bmp, blen) == 0);
                free(mbuf);
                remove_file_os(moved4);

                char dstj[900], movedj[900];
                join_path(dstj, sizeof(dstj), d1, "postlink_job.fold");
                join_path(movedj, sizeof(movedj), d1, "moved_job.fold");

                mem_reader_ctx mrc;
                mem_reader_init(&mrc, bmp, blen);
                rfc_io_reader rdr = make_mem_reader(&mrc);
                rfc_io_source src_io;
                rfc_io_sink dst_io;
                rfc_io_source_init_callbacks(&src_io, &rdr, "postlink.bmp",
                                             RFC_IMPORT_DEFAULT);
                rfc_io_sink_init_path(&dst_io, dstj, RFC_EXPORT_REFUSE_EXISTING);

                rfc_scheduler_config cfg;
                rfc_scheduler_config_default(&cfg);
                cfg.max_workers = 1u;
                rfc_scheduler *sched = rfc_scheduler_create(&cfg);
                CHECK(sched != NULL);

                posix_post_link_ctx plj;
                memset(&plj, 0, sizeof(plj));
                fs_mutex_init(&plj.lock);
                fs_cond_init(&plj.cond);
                plj.sync_enabled = 1;
                snprintf(plj.src_path, sizeof(plj.src_path), "%s", dstj);
                snprintf(plj.moved_path, sizeof(plj.moved_path), "%s", movedj);

                rfc_job_spec spec;
                memset(&spec, 0, sizeof(spec));
                spec.source_format = RFC_FORMAT_BMP;
                spec.target_format = RFC_FORMAT_RASTERFOLD;
                spec.expected_working_set_bytes = 64u * 1024u;
                spec.io_source = &src_io;
                spec.io_sink = &dst_io;

                rfc_output_set_posix_post_link_for_testing(posix_post_link_cb,
                                                           &plj);
                rfc_job *job = NULL;
                CHECK(rfc_scheduler_submit(sched, NULL, &spec, &job) == RFC_OK);

                fs_mutex_lock(&plj.lock);
                plj.job = job;
                while (!plj.entered) {
                    fs_cond_wait(&plj.cond, &plj.lock);
                }
                fs_mutex_unlock(&plj.lock);

                fs_thread_t cth;
                CHECK(fs_thread_start(&cth, post_link_cancel_worker, &plj));
                fs_mutex_lock(&plj.lock);
                while (!plj.cancel_dispatched) {
                    fs_cond_wait(&plj.cond, &plj.lock);
                }
                plj.allow_finish = 1;
                fs_cond_broadcast(&plj.cond);
                fs_mutex_unlock(&plj.lock);

                fs_thread_join(cth);
                CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
                rfc_output_set_posix_post_link_for_testing(NULL, NULL);

                rfc_job_snapshot snap;
                CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
                CHECK(snap.state == RFC_JOB_SUCCEEDED);
                CHECK(snap.status == RFC_OK);

                rfc_export_report erj;
                CHECK(rfc_job_export_report_get(job, &erj) == RFC_OK);
                CHECK(erj.valid == 1 && erj.published == 1 &&
                      erj.status == RFC_OK && erj.temp_cleaned_up == 1);

                rfc_scheduler_stats stats;
                rfc_scheduler_stats_get(sched, &stats);
                CHECK(stats.published_cancelled_count == 0u);
                CHECK(stats.total_completed == 1u);
                CHECK(stats.total_cancelled == 0u);
                CHECK(plj.fired == 1);
                CHECK(!file_exists_os(dstj));

                uint8_t *jbuf = NULL;
                size_t jlen = 0u;
                read_file_exact(movedj, &jbuf, &jlen);
                CHECK(jlen == (size_t)erj.bytes_written && jlen > 32u);
                uint8_t jsha[32];
                rfc_bytes_hash_sha256(jbuf, jlen, jsha);
                CHECK(memcmp(jsha, erj.output_sha256, 32u) == 0);
                rf_pixels jpx = {0};
                rf_error jerr = {0, NULL};
                CHECK(rf_decode_image(jbuf, jlen, NULL, &jpx, &jerr) == RF_OK);
                CHECK(jpx.width == 4u && jpx.height == 4u);
                rf_free(jpx.data);
                free(jbuf);
                remove_file_os(movedj);

                rfc_job_release(job);
                rfc_scheduler_destroy(sched);
                fs_cond_destroy(&plj.cond);
                fs_mutex_destroy(&plj.lock);
                mem_reader_destroy(&mrc);
            }

            remove_dir_os(d1);
        }

        /* 9c. Parent directory substitution -> symlink to attacker_dir */
        {
            char d2[700], sub[900], mov[900], att[900], dst2[1100], at_dst[1100],
                mv_dst[1100];
            join_path(d2, sizeof(d2), root, "case_dirswap");
            CHECK(mkdir(d2, 0755) == 0);
            join_path(sub, sizeof(sub), d2, "sub");
            join_path(mov, sizeof(mov), d2, "moved");
            join_path(att, sizeof(att), d2, "attacker");
            CHECK(mkdir(sub, 0755) == 0);
            CHECK(mkdir(att, 0755) == 0);
            join_path(dst2, sizeof(dst2), sub, "out.fold");
            join_path(at_dst, sizeof(at_dst), att, "out.fold");
            join_path(mv_dst, sizeof(mv_dst), mov, "out.fold");

            race_hook_ctx rc;
            memset(&rc, 0, sizeof(rc));
            snprintf(rc.dir, sizeof(rc.dir), "%s", sub);
            snprintf(rc.target_or_moved, sizeof(rc.target_or_moved), "%s", mov);
            snprintf(rc.attacker_dir, sizeof(rc.attacker_dir), "%s", att);
            rc.mode = 2;
            rfc_output_set_commit_barrier_for_testing(race_before_publish_cb,
                                                      NULL, &rc);

            rfc_export_report er;
            (void)rfc_output_write_path(dst2, NULL, NULL,
                                        RFC_EXPORT_REFUSE_EXISTING, NULL, 95u,
                                        bmp, blen, NULL, &er);
            rfc_output_set_commit_barrier_for_testing(NULL, NULL, NULL);
            CHECK(rc.verified == 1);
            CHECK(!file_exists_os(at_dst));
            remove_file_os(mv_dst);
            remove_file_os(sub); /* symlink */
            remove_dir_os(att);
            remove_dir_os(mov);
            remove_dir_os(d2);
        }

        /* 9d. chmod 0555 refuses publish cleanly with anonymous temp cleanup,
         * and RFC_IO_FAULT_POSIX_NO_TMPFILE refuses cleanly without partial. */
        {
            char d3[700], dst3[900];
            join_path(d3, sizeof(d3), root, "case_ro");
            CHECK(mkdir(d3, 0755) == 0);
            join_path(dst3, sizeof(dst3), d3, "out.fold");

            if (geteuid() != 0) {
                race_hook_ctx rc;
                memset(&rc, 0, sizeof(rc));
                snprintf(rc.dir, sizeof(rc.dir), "%s", d3);
                rc.mode = 3;
                rfc_output_set_commit_barrier_for_testing(
                    race_before_publish_cb, NULL, &rc);

                rfc_export_report er;
                CHECK(rfc_output_write_path(dst3, NULL, NULL,
                                            RFC_EXPORT_REFUSE_EXISTING, NULL,
                                            96u, bmp, blen, NULL, &er) ==
                      RFC_ERR_PERMISSION_DENIED);
                rfc_output_set_commit_barrier_for_testing(NULL, NULL, NULL);
                CHECK(rc.verified == 1 && er.published == 0 &&
                      er.temp_cleaned_up == 1);
                CHECK(chmod(d3, 0755) == 0);
                CHECK(!file_exists_os(dst3));
                CHECK(dir_has_prefix(d3, "out.fold.rfc-partial.") == 0);
            }

            rfc_output_set_write_fault_for_testing(
                RFC_IO_FAULT_POSIX_NO_TMPFILE, 0u, RFC_ERR_IO);
            rfc_export_report er_nt;
            CHECK(rfc_output_write_path(dst3, NULL, NULL,
                                        RFC_EXPORT_REFUSE_EXISTING, NULL, 97u,
                                        bmp, blen, NULL, &er_nt) ==
                  RFC_ERR_IO);
            rfc_output_set_write_fault_for_testing(RFC_IO_FAULT_NONE, 0u,
                                                   RFC_OK);
            CHECK(er_nt.published == 0 && !file_exists_os(dst3));
            CHECK(dir_has_prefix(d3, "out.fold.rfc-partial.") == 0);
            remove_dir_os(d3);
        }
#else
        /* 9e. Win32 share-mode pinning locks both partial file and parent dir while staged */
        {
            char d4[700], sub4[900], mov4[900], dst4[1100];
            wchar_t wd4[700], ws4[900];
            join_path(d4, sizeof(d4), root, "case_winpin");
            CHECK(utf8_to_wide(d4, wd4, 700u));
            CHECK(CreateDirectoryW(wd4, NULL));
            join_path(sub4, sizeof(sub4), d4, "sub");
            join_path(mov4, sizeof(mov4), d4, "moved");
            CHECK(utf8_to_wide(sub4, ws4, 900u));
            CHECK(CreateDirectoryW(ws4, NULL));
            join_path(dst4, sizeof(dst4), sub4, "out.fold");

            race_hook_ctx rc;
            memset(&rc, 0, sizeof(rc));
            snprintf(rc.dir, sizeof(rc.dir), "%s", sub4);
            snprintf(rc.target_or_moved, sizeof(rc.target_or_moved), "%s", mov4);
            rc.mode = 4;
            rfc_output_set_commit_barrier_for_testing(race_before_publish_cb,
                                                      NULL, &rc);

            rfc_export_report er;
            CHECK(rfc_output_write_path(dst4, NULL, NULL,
                                        RFC_EXPORT_REFUSE_EXISTING, NULL, 95u,
                                        bmp, blen, NULL, &er) == RFC_OK);
            rfc_output_set_commit_barrier_for_testing(NULL, NULL, NULL);
            CHECK(rc.verified == 1 && er.published == 1 &&
                  er.temp_cleaned_up == 1);
            CHECK(file_exists_os(dst4));
            remove_file_os(dst4);
            remove_dir_os(sub4);
            remove_dir_os(d4);
        }

        /* 9f. Gap 3: Win32 UNC share-root anchoring, publish, existing-target refusal,
         * nested subdir, malformed UNC, and junction share-root refusal. */
        {
            char uroot[700], usub[900], ujunc[900], uout_back[1100],
                usub_back[1100];
            wchar_t wuroot[700], wusub[900], wujunc[900];
            join_path(uroot, sizeof(uroot), root, "unc_backing");
            join_path(usub, sizeof(usub), uroot, "sub");
            join_path(ujunc, sizeof(ujunc), root, "unc_junc");
            join_path(uout_back, sizeof(uout_back), uroot, "output.fold");
            join_path(usub_back, sizeof(usub_back), usub, "nested.fold");
            CHECK(utf8_to_wide(uroot, wuroot, 700u));
            CHECK(CreateDirectoryW(wuroot, NULL));
            CHECK(utf8_to_wide(usub, wusub, 900u));
            CHECK(CreateDirectoryW(wusub, NULL));
            win32_make_junction(ujunc, uroot);
            CHECK(utf8_to_wide(ujunc, wujunc, 900u));

            rfc_output_set_win32_unc_root_for_testing("\\\\rf-unc-srv\\share",
                                                      uroot);

            CHECK(rfc_output_check_destination_path(
                      "\\\\rf-unc-srv\\share\\output.fold", NULL, NULL,
                      RFC_EXPORT_REFUSE_EXISTING) == RFC_OK);

            rfc_export_report er;
            CHECK(rfc_output_write_path("\\\\rf-unc-srv\\share\\output.fold",
                                        NULL, NULL, RFC_EXPORT_REFUSE_EXISTING,
                                        NULL, 98u, bmp, blen, NULL,
                                        &er) == RFC_OK);
            CHECK(er.published == 1 && er.temp_cleaned_up == 1);

            uint8_t *obuf = NULL;
            size_t olen = 0u;
            read_file_exact(uout_back, &obuf, &olen);
            CHECK(olen == blen && memcmp(obuf, bmp, blen) == 0);
            free(obuf);

            CHECK(rfc_output_check_destination_path(
                      "\\\\rf-unc-srv\\share\\output.fold", NULL, NULL,
                      RFC_EXPORT_REFUSE_EXISTING) ==
                  RFC_ERR_DESTINATION_EXISTS);

            uint8_t other[64];
            memset(other, 0x5Au, sizeof(other));
            rfc_export_report er_ex;
            CHECK(rfc_output_write_path("\\\\rf-unc-srv\\share\\output.fold",
                                        NULL, NULL, RFC_EXPORT_REFUSE_EXISTING,
                                        NULL, 99u, other, sizeof(other), NULL,
                                        &er_ex) == RFC_ERR_DESTINATION_EXISTS);
            CHECK(er_ex.published == 0);
            read_file_exact(uout_back, &obuf, &olen);
            CHECK(olen == blen && memcmp(obuf, bmp, blen) == 0);
            free(obuf);

            rfc_export_report er_sub;
            CHECK(rfc_output_write_path(
                      "\\\\rf-unc-srv\\share\\sub\\nested.fold", NULL, NULL,
                      RFC_EXPORT_REFUSE_EXISTING, NULL, 100u, bmp, blen, NULL,
                      &er_sub) == RFC_OK);
            CHECK(er_sub.published == 1 && er_sub.temp_cleaned_up == 1);
            read_file_exact(usub_back, &obuf, &olen);
            CHECK(olen == blen && memcmp(obuf, bmp, blen) == 0);
            free(obuf);

            CHECK(rfc_output_check_destination_path(
                      "\\\\rf-unc-srv\\", NULL, NULL,
                      RFC_EXPORT_REFUSE_EXISTING) == RFC_ERR_INVALID_ARGUMENT);
            CHECK(rfc_output_check_destination_path(
                      "\\\\rf-unc-srv", NULL, NULL,
                      RFC_EXPORT_REFUSE_EXISTING) == RFC_ERR_INVALID_ARGUMENT);
            CHECK(rfc_output_check_destination_path(
                      "\\\\rf-unc-srv\\output.fold", NULL, NULL,
                      RFC_EXPORT_REFUSE_EXISTING) == RFC_ERR_INVALID_ARGUMENT);

            rfc_output_set_win32_unc_root_for_testing("\\\\rf-unc-srv\\jshare",
                                                      ujunc);
            CHECK(rfc_output_check_destination_path(
                      "\\\\rf-unc-srv\\jshare\\x.fold", NULL, NULL,
                      RFC_EXPORT_REFUSE_EXISTING) == RFC_ERR_PATH_ALIAS);
            rfc_export_report er_j;
            CHECK(rfc_output_write_path("\\\\rf-unc-srv\\jshare\\x.fold", NULL,
                                        NULL, RFC_EXPORT_REFUSE_EXISTING, NULL,
                                        101u, bmp, blen, NULL,
                                        &er_j) == RFC_ERR_PATH_ALIAS);
            CHECK(er_j.published == 0);

            rfc_output_set_win32_unc_root_for_testing(NULL, NULL);
            CHECK(RemoveDirectoryW(wujunc) != FALSE);
            remove_file_os(usub_back);
            remove_file_os(uout_back);
            remove_dir_os(usub);
            remove_dir_os(uroot);
        }
#endif
        remove_dir_os(root);
    }

    printf("file-safety: audit_regressions ok\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(rfc_registry_register_session(&g_rf_test_adp) == RFC_OK);
    CHECK(rfc_registry_register_session(&g_bmp_test_adp) == RFC_OK);

    int rc = 0;
    if (argc == 3 && strcmp(argv[1], "sniff") == 0) {
        rc = cmd_sniff(argv[2]);
    } else if ((argc == 3 || argc == 4) &&
               strcmp(argv[1], "read-check") == 0) {
        uint32_t flags = (argc == 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 0u;
        rc = cmd_read_check(argv[2], flags);
    } else if (argc == 5 && strcmp(argv[1], "export-refuse") == 0) {
        uint32_t flags = (uint32_t)strtoul(argv[4], NULL, 10);
        rc = cmd_export_refuse(argv[2], argv[3], flags);
    } else if ((argc == 4 || argc == 5 || argc == 6) &&
               strcmp(argv[1], "pipeline-fold") == 0) {
        uint32_t flags = (argc >= 5) ? (uint32_t)strtoul(argv[4], NULL, 10) : 0u;
        int ovr = (argc == 6) ? (int)strtol(argv[5], NULL, 10) : 0;
        rc = cmd_pipeline_fold(argv[2], argv[3], flags, ovr);
    } else if (argc == 5 && strcmp(argv[1], "write-fault") == 0) {
        size_t sz = (size_t)strtoull(argv[3], NULL, 10);
        int fst = (int)strtol(argv[4], NULL, 10);
        rc = cmd_write_fault(argv[2], sz, fst);
    } else if (argc == 1) {
        test_sniffing();
        test_conflicts_and_bounds();
        test_path_identity_and_refusals();
        test_disk_full_and_cleanup();
        test_callback_ownership_and_retry();
        test_commit_cancel_serialization();
        test_fold_roundtrip();
        test_audit_regressions();
        printf("file-safety: all checks passed\n");
    } else {
        fprintf(stderr, "usage: test_file_safety [sniff|export-refuse|pipeline-fold|write-fault ...]\n");
        rc = 2;
    }

    rfc_registry_unregister_session(RFC_FORMAT_BMP);
    rfc_registry_unregister_session(RFC_FORMAT_RASTERFOLD);
    return rc;
}
