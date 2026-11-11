/* Internal codec and platform declarations. Public ABI: include/rasterfold.h. */
#ifndef RF_H
#define RF_H

#include <stddef.h>
#include <stdint.h>
#include "../include/rasterfold.h"

#ifdef _WIN32
typedef uint16_t rf_path_char;
#else
typedef char rf_path_char;
#endif


#define RF_MAX_DIM 32768u
#define RF_MAX_PIXELS 33554432ull
#define RF_MAX_FILE 268435456ull

#define RF_HEADER_SIZE 32u
#define RF_VERSION 3u
#define RF_FORMAT_RGB8 1u
#define RF_METHOD_RAW 0u
#define RF_METHOD_CM 1u
#define RF_METHOD_PAL 2u
#define RF_METHOD_PAL2 3u
#define RF_METHOD_PAL3 4u      /* compact palette; version 3 files only */

#define RF_INLINE static inline __attribute__((always_inline))
#define RF_NOINLINE __attribute__((noinline))

/* An image is packed top-down RGB, 3 bytes per pixel, no row padding. */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *rgb;
} rf_image;

/* sys.c */
void *rf_alloc(size_t size);            /* 64-byte aligned, zero-filled, or NULL */
void rf_free(void *p);
extern uint64_t rf_internal_alloc_peak;
int rf_fail(rf_error *err, int code, const char *msg); /* records the first error; returns code */
void rf_account_add(size_t size);
void rf_account_remove(size_t size);
int rf_read_file(const rf_path_char *path, uint8_t **data, size_t *size, rf_error *err);
int rf_path_blocked(const rf_path_char *path, rf_error *err);
int rf_write_atomic(const rf_path_char *path, const uint8_t *data, size_t size, rf_error *err);
int rf_write_atomic_replace(const rf_path_char *path, const uint8_t *data, size_t size, rf_error *err);
uint64_t rf_ticks(void);
uint64_t rf_elapsed_us(uint64_t start, uint64_t end);
uint64_t rf_peak_working_set(void);
void *rf_thread_create(void (*fn)(void *), void *arg);  /* suspended; NULL on failure */
void rf_thread_resume(void *thread);
void rf_thread_join(void *thread);                      /* waits, then closes the handle */
void rf_yield(void);
int rf_cpu_count(void);
void rf_print(int to_stderr, const char *s);
void rf_print_u64(int to_stderr, uint64_t v);
void rf_print_hex32(int to_stderr, uint32_t v);

/* crc32.c: zlib / PNG CRC-32 */
uint32_t rf_crc32(const uint8_t *p, size_t n);

/* bmp.c */
void rf_bmp_header(uint8_t *b, uint32_t w, uint32_t h);
void rf_bmp_write_raw(uint8_t *bmp, size_t stride, const uint8_t *rgb, uint32_t w, uint32_t h);
int rf_bmp_alloc(uint32_t w, uint32_t h, uint8_t **file, size_t *size, size_t *stride, rf_error *err);
int rf_bmp_load(const uint8_t *file, size_t size, rf_image *out, rf_error *err);
int rf_bmp_store(const rf_image *img, uint8_t **file, size_t *size, rf_error *err);

/* zlib.c */
uint32_t rf_adler32(const uint8_t *data, size_t size);
int rf_zlib_inflate(const uint8_t *in, size_t size, uint8_t *out, size_t out_size, rf_error *err);
int rf_zlib_deflate(const uint8_t *in, size_t size, uint8_t **out, size_t *out_size, rf_error *err);

/* ppm.c */
int rf_ppm_load(const uint8_t *file, size_t size, rf_image *out, rf_error *err);
int rf_ppm_store(const rf_image *img, uint8_t **file, size_t *size, rf_error *err);

/* png.c */
int rf_png_load(const uint8_t *file, size_t size, rf_image *out, rf_error *err);
int rf_png_store(const rf_image *img, uint8_t **file, size_t *size, rf_error *err);

/* fold.c */
int rf_encode(const rf_image *img, uint8_t **file, size_t *size, int threads, rf_error *err);
int rf_decode(const uint8_t *file, size_t size, rf_image *out, rf_info *info, int threads, rf_error *err);
int rf_decode_bmp(const uint8_t *file, size_t size, uint8_t **bmp, size_t *bmp_size, int threads, rf_error *err);

/* cm.c: the context-mixing method. For strip count S in {1, 2, 3}, rf_cm_encode
   codes strip s channel c into a region of w * h_s + (64 / S) bytes after the
   1 + 12 * S byte strip table (so out needs 1 + 12 * S + 3 * w * h + 3 * S *
   (64 / S) <= 3 * w * h + 226 writable bytes) and sets *written to 0 when the
   payload would not fit in cap bytes or a stream overflows its region; its
   return value is an error code. threads >= 2 codes on multiple threads (up to
   3 * S); the bytes are the same for any thread count. When stride is 0,
   rf_cm_decode writes packed top-down RGB to out; otherwise it writes bottom-up
   BGR rows of stride bytes starting at out + 54. */
int rf_cm_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap, size_t *written, int threads, rf_error *err);
int rf_cm_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, int threads, int version, rf_error *err);

/* pal.c: methods 2 and 3, palette with equality contexts and binary range coding. */
uint32_t rf_pal_build(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *pal);
int rf_pal_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap, size_t *written, rf_error *err);
int rf_pal_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, uint32_t method, rf_error *err);

/* mem.c */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
