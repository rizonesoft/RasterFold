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

#ifdef __cplusplus
}
#endif

#endif /* RFC_CODECS_H */
