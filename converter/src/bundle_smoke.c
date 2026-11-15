/* Strictly C11 smoke harness proving every pinned converter dependency links
 * statically and executes real codec/support work on Windows x64. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rasterfold.h"
#include "rfc_codec.h"

#include <zlib.h>
#include <libdeflate.h>
#include <png.h>
#ifndef PNG_APNG_SUPPORTED
#error "libpng-apng patch must enable PNG_APNG_SUPPORTED"
#endif
#include <webp/encode.h>
#include <webp/decode.h>
#include <webp/sharpyuv/sharpyuv.h>
#include <brotli/encode.h>
#include <brotli/decode.h>
#include <lcms2.h>
#include <jxl/encode.h>
#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>
#include <aom/aom_codec.h>
#include <aom/aomcx.h>
#include <aom/aomdx.h>
#include <avif/avif.h>
#include <turbojpeg.h>
#include <tiffio.h>
#include <gif_lib.h>
#include <qoi.h>
#include <openjpeg.h>
#include <openexr.h>
#include <libraw/libraw.h>

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t pos;
} mem_stream;

static int mem_append(mem_stream *m, const void *buf, size_t n) {
    if (n > m->cap - m->len) {
        size_t next = m->cap ? m->cap : 4096u;
        while (next - m->len < n) {
            if (next > SIZE_MAX / 2u) {
                return 0;
            }
            next *= 2u;
        }
        uint8_t *grown = (uint8_t *)realloc(m->data, next);
        if (!grown) {
            return 0;
        }
        m->data = grown;
        m->cap = next;
    }
    if (n > 0) {
        memcpy(m->data + m->len, buf, n);
        m->len += n;
    }
    return 1;
}

static int mem_write_at(mem_stream *m, uint64_t offset, const void *buf, size_t n) {
    if (offset > SIZE_MAX - n) {
        return 0;
    }
    size_t end = (size_t)offset + n;
    if (end > m->cap) {
        size_t next = m->cap ? m->cap : 4096u;
        while (next < end) {
            if (next > SIZE_MAX / 2u) {
                return 0;
            }
            next *= 2u;
        }
        uint8_t *grown = (uint8_t *)realloc(m->data, next);
        if (!grown) {
            return 0;
        }
        if (next > m->cap) {
            memset(grown + m->cap, 0, next - m->cap);
        }
        m->data = grown;
        m->cap = next;
    }
    if (n > 0) {
        memcpy(m->data + (size_t)offset, buf, n);
    }
    if (end > m->len) {
        m->len = end;
    }
    return 1;
}

static void fill_rgb4x4(uint8_t rgb[4 * 4 * 3]) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            size_t i = (size_t)(y * 4 + x) * 3u;
            rgb[i + 0] = (uint8_t)(20 + x * 60);
            rgb[i + 1] = (uint8_t)(40 + y * 55);
            rgb[i + 2] = (uint8_t)(220 - x * 35 - y * 20);
        }
    }
}

static int check_registry(void) {
    size_t count = rfc_registry_count();
    if (count != 19u) {
        fprintf(stderr, "FAIL registry: count=%zu want 19\n", count);
        return 1;
    }
    int saw_heif = 0, saw_heic = 0;
    for (size_t i = 0; i < count; i++) {
        const rfc_capability *cap = rfc_registry_at(i);
        if (!cap || !cap->name || !cap->provider) {
            fprintf(stderr, "FAIL registry: null entry %zu\n", i);
            return 1;
        }
        if (strcmp(cap->name, "heif") == 0) {
            saw_heif = !cap->can_write;
        } else if (strcmp(cap->name, "heic") == 0) {
            saw_heic = !cap->can_write;
        }
    }
    if (!saw_heif || !saw_heic) {
        fprintf(stderr, "FAIL registry: expected non-writable WIC heif/heic\n");
        return 1;
    }
    printf("PASS provider=rfc_registry count=%zu\n", count);
    return 0;
}

static int check_rasterfold(void) {
    uint8_t rgb[4 * 4 * 3];
    fill_rgb4x4(rgb);
    rf_pixels src = {4u, 4u, 12u, RF_LAYOUT_RGB8, rgb, sizeof(rgb)};
    rf_buffer enc = {0};
    rf_error err = {0};
    if (rf_encode_image(&src, NULL, &enc, &err) != RF_OK) {
        fprintf(stderr, "FAIL rasterfold: encode %d (%s)\n", err.code, err.message ? err.message : "");
        return 1;
    }
    rf_info info = {0};
    if (rf_inspect(enc.data, enc.size, &info, &err) != RF_OK || info.width != 4u || info.height != 4u) {
        rf_free(enc.data);
        fprintf(stderr, "FAIL rasterfold: inspect %d\n", err.code);
        return 1;
    }
    rf_pixels dec = {0};
    if (rf_decode_image(enc.data, enc.size, NULL, &dec, &err) != RF_OK ||
        dec.size != sizeof(rgb) || memcmp(dec.data, rgb, sizeof(rgb)) != 0) {
        rf_free(enc.data);
        rf_free(dec.data);
        fprintf(stderr, "FAIL rasterfold: roundtrip\n");
        return 1;
    }
    rf_free(enc.data);
    rf_free(dec.data);
    printf("PASS provider=rasterfold api=0x%08x\n", (unsigned)rf_version());
    return 0;
}

static int check_zlib(void) {
    static const char sample[] = "RasterFold-zlib-pinned-bundle-smoke";
    Bytef comp[128], decomp[128];
    uLongf comp_len = (uLongf)sizeof(comp);
    uLongf decomp_len = (uLongf)sizeof(decomp);
    if (compress2(comp, &comp_len, (const Bytef *)sample, (uLong)sizeof(sample), Z_BEST_COMPRESSION) != Z_OK ||
        uncompress(decomp, &decomp_len, comp, comp_len) != Z_OK ||
        decomp_len != sizeof(sample) || memcmp(decomp, sample, sizeof(sample)) != 0) {
        fprintf(stderr, "FAIL zlib: roundtrip\n");
        return 1;
    }
    printf("PASS provider=zlib version=%s\n", zlibVersion());
    return 0;
}

static int check_libdeflate(void) {
    static const char sample[] = "RasterFold-libdeflate-pinned-bundle-smoke";
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(6);
    struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
    uint8_t comp[128], decomp[128];
    size_t clen = c ? libdeflate_deflate_compress(c, sample, sizeof(sample), comp, sizeof(comp)) : 0u;
    size_t actual = 0u;
    int ok = c && d && clen > 0u &&
             libdeflate_deflate_decompress(d, comp, clen, decomp, sizeof(sample), &actual) == LIBDEFLATE_SUCCESS &&
             actual == sizeof(sample) && memcmp(decomp, sample, sizeof(sample)) == 0;
    libdeflate_free_compressor(c);
    libdeflate_free_decompressor(d);
    if (!ok) {
        fprintf(stderr, "FAIL libdeflate: roundtrip\n");
        return 1;
    }
    printf("PASS provider=libdeflate\n");
    return 0;
}

static void png_mem_write(png_structp png_ptr, png_bytep data, png_size_t length) {
    mem_stream *m = (mem_stream *)png_get_io_ptr(png_ptr);
    if (!mem_append(m, data, (size_t)length)) {
        png_error(png_ptr, "oom");
    }
}

static void png_mem_flush(png_structp png_ptr) {
    (void)png_ptr;
}

static void png_mem_read(png_structp png_ptr, png_bytep data, png_size_t length) {
    mem_stream *m = (mem_stream *)png_get_io_ptr(png_ptr);
    if (length > m->len - m->pos) {
        png_error(png_ptr, "eof");
    }
    memcpy(data, m->data + m->pos, (size_t)length);
    m->pos += (size_t)length;
}

static int check_libpng_apng(void) {
    uint8_t rgb[4 * 4 * 3];
    fill_rgb4x4(rgb);
    mem_stream ms = {0};
    png_structp w = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop wi = w ? png_create_info_struct(w) : NULL;
    if (!w || !wi || setjmp(png_jmpbuf(w))) {
        png_destroy_write_struct(&w, &wi);
        free(ms.data);
        fprintf(stderr, "FAIL libpng: write init\n");
        return 1;
    }
    png_set_write_fn(w, &ms, png_mem_write, png_mem_flush);
    png_set_IHDR(w, wi, 4u, 4u, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_acTL(w, wi, 1u, 0u);
    png_bytep rows[4] = {rgb, rgb + 12, rgb + 24, rgb + 36};
    png_write_info(w, wi);
    png_write_frame_head(w, wi, rows, 4u, 4u, 0u, 0u, 1u, 100u, PNG_DISPOSE_OP_NONE, PNG_BLEND_OP_SOURCE);
    png_write_image(w, rows);
    png_write_frame_tail(w, wi);
    png_write_end(w, wi);
    png_destroy_write_struct(&w, &wi);

    ms.pos = 0;
    png_structp r = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop ri = r ? png_create_info_struct(r) : NULL;
    png_uint_32 frames = 0u, plays = 0u;
    uint8_t decoded[4 * 4 * 3] = {0};
    png_bytep drows[4] = {decoded, decoded + 12, decoded + 24, decoded + 36};
    if (!r || !ri || setjmp(png_jmpbuf(r))) {
        png_destroy_read_struct(&r, &ri, NULL);
        free(ms.data);
        fprintf(stderr, "FAIL libpng: read init\n");
        return 1;
    }
    png_set_read_fn(r, &ms, png_mem_read);
    png_read_info(r, ri);
    if (!png_get_acTL(r, ri, &frames, &plays) || frames != 1u) {
        png_destroy_read_struct(&r, &ri, NULL);
        free(ms.data);
        fprintf(stderr, "FAIL libpng: acTL missing\n");
        return 1;
    }
    png_read_image(r, drows);
    png_destroy_read_struct(&r, &ri, NULL);
    free(ms.data);
    if (memcmp(decoded, rgb, sizeof(rgb)) != 0) {
        fprintf(stderr, "FAIL libpng: roundtrip\n");
        return 1;
    }
    printf("PASS provider=libpng version=%lu apng=1\n", (unsigned long)png_access_version_number());
    return 0;
}

static int check_libwebp(void) {
    uint8_t rgb[4 * 4 * 3];
    fill_rgb4x4(rgb);
    uint8_t *webp = NULL;
    size_t sz = WebPEncodeLosslessRGB(rgb, 4, 4, 12, &webp);
    int w = 0, h = 0;
    uint8_t *dec = sz ? WebPDecodeRGB(webp, sz, &w, &h) : NULL;
    int ok = dec && w == 4 && h == 4 && memcmp(dec, rgb, sizeof(rgb)) == 0;
    WebPFree(webp);
    WebPFree(dec);
    if (!ok) {
        fprintf(stderr, "FAIL libwebp: roundtrip\n");
        return 1;
    }
    printf("PASS provider=libwebp version=0x%06x sharpyuv=0x%06x\n",
           (unsigned)WebPGetEncoderVersion(), (unsigned)SharpYuvGetVersion());
    return 0;
}

static int check_brotli(void) {
    static const uint8_t sample[] = "RasterFold-brotli-pinned-bundle-smoke";
    size_t enc_len = BrotliEncoderMaxCompressedSize(sizeof(sample));
    uint8_t enc[256], dec[256];
    if (enc_len > sizeof(enc) ||
        !BrotliEncoderCompress(4, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC,
                               sizeof(sample), sample, &enc_len, enc)) {
        fprintf(stderr, "FAIL brotli: encode\n");
        return 1;
    }
    size_t dec_len = sizeof(dec);
    if (BrotliDecoderDecompress(enc_len, enc, &dec_len, dec) != BROTLI_DECODER_RESULT_SUCCESS ||
        dec_len != sizeof(sample) || memcmp(dec, sample, sizeof(sample)) != 0) {
        fprintf(stderr, "FAIL brotli: decode\n");
        return 1;
    }
    printf("PASS provider=brotli version=0x%06x\n", (unsigned)BrotliDecoderVersion());
    return 0;
}

static int check_lcms2(void) {
    cmsHPROFILE srgb = cmsCreate_sRGBProfile();
    cmsHTRANSFORM xf = srgb ? cmsCreateTransform(srgb, TYPE_RGB_8, srgb, TYPE_RGB_8, INTENT_PERCEPTUAL, 0) : NULL;
    uint8_t in[3] = {120, 180, 45}, out[3] = {0};
    if (xf) {
        cmsDoTransform(xf, in, out, 1);
        cmsDeleteTransform(xf);
    }
    if (srgb) {
        cmsCloseProfile(srgb);
    }
    if (!xf) {
        fprintf(stderr, "FAIL lcms2: transform\n");
        return 1;
    }
    printf("PASS provider=lcms2 version=%d\n", cmsGetEncodedCMMversion());
    return 0;
}

static int check_libjxl(void) {
    uint8_t rgb[4 * 4 * 3];
    fill_rgb4x4(rgb);
    void *runner = JxlThreadParallelRunnerCreate(NULL, 1);
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!runner || !enc ||
        JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner) != JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        JxlThreadParallelRunnerDestroy(runner);
        fprintf(stderr, "FAIL libjxl: encoder init\n");
        return 1;
    }
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = 4u;
    bi.ysize = 4u;
    bi.bits_per_sample = 8u;
    bi.uses_original_profile = JXL_TRUE;
    JxlPixelFormat pf = {3u, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0u};
    JxlEncoderFrameSettings *opts = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!opts ||
        JxlEncoderSetBasicInfo(enc, &bi) != JXL_ENC_SUCCESS ||
        JxlEncoderSetFrameLossless(opts, JXL_TRUE) != JXL_ENC_SUCCESS ||
        JxlEncoderAddImageFrame(opts, &pf, rgb, sizeof(rgb)) != JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        JxlThreadParallelRunnerDestroy(runner);
        fprintf(stderr, "FAIL libjxl: add frame\n");
        return 1;
    }
    JxlEncoderCloseInput(enc);
    uint8_t jxl_buf[4096];
    uint8_t *next_out = jxl_buf;
    size_t avail_out = sizeof(jxl_buf);
    if (JxlEncoderProcessOutput(enc, &next_out, &avail_out) != JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        JxlThreadParallelRunnerDestroy(runner);
        fprintf(stderr, "FAIL libjxl: process output\n");
        return 1;
    }
    size_t jxl_size = (size_t)(next_out - jxl_buf);
    JxlEncoderDestroy(enc);

    JxlDecoder *dec = JxlDecoderCreate(NULL);
    uint8_t decoded[4 * 4 * 3] = {0};
    int got_pixels = 0;
    if (!dec ||
        JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(dec, JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, jxl_buf, jxl_size) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        JxlThreadParallelRunnerDestroy(runner);
        fprintf(stderr, "FAIL libjxl: decoder init\n");
        return 1;
    }
    JxlDecoderCloseInput(dec);
    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t need = 0;
            if (JxlDecoderImageOutBufferSize(dec, &pf, &need) != JXL_DEC_SUCCESS || need != sizeof(decoded) ||
                JxlDecoderSetImageOutBuffer(dec, &pf, decoded, sizeof(decoded)) != JXL_DEC_SUCCESS) {
                break;
            }
        } else if (st == JXL_DEC_FULL_IMAGE) {
            got_pixels = 1;
        } else if (st == JXL_DEC_SUCCESS) {
            break;
        } else {
            got_pixels = 0;
            break;
        }
    }
    JxlDecoderDestroy(dec);
    JxlThreadParallelRunnerDestroy(runner);
    if (!got_pixels || memcmp(decoded, rgb, sizeof(rgb)) != 0) {
        fprintf(stderr, "FAIL libjxl: roundtrip\n");
        return 1;
    }
    printf("PASS provider=libjxl+highway+jxl_threads+jxl_cms decoder=0x%06x\n",
           (unsigned)JxlDecoderVersion());
    return 0;
}

static int check_libaom_avif(void) {
    if (!aom_codec_av1_cx() || !aom_codec_av1_dx()) {
        fprintf(stderr, "FAIL libaom: missing av1 interfaces\n");
        return 1;
    }
    uint8_t rgb[4 * 4 * 3];
    fill_rgb4x4(rgb);
    avifImage *img = avifImageCreate(4, 4, 8, AVIF_PIXEL_FORMAT_YUV444);
    avifEncoder *enc = avifEncoderCreate();
    avifRWData raw = AVIF_DATA_EMPTY;
    if (!img || !enc) {
        if (img) avifImageDestroy(img);
        if (enc) avifEncoderDestroy(enc);
        fprintf(stderr, "FAIL libavif: alloc\n");
        return 1;
    }
    avifRGBImage src_rgb;
    avifRGBImageSetDefaults(&src_rgb, img);
    src_rgb.format = AVIF_RGB_FORMAT_RGB;
    src_rgb.pixels = rgb;
    src_rgb.rowBytes = 12u;
    enc->codecChoice = AVIF_CODEC_CHOICE_AOM;
    enc->speed = 10;
    enc->quality = AVIF_QUALITY_LOSSLESS;
    avifResult res = avifImageRGBToYUV(img, &src_rgb);
    if (res == AVIF_RESULT_OK) {
        res = avifEncoderAddImage(enc, img, 1, AVIF_ADD_IMAGE_FLAG_SINGLE);
    }
    if (res == AVIF_RESULT_OK) {
        res = avifEncoderFinish(enc, &raw);
    }
    avifEncoderDestroy(enc);
    avifImageDestroy(img);
    if (res != AVIF_RESULT_OK || raw.size == 0u) {
        avifRWDataFree(&raw);
        fprintf(stderr, "FAIL libavif: encode (%s)\n", avifResultToString(res));
        return 1;
    }

    avifDecoder *dec = avifDecoderCreate();
    avifResult dres = dec ? avifDecoderSetIOMemory(dec, raw.data, raw.size) : AVIF_RESULT_OUT_OF_MEMORY;
    if (dres == AVIF_RESULT_OK) {
        dres = avifDecoderParse(dec);
    }
    if (dres == AVIF_RESULT_OK) {
        dres = avifDecoderNextImage(dec);
    }
    int ok = dres == AVIF_RESULT_OK && dec->image && dec->image->width == 4u && dec->image->height == 4u;
    if (dec) {
        avifDecoderDestroy(dec);
    }
    avifRWDataFree(&raw);
    if (!ok) {
        fprintf(stderr, "FAIL libavif: decode (%s)\n", avifResultToString(dres));
        return 1;
    }
    printf("PASS provider=libavif+libaom avif=%s aom=%s\n", avifVersion(), aom_codec_version_str());
    return 0;
}

static int check_libjpeg_turbo(void) {
    uint8_t rgb[4 * 4 * 3];
    fill_rgb4x4(rgb);
    tjhandle c = tj3Init(TJINIT_COMPRESS);
    tjhandle d = tj3Init(TJINIT_DECOMPRESS);
    unsigned char *jpg = NULL;
    size_t jpg_sz = 0;
    uint8_t out[4 * 4 * 3] = {0};
    int ok = 0;
    if (c && d &&
        tj3Set(c, TJPARAM_SUBSAMP, TJSAMP_444) == 0 &&
        tj3Set(c, TJPARAM_QUALITY, 95) == 0 &&
        tj3Compress8(c, rgb, 4, 0, 4, TJPF_RGB, &jpg, &jpg_sz) == 0 &&
        tj3Decompress8(d, jpg, jpg_sz, out, 0, TJPF_RGB) == 0) {
        ok = 1;
    }
    tj3Free(jpg);
    if (c) tj3Destroy(c);
    if (d) tj3Destroy(d);
    if (!ok) {
        fprintf(stderr, "FAIL libjpeg-turbo: compress/decompress\n");
        return 1;
    }
    printf("PASS provider=libjpeg-turbo bytes=%zu\n", jpg_sz);
    return 0;
}

static tmsize_t tiff_mem_read(thandle_t h, void *buf, tmsize_t size) {
    mem_stream *m = (mem_stream *)h;
    size_t avail = m->pos < m->len ? (m->len - m->pos) : 0u;
    size_t want = (size_t)size;
    size_t n = want < avail ? want : avail;
    if (n > 0u) {
        memcpy(buf, m->data + m->pos, n);
        m->pos += n;
    }
    return (tmsize_t)n;
}

static tmsize_t tiff_mem_write(thandle_t h, void *buf, tmsize_t size) {
    mem_stream *m = (mem_stream *)h;
    if (!mem_write_at(m, m->pos, buf, (size_t)size)) {
        return -1;
    }
    m->pos += (size_t)size;
    return size;
}

static toff_t tiff_mem_seek(thandle_t h, toff_t off, int whence) {
    mem_stream *m = (mem_stream *)h;
    size_t base = 0u;
    if (whence == SEEK_CUR) {
        base = m->pos;
    } else if (whence == SEEK_END) {
        base = m->len;
    }
    m->pos = base + (size_t)off;
    return (toff_t)m->pos;
}

static int tiff_mem_close(thandle_t h) {
    (void)h;
    return 0;
}

static toff_t tiff_mem_size(thandle_t h) {
    mem_stream *m = (mem_stream *)h;
    return (toff_t)m->len;
}

static int tiff_mem_map(thandle_t h, void **base, toff_t *size) {
    (void)h;
    (void)base;
    (void)size;
    return 0;
}

static void tiff_mem_unmap(thandle_t h, void *base, toff_t size) {
    (void)h;
    (void)base;
    (void)size;
}

static int check_libtiff(void) {
    uint8_t rgb[4 * 4 * 3];
    fill_rgb4x4(rgb);
    mem_stream ms = {0};
    TIFF *tw = TIFFClientOpen("mem.tiff", "wm", &ms, tiff_mem_read, tiff_mem_write,
                              tiff_mem_seek, tiff_mem_close, tiff_mem_size, tiff_mem_map, tiff_mem_unmap);
    int w_ok = tw &&
               TIFFSetField(tw, TIFFTAG_IMAGEWIDTH, 4) &&
               TIFFSetField(tw, TIFFTAG_IMAGELENGTH, 4) &&
               TIFFSetField(tw, TIFFTAG_SAMPLESPERPIXEL, 3) &&
               TIFFSetField(tw, TIFFTAG_BITSPERSAMPLE, 8) &&
               TIFFSetField(tw, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) &&
               TIFFSetField(tw, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) &&
               TIFFSetField(tw, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    if (w_ok) {
        for (uint32_t y = 0; y < 4u; y++) {
            if (TIFFWriteScanline(tw, rgb + y * 12u, y, 0) < 0) {
                w_ok = 0;
                break;
            }
        }
    }
    if (tw) TIFFClose(tw);

    ms.pos = 0u;
    TIFF *tr = w_ok ? TIFFClientOpen("mem.tiff", "rm", &ms, tiff_mem_read, tiff_mem_write,
                                     tiff_mem_seek, tiff_mem_close, tiff_mem_size, tiff_mem_map, tiff_mem_unmap) : NULL;
    uint32_t rw = 0, rh = 0;
    uint8_t dec[4 * 4 * 3] = {0};
    int r_ok = tr && TIFFGetField(tr, TIFFTAG_IMAGEWIDTH, &rw) && TIFFGetField(tr, TIFFTAG_IMAGELENGTH, &rh) &&
               rw == 4u && rh == 4u;
    if (r_ok) {
        for (uint32_t y = 0; y < 4u; y++) {
            if (TIFFReadScanline(tr, dec + y * 12u, y, 0) < 0) {
                r_ok = 0;
                break;
            }
        }
    }
    if (tr) TIFFClose(tr);
    free(ms.data);
    if (!r_ok || memcmp(dec, rgb, sizeof(rgb)) != 0) {
        fprintf(stderr, "FAIL libtiff: roundtrip\n");
        return 1;
    }
    (void)TIFFGetVersion();
    printf("PASS provider=libtiff\n");
    return 0;
}

static int gif_mem_write(GifFileType *gf, const GifByteType *buf, int len) {
    mem_stream *m = (mem_stream *)gf->UserData;
    return mem_append(m, buf, (size_t)len) ? len : 0;
}

static int gif_mem_read(GifFileType *gf, GifByteType *buf, int len) {
    mem_stream *m = (mem_stream *)gf->UserData;
    size_t avail = m->pos < m->len ? (m->len - m->pos) : 0u;
    int n = (size_t)len < avail ? len : (int)avail;
    if (n > 0) {
        memcpy(buf, m->data + m->pos, (size_t)n);
        m->pos += (size_t)n;
    }
    return n;
}

static int check_giflib(void) {
    mem_stream ms = {0};
    int err = 0;
    GifColorType pal[2] = {{0, 0, 0}, {255, 255, 255}};
    ColorMapObject *cmap = GifMakeMapObject(2, pal);
    GifFileType *gw = cmap ? EGifOpen(&ms, gif_mem_write, &err) : NULL;
    int w_ok = gw &&
               EGifPutScreenDesc(gw, 4, 4, 1, 0, cmap) == GIF_OK &&
               EGifPutImageDesc(gw, 0, 0, 4, 4, false, NULL) == GIF_OK;
    if (w_ok) {
        GifPixelType line[4] = {0, 1, 0, 1};
        for (int y = 0; y < 4; y++) {
            if (EGifPutLine(gw, line, 4) != GIF_OK) {
                w_ok = 0;
                break;
            }
        }
    }
    if (gw) EGifCloseFile(gw, &err);
    if (cmap) GifFreeMapObject(cmap);

    ms.pos = 0u;
    GifFileType *gr = w_ok ? DGifOpen(&ms, gif_mem_read, &err) : NULL;
    int r_ok = gr && DGifSlurp(gr) == GIF_OK && gr->SWidth == 4 && gr->SHeight == 4 && gr->ImageCount == 1;
    if (gr) DGifCloseFile(gr, &err);
    free(ms.data);
    if (!r_ok) {
        fprintf(stderr, "FAIL giflib: roundtrip (err=%d)\n", err);
        return 1;
    }
    printf("PASS provider=giflib\n");
    return 0;
}

static int check_qoi(void) {
    uint8_t rgb[4 * 4 * 3];
    fill_rgb4x4(rgb);
    qoi_desc desc = {4u, 4u, 3u, QOI_SRGB};
    int qlen = 0;
    void *q = qoi_encode(rgb, &desc, &qlen);
    qoi_desc ddesc = {0};
    void *dec = q ? qoi_decode(q, qlen, &ddesc, 3) : NULL;
    int ok = dec && ddesc.width == 4u && ddesc.height == 4u && memcmp(dec, rgb, sizeof(rgb)) == 0;
    free(q);
    free(dec);
    if (!ok) {
        fprintf(stderr, "FAIL qoi: roundtrip\n");
        return 1;
    }
    printf("PASS provider=qoi bytes=%d\n", qlen);
    return 0;
}

static OPJ_SIZE_T opj_mem_write(void *buf, OPJ_SIZE_T nb_bytes, void *user) {
    mem_stream *m = (mem_stream *)user;
    return mem_append(m, buf, (size_t)nb_bytes) ? nb_bytes : (OPJ_SIZE_T)-1;
}

static OPJ_SIZE_T opj_mem_read(void *buf, OPJ_SIZE_T nb_bytes, void *user) {
    mem_stream *m = (mem_stream *)user;
    size_t avail = m->pos < m->len ? (m->len - m->pos) : 0u;
    size_t n = (size_t)nb_bytes < avail ? (size_t)nb_bytes : avail;
    if (n == 0u) {
        return (OPJ_SIZE_T)-1;
    }
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return (OPJ_SIZE_T)n;
}

static int check_openjpeg(void) {
    opj_image_cmptparm_t parms[3];
    memset(parms, 0, sizeof(parms));
    for (int i = 0; i < 3; i++) {
        parms[i].dx = 1;
        parms[i].dy = 1;
        parms[i].w = 4;
        parms[i].h = 4;
        parms[i].prec = 8;
    }
    opj_image_t *img = opj_image_create(3u, parms, OPJ_CLRSPC_SRGB);
    if (!img) {
        fprintf(stderr, "FAIL openjpeg: image create\n");
        return 1;
    }
    img->x1 = 4;
    img->y1 = 4;
    for (int i = 0; i < 16; i++) {
        img->comps[0].data[i] = (OPJ_INT32)(i * 10);
        img->comps[1].data[i] = (OPJ_INT32)(255 - i * 10);
        img->comps[2].data[i] = (OPJ_INT32)(i * 5);
    }

    opj_cparameters_t cp;
    opj_set_default_encoder_parameters(&cp);
    cp.numresolution = 2;
    cp.tcp_numlayers = 1;
    cp.tcp_rates[0] = 0;
    cp.irreversible = 0;

    mem_stream ms = {0};
    opj_codec_t *enc = opj_create_compress(OPJ_CODEC_J2K);
    opj_stream_t *ws = opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_FALSE);
    if (ws) {
        opj_stream_set_user_data(ws, &ms, NULL);
        opj_stream_set_write_function(ws, opj_mem_write);
    }
    int ok = enc && ws &&
             opj_setup_encoder(enc, &cp, img) &&
             opj_start_compress(enc, img, ws) &&
             opj_encode(enc, ws) &&
             opj_end_compress(enc, ws);
    if (ws) opj_stream_destroy(ws);
    if (enc) opj_destroy_codec(enc);
    opj_image_destroy(img);

    ms.pos = 0u;
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    opj_codec_t *dec = ok ? opj_create_decompress(OPJ_CODEC_J2K) : NULL;
    opj_stream_t *rs = dec ? opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_TRUE) : NULL;
    opj_image_t *dimg = NULL;
    if (rs) {
        opj_stream_set_user_data(rs, &ms, NULL);
        opj_stream_set_user_data_length(rs, (OPJ_UINT64)ms.len);
        opj_stream_set_read_function(rs, opj_mem_read);
    }
    int dok = dec && rs &&
              opj_setup_decoder(dec, &dp) &&
              opj_read_header(rs, dec, &dimg) &&
              opj_decode(dec, rs, dimg) &&
              dimg && dimg->numcomps == 3u && dimg->comps[0].data[5] == 50;
    if (dimg) opj_image_destroy(dimg);
    if (rs) opj_stream_destroy(rs);
    if (dec) opj_destroy_codec(dec);
    free(ms.data);
    if (!dok) {
        fprintf(stderr, "FAIL openjpeg: roundtrip\n");
        return 1;
    }
    printf("PASS provider=openjpeg version=%s\n", opj_version());
    return 0;
}

static int64_t exr_mem_write_cb(exr_const_context_t ctxt, void *userdata, const void *buffer,
                                uint64_t sz, uint64_t offset, exr_stream_error_func_ptr_t err_cb) {
    (void)ctxt;
    (void)err_cb;
    mem_stream *m = (mem_stream *)userdata;
    return mem_write_at(m, offset, buffer, (size_t)sz) ? (int64_t)sz : -1;
}

static int64_t exr_mem_read_cb(exr_const_context_t ctxt, void *userdata, void *buffer,
                               uint64_t sz, uint64_t offset, exr_stream_error_func_ptr_t err_cb) {
    (void)ctxt;
    (void)err_cb;
    mem_stream *m = (mem_stream *)userdata;
    if (offset >= m->len) {
        return 0;
    }
    size_t avail = m->len - (size_t)offset;
    size_t n = (size_t)sz < avail ? (size_t)sz : avail;
    if (n > 0u) {
        memcpy(buffer, m->data + (size_t)offset, n);
    }
    return (int64_t)n;
}

static int64_t exr_mem_size_cb(exr_const_context_t ctxt, void *userdata) {
    (void)ctxt;
    return (int64_t)((const mem_stream *)userdata)->len;
}

static int check_openexr_htj2k(void) {
    int maj = 0, min = 0, patch = 0;
    const char *extra = NULL;
    exr_get_library_version(&maj, &min, &patch, &extra);

    mem_stream ms = {0};
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.user_data = &ms;
    init.write_fn = exr_mem_write_cb;
    init.read_fn = exr_mem_read_cb;
    init.size_fn = exr_mem_size_cb;

    exr_context_t wc = NULL;
    int part = 0;
    uint16_t pixels[4 * 4];
    for (int i = 0; i < 16; i++) {
        pixels[i] = (uint16_t)(0x3c00u + (uint16_t)i);
    }
    if (exr_start_write(&wc, "mem.exr", EXR_WRITE_FILE_DIRECTLY, &init) != EXR_ERR_SUCCESS ||
        exr_add_part(wc, "main", EXR_STORAGE_SCANLINE, &part) != EXR_ERR_SUCCESS ||
        exr_initialize_required_attr_simple(wc, part, 4, 4, EXR_COMPRESSION_HTJ2K32) != EXR_ERR_SUCCESS ||
        exr_add_channel(wc, part, "Y", EXR_PIXEL_HALF, EXR_PERCEPTUALLY_LINEAR, 1, 1) != EXR_ERR_SUCCESS ||
        exr_write_header(wc) != EXR_ERR_SUCCESS) {
        if (wc) exr_finish(&wc);
        free(ms.data);
        fprintf(stderr, "FAIL openexr: write header\n");
        return 1;
    }
    exr_chunk_info_t wcinfo;
    exr_encode_pipeline_t ep = { .pipe_size = sizeof(exr_encode_pipeline_t) };
    int enc_ok = 0;
    if (exr_write_scanline_chunk_info(wc, part, 0, &wcinfo) == EXR_ERR_SUCCESS &&
        exr_encoding_initialize(wc, part, &wcinfo, &ep) == EXR_ERR_SUCCESS) {
        ep.channels[0].encode_from_ptr = (const uint8_t *)pixels;
        ep.channels[0].user_pixel_stride = 2;
        ep.channels[0].user_line_stride = 8;
        ep.channels[0].user_bytes_per_element = 2;
        ep.channels[0].user_data_type = EXR_PIXEL_HALF;
        if (exr_encoding_choose_default_routines(wc, part, &ep) == EXR_ERR_SUCCESS &&
            exr_encoding_run(wc, part, &ep) == EXR_ERR_SUCCESS) {
            enc_ok = 1;
        }
        exr_encoding_destroy(wc, &ep);
    }
    exr_finish(&wc);

    exr_context_t rc = NULL;
    uint16_t decoded[4 * 4] = {0};
    int dec_ok = 0;
    if (enc_ok && exr_start_read(&rc, "mem.exr", &init) == EXR_ERR_SUCCESS) {
        exr_chunk_info_t rcinfo;
        exr_decode_pipeline_t dp = { .pipe_size = sizeof(exr_decode_pipeline_t) };
        if (exr_read_scanline_chunk_info(rc, 0, 0, &rcinfo) == EXR_ERR_SUCCESS &&
            exr_decoding_initialize(rc, 0, &rcinfo, &dp) == EXR_ERR_SUCCESS) {
            dp.channels[0].decode_to_ptr = (uint8_t *)decoded;
            dp.channels[0].user_pixel_stride = 2;
            dp.channels[0].user_line_stride = 8;
            dp.channels[0].user_bytes_per_element = 2;
            dp.channels[0].user_data_type = EXR_PIXEL_HALF;
            if (exr_decoding_choose_default_routines(rc, 0, &dp) == EXR_ERR_SUCCESS &&
                exr_decoding_run(rc, 0, &dp) == EXR_ERR_SUCCESS) {
                dec_ok = memcmp(decoded, pixels, sizeof(pixels)) == 0;
            }
            exr_decoding_destroy(rc, &dp);
        }
        exr_finish(&rc);
    }
    free(ms.data);
    if (!dec_ok) {
        fprintf(stderr, "FAIL openexr+imath+openjph: HTJ2K roundtrip\n");
        return 1;
    }
    printf("PASS provider=openexr+imath+openjph version=%d.%d.%d htj2k_chunk=%d\n",
           maj, min, patch, exr_compression_lines_per_chunk(EXR_COMPRESSION_HTJ2K32));
    return 0;
}

static int check_libraw(void) {
    const char *ver = libraw_version();
    int vnum = libraw_versionNumber();
    libraw_data_t *lr = libraw_init(0);
    if (!ver || vnum <= 0 || !lr) {
        if (lr) libraw_close(lr);
        fprintf(stderr, "FAIL libraw: init\n");
        return 1;
    }
    static const uint8_t bogus[8] = {'N', 'O', 'T', 'R', 'A', 'W', '0', '1'};
    int rc = libraw_open_buffer(lr, bogus, sizeof(bogus));
    libraw_close(lr);
    if (rc == 0) {
        fprintf(stderr, "FAIL libraw: accepted bogus header\n");
        return 1;
    }
    printf("PASS provider=libraw version=%s\n", ver);
    return 0;
}

int main(void) {
    int fail = 0;
    fail |= check_registry();
    fail |= check_rasterfold();
    fail |= check_zlib();
    fail |= check_libdeflate();
    fail |= check_libpng_apng();
    fail |= check_libwebp();
    fail |= check_brotli();
    fail |= check_lcms2();
    fail |= check_libjxl();
    fail |= check_libaom_avif();
    fail |= check_libjpeg_turbo();
    fail |= check_libtiff();
    fail |= check_giflib();
    fail |= check_qoi();
    fail |= check_openjpeg();
    fail |= check_openexr_htj2k();
    fail |= check_libraw();
    if (fail) {
        fprintf(stderr, "BUNDLE_SMOKE FAIL\n");
        return 1;
    }
    printf("BUNDLE_SMOKE PASS\n");
    return 0;
}
