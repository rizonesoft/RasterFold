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

/* Packed API version: (major << 16) | minor. This versions the C API only;
 * the .fold format carries its own version in every file. */
#define RF_API_VERSION_MAJOR 1u
#define RF_API_VERSION_MINOR 0u
#define RF_API_VERSION ((RF_API_VERSION_MAJOR << 16) | RF_API_VERSION_MINOR)
/* The only pixel layout in API version 1: 8-bit RGB, no alpha. */
#define RF_LAYOUT_RGB8 1u
/* Return codes. Every function that can fail returns RF_OK (0) or one of
 * these and, when err is not NULL, also fills *err with the same code:
 *   RF_OK success; RF_ERR_USAGE a required pointer is NULL or inconsistent
 *   (non-overlap below is a caller precondition, not a detected error); RF_ERR_IO reserved for callers' I/O layers (the library
 *   does no I/O); RF_ERR_UNSUPPORTED a layout, option size, or reserved
 *   field this version refuses; RF_ERR_FORMAT the input is not a
 *   well-formed .fold file or image; RF_ERR_LIMIT a size limit would be
 *   exceeded, or allocation failed. */
#define RF_OK 0
#define RF_ERR_USAGE 1
#define RF_ERR_IO 2
#define RF_ERR_UNSUPPORTED 3
#define RF_ERR_FORMAT 4
#define RF_ERR_LIMIT 5

/* Initialize size to sizeof(rf_options), threads to 0 (automatic) or 1..9,
 * and reserved to zero. NULL selects defaults. The original 8-byte prefix
 * (size, threads) is accepted. Refusals: size below 8, a size not a multiple
 * of four, or threads above 9 is RF_ERR_USAGE; a size larger than this
 * library's structure, or a nonzero reserved word covered by size, is
 * RF_ERR_UNSUPPORTED. Reserved words beyond size are absent, not read, and
 * may hold anything; reserved words covered by size must be zero. */
typedef struct {
    uint32_t size, threads;
    uint32_t reserved[4];
} rf_options;

/* An RGB8 image. On input the caller owns data; the library borrows it for
 * the call and never writes through it. On output from rf_decode_image the
 * library allocates data and the caller releases it with rf_free.
 *   width, height: image dimensions in pixels, both nonzero.
 *   stride: bytes between rows; at least width*3 on input, exactly width*3
 *     in decoded output.
 *   layout: RF_LAYOUT_RGB8.
 *   data: the first pixel (top-left), rows top-down.
 *   size: accessible bytes at data; on input at least
 *     (height-1)*stride + width*3; in decoded output exactly stride*height. */
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

/* Validated format metadata filled by rf_inspect.
 *   width, height: image dimensions in pixels.
 *   method: the file's coding method (0 raw, 1 CM, 2 palette, 3 extended
 *     palette, 4 compact palette), as stored in the header.
 *   payload_size: bytes of payload after the 32-byte header.
 *   payload_crc, header_crc: the CRC-32 values stored in the file, verified
 *     against the bytes before this structure is filled.
 *   stream_size: coded stream sizes in G, R, B channel order for method 1
 *     (version 3 reports the sum across strips); zero for the raw and
 *     palette methods. */
typedef struct {
    uint32_t width, height, method;
    uint32_t payload_size, payload_crc, header_crc;
    uint32_t stream_size[3];
} rf_info;

/* Returns the packed API version (RF_API_VERSION in the headers this call
 * was compiled against may differ if a newer DLL is loaded). Cannot fail. */
RF_API uint32_t rf_version(void);
/* Encode RGB8 pixels to a complete .fold file.
 *   pixels: input image (borrowed); layout must be RF_LAYOUT_RGB8.
 *   options: NULL for defaults, or an rf_options whose size field selects
 *     how many words are meaningful; see rf_options.
 *   out: required; on success holds a library allocation the caller frees
 *     with rf_free; on failure it is zeroed. Existing out.data is never
 *     freed implicitly.
 *   err: NULL, or a per-call result object.
 * Precondition: no output object (out, err) and no input storage (the
 * pixels struct, its data, options) may overlap each other; the library
 * clears outputs before reading inputs and does not detect overlap.
 * Shared inputs must stay immutable while concurrent calls use them.
 * Returns RF_OK or RF_ERR_USAGE / RF_ERR_UNSUPPORTED / RF_ERR_FORMAT /
 * RF_ERR_LIMIT as described at the codes above. */
RF_API int rf_encode_image(const rf_pixels *pixels, const rf_options *options,
                           rf_buffer *out, rf_error *err);
/* Decode a complete .fold file (bytes borrowed, covering size bytes) to
 * tightly packed RGB8. out is required and zeroed on failure; its data is
 * a library allocation freed with rf_free. The same non-overlap and
 * immutability rules as rf_encode_image apply to out, err, options, and
 * bytes. out, err, options, and the input bytes may not overlap each
 * other. Returns the same codes as rf_encode_image; malformed input is
 * RF_ERR_FORMAT and is never partially decoded into out. */
RF_API int rf_decode_image(const void *bytes, size_t size, const rf_options *options,
                           rf_pixels *out, rf_error *err);
/* Validate a complete .fold file, including decoding its payload, and fill
 * info. This is not a header-only fast path. info is required; info, err,
 * and the input bytes may not overlap each other; info is zeroed on
 * failure. Returns the same codes as rf_encode_image. */
RF_API int rf_inspect(const void *bytes, size_t size, rf_info *info, rf_error *err);
/* Release the base pointer of a live allocation returned by this library
 * (rf_buffer.data or decoded rf_pixels.data), exactly once. NULL is
 * allowed. Never pass a caller allocation, an interior pointer, or a
 * pointer obtained from another loaded copy of the library. */
RF_API void rf_free(void *pointer);

#ifdef __cplusplus
}
#endif
#endif
