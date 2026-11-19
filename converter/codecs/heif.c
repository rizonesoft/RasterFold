/* Import-only HEIF and HEIC session adapter (D10 T02 s7).
 *
 * The converter bundles no HEVC code (docs/codec-distribution.md): images are
 * decoded by the Windows Imaging Component HEIF decoder, with the HEVC
 * bitstream handed to the Media Foundation H.265 decoder that the system
 * provides. The adapter reaches WIC only through COM in ole32.dll and loads
 * mfplat.dll at run time to ask for an HEVC decoder, so the converter imports
 * no new DLL. Facts WIC does not report faithfully come from the shared
 * ISOBMFF reader (rfc_heif_scan.h), which the submit sniff also runs.
 *
 * Contract: one opaque 8-bit top-level picture, returned as rgb8 exactly as
 * the system decoder produced it (its own pixel format, channels reordered,
 * never converted by WIC on the converter's behalf); orientation is applied
 * by the system decoder, so the frame is already oriented; the primary's ICC
 * (from the file) or nclx (as CICP) is reported; Exif and XMP items are
 * refused under metadata=preserve and left out under metadata=strip. On a
 * system without the components, and on every non-Windows build, the
 * adapter refuses with RFC_ERR_NO_ADAPTER and a detail naming what is
 * missing. HEIF and HEIC are never written. */

#ifdef _WIN32
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <wincodec.h>
#endif

#include "rfc_codecs.h"
#include "rfc_heif_scan.h"
#include "rfc_job.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* WIC and the HEVC decoder allocate outside the job allocator: the adapter
 * reserves this much beyond the frame it allocates (see limits.c). */
#define HEIF_SYSTEM_FIXED (32u * 1024u * 1024u)
#define HEIF_SYSTEM_FRAMES 3u /* decoder output + reference + conversion */
#define HEIF_BAND_ROWS 64u

#ifndef _WIN32
static const char k_no_windows[] =
    "heif: HEIF and HEIC import needs the Windows Imaging Component HEIF "
    "decoder (Windows 10 version 1809 or later); this build has none";
#else
static const char k_no_wic[] =
    "heif: the Windows Imaging Component is unavailable on this system";
static const char k_no_heif[] =
    "heif: the HEIF Image Extensions (Microsoft Store 9PMMSR1CGPWG) are not "
    "installed";
static const char k_no_hevc[] =
    "heif: the HEVC Video Extensions (Microsoft Store 9N4WGH0Z6VHQ) are not "
    "installed";

#endif

/* Test seam: force the component lookups to report a missing component,
 * or the WIC decoder creation to fail with a given HRESULT. */
static atomic_int g_force_no_heif = 0;
static atomic_int g_force_no_hevc = 0;
static atomic_long g_force_create_hr = 0;

void rfc_codecs_heif_set_create_hr_for_testing(long hr) {
    atomic_store(&g_force_create_hr, hr);
}

void rfc_codecs_heif_set_missing_for_testing(int heif_decoder, int hevc_decoder) {
    atomic_store(&g_force_no_heif, heif_decoder ? 1 : 0);
    atomic_store(&g_force_no_hevc, hevc_decoder ? 1 : 0);
}

/* Test seam: called before each band of pixels is copied. */
static void (*g_heif_band_hook)(void *) = NULL;
static void *g_heif_band_hook_ctx = NULL;

void rfc_codecs_heif_set_band_hook_for_testing(void (*hook)(void *), void *ctx) {
    g_heif_band_hook = hook;
    g_heif_band_hook_ctx = ctx;
}

typedef struct heif_session {
    atomic_int cancelled;
    rfc_format_id format;
} heif_session;

static int heif_cancelled(const heif_session *s, const rfc_exec_context *exec) {
    if (s && atomic_load_explicit(&s->cancelled, memory_order_acquire)) {
        return 1;
    }
    return exec ? rfc_exec_is_cancelled(exec) : 0;
}

static rfc_status heif_open(const rfc_exec_context *exec, void **out,
                            rfc_format_id fmt) {
    (void)exec;
    if (!out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    heif_session *s = (heif_session *)malloc(sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    atomic_init(&s->cancelled, 0);
    s->format = fmt;
    *out = s;
    return RFC_OK;
}

static rfc_status heif_open_heif(const rfc_exec_context *exec, void **out) {
    return heif_open(exec, out, RFC_FORMAT_HEIF);
}

static rfc_status heif_open_heic(const rfc_exec_context *exec, void **out) {
    return heif_open(exec, out, RFC_FORMAT_HEIC);
}

static void heif_close(void *session) {
    free(session);
}

static void heif_cancel(void *session) {
    heif_session *s = (heif_session *)session;
    if (s) {
        atomic_store_explicit(&s->cancelled, 1, memory_order_release);
    }
}

#ifdef _WIN32
static void *heif_alloc_fallback(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n ? n : 1u);
}

static void heif_free_fallback(void *ctx, void *p) {
    (void)ctx;
    free(p);
}

static const rfc_allocator k_heif_stdlib = {heif_alloc_fallback,
                                            heif_free_fallback, NULL};

static const rfc_allocator *heif_alloc_of(const rfc_exec_context *exec) {
    const rfc_allocator *a = rfc_exec_allocator(exec);
    return (a && a->alloc && a->free) ? a : &k_heif_stdlib;
}

static const rfc_limits *heif_limits_of(const rfc_exec_context *exec,
                                        rfc_limits *scratch) {
    const rfc_limits *l = rfc_exec_limits(exec);
    if (l) {
        return l;
    }
    rfc_limits_default(scratch);
    return scratch;
}

typedef struct heif_mem {
    const uint8_t *data;
    size_t size;
} heif_mem;

static rfc_status heif_mem_read(void *ctx, uint64_t off, uint8_t *dst, size_t n,
                                size_t *got) {
    const heif_mem *m = (const heif_mem *)ctx;
    *got = 0u;
    if (off >= (uint64_t)m->size) {
        return RFC_OK;
    }
    size_t avail = m->size - (size_t)off;
    size_t k = n < avail ? n : avail;
    memcpy(dst, m->data + off, k);
    *got = k;
    return RFC_OK;
}

/* The container facts and the metadata they give, without settings. The
 * image limits apply to the coded picture, which the system decoder decodes
 * in full, and to the picture it returns. */
static rfc_status heif_inspect(const rfc_exec_context *exec,
                               const uint8_t *bytes, size_t size,
                               rfc_heif_info *info) {
    rfc_limits scratch;
    const rfc_limits *limits = heif_limits_of(exec, &scratch);
    rfc_status st = rfc_limits_check_compressed(limits, (uint64_t)size);
    if (st != RFC_OK) {
        return st;
    }
    heif_mem m = {bytes, size};
    const char *detail = NULL;
    st = rfc_heif_scan(heif_mem_read, &m, (uint64_t)size, heif_alloc_of(exec),
                       info, &detail, NULL);
    if (st != RFC_OK) {
        if (detail) {
            rfc_exec_set_detail(exec, detail);
        }
        return st;
    }
    st = rfc_limits_check_image(limits, &info->coded_layout, 1u);
    if (st == RFC_OK) {
        st = rfc_limits_check_image(limits, &info->tile_layout, 1u);
    }
    if (st == RFC_OK) {
        st = rfc_limits_check_image(limits, &info->layout, 1u);
    }
    if (st != RFC_OK) {
        return st;
    }
    rfc_metadata m2;
    memset(&m2, 0, sizeof(m2));
    m2.icc_len = info->has_icc ? info->icc_len : 0u;
    return rfc_limits_check_metadata(limits, &m2);
}

static void heif_fill_meta(const uint8_t *bytes, const rfc_heif_info *info,
                           rfc_metadata *meta) {
    memset(meta, 0, sizeof(*meta));
    meta->orientation = RFC_ORIENTATION_NORMAL; /* applied by the system */
    if (info->has_icc) {
        meta->icc = bytes + info->icc_offset;
        meta->icc_len = info->icc_len;
    } else if (info->has_nclx &&
               (info->primaries != 2u || info->transfer != 2u)) {
        /* the RGB output carries the primaries and transfer; matrix 0 and
         * full range describe that RGB, as for AVIF */
        meta->color.has_cicp = 1u;
        meta->color.cicp[0] = (uint8_t)info->primaries;
        meta->color.cicp[1] = (uint8_t)info->transfer;
        meta->color.cicp[2] = 0u;
        meta->color.cicp[3] = 1u;
    }
}

#endif /* _WIN32 */

#ifdef _WIN32

typedef struct mf_type_info {
    GUID major, sub;
} mf_type_info;
typedef HRESULT(WINAPI *mft_enum_ex_fn)(GUID, UINT32, const mf_type_info *,
                                         const mf_type_info *, IUnknown ***,
                                         UINT32 *);

static const GUID k_mft_category_video_decoder = {
    0xd6c02d4b, 0x6833, 0x45b4, {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
static const GUID k_mf_media_type_video = {
    0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID k_mf_video_format_hevc = {
    0x43564548, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/* Ask Media Foundation, loaded at run time from the system directory only,
 * for a decoder taking the primary picture's coding (HEVC, the only coding
 * the shared reader accepts). */
static int heif_system_has_decoder(rfc_heif_coding coding) {
    (void)coding;
    if (atomic_load(&g_force_no_hevc)) {
        return 0;
    }
    HMODULE mf = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mf) {
        return 0;
    }
    mft_enum_ex_fn fn =
        (mft_enum_ex_fn)(void (*)(void))GetProcAddress(mf, "MFTEnumEx");
    int found = 0;
    if (fn) {
        mf_type_info in = {k_mf_media_type_video, k_mf_video_format_hevc};
        IUnknown **acts = NULL;
        UINT32 n = 0u;
        if (SUCCEEDED(fn(k_mft_category_video_decoder, 0x3Fu /* all */, &in,
                         NULL, &acts, &n))) {
            found = n > 0u;
            for (UINT32 i = 0u; i < n; i++) {
                if (acts[i]) {
                    acts[i]->lpVtbl->Release(acts[i]);
                }
            }
            CoTaskMemFree(acts);
        }
    }
    FreeLibrary(mf);
    return found;
}

static const char *heif_codec_detail(rfc_heif_coding coding) {
    (void)coding;
    return k_no_hevc;
}

/* A WIC object could not be created: only a missing component earns the
 * installation guidance; resource and other failures keep their meaning. */
static rfc_status heif_create_failure(const rfc_exec_context *exec, HRESULT hr,
                                      const char *missing) {
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND || hr == REGDB_E_CLASSNOTREG) {
        rfc_exec_set_detail(exec, missing);
        return RFC_ERR_NO_ADAPTER;
    }
    if (hr == E_OUTOFMEMORY) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    return RFC_ERR_IO;
}

typedef struct heif_com {
    int uninit;
    IWICImagingFactory *factory;
    IWICStream *stream;
    IWICBitmapDecoder *decoder;
    IWICBitmapFrameDecode *frame;
} heif_com;

static void heif_com_end(heif_com *c) {
    if (c->frame) IWICBitmapFrameDecode_Release(c->frame);
    if (c->decoder) IWICBitmapDecoder_Release(c->decoder);
    if (c->stream) IWICStream_Release(c->stream);
    if (c->factory) IWICImagingFactory_Release(c->factory);
    if (c->uninit) CoUninitialize();
    memset(c, 0, sizeof(*c));
}

/* A failure while the system decodes the picture: a missing codec names
 * the video extension for the picture's coding. */
static rfc_status heif_map_hr(const rfc_exec_context *exec,
                              rfc_heif_coding coding, HRESULT hr) {
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND || hr == REGDB_E_CLASSNOTREG ||
        hr == (HRESULT)0xC00D5212L /* MF_E_TOPO_CODEC_NOT_FOUND */) {
        rfc_exec_set_detail(exec, heif_codec_detail(coding));
        return RFC_ERR_NO_ADAPTER;
    }
    if (hr == E_OUTOFMEMORY) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    return RFC_ERR_INVALID_ARGUMENT;
}

/* The WIC HEIF decoder and a Media Foundation decoder for the picture's
 * coding, looked up for real (the probe runs this before any decode). */
static rfc_status heif_components_ready(const rfc_exec_context *exec,
                                        const rfc_heif_info *info) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int uninit = hr == S_OK || hr == S_FALSE;
    if (!uninit && hr != RPC_E_CHANGED_MODE) {
        return RFC_ERR_IO;
    }
    IWICImagingFactory *f = NULL;
    rfc_status st = RFC_OK;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&f);
    if (FAILED(hr)) {
        st = heif_create_failure(exec, hr, k_no_wic);
    } else {
        IWICBitmapDecoder *d = NULL;
        long forced = atomic_load(&g_force_create_hr);
        hr = atomic_load(&g_force_no_heif) ? WINCODEC_ERR_COMPONENTNOTFOUND
             : forced                      ? (HRESULT)forced
                                           : IWICImagingFactory_CreateDecoder(
                                                 f, &GUID_ContainerFormatHeif, NULL, &d);
        if (FAILED(hr)) {
            st = heif_create_failure(exec, hr, k_no_heif);
        } else {
            IWICBitmapDecoder_Release(d);
        }
        IWICImagingFactory_Release(f);
    }
    if (uninit) {
        CoUninitialize();
    }
    if (st == RFC_OK && !heif_system_has_decoder(info->coding)) {
        rfc_exec_set_detail(exec, heif_codec_detail(info->coding));
        st = RFC_ERR_NO_ADAPTER;
    }
    return st;
}

/* Open the system decoder on the bytes; frame 0 ready. */
static rfc_status heif_com_open(const rfc_exec_context *exec,
                                const rfc_heif_info *info,
                                const uint8_t *bytes, size_t size,
                                heif_com *c) {
    memset(c, 0, sizeof(*c));
    if (!heif_system_has_decoder(info->coding)) {
        rfc_exec_set_detail(exec, heif_codec_detail(info->coding));
        return RFC_ERR_NO_ADAPTER;
    }
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        c->uninit = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return RFC_ERR_IO;
    }
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&c->factory);
    if (FAILED(hr)) {
        heif_com_end(c);
        return heif_create_failure(exec, hr, k_no_wic);
    }
    long forced = atomic_load(&g_force_create_hr);
    hr = atomic_load(&g_force_no_heif) ? WINCODEC_ERR_COMPONENTNOTFOUND
         : forced                      ? (HRESULT)forced
                                       : IWICImagingFactory_CreateDecoder(
                                             c->factory, &GUID_ContainerFormatHeif,
                                             NULL, &c->decoder);
    if (FAILED(hr)) {
        heif_com_end(c);
        return heif_create_failure(exec, hr, k_no_heif);
    }
    if (size > 0xFFFFFFFFu) {
        heif_com_end(c);
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    hr = IWICImagingFactory_CreateStream(c->factory, &c->stream);
    if (SUCCEEDED(hr)) {
        hr = IWICStream_InitializeFromMemory(c->stream, (BYTE *)bytes,
                                             (DWORD)size);
    }
    if (SUCCEEDED(hr)) {
        hr = IWICBitmapDecoder_Initialize(c->decoder, (IStream *)c->stream,
                                          WICDecodeMetadataCacheOnDemand);
    }
    UINT frames = 0u;
    if (SUCCEEDED(hr)) {
        hr = IWICBitmapDecoder_GetFrameCount(c->decoder, &frames);
    }
    if (SUCCEEDED(hr) && frames != 1u) {
        heif_com_end(c);
        rfc_exec_set_detail(exec, "heif: the system decoder reports more than one image");
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }
    if (SUCCEEDED(hr)) {
        hr = IWICBitmapDecoder_GetFrame(c->decoder, 0u, &c->frame);
    }
    if (FAILED(hr)) {
        rfc_status st = heif_map_hr(exec, info->coding, hr);
        heif_com_end(c);
        return st;
    }
    UINT w = 0u, h = 0u;
    hr = IWICBitmapFrameDecode_GetSize(c->frame, &w, &h);
    if (FAILED(hr) || w != info->width || h != info->height) {
        heif_com_end(c);
        return RFC_ERR_INVALID_ARGUMENT; /* the container and decoder disagree */
    }
    return RFC_OK;
}

/* Copy the system decoder's pixels into the rgb8 frame, in bands. */
static rfc_status heif_copy_pixels(const heif_session *s,
                                   const rfc_exec_context *exec, heif_com *c,
                                   rfc_heif_coding coding,
                                   const rfc_allocator *alloc, rfc_frame *fr) {
    WICPixelFormatGUID pf;
    HRESULT hr = IWICBitmapFrameDecode_GetPixelFormat(c->frame, &pf);
    if (FAILED(hr)) {
        return heif_map_hr(exec, coding, hr);
    }
    uint32_t bpp = 0u;
    if (IsEqualGUID(&pf, &GUID_WICPixelFormat24bppBGR)) {
        bpp = 3u;
    } else if (IsEqualGUID(&pf, &GUID_WICPixelFormat32bppBGR)) {
        bpp = 4u;
    } else {
        rfc_exec_set_detail(exec, "heif: the system decoder returned a pixel "
                                  "format other than 24bppBGR or 32bppBGR for "
                                  "an 8-bit opaque image");
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    const rfc_layout *L = &fr->layout;
    size_t src_stride = (size_t)L->width * bpp;
    uint32_t band = HEIF_BAND_ROWS < L->height ? HEIF_BAND_ROWS : L->height;
    uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, src_stride * band);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    rfc_status st = RFC_OK;
    for (uint32_t y = 0u; y < L->height && st == RFC_OK; y += band) {
        if (g_heif_band_hook) {
            g_heif_band_hook(g_heif_band_hook_ctx);
        }
        if (heif_cancelled(s, exec)) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        uint32_t rows = L->height - y < band ? L->height - y : band;
        WICRect rc = {0, (INT)y, (INT)L->width, (INT)rows};
        hr = IWICBitmapFrameDecode_CopyPixels(c->frame, &rc, (UINT)src_stride,
                                              (UINT)(src_stride * rows), buf);
        if (FAILED(hr)) {
            st = heif_map_hr(exec, coding, hr);
            break;
        }
        for (uint32_t r = 0u; r < rows; r++) {
            const uint8_t *in = buf + (size_t)r * src_stride;
            uint8_t *out = fr->data + (size_t)(y + r) * L->stride_bytes;
            for (uint32_t x = 0u; x < L->width; x++) {
                out[3u * x + 0u] = in[bpp * x + 2u];
                out[3u * x + 1u] = in[bpp * x + 1u];
                out[3u * x + 2u] = in[bpp * x + 0u];
            }
        }
    }
    alloc->free(alloc->ctx, buf);
    return st;
}

#endif /* _WIN32 */

#ifdef _WIN32
typedef struct heif_guard {
    rfc_budget *budget;
    size_t bytes;
} heif_guard;

/* The system decoders work on the coded picture: reserve from it. */
static rfc_status heif_reserve(const rfc_exec_context *exec,
                               const rfc_layout *L, heif_guard *g) {
    memset(g, 0, sizeof(*g));
    if (!exec || !exec->budget) {
        return RFC_OK;
    }
    uint64_t frame32 = (uint64_t)L->width * L->height * 4u;
    uint64_t want = frame32 * HEIF_SYSTEM_FRAMES + HEIF_SYSTEM_FIXED;
    if (want > SIZE_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    rfc_status st = rfc_budget_reserve(exec->budget, (size_t)want);
    if (st != RFC_OK) {
        return st;
    }
    g->budget = exec->budget;
    g->bytes = (size_t)want;
    return RFC_OK;
}

static void heif_release(heif_guard *g) {
    if (g->budget) {
        rfc_budget_release(g->budget, g->bytes);
    }
    memset(g, 0, sizeof(*g));
}

#endif /* _WIN32 */

static rfc_status heif_probe(void *session, const rfc_exec_context *exec,
                             const uint8_t *bytes, size_t size,
                             rfc_image_probe *out_probe) {
    heif_session *s = (heif_session *)session;
    if (!out_probe || !bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (heif_cancelled(s, exec)) {
        return RFC_ERR_CANCELLED;
    }
#ifndef _WIN32
    (void)size;
    rfc_exec_set_detail(exec, k_no_windows);
    return RFC_ERR_NO_ADAPTER;
#else
    rfc_heif_info info;
    rfc_status st = heif_inspect(exec, bytes, size, &info);
    if (st != RFC_OK) {
        return st;
    }
    /* the same components the decode needs, looked up for real */
    st = heif_components_ready(exec, &info);
    if (st != RFC_OK) {
        return st;
    }
    out_probe->first_frame = info.layout;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)info.width * info.height;
    out_probe->total_decoded_bytes =
        (uint64_t)info.layout.stride_bytes * info.height;
    rfc_metadata meta;
    heif_fill_meta(bytes, &info, &meta);
    out_probe->container_meta.color = meta.color;
    out_probe->container_meta.orientation = meta.orientation;
    out_probe->container_meta.icc_len = meta.icc_len;
    rfc_limits scratch;
    st = rfc_limits_check_probe(heif_limits_of(exec, &scratch), out_probe,
                                (uint64_t)size, 1);
    out_probe->container_meta.icc_len = 0u;
    if (st == RFC_OK && meta.icc_len > 0u) {
        /* the ICC bytes, copied out through the exec allocator */
        rfc_frame tmp;
        memset(&tmp, 0, sizeof(tmp));
        rfc_layout one = {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 1u, 1u, 1u, 8u};
        st = rfc_frame_alloc(&tmp, &one, &meta, heif_alloc_of(exec));
        if (st == RFC_OK) {
            out_probe->container_meta = tmp.metadata;
            tmp.metadata.icc = NULL;
            tmp.metadata.icc_len = 0u;
            rfc_frame_free(&tmp);
        }
    }
    return st;
#endif
}

static rfc_status heif_decode(void *session, const rfc_exec_context *exec,
                              const uint8_t *bytes, size_t size,
                              const rfc_setting_value *settings,
                              size_t setting_count, rfc_frame **out_frames,
                              size_t *out_frame_count, rfc_metadata *out_meta) {
    heif_session *s = (heif_session *)session;
    if (!out_frames || !out_frame_count || !bytes) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    *out_frames = NULL;
    *out_frame_count = 0u;
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    rfc_format_id fmt = s ? s->format : RFC_FORMAT_HEIC;
    int strip = 0;
    if (setting_count > 0u) {
        rfc_status vst =
            rfc_registry_validate_settings(fmt, settings, setting_count, NULL);
        if (vst != RFC_OK) {
            return vst;
        }
        for (size_t i = 0u; i < setting_count; i++) {
            if (strcmp(settings[i].id, "metadata") == 0) {
                strip = strcmp(settings[i].value, "strip") == 0;
            }
        }
    }
    if (heif_cancelled(s, exec)) {
        return RFC_ERR_CANCELLED;
    }
#ifndef _WIN32
    (void)size;
    (void)strip;
    rfc_exec_set_detail(exec, k_no_windows);
    return RFC_ERR_NO_ADAPTER;
#else
    rfc_heif_info info;
    rfc_status st = heif_inspect(exec, bytes, size, &info);
    if (st != RFC_OK) {
        return st;
    }
    if (!strip && (info.has_exif || info.has_xmp)) {
        rfc_exec_set_detail(exec, "heif: Exif or XMP items cannot be carried yet "
                                  "(metadata=strip imports the image without them)");
        return RFC_ERR_METADATA_POLICY;
    }
    heif_guard g;
    st = heif_reserve(exec, &info.coded_layout, &g);
    if (st != RFC_OK) {
        return st;
    }
    heif_com c;
    st = heif_com_open(exec, &info, bytes, size, &c);
    if (st != RFC_OK) {
        heif_release(&g);
        return st;
    }
    const rfc_allocator *alloc = heif_alloc_of(exec);
    rfc_metadata meta;
    heif_fill_meta(bytes, &info, &meta);
    rfc_frame *frames = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(rfc_frame));
    if (!frames) {
        st = RFC_ERR_OUT_OF_MEMORY;
    } else {
        memset(frames, 0, sizeof(*frames));
        st = exec ? rfc_exec_alloc_frame(exec, &frames[0], &info.layout, &meta)
                  : rfc_frame_alloc(&frames[0], &info.layout, &meta, alloc);
        if (st == RFC_OK) {
            st = heif_copy_pixels(s, exec, &c, info.coding, alloc, &frames[0]);
        }
        if (st != RFC_OK) {
            if (frames[0].data) {
                rfc_frame_free(&frames[0]);
            }
            alloc->free(alloc->ctx, frames);
            frames = NULL;
        }
    }
    heif_com_end(&c);
    heif_release(&g);
    if (st != RFC_OK) {
        return st;
    }
    *out_frames = frames;
    *out_frame_count = 1u;
    if (out_meta) {
        *out_meta = frames[0].metadata;
    }
    return RFC_OK;
#endif
}

static rfc_status heif_encode(void *session, const rfc_exec_context *exec,
                              const rfc_frame *frames, size_t frame_count,
                              const rfc_metadata *meta,
                              const rfc_setting_value *settings,
                              size_t setting_count, rfc_encoded *out) {
    (void)session;
    (void)frames;
    (void)frame_count;
    (void)meta;
    (void)settings;
    (void)setting_count;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    rfc_exec_set_detail(exec, "heif: HEIF and HEIC are import only (no HEVC encoder is bundled)");
    return RFC_ERR_UNSUPPORTED_COMBINATION;
}

static const rfc_session_adapter k_heif_adapter = {
    RFC_CODEC_ABI_VERSION, RFC_FORMAT_HEIF, heif_open_heif, heif_close,
    heif_probe, heif_encode, heif_decode, heif_cancel, NULL};

static const rfc_session_adapter k_heic_adapter = {
    RFC_CODEC_ABI_VERSION, RFC_FORMAT_HEIC, heif_open_heic, heif_close,
    heif_probe, heif_encode, heif_decode, heif_cancel, NULL};

rfc_status rfc_codecs_register_heif(void) {
    return rfc_registry_register_session_pair(&k_heif_adapter, &k_heic_adapter);
}

rfc_status rfc_codecs_unregister_heif(void) {
    return rfc_registry_unregister_session_pair(RFC_FORMAT_HEIF, RFC_FORMAT_HEIC);
}
