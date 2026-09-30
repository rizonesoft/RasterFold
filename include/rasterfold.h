/* RasterFold C API, version 1. All functions are safe for concurrent calls
 * with distinct output/error objects. The library never reads or writes files.
 * Include this header from C11 or C++. Link the static archive normally; define
 * RF_SHARED when importing the Windows DLL. */
#ifndef RASTERFOLD_H
#define RASTERFOLD_H
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(RF_SHARED)
#define RF_API __declspec(dllimport)
#elif !defined(_WIN32)
#define RF_API __attribute__((visibility("default")))
#else
#define RF_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define RF_API_VERSION_MAJOR 1u
#define RF_API_VERSION_MINOR 0u
#define RF_API_VERSION ((RF_API_VERSION_MAJOR << 16) | RF_API_VERSION_MINOR)
#define RF_LAYOUT_RGB8 1u
#define RF_OK 0
#define RF_ERR_USAGE 1
#define RF_ERR_IO 2
#define RF_ERR_UNSUPPORTED 3
#define RF_ERR_FORMAT 4
#define RF_ERR_LIMIT 5

/* Initialize size to sizeof(rf_options), threads to 0 (automatic) or 1..9,
 * and reserved to zero. NULL selects defaults. The original 8-byte prefix
 * (size, threads) is accepted. Sizes must be multiples of four; a size larger
 * than this library's structure is refused. Known reserved fields must be zero. */
typedef struct {
    uint32_t size, threads;
    uint32_t reserved[4];
} rf_options;

/* Packed or padded top-down RGB8. size covers (height-1)*stride+width*3
 * accessible bytes. Input memory is borrowed for the call. Decoding returns
 * tightly packed memory owned by the caller; release data using rf_free. */
typedef struct {
    uint32_t width, height;
    size_t stride;
    uint32_t layout;
    uint8_t *data;
    size_t size;
} rf_pixels;

/* Encoding returns an owned .fold buffer; release data using rf_free. */
typedef struct { uint8_t *data; size_t size; } rf_buffer;

/* Per-call result. message points to a static string in the loaded library;
 * never free it. Success sets code=0 and message=NULL. err may be NULL. */
typedef struct { int code; const char *message; } rf_error;

/* Validated format metadata. stream_size is zero for the raw method. */
typedef struct {
    uint32_t width, height, method;
    uint32_t payload_size, payload_crc, header_crc;
    uint32_t stream_size[3];
} rf_info;

/* Returns the packed API major/minor version, independent of .fold version. */
RF_API uint32_t rf_version(void);
/* Encode RGB8. out is required and must not alias input, options or err.
 * On failure out is zeroed. Existing out.data is never freed implicitly. */
RF_API int rf_encode_image(const rf_pixels *pixels, const rf_options *options,
                           rf_buffer *out, rf_error *err);
/* Decode a complete .fold file. out follows the same ownership/alias rules;
 * on failure it is zeroed. bytes is borrowed and must cover size bytes. */
RF_API int rf_decode_image(const void *bytes, size_t size, const rf_options *options,
                           rf_pixels *out, rf_error *err);
/* Validate the complete file, including decoding its payload, and fill info.
 * This is not a header-only fast path. info is required and zeroed on failure. */
RF_API int rf_inspect(const void *bytes, size_t size, rf_info *info, rf_error *err);
/* Free a pointer returned by this library. NULL is allowed. Never pass a
 * caller allocation or a pointer obtained from another loaded codec copy. */
RF_API void rf_free(void *pointer);

#ifdef __cplusplus
}
#endif
#endif
