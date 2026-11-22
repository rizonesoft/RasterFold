/* C11 test harness + CLI for JPEG 2000 session adapter (D10 T02 s9). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "rfc_codecs.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_verify.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#define set_env_var(k, v) _putenv_s((k), (v))
#define unset_env_var(k) _putenv_s((k), "")
#define jp2_test_yield() Sleep(0)
static int get_process_thread_count(void) {
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return -1;
    }
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    int count = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                count++;
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}
#else
#include <sched.h>
#include <unistd.h>
#define set_env_var(k, v) setenv((k), (v), 1)
#define unset_env_var(k) unsetenv((k))
#define jp2_test_yield() sched_yield()
static int get_process_thread_count(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) {
        return -1;
    }
    char line[256];
    int count = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Threads:", 8) == 0) {
            count = atoi(line + 8);
            break;
        }
    }
    fclose(f);
    return count;
}
#endif

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK FAILED (%s:%d): %s\n", __FILE__,          \
                    __LINE__, #cond);                                        \
            g_fails++;                                                       \
            return;                                                          \
        }                                                                    \
    } while (0)

typedef struct tracked_alloc {
    size_t live_bytes;
    size_t peak_bytes;
    size_t alloc_count;
} tracked_alloc;

static void *t_alloc(void *ctx, size_t n) {
    tracked_alloc *t = (tracked_alloc *)ctx;
    size_t *hdr = (size_t *)malloc(sizeof(size_t) + (n ? n : 1u));
    if (!hdr) {
        return NULL;
    }
    *hdr = n;
    t->live_bytes += n;
    if (t->live_bytes > t->peak_bytes) {
        t->peak_bytes = t->live_bytes;
    }
    t->alloc_count++;
    return (void *)(hdr + 1);
}

static void t_free(void *ctx, void *p) {
    if (!p) {
        return;
    }
    tracked_alloc *t = (tracked_alloc *)ctx;
    size_t *hdr = ((size_t *)p) - 1;
    t->live_bytes -= *hdr;
    free(hdr);
}

static void init_exec(rfc_exec_context *exec,
                      tracked_alloc *st,
                      rfc_allocator *al,
                      rfc_budget *budget) {
    memset(st, 0, sizeof(*st));
    memset(al, 0, sizeof(*al));
    al->ctx = st;
    al->alloc = t_alloc;
    al->free = t_free;
    memset(exec, 0, sizeof(*exec));
    exec->allocator = al;
    exec->budget = budget;
    exec->codec_threads = 1u;
    rfc_limits_default(&exec->limits);
}

static void temp_path(char *out, size_t cap, const char *name) {
#ifdef _WIN32
    char dir[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(dir), dir);
    if (n == 0u || n >= sizeof(dir)) {
        snprintf(out, cap, "%s", name);
        return;
    }
    snprintf(out, cap, "%srfc_jp2_%lu_%s", dir,
             (unsigned long)GetCurrentProcessId(), name);
#else
    snprintf(out, cap, "/tmp/rfc_jp2_%ld_%s", (long)getpid(), name);
#endif
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fclose(f);
    return 1;
}

static int write_file_all(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return 0;
    }
    int ok = (len == 0u) || (fwrite(buf, 1u, len, f) == len);
    fclose(f);
    return ok;
}

static uint8_t *read_file_all(const char *path, size_t *out_len) {
    *out_len = 0u;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1u, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

/* Locate a top-level JP2 box payload. */
static int find_jp2_box(const uint8_t *buf,
                        size_t len,
                        const char tag[4],
                        const uint8_t **out_ptr,
                        size_t *out_len) {
    if (len < 12u) {
        return 0;
    }
    size_t off = 12u;
    while (off + 8u <= len) {
        uint32_t l = be32(buf + off);
        size_t total = (l == 0u) ? (len - off) : (size_t)l;
        if (total < 8u || off + total > len) {
            return 0;
        }
        if (memcmp(buf + off + 4u, tag, 4u) == 0) {
            *out_ptr = buf + off + 8u;
            *out_len = total - 8u;
            return 1;
        }
        off += total;
    }
    return 0;
}

static int find_sub_box(const uint8_t *buf,
                        size_t len,
                        const char tag[4],
                        const uint8_t **out_ptr,
                        size_t *out_len) {
    size_t off = 0u;
    while (off + 8u <= len) {
        uint32_t l = be32(buf + off);
        size_t total = (l == 0u) ? (len - off) : (size_t)l;
        if (total < 8u || off + total > len) {
            return 0;
        }
        if (memcmp(buf + off + 4u, tag, 4u) == 0) {
            *out_ptr = buf + off + 8u;
            *out_len = total - 8u;
            return 1;
        }
        off += total;
    }
    return 0;
}

/* Verify that a JP2 file's jp2c codestream carries a 5-3 reversible COD marker. */
static int verify_cod_reversible(const uint8_t *jp2, size_t jp2_len) {
    const uint8_t *cs = NULL;
    size_t cs_len = 0u;
    if (!find_jp2_box(jp2, jp2_len, "jp2c", &cs, &cs_len)) {
        return 0;
    }
    if (cs_len < 6u || cs[0] != 0xFFu || cs[1] != 0x4Fu ||
        cs[2] != 0xFFu || cs[3] != 0x51u) {
        return 0;
    }
    uint16_t lsiz = be16(cs + 4u);
    size_t pos = 4u + (size_t)lsiz;
    while (pos + 4u <= cs_len) {
        if (cs[pos] != 0xFFu) {
            return 0;
        }
        uint8_t m = cs[pos + 1u];
        if (m == 0x90u) {
            break;
        }
        uint16_t ml = be16(cs + pos + 2u);
        if (ml < 2u || pos + 2u + ml > cs_len) {
            return 0;
        }
        if (m == 0x52u && ml >= 12u) {
            return cs[pos + 13u] == 1u; /* 1 == 5-3 reversible */
        }
        pos += 2u + ml;
    }
    return 0;
}

/* ---- Test 1: Registration and geometry sniffing ---- */

static void test_registration_and_geometry(void) {
    CHECK(rfc_registry_select(RFC_FORMAT_JPEG2000, RFC_MODE_READ) ==
          RFC_ERR_NO_ADAPTER);
    CHECK(rfc_codecs_register_jp2() == RFC_OK);
    CHECK(rfc_codecs_register_jp2() == RFC_ERR_DUPLICATE);
    CHECK(rfc_registry_select(RFC_FORMAT_JPEG2000, RFC_MODE_READ) == RFC_OK);
    CHECK(rfc_registry_select(RFC_FORMAT_JPEG2000, RFC_MODE_WRITE) == RFC_OK);
    CHECK(rfc_codecs_unregister_jp2() == RFC_OK);
    CHECK(rfc_registry_select(RFC_FORMAT_JPEG2000, RFC_MODE_READ) ==
          RFC_ERR_NO_ADAPTER);
    CHECK(rfc_codecs_register_jp2() == RFC_OK);

    /* Encode a small RGBA8 JP2 and verify both JP2 and bare-J2K geometry sniffing. */
    const rfc_session_adapter *adp =
        rfc_registry_session_adapter(RFC_FORMAT_JPEG2000);
    CHECK(adp != NULL);
    tracked_alloc st;
    rfc_allocator al;
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, NULL);

    rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 13u, 9u, 13u * 4u, 8u};
    rfc_frame src;
    CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
    for (size_t i = 0u; i < src.data_len; i++) {
        src.data[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }
    void *sess = NULL;
    CHECK(adp->open_session(&exec, &sess) == RFC_OK);
    rfc_encoded enc;
    CHECK(adp->encode_session(sess, &exec, &src, 1u, NULL, NULL, 0u, &enc) ==
          RFC_OK);
    adp->close_session(sess);

    rfc_sniff_result sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(rfc_input_sniff_bytes(enc.data, enc.len, "a.jp2", &sr) == RFC_OK);
    CHECK(sr.content_format == RFC_FORMAT_JPEG2000 && !sr.disagreement);

    /* RGB8 bare J2K sniff */
    rfc_frame_free(&src);
    rfc_encoded_free(&enc);
    rfc_layout lay3 = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 11u, 7u, 11u * 3u, 8u};
    CHECK(rfc_frame_alloc(&src, &lay3, NULL, &al) == RFC_OK);
    memset(src.data, 0x5Au, src.data_len);
    CHECK(adp->open_session(&exec, &sess) == RFC_OK);
    CHECK(adp->encode_session(sess, &exec, &src, 1u, NULL, NULL, 0u, &enc) ==
          RFC_OK);
    adp->close_session(sess);
    const uint8_t *cs = NULL;
    size_t cs_len = 0u;
    CHECK(find_jp2_box(enc.data, enc.len, "jp2c", &cs, &cs_len));
    memset(&sr, 0, sizeof(sr));
    CHECK(rfc_input_sniff_bytes(cs, cs_len, "b.j2k", &sr) == RFC_OK);
    CHECK(sr.content_format == RFC_FORMAT_JPEG2000 && !sr.disagreement);

    rfc_frame_free(&src);
    rfc_encoded_free(&enc);
    CHECK(st.live_bytes == 0u);
    printf("jp2: registration_and_geometry ok\n");
}

/* ---- Test 2: All 6 sample formats, small sizes, reduced precision, stride, ICC+RGBA cdef, bare J2K ---- */

static void test_samples_precision_stride_and_icc(void) {
    const rfc_session_adapter *adp =
        rfc_registry_session_adapter(RFC_FORMAT_JPEG2000);
    CHECK(adp != NULL);
    tracked_alloc st;
    rfc_allocator al;
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, NULL);

    /* 1. All 6 native sample formats + small dimensions (1x1, 1x17, 17x1, 43x31). */
    static const struct {
        rfc_sample_format fmt;
        rfc_alpha_mode alpha;
        uint32_t w;
        uint32_t h;
        uint32_t bpp;
        uint32_t bits;
    } cases[] = {
        {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 1u, 1u, 1u, 8u},
        {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 1u, 17u, 1u, 8u},
        {RFC_SAMPLE_GRAY16, RFC_ALPHA_NONE, 17u, 1u, 2u, 16u},
        {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 43u, 31u, 3u, 8u},
        {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 29u, 23u, 4u, 8u},
        {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 19u, 15u, 6u, 16u},
        {RFC_SAMPLE_RGBA16, RFC_ALPHA_STRAIGHT, 17u, 13u, 8u, 16u},
    };
    for (size_t ci = 0u; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        rfc_layout lay = {cases[ci].fmt, cases[ci].alpha, cases[ci].w,
                          cases[ci].h, cases[ci].w * cases[ci].bpp,
                          cases[ci].bits};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        for (size_t i = 0u; i < src.data_len; i++) {
            src.data[i] = (uint8_t)((i * 73u + ci * 19u + 7u) & 0xFFu);
        }
        /* For RGBA8/RGBA16, put A == 0 with non-zero RGB on pixel 0. */
        if (cases[ci].fmt == RFC_SAMPLE_RGBA8) {
            src.data[0] = 0xABu;
            src.data[1] = 0xCDu;
            src.data[2] = 0xEFu;
            src.data[3] = 0x00u;
        } else if (cases[ci].fmt == RFC_SAMPLE_RGBA16) {
            uint16_t *p16 = (uint16_t *)src.data;
            p16[0] = 0x1234u;
            p16[1] = 0x5678u;
            p16[2] = 0x9ABCu;
            p16[3] = 0x0000u;
        }

        void *s_enc = NULL;
        CHECK(adp->open_session(&exec, &s_enc) == RFC_OK);
        rfc_encoded enc;
        CHECK(adp->encode_session(s_enc, &exec, &src, 1u, NULL, NULL, 0u,
                                  &enc) == RFC_OK);
        adp->close_session(s_enc);
        CHECK(verify_cod_reversible(enc.data, enc.len));

        void *s_dec = NULL;
        CHECK(adp->open_session(&exec, &s_dec) == RFC_OK);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(s_dec, &exec, enc.data, enc.len, NULL, 0u,
                                  &dec, &dc, &dm) == RFC_OK);
        adp->close_session(s_dec);
        CHECK(dc == 1u && dec[0].layout.format == cases[ci].fmt &&
              dec[0].layout.alpha == cases[ci].alpha &&
              dec[0].layout.width == cases[ci].w &&
              dec[0].layout.height == cases[ci].h &&
              dec[0].layout.bits_per_sample == cases[ci].bits &&
              dec[0].data_len == src.data_len &&
              memcmp(dec[0].data, src.data, src.data_len) == 0);

        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
    }

    /* 2. Padded-stride multi-row frame round-trip. */
    {
        uint32_t w = 11u, h = 7u;
        size_t tight = (size_t)w * 3u;
        size_t stride = tight + 19u;
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, w, h, stride, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        memset(src.data, 0xEEu, src.data_len);
        for (uint32_t y = 0u; y < h; y++) {
            for (size_t x = 0u; x < tight; x++) {
                src.data[(size_t)y * stride + x] =
                    (uint8_t)((y * 41u + x * 13u) & 0xFFu);
            }
        }
        void *se = NULL, *sd = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded enc;
        CHECK(adp->encode_session(se, &exec, &src, 1u, NULL, NULL, 0u, &enc) ==
              RFC_OK);
        adp->close_session(se);
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(sd, &exec, enc.data, enc.len, NULL, 0u, &dec,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sd);
        CHECK(dc == 1u);
        for (uint32_t y = 0u; y < h; y++) {
            CHECK(memcmp(dec[0].data + (size_t)y * dec[0].layout.stride_bytes,
                         src.data + (size_t)y * stride, tight) == 0);
        }
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
    }

    /* 3. Reduced-precision left-aligned MSB contract (Plan #5):
     * 12-bit valid 0xFFF0 passes and emits prec=12; invalid 0xFFF1 refused. */
    {
        rfc_layout lay12 = {RFC_SAMPLE_RGB16, RFC_ALPHA_NONE, 4u, 4u,
                            4u * 6u, 12u};
        rfc_frame src12;
        CHECK(rfc_frame_alloc(&src12, &lay12, NULL, &al) == RFC_OK);
        uint16_t *p = (uint16_t *)src12.data;
        for (size_t i = 0u; i < 4u * 4u * 3u; i++) {
            p[i] = (uint16_t)(((i * 257u) & 0x0FFFu) << 4);
        }
        p[0] = 0xFFF0u;
        void *se = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded enc;
        CHECK(adp->encode_session(se, &exec, &src12, 1u, NULL, NULL, 0u,
                                  &enc) == RFC_OK);
        /* Verify SIZ Ssiz byte reports 12-bit unsigned (prec - 1 == 11). */
        const uint8_t *cs = NULL;
        size_t cs_len = 0u;
        CHECK(find_jp2_box(enc.data, enc.len, "jp2c", &cs, &cs_len));
        CHECK(cs_len > 42u && cs[42u] == 11u);
        void *sd = NULL;
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(sd, &exec, enc.data, enc.len, NULL, 0u, &dec,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sd);
        CHECK(dc == 1u && dec[0].layout.bits_per_sample == 12u &&
              memcmp(dec[0].data, src12.data, src12.data_len) == 0);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        rfc_encoded_free(&enc);

        /* Set low padding bit (0xFFF1): must be refused with RFC_ERR_INVALID_ARGUMENT. */
        p[0] = 0xFFF1u;
        CHECK(adp->encode_session(se, &exec, &src12, 1u, NULL, NULL, 0u,
                                  &enc) == RFC_ERR_INVALID_ARGUMENT);
        adp->close_session(se);
        rfc_frame_free(&src12);
    }

    /* 4. ICC-tagged RGBA8 (Plan #1): BOTH colr (meth=2) AND cdef (typ=1 on ch 3)
     * must be present in jp2h, and A==0 RGB survives fresh decode. */
    {
        static const uint8_t k_icc[32] = {
            0x00, 0x00, 0x00, 0x20, 't', 'e', 's', 't',
            0x04, 0x30, 0x00, 0x00, 'm', 'n', 't', 'r',
            'R',  'G',  'B',  ' ',  'X', 'Y', 'Z', ' ',
            'a',  'c',  's',  'p',  1,   2,   3,   4
        };
        rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 8u, 6u,
                          8u * 4u, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        for (size_t i = 0u; i < src.data_len; i++) {
            src.data[i] = (uint8_t)((i * 29u + 5u) & 0xFFu);
        }
        src.data[0] = 0xDEu;
        src.data[1] = 0xADu;
        src.data[2] = 0xBEu;
        src.data[3] = 0x00u; /* A == 0 with non-zero RGB */
        rfc_metadata m_in;
        memset(&m_in, 0, sizeof(m_in));
        m_in.icc = (uint8_t *)k_icc;
        m_in.icc_len = sizeof(k_icc);

        void *se = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded enc;
        CHECK(adp->encode_session(se, &exec, &src, 1u, &m_in, NULL, 0u, &enc) ==
              RFC_OK);
        adp->close_session(se);

        /* Inspect jp2h for BOTH colr(meth=2) and cdef(cn=3, typ=1, asoc=0). */
        const uint8_t *jp2h = NULL, *colr = NULL, *cdef = NULL;
        size_t jp2h_len = 0u, colr_len = 0u, cdef_len = 0u;
        CHECK(find_jp2_box(enc.data, enc.len, "jp2h", &jp2h, &jp2h_len));
        CHECK(find_sub_box(jp2h, jp2h_len, "colr", &colr, &colr_len));
        CHECK(colr_len == 3u + sizeof(k_icc) && colr[0] == 2u &&
              memcmp(colr + 3u, k_icc, sizeof(k_icc)) == 0);
        CHECK(find_sub_box(jp2h, jp2h_len, "cdef", &cdef, &cdef_len));
        CHECK(cdef_len == 2u + 4u * 6u && be16(cdef) == 4u &&
              be16(cdef + 2u + 3u * 6u) == 3u &&
              be16(cdef + 2u + 3u * 6u + 2u) == 1u &&
              be16(cdef + 2u + 3u * 6u + 4u) == 0u);

        /* Fresh decode preserves exact pixels (including A==0 RGB) and ICC. */
        void *sd = NULL;
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(sd, &exec, enc.data, enc.len, NULL, 0u, &dec,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sd);
        CHECK(dc == 1u && dec[0].layout.format == RFC_SAMPLE_RGBA8 &&
              dec[0].layout.alpha == RFC_ALPHA_STRAIGHT &&
              memcmp(dec[0].data, src.data, src.data_len) == 0 &&
              dm.icc_len == sizeof(k_icc) &&
              memcmp(dm.icc, k_icc, sizeof(k_icc)) == 0);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);

        /* Bare 4-component J2K codestream (extracted jp2c) must be REFUSED (Plan #2). */
        const uint8_t *cs4 = NULL;
        size_t cs4_len = 0u;
        CHECK(find_jp2_box(enc.data, enc.len, "jp2c", &cs4, &cs4_len));
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        size_t before_alloc = st.alloc_count;
        CHECK(adp->decode_session(sd, &exec, cs4, cs4_len, NULL, 0u, &dec, &dc,
                                  &dm) == RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(st.alloc_count == before_alloc);
        adp->close_session(sd);

        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
    }

    /* 5. Bare 1-ch (GRAY8) and 3-ch (RGB8) J2K codestreams import accurately (Plan #2). */
    for (int ch = 1; ch <= 3; ch += 2) {
        rfc_layout lay = {ch == 1 ? RFC_SAMPLE_GRAY8 : RFC_SAMPLE_RGB8,
                          RFC_ALPHA_NONE, 9u, 7u, (size_t)9u * (size_t)ch, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        for (size_t i = 0u; i < src.data_len; i++) {
            src.data[i] = (uint8_t)((i * 53u + 3u) & 0xFFu);
        }
        void *se = NULL, *sd = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded enc;
        CHECK(adp->encode_session(se, &exec, &src, 1u, NULL, NULL, 0u, &enc) ==
              RFC_OK);
        adp->close_session(se);
        const uint8_t *cs = NULL;
        size_t cs_len = 0u;
        CHECK(find_jp2_box(enc.data, enc.len, "jp2c", &cs, &cs_len));
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(sd, &exec, cs, cs_len, NULL, 0u, &dec, &dc,
                                  &dm) == RFC_OK);
        adp->close_session(sd);
        CHECK(dc == 1u && dec[0].layout.format == lay.format &&
              memcmp(dec[0].data, src.data, src.data_len) == 0);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
    }

    CHECK(st.live_bytes == 0u);
    printf("jp2: samples_precision_stride_and_icc ok\n");
}

/* ---- Test 3: Refusals, two-tier malformed inputs, metadata policy ---- */

static void test_refusals_malformed_and_metadata_policy(void) {
    const rfc_session_adapter *adp =
        rfc_registry_session_adapter(RFC_FORMAT_JPEG2000);
    CHECK(adp != NULL);
    tracked_alloc st;
    rfc_allocator al;
    rfc_budget *bud = rfc_budget_create(16u * 1024u * 1024u);
    CHECK(bud != NULL);
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, bud);

    /* Encode refusals: byte-identical, multi-frame, premultiplied alpha. */
    {
        rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 4u, 4u, 16u, 8u};
        rfc_frame fr[2];
        CHECK(rfc_frame_alloc(&fr[0], &lay, NULL, &al) == RFC_OK);
        CHECK(rfc_frame_alloc(&fr[1], &lay, NULL, &al) == RFC_OK);
        memset(fr[0].data, 0x40, fr[0].data_len);
        memset(fr[1].data, 0x40, fr[1].data_len);
        void *se = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded enc;
        rfc_setting_value bi[1] = {{"exactness", "byte-identical"}};
        CHECK(adp->encode_session(se, &exec, &fr[0], 1u, NULL, bi, 1u, &enc) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(adp->encode_session(se, &exec, fr, 2u, NULL, NULL, 0u, &enc) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        fr[0].layout.alpha = RFC_ALPHA_PREMULTIPLIED;
        CHECK(adp->encode_session(se, &exec, &fr[0], 1u, NULL, NULL, 0u,
                                  &enc) == RFC_ERR_UNSUPPORTED_ALPHA);
        adp->close_session(se);
        rfc_frame_free(&fr[0]);
        rfc_frame_free(&fr[1]);
    }

    /* Build a valid 8x8 RGB8 JP2 to derive Tier-1 (structural/representation)
     * and Tier-2 (valid framing, corrupt tile payload) malformed fixtures. */
    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 8u, 8u, 24u, 8u};
    rfc_frame src;
    CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
    for (size_t i = 0u; i < src.data_len; i++) {
        src.data[i] = (uint8_t)((i * 17u + 3u) & 0xFFu);
    }
    void *se = NULL;
    CHECK(adp->open_session(&exec, &se) == RFC_OK);
    rfc_encoded valid;
    CHECK(adp->encode_session(se, &exec, &src, 1u, NULL, NULL, 0u, &valid) ==
          RFC_OK);
    adp->close_session(se);

    /* Tier-1: bad magic, truncated header, signed Ssiz, subsampled XRsiz=2,
     * over-limit dimensions: all must fail BEFORE any frame allocation. */
    {
        static const uint8_t bad_sig[16] = {'N', 'O', 'P', 'E', 0, 0, 0, 0,
                                            0,   0,   0,   0,   0, 0, 0, 0};
        void *sd = NULL;
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        size_t ac0 = st.alloc_count;
        CHECK(adp->decode_session(sd, &exec, bad_sig, sizeof(bad_sig), NULL, 0u,
                                  &dec, &dc, &dm) == RFC_ERR_INVALID_ARGUMENT);
        CHECK(st.alloc_count == ac0);
        CHECK(adp->decode_session(sd, &exec, valid.data, 16u, NULL, 0u, &dec,
                                  &dc, &dm) == RFC_ERR_TRUNCATED_INPUT);
        CHECK(st.alloc_count == ac0);

        /* Mutate SIZ Ssiz to signed (bit 7 set) -> RFC_ERR_UNSUPPORTED_SAMPLE. */
        uint8_t *mut = (uint8_t *)malloc(valid.len);
        CHECK(mut != NULL);
        memcpy(mut, valid.data, valid.len);
        const uint8_t *cs_ptr = NULL;
        size_t cs_len = 0u;
        CHECK(find_jp2_box(mut, valid.len, "jp2c", &cs_ptr, &cs_len));
        size_t cs_off = (size_t)(cs_ptr - mut);
        mut[cs_off + 42u] |= 0x80u;
        CHECK(adp->decode_session(sd, &exec, mut, valid.len, NULL, 0u, &dec,
                                  &dc, &dm) == RFC_ERR_UNSUPPORTED_SAMPLE);
        CHECK(st.alloc_count == ac0);

        /* Subsampled XRsiz = 2 -> RFC_ERR_UNSUPPORTED_SAMPLE. */
        memcpy(mut, valid.data, valid.len);
        mut[cs_off + 43u] = 2u;
        CHECK(adp->decode_session(sd, &exec, mut, valid.len, NULL, 0u, &dec,
                                  &dc, &dm) == RFC_ERR_UNSUPPORTED_SAMPLE);
        CHECK(st.alloc_count == ac0);

        /* Tier-2 (Plan #3): valid framing, truncated tile/packet payload ->
         * fails within budget, releases reservation to 0, allocates no frame. */
        memcpy(mut, valid.data, valid.len);
        /* Truncate mid-tile after SOT so header passes but decode fails. */
        size_t trunc_len = valid.len - 6u;
        /* Update jp2c box length to match truncation. */
        if (cs_off >= 8u) {
            put_be32(mut + cs_off - 8u, (uint32_t)(trunc_len - (cs_off - 8u)));
        }
        rfc_status tst = adp->decode_session(sd, &exec, mut, trunc_len, NULL,
                                             0u, &dec, &dc, &dm);
        CHECK(tst != RFC_OK && dec == NULL && dc == 0u);
        rfc_budget_stats bsn;
        rfc_budget_stats_get(bud, &bsn);
        CHECK(bsn.current_allocated_bytes == 0u);

        free(mut);
        adp->close_session(sd);
    }

    /* Strip metadata policy drops ICC on decode. */
    {
        static const uint8_t k_icc[20] = {'a', 'c', 's', 'p', 1, 2, 3, 4, 5, 6,
                                          7,   8,   9,   10,  11, 12, 13, 14, 15, 16};
        rfc_metadata mi;
        memset(&mi, 0, sizeof(mi));
        mi.icc = (uint8_t *)k_icc;
        mi.icc_len = sizeof(k_icc);
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded e_icc;
        CHECK(adp->encode_session(se, &exec, &src, 1u, &mi, NULL, 0u, &e_icc) ==
              RFC_OK);
        adp->close_session(se);

        void *sd = NULL;
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        rfc_setting_value strip_sv[1] = {{"metadata", "strip"}};
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(sd, &exec, e_icc.data, e_icc.len, strip_sv,
                                  1u, &dec, &dc, &dm) == RFC_OK);
        adp->close_session(sd);
        CHECK(dc == 1u && dm.icc == NULL && dm.icc_len == 0u);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        rfc_encoded_free(&e_icc);
    }

    rfc_encoded_free(&valid);
    rfc_frame_free(&src);
    rfc_budget_destroy(bud);
    CHECK(st.live_bytes == 0u);
    printf("jp2: refusals_malformed_and_metadata_policy ok\n");
}

/* ---- Test 4: Jobs, budget, pre-decode premul alpha, threads, cancel, file safety ---- */

static struct {
    uint32_t decode_calls;
} g_jp2_premul_mock;

static rfc_status pm_open(const rfc_exec_context *exec, void **out_s) {
    (void)exec;
    *out_s = &g_jp2_premul_mock;
    return RFC_OK;
}
static void pm_close(void *s) { (void)s; }
static void pm_cancel(void *s) { (void)s; }
static rfc_status pm_probe(void *s,
                           const rfc_exec_context *exec,
                           const uint8_t *bytes,
                           size_t size,
                           rfc_image_probe *out_probe) {
    (void)s; (void)exec; (void)bytes; (void)size;
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGBA8;
    out_probe->first_frame.alpha = RFC_ALPHA_PREMULTIPLIED;
    out_probe->first_frame.width = 4u;
    out_probe->first_frame.height = 4u;
    out_probe->first_frame.stride_bytes = 16u;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    return RFC_OK;
}
static rfc_status pm_decode(void *s,
                            const rfc_exec_context *exec,
                            const uint8_t *bytes,
                            size_t size,
                            const rfc_setting_value *settings,
                            size_t setting_count,
                            rfc_frame **out_frames,
                            size_t *out_frame_count,
                            rfc_metadata *out_meta) {
    (void)s; (void)exec; (void)bytes; (void)size; (void)settings;
    (void)setting_count; (void)out_frames; (void)out_frame_count;
    (void)out_meta;
    g_jp2_premul_mock.decode_calls++;
    return RFC_ERR_TRUNCATED_INPUT;
}
static rfc_status pm_encode(void *s,
                            const rfc_exec_context *exec,
                            const rfc_frame *frames,
                            size_t frame_count,
                            const rfc_metadata *meta,
                            const rfc_setting_value *settings,
                            size_t setting_count,
                            rfc_encoded *out_encoded) {
    (void)s; (void)exec; (void)frames; (void)frame_count; (void)meta;
    (void)settings; (void)setting_count; (void)out_encoded;
    return RFC_ERR_UNSUPPORTED_COMBINATION;
}

static _Atomic(rfc_job *) g_cancel_job = NULL;
static _Atomic(int) g_cancel_armed = 0;
static void cancel_hook_fn(int phase) {
    if (phase == 1) {
        while (atomic_load_explicit(&g_cancel_armed, memory_order_acquire) == 0) {
            jp2_test_yield();
        }
        rfc_job *j = atomic_load_explicit(&g_cancel_job, memory_order_acquire);
        if (j) {
            rfc_job_cancel(j);
        }
    }
}

static _Atomic(uint32_t) g_cli_provider_calls = 0;
static _Atomic(uint32_t) g_cli_frame_alloc_calls = 0;
static _Atomic(int) g_obs_threads_dec_ctor = -1;
static _Atomic(int) g_obs_threads_enc_ctor = -1;
static void cli_obs_hook_fn(int phase) {
    if (phase == 1) {
        atomic_fetch_add_explicit(&g_cli_provider_calls, 1u,
                                  memory_order_relaxed);
    } else if (phase == 3) {
        atomic_fetch_add_explicit(&g_cli_frame_alloc_calls, 1u,
                                  memory_order_relaxed);
    } else if (phase == 4) {
        atomic_store_explicit(&g_obs_threads_dec_ctor,
                              get_process_thread_count(),
                              memory_order_release);
    } else if (phase == 5) {
        atomic_store_explicit(&g_obs_threads_enc_ctor,
                              get_process_thread_count(),
                              memory_order_release);
    }
}

static void test_review_round1_regressions(void) {
    const rfc_session_adapter *adp =
        rfc_registry_session_adapter(RFC_FORMAT_JPEG2000);
    CHECK(adp != NULL);
    tracked_alloc st;
    rfc_allocator al;
    rfc_budget *bud = rfc_budget_create(64u * 1024u * 1024u);
    CHECK(bud != NULL);
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, bud);

    /* 1. Odd padded stride on 16-bit RGB16 (alignment-safe, UB-free). */
    {
        uint32_t w = 5u, h = 4u;
        size_t odd_stride = (size_t)w * 6u + 3u; /* odd byte stride */
        uint8_t *buf = (uint8_t *)malloc(odd_stride * h);
        CHECK(buf != NULL);
        memset(buf, 0xCC, odd_stride * h);
        for (uint32_t y = 0u; y < h; y++) {
            uint8_t *row = buf + (size_t)y * odd_stride;
            for (uint32_t x = 0u; x < w * 3u; x++) {
                uint16_t v = (uint16_t)(((y * 37u + x * 113u) & 0x0FFFu) << 4);
                memcpy(row + (size_t)x * 2u, &v, 2u);
            }
        }
        rfc_frame fr;
        memset(&fr, 0, sizeof(fr));
        fr.layout.format = RFC_SAMPLE_RGB16;
        fr.layout.alpha = RFC_ALPHA_NONE;
        fr.layout.width = w;
        fr.layout.height = h;
        fr.layout.stride_bytes = odd_stride;
        fr.layout.bits_per_sample = 12u;
        fr.data = buf;
        fr.data_len = odd_stride * h;
        void *se = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded enc;
        CHECK(adp->encode_session(se, &exec, &fr, 1u, NULL, NULL, 0u, &enc) ==
              RFC_OK);
        adp->close_session(se);
        void *sd = NULL;
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(sd, &exec, enc.data, enc.len, NULL, 0u, &dec,
                                  &dc, &dm) == RFC_OK);
        adp->close_session(sd);
        CHECK(dc == 1u && dec[0].layout.bits_per_sample == 12u);
        for (uint32_t y = 0u; y < h; y++) {
            CHECK(memcmp(dec[0].data + (size_t)y * dec[0].layout.stride_bytes,
                         buf + (size_t)y * odd_stride, (size_t)w * 6u) == 0);
        }
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        rfc_encoded_free(&enc);
        free(buf);
    }

    /* 2. Direct session encode: unsupported metadata (XMP) refused under preserve,
     * oversized ICC refused under max_metadata_bytes. */
    {
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 4u, 4u, 12u, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        memset(src.data, 0x33, src.data_len);
        static const uint8_t k_xmp[] = "<x:xmpmeta/>";
        rfc_metadata mx;
        memset(&mx, 0, sizeof(mx));
        mx.xmp = k_xmp;
        mx.xmp_len = sizeof(k_xmp) - 1u;
        void *se = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded enc;
        CHECK(adp->encode_session(se, &exec, &src, 1u, &mx, NULL, 0u, &enc) ==
              RFC_ERR_METADATA_POLICY);
        /* Oversized ICC refused against max_metadata_bytes. */
        rfc_exec_context lim_exec = exec;
        lim_exec.limits.max_metadata_bytes = 16u;
        static const uint8_t k_big_icc[32] = {1};
        rfc_metadata mi;
        memset(&mi, 0, sizeof(mi));
        mi.icc = k_big_icc;
        mi.icc_len = sizeof(k_big_icc);
        CHECK(adp->encode_session(se, &lim_exec, &src, 1u, &mi, NULL, 0u,
                                  &enc) == RFC_ERR_LIMIT_EXCEEDED);
        adp->close_session(se);
        rfc_frame_free(&src);
    }

    /* 3. Decode metadata=strip enforces max_metadata_bytes before provider work,
     * 4-ch JP2 without straight-alpha cdef is refused, second jp2c is refused,
     * unknown UUID refused under preserve / ignored under strip,
     * and ihdr vs SIZ precision mismatch is refused. */
    {
        rfc_layout lay = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 6u, 5u, 24u, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        memset(src.data, 0x77, src.data_len);
        uint8_t icc32[32];
        memset(icc32, 0x41, sizeof(icc32));
        rfc_metadata mi;
        memset(&mi, 0, sizeof(mi));
        mi.icc = icc32;
        mi.icc_len = sizeof(icc32);
        void *se = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded enc;
        CHECK(adp->encode_session(se, &exec, &src, 1u, &mi, NULL, 0u, &enc) ==
              RFC_OK);
        adp->close_session(se);

        /* 3a. metadata=strip still refuses oversized ICC before provider entry. */
        rfc_exec_context lim_exec = exec;
        lim_exec.limits.max_metadata_bytes = 16u;
        rfc_setting_value strip_sv[1] = {{"metadata", "strip"}};
        atomic_store(&g_cli_provider_calls, 0u);
        rfc_codecs_jp2_set_hook_for_testing(cli_obs_hook_fn);
        void *sd = NULL;
        CHECK(adp->open_session(&lim_exec, &sd) == RFC_OK);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(sd, &lim_exec, enc.data, enc.len, strip_sv,
                                  1u, &dec, &dc, &dm) == RFC_ERR_LIMIT_EXCEEDED);
        CHECK(atomic_load(&g_cli_provider_calls) == 0u);
        adp->close_session(sd);
        rfc_codecs_jp2_set_hook_for_testing(NULL);

        /* 3b. 4-ch JP2 with cdef typ=65535 (unspecified auxiliary) refused. */
        uint8_t *mut = (uint8_t *)malloc(enc.len + 64u);
        CHECK(mut != NULL);
        memcpy(mut, enc.data, enc.len);
        const uint8_t *cdef_p = NULL;
        size_t cdef_l = 0u;
        const uint8_t *jp2h_p = NULL;
        size_t jp2h_l = 0u;
        CHECK(find_jp2_box(mut, enc.len, "jp2h", &jp2h_p, &jp2h_l));
        CHECK(find_sub_box(jp2h_p, jp2h_l, "cdef", &cdef_p, &cdef_l));
        size_t cdef_off = (size_t)(cdef_p - mut);
        /* Overwrite channel 3 typ=1 -> typ=0xFFFF (unspecified). */
        mut[cdef_off + 2u + 3u * 6u + 2u] = 0xFFu;
        mut[cdef_off + 2u + 3u * 6u + 3u] = 0xFFu;
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, mut, enc.len, NULL, 0u, &dec, &dc,
                                  &dm) == RFC_ERR_UNSUPPORTED_COMBINATION);
        adp->close_session(sd);

        /* 3c. ihdr bpc mismatch vs SIZ precision (ihdr says 16-bit, SIZ says 8-bit). */
        memcpy(mut, enc.data, enc.len);
        const uint8_t *ihdr_p = NULL;
        size_t ihdr_l = 0u;
        CHECK(find_jp2_box(mut, enc.len, "jp2h", &jp2h_p, &jp2h_l));
        CHECK(find_sub_box(jp2h_p, jp2h_l, "ihdr", &ihdr_p, &ihdr_l));
        mut[(size_t)(ihdr_p - mut) + 10u] = 15u; /* 16-bit in ihdr */
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, mut, enc.len, NULL, 0u, &dec, &dc,
                                  &dm) == RFC_ERR_INVALID_ARGUMENT);
        adp->close_session(sd);

        /* 3d. Second jp2c box appended -> refused with UNSUPPORTED_COMBINATION. */
        memcpy(mut, enc.data, enc.len);
        static const uint8_t k_dup_jp2c[12] = {0, 0, 0, 12, 'j', 'p', '2', 'c',
                                               0xFF, 0x4F, 0xFF, 0xD9};
        memcpy(mut + enc.len, k_dup_jp2c, sizeof(k_dup_jp2c));
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, mut, enc.len + sizeof(k_dup_jp2c),
                                  NULL, 0u, &dec, &dc, &dm) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        adp->close_session(sd);

        /* 3e. Unknown UUID box: refused under preserve, ignored under strip. */
        memcpy(mut, enc.data, enc.len);
        static const uint8_t k_unk_uuid[28] = {
            0, 0, 0, 28, 'u', 'u', 'i', 'd',
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
            'T', 'E', 'S', 'T'
        };
        memcpy(mut + enc.len, k_unk_uuid, sizeof(k_unk_uuid));
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, mut, enc.len + sizeof(k_unk_uuid),
                                  NULL, 0u, &dec, &dc, &dm) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(adp->decode_session(sd, &exec, mut, enc.len + sizeof(k_unk_uuid),
                                  strip_sv, 1u, &dec, &dc, &dm) == RFC_OK);
        CHECK(dc == 1u && dm.exif == NULL && dm.xmp == NULL && dm.icc == NULL);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        adp->close_session(sd);

        free(mut);
        rfc_encoded_free(&enc);
        rfc_frame_free(&src);
    }

    /* 4. Completion-review regressions:
     * (a) 1x1 + 512 KiB ICC exceeds a 400 KiB budget on BOTH encode and decode
     *     (strip and preserve) while 1x1 without ICC fits;
     * (b) sRGB-first + ICC-second colr boxes: first colr wins (no ICC returned);
     * (c) incomplete 1-entry cdef on 4-ch JP2 refused before provider work;
     * (d) redundant bpcc when ihdr bpc != 0xFF refused;
     * (e) non-XMP xml box and duplicate XMP refused under preserve, ok under strip;
     * (f) 1-thread vs 3-thread decode/encode produce identical pixels, and a
     *     long (>64B) OPJ_NUM_THREADS value is preserved verbatim. */
    {
        /* 4a. ICC budget reservation */
        rfc_layout l1 = {RFC_SAMPLE_GRAY8, RFC_ALPHA_NONE, 1u, 1u, 1u, 8u};
        rfc_frame f1;
        CHECK(rfc_frame_alloc(&f1, &l1, NULL, &al) == RFC_OK);
        f1.data[0] = 0x5Au;
        size_t icc_sz = 512u * 1024u;
        uint8_t *big_icc = (uint8_t *)calloc(1u, icc_sz);
        CHECK(big_icc != NULL);
        put_be32(big_icc, (uint32_t)icc_sz);
        memcpy(big_icc + 12u, "mntrGRAYXYZ ", 12u);
        memcpy(big_icc + 36u, "acsp", 4u);
        rfc_metadata mi;
        memset(&mi, 0, sizeof(mi));
        mi.icc = big_icc;
        mi.icc_len = icc_sz;

        void *se = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded e_noicc, e_icc;
        CHECK(adp->encode_session(se, &exec, &f1, 1u, NULL, NULL, 0u,
                                  &e_noicc) == RFC_OK);
        CHECK(adp->encode_session(se, &exec, &f1, 1u, &mi, NULL, 0u, &e_icc) ==
              RFC_OK);

        rfc_budget *b400 = rfc_budget_create(400u * 1024u);
        CHECK(b400 != NULL);
        rfc_exec_context e400 = exec;
        e400.budget = b400;
        rfc_encoded tmp_e;
        /* Without ICC fits 400 KiB budget; with 512 KiB ICC fails OOM! */
        CHECK(adp->encode_session(se, &e400, &f1, 1u, NULL, NULL, 0u, &tmp_e) ==
              RFC_OK);
        rfc_encoded_free(&tmp_e);
        CHECK(adp->encode_session(se, &e400, &f1, 1u, &mi, NULL, 0u, &tmp_e) ==
              RFC_ERR_OUT_OF_MEMORY);
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        rfc_setting_value strip_sv[1] = {{"metadata", "strip"}};
        CHECK(adp->decode_session(se, &e400, e_icc.data, e_icc.len, NULL, 0u,
                                  &dec, &dc, &dm) == RFC_ERR_OUT_OF_MEMORY);
        CHECK(adp->decode_session(se, &e400, e_icc.data, e_icc.len, strip_sv,
                                  1u, &dec, &dc, &dm) == RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_stats bs400;
        rfc_budget_stats_get(b400, &bs400);
        CHECK(bs400.current_allocated_bytes == 0u);
        adp->close_session(se);
        rfc_budget_destroy(b400);
        rfc_encoded_free(&e_noicc);
        rfc_encoded_free(&e_icc);
        rfc_frame_free(&f1);
        free(big_icc);

        /* 4b..4e: Build a 4-ch RGBA8 JP2 without ICC (has enumcs=16 + 4-entry cdef). */
        rfc_layout l4 = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 4u, 4u, 16u, 8u};
        rfc_frame f4;
        CHECK(rfc_frame_alloc(&f4, &l4, NULL, &al) == RFC_OK);
        for (size_t i = 0u; i < f4.data_len; i++) {
            f4.data[i] = (uint8_t)((i * 19u + 7u) & 0xFFu);
        }
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded e4;
        CHECK(adp->encode_session(se, &exec, &f4, 1u, NULL, NULL, 0u, &e4) ==
              RFC_OK);
        adp->close_session(se);

        const uint8_t *jp2h_p = NULL;
        size_t jp2h_l = 0u;
        CHECK(find_jp2_box(e4.data, e4.len, "jp2h", &jp2h_p, &jp2h_l));
        size_t jp2h_off = (size_t)(jp2h_p - e4.data) - 8u;
        size_t jp2h_end = jp2h_off + 8u + jp2h_l;

        /* 4b. Append a second colr(meth=2, ICC) inside jp2h after sRGB colr(meth=1). */
        static const uint8_t k_colr2[19] = {
            0, 0, 0, 19, 'c', 'o', 'l', 'r', 2, 0, 0, 'F', 'A', 'K', 'E', 'I',
            'C', 'C', '!'
        };
        uint8_t *m2 = (uint8_t *)malloc(e4.len + 128u);
        CHECK(m2 != NULL);
        memcpy(m2, e4.data, jp2h_end);
        put_be32(m2 + jp2h_off, (uint32_t)(8u + jp2h_l + sizeof(k_colr2)));
        memcpy(m2 + jp2h_end, k_colr2, sizeof(k_colr2));
        memcpy(m2 + jp2h_end + sizeof(k_colr2), e4.data + jp2h_end,
               e4.len - jp2h_end);
        void *sd = NULL;
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, m2, e4.len + sizeof(k_colr2), NULL,
                                  0u, &dec, &dc, &dm) == RFC_OK);
        CHECK(dc == 1u && dm.icc == NULL && dm.icc_len == 0u);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        adp->close_session(sd);

        /* 4c. Incomplete 1-entry cdef on 4-ch JP2 refused before provider work. */
        const uint8_t *cdef_p = NULL;
        size_t cdef_l = 0u;
        CHECK(find_sub_box(jp2h_p, jp2h_l, "cdef", &cdef_p, &cdef_l));
        size_t cdef_box_off = (size_t)(cdef_p - e4.data) - 8u;
        size_t cdef_box_end = cdef_box_off + 8u + cdef_l;
        static const uint8_t k_cdef1[16] = {
            0, 0, 0, 16, 'c', 'd', 'e', 'f',
            0, 1, /* N = 1 (missing channels 0..2) */
            0, 3, 0, 1, 0, 0
        };
        size_t shrink = (8u + cdef_l) - sizeof(k_cdef1);
        memcpy(m2, e4.data, cdef_box_off);
        memcpy(m2 + cdef_box_off, k_cdef1, sizeof(k_cdef1));
        memcpy(m2 + cdef_box_off + sizeof(k_cdef1), e4.data + cdef_box_end,
               e4.len - cdef_box_end);
        put_be32(m2 + jp2h_off, (uint32_t)(8u + jp2h_l - shrink));
        atomic_store(&g_cli_provider_calls, 0u);
        rfc_codecs_jp2_set_hook_for_testing(cli_obs_hook_fn);
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, m2, e4.len - shrink, NULL, 0u,
                                  &dec, &dc, &dm) ==
              RFC_ERR_UNSUPPORTED_COMBINATION);
        CHECK(atomic_load(&g_cli_provider_calls) == 0u);
        adp->close_session(sd);
        rfc_codecs_jp2_set_hook_for_testing(NULL);

        /* 4d. Redundant bpcc when ihdr bpc != 0xFF refused with INVALID_ARGUMENT. */
        static const uint8_t k_bpcc[12] = {0, 0, 0, 12, 'b', 'p', 'c', 'c',
                                           7, 7, 7, 7};
        memcpy(m2, e4.data, jp2h_end);
        put_be32(m2 + jp2h_off, (uint32_t)(8u + jp2h_l + sizeof(k_bpcc)));
        memcpy(m2 + jp2h_end, k_bpcc, sizeof(k_bpcc));
        memcpy(m2 + jp2h_end + sizeof(k_bpcc), e4.data + jp2h_end,
               e4.len - jp2h_end);
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, m2, e4.len + sizeof(k_bpcc), NULL,
                                  0u, &dec, &dc, &dm) == RFC_ERR_INVALID_ARGUMENT);
        adp->close_session(sd);

        /* 4e. Non-XMP xml box (including comment-spoofed <x:xmpmeta) refused
         * under preserve, ignored under strip. */
        static const uint8_t k_nonxmp_xml[20] = {
            0, 0, 0, 20, 'x', 'm', 'l', ' ',
            '<', 'g', 'm', 'l', '>', '1', '<', '/', 'g', 'm', 'l', '>'
        };
        memcpy(m2, e4.data, e4.len);
        memcpy(m2 + e4.len, k_nonxmp_xml, sizeof(k_nonxmp_xml));
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, m2, e4.len + sizeof(k_nonxmp_xml),
                                  NULL, 0u, &dec, &dc, &dm) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(adp->decode_session(sd, &exec, m2, e4.len + sizeof(k_nonxmp_xml),
                                  strip_sv, 1u, &dec, &dc, &dm) == RFC_OK);
        CHECK(dc == 1u && dm.xmp == NULL);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        adp->close_session(sd);

        static const uint8_t k_spoof_xml[40] = {
            0, 0, 0, 40, 'x', 'm', 'l', ' ',
            '<', '!', '-', '-', ' ', '<', 'x', ':', 'x', 'm', 'p', 'm', 'e',
            't', 'a', ' ', '-', '-', '>', '<', 'g', 'm', 'l', '>', '1', '<',
            '/', 'g', 'm', 'l', '>', ' '
        };
        memcpy(m2, e4.data, e4.len);
        memcpy(m2 + e4.len, k_spoof_xml, sizeof(k_spoof_xml));
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, m2, e4.len + sizeof(k_spoof_xml),
                                  NULL, 0u, &dec, &dc, &dm) ==
              RFC_ERR_METADATA_POLICY);
        CHECK(adp->decode_session(sd, &exec, m2, e4.len + sizeof(k_spoof_xml),
                                  strip_sv, 1u, &dec, &dc, &dm) == RFC_OK);
        CHECK(dc == 1u && dm.xmp == NULL);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        adp->close_session(sd);

        /* Prefix-spoofed <x:xmpmetadata xmlns:x="urn:unrelated"/> refused under preserve. */
        {
            const char *fake_xmp = "<x:xmpmetadata xmlns:x=\"urn:unrelated\"/>";
            size_t flen = strlen(fake_xmp);
            memcpy(m2, e4.data, e4.len);
            put_be32(m2 + e4.len, (uint32_t)(8u + flen));
            memcpy(m2 + e4.len + 4u, "xml ", 4u);
            memcpy(m2 + e4.len + 8u, fake_xmp, flen);
            CHECK(adp->open_session(&exec, &sd) == RFC_OK);
            CHECK(adp->decode_session(sd, &exec, m2, e4.len + 8u + flen, NULL,
                                      0u, &dec, &dc, &dm) ==
                  RFC_ERR_METADATA_POLICY);
            CHECK(adp->decode_session(sd, &exec, m2, e4.len + 8u + flen,
                                      strip_sv, 1u, &dec, &dc, &dm) == RFC_OK);
            CHECK(dc == 1u && dm.xmp == NULL);
            rfc_metadata_free(&dm, &al);
            rfc_frame_free(&dec[0]);
            al.free(al.ctx, dec);
            adp->close_session(sd);
        }

        /* Attribute-value spoofed namespace refused under preserve, ignored under strip. */
        {
            const char *attr_spoof =
                "<x:xmpmeta xmlns:x=\"urn:unrelated\" note='xmlns:x=\"adobe:ns:meta/\"'/>";
            size_t alen = strlen(attr_spoof);
            uint8_t *ma = (uint8_t *)malloc(e4.len + 8u + alen);
            CHECK(ma != NULL);
            memcpy(ma, e4.data, e4.len);
            put_be32(ma + e4.len, (uint32_t)(8u + alen));
            memcpy(ma + e4.len + 4u, "xml ", 4u);
            memcpy(ma + e4.len + 8u, attr_spoof, alen);
            CHECK(adp->open_session(&exec, &sd) == RFC_OK);
            CHECK(adp->decode_session(sd, &exec, ma, e4.len + 8u + alen, NULL,
                                      0u, &dec, &dc, &dm) ==
                  RFC_ERR_METADATA_POLICY);
            CHECK(adp->decode_session(sd, &exec, ma, e4.len + 8u + alen,
                                      strip_sv, 1u, &dec, &dc, &dm) == RFC_OK);
            CHECK(dc == 1u && dm.xmp == NULL);
            rfc_metadata_free(&dm, &al);
            rfc_frame_free(&dec[0]);
            al.free(al.ctx, dec);
            adp->close_session(sd);
            free(ma);
        }

        /* Valid XMP preceded by 20 XML comments + whitespace around '=' preserved verbatim,
         * plus alternate namespace prefix (m:xmpmeta), default namespace (xmpmeta),
         * numeric character references in namespace URI (&#x2F; / &#47;), and refusals for
         * mismatched prefix, invalid NCName prefix (<1:xmpmeta), reserved prefix, and
         * malformed attribute value. */
        {
            const char *good_cases[] = {
                "<!-- c01 --><!-- c02 --><!-- c03 --><!-- c04 --><!-- c05 -->"
                "<!-- c06 --><!-- c07 --><!-- c08 --><!-- c09 --><!-- c10 -->"
                "<!-- c11 --><!-- c12 --><!-- c13 --><!-- c14 --><!-- c15 -->"
                "<!-- c16 --><!-- c17 --><!-- c18 --><!-- c19 --><!-- c20 -->"
                "<x:xmpmeta xmlns:x = \"adobe:ns:meta/\">"
                "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></x:xmpmeta>",
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta/\">"
                "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></m:xmpmeta>",
                "<xmpmeta xmlns=\"adobe:ns:meta/\">"
                "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></xmpmeta>",
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta&#x2F;\">"
                "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></m:xmpmeta>",
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta&#47;\">"
                "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></m:xmpmeta>",
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<?xml-stylesheet type=\"text/xsl\" href=\"local.xsl\"?>"
                "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta/\">"
                "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></m:xmpmeta>"
                "<?xpacket end=\"w\"?>"
            };
            for (size_t gi = 0u; gi < 6u; gi++) {
                const char *good_xmp = good_cases[gi];
                size_t glen = strlen(good_xmp);
                uint8_t *mg = (uint8_t *)malloc(e4.len + 8u + glen);
                CHECK(mg != NULL);
                memcpy(mg, e4.data, e4.len);
                put_be32(mg + e4.len, (uint32_t)(8u + glen));
                memcpy(mg + e4.len + 4u, "xml ", 4u);
                memcpy(mg + e4.len + 8u, good_xmp, glen);
                CHECK(adp->open_session(&exec, &sd) == RFC_OK);
                CHECK(adp->decode_session(sd, &exec, mg, e4.len + 8u + glen, NULL,
                                          0u, &dec, &dc, &dm) == RFC_OK);
                CHECK(dc == 1u && dm.xmp != NULL && dm.xmp_len == glen &&
                      memcmp(dm.xmp, good_xmp, glen) == 0);
                rfc_metadata_free(&dm, &al);
                rfc_frame_free(&dec[0]);
                al.free(al.ctx, dec);
                adp->close_session(sd);
                free(mg);
            }
            /* Deeply nested valid XMP (>64 levels) preserved verbatim (Finding 20). */
            {
                char deep[4096];
                size_t pos = 0u;
                pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos,
                                        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">");
                for (int d = 0; d < 70; d++) {
                    pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "<n%d>", d);
                }
                for (int d = 69; d >= 0; d--) {
                    pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "</n%d>", d);
                }
                pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "</x:xmpmeta>");
                uint8_t *md = (uint8_t *)malloc(e4.len + 8u + pos);
                CHECK(md != NULL);
                memcpy(md, e4.data, e4.len);
                put_be32(md + e4.len, (uint32_t)(8u + pos));
                memcpy(md + e4.len + 4u, "xml ", 4u);
                memcpy(md + e4.len + 8u, deep, pos);
                CHECK(adp->open_session(&exec, &sd) == RFC_OK);
                CHECK(adp->decode_session(sd, &exec, md, e4.len + 8u + pos, NULL,
                                          0u, &dec, &dc, &dm) == RFC_OK);
                CHECK(dc == 1u && dm.xmp != NULL && dm.xmp_len == pos &&
                      memcmp(dm.xmp, deep, pos) == 0);
                rfc_metadata_free(&dm, &al);
                rfc_frame_free(&dec[0]);
                al.free(al.ctx, dec);
                adp->close_session(sd);
                free(md);
            }
            /* >256 distinct attributes accepted; duplicate attribute >256 refused (Finding 22). */
            {
                char many[8192];
                size_t pos = 0u;
                pos += (size_t)snprintf(many + pos, sizeof(many) - pos,
                                        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><c");
                for (int a = 0; a < 260; a++) {
                    pos += (size_t)snprintf(many + pos, sizeof(many) - pos,
                                            " a%d=\"1\"", a);
                }
                size_t base_pos = pos;
                pos += (size_t)snprintf(many + pos, sizeof(many) - pos, "/></x:xmpmeta>");
                uint8_t *mm = (uint8_t *)malloc(e4.len + 8u + pos);
                CHECK(mm != NULL);
                memcpy(mm, e4.data, e4.len);
                put_be32(mm + e4.len, (uint32_t)(8u + pos));
                memcpy(mm + e4.len + 4u, "xml ", 4u);
                memcpy(mm + e4.len + 8u, many, pos);
                CHECK(adp->open_session(&exec, &sd) == RFC_OK);
                CHECK(adp->decode_session(sd, &exec, mm, e4.len + 8u + pos, NULL,
                                          0u, &dec, &dc, &dm) == RFC_OK);
                CHECK(dc == 1u && dm.xmp != NULL && dm.xmp_len == pos);
                rfc_metadata_free(&dm, &al);
                rfc_frame_free(&dec[0]);
                al.free(al.ctx, dec);
                adp->close_session(sd);
                free(mm);

                size_t dpos = base_pos;
                dpos += (size_t)snprintf(many + dpos, sizeof(many) - dpos,
                                         " a259=\"dup\"/></x:xmpmeta>");
                uint8_t *mdup = (uint8_t *)malloc(e4.len + 8u + dpos);
                CHECK(mdup != NULL);
                memcpy(mdup, e4.data, e4.len);
                put_be32(mdup + e4.len, (uint32_t)(8u + dpos));
                memcpy(mdup + e4.len + 4u, "xml ", 4u);
                memcpy(mdup + e4.len + 8u, many, dpos);
                CHECK(adp->open_session(&exec, &sd) == RFC_OK);
                CHECK(adp->decode_session(sd, &exec, mdup, e4.len + 8u + dpos, NULL,
                                          0u, &dec, &dc, &dm) == RFC_ERR_METADATA_POLICY);
                adp->close_session(sd);
                free(mdup);
            }
            /* Invalid/mismatched/unbound/truncated/bad-XMLDecl boxes refuse under preserve. */
            const char *bad_cases[] = {
                "<m:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF/></m:xmpmeta>",
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta/\"><rdf:RDF/></m:xmpmeta>",
                "<1:xmpmeta xmlns:1=\"adobe:ns:meta/\"/>",
                "<xml:xmpmeta xmlns:xml=\"adobe:ns:meta/\"/>",
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta&#x2F;\" bad=\"a<b\"/>",
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta&#47;\">",
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta/\"></x:xmpmeta>",
                "<m:xmpmeta xmlns:m=\"adobe:ns:meta/\"/>trailing",
                "<?xml garbage?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>",
                "<?xml version=\"2.0\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>",
                "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" xmlns:a=\"urn:u\" xmlns:b=\"urn:u\" a:k=\"1\" b:k=\"2\"/>"
            };
            for (size_t bi = 0u; bi < 11u; bi++) {
                const char *bad_xmp = bad_cases[bi];
                size_t blen = strlen(bad_xmp);
                uint8_t *mb = (uint8_t *)malloc(e4.len + 8u + blen);
                CHECK(mb != NULL);
                memcpy(mb, e4.data, e4.len);
                put_be32(mb + e4.len, (uint32_t)(8u + blen));
                memcpy(mb + e4.len + 4u, "xml ", 4u);
                memcpy(mb + e4.len + 8u, bad_xmp, blen);
                CHECK(adp->open_session(&exec, &sd) == RFC_OK);
                CHECK(adp->decode_session(sd, &exec, mb, e4.len + 8u + blen, NULL,
                                          0u, &dec, &dc, &dm) == RFC_ERR_METADATA_POLICY);
                adp->close_session(sd);
                free(mb);
            }
        }

        /* 4g. meth=3 colr before meth=1 sRGB: meth=3 ignored, sRGB wins (no ICC). */
        const uint8_t *colr1_p = NULL;
        size_t colr1_l = 0u;
        CHECK(find_sub_box(jp2h_p, jp2h_l, "colr", &colr1_p, &colr1_l));
        size_t colr1_box_off = (size_t)(colr1_p - e4.data) - 8u;
        static const uint8_t k_colr3[19] = {
            0, 0, 0, 19, 'c', 'o', 'l', 'r', 3, 0, 0, 'M', '3', 'I', 'G', 'N',
            'O', 'R', 'E'
        };
        memcpy(m2, e4.data, colr1_box_off);
        memcpy(m2 + colr1_box_off, k_colr3, sizeof(k_colr3));
        memcpy(m2 + colr1_box_off + sizeof(k_colr3), e4.data + colr1_box_off,
               e4.len - colr1_box_off);
        put_be32(m2 + jp2h_off, (uint32_t)(8u + jp2h_l + sizeof(k_colr3)));
        CHECK(adp->open_session(&exec, &sd) == RFC_OK);
        CHECK(adp->decode_session(sd, &exec, m2, e4.len + sizeof(k_colr3), NULL,
                                  0u, &dec, &dc, &dm) == RFC_OK);
        CHECK(dc == 1u && dm.icc == NULL && dm.icc_len == 0u);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        adp->close_session(sd);

        /* 4h. sRGB-first + 512 KiB second colr inside jp2h exceeds 400 KiB budget. */
        {
            size_t extra_sz = 512u * 1024u;
            uint8_t *m_big = (uint8_t *)calloc(1u, e4.len + extra_sz + 16u);
            CHECK(m_big != NULL);
            memcpy(m_big, e4.data, jp2h_end);
            put_be32(m_big + jp2h_off,
                     (uint32_t)(8u + jp2h_l + 11u + extra_sz));
            put_be32(m_big + jp2h_end, (uint32_t)(11u + extra_sz));
            memcpy(m_big + jp2h_end + 4u, "colr", 4u);
            m_big[jp2h_end + 8u] = 2u;
            memcpy(m_big + jp2h_end + 11u + extra_sz, e4.data + jp2h_end,
                   e4.len - jp2h_end);
            rfc_budget *b400b = rfc_budget_create(400u * 1024u);
            CHECK(b400b != NULL);
            rfc_exec_context e400b = exec;
            e400b.budget = b400b;
            CHECK(adp->open_session(&e400b, &sd) == RFC_OK);
            CHECK(adp->decode_session(sd, &e400b, m_big,
                                      e4.len + 11u + extra_sz, NULL, 0u, &dec,
                                      &dc, &dm) == RFC_ERR_OUT_OF_MEMORY);
            CHECK(adp->decode_session(sd, &e400b, m_big,
                                      e4.len + 11u + extra_sz, strip_sv, 1u,
                                      &dec, &dc, &dm) == RFC_ERR_OUT_OF_MEMORY);
            adp->close_session(sd);
            rfc_budget_destroy(b400b);
            free(m_big);
        }

        /* 4i. ihdr_nc=33 + cdef(cn=32, typ=0, asoc=33) refused before shift (no UB). */
        {
            memcpy(m2, e4.data, e4.len);
            const uint8_t *ihp = NULL;
            size_t ihl = 0u;
            CHECK(find_sub_box(jp2h_p, jp2h_l, "ihdr", &ihp, &ihl));
            size_t ih_off = (size_t)(ihp - e4.data);
            m2[ih_off + 8u] = 0u;
            m2[ih_off + 9u] = 33u; /* nc = 33 */
            CHECK(adp->open_session(&exec, &sd) == RFC_OK);
            CHECK(adp->decode_session(sd, &exec, m2, e4.len, NULL, 0u, &dec,
                                      &dc, &dm) == RFC_ERR_UNSUPPORTED_SAMPLE);
            adp->close_session(sd);
        }

        /* 4f. 1-thread vs 3-thread decode/encode equivalence, zero constructor
         * worker threads observed BEFORE jp2_apply_threads on both encode and
         * decode under OPJ_NUM_THREADS=4, + >64B env preserved. */
        set_env_var("OPJ_NUM_THREADS", "4");
        int threads_before = get_process_thread_count();
        atomic_store_explicit(&g_obs_threads_enc_ctor, -1, memory_order_release);
        atomic_store_explicit(&g_obs_threads_dec_ctor, -1, memory_order_release);
        rfc_codecs_jp2_set_hook_for_testing(cli_obs_hook_fn);
        rfc_exec_context e1 = exec;
        e1.codec_threads = 1u;
        void *s1 = NULL;
        CHECK(adp->open_session(&e1, &s1) == RFC_OK);
        rfc_encoded e4_t1;
        CHECK(adp->encode_session(s1, &e1, &f4, 1u, NULL, NULL, 0u, &e4_t1) ==
              RFC_OK);
        rfc_frame *dec1 = NULL;
        size_t dc1 = 0u;
        rfc_metadata dm1;
        CHECK(adp->decode_session(s1, &e1, e4_t1.data, e4_t1.len, NULL, 0u,
                                  &dec1, &dc1, &dm1) == RFC_OK);
        rfc_codecs_jp2_set_hook_for_testing(NULL);
        int threads_enc =
            atomic_load_explicit(&g_obs_threads_enc_ctor, memory_order_acquire);
        int threads_dec =
            atomic_load_explicit(&g_obs_threads_dec_ctor, memory_order_acquire);
        CHECK(threads_before > 0 && threads_enc == threads_before &&
              threads_dec == threads_before);
        adp->close_session(s1);

        const char *long_env =
            "4_padding_long_value_over_64_bytes_0123456789_0123456789_0123456789";
        set_env_var("OPJ_NUM_THREADS", long_env);
        rfc_exec_context e3 = exec;
        e3.codec_threads = 3u;
        CHECK(adp->open_session(&e3, &se) == RFC_OK);
        rfc_encoded e4_t3;
        CHECK(adp->encode_session(se, &e3, &f4, 1u, NULL, NULL, 0u, &e4_t3) ==
              RFC_OK);
        CHECK(adp->decode_session(se, &e3, e4_t3.data, e4_t3.len, NULL, 0u,
                                  &dec, &dc, &dm) == RFC_OK);
        CHECK(dc1 == 1u && dc == 1u &&
              dec1[0].data_len == dec[0].data_len &&
              memcmp(dec1[0].data, dec[0].data, dec[0].data_len) == 0 &&
              memcmp(dec[0].data, f4.data, f4.data_len) == 0);
        const char *after_env = getenv("OPJ_NUM_THREADS");
        CHECK(after_env != NULL && strcmp(after_env, long_env) == 0);
        unset_env_var("OPJ_NUM_THREADS");
        rfc_metadata_free(&dm1, &al);
        rfc_frame_free(&dec1[0]);
        al.free(al.ctx, dec1);
        rfc_encoded_free(&e4_t1);
        rfc_metadata_free(&dm, &al);
        rfc_frame_free(&dec[0]);
        al.free(al.ctx, dec);
        adp->close_session(se);
        rfc_encoded_free(&e4_t3);

        free(m2);
        rfc_encoded_free(&e4);
        rfc_frame_free(&f4);
    }

    rfc_budget_destroy(bud);
    CHECK(st.live_bytes == 0u);
    printf("jp2: review_round1_regressions ok\n");
}

static void test_jobs_budget_cancel_threads_and_file_safety(void) {
    CHECK(rfc_codecs_register_basic() == RFC_OK);
    const rfc_session_adapter *adp =
        rfc_registry_session_adapter(RFC_FORMAT_JPEG2000);
    CHECK(adp != NULL);

    /* 1. Insufficient budget refused on BOTH encode and decode, 0 live leaked. */
    {
        tracked_alloc st;
        rfc_allocator al;
        rfc_budget *ok_bud = rfc_budget_create(16u * 1024u * 1024u);
        rfc_budget *tight = rfc_budget_create(128u);
        CHECK(ok_bud != NULL && tight != NULL);
        rfc_exec_context ok_exec, tight_exec;
        init_exec(&ok_exec, &st, &al, ok_bud);
        tight_exec = ok_exec;
        tight_exec.budget = tight;
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 16u, 16u, 48u, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        memset(src.data, 0x22, src.data_len);
        void *s = NULL;
        CHECK(adp->open_session(&ok_exec, &s) == RFC_OK);
        rfc_encoded good_enc;
        CHECK(adp->encode_session(s, &ok_exec, &src, 1u, NULL, NULL, 0u,
                                  &good_enc) == RFC_OK);
        /* Tight encode refusal */
        rfc_encoded bad_enc;
        CHECK(adp->encode_session(s, &tight_exec, &src, 1u, NULL, NULL, 0u,
                                  &bad_enc) == RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_stats bsn;
        rfc_budget_stats_get(tight, &bsn);
        CHECK(bsn.current_allocated_bytes == 0u);
        /* Tight decode refusal */
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        CHECK(adp->decode_session(s, &tight_exec, good_enc.data, good_enc.len,
                                  NULL, 0u, &dec, &dc, &dm) ==
              RFC_ERR_OUT_OF_MEMORY);
        rfc_budget_stats_get(tight, &bsn);
        CHECK(bsn.current_allocated_bytes == 0u && dec == NULL && dc == 0u);
        adp->close_session(s);
        rfc_encoded_free(&good_enc);
        rfc_frame_free(&src);
        rfc_budget_destroy(tight);
        rfc_budget_destroy(ok_bud);
        CHECK(st.live_bytes == 0u);
    }

    /* 2. Scheduler file jobs: JP2 -> PNG, PNG -> JP2, byte-identical JP2 -> JP2,
     * 1-thread job under conflicting OPJ_NUM_THREADS=4 (Plan #7),
     * pre-decode premultiplied RGBA refusal (Plan #4), active cancel & rollback. */
    {
        rfc_scheduler_config cfg;
        rfc_scheduler_config_default(&cfg);
        cfg.max_workers = 1u;
        cfg.memory_budget_bytes = 64u * 1024u * 1024u;
        rfc_scheduler *sched = rfc_scheduler_create(&cfg);
        CHECK(sched != NULL);

        /* Create a valid JP2 on disk. */
        tracked_alloc st;
        rfc_allocator al;
        rfc_exec_context exec;
        init_exec(&exec, &st, &al, NULL);
        rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 24u, 20u, 72u, 8u};
        rfc_frame src;
        CHECK(rfc_frame_alloc(&src, &lay, NULL, &al) == RFC_OK);
        for (size_t i = 0u; i < src.data_len; i++) {
            src.data[i] = (uint8_t)((i * 31u + 9u) & 0xFFu);
        }
        void *se = NULL;
        CHECK(adp->open_session(&exec, &se) == RFC_OK);
        rfc_encoded j_bytes;
        CHECK(adp->encode_session(se, &exec, &src, 1u, NULL, NULL, 0u,
                                  &j_bytes) == RFC_OK);
        adp->close_session(se);

        char p_jp2[256], p_png[256], p_jp2_2[256], p_bi[256], p_occ[256];
        temp_path(p_jp2, sizeof(p_jp2), "src.jp2");
        temp_path(p_png, sizeof(p_png), "out.png");
        temp_path(p_jp2_2, sizeof(p_jp2_2), "out2.jp2");
        temp_path(p_bi, sizeof(p_bi), "bi.jp2");
        temp_path(p_occ, sizeof(p_occ), "occ.jp2");
        remove(p_jp2); remove(p_png); remove(p_jp2_2); remove(p_bi); remove(p_occ);
        CHECK(write_file_all(p_jp2, j_bytes.data, j_bytes.len));

        /* 1-thread job under conflicting OPJ_NUM_THREADS=4 (Plan #7). */
        set_env_var("OPJ_NUM_THREADS", "4");
        rfc_io_source si;
        rfc_io_sink so;
        rfc_job_spec sp;
        rfc_job *job = NULL;
        rfc_job_snapshot snap;

        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_jp2, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_png, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_JPEG2000;
        sp.target_format = RFC_FORMAT_PNG;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(job);

        /* PNG -> JP2 layout-free job. */
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_png, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_jp2_2, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_PNG;
        sp.target_format = RFC_FORMAT_JPEG2000;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(job);
        unset_env_var("OPJ_NUM_THREADS");

        /* Same-format byte-identical JP2 -> JP2 copy. */
        rfc_setting_value bi_sv[1] = {{"exactness", "byte-identical"}};
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_jp2, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_bi, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_JPEG2000;
        sp.target_format = RFC_FORMAT_JPEG2000;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.settings = bi_sv;
        sp.setting_count = 1u;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK);
        rfc_job_release(job);
        size_t bi_len = 0u;
        uint8_t *bi_buf = read_file_all(p_bi, &bi_len);
        CHECK(bi_buf != NULL && bi_len == j_bytes.len &&
              memcmp(bi_buf, j_bytes.data, bi_len) == 0);
        free(bi_buf);

        /* Pre-decode premultiplied RGBA refusal (Plan #4): decode_calls == 0. */
        {
            rfc_session_adapter msess;
            memset(&msess, 0, sizeof(msess));
            msess.abi_version = RFC_CODEC_ABI_VERSION;
            msess.format = RFC_FORMAT_JPEG_XL;
            msess.open_session = pm_open;
            msess.close_session = pm_close;
            msess.probe_session = pm_probe;
            msess.decode_session = pm_decode;
            msess.encode_session = pm_encode;
            msess.cancel_session = pm_cancel;
            CHECK(rfc_registry_register_session(&msess) == RFC_OK);

            char p_pm[256], p_pm_out[256];
            temp_path(p_pm, sizeof(p_pm), "premul.jxl");
            temp_path(p_pm_out, sizeof(p_pm_out), "premul_out.jp2");
            remove(p_pm_out);
            static const uint8_t jxl_stub[16] = {0xFFu, 0x0Au};
            CHECK(write_file_all(p_pm, jxl_stub, sizeof(jxl_stub)));

            g_jp2_premul_mock.decode_calls = 0u;
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_pm, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_pm_out, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG_XL;
            sp.target_format = RFC_FORMAT_JPEG2000;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.expected_working_set_bytes = 64u * 1024u;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_UNSUPPORTED_ALPHA &&
                  snap.detail[0] != '\0' &&
                  g_jp2_premul_mock.decode_calls == 0u &&
                  !file_exists(p_pm_out));
            rfc_job_release(job);
            remove(p_pm);
            CHECK(rfc_registry_unregister_session(RFC_FORMAT_JPEG_XL) == RFC_OK);
        }

        /* Occupied destination rollback + mid-decode cancellation. */
        static const uint8_t sentinel[12] = {'K', 'E', 'E', 'P', 'M', 'E',
                                             'S', 'A', 'F', 'E', '!', '!'};
        CHECK(write_file_all(p_occ, sentinel, sizeof(sentinel)));
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_png, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_occ, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_PNG;
        sp.target_format = RFC_FORMAT_JPEG2000;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        job = NULL;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) ==
                  RFC_ERR_DESTINATION_EXISTS &&
              job == NULL);
        size_t occ_len = 0u;
        uint8_t *occ_buf = read_file_all(p_occ, &occ_len);
        CHECK(occ_buf != NULL && occ_len == sizeof(sentinel) &&
              memcmp(occ_buf, sentinel, sizeof(sentinel)) == 0);
        free(occ_buf);

        char p_can[256];
        temp_path(p_can, sizeof(p_can), "cancel_out.png");
        remove(p_can);
        atomic_store_explicit(&g_cancel_job, NULL, memory_order_release);
        atomic_store_explicit(&g_cancel_armed, 0, memory_order_release);
        rfc_codecs_jp2_set_hook_for_testing(cancel_hook_fn);
        memset(&sp, 0, sizeof(sp));
        rfc_io_source_init_path(&si, p_jp2, RFC_IMPORT_DEFAULT);
        rfc_io_sink_init_path(&so, p_can, RFC_EXPORT_REFUSE_EXISTING);
        sp.source_format = RFC_FORMAT_JPEG2000;
        sp.target_format = RFC_FORMAT_PNG;
        sp.io_source = &si;
        sp.io_sink = &so;
        sp.requested_codec_threads = 1u;
        CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
        atomic_store_explicit(&g_cancel_job, job, memory_order_release);
        atomic_store_explicit(&g_cancel_armed, 1, memory_order_release);
        CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
        atomic_store_explicit(&g_cancel_job, NULL, memory_order_release);
        rfc_codecs_jp2_set_hook_for_testing(NULL);
        CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
        CHECK(snap.state == RFC_JOB_CANCELLED && !file_exists(p_can));
        rfc_job_release(job);

        /* Layout-free file job with >16 KiB ICC profile inside jp2h. */
        {
            uint8_t *big_icc = (uint8_t *)calloc(1u, 20480u);
            CHECK(big_icc != NULL);
            put_be32(big_icc, 20480u);
            big_icc[8] = 2u; /* ICC v2.0 */
            memcpy(big_icc + 12u, "mntrRGB XYZ ", 12u);
            memcpy(big_icc + 36u, "acsp", 4u);
            rfc_metadata bm;
            memset(&bm, 0, sizeof(bm));
            bm.icc = big_icc;
            bm.icc_len = 20480u;
            void *s_big = NULL;
            CHECK(adp->open_session(&exec, &s_big) == RFC_OK);
            rfc_encoded e_big;
            CHECK(adp->encode_session(s_big, &exec, &src, 1u, &bm, NULL, 0u,
                                      &e_big) == RFC_OK);
            adp->close_session(s_big);
            char p_big_in[256], p_big_out[256];
            temp_path(p_big_in, sizeof(p_big_in), "big_icc.jp2");
            temp_path(p_big_out, sizeof(p_big_out), "big_icc_out.png");
            remove(p_big_in); remove(p_big_out);
            CHECK(write_file_all(p_big_in, e_big.data, e_big.len));
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_big_in, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_big_out, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG2000;
            sp.target_format = RFC_FORMAT_PNG;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK &&
                  file_exists(p_big_out));
            rfc_job_release(job);
            remove(p_big_in); remove(p_big_out);
            rfc_encoded_free(&e_big);
            free(big_icc);
        }

        /* Layout-free file job with 1025 leading free boxes (one 24 KiB) before jp2h. */
        {
            const uint8_t *jp2h_p = NULL;
            size_t jp2h_l = 0u;
            CHECK(find_jp2_box(j_bytes.data, j_bytes.len, "jp2h", &jp2h_p,
                               &jp2h_l));
            size_t jp2h_off = (size_t)(jp2h_p - j_bytes.data) - 8u;
            size_t small_free = 8u;
            size_t big_free = 24u * 1024u;
            size_t total_free = 1024u * small_free + big_free;
            uint8_t *buf_f = (uint8_t *)calloc(1u, j_bytes.len + total_free);
            CHECK(buf_f != NULL);
            memcpy(buf_f, j_bytes.data, jp2h_off);
            size_t pos_f = jp2h_off;
            for (int fi = 0; fi < 1024; fi++) {
                put_be32(buf_f + pos_f, (uint32_t)small_free);
                memcpy(buf_f + pos_f + 4u, "free", 4u);
                pos_f += small_free;
            }
            put_be32(buf_f + pos_f, (uint32_t)big_free);
            memcpy(buf_f + pos_f + 4u, "free", 4u);
            pos_f += big_free;
            memcpy(buf_f + pos_f, j_bytes.data + jp2h_off,
                   j_bytes.len - jp2h_off);
            char p_free_in[256], p_free_out[256];
            temp_path(p_free_in, sizeof(p_free_in), "free_prefix.jp2");
            temp_path(p_free_out, sizeof(p_free_out), "free_prefix_out.png");
            remove(p_free_in); remove(p_free_out);
            CHECK(write_file_all(p_free_in, buf_f, j_bytes.len + total_free));
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_free_in, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_free_out, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG2000;
            sp.target_format = RFC_FORMAT_PNG;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK &&
                  file_exists(p_free_out));
            rfc_job_release(job);
            remove(p_free_in); remove(p_free_out);
            free(buf_f);
        }

        /* Hostile 64-bit wrapping nested box length inside jp2h past 16 KiB prefix
         * must terminate immediately in sniff_jp2_geometry_stream without wrapping
         * backward into the preceding free box (where a decoy ihdr is planted at
         * offset joff + 8 - 256 so an unguarded sub += stot would extract 17x19
         * geometry and admit a layout-free submission instead of refusing it). */
        {
            size_t free_sz = 20u * 1024u;
            size_t total_sz = 12u + 20u + free_sz + 8u + 16u;
            uint8_t *hostile = (uint8_t *)calloc(1u, total_sz);
            CHECK(hostile != NULL);
            memcpy(hostile, "\x00\x00\x00\x0cjP  \x0d\x0a\x87\x0a", 12u);
            put_be32(hostile + 12u, 20u);
            memcpy(hostile + 16u, "ftypjp2 \x00\x00\x00\x00jp2 ", 16u);
            put_be32(hostile + 32u, (uint32_t)free_sz);
            memcpy(hostile + 36u, "free", 4u);
            size_t joff = 32u + free_sz;
            /* Plant a valid 22-byte decoy ihdr (17x19, 3ch, 8-bit) at the exact
             * offset (joff + 8 - 256) that sub += 0xFFFFFFFFFFFFFF00 wraps to. */
            size_t decoy = joff + 8u - 256u;
            put_be32(hostile + decoy, 22u);
            memcpy(hostile + decoy + 4u, "ihdr", 4u);
            put_be32(hostile + decoy + 8u, 19u);   /* height */
            put_be32(hostile + decoy + 12u, 17u);  /* width */
            hostile[decoy + 16u] = 0u;
            hostile[decoy + 17u] = 3u;             /* nc = 3 */
            hostile[decoy + 18u] = 7u;             /* bpc = 8-bit unsigned */
            hostile[decoy + 19u] = 7u;             /* C = 7 */
            hostile[decoy + 20u] = 0u;             /* UnkC */
            hostile[decoy + 21u] = 0u;             /* IPR */
            put_be32(hostile + joff, 24u);
            memcpy(hostile + joff + 4u, "jp2h", 4u);
            /* Child box with lbox=1 and 64-bit length 0xFFFFFFFFFFFFFF00 (> sub_end). */
            put_be32(hostile + joff + 8u, 1u);
            memcpy(hostile + joff + 12u, "free", 4u);
            put_be32(hostile + joff + 16u, 0xFFFFFFFFu);
            put_be32(hostile + joff + 20u, 0xFFFFFF00u);
            char p_hos[256], p_hos_out[256];
            temp_path(p_hos, sizeof(p_hos), "hostile_wrap.jp2");
            temp_path(p_hos_out, sizeof(p_hos_out), "hostile_wrap.png");
            remove(p_hos);
            remove(p_hos_out);
            CHECK(write_file_all(p_hos, hostile, total_sz));
            rfc_sniff_result hsr;
            uint64_t hsz = 0u;
            rfc_file_identity hid;
            memset(&hsr, 0, sizeof(hsr));
            memset(&hid, 0, sizeof(hid));
            CHECK(rfc_input_sniff_path(p_hos, &hsr, &hsz, &hid) == RFC_OK);
            CHECK(hsr.content_format == RFC_FORMAT_JPEG2000);
            /* Layout-free scheduler submission must NOT read the backward decoy
             * ihdr; it must refuse admission before queueing with no job/output. */
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_hos, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_hos_out, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG2000;
            sp.target_format = RFC_FORMAT_PNG;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.requested_codec_threads = 1u;
            job = NULL;
            rfc_status wst = rfc_scheduler_submit(sched, NULL, &sp, &job);
            CHECK(wst == RFC_ERR_LIMIT_EXCEEDED && job == NULL &&
                  !file_exists(p_hos_out));
            if (job) {
                rfc_job_wait(job, UINT32_MAX);
                rfc_job_release(job);
            }
            remove(p_hos);
            remove(p_hos_out);
            free(hostile);
        }

        /* Bare J2K 3-ch accepted + bare J2K 4-ch refused through file importer. */
        {
            const uint8_t *cs3 = NULL;
            size_t cs3_len = 0u;
            CHECK(find_jp2_box(j_bytes.data, j_bytes.len, "jp2c", &cs3, &cs3_len));
            char p_j2k3[256], p_j2k3_out[256];
            temp_path(p_j2k3, sizeof(p_j2k3), "bare3.j2k");
            temp_path(p_j2k3_out, sizeof(p_j2k3_out), "bare3_out.png");
            remove(p_j2k3); remove(p_j2k3_out);
            CHECK(write_file_all(p_j2k3, cs3, cs3_len));
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_j2k3, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_j2k3_out, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG2000;
            sp.target_format = RFC_FORMAT_PNG;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_SUCCEEDED && snap.status == RFC_OK &&
                  file_exists(p_j2k3_out));
            rfc_job_release(job);
            remove(p_j2k3); remove(p_j2k3_out);

            /* 4-ch bare J2K refused with RFC_ERR_UNSUPPORTED_COMBINATION. */
            rfc_layout l4 = {RFC_SAMPLE_RGBA8, RFC_ALPHA_STRAIGHT, 8u, 8u, 32u, 8u};
            rfc_frame f4;
            CHECK(rfc_frame_alloc(&f4, &l4, NULL, &al) == RFC_OK);
            memset(f4.data, 0x55, f4.data_len);
            void *s4 = NULL;
            CHECK(adp->open_session(&exec, &s4) == RFC_OK);
            rfc_encoded e4;
            CHECK(adp->encode_session(s4, &exec, &f4, 1u, NULL, NULL, 0u, &e4) ==
                  RFC_OK);
            adp->close_session(s4);
            const uint8_t *cs4 = NULL;
            size_t cs4_len = 0u;
            CHECK(find_jp2_box(e4.data, e4.len, "jp2c", &cs4, &cs4_len));
            char p_j2k4[256], p_j2k4_out[256];
            temp_path(p_j2k4, sizeof(p_j2k4), "bare4.j2k");
            temp_path(p_j2k4_out, sizeof(p_j2k4_out), "bare4_out.png");
            remove(p_j2k4); remove(p_j2k4_out);
            CHECK(write_file_all(p_j2k4, cs4, cs4_len));
            memset(&sp, 0, sizeof(sp));
            rfc_io_source_init_path(&si, p_j2k4, RFC_IMPORT_DEFAULT);
            rfc_io_sink_init_path(&so, p_j2k4_out, RFC_EXPORT_REFUSE_EXISTING);
            sp.source_format = RFC_FORMAT_JPEG2000;
            sp.target_format = RFC_FORMAT_PNG;
            sp.io_source = &si;
            sp.io_sink = &so;
            sp.requested_codec_threads = 1u;
            CHECK(rfc_scheduler_submit(sched, NULL, &sp, &job) == RFC_OK);
            CHECK(rfc_job_wait(job, UINT32_MAX) == RFC_OK);
            CHECK(rfc_job_snapshot_get(job, &snap) == RFC_OK);
            CHECK(snap.state == RFC_JOB_FAILED &&
                  snap.status == RFC_ERR_UNSUPPORTED_COMBINATION &&
                  !file_exists(p_j2k4_out));
            rfc_job_release(job);
            remove(p_j2k4); remove(p_j2k4_out);
            rfc_encoded_free(&e4);
            rfc_frame_free(&f4);
        }

        remove(p_jp2); remove(p_png); remove(p_jp2_2); remove(p_bi);
        remove(p_occ); remove(p_can);
        rfc_encoded_free(&j_bytes);
        rfc_frame_free(&src);
        rfc_scheduler_destroy(sched);
        CHECK(st.live_bytes == 0u);
    }

    CHECK(rfc_codecs_unregister_basic() == RFC_OK);
    printf("jp2: jobs_budget_cancel_threads_and_file_safety ok\n");
}

/* ---- CLI mode for test_jp2.py ----
 *   encode <in.rfqd> <out.jp2> <preserve|strip> [icc_path]
 *   decode <in.jp2>  <out.rfqd> <preserve|strip>
 * RFQD binary layout:
 *   4B "RFQD", u32 LE w, u32 LE h, u32 LE fmt, u32 LE alpha, u32 LE bits,
 *   u32 LE stride, u32 LE data_len, u32 LE icc_len, data[data_len], icc[icc_len]. */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static int cli_main(int argc, char **argv) {
    if (rfc_codecs_register_jp2() != RFC_OK) {
        printf("status=register-failed\n");
        return 2;
    }
    const rfc_session_adapter *adp =
        rfc_registry_session_adapter(RFC_FORMAT_JPEG2000);
    tracked_alloc st;
    rfc_allocator al;
    rfc_budget *bud = rfc_budget_create(128u * 1024u * 1024u);
    rfc_exec_context exec;
    init_exec(&exec, &st, &al, bud);

    if (strcmp(argv[1], "encode") == 0 && argc >= 5) {
        size_t in_len = 0u;
        uint8_t *raw = read_file_all(argv[2], &in_len);
        if (!raw || in_len < 36u || memcmp(raw, "RFQD", 4) != 0) {
            free(raw);
            printf("status=invalid-argument\n");
            rfc_budget_destroy(bud);
            return 2;
        }
        rfc_frame fr;
        memset(&fr, 0, sizeof(fr));
        fr.layout.width = le32(raw + 4u);
        fr.layout.height = le32(raw + 8u);
        fr.layout.format = (rfc_sample_format)le32(raw + 12u);
        fr.layout.alpha = (rfc_alpha_mode)le32(raw + 16u);
        fr.layout.bits_per_sample = le32(raw + 20u);
        fr.layout.stride_bytes = le32(raw + 24u);
        uint32_t dlen = le32(raw + 28u);
        uint32_t ilen = le32(raw + 32u);
        if ((size_t)36u + dlen + ilen > in_len) {
            free(raw);
            printf("status=invalid-argument\n");
            rfc_budget_destroy(bud);
            return 2;
        }
        fr.data = raw + 36u;
        fr.data_len = dlen;
        rfc_metadata meta;
        memset(&meta, 0, sizeof(meta));
        if (ilen > 0u) {
            meta.icc = raw + 36u + dlen;
            meta.icc_len = ilen;
        }
        rfc_setting_value sv[1] = {{"metadata", argv[4]}};
        void *s = NULL;
        rfc_status rc = adp->open_session(&exec, &s);
        rfc_encoded enc;
        memset(&enc, 0, sizeof(enc));
        if (rc == RFC_OK) {
            rc = adp->encode_session(s, &exec, &fr, 1u, &meta, sv, 1u, &enc);
            adp->close_session(s);
        }
        printf("status=%s\nlen=%zu\n", rfc_status_name(rc), enc.len);
        int exit_c = (rc == RFC_OK) ? 0 : 2;
        if (rc == RFC_OK && !write_file_all(argv[3], enc.data, enc.len)) {
            exit_c = 2;
        }
        rfc_encoded_free(&enc);
        free(raw);
        rfc_budget_destroy(bud);
        return exit_c;
    }

    if (strcmp(argv[1], "decode") == 0 && argc >= 5) {
        size_t in_len = 0u;
        uint8_t *raw = read_file_all(argv[2], &in_len);
        if (!raw) {
            printf("status=io-error\n");
            rfc_budget_destroy(bud);
            return 2;
        }
        rfc_setting_value sv[1] = {{"metadata", argv[4]}};
        void *s = NULL;
        rfc_status rc = adp->open_session(&exec, &s);
        size_t base_alloc = st.live_bytes;
        st.peak_bytes = base_alloc;
        rfc_frame *dec = NULL;
        size_t dc = 0u;
        rfc_metadata dm;
        memset(&dm, 0, sizeof(dm));
        atomic_store_explicit(&g_cli_provider_calls, 0u, memory_order_release);
        atomic_store_explicit(&g_cli_frame_alloc_calls, 0u, memory_order_release);
        rfc_codecs_jp2_set_hook_for_testing(cli_obs_hook_fn);
        if (rc == RFC_OK) {
            rc = adp->decode_session(s, &exec, raw, in_len, sv, 1u, &dec, &dc,
                                     &dm);
            adp->close_session(s);
        }
        rfc_codecs_jp2_set_hook_for_testing(NULL);
        size_t peak_delta =
            (st.peak_bytes > base_alloc) ? (st.peak_bytes - base_alloc) : 0u;
        rfc_budget_stats bsn;
        rfc_budget_stats_get(bud, &bsn);
        printf("status=%s\npeak_alloc=%zu\npeak_budget=%llu\nlive_budget=%llu\n"
               "provider_calls=%u\nframe_alloc_calls=%u\nout_frames=%zu\n",
               rfc_status_name(rc), peak_delta,
               (unsigned long long)bsn.peak_allocated_bytes,
               (unsigned long long)bsn.current_allocated_bytes,
               (unsigned)atomic_load_explicit(&g_cli_provider_calls,
                                              memory_order_acquire),
               (unsigned)atomic_load_explicit(&g_cli_frame_alloc_calls,
                                              memory_order_acquire),
               dc);
        int exit_c = (rc == RFC_OK) ? 0 : 2;
        if (rc == RFC_OK && dc == 1u) {
            printf("w=%u\nh=%u\nfmt=%u\nalpha=%u\nbits=%u\nicc_len=%zu\n",
                   dec[0].layout.width, dec[0].layout.height,
                   (unsigned)dec[0].layout.format,
                   (unsigned)dec[0].layout.alpha,
                   dec[0].layout.bits_per_sample, dm.icc_len);
            size_t total = 36u + dec[0].data_len + dm.icc_len;
            uint8_t *out = (uint8_t *)malloc(total);
            if (out) {
                memcpy(out, "RFQD", 4u);
                put_le32(out + 4u, dec[0].layout.width);
                put_le32(out + 8u, dec[0].layout.height);
                put_le32(out + 12u, (uint32_t)dec[0].layout.format);
                put_le32(out + 16u, (uint32_t)dec[0].layout.alpha);
                put_le32(out + 20u, dec[0].layout.bits_per_sample);
                put_le32(out + 24u, (uint32_t)dec[0].layout.stride_bytes);
                put_le32(out + 28u, (uint32_t)dec[0].data_len);
                put_le32(out + 32u, (uint32_t)dm.icc_len);
                memcpy(out + 36u, dec[0].data, dec[0].data_len);
                if (dm.icc_len > 0u) {
                    memcpy(out + 36u + dec[0].data_len, dm.icc, dm.icc_len);
                }
                if (!write_file_all(argv[3], out, total)) {
                    exit_c = 2;
                }
                free(out);
            }
            rfc_metadata_free(&dm, &al);
            rfc_frame_free(&dec[0]);
            al.free(al.ctx, dec);
        }
        free(raw);
        rfc_budget_destroy(bud);
        return exit_c;
    }

    rfc_budget_destroy(bud);
    return 2;
}

int main(int argc, char **argv) {
    if (argc >= 2) {
        return cli_main(argc, argv);
    }
    test_registration_and_geometry();
    test_samples_precision_stride_and_icc();
    test_refusals_malformed_and_metadata_policy();
    test_review_round1_regressions();
    test_jobs_budget_cancel_threads_and_file_safety();
    if (g_fails != 0) {
        fprintf(stderr, "jp2: %d FAILURES out of %d checks\n", g_fails,
                g_checks);
        return 1;
    }
    printf("jp2: all checks passed (%d checks)\n", g_checks);
    return 0;
}
