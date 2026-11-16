/* Converter-private declarations for librasterfold.a's existing D07 CLI
 * image readers/writers and shared constants matching src/rf.h, keeping
 * src/ and include/rasterfold.h untouched. */
#ifndef RF_SHARED_IO_H
#define RF_SHARED_IO_H

#include <stddef.h>
#include <stdint.h>

#include "rasterfold.h"

#ifdef __cplusplus
extern "C" {
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
#define RF_METHOD_PAL3 4u
#define RF_METHOD_SC 5u
#define RF_METHOD_SC2 6u

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *rgb;
} rf_image;

extern uint64_t rf_internal_alloc_peak;

uint32_t rf_crc32(const uint8_t *p, size_t n);
int rf_bmp_load(const uint8_t *file, size_t size, rf_image *out, rf_error *err);
int rf_bmp_store(const rf_image *img, uint8_t **file, size_t *size, rf_error *err);
int rf_ppm_load(const uint8_t *file, size_t size, rf_image *out, rf_error *err);
int rf_ppm_store(const rf_image *img, uint8_t **file, size_t *size, rf_error *err);
int rf_png_load(const uint8_t *file, size_t size, rf_image *out, rf_error *err);
int rf_png_store(const rf_image *img, uint8_t **file, size_t *size, rf_error *err);

#ifdef __cplusplus
}
#endif

#endif /* RF_SHARED_IO_H */
