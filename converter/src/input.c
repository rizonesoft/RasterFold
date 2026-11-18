#if defined(_WIN32)
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#if !defined(WINVER) || WINVER < 0x0A00
#undef WINVER
#define WINVER 0x0A00
#endif
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "job_internal.h"
#include "rfc_gif_scan.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define RFC_INPUT_CHUNK_BYTES (64u * 1024u)
#define RFC_SNIFF_BUF_BYTES 16384u

static void copy_sniff_detail(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t rd16be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64le(const uint8_t *p) {
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

static uint64_t rd64be(const uint8_t *p) {
    return ((uint64_t)rd32be(p) << 32) | (uint64_t)rd32be(p + 4);
}

static int eq_ascii_ci(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

static int has_prefix_ci(const char *s, size_t len, const char *lit) {
    size_t l = strlen(lit);
    if (len < l) {
        return 0;
    }
    for (size_t i = 0u; i < l; i++) {
        unsigned char ca = (unsigned char)s[i];
        unsigned char cb = (unsigned char)lit[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static int is_ascii_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int validate_utf8_cstr(const char *s, size_t max_bytes) {
    if (!s || s[0] == '\0') {
        return 0;
    }
    size_t i = 0u;
    while (s[i] != '\0') {
        if (i >= max_bytes) {
            return 0;
        }
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80u) {
            i++;
            continue;
        }
        if ((c & 0xE0u) == 0xC0u) {
            if (max_bytes - i <= 1u) {
                return 0;
            }
            unsigned char c1 = (unsigned char)s[i + 1u];
            if ((c1 & 0xC0u) != 0x80u) {
                return 0;
            }
            uint32_t cp = ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(c1 & 0x3Fu);
            if (cp < 0x80u) {
                return 0;
            }
            i += 2u;
        } else if ((c & 0xF0u) == 0xE0u) {
            if (max_bytes - i <= 2u) {
                return 0;
            }
            unsigned char c1 = (unsigned char)s[i + 1u];
            if ((c1 & 0xC0u) != 0x80u) {
                return 0;
            }
            unsigned char c2 = (unsigned char)s[i + 2u];
            if ((c2 & 0xC0u) != 0x80u) {
                return 0;
            }
            uint32_t cp = ((uint32_t)(c & 0x0Fu) << 12) |
                          ((uint32_t)(c1 & 0x3Fu) << 6) | (uint32_t)(c2 & 0x3Fu);
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                return 0;
            }
            i += 3u;
        } else if ((c & 0xF8u) == 0xF0u) {
            if (max_bytes - i <= 3u) {
                return 0;
            }
            unsigned char c1 = (unsigned char)s[i + 1u];
            if ((c1 & 0xC0u) != 0x80u) {
                return 0;
            }
            unsigned char c2 = (unsigned char)s[i + 2u];
            if ((c2 & 0xC0u) != 0x80u) {
                return 0;
            }
            unsigned char c3 = (unsigned char)s[i + 3u];
            if ((c3 & 0xC0u) != 0x80u) {
                return 0;
            }
            uint32_t cp = ((uint32_t)(c & 0x07u) << 18) |
                          ((uint32_t)(c1 & 0x3Fu) << 12) |
                          ((uint32_t)(c2 & 0x3Fu) << 6) | (uint32_t)(c3 & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu) {
                return 0;
            }
            i += 4u;
        } else {
            return 0;
        }
    }
    return i <= max_bytes;
}

void rfc_io_source_init_path(rfc_io_source *out,
                             const char *path_utf8,
                             uint32_t import_flags) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->path_utf8 = path_utf8;
    out->import_flags = import_flags;
}

void rfc_io_source_init_callbacks(rfc_io_source *out,
                                  const rfc_io_reader *reader,
                                  const char *virtual_name_utf8,
                                  uint32_t import_flags) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->reader = reader;
    out->virtual_name_utf8 = virtual_name_utf8;
    out->import_flags = import_flags;
}

void rfc_io_sink_init_path(rfc_io_sink *out,
                           const char *path_utf8,
                           uint32_t export_flags) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->path_utf8 = path_utf8;
    out->export_flags = export_flags;
}

void rfc_io_sink_init_callbacks(rfc_io_sink *out,
                                const rfc_io_publisher *publisher,
                                uint32_t export_flags) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->publisher = publisher;
    out->export_flags = export_flags;
}

rfc_status rfc_input_extension_format(const char *path_or_name_utf8,
                                      rfc_format_id *out_format,
                                      int *out_present,
                                      int *out_known) {
    if (out_format) {
        *out_format = (rfc_format_id)0;
    }
    if (out_present) {
        *out_present = 0;
    }
    if (out_known) {
        *out_known = 0;
    }
    if (!path_or_name_utf8) {
        return RFC_OK;
    }
    const char *base = path_or_name_utf8;
    for (const char *p = path_or_name_utf8; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    const char *dot = NULL;
    for (const char *p = base; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot || dot == base || dot[1] == '\0') {
        return RFC_OK;
    }
    if (out_present) {
        *out_present = 1;
    }
    const char *ext = dot + 1;

    static const struct {
        const char *ext;
        rfc_format_id fmt;
    } map[] = {
        {"fold", RFC_FORMAT_RASTERFOLD},
        {"rfi", RFC_FORMAT_RASTERFOLD},
        {"bmp", RFC_FORMAT_BMP},
        {"dib", RFC_FORMAT_BMP},
        {"png", RFC_FORMAT_PNG},
        {"apng", RFC_FORMAT_APNG},
        {"pbm", RFC_FORMAT_PNM},
        {"pgm", RFC_FORMAT_PNM},
        {"ppm", RFC_FORMAT_PNM},
        {"pnm", RFC_FORMAT_PNM},
        {"pam", RFC_FORMAT_PAM},
        {"webp", RFC_FORMAT_WEBP},
        {"jxl", RFC_FORMAT_JPEG_XL},
        {"avif", RFC_FORMAT_AVIF},
        {"jpg", RFC_FORMAT_JPEG},
        {"jpeg", RFC_FORMAT_JPEG},
        {"jpe", RFC_FORMAT_JPEG},
        {"jfif", RFC_FORMAT_JPEG},
        {"gif", RFC_FORMAT_GIF},
        {"tif", RFC_FORMAT_TIFF},
        {"tiff", RFC_FORMAT_TIFF},
        {"heif", RFC_FORMAT_HEIF},
        {"hif", RFC_FORMAT_HEIF},
        {"heic", RFC_FORMAT_HEIC},
        {"jp2", RFC_FORMAT_JPEG2000},
        {"j2k", RFC_FORMAT_JPEG2000},
        {"j2c", RFC_FORMAT_JPEG2000},
        {"jpc", RFC_FORMAT_JPEG2000},
        {"jpx", RFC_FORMAT_JPEG2000},
        {"qoi", RFC_FORMAT_QOI},
        {"exr", RFC_FORMAT_OPENEXR},
        {"cr2", RFC_FORMAT_CAMERA_RAW},
        {"cr3", RFC_FORMAT_CAMERA_RAW},
        {"crw", RFC_FORMAT_CAMERA_RAW},
        {"nef", RFC_FORMAT_CAMERA_RAW},
        {"nrw", RFC_FORMAT_CAMERA_RAW},
        {"arw", RFC_FORMAT_CAMERA_RAW},
        {"srf", RFC_FORMAT_CAMERA_RAW},
        {"sr2", RFC_FORMAT_CAMERA_RAW},
        {"orf", RFC_FORMAT_CAMERA_RAW},
        {"rw2", RFC_FORMAT_CAMERA_RAW},
        {"raf", RFC_FORMAT_CAMERA_RAW},
        {"pef", RFC_FORMAT_CAMERA_RAW},
        {"srw", RFC_FORMAT_CAMERA_RAW},
        {"mrw", RFC_FORMAT_CAMERA_RAW},
        {"x3f", RFC_FORMAT_CAMERA_RAW},
        {"raw", RFC_FORMAT_CAMERA_RAW},
        {"dng", RFC_FORMAT_DNG},
    };
    for (size_t i = 0u; i < sizeof(map) / sizeof(map[0]); i++) {
        if (eq_ascii_ci(ext, map[i].ext)) {
            if (out_format) {
                *out_format = map[i].fmt;
            }
            if (out_known) {
                *out_known = 1;
            }
            return RFC_OK;
        }
    }
    return RFC_OK;
}

typedef struct seek_stream {
    rfc_status (*read_at)(void *ctx, uint64_t offset, uint8_t *buf,
                          size_t max_bytes, size_t *out_read);
    void *ctx;
    uint64_t total_size;
    int size_known;
} seek_stream;

typedef struct mem_stream_ctx {
    const uint8_t *data;
    size_t len;
} mem_stream_ctx;

static rfc_status mem_stream_read_at(void *ctx,
                                     uint64_t offset,
                                     uint8_t *buf,
                                     size_t max_bytes,
                                     size_t *out_read) {
    mem_stream_ctx *m = (mem_stream_ctx *)ctx;
    *out_read = 0u;
    if (offset >= (uint64_t)m->len) {
        return RFC_OK;
    }
    size_t avail = m->len - (size_t)offset;
    size_t take = avail < max_bytes ? avail : max_bytes;
    memcpy(buf, m->data + (size_t)offset, take);
    *out_read = take;
    return RFC_OK;
}

static rfc_status reader_stream_read_at(void *ctx,
                                        uint64_t offset,
                                        uint8_t *buf,
                                        size_t max_bytes,
                                        size_t *out_read) {
    const rfc_io_reader *r = (const rfc_io_reader *)ctx;
    *out_read = 0u;
    rfc_status st = r->seek_fn(r->ctx, offset);
    if (st != RFC_OK) {
        return st;
    }
    size_t total = 0u;
    while (total < max_bytes) {
        size_t got = 0u;
        st = r->read_fn(r->ctx, buf + total, max_bytes - total, &got);
        if (st != RFC_OK) {
            return st;
        }
        if (got == 0u) {
            break;
        }
        if (got > max_bytes - total) {
            return RFC_ERR_IO;
        }
        total += got;
    }
    *out_read = total;
    return RFC_OK;
}

static int read_exact_at(const seek_stream *ss,
                         uint64_t offset,
                         uint8_t *buf,
                         size_t want,
                         rfc_status *inout_io_status) {
    size_t got = 0u;
    rfc_status st = ss->read_at(ss->ctx, offset, buf, want, &got);
    if (st != RFC_OK) {
        if (inout_io_status && *inout_io_status == RFC_OK) {
            *inout_io_status = st;
        }
        return 0;
    }
    return got == want;
}

static int looks_like_markup_or_script(const uint8_t *buf,
                                       size_t len,
                                       int *out_is_svg_xml) {
    *out_is_svg_xml = 0;
    size_t pos = 0u;
    if (len >= 3u && buf[0] == 0xEFu && buf[1] == 0xBBu && buf[2] == 0xBFu) {
        pos = 3u;
    }
    while (pos < len && is_ascii_ws(buf[pos])) {
        pos++;
    }
    if (pos >= len) {
        return 0;
    }
    const char *s = (const char *)(buf + pos);
    size_t rem = len - pos;
    if (has_prefix_ci(s, rem, "<svg") || has_prefix_ci(s, rem, "<?xml") ||
        has_prefix_ci(s, rem, "<!doctype svg") ||
        has_prefix_ci(s, rem, "<!doctype html") ||
        has_prefix_ci(s, rem, "<html") || has_prefix_ci(s, rem, "<script") ||
        (rem >= 2u && s[0] == '#' && s[1] == '!')) {
        *out_is_svg_xml = 1;
        return 1;
    }
    return 0;
}

static rfc_format_id sniff_png_chunks(const seek_stream *ss,
                                      const uint8_t *hdr,
                                      size_t hdr_len,
                                      rfc_status *out_io_status) {
    (void)hdr;
    (void)hdr_len;
    uint64_t pos = 8u;
    for (uint32_t step = 0u; step < 64u; step++) {
        uint8_t ch[8];
        if (!read_exact_at(ss, pos, ch, 8u, out_io_status)) {
            if (out_io_status && *out_io_status != RFC_OK) {
                return (rfc_format_id)0;
            }
            break;
        }
        uint32_t clen = rd32be(ch);
        if (memcmp(ch + 4, "acTL", 4) == 0) {
            return RFC_FORMAT_APNG;
        }
        if (memcmp(ch + 4, "IDAT", 4) == 0 || memcmp(ch + 4, "IEND", 4) == 0) {
            return RFC_FORMAT_PNG;
        }
        uint64_t step = (uint64_t)clen + 12ull;
        if (pos > UINT64_MAX - step) {
            break;
        }
        pos += step;
    }
    return RFC_FORMAT_PNG;
}

static rfc_format_id sniff_isobmff(const uint8_t *b, size_t len) {
    if (len < 16u || memcmp(b + 4, "ftyp", 4) != 0) {
        return (rfc_format_id)0;
    }
    uint32_t box_sz = rd32be(b);
    size_t end = (box_sz >= 16u && (size_t)box_sz <= len) ? (size_t)box_sz : len;
    const uint8_t *maj = b + 8;
    if (memcmp(maj, "crx ", 4) == 0) {
        return RFC_FORMAT_CAMERA_RAW;
    }
    if (memcmp(maj, "avif", 4) == 0 || memcmp(maj, "avis", 4) == 0) {
        return RFC_FORMAT_AVIF;
    }
    if (memcmp(maj, "heic", 4) == 0 || memcmp(maj, "heix", 4) == 0 ||
        memcmp(maj, "heim", 4) == 0 || memcmp(maj, "heis", 4) == 0 ||
        memcmp(maj, "hevc", 4) == 0 || memcmp(maj, "hevx", 4) == 0) {
        return RFC_FORMAT_HEIC;
    }
    int saw_avif = 0;
    int saw_heic = 0;
    int saw_heif = 0;
    int saw_cr3 = 0;
    for (size_t off = 16u; off + 4u <= end; off += 4u) {
        const uint8_t *cb = b + off;
        if (memcmp(cb, "crx ", 4) == 0) {
            saw_cr3 = 1;
        } else if (memcmp(cb, "avif", 4) == 0 || memcmp(cb, "avis", 4) == 0) {
            saw_avif = 1;
        } else if (memcmp(cb, "heic", 4) == 0 || memcmp(cb, "heix", 4) == 0 ||
                   memcmp(cb, "heim", 4) == 0 || memcmp(cb, "heis", 4) == 0 ||
                   memcmp(cb, "hevc", 4) == 0 || memcmp(cb, "hevx", 4) == 0) {
            saw_heic = 1;
        } else if (memcmp(cb, "mif1", 4) == 0 || memcmp(cb, "msf1", 4) == 0 ||
                   memcmp(cb, "heif", 4) == 0) {
            saw_heif = 1;
        }
    }
    if (saw_cr3) {
        return RFC_FORMAT_CAMERA_RAW;
    }
    if (saw_avif) {
        return RFC_FORMAT_AVIF;
    }
    if (saw_heic) {
        return RFC_FORMAT_HEIC;
    }
    if (saw_heif || memcmp(maj, "mif1", 4) == 0 ||
        memcmp(maj, "msf1", 4) == 0 || memcmp(maj, "heif", 4) == 0) {
        return RFC_FORMAT_HEIF;
    }
    return (rfc_format_id)0;
}

static rfc_format_id sniff_tiff_family(const seek_stream *ss,
                                       const uint8_t *b,
                                       size_t len,
                                       rfc_status *out_io_status) {
    if (len < 8u) {
        return (rfc_format_id)0;
    }
    /* Olympus ORF & Panasonic RW2 variants */
    if ((b[0] == 'I' && b[1] == 'I' && b[2] == 'R' &&
         (b[3] == 'O' || b[3] == 'S')) ||
        (b[0] == 'M' && b[1] == 'M' && b[2] == 'O' && b[3] == 'R') ||
        (b[0] == 'I' && b[1] == 'I' && b[2] == 'U' && b[3] == 0x00u)) {
        return RFC_FORMAT_CAMERA_RAW;
    }

    int le = 0;
    int bigtiff = 0;
    if (b[0] == 'I' && b[1] == 'I' && b[2] == 0x2Au && b[3] == 0x00u) {
        le = 1;
    } else if (b[0] == 'M' && b[1] == 'M' && b[2] == 0x00u && b[3] == 0x2Au) {
        le = 0;
    } else if (b[0] == 'I' && b[1] == 'I' && b[2] == 0x2Bu && b[3] == 0x00u &&
               len >= 16u && rd16le(b + 4) == 8u && rd16le(b + 6) == 0u) {
        le = 1;
        bigtiff = 1;
    } else if (b[0] == 'M' && b[1] == 'M' && b[2] == 0x00u && b[3] == 0x2Bu &&
               len >= 16u && rd16be(b + 4) == 8u && rd16be(b + 6) == 0u) {
        le = 0;
        bigtiff = 1;
    } else {
        return (rfc_format_id)0;
    }

    /* Canon CR2 places "CR\x02\x00" at offset 8 right after the IFD0 offset. */
    if (!bigtiff && le && len >= 12u && b[8] == 'C' && b[9] == 'R' &&
        b[10] == 2u) {
        return RFC_FORMAT_CAMERA_RAW;
    }

    uint64_t ifd_off = 0u;
    if (bigtiff) {
        ifd_off = le ? rd64le(b + 8) : rd64be(b + 8);
        uint8_t cnt_buf[8];
        if (ifd_off > UINT64_MAX - 8u ||
            !read_exact_at(ss, ifd_off, cnt_buf, 8u, out_io_status)) {
            if (out_io_status && *out_io_status != RFC_OK) {
                return (rfc_format_id)0;
            }
            return RFC_FORMAT_TIFF;
        }
        uint64_t count = le ? rd64le(cnt_buf) : rd64be(cnt_buf);
        if (count > 256u) {
            count = 256u;
        }
        for (uint64_t i = 0u; i < count; i++) {
            if (ifd_off > UINT64_MAX - 8u - i * 20u) {
                break;
            }
            uint8_t ent[20];
            uint64_t eoff = ifd_off + 8u + i * 20u;
            if (!read_exact_at(ss, eoff, ent, 20u, out_io_status)) {
                if (out_io_status && *out_io_status != RFC_OK) {
                    return (rfc_format_id)0;
                }
                break;
            }
            uint16_t tag = le ? rd16le(ent) : rd16be(ent);
            if (tag == 0xC612u) {
                return RFC_FORMAT_DNG;
            }
        }
        return RFC_FORMAT_TIFF;
    }

    ifd_off = (uint64_t)(le ? rd32le(b + 4) : rd32be(b + 4));
    uint8_t cnt2[2];
    if (ifd_off > UINT64_MAX - 2u ||
        !read_exact_at(ss, ifd_off, cnt2, 2u, out_io_status)) {
        if (out_io_status && *out_io_status != RFC_OK) {
            return (rfc_format_id)0;
        }
        return RFC_FORMAT_TIFF;
    }
    uint16_t count = le ? rd16le(cnt2) : rd16be(cnt2);
    if (count > 256u) {
        count = 256u;
    }
    for (uint16_t i = 0u; i < count; i++) {
        uint8_t ent[12];
        uint64_t eoff = ifd_off + 2u + (uint64_t)i * 12u;
        if (!read_exact_at(ss, eoff, ent, 12u, out_io_status)) {
            if (out_io_status && *out_io_status != RFC_OK) {
                return (rfc_format_id)0;
            }
            break;
        }
        uint16_t tag = le ? rd16le(ent) : rd16be(ent);
        if (tag == 0xC612u) {
            return RFC_FORMAT_DNG;
        }
    }
    return RFC_FORMAT_TIFF;
}

/* Walk JPEG marker segments with positional reads up to the first SOF (never
 * past the first SOS) and record the frame geometry. Bounded: at most 4096
 * segments and 20 payload bytes read; any malformation leaves no geometry. */
static void sniff_jpeg_geometry(const seek_stream *ss, rfc_sniff_geometry *geo,
                                rfc_sniff_result *out_result) {
    uint64_t pos = 2u;
    for (uint32_t n = 0u; n < 4096u; n++) {
        uint8_t hdr[4];
        size_t got = 0u;
        if (ss->read_at(ss->ctx, pos, hdr, sizeof(hdr), &got) != RFC_OK ||
            got < 2u || hdr[0] != 0xFFu) {
            return;
        }
        if (hdr[1] == 0xFFu) {
            pos++;
            continue;
        }
        uint8_t m = hdr[1];
        if (m == 0xD8u || m == 0x01u || (m >= 0xD0u && m <= 0xD7u)) {
            pos += 2u;
            continue;
        }
        if (m == 0xD9u || m == 0xDAu || got < 4u) {
            return;
        }
        uint16_t len = (uint16_t)(((uint16_t)hdr[2] << 8) | hdr[3]);
        if (len < 2u) {
            return;
        }
        if (m >= 0xC0u && m <= 0xCFu && m != 0xC4u && m != 0xC8u &&
            m != 0xCCu) {
            uint8_t sof[18];
            size_t sg = 0u;
            if (len < 8u ||
                ss->read_at(ss->ctx, pos + 4u, sof, sizeof(sof), &sg) !=
                    RFC_OK ||
                sg < 6u) {
                return;
            }
            uint32_t nc = sof[5];
            if (nc == 0u || nc > 4u || (size_t)len != 8u + 3u * nc ||
                sg < 6u + 3u * (size_t)nc) {
                return;
            }
            out_result->jpeg_height = ((uint32_t)sof[1] << 8) | sof[2];
            out_result->jpeg_width = ((uint32_t)sof[3] << 8) | sof[4];
            out_result->jpeg_components = nc;
            for (uint32_t c = 0u; c < nc; c++) {
                out_result->jpeg_h_samp[c] = (uint8_t)(sof[7u + 3u * c] >> 4);
                out_result->jpeg_v_samp[c] = (uint8_t)(sof[7u + 3u * c] & 15u);
            }
            out_result->has_jpeg_geometry =
                out_result->jpeg_width > 0u && out_result->jpeg_height > 0u;
            geo->height = out_result->jpeg_height;
            geo->width = out_result->jpeg_width;
            if (nc == 1u || nc == 3u) {
                geo->format =
                    nc == 1u ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_RGB8;
                geo->has_geometry = geo->width > 0u && geo->height > 0u;
            }
            return;
        }
        pos += 2u + (uint64_t)len;
    }
}

/* TIFF: ImageWidth, ImageLength, SamplesPerPixel, BitsPerSample and
 * ExtraSamples of the first IFD by positional reads (classic TIFF only, at
 * most 4096 entries); unknown or unsupported layouts leave no geometry. */
static uint64_t tiff_rd(const uint8_t *p, unsigned n, int le) {
    uint64_t v = 0u;
    for (unsigned i = 0u; i < n; i++) {
        unsigned k = le ? n - 1u - i : i;
        v = (v << 8) | p[k];
    }
    return v;
}

/* TIFF and BigTIFF: the first IFD's ImageWidth, ImageLength,
 * SamplesPerPixel, BitsPerSample and ExtraSamples, read with bounded
 * positional reads (at most 4,096 entries). */
static void sniff_tiff_geometry(const seek_stream *ss, const uint8_t *b,
                                size_t len, rfc_sniff_geometry *geo) {
    if (len < 8u) {
        return;
    }
    int le = (b[0] == 'I');
    uint64_t magic = tiff_rd(b + 2, 2u, le);
    int big = 0;
    uint64_t ifd = 0u;
    if (magic == 42u) {
        ifd = tiff_rd(b + 4, 4u, le);
    } else if (magic == 43u && len >= 16u && tiff_rd(b + 4, 2u, le) == 8u &&
               tiff_rd(b + 6, 2u, le) == 0u) {
        big = 1;
        ifd = tiff_rd(b + 8, 8u, le);
    } else {
        return;
    }
    unsigned cnt_sz = big ? 8u : 2u;   /* entry-count field */
    unsigned ent_sz = big ? 20u : 12u; /* directory entry */
    unsigned val_sz = big ? 8u : 4u;   /* count and value/offset fields */
    uint8_t cnt_b[8];
    size_t got = 0u;
    if (ss->read_at(ss->ctx, ifd, cnt_b, cnt_sz, &got) != RFC_OK ||
        got != cnt_sz) {
        return;
    }
    uint64_t n = tiff_rd(cnt_b, cnt_sz, le);
    if (n == 0u || n > 4096u) {
        return;
    }
    uint64_t w = 0u, h = 0u, spp = 1u, bps = 0u;
    int extra = 0;
    for (uint64_t i = 0u; i < n; i++) {
        uint8_t e[20];
        if (ss->read_at(ss->ctx, ifd + cnt_sz + (uint64_t)ent_sz * i, e, ent_sz,
                        &got) != RFC_OK ||
            got != ent_sz) {
            return;
        }
        uint64_t tag = tiff_rd(e, 2u, le);
        uint64_t typ = tiff_rd(e + 2, 2u, le);
        uint64_t count = tiff_rd(e + 4, val_sz, le);
        const uint8_t *val = e + 4 + val_sz;
        uint64_t v = 0u;
        if (typ == 3u) {
            v = tiff_rd(val, 2u, le);
        } else if (typ == 4u) {
            v = tiff_rd(val, 4u, le);
        } else if (typ == 16u && big) {
            v = tiff_rd(val, 8u, le);
        } else {
            continue;
        }
        if (tag == 256u) {
            w = v;
        } else if (tag == 257u) {
            h = v;
        } else if (tag == 277u) {
            spp = v;
        } else if (tag == 258u) {
            /* An array too large for the value field lives elsewhere; it
             * starts with the same depth for every supported file. */
            if (typ == 3u && count * 2u > val_sz) {
                uint8_t first[2];
                uint64_t off = tiff_rd(val, val_sz, le);
                if (ss->read_at(ss->ctx, off, first, 2u, &got) != RFC_OK ||
                    got != 2u) {
                    return;
                }
                v = tiff_rd(first, 2u, le);
            }
            bps = v;
        } else if (tag == 338u) {
            extra = 1;
        }
    }
    if (w == 0u || h == 0u || w > UINT32_MAX || h > UINT32_MAX ||
        (bps != 8u && bps != 16u)) {
        return;
    }
    rfc_sample_format f;
    if (spp == 1u) {
        f = bps == 8u ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_GRAY16;
    } else if (spp == 3u) {
        f = bps == 8u ? RFC_SAMPLE_RGB8 : RFC_SAMPLE_RGB16;
    } else if (spp == 4u && extra) {
        f = bps == 8u ? RFC_SAMPLE_RGBA8 : RFC_SAMPLE_RGBA16;
    } else {
        return;
    }
    geo->width = (uint32_t)w;
    geo->height = (uint32_t)h;
    geo->format = f;
    geo->has_geometry = 1;
}

static rfc_status gif_stream_read(void *ctx, uint64_t off, uint8_t *dst,
                                  size_t n, size_t *got) {
    const seek_stream *ss = (const seek_stream *)ctx;
    *got = 0u;
    return ss->read_at(ss->ctx, off, dst, n, got);
}

/* GIF: the logical screen and the layout the adapter decodes it to, found by
 * the walk the adapter itself runs (rfc_gif_scan.h) under the job's limits,
 * so admission and the adapter decide alike. Without limits, or when the
 * walk refuses the file (the adapter will then refuse it with its own
 * status), the screen is given as rgb8: a lower bound of every decoded
 * layout, so admission never refuses what the adapter accepts. */
static void sniff_gif_geometry(const seek_stream *ss, const uint8_t *b,
                               size_t len, const rfc_limits *limits,
                               rfc_sniff_geometry *geo) {
    if (len < 10u) {
        return;
    }
    uint32_t w = (uint32_t)b[6] | ((uint32_t)b[7] << 8);
    uint32_t h = (uint32_t)b[8] | ((uint32_t)b[9] << 8);
    if (w == 0u || h == 0u) {
        return;
    }
    geo->width = w;
    geo->height = h;
    geo->format = RFC_SAMPLE_RGB8;
    geo->has_geometry = 1;
    if (!limits) {
        return;
    }
    rfc_gif_walk walk;
    if (rfc_gif_walk_begin(&walk, gif_stream_read, (void *)ss, NULL, limits,
                           NULL, NULL) != RFC_OK) {
        return;
    }
    rfc_gif_image img;
    if (rfc_gif_scan_first_image(&walk, &img) == RFC_OK) {
        geo->format = img.layout.format;
    }
    rfc_gif_walk_end(&walk);
}

/* PNG: the IHDR (always the first chunk) gives the decoded layout for the
 * colour types the PNG adapter decodes; palette and gray-alpha images give
 * a lower bound (rgb and gray at their depth), like every sniffed layout. */
static void sniff_png_geometry(const uint8_t *b, size_t len,
                               rfc_sniff_geometry *geo) {
    if (len < 26u || memcmp(b + 12, "IHDR", 4) != 0) {
        return;
    }
    uint32_t w = rd32be(b + 16);
    uint32_t h = rd32be(b + 20);
    uint8_t depth = b[24];
    uint8_t ct = b[25];
    if (w == 0u || h == 0u || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) {
        return;
    }
    int wide = depth == 16u;
    rfc_sample_format f;
    if (ct == 0u) {
        f = wide ? RFC_SAMPLE_GRAY16 : RFC_SAMPLE_GRAY8;
    } else if (ct == 2u) {
        f = wide ? RFC_SAMPLE_RGB16 : RFC_SAMPLE_RGB8;
    } else if (ct == 3u) {
        f = wide ? RFC_SAMPLE_RGB16 : RFC_SAMPLE_RGB8;
    } else if (ct == 4u) {
        f = wide ? RFC_SAMPLE_GRAY16 : RFC_SAMPLE_GRAY8;
    } else if (ct == 6u) {
        f = wide ? RFC_SAMPLE_RGBA16 : RFC_SAMPLE_RGBA8;
    } else {
        return;
    }
    geo->width = w;
    geo->height = h;
    geo->format = f;
    geo->has_geometry = 1;
}

static rfc_format_id sniff_stream_core(const seek_stream *ss,
                                       const char *name_utf8,
                                       rfc_sniff_result *out_result,
                                       rfc_status *out_io_status,
                                       rfc_sniff_geometry *geo_out,
                                       const rfc_limits *geo_limits) {
    rfc_sniff_geometry geo_scratch;
    rfc_sniff_geometry *geo = geo_out ? geo_out : &geo_scratch;
    memset(geo, 0, sizeof(*geo));
    if (out_io_status) {
        *out_io_status = RFC_OK;
    }
    memset(out_result, 0, sizeof(*out_result));
    rfc_input_extension_format(name_utf8, &out_result->extension_format,
                               &out_result->extension_present,
                               &out_result->extension_known);

    uint8_t b[RFC_SNIFF_BUF_BYTES];
    size_t len = 0u;
    rfc_status st = ss->read_at(ss->ctx, 0u, b, sizeof(b), &len);
    if (st != RFC_OK) {
        if (out_io_status) {
            *out_io_status = st;
        }
        copy_sniff_detail(out_result->detail, sizeof(out_result->detail),
                          "failed to read sniff header");
        return (rfc_format_id)0;
    }

    int is_svg = 0;
    if (looks_like_markup_or_script(b, len, &is_svg)) {
        out_result->content_format = (rfc_format_id)0;
        out_result->disagreement = out_result->extension_present ? 1 : 0;
        copy_sniff_detail(
            out_result->detail, sizeof(out_result->detail),
            "SVG/XML or script payload refused as non-raster input");
        return (rfc_format_id)0;
    }

    rfc_format_id detected = (rfc_format_id)0;

    /* 1. RasterFold .fold */
    if (len >= 4u && b[0] == 'R' && b[1] == 'F' && b[2] == 'I' &&
        (b[3] == 2u || b[3] == 3u) &&
        (len < 6u || rd16le(b + 4) == (uint16_t)b[3])) {
        detected = RFC_FORMAT_RASTERFOLD;
    }
    /* 2. PNG vs APNG */
    else if (len >= 8u &&
             memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) {
        detected = sniff_png_chunks(ss, b, len, out_io_status);
        if (detected == RFC_FORMAT_PNG) {
            sniff_png_geometry(b, len, geo);
        }
    }
    /* 3. BMP */
    else if (len >= 2u && b[0] == 'B' && b[1] == 'M') {
        if (len < 18u) {
            detected = RFC_FORMAT_BMP;
        } else {
            uint32_t dib = rd32le(b + 14);
            if (dib == 12u || dib == 16u || dib == 40u || dib == 52u ||
                dib == 56u || dib == 64u || dib == 108u || dib == 124u) {
                detected = RFC_FORMAT_BMP;
            }
        }
    }
    /* 4. PAM (P7) & PNM (P1..P6) */
    else if (len >= 3u && b[0] == 'P' && b[1] == '7' && is_ascii_ws(b[2])) {
        detected = RFC_FORMAT_PAM;
    } else if (len >= 3u && b[0] == 'P' && b[1] >= '1' && b[1] <= '6' &&
               (is_ascii_ws(b[2]) || b[2] == '#')) {
        detected = RFC_FORMAT_PNM;
    }
    /* 5. WebP */
    else if (len >= 12u && memcmp(b, "RIFF", 4) == 0 &&
             memcmp(b + 8, "WEBP", 4) == 0) {
        detected = RFC_FORMAT_WEBP;
    }
    /* 6. JPEG XL (bare codestream or ISOBMFF box) */
    else if ((len >= 2u && b[0] == 0xFFu && b[1] == 0x0Au) ||
             (len >= 12u &&
              memcmp(b, "\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a", 12) == 0)) {
        detected = RFC_FORMAT_JPEG_XL;
    }
    /* 7. JPEG 2000 (JP2 container or J2K codestream) */
    else if ((len >= 12u &&
              memcmp(b, "\x00\x00\x00\x0cjP  \x0d\x0a\x87\x0a", 12) == 0) ||
             (len >= 4u && b[0] == 0xFFu && b[1] == 0x4Fu && b[2] == 0xFFu &&
              b[3] == 0x51u)) {
        detected = RFC_FORMAT_JPEG2000;
    }
    /* 8. ISOBMFF (AVIF, HEIC, HEIF, CR3) */
    else if (len >= 16u && memcmp(b + 4, "ftyp", 4) == 0) {
        detected = sniff_isobmff(b, len);
    }
    /* 9. JPEG */
    else if (len >= 3u && b[0] == 0xFFu && b[1] == 0xD8u && b[2] == 0xFFu) {
        detected = RFC_FORMAT_JPEG;
        sniff_jpeg_geometry(ss, geo, out_result);
    }
    /* 10. GIF */
    else if (len >= 6u &&
             (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0)) {
        detected = RFC_FORMAT_GIF;
        sniff_gif_geometry(ss, b, len, geo_limits, geo);
    }
    /* 11. QOI */
    else if (len >= 4u && memcmp(b, "qoif", 4) == 0) {
        detected = RFC_FORMAT_QOI;
    }
    /* 12. OpenEXR */
    else if (len >= 8u && b[0] == 0x76u && b[1] == 0x2Fu && b[2] == 0x31u &&
             b[3] == 0x01u) {
        detected = RFC_FORMAT_OPENEXR;
    }
    /* 13. Non-TIFF Camera RAW magics (RAF, MRW, X3F, CRW) */
    else if ((len >= 15u && memcmp(b, "FUJIFILMCCD-RAW", 15) == 0) ||
             (len >= 4u && b[0] == 0x00u && memcmp(b + 1, "MRM", 3) == 0) ||
             (len >= 4u && memcmp(b, "FOVb", 4) == 0) ||
             (len >= 14u && b[0] == 'I' && b[1] == 'I' && b[2] == 0x1Au &&
              b[3] == 0x00u && memcmp(b + 6, "HEAPCCDR", 8) == 0)) {
        detected = RFC_FORMAT_CAMERA_RAW;
    }
    /* 14. TIFF / DNG / CR2 / ORF / RW2 */
    else {
        detected = sniff_tiff_family(ss, b, len, out_io_status);
        if (detected == RFC_FORMAT_TIFF) {
            sniff_tiff_geometry(ss, b, len, geo);
        }
    }

    if (out_io_status && *out_io_status != RFC_OK) {
        out_result->content_format = (rfc_format_id)0;
        copy_sniff_detail(out_result->detail, sizeof(out_result->detail),
                          "I/O failure while inspecting container structure");
        return (rfc_format_id)0;
    }

    out_result->content_format = detected;
    if (detected == (rfc_format_id)0) {
        out_result->disagreement = out_result->extension_known ? 1 : 0;
        copy_sniff_detail(out_result->detail, sizeof(out_result->detail),
                          "unrecognized or non-image content");
        return (rfc_format_id)0;
    }

    if (out_result->extension_known &&
        out_result->extension_format != detected) {
        /* PNG vs APNG or TIFF vs DNG or general content mismatch: report mismatch clearly. */
        out_result->disagreement = 1;
        const rfc_capability *ccap = rfc_registry_find(detected);
        const rfc_capability *ecap =
            rfc_registry_find(out_result->extension_format);
        snprintf(out_result->detail, sizeof(out_result->detail),
                 "content format %s disagrees with filename extension %s",
                 ccap ? ccap->name : "unknown",
                 ecap ? ecap->name : "unknown");
    } else if (out_result->extension_present && !out_result->extension_known) {
        out_result->disagreement = 1;
        const rfc_capability *ccap = rfc_registry_find(detected);
        snprintf(out_result->detail, sizeof(out_result->detail),
                 "content format %s disagrees with unrecognized extension",
                 ccap ? ccap->name : "unknown");
    } else {
        const rfc_capability *ccap = rfc_registry_find(detected);
        snprintf(out_result->detail, sizeof(out_result->detail),
                 "sniffed %s", ccap ? ccap->name : "unknown");
    }

    return detected;
}

static rfc_status sniff_bytes_impl(const uint8_t *bytes, size_t len,
                                   const char *name_utf8,
                                   rfc_sniff_result *out_result,
                                   rfc_sniff_geometry *geo,
                                   const rfc_limits *limits) {
    if (!out_result || (len > 0u && !bytes)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    mem_stream_ctx mc = {bytes, len};
    seek_stream ss = {mem_stream_read_at, &mc, (uint64_t)len, 1};
    rfc_status io_st = RFC_OK;
    rfc_format_id fmt =
        sniff_stream_core(&ss, name_utf8, out_result, &io_st, geo, limits);
    if (io_st != RFC_OK) {
        return io_st;
    }
    if (fmt == (rfc_format_id)0) {
        return out_result->disagreement ? RFC_ERR_FORMAT_DISAGREEMENT
                                        : RFC_ERR_UNKNOWN_FORMAT;
    }
    return RFC_OK;
}

rfc_status rfc_input_sniff_bytes(const uint8_t *bytes,
                                 size_t len,
                                 const char *name_utf8,
                                 rfc_sniff_result *out_result) {
    return sniff_bytes_impl(bytes, len, name_utf8, out_result, NULL, NULL);
}

rfc_status rfc_input_internal_sniff_bytes_geometry(
    const uint8_t *bytes, size_t len, const char *name_utf8,
    const rfc_limits *limits, rfc_sniff_result *out_result,
    rfc_sniff_geometry *out_geometry) {
    return sniff_bytes_impl(bytes, len, name_utf8, out_result, out_geometry,
                            limits);
}

static rfc_status sniff_reader_impl(const rfc_io_reader *reader,
                                    const char *name_utf8,
                                    rfc_sniff_result *out_result,
                                    int *out_size_known, uint64_t *out_size,
                                    rfc_sniff_geometry *geo,
                                    const rfc_limits *limits) {
    if (out_size_known) {
        *out_size_known = 0;
    }
    if (out_size) {
        *out_size = 0u;
    }
    if (!reader || !reader->read_fn || !reader->seek_fn || !out_result) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int known = 0;
    uint64_t sz = 0u;
    if (reader->size_fn) {
        rfc_status st = reader->size_fn(reader->ctx, &known, &sz);
        if (st != RFC_OK) {
            return st;
        }
    }
    if (out_size_known) {
        *out_size_known = known;
    }
    if (out_size) {
        *out_size = sz;
    }
    seek_stream ss = {reader_stream_read_at, (void *)reader, sz, known};
    rfc_status io_st = RFC_OK;
    rfc_format_id fmt =
        sniff_stream_core(&ss, name_utf8, out_result, &io_st, geo, limits);
    /* Always leave seekable reader positioned at offset 0. */
    rfc_status rwst = reader->seek_fn(reader->ctx, 0u);
    if (rwst != RFC_OK) {
        return rwst;
    }
    if (io_st != RFC_OK) {
        return io_st;
    }
    if (fmt == (rfc_format_id)0) {
        return out_result->disagreement ? RFC_ERR_FORMAT_DISAGREEMENT
                                        : RFC_ERR_UNKNOWN_FORMAT;
    }
    return RFC_OK;
}

#ifdef _WIN32
static rfc_status utf8_to_wide_stack(const char *utf8,
                                     wchar_t *out_buf,
                                     size_t out_cap) {
    if (!validate_utf8_cstr(utf8, 4096u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                     out_buf, (int)out_cap);
    if (needed <= 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

static rfc_status map_win32_open_error(DWORD err) {
    if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION ||
        err == ERROR_LOCK_VIOLATION) {
        return RFC_ERR_PERMISSION_DENIED;
    }
    return RFC_ERR_IO;
}

typedef struct win_file_reader_ctx {
    HANDLE handle;
    uint64_t size;
} win_file_reader_ctx;

static rfc_status win_file_size(void *ctx, int *out_known, uint64_t *out_size) {
    win_file_reader_ctx *w = (win_file_reader_ctx *)ctx;
    *out_known = 1;
    *out_size = w->size;
    return RFC_OK;
}

static rfc_status win_file_seek(void *ctx, uint64_t offset) {
    win_file_reader_ctx *w = (win_file_reader_ctx *)ctx;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(w->handle, li, NULL, FILE_BEGIN)) {
        return RFC_ERR_IO;
    }
    return RFC_OK;
}

static rfc_status win_file_read(void *ctx,
                                uint8_t *buf,
                                size_t max_bytes,
                                size_t *out_read) {
    win_file_reader_ctx *w = (win_file_reader_ctx *)ctx;
    *out_read = 0u;
    DWORD want = max_bytes > 0x10000000u ? 0x10000000u : (DWORD)max_bytes;
    DWORD got = 0u;
    if (!ReadFile(w->handle, buf, want, &got, NULL)) {
        return map_win32_open_error(GetLastError());
    }
    *out_read = (size_t)got;
    return RFC_OK;
}

static rfc_status open_win_input_reader(const char *path_utf8,
                                        win_file_reader_ctx *ctx_out,
                                        rfc_io_reader *reader_out,
                                        rfc_file_identity *id_out) {
    memset(ctx_out, 0, sizeof(*ctx_out));
    memset(reader_out, 0, sizeof(*reader_out));
    if (id_out) {
        memset(id_out, 0, sizeof(*id_out));
    }
    wchar_t wpath[4096];
    rfc_status st = utf8_to_wide_stack(path_utf8, wpath, 4096u);
    if (st != RFC_OK) {
        return st;
    }
    HANDLE h = CreateFileW(wpath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return map_win32_open_error(GetLastError());
    }
    LARGE_INTEGER fsz;
    if (!GetFileSizeEx(h, &fsz) || fsz.QuadPart < 0) {
        CloseHandle(h);
        return RFC_ERR_IO;
    }
    if (id_out) {
        BY_HANDLE_FILE_INFORMATION bhfi;
        memset(&bhfi, 0, sizeof(bhfi));
        if (GetFileInformationByHandle(h, &bhfi)) {
            id_out->valid = 1;
            id_out->volume_id_lo = (uint64_t)bhfi.dwVolumeSerialNumber;
            id_out->file_id_lo = ((uint64_t)bhfi.nFileIndexHigh << 32) |
                                 (uint64_t)bhfi.nFileIndexLow;
            id_out->link_count = (uint64_t)bhfi.nNumberOfLinks;
        }
        FILE_ID_INFO fid;
        memset(&fid, 0, sizeof(fid));
        if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof(fid))) {
            id_out->valid = 1;
            id_out->volume_id_lo = fid.VolumeSerialNumber;
            uint64_t lo = 0u, hi = 0u;
            memcpy(&lo, fid.FileId.Identifier, 8u);
            memcpy(&hi, fid.FileId.Identifier + 8u, 8u);
            id_out->file_id_lo = lo;
            id_out->file_id_hi = hi;
        }
        DWORD pattrs = GetFileAttributesW(wpath);
        if (pattrs != INVALID_FILE_ATTRIBUTES &&
            (pattrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            id_out->is_symlink_or_reparse = 1;
        }
    }
    ctx_out->handle = h;
    ctx_out->size = (uint64_t)fsz.QuadPart;
    reader_out->ctx = ctx_out;
    reader_out->size_fn = win_file_size;
    reader_out->read_fn = win_file_read;
    reader_out->seek_fn = win_file_seek;
    return RFC_OK;
}
#else
static rfc_status map_posix_io_error(int err) {
    if (err == EACCES || err == EPERM) {
        return RFC_ERR_PERMISSION_DENIED;
    }
    return RFC_ERR_IO;
}

typedef struct posix_file_reader_ctx {
    int fd;
    uint64_t size;
} posix_file_reader_ctx;

static rfc_status posix_file_size(void *ctx,
                                  int *out_known,
                                  uint64_t *out_size) {
    posix_file_reader_ctx *p = (posix_file_reader_ctx *)ctx;
    *out_known = 1;
    *out_size = p->size;
    return RFC_OK;
}

static rfc_status posix_file_seek(void *ctx, uint64_t offset) {
    posix_file_reader_ctx *p = (posix_file_reader_ctx *)ctx;
    if (offset > (uint64_t)INT64_MAX) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    if (lseek(p->fd, (off_t)offset, SEEK_SET) == (off_t)-1) {
        return map_posix_io_error(errno);
    }
    return RFC_OK;
}

static rfc_status posix_file_read(void *ctx,
                                  uint8_t *buf,
                                  size_t max_bytes,
                                  size_t *out_read) {
    posix_file_reader_ctx *p = (posix_file_reader_ctx *)ctx;
    *out_read = 0u;
    ssize_t n;
    do {
        n = read(p->fd, buf, max_bytes);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return map_posix_io_error(errno);
    }
    *out_read = (size_t)n;
    return RFC_OK;
}

static rfc_status open_posix_input_reader(const char *path_utf8,
                                          posix_file_reader_ctx *ctx_out,
                                          rfc_io_reader *reader_out,
                                          rfc_file_identity *id_out) {
    memset(ctx_out, 0, sizeof(*ctx_out));
    memset(reader_out, 0, sizeof(*reader_out));
    if (id_out) {
        memset(id_out, 0, sizeof(*id_out));
    }
    if (!validate_utf8_cstr(path_utf8, 4096u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int open_flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    int fd;
    do {
        fd = open(path_utf8, open_flags);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return map_posix_io_error(errno);
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return RFC_ERR_IO;
    }
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0 && (fl & O_NONBLOCK) != 0) {
        (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }
    if (id_out) {
        id_out->valid = 1;
        id_out->is_symlink_or_reparse = 0;
        id_out->volume_id_lo = (uint64_t)st.st_dev;
        id_out->file_id_lo = (uint64_t)st.st_ino;
        id_out->link_count = (uint64_t)st.st_nlink;
        struct stat lst;
        if (lstat(path_utf8, &lst) == 0 && S_ISLNK(lst.st_mode)) {
            id_out->is_symlink_or_reparse = 1;
        }
    }
    ctx_out->fd = fd;
    ctx_out->size = (uint64_t)st.st_size;
    reader_out->ctx = ctx_out;
    reader_out->size_fn = posix_file_size;
    reader_out->read_fn = posix_file_read;
    reader_out->seek_fn = posix_file_seek;
    return RFC_OK;
}
#endif

rfc_status rfc_input_sniff_reader(const rfc_io_reader *reader,
                                  const char *name_utf8,
                                  rfc_sniff_result *out_result,
                                  int *out_size_known,
                                  uint64_t *out_size) {
    return sniff_reader_impl(reader, name_utf8, out_result, out_size_known,
                             out_size, NULL, NULL);
}

static rfc_status sniff_path_impl(const char *path_utf8,
                                  rfc_sniff_result *out_result,
                                  uint64_t *out_size,
                                  rfc_file_identity *out_identity,
                                  rfc_sniff_geometry *geo,
                                  const rfc_limits *limits) {
    if (out_size) {
        *out_size = 0u;
    }
    if (out_identity) {
        memset(out_identity, 0, sizeof(*out_identity));
    }
    if (!path_utf8 || !out_result) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    win_file_reader_ctx wctx;
    rfc_io_reader r;
    rfc_status st = open_win_input_reader(path_utf8, &wctx, &r, out_identity);
    if (st != RFC_OK) {
        return st;
    }
    int known = 0;
    uint64_t sz = 0u;
    st = sniff_reader_impl(&r, path_utf8, out_result, &known, &sz, geo, limits);
    CloseHandle(wctx.handle);
    if (out_size) {
        *out_size = sz;
    }
    return st;
#else
    posix_file_reader_ctx pctx;
    rfc_io_reader r;
    rfc_status st = open_posix_input_reader(path_utf8, &pctx, &r, out_identity);
    if (st != RFC_OK) {
        return st;
    }
    int known = 0;
    uint64_t sz = 0u;
    st = sniff_reader_impl(&r, path_utf8, out_result, &known, &sz, geo, limits);
    close(pctx.fd);
    if (out_size) {
        *out_size = sz;
    }
    return st;
#endif
}

rfc_status rfc_input_sniff_path(const char *path_utf8,
                                rfc_sniff_result *out_result,
                                uint64_t *out_size,
                                rfc_file_identity *out_identity) {
    return sniff_path_impl(path_utf8, out_result, out_size, out_identity, NULL,
                           NULL);
}

static rfc_status check_container_completeness(rfc_format_id fmt,
                                               const uint8_t *b,
                                               size_t len,
                                               char *detail,
                                               size_t detail_cap) {
    if (len == 0u || !b) {
        copy_sniff_detail(detail, detail_cap, "empty input stream");
        return RFC_ERR_TRUNCATED_INPUT;
    }

    switch (fmt) {
        case RFC_FORMAT_RASTERFOLD: {
            if (len < 32u) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated .fold header (< 32 bytes)");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            uint32_t plen = rd32le(b + 20);
            if ((uint64_t)32u + (uint64_t)plen > (uint64_t)len) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated .fold payload");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            if (b[16] == 0u) {
                uint64_t w = (uint64_t)rd32le(b + 8);
                uint64_t h = (uint64_t)rd32le(b + 12);
                if (w > 0u && h > 0u && w <= UINT64_MAX / 3u &&
                    h <= UINT64_MAX / (w * 3u)) {
                    uint64_t raw = w * h * 3u;
                    if ((uint64_t)plen < raw) {
                        copy_sniff_detail(detail, detail_cap,
                                          "truncated .fold raw pixel payload");
                        return RFC_ERR_TRUNCATED_INPUT;
                    }
                }
            }
            break;
        }
        case RFC_FORMAT_BMP: {
            if (len < 18u) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated BMP header");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            uint32_t dib = rd32le(b + 14);
            uint32_t fsz = rd32le(b + 2);
            if ((uint64_t)len < 14ull + (uint64_t)dib ||
                ((uint64_t)fsz >= 14ull + (uint64_t)dib &&
                 (uint64_t)len < (uint64_t)fsz)) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated BMP file payload");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            break;
        }
        case RFC_FORMAT_PNG:
        case RFC_FORMAT_APNG: {
            if (len < 8u + 12u + 13u + 12u) {
                copy_sniff_detail(detail, detail_cap, "truncated PNG stream");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            size_t pos = 8u;
            int saw_iend = 0;
            int saw_ihdr = 0;
            while (pos < len) {
                if (len - pos < 12u) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated PNG chunk header");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                uint32_t clen = rd32be(b + pos);
                if ((uint64_t)clen > (uint64_t)(len - pos - 12u)) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated PNG chunk body");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                if (pos == 8u && memcmp(b + pos + 4, "IHDR", 4) == 0 &&
                    clen >= 13u) {
                    saw_ihdr = 1;
                }
                if (memcmp(b + pos + 4, "IEND", 4) == 0) {
                    saw_iend = 1;
                    break;
                }
                pos += 12u + (size_t)clen;
            }
            if (!saw_ihdr || !saw_iend) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated PNG missing IHDR or IEND");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            break;
        }
        case RFC_FORMAT_WEBP: {
            if (len < 12u) {
                copy_sniff_detail(detail, detail_cap, "truncated WebP header");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            uint32_t rsz = rd32le(b + 4);
            if ((uint64_t)len < 8ull + (uint64_t)rsz) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated WebP RIFF payload");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            break;
        }
        case RFC_FORMAT_QOI: {
            if (len < 22u ||
                memcmp(b + len - 8u, "\x00\x00\x00\x00\x00\x00\x00\x01", 8) !=
                    0) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated QOI stream missing end marker");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            break;
        }
        case RFC_FORMAT_GIF: {
            if (len < 14u) {
                copy_sniff_detail(detail, detail_cap, "truncated GIF header");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            size_t pos = 13u;
            uint8_t lsd = b[10];
            if (lsd & 0x80u) {
                size_t gct = (size_t)3u << ((lsd & 0x07u) + 1u);
                if (pos + gct > len) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated GIF global color table");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                pos += gct;
            }
            int saw_trailer = 0;
            while (pos < len) {
                uint8_t intro = b[pos++];
                if (intro == 0x3Bu) {
                    saw_trailer = 1;
                    break;
                } else if (intro == 0x21u) {
                    if (pos >= len) {
                        break;
                    }
                    pos++; /* extension label */
                    while (pos < len) {
                        uint8_t sub = b[pos++];
                        if (sub == 0u) {
                            break;
                        }
                        if ((size_t)sub > len - pos) {
                            pos = len + 1u;
                            break;
                        }
                        pos += (size_t)sub;
                    }
                    if (pos > len) {
                        break;
                    }
                } else if (intro == 0x2Cu) {
                    if (pos + 9u > len) {
                        break;
                    }
                    uint8_t ip = b[pos + 8u];
                    pos += 9u;
                    if (ip & 0x80u) {
                        size_t lct = (size_t)3u << ((ip & 0x07u) + 1u);
                        if (pos + lct > len) {
                            break;
                        }
                        pos += lct;
                    }
                    if (pos >= len) {
                        break;
                    }
                    pos++; /* LZW minimum code size */
                    while (pos < len) {
                        uint8_t sub = b[pos++];
                        if (sub == 0u) {
                            break;
                        }
                        if ((size_t)sub > len - pos) {
                            pos = len + 1u;
                            break;
                        }
                        pos += (size_t)sub;
                    }
                    if (pos > len) {
                        break;
                    }
                } else {
                    break;
                }
            }
            if (!saw_trailer) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated GIF missing trailer");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            break;
        }
        case RFC_FORMAT_JPEG: {
            if (len < 4u) {
                copy_sniff_detail(detail, detail_cap, "truncated JPEG");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            size_t pos = 2u;
            int saw_eoi = 0;
            while (pos < len) {
                if (b[pos] != 0xFFu) {
                    pos++;
                    continue;
                }
                while (pos < len && b[pos] == 0xFFu) {
                    pos++;
                }
                if (pos >= len) {
                    break;
                }
                uint8_t marker = b[pos++];
                if (marker == 0xD9u) {
                    saw_eoi = 1;
                    break;
                }
                if (marker == 0x00u || (marker >= 0xD0u && marker <= 0xD7u) ||
                    marker == 0x01u) {
                    continue;
                }
                if (pos + 2u > len) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated JPEG marker length");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                uint16_t mlen = rd16be(b + pos);
                if (mlen < 2u || (size_t)mlen > len - pos) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated JPEG marker payload");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                pos += (size_t)mlen;
                if (marker == 0xDAu) {
                    /* Scan entropy-coded data up to EOI (0xFF 0xD9). */
                    while (pos + 1u < len) {
                        if (b[pos] == 0xFFu) {
                            uint8_t nxt = b[pos + 1u];
                            if (nxt == 0xD9u) {
                                saw_eoi = 1;
                                pos += 2u;
                                break;
                            }
                            if (nxt == 0x00u || nxt == 0xFFu ||
                                (nxt >= 0xD0u && nxt <= 0xD7u)) {
                                pos += (nxt == 0xFFu) ? 1u : 2u;
                                continue;
                            }
                            break;
                        }
                        pos++;
                    }
                    if (saw_eoi) {
                        break;
                    }
                }
            }
            if (!saw_eoi) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated JPEG missing EOI marker");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            break;
        }
        case RFC_FORMAT_AVIF:
        case RFC_FORMAT_HEIF:
        case RFC_FORMAT_HEIC: {
            if (len < 16u) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated ISOBMFF container");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            size_t pos = 0u;
            while (pos < len) {
                if (len - pos < 8u) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated ISOBMFF box header");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                uint64_t bsz = (uint64_t)rd32be(b + pos);
                if (bsz == 0u) {
                    break;
                }
                if (bsz == 1u) {
                    if (len - pos < 16u) {
                        copy_sniff_detail(detail, detail_cap,
                                          "truncated ISOBMFF extended box");
                        return RFC_ERR_TRUNCATED_INPUT;
                    }
                    bsz = rd64be(b + pos + 8u);
                    if (bsz < 16u || bsz > (uint64_t)(len - pos)) {
                        copy_sniff_detail(detail, detail_cap,
                                          "truncated ISOBMFF extended box body");
                        return RFC_ERR_TRUNCATED_INPUT;
                    }
                } else if (bsz < 8u || bsz > (uint64_t)(len - pos)) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated ISOBMFF box body");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                pos += (size_t)bsz;
            }
            break;
        }
        case RFC_FORMAT_TIFF:
        case RFC_FORMAT_DNG: {
            if (len < 8u) {
                copy_sniff_detail(detail, detail_cap, "truncated TIFF/DNG header");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            int le = (b[0] == 'I' && b[1] == 'I');
            int big = (le ? rd16le(b + 2) : rd16be(b + 2)) == 43u;
            if (big) {
                if (len < 16u) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated BigTIFF header");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                uint64_t ifd_off = le ? rd64le(b + 8) : rd64be(b + 8);
                if (ifd_off > (uint64_t)len || (uint64_t)len - ifd_off < 8u) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated BigTIFF IFD0");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
            } else {
                uint32_t ifd_off = le ? rd32le(b + 4) : rd32be(b + 4);
                if ((uint64_t)ifd_off > (uint64_t)len ||
                    (uint64_t)len - (uint64_t)ifd_off < 2u) {
                    copy_sniff_detail(detail, detail_cap, "truncated TIFF IFD0");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
                uint16_t cnt = le ? rd16le(b + ifd_off) : rd16be(b + ifd_off);
                uint64_t need = (uint64_t)ifd_off + 2ull + (uint64_t)cnt * 12ull;
                if (need > (uint64_t)len) {
                    copy_sniff_detail(detail, detail_cap,
                                      "truncated TIFF IFD0 entries");
                    return RFC_ERR_TRUNCATED_INPUT;
                }
            }
            break;
        }
        case RFC_FORMAT_OPENEXR: {
            if (len < 10u) {
                copy_sniff_detail(detail, detail_cap, "truncated OpenEXR header");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            size_t pos = 8u;
            int found_end = 0;
            while (pos < len) {
                if (b[pos] == 0u) {
                    found_end = 1;
                    pos++;
                    break;
                }
                while (pos < len && b[pos] != 0u) {
                    pos++;
                }
                if (pos >= len) {
                    break;
                }
                pos++; /* attr name NUL */
                while (pos < len && b[pos] != 0u) {
                    pos++;
                }
                if (pos + 5u > len) {
                    break;
                }
                pos++; /* attr type NUL */
                uint32_t asz = rd32le(b + pos);
                pos += 4u;
                if ((uint64_t)asz > (uint64_t)(len - pos)) {
                    pos = len + 1u;
                    break;
                }
                pos += (size_t)asz;
            }
            if (!found_end) {
                copy_sniff_detail(detail, detail_cap,
                                  "truncated OpenEXR header attributes");
                return RFC_ERR_TRUNCATED_INPUT;
            }
            break;
        }
        default:
            break;
    }
    return RFC_OK;
}

static void *input_alloc(const rfc_allocator *alloc, size_t sz) {
    if (alloc && alloc->alloc) {
        return alloc->alloc(alloc->ctx, sz);
    }
    return malloc(sz ? sz : 1u);
}

static void input_free(const rfc_allocator *alloc, void *ptr) {
    if (!ptr) {
        return;
    }
    if (alloc && alloc->free) {
        alloc->free(alloc->ctx, ptr);
    } else {
        free(ptr);
    }
}

rfc_status rfc_input_read_bounded(const rfc_io_reader *reader,
                                  const char *name_utf8,
                                  uint32_t import_flags,
                                  const rfc_limits *limits,
                                  const rfc_allocator *alloc,
                                  const rfc_exec_context *exec,
                                  rfc_imported_input *out_input) {
    if (out_input) {
        memset(out_input, 0, sizeof(*out_input));
    }
    if (!reader || !reader->read_fn || !reader->seek_fn || !out_input) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if ((import_flags & ~(uint32_t)RFC_IMPORT_ALLOW_DISAGREEMENT) != 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_limits eff_limits;
    if (limits) {
        eff_limits = *limits;
        rfc_status lst = rfc_limits_validate(&eff_limits);
        if (lst != RFC_OK) {
            return lst;
        }
    } else {
        rfc_limits_default(&eff_limits);
    }

    rfc_allocator eff_alloc = {NULL, NULL, NULL};
    if (alloc) {
        if (!alloc->alloc || !alloc->free) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        eff_alloc = *alloc;
    }

    if (exec && rfc_exec_is_cancelled(exec)) {
        return RFC_ERR_CANCELLED;
    }

    int size_known = 0;
    uint64_t declared_size = 0u;
    rfc_status st = rfc_input_sniff_reader(
        reader, name_utf8, &out_input->sniff, &size_known, &declared_size);
    if (st != RFC_OK) {
        return st;
    }
    if (out_input->sniff.content_format == (rfc_format_id)0) {
        copy_sniff_detail(out_input->sniff.detail,
                          sizeof(out_input->sniff.detail),
                          "unknown input format");
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (out_input->sniff.disagreement &&
        (import_flags & RFC_IMPORT_ALLOW_DISAGREEMENT) == 0u) {
        copy_sniff_detail(out_input->sniff.detail,
                          sizeof(out_input->sniff.detail),
                          "extension disagrees with sniffed content format");
        return RFC_ERR_FORMAT_DISAGREEMENT;
    }
    if (size_known) {
        st = rfc_limits_check_compressed(&eff_limits, declared_size);
        if (st != RFC_OK) {
            copy_sniff_detail(out_input->sniff.detail,
                              sizeof(out_input->sniff.detail),
                              "compressed input exceeds configured byte limit");
            return st;
        }
        if (declared_size == 0u || declared_size > (uint64_t)SIZE_MAX) {
            copy_sniff_detail(out_input->sniff.detail,
                              sizeof(out_input->sniff.detail),
                              declared_size == 0u
                                  ? "empty input stream"
                                  : "compressed input exceeds configured byte limit");
            return declared_size == 0u ? RFC_ERR_TRUNCATED_INPUT
                                       : RFC_ERR_LIMIT_EXCEEDED;
        }
    }

    if (reader->identity_fn) {
        rfc_status ist = reader->identity_fn(reader->ctx, &out_input->identity);
        if (ist != RFC_OK) {
            memset(&out_input->identity, 0, sizeof(out_input->identity));
        }
    }

    st = reader->seek_fn(reader->ctx, 0u);
    if (st != RFC_OK) {
        copy_sniff_detail(out_input->sniff.detail,
                          sizeof(out_input->sniff.detail),
                          "failed to rewind input stream");
        return st;
    }

    uint64_t max_bytes = eff_limits.max_compressed_bytes;
    if (max_bytes > (uint64_t)SIZE_MAX) {
        max_bytes = (uint64_t)SIZE_MAX;
    }

    uint8_t *buf = NULL;
    size_t cap = 0u;
    size_t used = 0u;
    if (size_known) {
        cap = (size_t)declared_size;
        buf = (uint8_t *)input_alloc(alloc, cap);
        if (!buf) {
            copy_sniff_detail(out_input->sniff.detail,
                              sizeof(out_input->sniff.detail),
                              "out of memory reading input stream");
            return RFC_ERR_OUT_OF_MEMORY;
        }
    }

    uint8_t chunk[RFC_INPUT_CHUNK_BYTES];
    for (;;) {
        if (exec && rfc_exec_is_cancelled(exec)) {
            input_free(alloc, buf);
            copy_sniff_detail(out_input->sniff.detail,
                              sizeof(out_input->sniff.detail),
                              "input read cancelled");
            return RFC_ERR_CANCELLED;
        }
        size_t got = 0u;
        st = reader->read_fn(reader->ctx, chunk, sizeof(chunk), &got);
        if (st != RFC_OK) {
            input_free(alloc, buf);
            copy_sniff_detail(out_input->sniff.detail,
                              sizeof(out_input->sniff.detail),
                              "input stream read failed");
            return st;
        }
        if (got == 0u) {
            break;
        }
        if (got > sizeof(chunk)) {
            input_free(alloc, buf);
            copy_sniff_detail(out_input->sniff.detail,
                              sizeof(out_input->sniff.detail),
                              "reader callback returned byte count exceeding buffer");
            return RFC_ERR_IO;
        }
        if ((uint64_t)got > max_bytes - (uint64_t)used ||
            got > SIZE_MAX - used) {
            input_free(alloc, buf);
            copy_sniff_detail(out_input->sniff.detail,
                              sizeof(out_input->sniff.detail),
                              "compressed input exceeds configured byte limit");
            return RFC_ERR_LIMIT_EXCEEDED;
        }
        if (size_known && (uint64_t)got > declared_size - (uint64_t)used) {
            input_free(alloc, buf);
            copy_sniff_detail(out_input->sniff.detail,
                              sizeof(out_input->sniff.detail),
                              "input stream exceeded declared byte size");
            return RFC_ERR_IO;
        }
        size_t needed = used + got;
        if (needed > cap) {
            size_t next_cap = cap ? cap : 4096u;
            while (next_cap < needed) {
                if (next_cap > SIZE_MAX / 2u) {
                    next_cap = needed;
                    break;
                }
                next_cap *= 2u;
            }
            if ((uint64_t)next_cap > max_bytes) {
                next_cap = (size_t)max_bytes;
            }
            uint8_t *nb = (uint8_t *)input_alloc(alloc, next_cap);
            if (!nb) {
                input_free(alloc, buf);
                copy_sniff_detail(out_input->sniff.detail,
                                  sizeof(out_input->sniff.detail),
                                  "out of memory reading input stream");
                return RFC_ERR_OUT_OF_MEMORY;
            }
            if (used > 0u && buf) {
                memcpy(nb, buf, used);
            }
            input_free(alloc, buf);
            buf = nb;
            cap = next_cap;
        }
        memcpy(buf + used, chunk, got);
        used += got;
    }

    if (size_known && (uint64_t)used < declared_size) {
        input_free(alloc, buf);
        copy_sniff_detail(out_input->sniff.detail,
                          sizeof(out_input->sniff.detail),
                          "stream ended before declared size");
        return RFC_ERR_TRUNCATED_INPUT;
    }

    rfc_sniff_result buf_sniff;
    rfc_status bst = rfc_input_sniff_bytes(buf, used, name_utf8, &buf_sniff);
    if (bst != RFC_OK || buf_sniff.content_format == (rfc_format_id)0) {
        input_free(alloc, buf);
        copy_sniff_detail(
            out_input->sniff.detail, sizeof(out_input->sniff.detail),
            buf_sniff.detail[0] ? buf_sniff.detail : "unknown input format");
        if (bst == RFC_ERR_FORMAT_DISAGREEMENT &&
            (import_flags & RFC_IMPORT_ALLOW_DISAGREEMENT) == 0u) {
            return RFC_ERR_FORMAT_DISAGREEMENT;
        }
        return (bst != RFC_OK && bst != RFC_ERR_FORMAT_DISAGREEMENT)
                   ? bst
                   : RFC_ERR_UNKNOWN_FORMAT;
    }
    if (buf_sniff.content_format != out_input->sniff.content_format) {
        input_free(alloc, buf);
        copy_sniff_detail(out_input->sniff.detail,
                          sizeof(out_input->sniff.detail),
                          "input stream content changed between sniff and read");
        return RFC_ERR_FORMAT_DISAGREEMENT;
    }
    if (buf_sniff.disagreement &&
        (import_flags & RFC_IMPORT_ALLOW_DISAGREEMENT) == 0u) {
        input_free(alloc, buf);
        copy_sniff_detail(out_input->sniff.detail,
                          sizeof(out_input->sniff.detail),
                          "extension disagrees with sniffed content format");
        return RFC_ERR_FORMAT_DISAGREEMENT;
    }
    out_input->sniff = buf_sniff;

    st = check_container_completeness(
        out_input->sniff.content_format, buf, used,
        out_input->sniff.detail, sizeof(out_input->sniff.detail));
    if (st != RFC_OK) {
        input_free(alloc, buf);
        return st;
    }

    out_input->bytes = buf;
    out_input->size = used;
    out_input->allocator = eff_alloc;
    return RFC_OK;
}

rfc_status rfc_input_read_path(const char *path_utf8,
                               uint32_t import_flags,
                               const rfc_limits *limits,
                               const rfc_allocator *alloc,
                               const rfc_exec_context *exec,
                               rfc_imported_input *out_input) {
    if (!path_utf8 || !out_input) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    win_file_reader_ctx wctx;
    rfc_io_reader r;
    rfc_file_identity id;
    rfc_status st = open_win_input_reader(path_utf8, &wctx, &r, &id);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_input_read_bounded(&r, path_utf8, import_flags, limits, alloc,
                                exec, out_input);
    CloseHandle(wctx.handle);
    if (st == RFC_OK) {
        out_input->identity = id;
    }
    return st;
#else
    posix_file_reader_ctx pctx;
    rfc_io_reader r;
    rfc_file_identity id;
    rfc_status st = open_posix_input_reader(path_utf8, &pctx, &r, &id);
    if (st != RFC_OK) {
        return st;
    }
    st = rfc_input_read_bounded(&r, path_utf8, import_flags, limits, alloc,
                                exec, out_input);
    close(pctx.fd);
    if (st == RFC_OK) {
        out_input->identity = id;
    }
    return st;
#endif
}

void rfc_imported_input_free(rfc_imported_input *input) {
    if (!input) {
        return;
    }
    if (input->bytes) {
        if (input->allocator.free) {
            input->allocator.free(input->allocator.ctx, input->bytes);
        } else {
            free(input->bytes);
        }
    }
    memset(input, 0, sizeof(*input));
}

rfc_status rfc_input_sniff_source_preflight(
    const rfc_limits *limits,
    const rfc_io_source *io_source,
    rfc_format_id caller_source_format,
    rfc_sniff_result *out_sniff,
    uint64_t *out_source_size,
    rfc_file_identity *out_identity,
    rfc_sniff_geometry *out_geometry) {
    if (out_geometry) {
        memset(out_geometry, 0, sizeof(*out_geometry));
    }
    if (!limits || !io_source || !out_sniff || !out_source_size ||
        !out_identity) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_sniff, 0, sizeof(*out_sniff));
    *out_source_size = 0u;
    memset(out_identity, 0, sizeof(*out_identity));

    if ((io_source->import_flags & ~(uint32_t)RFC_IMPORT_ALLOW_DISAGREEMENT) !=
        0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if ((io_source->path_utf8 != NULL) == (io_source->reader != NULL)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (io_source->virtual_name_utf8 &&
        !validate_utf8_cstr(io_source->virtual_name_utf8, 4096u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_status st = RFC_OK;
    if (io_source->path_utf8) {
        st = sniff_path_impl(io_source->path_utf8, out_sniff, out_source_size,
                             out_identity, out_geometry, limits);
    } else {
        const rfc_io_reader *r = io_source->reader;
        if (!r->read_fn || !r->seek_fn) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        int known = 0;
        uint64_t sz = 0u;
        st = sniff_reader_impl(r, io_source->virtual_name_utf8, out_sniff,
                               &known, &sz, out_geometry, limits);
        *out_source_size = known ? sz : 0u;
        if (st == RFC_OK && r->identity_fn) {
            if (r->identity_fn(r->ctx, out_identity) != RFC_OK) {
                memset(out_identity, 0, sizeof(*out_identity));
            }
        }
    }

    if (st != RFC_OK) {
        return st;
    }
    if (out_sniff->content_format == (rfc_format_id)0) {
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if (out_sniff->disagreement &&
        (io_source->import_flags & RFC_IMPORT_ALLOW_DISAGREEMENT) == 0u) {
        return RFC_ERR_FORMAT_DISAGREEMENT;
    }
    if (caller_source_format != (rfc_format_id)0 &&
        caller_source_format != out_sniff->content_format) {
        if (!rfc_registry_find(caller_source_format)) {
            return RFC_ERR_UNKNOWN_FORMAT;
        }
        return RFC_ERR_FORMAT_DISAGREEMENT;
    }
    if (*out_source_size > limits->max_compressed_bytes) {
        return RFC_ERR_LIMIT_EXCEEDED;
    }
    return RFC_OK;
}

rfc_status rfc_input_read_job_source(
    rfc_job *job,
    const rfc_exec_context *exec,
    const rfc_io_source *io_source,
    rfc_io_reader_box *reader_box,
    uint32_t import_flags,
    rfc_format_id expected_format,
    const char **out_err_detail) {
    if (out_err_detail) {
        *out_err_detail = NULL;
    }
    if (!job || !exec || !io_source) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_imported_input imp;
    memset(&imp, 0, sizeof(imp));
    rfc_status st = RFC_OK;

    if (io_source->path_utf8) {
        st = rfc_input_read_path(io_source->path_utf8, import_flags,
                                 &exec->limits, exec->allocator, exec, &imp);
    } else if (reader_box) {
        rfc_mutex_lock(&reader_box->lock);
        st = rfc_input_read_bounded(&reader_box->reader,
                                    io_source->virtual_name_utf8, import_flags,
                                    &exec->limits, exec->allocator, exec, &imp);
        rfc_mutex_unlock(&reader_box->lock);
    } else {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    if (st != RFC_OK) {
        if (out_err_detail && imp.sniff.detail[0] != '\0') {
            rfc_mutex_lock(&job->lock);
            rfc_job_internal_set_detail(job->detail, imp.sniff.detail);
            rfc_mutex_unlock(&job->lock);
        }
        return st;
    }

    /* Enforce that TOCTOU file replacement did not alter sniffed content format. */
    if (imp.sniff.content_format == (rfc_format_id)0 ||
        (expected_format != (rfc_format_id)0 &&
         imp.sniff.content_format != expected_format)) {
        if (exec->allocator && exec->allocator->free && imp.bytes) {
            exec->allocator->free(exec->allocator->ctx, imp.bytes);
        } else {
            free(imp.bytes);
        }
        if (out_err_detail) {
            *out_err_detail = "imported content disagrees with expected format";
        }
        rfc_mutex_lock(&job->lock);
        rfc_job_internal_set_detail(
            job->detail, "imported content disagrees with expected format");
        rfc_mutex_unlock(&job->lock);
        return RFC_ERR_FORMAT_DISAGREEMENT;
    }

    rfc_mutex_lock(&job->lock);
    uint8_t *old_bytes = job->imported_source_bytes;
    job->imported_source_bytes = imp.bytes;
    job->imported_source_size = imp.size;
    job->sniff_result = imp.sniff;
    job->has_sniff_result = 1;
    job->source_identity = imp.identity;
    rfc_mutex_unlock(&job->lock);

    if (old_bytes) {
        if (exec->allocator && exec->allocator->free) {
            exec->allocator->free(exec->allocator->ctx, old_bytes);
        } else {
            free(old_bytes);
        }
    }
    return RFC_OK;
}
