/* Internal bridge between registry.c (rfc_frame_alloc) and job.c execution
 * guards. Keeps test_contract.c standalone while enforcing pre-allocation frame
 * checks during active decode sessions. */
#ifndef RFC_CODEC_INTERNAL_H
#define RFC_CODEC_INTERNAL_H

#include "rfc_codec.h"

typedef rfc_status (*rfc_frame_alloc_guard_fn)(
    const rfc_allocator *allocator,
    const rfc_layout *layout,
    const rfc_metadata *metadata);

void rfc_internal_set_frame_alloc_guard(rfc_frame_alloc_guard_fn fn);

#endif /* RFC_CODEC_INTERNAL_H */
