/* Public registration interface for shipped converter codec adapters
 * (D10 T02). Zero window, GUI, or <windows.h> dependencies in public
 * C11/C++17 headers. */
#ifndef RFC_CODECS_H
#define RFC_CODECS_H

#include "rfc_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register shipped session adapters for RFC_FORMAT_RASTERFOLD, RFC_FORMAT_BMP,
 * RFC_FORMAT_PNG, RFC_FORMAT_PNM, and RFC_FORMAT_PAM. If any registration fails,
 * previously added adapters from this call are rolled back cleanly. */
rfc_status rfc_codecs_register_basic(void);

/* Unregister any basic session adapters currently registered. Returns
 * RFC_ERR_BUSY if a session is still active. */
rfc_status rfc_codecs_unregister_basic(void);

/* Register and unregister the shipped WebP session adapter (D10 T02 s2). */
rfc_status rfc_codecs_register_webp(void);
rfc_status rfc_codecs_unregister_webp(void);

/* Register and unregister the shipped JPEG XL session adapter and the
 * JPEG -> JPEG XL lossless byte-reconstruction adapter (D10 T02 s3). */
rfc_status rfc_codecs_register_jxl(void);
rfc_status rfc_codecs_unregister_jxl(void);

/* Register and unregister the shipped AVIF session adapter (D10 T02 s4). */
rfc_status rfc_codecs_register_avif(void);
rfc_status rfc_codecs_unregister_avif(void);

/* Register and unregister the shipped JPEG session adapter and the JPEG ->
 * JPEG coefficient-exact entropy-optimization adapter (D10 T02 s5) as one
 * registry transaction: both or neither. Unregister returns RFC_ERR_BUSY,
 * leaving both registered, while either adapter is acquired. */
rfc_status rfc_codecs_register_jpeg(void);
rfc_status rfc_codecs_unregister_jpeg(void);

/* Register and unregister the shipped TIFF and GIF session adapters
 * (D10 T02 s6) as one registry transaction: both or neither; unregister
 * returns RFC_ERR_BUSY, leaving both registered, while either is acquired. */
rfc_status rfc_codecs_register_tiff_gif(void);
rfc_status rfc_codecs_unregister_tiff_gif(void);

/* Register and unregister the import-only HEIF and HEIC session adapters
 * (D10 T02 s7) as one registry transaction: both or neither; unregister
 * returns RFC_ERR_BUSY, leaving both registered, while either is acquired.
 * On a build or a system without the Windows Imaging Component HEIF decoder
 * the adapters refuse with RFC_ERR_NO_ADAPTER and a detail naming what is
 * missing. */
rfc_status rfc_codecs_register_heif(void);
rfc_status rfc_codecs_unregister_heif(void);

/* Test seam: report the WIC HEIF decoder and/or the Media Foundation HEVC
 * decoder as missing (0 restores the real lookups). */
void rfc_codecs_heif_set_missing_for_testing(int heif_decoder, int hevc_decoder);

/* Test seam: hook called before each 64-row band of HEIF pixels is copied
 * (NULL removes it), for deterministic mid-decode cancellation tests. */
void rfc_codecs_heif_set_band_hook_for_testing(void (*hook)(void *), void *ctx);

/* Test seam: hook called before each decoded TIFF strip or tile, each GIF
 * row, every 256 GIF LZW sub-blocks of the pre-scan and each row of the GIF
 * palette pass (NULL removes it), for deterministic mid-decode cancellation tests. */
void rfc_codecs_tiff_gif_set_row_hook_for_testing(void (*hook)(void *),
                                                  void *ctx);

/* Test seam: the optimization output buffer is source size + slack bytes
 * (default 64 KiB); a negative slack shrinks it below the source so tests can
 * drive the destination-full fallback. -1 restores the default. */
void rfc_codecs_jpeg_set_output_slack_for_testing(long long slack);

/* Direct JPEG <-> JPEG XL lossless byte-reconstruction entry points used by
 * the JXL reconstruction adapter and D10 T02 s5 delegation. */
rfc_status rfc_jxl_transcode_jpeg(const rfc_exec_context *exec,
                                  const uint8_t *jpeg_bytes,
                                  size_t jpeg_size,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_encoded *out_jxl,
                                  rfc_metadata *out_src_meta);
rfc_status rfc_jxl_reconstruct_jpeg(const rfc_exec_context *exec,
                                    const uint8_t *jxl_bytes,
                                    size_t jxl_size,
                                    rfc_encoded *out_jpeg,
                                    rfc_metadata *out_dst_meta);

#ifdef __cplusplus
}
#endif

#endif /* RFC_CODECS_H */
