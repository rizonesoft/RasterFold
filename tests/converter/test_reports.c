/* Measurements, sequential comparison, versioned JSON report, build-config
 * provenance, cancellation, partial-iteration, and tamper-evident JSONL
 * history test driver for D10 T01 §6. */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif

#include "rasterfold.h"
#include "rfc_codec.h"
#include "rfc_io.h"
#include "rfc_job.h"
#include "rfc_report.h"
#include "rfc_verify.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond); \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0u; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xedb88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xffffffffu;
}

/* ----------------------------------------------------------------
 * Portable temp dir & file helpers
 * ---------------------------------------------------------------- */

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    size_t dl = strlen(dir);
    if (dl > 0u && (dir[dl - 1u] == '/' || dir[dl - 1u] == '\\')) {
        sep = "";
    }
    int n = snprintf(out, cap, "%s%s%s", dir, sep, name);
    CHECK(n > 0 && (size_t)n < cap);
}

static int file_exists_utf8(const char *path) {
#ifdef _WIN32
    wchar_t wpath[4096];
    int wl = MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096);
    if (wl <= 0) {
        return 0;
    }
    return GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return lstat(path, &st) == 0;
#endif
}

static void make_temp_dir(char *out_dir, size_t cap, const char *tag) {
    static atomic_uint g_dir_seq = 1u;
    unsigned seq = atomic_fetch_add(&g_dir_seq, 1u);
#ifdef _WIN32
    wchar_t wbase[MAX_PATH];
    DWORD bn = GetTempPathW(MAX_PATH, wbase);
    CHECK(bn > 0 && bn < MAX_PATH - 96);
    char base[1024];
    int u8n = WideCharToMultiByte(CP_UTF8, 0, wbase, -1, base, sizeof(base),
                                  NULL, NULL);
    CHECK(u8n > 0);
    size_t bl = strlen(base);
    while (bl > 0u && (base[bl - 1u] == '\\' || base[bl - 1u] == '/')) {
        base[--bl] = '\0';
    }
    int n = snprintf(out_dir, cap, "%s\\rfc_%s_%lu_%u", base, tag,
                     (unsigned long)GetCurrentProcessId(), seq);
    CHECK(n > 0 && (size_t)n < cap);
    wchar_t wout[4096];
    CHECK(MultiByteToWideChar(CP_UTF8, 0, out_dir, -1, wout, 4096) > 0);
    CHECK(CreateDirectoryW(wout, NULL) != 0);
#else
    char tmpl[1024];
    int n = snprintf(tmpl, sizeof(tmpl), "/tmp/rfc_%s_%lu_%u_XXXXXX", tag,
                     (unsigned long)getpid(), seq);
    CHECK(n > 0 && (size_t)n < sizeof(tmpl));
    CHECK(mkdtemp(tmpl) != NULL);
    CHECK(strlen(tmpl) < cap);
    memcpy(out_dir, tmpl, strlen(tmpl) + 1u);
#endif
}

static void rm_tree(const char *path) {
#ifdef _WIN32
    wchar_t wpath[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096) <= 0) {
        return;
    }
    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        char pat[4096];
        join_path(pat, sizeof(pat), path, "*");
        wchar_t wpat[4096];
        if (MultiByteToWideChar(CP_UTF8, 0, pat, -1, wpat, 4096) > 0) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(wpat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    char cname[512];
                    if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, cname,
                                            sizeof(cname), NULL, NULL) > 0) {
                        if (strcmp(cname, ".") != 0 &&
                            strcmp(cname, "..") != 0) {
                            char child[4096];
                            join_path(child, sizeof(child), path, cname);
                            rm_tree(child);
                        }
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        RemoveDirectoryW(wpath);
    } else {
        SetFileAttributesW(wpath, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(wpath);
    }
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") != 0 &&
                    strcmp(ent->d_name, "..") != 0) {
                    char child[4096];
                    join_path(child, sizeof(child), path, ent->d_name);
                    rm_tree(child);
                }
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
#endif
}

static int dir_has_prefix(const char *dir, const char *prefix) {
    size_t plen = strlen(prefix);
#ifdef _WIN32
    char pat[4096];
    join_path(pat, sizeof(pat), dir, "*");
    wchar_t wpat[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, pat, -1, wpat, 4096) <= 0) {
        return 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int found = 0;
    do {
        char cname[512];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, cname,
                                sizeof(cname), NULL, NULL) > 0) {
            if (strstr(cname, prefix) != NULL) {
                found = 1;
                break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    (void)plen;
    return found;
#else
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strstr(ent->d_name, prefix) != NULL ||
            strncmp(ent->d_name, prefix, plen) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
#endif
}

static void read_file_all(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    CHECK(fseek(fp, 0, SEEK_END) == 0);
    long sz = ftell(fp);
    CHECK(sz >= 0);
    CHECK(fseek(fp, 0, SEEK_SET) == 0);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1u);
    CHECK(buf != NULL);
    if (sz > 0) {
        CHECK(fread(buf, 1u, (size_t)sz, fp) == (size_t)sz);
    }
    buf[sz] = '\0';
    fclose(fp);
    *out_buf = buf;
    *out_len = (size_t)sz;
}

static void write_file_all(const char *path, const uint8_t *buf, size_t len) {
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (len > 0u) {
        CHECK(fwrite(buf, 1u, len, fp) == len);
    }
    CHECK(fclose(fp) == 0);
}

/* ----------------------------------------------------------------
 * Self-contained RFC 1950/1951 zlib inflater + PNG decoder (test only)
 * ---------------------------------------------------------------- */

#define INF_MAX_BITS 15
#define INF_MAX_SYMS 288

typedef struct inf_huff {
    int16_t counts[INF_MAX_BITS + 1];
    int32_t first_code[INF_MAX_BITS + 1];
    int32_t first_sym[INF_MAX_BITS + 1];
    uint16_t syms[INF_MAX_SYMS];
} inf_huff;

static int inf_build_huff(inf_huff *h, const uint8_t *lens, int nsyms) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < nsyms; i++) {
        if (lens[i] > INF_MAX_BITS) {
            return 0;
        }
        h->counts[lens[i]]++;
    }
    int code = 0;
    int left = 1;
    int sym_off = 0;
    h->counts[0] = 0;
    for (int bits = 1; bits <= INF_MAX_BITS; bits++) {
        code <<= 1;
        left <<= 1;
        left -= h->counts[bits];
        if (left < 0) {
            return 0;
        }
        h->first_code[bits] = code;
        h->first_sym[bits] = sym_off;
        code += h->counts[bits];
        sym_off += h->counts[bits];
    }
    int offs[INF_MAX_BITS + 1];
    for (int bits = 0; bits <= INF_MAX_BITS; bits++) {
        offs[bits] = h->first_sym[bits];
    }
    for (int i = 0; i < nsyms; i++) {
        if (lens[i] != 0u) {
            h->syms[offs[lens[i]]++] = (uint16_t)i;
        }
    }
    return 1;
}

typedef struct inf_bs {
    const uint8_t *in;
    size_t len;
    size_t pos;
    uint64_t buf;
    int cnt;
} inf_bs;

static int inf_ensure(inf_bs *bs, int n) {
    while (bs->cnt < n) {
        if (bs->pos >= bs->len) {
            return 0;
        }
        bs->buf |= ((uint64_t)bs->in[bs->pos++]) << bs->cnt;
        bs->cnt += 8;
    }
    return 1;
}

static int inf_read_bits(inf_bs *bs, int n, uint32_t *out) {
    if (n == 0) {
        *out = 0u;
        return 1;
    }
    if (!inf_ensure(bs, n)) {
        return 0;
    }
    *out = (uint32_t)(bs->buf & ((1ull << n) - 1ull));
    bs->buf >>= n;
    bs->cnt -= n;
    return 1;
}

static int inf_decode_sym(inf_bs *bs, const inf_huff *h, uint16_t *out) {
    int code = 0;
    for (int bits = 1; bits <= INF_MAX_BITS; bits++) {
        uint32_t bit = 0u;
        if (!inf_read_bits(bs, 1, &bit)) {
            return 0;
        }
        code = (code << 1) | (int)bit;
        int idx = code - h->first_code[bits];
        if (idx >= 0 && idx < h->counts[bits]) {
            *out = h->syms[h->first_sym[bits] + idx];
            return 1;
        }
    }
    return 0;
}

static rfc_status inf_ensure_out_cap(uint8_t **out_ptr,
                                     size_t *cap_ptr,
                                     size_t used,
                                     size_t need,
                                     size_t expected_len,
                                     const rfc_allocator *alloc) {
    if (expected_len > 0u && need > expected_len) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (need <= *cap_ptr) {
        return RFC_OK;
    }
    size_t ncap = *cap_ptr ? *cap_ptr : 4096u;
    while (ncap < need) {
        ncap = (ncap > SIZE_MAX / 2u) ? need : (ncap * 2u);
    }
    uint8_t *nb = (uint8_t *)(alloc ? alloc->alloc(alloc->ctx, ncap) : malloc(ncap));
    if (!nb) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    if (*out_ptr && used > 0u) {
        memcpy(nb, *out_ptr, used);
    }
    if (alloc) {
        alloc->free(alloc->ctx, *out_ptr);
    } else {
        free(*out_ptr);
    }
    *out_ptr = nb;
    *cap_ptr = ncap;
    return RFC_OK;
}

static rfc_status inf_zlib_decode(const uint8_t *in,
                                  size_t in_len,
                                  size_t expected_len,
                                  const rfc_allocator *alloc,
                                  uint8_t **out_data,
                                  size_t *out_len) {
    if (!in || in_len < 6u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint8_t cmf = in[0];
    uint8_t flg = in[1];
    if ((cmf & 0x0fu) != 8u || (((uint32_t)cmf << 8) | flg) % 31u != 0u ||
        (flg & 0x20u) != 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_status err_st = RFC_ERR_INVALID_ARGUMENT;
    inf_bs bs = {in, in_len - 4u, 2u, 0u, 0};
    size_t cap = expected_len ? expected_len : 4096u;
    size_t used = 0u;
    uint8_t *out = (uint8_t *)(alloc ? alloc->alloc(alloc->ctx, cap) : malloc(cap));
    if (!out) {
        return RFC_ERR_OUT_OF_MEMORY;
    }

    static const uint8_t cl_ord[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                       11, 4,  12, 3, 13, 2, 14, 1, 15};
    static const uint16_t len_base[29] = {
        3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t len_ext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                        1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                        4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dist_base[30] = {
        1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
        33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dist_ext[30] = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,
                                         4, 4, 5, 5, 6, 6, 7,  7,  8,  8,
                                         9, 9, 10,10,11,11,12, 12, 13, 13};

    uint32_t bfinal = 0u;
    do {
        uint32_t btype = 0u;
        if (!inf_read_bits(&bs, 1, &bfinal) ||
            !inf_read_bits(&bs, 2, &btype) || btype == 3u) {
            goto fail;
        }
        if (btype == 0u) {
            int drop = bs.cnt & 7;
            bs.buf >>= drop;
            bs.cnt -= drop;
            if (bs.cnt != 0) {
                size_t back = (size_t)(bs.cnt / 8);
                bs.pos -= back;
                bs.cnt = 0;
                bs.buf = 0u;
            }
            if (bs.pos + 4u > bs.len) {
                goto fail;
            }
            uint16_t blen = rd16le(bs.in + bs.pos);
            uint16_t nlen = rd16le(bs.in + bs.pos + 2u);
            bs.pos += 4u;
            if ((uint16_t)(blen ^ nlen) != 0xffffu ||
                bs.pos + blen > bs.len) {
                goto fail;
            }
            err_st = inf_ensure_out_cap(&out, &cap, used, used + blen,
                                        expected_len, alloc);
            if (err_st != RFC_OK) {
                goto fail;
            }
            if (blen > 0u) {
                memcpy(out + used, bs.in + bs.pos, blen);
                used += blen;
                bs.pos += blen;
            }
            continue;
        }

        inf_huff lit_h, dist_h;
        if (btype == 1u) {
            uint8_t llens[288];
            uint8_t dlens[32];
            for (int i = 0; i <= 143; i++) llens[i] = 8u;
            for (int i = 144; i <= 255; i++) llens[i] = 9u;
            for (int i = 256; i <= 279; i++) llens[i] = 7u;
            for (int i = 280; i <= 287; i++) llens[i] = 8u;
            for (int i = 0; i < 32; i++) dlens[i] = 5u;
            if (!inf_build_huff(&lit_h, llens, 288) ||
                !inf_build_huff(&dist_h, dlens, 32)) {
                goto fail;
            }
        } else {
            uint32_t hlit_u = 0u, hdist_u = 0u, hclen_u = 0u;
            if (!inf_read_bits(&bs, 5, &hlit_u) ||
                !inf_read_bits(&bs, 5, &hdist_u) ||
                !inf_read_bits(&bs, 4, &hclen_u)) {
                goto fail;
            }
            int hlit = (int)hlit_u + 257;
            int hdist = (int)hdist_u + 1;
            int hclen = (int)hclen_u + 4;
            if (hlit > 286 || hdist > 30) {
                goto fail;
            }
            uint8_t clens[19];
            memset(clens, 0, sizeof(clens));
            for (int i = 0; i < hclen; i++) {
                uint32_t v = 0u;
                if (!inf_read_bits(&bs, 3, &v)) {
                    goto fail;
                }
                clens[cl_ord[i]] = (uint8_t)v;
            }
            inf_huff cl_h;
            if (!inf_build_huff(&cl_h, clens, 19)) {
                goto fail;
            }
            uint8_t all_lens[320];
            memset(all_lens, 0, sizeof(all_lens));
            int idx = 0;
            while (idx < hlit + hdist) {
                uint16_t sym = 0u;
                if (!inf_decode_sym(&bs, &cl_h, &sym)) {
                    goto fail;
                }
                if (sym < 16u) {
                    all_lens[idx++] = (uint8_t)sym;
                } else {
                    uint8_t rep_v = 0u;
                    int rep_c = 0;
                    uint32_t ext = 0u;
                    if (sym == 16u) {
                        if (idx == 0 || !inf_read_bits(&bs, 2, &ext)) {
                            goto fail;
                        }
                        rep_v = all_lens[idx - 1];
                        rep_c = 3 + (int)ext;
                    } else if (sym == 17u) {
                        if (!inf_read_bits(&bs, 3, &ext)) {
                            goto fail;
                        }
                        rep_v = 0u;
                        rep_c = 3 + (int)ext;
                    } else if (sym == 18u) {
                        if (!inf_read_bits(&bs, 7, &ext)) {
                            goto fail;
                        }
                        rep_v = 0u;
                        rep_c = 11 + (int)ext;
                    } else {
                        goto fail;
                    }
                    if (idx + rep_c > hlit + hdist) {
                        goto fail;
                    }
                    for (int k = 0; k < rep_c; k++) {
                        all_lens[idx++] = rep_v;
                    }
                }
            }
            if (!inf_build_huff(&lit_h, all_lens, hlit) ||
                !inf_build_huff(&dist_h, all_lens + hlit, hdist)) {
                goto fail;
            }
        }

        for (;;) {
            uint16_t sym = 0u;
            if (!inf_decode_sym(&bs, &lit_h, &sym)) {
                goto fail;
            }
            if (sym < 256u) {
                err_st = inf_ensure_out_cap(&out, &cap, used, used + 1u,
                                            expected_len, alloc);
                if (err_st != RFC_OK) {
                    goto fail;
                }
                out[used++] = (uint8_t)sym;
            } else if (sym == 256u) {
                break;
            } else {
                if (sym > 285u) {
                    goto fail;
                }
                uint32_t le = 0u, de = 0u;
                uint16_t dsym = 0u;
                if (!inf_read_bits(&bs, len_ext[sym - 257u], &le) ||
                    !inf_decode_sym(&bs, &dist_h, &dsym) || dsym >= 30u ||
                    !inf_read_bits(&bs, dist_ext[dsym], &de)) {
                    goto fail;
                }
                size_t match_len = len_base[sym - 257u] + (size_t)le;
                size_t match_dist = dist_base[dsym] + (size_t)de;
                if (match_dist == 0u || match_dist > used) {
                    goto fail;
                }
                err_st = inf_ensure_out_cap(&out, &cap, used, used + match_len,
                                            expected_len, alloc);
                if (err_st != RFC_OK) {
                    goto fail;
                }
                for (size_t k = 0u; k < match_len; k++) {
                    out[used] = out[used - match_dist];
                    used++;
                }
            }
        }
        err_st = RFC_ERR_INVALID_ARGUMENT;
    } while (!bfinal);

    uint32_t s1 = 1u, s2 = 0u;
    for (size_t i = 0u; i < used; i++) {
        s1 = (s1 + out[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    uint32_t expect_adler = ((s2 << 16) | s1);
    uint32_t got_adler = rd32be(in + in_len - 4u);
    if (expect_adler != got_adler) {
        err_st = RFC_ERR_INVALID_ARGUMENT;
        goto fail;
    }

    *out_data = out;
    *out_len = used;
    return RFC_OK;

fail:
    if (alloc) {
        alloc->free(alloc->ctx, out);
    } else {
        free(out);
    }
    return err_st;
}

typedef struct tr_test_sess {
    const rfc_exec_context *exec;
    atomic_int cancelled;
} tr_test_sess;

static atomic_int g_active_encodes = 0;
static atomic_int g_max_concurrent_encodes = 0;

static rfc_status tr_sess_open(const rfc_exec_context *exec, void **out_sess) {
    if (!exec || !exec->allocator || !out_sess) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    tr_test_sess *s = (tr_test_sess *)exec->allocator->alloc(
        exec->allocator->ctx, sizeof(*s));
    if (!s) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    s->exec = exec;
    atomic_init(&s->cancelled, 0);
    *out_sess = s;
    return RFC_OK;
}

static void tr_sess_close(void *raw) {
    tr_test_sess *s = (tr_test_sess *)raw;
    if (s) {
        const rfc_allocator *a = s->exec->allocator;
        a->free(a->ctx, s);
    }
}

static void tr_sess_cancel(void *raw) {
    tr_test_sess *s = (tr_test_sess *)raw;
    if (s) {
        atomic_store_explicit(&s->cancelled, 1, memory_order_release);
    }
}

static void tr_sess_free_encoded(void *raw, rfc_encoded *enc) {
    (void)raw;
    rfc_encoded_free(enc);
}

static int png_parse_ihdr(const uint8_t *bytes,
                          size_t size,
                          uint32_t *out_w,
                          uint32_t *out_h,
                          rfc_sample_format *out_fmt,
                          rfc_alpha_mode *out_alpha) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!bytes || size < 33u || memcmp(bytes, sig, 8u) != 0) {
        return 0;
    }
    uint32_t clen = rd32be(bytes + 8u);
    if (clen != 13u || memcmp(bytes + 12u, "IHDR", 4u) != 0) {
        return 0;
    }
    uint32_t w = rd32be(bytes + 16u);
    uint32_t h = rd32be(bytes + 20u);
    uint8_t bd = bytes[24];
    uint8_t ct = bytes[25];
    uint8_t comp = bytes[26];
    uint8_t filt = bytes[27];
    uint8_t interl = bytes[28];
    if (w == 0u || h == 0u || bd != 8u || comp != 0u || filt != 0u ||
        interl != 0u) {
        return 0;
    }
    *out_w = w;
    *out_h = h;
    if (ct == 2u) {
        *out_fmt = RFC_SAMPLE_RGB8;
        *out_alpha = RFC_ALPHA_NONE;
        return 1;
    }
    if (ct == 6u) {
        *out_fmt = RFC_SAMPLE_RGBA8;
        *out_alpha = RFC_ALPHA_STRAIGHT;
        return 1;
    }
    if (ct == 0u) {
        *out_fmt = RFC_SAMPLE_GRAY8;
        *out_alpha = RFC_ALPHA_NONE;
        return 1;
    }
    return 0;
}

static rfc_status png_sess_probe(void *raw,
                                 const rfc_exec_context *exec,
                                 const uint8_t *bytes,
                                 size_t size,
                                 rfc_image_probe *out_probe) {
    (void)raw;
    (void)exec;
    uint32_t w = 0u, h = 0u;
    rfc_sample_format fmt = RFC_SAMPLE_RGB8;
    rfc_alpha_mode alpha = RFC_ALPHA_NONE;
    if (!out_probe || !png_parse_ihdr(bytes, size, &w, &h, &fmt, &alpha)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    size_t bpp = rfc_sample_bytes(fmt);
    out_probe->first_frame.format = fmt;
    out_probe->first_frame.alpha = alpha;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.stride_bytes = (size_t)w * bpp;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)w * (uint64_t)h;
    out_probe->total_decoded_bytes =
        (uint64_t)w * (uint64_t)h * (uint64_t)bpp;
    return RFC_OK;
}

static uint8_t paeth_pred(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p > (int)a ? (p - (int)a) : ((int)a - p);
    int pb = p > (int)b ? (p - (int)b) : ((int)b - p);
    int pc = p > (int)c ? (p - (int)c) : ((int)c - p);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

static rfc_status png_sess_decode(void *raw,
                                  const rfc_exec_context *exec,
                                  const uint8_t *bytes,
                                  size_t size,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_frame **out_frames,
                                  size_t *out_count,
                                  rfc_metadata *out_meta) {
    (void)raw;
    (void)settings;
    (void)setting_count;
    const rfc_allocator *alloc = rfc_exec_allocator(exec);
    uint32_t w = 0u, h = 0u;
    rfc_sample_format fmt = RFC_SAMPLE_RGB8;
    rfc_alpha_mode alpha = RFC_ALPHA_NONE;
    if (!alloc || !png_parse_ihdr(bytes, size, &w, &h, &fmt, &alpha)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    size_t bpp = rfc_sample_bytes(fmt);
    size_t row_bytes = (size_t)w * bpp;
    size_t expect_raw = (1u + row_bytes) * (size_t)h;

    size_t idat_total = 0u;
    size_t pos = 8u;
    int saw_iend = 0;
    while (pos + 12u <= size) {
        uint32_t clen = rd32be(bytes + pos);
        const uint8_t *ctype = bytes + pos + 4u;
        if (pos + 12u + (size_t)clen > size) {
            return RFC_ERR_TRUNCATED_INPUT;
        }
        if (memcmp(ctype, "IDAT", 4u) == 0 && clen > 0u) {
            if ((size_t)clen > SIZE_MAX - idat_total) {
                return RFC_ERR_LIMIT_EXCEEDED;
            }
            idat_total += (size_t)clen;
        } else if (memcmp(ctype, "IEND", 4u) == 0) {
            saw_iend = 1;
            break;
        }
        pos += 12u + (size_t)clen;
    }
    if (!saw_iend || idat_total == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    uint8_t *idat = (uint8_t *)alloc->alloc(alloc->ctx, idat_total);
    if (!idat) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    size_t idat_len = 0u;
    pos = 8u;
    while (pos + 12u <= size) {
        uint32_t clen = rd32be(bytes + pos);
        const uint8_t *ctype = bytes + pos + 4u;
        const uint8_t *cdata = bytes + pos + 8u;
        if (memcmp(ctype, "IDAT", 4u) == 0 && clen > 0u) {
            memcpy(idat + idat_len, cdata, (size_t)clen);
            idat_len += (size_t)clen;
        } else if (memcmp(ctype, "IEND", 4u) == 0) {
            break;
        }
        pos += 12u + (size_t)clen;
    }

    uint8_t *raw_rows = NULL;
    size_t raw_len = 0u;
    rfc_status inf_st =
        inf_zlib_decode(idat, idat_len, expect_raw, alloc, &raw_rows, &raw_len);
    alloc->free(alloc->ctx, idat);
    if (inf_st != RFC_OK) {
        return inf_st;
    }
    if (raw_len != expect_raw) {
        alloc->free(alloc->ctx, raw_rows);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_layout lay = {fmt, alpha, w, h, row_bytes, 8u};
    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0}};
    rfc_frame *fr = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(*fr));
    if (!fr) {
        alloc->free(alloc->ctx, raw_rows);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(fr, 0, sizeof(*fr));
    rfc_status st = rfc_exec_alloc_frame(exec, fr, &lay, &meta);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, fr);
        alloc->free(alloc->ctx, raw_rows);
        return st;
    }

    for (uint32_t y = 0u; y < h; y++) {
        const uint8_t *src_row = raw_rows + (size_t)y * (1u + row_bytes);
        uint8_t ftype = src_row[0];
        const uint8_t *rbytes = src_row + 1u;
        uint8_t *dst = fr->data + (size_t)y * row_bytes;
        const uint8_t *prev = (y > 0u) ? (dst - row_bytes) : NULL;
        for (size_t x = 0u; x < row_bytes; x++) {
            uint8_t a = (x >= bpp) ? dst[x - bpp] : 0u;
            uint8_t b = prev ? prev[x] : 0u;
            uint8_t c = (prev && x >= bpp) ? prev[x - bpp] : 0u;
            uint8_t v = rbytes[x];
            if (ftype == 1u) {
                v = (uint8_t)(v + a);
            } else if (ftype == 2u) {
                v = (uint8_t)(v + b);
            } else if (ftype == 3u) {
                v = (uint8_t)(v + ((uint16_t)(a + b) >> 1));
            } else if (ftype == 4u) {
                v = (uint8_t)(v + paeth_pred(a, b, c));
            } else if (ftype != 0u) {
                rfc_frame_free(fr);
                alloc->free(alloc->ctx, fr);
                alloc->free(alloc->ctx, raw_rows);
                return RFC_ERR_INVALID_ARGUMENT;
            }
            dst[x] = v;
        }
    }

    alloc->free(alloc->ctx, raw_rows);
    *out_frames = fr;
    *out_count = 1u;
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

static rfc_status png_sess_encode(void *raw,
                                  const rfc_exec_context *exec,
                                  const rfc_frame *frames,
                                  size_t frame_count,
                                  const rfc_metadata *meta,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_encoded *out_encoded) {
    (void)raw;
    (void)exec;
    (void)frames;
    (void)frame_count;
    (void)meta;
    (void)settings;
    (void)setting_count;
    (void)out_encoded;
    return RFC_ERR_NO_ADAPTER;
}

static void record_concurrency_enter(void) {
    int prev = atomic_fetch_add(&g_active_encodes, 1);
    int cur = prev + 1;
    int old_max = atomic_load(&g_max_concurrent_encodes);
    while (cur > old_max &&
           !atomic_compare_exchange_weak(&g_max_concurrent_encodes, &old_max,
                                         cur)) {
    }
}

static void record_concurrency_leave(void) {
    atomic_fetch_sub(&g_active_encodes, 1);
}

static rfc_status (*g_custom_rf_encode_hook)(
    void *sess,
    const rfc_exec_context *exec,
    const rfc_frame *frames,
    size_t frame_count,
    const rfc_metadata *meta,
    const rfc_setting_value *settings,
    size_t setting_count,
    rfc_encoded *out_encoded) = NULL;

static rfc_status rf_sess_probe(void *raw,
                                const rfc_exec_context *exec,
                                const uint8_t *bytes,
                                size_t size,
                                rfc_image_probe *out_probe) {
    (void)raw;
    (void)exec;
    if (!bytes || size < 32u || !out_probe) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (bytes[0] != 'R' || bytes[1] != 'F' || bytes[2] != 'I' ||
        (bytes[3] != 2u && bytes[3] != 3u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t w = rd32le(bytes + 8u);
    uint32_t h = rd32le(bytes + 12u);
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.stride_bytes = (size_t)w * 3u;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)w * (uint64_t)h;
    out_probe->total_decoded_bytes = (uint64_t)w * (uint64_t)h * 3ull;
    return RFC_OK;
}

static rfc_status rf_sess_encode(void *raw,
                                 const rfc_exec_context *exec,
                                 const rfc_frame *frames,
                                 size_t frame_count,
                                 const rfc_metadata *meta,
                                 const rfc_setting_value *settings,
                                 size_t setting_count,
                                 rfc_encoded *out_encoded) {
    tr_test_sess *s = (tr_test_sess *)raw;
    record_concurrency_enter();
    if (rfc_exec_is_cancelled(exec) ||
        atomic_load_explicit(&s->cancelled, memory_order_acquire)) {
        record_concurrency_leave();
        return RFC_ERR_CANCELLED;
    }
    if (g_custom_rf_encode_hook) {
        rfc_status hst = g_custom_rf_encode_hook(
            raw, exec, frames, frame_count, meta, settings, setting_count,
            out_encoded);
        record_concurrency_leave();
        return hst;
    }
    if (!exec || !frames || frame_count != 1u ||
        frames[0].layout.format != RFC_SAMPLE_RGB8 ||
        frames[0].layout.alpha != RFC_ALPHA_NONE) {
        record_concurrency_leave();
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    const rfc_allocator *alloc = rfc_exec_allocator(exec);
    if (!alloc || !alloc->alloc || !alloc->free) {
        record_concurrency_leave();
        return RFC_ERR_INVALID_ARGUMENT;
    }

    const rfc_frame *fr = &frames[0];
    rf_pixels px;
    px.width = fr->layout.width;
    px.height = fr->layout.height;
    px.stride = fr->layout.stride_bytes;
    px.layout = RF_LAYOUT_RGB8;
    px.data = fr->data;
    px.size = fr->data_len;

    rf_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.size = (uint32_t)sizeof(opts);
    uint32_t threads = rfc_exec_threads(exec);
    opts.threads = threads > 9u ? 9u : threads;

    rf_buffer fbuf = {NULL, 0u};
    rf_error ferr = {0, NULL};
    int rc = rf_encode_image(&px, &opts, &fbuf, &ferr);
    if (rc != RF_OK || !fbuf.data) {
        rf_free(fbuf.data);
        record_concurrency_leave();
        return RFC_ERR_OUT_OF_MEMORY;
    }

    uint8_t *out = (uint8_t *)alloc->alloc(alloc->ctx, fbuf.size);
    if (!out) {
        rf_free(fbuf.data);
        record_concurrency_leave();
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memcpy(out, fbuf.data, fbuf.size);
    size_t fold_sz = fbuf.size;
    rf_free(fbuf.data);

    memset(out_encoded, 0, sizeof(*out_encoded));
    out_encoded->data = out;
    out_encoded->len = fold_sz;
    out_encoded->allocator = alloc;
    record_concurrency_leave();
    return RFC_OK;
}

static rfc_status rf_sess_decode(void *raw,
                                 const rfc_exec_context *exec,
                                 const uint8_t *bytes,
                                 size_t size,
                                 const rfc_setting_value *settings,
                                 size_t setting_count,
                                 rfc_frame **out_frames,
                                 size_t *out_count,
                                 rfc_metadata *out_meta) {
    (void)raw;
    (void)settings;
    (void)setting_count;
    const rfc_allocator *alloc = rfc_exec_allocator(exec);
    if (!alloc || !bytes || size < 32u || !out_frames || !out_count ||
        !out_meta) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rf_options dec_opts;
    memset(&dec_opts, 0, sizeof(dec_opts));
    dec_opts.size = (uint32_t)sizeof(dec_opts);
    dec_opts.threads = rfc_exec_threads(exec);
    if (dec_opts.threads > 9u) {
        dec_opts.threads = 9u;
    }
    rf_pixels px;
    memset(&px, 0, sizeof(px));
    rf_error ferr = {0, NULL};
    if (rf_decode_image(bytes, size, &dec_opts, &px, &ferr) != RF_OK ||
        !px.data) {
        rf_free(px.data);
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, px.width, px.height,
                      px.stride, 8u};
    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0}};
    rfc_frame *fr = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(*fr));
    if (!fr) {
        rf_free(px.data);
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(fr, 0, sizeof(*fr));
    rfc_status st = rfc_exec_alloc_frame(exec, fr, &lay, &meta);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, fr);
        rf_free(px.data);
        return st;
    }
    memcpy(fr->data, px.data, px.size);
    rf_free(px.data);
    *out_frames = fr;
    *out_count = 1u;
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

static rfc_status bmp_sess_probe(void *raw,
                                 const rfc_exec_context *exec,
                                 const uint8_t *bytes,
                                 size_t size,
                                 rfc_image_probe *out_probe) {
    (void)raw;
    (void)exec;
    if (!bytes || size < 54u || !out_probe || bytes[0] != 'B' ||
        bytes[1] != 'M') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t w = rd32le(bytes + 18u);
    uint32_t h = rd32le(bytes + 22u);
    uint16_t bpp = rd16le(bytes + 28u);
    if (w == 0u || h == 0u || bpp != 24u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->first_frame.format = RFC_SAMPLE_RGB8;
    out_probe->first_frame.alpha = RFC_ALPHA_NONE;
    out_probe->first_frame.width = w;
    out_probe->first_frame.height = h;
    out_probe->first_frame.stride_bytes = (size_t)w * 3u;
    out_probe->first_frame.bits_per_sample = 8u;
    out_probe->frame_count = 1u;
    out_probe->total_pixels = (uint64_t)w * (uint64_t)h;
    out_probe->total_decoded_bytes = (uint64_t)w * (uint64_t)h * 3ull;
    return RFC_OK;
}

static rfc_status bmp_sess_encode(void *raw,
                                  const rfc_exec_context *exec,
                                  const rfc_frame *frames,
                                  size_t frame_count,
                                  const rfc_metadata *meta,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_encoded *out_encoded) {
    (void)raw;
    (void)meta;
    (void)settings;
    (void)setting_count;
    record_concurrency_enter();
    const rfc_allocator *alloc = rfc_exec_allocator(exec);
    if (!alloc || !frames || frame_count != 1u ||
        frames[0].layout.format != RFC_SAMPLE_RGB8) {
        record_concurrency_leave();
        return RFC_ERR_UNSUPPORTED_SAMPLE;
    }
    const rfc_frame *fr = &frames[0];
    size_t row_stride = ((size_t)fr->layout.width * 3u + 3u) & ~3u;
    size_t pix_bytes = row_stride * (size_t)fr->layout.height;
    size_t total = 54u + pix_bytes;
    uint8_t *out = (uint8_t *)alloc->alloc(alloc->ctx, total);
    if (!out) {
        record_concurrency_leave();
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(out, 0, total);
    out[0] = 'B';
    out[1] = 'M';
    wr32le(out + 2u, (uint32_t)total);
    wr32le(out + 10u, 54u);
    wr32le(out + 14u, 40u);
    wr32le(out + 18u, fr->layout.width);
    wr32le(out + 22u, fr->layout.height);
    wr16le(out + 26u, 1u);
    wr16le(out + 28u, 24u);
    wr32le(out + 34u, (uint32_t)pix_bytes);
    wr32le(out + 38u, 2835u);
    wr32le(out + 42u, 2835u);

    for (uint32_t y = 0u; y < fr->layout.height; y++) {
        const uint8_t *src = fr->data + (size_t)y * fr->layout.stride_bytes;
        uint8_t *dst =
            out + 54u + (size_t)(fr->layout.height - 1u - y) * row_stride;
        for (uint32_t x = 0u; x < fr->layout.width; x++) {
            dst[x * 3u + 0u] = src[x * 3u + 2u];
            dst[x * 3u + 1u] = src[x * 3u + 1u];
            dst[x * 3u + 2u] = src[x * 3u + 0u];
        }
    }

    memset(out_encoded, 0, sizeof(*out_encoded));
    out_encoded->data = out;
    out_encoded->len = total;
    out_encoded->allocator = alloc;
    record_concurrency_leave();
    return RFC_OK;
}

static rfc_status bmp_sess_decode(void *raw,
                                  const rfc_exec_context *exec,
                                  const uint8_t *bytes,
                                  size_t size,
                                  const rfc_setting_value *settings,
                                  size_t setting_count,
                                  rfc_frame **out_frames,
                                  size_t *out_count,
                                  rfc_metadata *out_meta) {
    (void)raw;
    (void)settings;
    (void)setting_count;
    const rfc_allocator *alloc = rfc_exec_allocator(exec);
    if (!alloc || !bytes || size < 54u || bytes[0] != 'B' ||
        bytes[1] != 'M') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint32_t off = rd32le(bytes + 10u);
    uint32_t w = rd32le(bytes + 18u);
    uint32_t h = rd32le(bytes + 22u);
    size_t row_stride = ((size_t)w * 3u + 3u) & ~3u;
    if (off > size || (size_t)off + row_stride * (size_t)h > size) {
        return RFC_ERR_TRUNCATED_INPUT;
    }

    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, w, h, (size_t)w * 3u, 8u};
    rfc_metadata meta = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0}};
    rfc_frame *fr = (rfc_frame *)alloc->alloc(alloc->ctx, sizeof(*fr));
    if (!fr) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    memset(fr, 0, sizeof(*fr));
    rfc_status st = rfc_exec_alloc_frame(exec, fr, &lay, &meta);
    if (st != RFC_OK) {
        alloc->free(alloc->ctx, fr);
        return st;
    }
    for (uint32_t y = 0u; y < h; y++) {
        const uint8_t *src =
            bytes + off + (size_t)(h - 1u - y) * row_stride;
        uint8_t *dst = fr->data + (size_t)y * (size_t)w * 3u;
        for (uint32_t x = 0u; x < w; x++) {
            dst[x * 3u + 0u] = src[x * 3u + 2u];
            dst[x * 3u + 1u] = src[x * 3u + 1u];
            dst[x * 3u + 2u] = src[x * 3u + 0u];
        }
    }
    *out_frames = fr;
    *out_count = 1u;
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->orientation = RFC_ORIENTATION_NORMAL;
    return RFC_OK;
}

static const rfc_session_adapter g_rf_sess = {
    RFC_CODEC_ABI_VERSION, RFC_FORMAT_RASTERFOLD, tr_sess_open,
    tr_sess_close,         rf_sess_probe,         rf_sess_encode,
    rf_sess_decode,        tr_sess_cancel,        tr_sess_free_encoded};
static const rfc_session_adapter g_bmp_sess = {
    RFC_CODEC_ABI_VERSION, RFC_FORMAT_BMP, tr_sess_open,
    tr_sess_close,         bmp_sess_probe, bmp_sess_encode,
    bmp_sess_decode,       tr_sess_cancel, tr_sess_free_encoded};
static const rfc_session_adapter g_png_sess = {
    RFC_CODEC_ABI_VERSION, RFC_FORMAT_PNG, tr_sess_open,
    tr_sess_close,         png_sess_probe, png_sess_encode,
    png_sess_decode,       tr_sess_cancel, tr_sess_free_encoded};

static void register_test_adapters(void) {
    CHECK(rfc_registry_register_session(&g_rf_sess) == RFC_OK);
    CHECK(rfc_registry_register_session(&g_bmp_sess) == RFC_OK);
    CHECK(rfc_registry_register_session(&g_png_sess) == RFC_OK);
}

static void unregister_test_adapters(void) {
    (void)rfc_registry_unregister_session(RFC_FORMAT_PNG);
    (void)rfc_registry_unregister_session(RFC_FORMAT_BMP);
    (void)rfc_registry_unregister_session(RFC_FORMAT_RASTERFOLD);
}

typedef struct def_bw {
    uint8_t *buf;
    size_t len;
    size_t cap;
    uint64_t bits;
    int cnt;
} def_bw;

static void def_bw_emit(def_bw *bw, uint32_t v, int n) {
    bw->bits |= ((uint64_t)v) << bw->cnt;
    bw->cnt += n;
    while (bw->cnt >= 8) {
        if (bw->len == bw->cap) {
            bw->cap = bw->cap ? bw->cap * 2u : 4096u;
            bw->buf = (uint8_t *)realloc(bw->buf, bw->cap);
            CHECK(bw->buf != NULL);
        }
        bw->buf[bw->len++] = (uint8_t)(bw->bits & 0xffu);
        bw->bits >>= 8;
        bw->cnt -= 8;
    }
}

static uint32_t def_rev(uint32_t v, int n) {
    uint32_t r = 0u;
    for (int i = 0; i < n; i++) {
        r = (r << 1) | ((v >> i) & 1u);
    }
    return r;
}

static void def_emit_lit(def_bw *bw, uint32_t sym) {
    if (sym <= 143u) {
        def_bw_emit(bw, def_rev(0x30u + sym, 8), 8);
    } else if (sym <= 255u) {
        def_bw_emit(bw, def_rev(0x190u + (sym - 144u), 9), 9);
    } else if (sym <= 279u) {
        def_bw_emit(bw, def_rev(sym - 256u, 7), 7);
    } else {
        def_bw_emit(bw, def_rev(0xc0u + (sym - 280u), 8), 8);
    }
}

/* Build a compressed fixed-Huffman Deflate PNG buffer with nontrivial RGB rows
 * (repeating scanline blocks so PNG < BMP while RASTERFOLD < PNG). */
static void make_test_png_rgb(uint32_t w,
                              uint32_t h,
                              uint32_t seed,
                              uint8_t **out_bytes,
                              size_t *out_len) {
    size_t row = 1u + (size_t)w * 3u;
    size_t raw_sz = row * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_sz);
    CHECK(raw != NULL);
    /* Period-24 colorful horizontal strip repeated every 8 rows so PNG
     * compresses cleanly below BMP yet stays larger than RasterFold's CM/palette file. */
    size_t tile_rows = h < 8u ? h : 8u;
    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        uint32_t py = y % (uint32_t)tile_rows;
        r[0] = 0u;
        for (uint32_t x = 0u; x < w; x++) {
            uint32_t px = x % 24u;
            r[1u + x * 3u + 0u] = (uint8_t)((px * 17u + py * 29u + seed) & 0xffu);
            r[1u + x * 3u + 1u] = (uint8_t)((px * 31u + py * 11u + seed * 3u) & 0xffu);
            r[1u + x * 3u + 2u] = (uint8_t)((px * 13u + py * 37u + seed * 7u) & 0xffu);
        }
    }

    def_bw bw;
    memset(&bw, 0, sizeof(bw));
    bw.cap = raw_sz + 64u;
    bw.buf = (uint8_t *)malloc(bw.cap);
    CHECK(bw.buf != NULL);
    bw.buf[bw.len++] = 0x78u;
    bw.buf[bw.len++] = 0x9cu;
    def_bw_emit(&bw, 1u, 1); /* BFINAL=1 */
    def_bw_emit(&bw, 1u, 2); /* BTYPE=01 fixed Huffman */

    /* Emit first 12*row bytes (or full image if small) literally; repeat tile_rows*row via matches */
    static const uint16_t len_base[29] = {
        3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t len_ext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                        1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                        4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dist_base[30] = {
        1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
        33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dist_ext[30] = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,
                                         4, 4, 5, 5, 6, 6, 7,  7,  8,  8,
                                         9, 9, 10,10,11,11,12, 12, 13, 13};

    size_t period = tile_rows * row;
    size_t off = 0u;
    while (off < raw_sz) {
        if (off >= period && period <= 24577u && raw_sz - off >= 3u) {
            size_t take = raw_sz - off;
            if (take > 258u) {
                take = 258u;
            }
            int lidx = 28;
            for (int k = 0; k < 28; k++) {
                if (take >= len_base[k] && take < len_base[k + 1]) {
                    lidx = k;
                    break;
                }
            }
            int didx = 0;
            for (int k = 0; k < 29; k++) {
                if (period >= dist_base[k] && period < dist_base[k + 1]) {
                    didx = k;
                    break;
                }
                didx = 29;
            }
            def_emit_lit(&bw, (uint32_t)(257 + lidx));
            if (len_ext[lidx] > 0) {
                def_bw_emit(&bw, (uint32_t)(take - len_base[lidx]),
                            len_ext[lidx]);
            }
            def_bw_emit(&bw, def_rev((uint32_t)didx, 5), 5);
            if (dist_ext[didx] > 0) {
                def_bw_emit(&bw, (uint32_t)(period - dist_base[didx]),
                            dist_ext[didx]);
            }
            off += take;
        } else {
            def_emit_lit(&bw, raw[off++]);
        }
    }
    def_emit_lit(&bw, 256u);
    if (bw.cnt > 0) {
        def_bw_emit(&bw, 0u, 8 - bw.cnt);
    }

    uint32_t s1 = 1u, s2 = 0u;
    for (size_t i = 0u; i < raw_sz; i++) {
        s1 = (s1 + raw[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    if (bw.len + 4u > bw.cap) {
        bw.cap += 64u;
        bw.buf = (uint8_t *)realloc(bw.buf, bw.cap);
        CHECK(bw.buf != NULL);
    }
    wr32be(bw.buf + bw.len, (s2 << 16) | s1);
    bw.len += 4u;
    free(raw);

    uint8_t *z = bw.buf;
    size_t zp = bw.len;

    size_t png_cap = 8u + 25u + (12u + zp) + 12u;
    uint8_t *png = (uint8_t *)malloc(png_cap);
    CHECK(png != NULL);
    size_t pp = 0u;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(png, sig, 8u);
    pp = 8u;

    /* IHDR */
    wr32be(png + pp, 13u);
    memcpy(png + pp + 4u, "IHDR", 4u);
    wr32be(png + pp + 8u, w);
    wr32be(png + pp + 12u, h);
    png[pp + 16u] = 8u;
    png[pp + 17u] = 2u;
    png[pp + 18u] = 0u;
    png[pp + 19u] = 0u;
    png[pp + 20u] = 0u;
    wr32be(png + pp + 21u, crc32_ieee(png + pp + 4u, 17u));
    pp += 25u;

    /* IDAT */
    wr32be(png + pp, (uint32_t)zp);
    memcpy(png + pp + 4u, "IDAT", 4u);
    memcpy(png + pp + 8u, z, zp);
    wr32be(png + pp + 8u + zp, crc32_ieee(png + pp + 4u, 4u + zp));
    pp += 12u + zp;
    free(z);

    /* IEND */
    wr32be(png + pp, 0u);
    memcpy(png + pp + 4u, "IEND", 4u);
    wr32be(png + pp + 8u, crc32_ieee(png + pp + 4u, 4u));
    pp += 12u;

    *out_bytes = png;
    *out_len = pp;
}

static void make_test_bmp_rgb(uint32_t w,
                              uint32_t h,
                              uint32_t seed,
                              uint8_t **out_bytes,
                              size_t *out_len) {
    size_t row_stride = ((size_t)w * 3u + 3u) & ~3u;
    size_t total = 54u + row_stride * (size_t)h;
    uint8_t *out = (uint8_t *)calloc(1u, total);
    CHECK(out != NULL);
    out[0] = 'B';
    out[1] = 'M';
    wr32le(out + 2u, (uint32_t)total);
    wr32le(out + 10u, 54u);
    wr32le(out + 14u, 40u);
    wr32le(out + 18u, w);
    wr32le(out + 22u, h);
    wr16le(out + 26u, 1u);
    wr16le(out + 28u, 24u);
    wr32le(out + 34u, (uint32_t)(row_stride * (size_t)h));
    wr32le(out + 38u, 2835u);
    wr32le(out + 42u, 2835u);
    for (uint32_t y = 0u; y < h; y++) {
        uint8_t *dst = out + 54u + (size_t)(h - 1u - y) * row_stride;
        for (uint32_t x = 0u; x < w; x++) {
            dst[x * 3u + 2u] = (uint8_t)((x * 13u + y * 29u + seed) & 0xffu);
            dst[x * 3u + 1u] = (uint8_t)((x * 37u + y * 7u + seed) & 0xffu);
            dst[x * 3u + 0u] = (uint8_t)((x * 11u + y * 19u + seed) & 0xffu);
        }
    }
    *out_bytes = out;
    *out_len = total;
}

/* ----------------------------------------------------------------
 * Callback publisher fixture (supports lying truncate / bit-flip / reader-close order)
 * ---------------------------------------------------------------- */

typedef struct cb_pub_ctx {
    uint8_t *stored;
    size_t stored_len;
    int truncate_one_byte;
    int flip_one_byte;
    int reader_closed_count;
    int release_called_count;
    int closed_before_release;
} cb_pub_ctx;

typedef struct cb_pub_stage {
    uint8_t *buf;
    size_t len;
    size_t cap;
} cb_pub_stage;

typedef struct cb_rdr_ctx {
    cb_pub_ctx *owner;
    size_t pos;
} cb_rdr_ctx;

static rfc_status cb_rdr_size(void *raw, int *out_known, uint64_t *out_sz) {
    cb_rdr_ctx *r = (cb_rdr_ctx *)raw;
    *out_known = 1;
    *out_sz = (uint64_t)r->owner->stored_len;
    return RFC_OK;
}

static rfc_status cb_rdr_seek(void *raw, uint64_t off) {
    cb_rdr_ctx *r = (cb_rdr_ctx *)raw;
    if (off > (uint64_t)r->owner->stored_len) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    r->pos = (size_t)off;
    return RFC_OK;
}

static rfc_status cb_rdr_read(void *raw,
                              uint8_t *buf,
                              size_t max_bytes,
                              size_t *out_read) {
    cb_rdr_ctx *r = (cb_rdr_ctx *)raw;
    size_t rem = r->owner->stored_len - r->pos;
    size_t take = rem < max_bytes ? rem : max_bytes;
    if (take > 0u) {
        memcpy(buf, r->owner->stored + r->pos, take);
        r->pos += take;
    }
    *out_read = take;
    return RFC_OK;
}

static void cb_rdr_close(void *raw) {
    cb_rdr_ctx *r = (cb_rdr_ctx *)raw;
    if (r) {
        r->owner->reader_closed_count++;
        if (r->owner->release_called_count == 0) {
            r->owner->closed_before_release = 1;
        }
        free(r);
    }
}

static rfc_status cb_pub_begin(void *raw_ctx,
                               rfc_job_id job_id,
                               uint64_t attempt,
                               uint64_t expected_bytes,
                               const rfc_file_identity *src_id,
                               void **out_sh) {
    (void)raw_ctx;
    (void)job_id;
    (void)attempt;
    (void)expected_bytes;
    (void)src_id;
    cb_pub_stage *sh = (cb_pub_stage *)calloc(1u, sizeof(*sh));
    CHECK(sh != NULL);
    *out_sh = sh;
    return RFC_OK;
}

static rfc_status cb_pub_write(void *raw_ctx,
                               void *raw_sh,
                               const uint8_t *data,
                               size_t len) {
    (void)raw_ctx;
    cb_pub_stage *sh = (cb_pub_stage *)raw_sh;
    if (len == 0u) {
        return RFC_OK;
    }
    if (sh->len + len > sh->cap) {
        size_t ncap = sh->cap ? sh->cap : 4096u;
        while (ncap < sh->len + len) ncap *= 2u;
        uint8_t *nb = (uint8_t *)realloc(sh->buf, ncap);
        CHECK(nb != NULL);
        sh->buf = nb;
        sh->cap = ncap;
    }
    memcpy(sh->buf + sh->len, data, len);
    sh->len += len;
    return RFC_OK;
}

static rfc_status cb_pub_flush(void *raw_ctx, void *raw_sh) {
    (void)raw_ctx;
    (void)raw_sh;
    return RFC_OK;
}

static rfc_status cb_pub_commit(void *raw_ctx,
                                void *raw_sh,
                                int *out_cleaned) {
    cb_pub_ctx *pc = (cb_pub_ctx *)raw_ctx;
    cb_pub_stage *sh = (cb_pub_stage *)raw_sh;
    free(pc->stored);
    pc->stored = sh->buf;
    pc->stored_len = sh->len;
    free(sh);
    if (pc->truncate_one_byte && pc->stored_len > 0u) {
        pc->stored_len--;
    }
    if (pc->flip_one_byte && pc->stored_len > 0u) {
        pc->stored[pc->stored_len / 2u] ^= 0x01u;
    }
    if (out_cleaned) {
        *out_cleaned = 1;
    }
    return RFC_OK;
}

static rfc_status cb_pub_abort(void *raw_ctx, void *raw_sh) {
    (void)raw_ctx;
    cb_pub_stage *sh = (cb_pub_stage *)raw_sh;
    if (sh) {
        free(sh->buf);
        free(sh);
    }
    return RFC_OK;
}

static void cb_pub_release(void *raw_ctx) {
    cb_pub_ctx *pc = (cb_pub_ctx *)raw_ctx;
    pc->release_called_count++;
}

static rfc_status cb_pub_open_reader(void *raw_ctx,
                                     rfc_job_id jid,
                                     uint64_t attempt,
                                     rfc_io_reader *out_rdr) {
    (void)jid;
    (void)attempt;
    cb_pub_ctx *pc = (cb_pub_ctx *)raw_ctx;
    cb_rdr_ctx *r = (cb_rdr_ctx *)calloc(1u, sizeof(*r));
    CHECK(r != NULL);
    r->owner = pc;
    memset(out_rdr, 0, sizeof(*out_rdr));
    out_rdr->ctx = r;
    out_rdr->size_fn = cb_rdr_size;
    out_rdr->read_fn = cb_rdr_read;
    out_rdr->seek_fn = cb_rdr_seek;
    out_rdr->close_fn = cb_rdr_close;
    return RFC_OK;
}

static rfc_io_publisher make_cb_pub(cb_pub_ctx *pc, int with_reader) {
    rfc_io_publisher p;
    memset(&p, 0, sizeof(p));
    p.ctx = pc;
    p.begin_stage = cb_pub_begin;
    p.write_stage = cb_pub_write;
    p.flush_stage = cb_pub_flush;
    p.commit_stage = cb_pub_commit;
    p.abort_stage = cb_pub_abort;
    p.release_ctx = cb_pub_release;
    p.open_published_reader = with_reader ? cb_pub_open_reader : NULL;
    return p;
}

/* ----------------------------------------------------------------
 * Test 1: Stage timings and retry isolation
 * ---------------------------------------------------------------- */

static rfc_status stage_fail_import(rfc_job *j,
                                    const rfc_exec_context *e,
                                    void *u) {
    (void)j;
    (void)e;
    (void)u;
    return RFC_ERR_INVALID_ARGUMENT;
}

static void test_stage_timings_and_retry(void) {
    uint8_t *bmp = NULL;
    size_t blen = 0u;
    make_test_bmp_rgb(32u, 32u, 11u, &bmp, &blen);

    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "timings");
    char dst1[1024], dst2[1024];
    join_path(dst1, sizeof(dst1), tmp, "out1.fold");
    join_path(dst2, sizeof(dst2), tmp, "out2.fold");

    rfc_scheduler *sched = rfc_scheduler_create(NULL);
    CHECK(sched != NULL);

    rfc_layout lay = {RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 32u, 32u, 96u, 8u};
    rfc_io_sink s1;
    rfc_io_sink_init_path(&s1, dst1, RFC_EXPORT_REFUSE_EXISTING);

    rfc_job_spec sp1;
    memset(&sp1, 0, sizeof(sp1));
    sp1.source_format = RFC_FORMAT_BMP;
    sp1.target_format = RFC_FORMAT_RASTERFOLD;
    sp1.source_bytes = bmp;
    sp1.source_size = blen;
    sp1.source_layout = &lay;
    sp1.io_sink = &s1;

    rfc_job *job1 = NULL;
    CHECK(rfc_scheduler_submit(sched, NULL, &sp1, &job1) == RFC_OK);
    CHECK(rfc_job_wait(job1, 0xFFFFFFFFu) == RFC_OK);

    rfc_stage_timings t1;
    CHECK(rfc_job_stage_timings_get(job1, &t1) == RFC_OK);
    CHECK(t1.valid == 1 && t1.import_ran == 1 && t1.encode_ran == 1 &&
          t1.verify_ran == 1 && t1.export_ran == 1);
    CHECK(t1.wall_ns >=
          t1.import_ns + t1.encode_ns + t1.verify_ns + t1.export_ns);
    CHECK(rfc_job_stage_timings_get(NULL, &t1) == RFC_ERR_INVALID_ARGUMENT);
    rfc_job_release(job1);

    /* Retry slot isolation: attempt 1 fails in IMPORT, retry clears/isolates timings. */
    rfc_job_slot *slot = rfc_job_slot_create();
    CHECK(slot != NULL);

    rfc_job_spec sp2;
    memset(&sp2, 0, sizeof(sp2));
    sp2.source_format = RFC_FORMAT_BMP;
    sp2.target_format = RFC_FORMAT_RASTERFOLD;
    sp2.source_bytes = bmp;
    sp2.source_size = blen;
    sp2.source_layout = &lay;
    sp2.has_stage_ops = 1;
    sp2.stage_ops.on_import = stage_fail_import;

    rfc_job *att1 = NULL;
    CHECK(rfc_scheduler_submit(sched, slot, &sp2, &att1) == RFC_OK);
    CHECK(rfc_job_wait(att1, 0xFFFFFFFFu) == RFC_OK);
    rfc_stage_timings ta1;
    CHECK(rfc_job_stage_timings_get(att1, &ta1) == RFC_OK);
    CHECK(ta1.import_ran == 1 && ta1.encode_ran == 0 && ta1.verify_ran == 0 &&
          ta1.export_ran == 0);
    rfc_job_release(att1);

    rfc_job *att2 = NULL;
    CHECK(rfc_scheduler_retry(sched, slot, &att2) == RFC_OK);
    CHECK(rfc_job_wait(att2, 0xFFFFFFFFu) == RFC_OK);
    rfc_stage_timings ta2;
    CHECK(rfc_job_stage_timings_get(att2, &ta2) == RFC_OK);
    CHECK(ta2.import_ran == 1 && ta2.encode_ran == 0 && ta2.verify_ran == 0);
    rfc_job_release(att2);

    /* Clean valid run on slot */
    rfc_io_sink s2;
    rfc_io_sink_init_path(&s2, dst2, RFC_EXPORT_REFUSE_EXISTING);
    memset(&sp2, 0, sizeof(sp2));
    sp2.source_format = RFC_FORMAT_BMP;
    sp2.target_format = RFC_FORMAT_RASTERFOLD;
    sp2.source_bytes = bmp;
    sp2.source_size = blen;
    sp2.source_layout = &lay;
    sp2.io_sink = &s2;
    rfc_job *att3 = NULL;
    CHECK(rfc_scheduler_submit(sched, slot, &sp2, &att3) == RFC_OK);
    CHECK(rfc_job_wait(att3, 0xFFFFFFFFu) == RFC_OK);
    rfc_stage_timings ta3;
    CHECK(rfc_job_stage_timings_get(att3, &ta3) == RFC_OK);
    CHECK(ta3.import_ran == 1 && ta3.encode_ran == 1 && ta3.verify_ran == 1 &&
          ta3.export_ran == 1);
    rfc_job_release(att3);

    rfc_job_slot_destroy(slot);
    rfc_scheduler_destroy(sched);
    rm_tree(tmp);
    free(bmp);
    printf("reports: stage_timings_and_retry ok\n");
}

/* ----------------------------------------------------------------
 * Test 2: Size outcomes (equal-size vs identical-bytes) & callback read-back
 * ---------------------------------------------------------------- */

static rfc_status equal_diff_encode(void *raw,
                                    const rfc_exec_context *exec,
                                    const rfc_frame *frames,
                                    size_t frame_count,
                                    const rfc_metadata *meta,
                                    const rfc_setting_value *settings,
                                    size_t setting_count,
                                    rfc_encoded *out_enc) {
    (void)raw;
    (void)meta;
    (void)settings;
    (void)setting_count;
    const rfc_allocator *alloc = rfc_exec_allocator(exec);
    size_t row_stride = ((size_t)frames[0].layout.width * 3u + 3u) & ~3u;
    size_t sz = 54u + row_stride * (size_t)frames[0].layout.height;
    /* Emit valid BMP with identical byte count to source BMP, but flip an
     * unused dib-header padding/XPels byte OR use BMP->BMP identical bytes in separate test!
     * For equal-size different-SHA-256: write same-length header with different XPelsPerMeter (stays pixel-identical)! */
    rfc_encoded tmp;
    rfc_status st = bmp_sess_encode(raw, exec, frames, frame_count, meta,
                                    settings, setting_count, &tmp);
    if (st != RFC_OK) {
        return st;
    }
    CHECK(tmp.len == sz);
    wr32le(tmp.data + 38u, 9999u); /* different XPelsPerMeter, identical pixels & length */
    *out_enc = tmp;
    (void)alloc;
    return RFC_OK;
}

static void test_size_outcomes_and_callback_readback(void) {
    /* Direct classification tests for all 5 outcomes */
    uint8_t ha[32] = {1};
    uint8_t hb[32] = {2};
    rfc_size_metrics sm;
    CHECK(rfc_metrics_classify_size(1000u, 3000u, 700u, ha, 1, hb, 1, &sm) ==
          RFC_OK);
    CHECK(sm.outcome == RFC_SIZE_SMALLER && sm.savings_bytes == 300u &&
          sm.growth_bytes == 0u && sm.signed_delta_bytes == -300);

    CHECK(rfc_metrics_classify_size(1000u, 3000u, 1000u, ha, 1, hb, 1, &sm) ==
          RFC_OK);
    CHECK(sm.outcome == RFC_SIZE_EQUAL_SIZE && sm.savings_bytes == 0u &&
          sm.growth_bytes == 0u && sm.signed_delta_bytes == 0);

    CHECK(rfc_metrics_classify_size(1000u, 3000u, 1000u, ha, 0, ha, 1, &sm) ==
          RFC_OK);
    CHECK(sm.outcome == RFC_SIZE_EQUAL_SIZE);

    CHECK(rfc_metrics_classify_size(1000u, 3000u, 1000u, ha, 1, ha, 1, &sm) ==
          RFC_OK);
    CHECK(sm.outcome == RFC_SIZE_IDENTICAL_BYTES);

    CHECK(rfc_metrics_classify_size(1000u, 3000u, 1450u, ha, 1, hb, 1, &sm) ==
          RFC_OK);
    CHECK(sm.outcome == RFC_SIZE_GROWTH && sm.growth_bytes == 450u &&
          sm.savings_bytes == 0u && sm.signed_delta_bytes == 450);

    CHECK(strcmp(rfc_size_outcome_name(RFC_SIZE_NOT_APPLICABLE),
                 "not-applicable") == 0);
    CHECK(strcmp(rfc_size_outcome_name(RFC_SIZE_EQUAL_SIZE), "equal-size") == 0);
    CHECK(strcmp(rfc_size_outcome_name(RFC_SIZE_IDENTICAL_BYTES),
                 "identical-bytes") == 0);

    /* Real compare runs over BMP -> BMP callback publishers */
    uint8_t *bmp = NULL;
    size_t blen = 0u;
    make_test_bmp_rgb(32u, 32u, 55u, &bmp, &blen);
    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "cbread");

    /* 1. NULL open_published_reader refused cleanly */
    cb_pub_ctx pc_null;
    memset(&pc_null, 0, sizeof(pc_null));
    rfc_io_publisher pub_null = make_cb_pub(&pc_null, 0);

    /* 2. Lying truncating callback publisher refused with RFC_ERR_VERIFY_MISMATCH */
    cb_pub_ctx pc_trunc;
    memset(&pc_trunc, 0, sizeof(pc_trunc));
    pc_trunc.truncate_one_byte = 1;
    rfc_io_publisher pub_trunc = make_cb_pub(&pc_trunc, 1);

    /* 3. Lying 1-byte bit-flipping callback publisher refused with RFC_ERR_VERIFY_MISMATCH */
    cb_pub_ctx pc_flip;
    memset(&pc_flip, 0, sizeof(pc_flip));
    pc_flip.flip_one_byte = 1;
    rfc_io_publisher pub_flip = make_cb_pub(&pc_flip, 1);

    /* 4. Truthful callback publisher producing exact identical BMP bytes -> IDENTICAL_BYTES */
    cb_pub_ctx pc_ident;
    memset(&pc_ident, 0, sizeof(pc_ident));
    rfc_io_publisher pub_ident = make_cb_pub(&pc_ident, 1);

    rfc_compare_candidate_spec cands[4];
    memset(cands, 0, sizeof(cands));
    cands[0].target_format = RFC_FORMAT_BMP;
    cands[0].publisher = &pub_null;
    cands[1].target_format = RFC_FORMAT_BMP;
    cands[1].publisher = &pub_trunc;
    cands[2].target_format = RFC_FORMAT_BMP;
    cands[2].publisher = &pub_flip;
    cands[3].target_format = RFC_FORMAT_BMP;
    cands[3].publisher = &pub_ident;

    rfc_compare_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_bytes = bmp;
    sp.source_size = blen;
    sp.source_format = RFC_FORMAT_BMP;
    sp.staging_dir_utf8 = tmp;
    sp.requested_threads = 1u;
    sp.metadata_policy = RFC_META_POLICY_PRESERVE;
    sp.warmup_iterations = 0u;
    sp.measured_iterations = 1u;
    sp.candidates = cands;
    sp.candidate_count = 4u;
    sp.run_id = "cb-readback";
    sp.recorded_utc = "2026-11-16T05:00:00Z";

    rfc_comparison_report rep;
    CHECK(rfc_compare_run(&sp, &rep) == RFC_OK);
    CHECK(rep.failed_count == 3u && rep.completed_count == 1u);
    CHECK(rep.candidates[0].outcome == RFC_CANDIDATE_FAILED &&
          rep.candidates[0].failing_stage == RFC_STAGE_PREFLIGHT &&
          rep.candidates[0].status == RFC_ERR_NO_ADAPTER &&
          rep.candidates[0].rank == 0u && !rep.candidates[0].has_produced_output);
    CHECK(rep.candidates[1].outcome == RFC_CANDIDATE_FAILED &&
          rep.candidates[1].failing_stage == RFC_STAGE_VERIFY &&
          rep.candidates[1].status == RFC_ERR_VERIFY_MISMATCH &&
          rep.candidates[1].rank == 0u &&
          rep.candidates[1].has_produced_output == 1 &&
          pc_trunc.stored != NULL);
    CHECK(rep.candidates[2].outcome == RFC_CANDIDATE_FAILED &&
          rep.candidates[2].failing_stage == RFC_STAGE_VERIFY &&
          rep.candidates[2].status == RFC_ERR_VERIFY_MISMATCH &&
          rep.candidates[2].rank == 0u &&
          rep.candidates[2].has_produced_output == 1 &&
          pc_flip.stored != NULL);
    CHECK(rep.candidates[3].outcome == RFC_CANDIDATE_COMPLETED &&
          rep.candidates[3].status == RFC_OK &&
          rep.candidates[3].rank == 1u &&
          rep.candidates[3].has_produced_output == 1 &&
          rep.candidates[3].size.outcome == RFC_SIZE_IDENTICAL_BYTES);
    CHECK(rfc_report_validate(&rep) == RFC_OK);
    CHECK(pc_ident.reader_closed_count == 1 &&
          pc_ident.release_called_count == 1 && pc_ident.closed_before_release);

    char cb_hist[1024];
    join_path(cb_hist, sizeof(cb_hist), tmp, "cb_readback.jsonl");
    CHECK(rfc_history_append_path(cb_hist, &rep) == RFC_OK);
    rfc_history_log cb_hlog;
    CHECK(rfc_history_read_path(cb_hist, &cb_hlog) == RFC_OK);
    CHECK(cb_hlog.entry_count == 1u && cb_hlog.entries[0].candidate_count == 4u);
    CHECK(cb_hlog.entries[0].candidates[0].outcome == RFC_CANDIDATE_FAILED &&
          cb_hlog.entries[0].candidates[0].failing_stage == RFC_STAGE_PREFLIGHT &&
          cb_hlog.entries[0].candidates[0].has_produced_output == 0);
    CHECK(cb_hlog.entries[0].candidates[1].outcome == RFC_CANDIDATE_FAILED &&
          cb_hlog.entries[0].candidates[1].failing_stage == RFC_STAGE_VERIFY &&
          cb_hlog.entries[0].candidates[1].has_produced_output == 1);
    CHECK(cb_hlog.entries[0].candidates[2].outcome == RFC_CANDIDATE_FAILED &&
          cb_hlog.entries[0].candidates[2].failing_stage == RFC_STAGE_VERIFY &&
          cb_hlog.entries[0].candidates[2].has_produced_output == 1);
    rfc_history_log_free(&cb_hlog);

    /* Separate test proving equal-length different-digest completes as EQUAL_SIZE (not IDENTICAL_BYTES) */
    cb_pub_ctx pc_eq;
    memset(&pc_eq, 0, sizeof(pc_eq));
    rfc_io_publisher pub_eq = make_cb_pub(&pc_eq, 1);

    const rfc_session_adapter *orig_bmp =
        rfc_registry_session_adapter(RFC_FORMAT_BMP);
    CHECK(orig_bmp != NULL);
    rfc_session_adapter eq_bmp_sess = *orig_bmp;
    eq_bmp_sess.encode_session = equal_diff_encode;
    CHECK(rfc_registry_unregister_session(RFC_FORMAT_BMP) == RFC_OK);
    CHECK(rfc_registry_register_session(&eq_bmp_sess) == RFC_OK);

    rfc_compare_candidate_spec ceq;
    memset(&ceq, 0, sizeof(ceq));
    ceq.target_format = RFC_FORMAT_BMP;
    ceq.publisher = &pub_eq;
    sp.candidates = &ceq;
    sp.candidate_count = 1u;
    sp.run_id = "cb-equal-size";
    CHECK(rfc_compare_run(&sp, &rep) == RFC_OK);
    CHECK(rep.completed_count == 1u);
    CHECK(rep.candidates[0].size.produced_file_bytes ==
          rep.candidates[0].size.source_file_bytes);
    CHECK(rep.candidates[0].size.outcome == RFC_SIZE_EQUAL_SIZE);
    CHECK(memcmp(rep.candidates[0].size.source_sha256,
                 rep.candidates[0].size.produced_sha256, 32u) != 0);

    CHECK(rfc_registry_unregister_session(RFC_FORMAT_BMP) == RFC_OK);
    CHECK(rfc_registry_register_session(orig_bmp) == RFC_OK);

    free(pc_null.stored);
    free(pc_trunc.stored);
    free(pc_flip.stored);
    free(pc_ident.stored);
    free(pc_eq.stored);
    rm_tree(tmp);
    free(bmp);
    printf("reports: size_outcomes_and_callback_readback ok\n");
}

/* ----------------------------------------------------------------
 * Test 3: Sequential compare, warmup exclusion, growth & unsupported states
 * ---------------------------------------------------------------- */

static void test_sequential_compare_and_ranking(void) {
    uint8_t *png = NULL;
    size_t plen = 0u;
    make_test_png_rgb(64u, 64u, 23u, &png, &plen);

    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "seqcmp");
    char src_path[1024], fold_out[1024], bmp_out[1024];
    join_path(src_path, sizeof(src_path), tmp, "src.png");
    join_path(fold_out, sizeof(fold_out), tmp, "out.fold");
    join_path(bmp_out, sizeof(bmp_out), tmp, "out.bmp");
    write_file_all(src_path, png, plen);

    rfc_io_source src_io;
    rfc_io_source_init_path(&src_io, src_path, RFC_IMPORT_DEFAULT);

    rfc_compare_candidate_spec cands[4];
    memset(cands, 0, sizeof(cands));
    cands[0].target_format = RFC_FORMAT_WEBP; /* unsupported -> rank 0 */
    cands[1].target_format = RFC_FORMAT_BMP;  /* growth -> rank 2 */
    cands[1].dest_path_utf8 = bmp_out;
    cands[2].target_format = RFC_FORMAT_RASTERFOLD; /* smaller -> rank 1 */
    cands[2].dest_path_utf8 = fold_out;
    cands[3].target_format = RFC_FORMAT_HEIC; /* import-only -> rank 0 */

    atomic_store(&g_active_encodes, 0);
    atomic_store(&g_max_concurrent_encodes, 0);

    rfc_compare_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.io_source = &src_io;
    sp.staging_dir_utf8 = tmp;
    sp.requested_threads = 1u;
    sp.metadata_policy = RFC_META_POLICY_PRESERVE;
    sp.warmup_iterations = 1u;
    sp.measured_iterations = 3u;
    sp.candidates = cands;
    sp.candidate_count = 4u;
    sp.run_id = "seq-ranking";
    sp.recorded_utc = "2026-11-16T05:05:00Z";

    rfc_comparison_report rep;
    CHECK(rfc_compare_run(&sp, &rep) == RFC_OK);
    CHECK(atomic_load(&g_max_concurrent_encodes) == 1);
    CHECK(rep.run_status == RFC_OK && rep.ranking_complete == 1 && !rep.cancelled);
    CHECK(rep.source_format == RFC_FORMAT_PNG);
    CHECK(rep.source_file_bytes == plen);
    CHECK(rep.raw_frame_bytes == 64ull * 64ull * 3ull);
    CHECK(rep.requested_threads == 1u && rep.effective_threads == 1u);
    CHECK(rep.completed_count == 2u && rep.unsupported_count == 2u &&
          rep.failed_count == 0u && rep.cancelled_count == 0u);

    /* WEBP unsupported -> rank 0, no output */
    CHECK(rep.candidates[0].outcome == RFC_CANDIDATE_UNSUPPORTED);
    CHECK(rep.candidates[0].rank == 0u &&
          rep.candidates[0].has_produced_output == 0);
    CHECK(rep.candidates[0].size.outcome == RFC_SIZE_NOT_APPLICABLE);

    /* BMP growth -> rank 2 */
    CHECK(rep.candidates[1].outcome == RFC_CANDIDATE_COMPLETED);
    CHECK(rep.candidates[1].rank == 2u &&
          rep.candidates[1].has_produced_output == 1);
    CHECK(rep.candidates[1].warmup_completed == 1u &&
          rep.candidates[1].measured_completed == 3u);
    CHECK(rep.candidates[1].size.outcome == RFC_SIZE_GROWTH);
    CHECK(rep.candidates[1].size.growth_bytes > 0u);
    CHECK(rep.candidates[1].min_timings.wall_ns <=
          rep.candidates[1].median_timings.wall_ns);

    /* FOLD smaller -> rank 1 */
    CHECK(rep.candidates[2].outcome == RFC_CANDIDATE_COMPLETED);
    CHECK(rep.candidates[2].rank == 1u &&
          rep.candidates[2].has_produced_output == 1);
    CHECK(rep.candidates[2].size.produced_file_bytes <
          rep.candidates[1].size.produced_file_bytes);

    /* HEIC import-only unsupported -> rank 0 */
    CHECK(rep.candidates[3].outcome == RFC_CANDIDATE_UNSUPPORTED);
    CHECK(rep.candidates[3].status == RFC_ERR_UNSUPPORTED_COMBINATION);
    CHECK(rep.candidates[3].rank == 0u &&
          rep.candidates[3].has_produced_output == 0);

    CHECK(!dir_has_prefix(tmp, ".rfc-cmp."));
    rm_tree(tmp);
    free(png);
    printf("reports: sequential_compare_and_ranking ok\n");
}

/* ----------------------------------------------------------------
 * Test 4: Cancellation (warmup / measurement / between candidates) & partial failure
 * ---------------------------------------------------------------- */

typedef struct cancel_inject_state {
    rfc_compare_token *tok;
    int mode; /* 1=warmup, 2=measured-iter-1, 3=between-cand, 4=fail-iter-1 */
    int encode_calls;
} cancel_inject_state;

static cancel_inject_state g_ci;

static rfc_status ci_rf_encode_hook(void *sess,
                                    const rfc_exec_context *exec,
                                    const rfc_frame *frames,
                                    size_t frame_count,
                                    const rfc_metadata *meta,
                                    const rfc_setting_value *settings,
                                    size_t setting_count,
                                    rfc_encoded *out_enc) {
    (void)sess;
    (void)exec;
    (void)frames;
    (void)frame_count;
    (void)meta;
    (void)settings;
    (void)setting_count;
    (void)out_enc;
    g_ci.encode_calls++;
    if (g_ci.mode == 1 && g_ci.encode_calls == 1) {
        rfc_compare_cancel(g_ci.tok);
        return RFC_ERR_CANCELLED;
    }
    if (g_ci.mode == 2 && g_ci.encode_calls == 3) {
        /* 1 warmup + measured 0 succeeded; cancel during measured 1 */
        rfc_compare_cancel(g_ci.tok);
        return RFC_ERR_CANCELLED;
    }
    if (g_ci.mode == 4 && g_ci.encode_calls == 2) {
        /* Measured 0 succeeded; measured 1 fails */
        return RFC_ERR_IO;
    }
    g_custom_rf_encode_hook = NULL;
    rfc_status st = rf_sess_encode(sess, exec, frames, frame_count, meta,
                                   settings, setting_count, out_enc);
    g_custom_rf_encode_hook = ci_rf_encode_hook;
    return st;
}

static void ci_between_hook(rfc_compare_hook_phase phase,
                            size_t cand_idx,
                            uint32_t iter_idx,
                            rfc_compare_token *tok,
                            void *user) {
    (void)iter_idx;
    cancel_inject_state *st = (cancel_inject_state *)user;
    if (st->mode == 3 && phase == RFC_COMPARE_HOOK_BEFORE_CANDIDATE &&
        cand_idx == 1u && tok) {
        rfc_compare_cancel(tok);
    }
}

static void test_cancel_and_partial_iterations(void) {
    uint8_t *png = NULL;
    size_t plen = 0u;
    make_test_png_rgb(48u, 48u, 71u, &png, &plen);

    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "cancmp");

    rfc_compare_candidate_spec cands[3];
    char d0[1024], d1[1024], d2[1024];

    /* Case A: cancel during warm-up */
    join_path(d0, sizeof(d0), tmp, "a0.fold");
    join_path(d1, sizeof(d1), tmp, "a1.bmp");
    memset(cands, 0, sizeof(cands));
    cands[0].target_format = RFC_FORMAT_RASTERFOLD;
    cands[0].dest_path_utf8 = d0;
    cands[1].target_format = RFC_FORMAT_BMP;
    cands[1].dest_path_utf8 = d1;

    rfc_compare_token *tokA = NULL;
    CHECK(rfc_compare_token_create(&tokA) == RFC_OK);
    memset(&g_ci, 0, sizeof(g_ci));
    g_ci.tok = tokA;
    g_ci.mode = 1;
    g_custom_rf_encode_hook = ci_rf_encode_hook;

    rfc_compare_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_bytes = png;
    sp.source_size = plen;
    sp.source_format = RFC_FORMAT_PNG;
    sp.staging_dir_utf8 = tmp;
    sp.requested_threads = 1u;
    sp.metadata_policy = RFC_META_POLICY_PRESERVE;
    sp.warmup_iterations = 1u;
    sp.measured_iterations = 2u;
    sp.candidates = cands;
    sp.candidate_count = 2u;
    sp.cancel_token = tokA;
    sp.run_id = "cancel-warmup";
    sp.recorded_utc = "2026-11-16T05:06:00Z";

    rfc_comparison_report rep;
    CHECK(rfc_compare_run(&sp, &rep) == RFC_ERR_CANCELLED);
    g_custom_rf_encode_hook = NULL;
    rfc_compare_token_destroy(tokA);
    CHECK(rep.cancelled == 1 && rep.ranking_complete == 0 &&
          rep.cancelled_count == 2u);
    CHECK(rep.candidates[0].outcome == RFC_CANDIDATE_CANCELLED &&
          rep.candidates[0].was_started == 1 &&
          !rep.candidates[0].has_produced_output && rep.candidates[0].rank == 0u);
    CHECK(rep.candidates[1].outcome == RFC_CANDIDATE_CANCELLED &&
          rep.candidates[1].was_started == 0 && rep.candidates[1].rank == 0u);
    CHECK(!file_exists_utf8(d0) && !file_exists_utf8(d1) &&
          !dir_has_prefix(tmp, ".rfc-cmp."));

    /* Case B: cancel during measured iter 1 after warmup and measured 0 succeeded */
    join_path(d0, sizeof(d0), tmp, "b0.fold");
    join_path(d1, sizeof(d1), tmp, "b1.bmp");
    rfc_compare_token *tokB = NULL;
    CHECK(rfc_compare_token_create(&tokB) == RFC_OK);
    memset(&g_ci, 0, sizeof(g_ci));
    g_ci.tok = tokB;
    g_ci.mode = 2;
    g_custom_rf_encode_hook = ci_rf_encode_hook;
    sp.cancel_token = tokB;
    sp.measured_iterations = 3u;
    sp.run_id = "cancel-measured";
    CHECK(rfc_compare_run(&sp, &rep) == RFC_ERR_CANCELLED);
    g_custom_rf_encode_hook = NULL;
    rfc_compare_token_destroy(tokB);
    CHECK(rep.cancelled == 1 && rep.ranking_complete == 0);
    CHECK(rep.candidates[0].outcome == RFC_CANDIDATE_CANCELLED &&
          !rep.candidates[0].has_produced_output && rep.candidates[0].rank == 0u);
    CHECK(rep.candidates[1].outcome == RFC_CANDIDATE_CANCELLED &&
          rep.candidates[1].was_started == 0);
    CHECK(!file_exists_utf8(d0) && !dir_has_prefix(tmp, ".rfc-cmp."));

    /* Case C: cancel between candidate 0 completion and candidate 1 start */
    join_path(d0, sizeof(d0), tmp, "c0.fold");
    join_path(d1, sizeof(d1), tmp, "c1.bmp");
    join_path(d2, sizeof(d2), tmp, "c2.bmp");
    cands[2].target_format = RFC_FORMAT_BMP;
    cands[2].dest_path_utf8 = d2;
    rfc_compare_token *tokC = NULL;
    CHECK(rfc_compare_token_create(&tokC) == RFC_OK);
    memset(&g_ci, 0, sizeof(g_ci));
    g_ci.tok = tokC;
    g_ci.mode = 3;
    rfc_compare_set_hook_for_testing(ci_between_hook, &g_ci);
    sp.candidate_count = 3u;
    sp.warmup_iterations = 0u;
    sp.measured_iterations = 1u;
    sp.cancel_token = tokC;
    sp.run_id = "cancel-between";
    CHECK(rfc_compare_run(&sp, &rep) == RFC_ERR_CANCELLED);
    rfc_compare_set_hook_for_testing(NULL, NULL);
    rfc_compare_token_destroy(tokC);
    CHECK(rep.cancelled == 1 && rep.ranking_complete == 0);
    CHECK(rep.completed_count == 1u && rep.cancelled_count == 2u);
    CHECK(rep.candidates[0].outcome == RFC_CANDIDATE_COMPLETED &&
          rep.candidates[0].has_produced_output == 1 &&
          rep.candidates[0].rank == 0u);
    CHECK(file_exists_utf8(d0));
    CHECK(rep.candidates[1].outcome == RFC_CANDIDATE_CANCELLED &&
          rep.candidates[1].was_started == 0 && rep.candidates[1].rank == 0u);
    CHECK(rep.candidates[2].outcome == RFC_CANDIDATE_CANCELLED &&
          rep.candidates[2].was_started == 0 && rep.candidates[2].rank == 0u);
    CHECK(!file_exists_utf8(d1) && !dir_has_prefix(tmp, ".rfc-cmp."));

    /* Case D: partial iteration failure (iter 2 fails after iter 1 succeeded -> failed, continue) */
    join_path(d0, sizeof(d0), tmp, "d0.fold");
    join_path(d1, sizeof(d1), tmp, "d1.bmp");
    memset(&g_ci, 0, sizeof(g_ci));
    g_ci.mode = 4;
    g_custom_rf_encode_hook = ci_rf_encode_hook;
    sp.cancel_token = NULL;
    sp.candidate_count = 2u;
    sp.warmup_iterations = 0u;
    sp.measured_iterations = 2u;
    sp.run_id = "partial-iter-fail";
    CHECK(rfc_compare_run(&sp, &rep) == RFC_OK);
    g_custom_rf_encode_hook = NULL;
    CHECK(rep.ranking_complete == 1 && rep.failed_count == 1u &&
          rep.completed_count == 1u);
    CHECK(rep.candidates[0].outcome == RFC_CANDIDATE_FAILED &&
          !rep.candidates[0].has_produced_output &&
          rep.candidates[0].rank == 0u);
    CHECK(!file_exists_utf8(d0));
    CHECK(rep.candidates[1].outcome == RFC_CANDIDATE_COMPLETED &&
          rep.candidates[1].rank == 1u && file_exists_utf8(d1));

    rm_tree(tmp);
    free(png);
    printf("reports: cancel_and_partial_iterations ok\n");
}

/* ----------------------------------------------------------------
 * Test 5: Provenance enforcement and mixed provider build configs
 * ---------------------------------------------------------------- */

static void test_provenance_and_mixed_configs(void) {
    uint8_t *bmp = NULL;
    size_t blen = 0u;
    make_test_bmp_rgb(32u, 32u, 99u, &bmp, &blen);
    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "prov");

    rfc_compare_candidate_spec cands[3];
    memset(cands, 0, sizeof(cands));

    /* Cand 0: valid custom build_config A */
    cands[0].target_format = RFC_FORMAT_RASTERFOLD;
    cands[0].has_build_config_override = 1;
    CHECK(rfc_report_default_provider_config(
              RFC_FORMAT_RASTERFOLD, &cands[0].build_config) == RFC_OK);

    /* Cand 1: valid differing build_config B (portable SIMD + gcc) */
    cands[1].target_format = RFC_FORMAT_RASTERFOLD;
    cands[1].has_build_config_override = 1;
    cands[1].build_config = cands[0].build_config;
    snprintf(cands[1].build_config.compiler_id,
             sizeof(cands[1].build_config.compiler_id), "gcc 14.2.0");
    snprintf(cands[1].build_config.simd_features,
             sizeof(cands[1].build_config.simd_features),
             "portable-c11 (RF_PORTABLE_SIMD)");

    /* Cand 2: missing codec_version provenance -> must fail preflight cleanly */
    cands[2].target_format = RFC_FORMAT_BMP;
    cands[2].has_build_config_override = 1;
    CHECK(rfc_report_default_provider_config(
              RFC_FORMAT_BMP, &cands[2].build_config) == RFC_OK);
    cands[2].build_config.codec_version[0] = '\0';

    rfc_compare_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_bytes = bmp;
    sp.source_size = blen;
    sp.source_format = RFC_FORMAT_BMP;
    sp.staging_dir_utf8 = tmp;
    sp.requested_threads = 1u;
    sp.metadata_policy = RFC_META_POLICY_PRESERVE;
    sp.warmup_iterations = 0u;
    sp.measured_iterations = 1u;
    sp.candidates = cands;
    sp.candidate_count = 3u;
    sp.run_id = "mixed-prov";
    sp.recorded_utc = "2026-11-16T05:07:00Z";

    rfc_comparison_report rep;
    CHECK(rfc_compare_run(&sp, &rep) == RFC_OK);
    CHECK(rep.completed_count == 2u && rep.failed_count == 1u);
    CHECK(rep.candidates[0].measurement_only == 1 &&
          rep.candidates[0].has_produced_output == 0);
    CHECK(rep.candidates[1].measurement_only == 1 &&
          rep.candidates[1].has_produced_output == 0);
    CHECK(rep.candidates[2].outcome == RFC_CANDIDATE_FAILED &&
          rep.candidates[2].status == RFC_ERR_INVALID_ARGUMENT &&
          rep.candidates[2].rank == 0u);
    CHECK(rep.mixed_provider_configurations == 1);
    CHECK(strstr(rep.configuration_notes, "compiler") != NULL);
    CHECK(strstr(rep.configuration_notes, "simd_features") != NULL);
    CHECK(rfc_report_validate(&rep) == RFC_OK);

    /* Ensure rfc_report_validate rejects tampered mixed=0, cleared provenance,
     * or inconsistent measurement_only/has_produced_output combinations. */
    rfc_comparison_report bad = rep;
    bad.mixed_provider_configurations = 0;
    CHECK(rfc_report_validate(&bad) == RFC_ERR_INVALID_ARGUMENT);
    bad = rep;
    bad.candidates[0].build_config.simd_features[0] = '\0';
    CHECK(rfc_report_validate(&bad) == RFC_ERR_INVALID_ARGUMENT);
    bad = rep;
    bad.candidates[0].has_produced_output = 1;
    CHECK(rfc_report_validate(&bad) == RFC_ERR_INVALID_ARGUMENT);
    bad = rep;
    bad.candidates[0].measurement_only = 0;
    CHECK(rfc_report_validate(&bad) == RFC_ERR_INVALID_ARGUMENT);

    rm_tree(tmp);
    free(bmp);
    printf("reports: provenance_and_mixed_configs ok\n");
}

/* ----------------------------------------------------------------
 * Test 6: JSON export & tamper-evident chained JSONL history
 * ---------------------------------------------------------------- */

static void test_json_and_history_chain(void) {
    uint8_t *png = NULL;
    size_t plen = 0u;
    make_test_png_rgb(48u, 32u, 13u, &png, &plen);

    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "hist");
    char hist_path[1024], json_path[1024], fold1[1024], fold2[1024],
        fold3[1024], bad_copy[1024];
    join_path(hist_path, sizeof(hist_path), tmp, "runs.jsonl");
    join_path(json_path, sizeof(json_path), tmp, "report.json");
    join_path(fold1, sizeof(fold1), tmp, "r1.fold");
    join_path(fold2, sizeof(fold2), tmp, "r2.fold");
    join_path(fold3, sizeof(fold3), tmp, "r3.fold");
    join_path(bad_copy, sizeof(bad_copy), tmp, "tampered.jsonl");

    rfc_setting_value c0_meta = {"metadata", "preserve"};
    rfc_compare_candidate_spec cands[3];
    memset(cands, 0, sizeof(cands));
    cands[0].target_format = RFC_FORMAT_RASTERFOLD;
    cands[0].dest_path_utf8 = fold1;
    cands[0].settings = &c0_meta;
    cands[0].setting_count = 1u;
    cands[1].target_format = RFC_FORMAT_WEBP;
    cands[2].target_format = RFC_FORMAT_HEIC;

    rfc_compare_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.source_bytes = png;
    sp.source_size = plen;
    sp.source_format = RFC_FORMAT_PNG;
    sp.staging_dir_utf8 = tmp;
    sp.requested_threads = 1u;
    sp.metadata_policy = RFC_META_POLICY_PRESERVE;
    sp.acknowledged_unsupported_metadata = 1;
    sp.warmup_iterations = 0u;
    sp.measured_iterations = 1u;
    sp.candidates = cands;
    sp.candidate_count = 3u;
    sp.history_path_utf8 = hist_path;
    sp.run_id = "run-001";
    sp.recorded_utc = "2026-11-16T05:10:00Z";

    rfc_comparison_report *reps =
        (rfc_comparison_report *)calloc(4u, sizeof(*reps));
    CHECK(reps != NULL);
    #define r1 (reps[0])
    #define r2 (reps[1])
    #define r3 (reps[2])
    #define r4 (reps[3])

    CHECK(rfc_compare_run(&sp, &r1) == RFC_OK);
    CHECK(r1.history_status == RFC_OK && r1.history_recorded == 1);
    CHECK(rfc_report_write_json_path(json_path, &r1) == RFC_OK);
    CHECK(rfc_report_write_json_path(json_path, &r1) ==
          RFC_ERR_DESTINATION_EXISTS);

    /* Append run-002 (default threads) and run-003 (between-candidate cancel) */
    cands[0].dest_path_utf8 = fold2;
    sp.requested_threads = 0u;
    sp.run_id = "run-002";
    sp.recorded_utc = "2026-11-16T05:11:00Z";
    CHECK(rfc_compare_run(&sp, &r2) == RFC_OK);
    CHECK(r2.history_recorded == 1 && r2.effective_threads > 1u);

    cands[0].dest_path_utf8 = fold3;
    rfc_compare_token *tok = NULL;
    CHECK(rfc_compare_token_create(&tok) == RFC_OK);
    cancel_inject_state stc = {tok, 3, 0};
    rfc_compare_set_hook_for_testing(ci_between_hook, &stc);
    sp.cancel_token = tok;
    sp.run_id = "run-003";
    sp.recorded_utc = "2026-11-16T05:12:00Z";
    CHECK(rfc_compare_run(&sp, &r3) == RFC_ERR_CANCELLED);
    rfc_compare_set_hook_for_testing(NULL, NULL);
    rfc_compare_token_destroy(tok);
    CHECK(r3.history_status == RFC_OK && r3.history_recorded == 1 &&
          r3.cancelled == 1);

    /* Reopen in fresh reader and validate chain */
    rfc_history_log hlog;
    CHECK(rfc_history_read_path(hist_path, &hlog) == RFC_OK);
    CHECK(hlog.entry_count == 3u);
    CHECK(hlog.entries[0].sequence == 1u && hlog.entries[1].sequence == 2u &&
          hlog.entries[2].sequence == 3u);
    CHECK(strcmp(hlog.entries[0].run_id, "run-001") == 0);
    CHECK(strcmp(hlog.entries[1].run_id, "run-002") == 0);
    CHECK(strcmp(hlog.entries[2].run_id, "run-003") == 0);
    CHECK(hlog.entries[2].run_status == RFC_ERR_CANCELLED &&
          !hlog.entries[2].ranking_complete);
    CHECK(memcmp(hlog.entries[1].prev_entry_sha256,
                 hlog.entries[0].entry_sha256, 32u) == 0);
    CHECK(memcmp(hlog.entries[2].prev_entry_sha256,
                 hlog.entries[1].entry_sha256, 32u) == 0);
    CHECK(hlog.entries[0].acknowledged_unsupported_metadata == 1);
    CHECK(hlog.entries[0].effective_threads == 1u);
    CHECK(hlog.entries[1].effective_threads > 1u);
    CHECK(strcmp(hlog.entries[0].configuration_notes, r1.configuration_notes) ==
          0);
    CHECK(hlog.entries[0].configuration_notes[0] != '\0');
    CHECK(hlog.entries[0].candidates[0].outcome == RFC_CANDIDATE_COMPLETED &&
          hlog.entries[0].candidates[0].failing_stage == RFC_STAGE_NONE &&
          hlog.entries[0].candidates[0].effective_threads ==
              r1.candidates[0].effective_threads);
    CHECK(hlog.entries[0].candidates[0].setting_count == 1u &&
          strcmp(hlog.entries[0].candidates[0].settings[0].id, "metadata") ==
              0 &&
          strcmp(hlog.entries[0].candidates[0].settings[0].value, "preserve") ==
              0);
    CHECK(strcmp(hlog.entries[0].candidates[0].build_config.provider_name,
                 r1.candidates[0].build_config.provider_name) == 0 &&
          strcmp(hlog.entries[0].candidates[0].build_config.compiler_id,
                 r1.candidates[0].build_config.compiler_id) == 0 &&
          strcmp(hlog.entries[0].candidates[0].build_config.build_flags,
                 r1.candidates[0].build_config.build_flags) == 0 &&
          strcmp(hlog.entries[0].candidates[0].build_config.simd_features,
                 r1.candidates[0].build_config.simd_features) == 0 &&
          strcmp(hlog.entries[0].candidates[0].build_config.threading_features,
                 r1.candidates[0].build_config.threading_features) == 0);
    CHECK(hlog.entries[0].candidates[0].verify_valid == 1 &&
          hlog.entries[0].candidates[0].verify_status == RFC_OK &&
          hlog.entries[0].candidates[0].exactness == RFC_EXACT_PIXEL_EXACT &&
          hlog.entries[0].candidates[0].independent_decoder ==
              r1.candidates[0].verify_report.independent_decoder &&
          strcmp(hlog.entries[0].candidates[0].decoder_provenance,
                 r1.candidates[0].verify_report.decoder_provenance) == 0);
    CHECK(memcmp(hlog.entries[0].candidates[0].source_hash,
                 r1.candidates[0].verify_report.source_hash,
                 32u) == 0);
    CHECK(memcmp(hlog.entries[0].candidates[0].verified_hash,
                 r1.candidates[0].verify_report.verified_hash,
                 32u) == 0);
    CHECK(hlog.entries[0].candidates[0].has_produced_sha256 == 1 &&
          memcmp(hlog.entries[0].candidates[0].produced_sha256,
                 r1.candidates[0].size.produced_sha256, 32u) == 0);
    CHECK(hlog.entries[0].candidates[0].final_publish_ns ==
          r1.candidates[0].final_publish_ns);
    rfc_history_log_free(&hlog);

    /* Snapshot exact good history bytes + SHA-256 */
    uint8_t *good_bytes = NULL;
    size_t good_len = 0u;
    read_file_all(hist_path, &good_bytes, &good_len);
    uint8_t good_sha[32];
    rfc_bytes_hash_sha256(good_bytes, good_len, good_sha);

    /* Tamper check on copy: alter run_001 -> run_999 */
    uint8_t *tbuf = (uint8_t *)malloc(good_len);
    CHECK(tbuf != NULL);
    memcpy(tbuf, good_bytes, good_len);
    char *hit = strstr((char *)tbuf, "run-001");
    CHECK(hit != NULL);
    hit[4] = '9';
    write_file_all(bad_copy, tbuf, good_len);
    CHECK(rfc_history_read_path(bad_copy, &hlog) == RFC_ERR_VERIFY_MISMATCH);
    CHECK(rfc_history_append_path(bad_copy, &r1) == RFC_ERR_VERIFY_MISMATCH);
    uint8_t *after_tamper = NULL;
    size_t after_tamper_len = 0u;
    read_file_all(bad_copy, &after_tamper, &after_tamper_len);
    CHECK(after_tamper_len == good_len &&
          memcmp(after_tamper, tbuf, good_len) == 0);
    free(after_tamper);
    free(tbuf);

    /* Truncation check (missing trailing newline) */
    write_file_all(bad_copy, good_bytes, good_len - 1u);
    CHECK(rfc_history_read_path(bad_copy, &hlog) == RFC_ERR_TRUNCATED_INPUT);
    CHECK(rfc_history_append_path(bad_copy, &r1) == RFC_ERR_TRUNCATED_INPUT);

    /* Fault injection: disk-full & flush I/O preserve existing history and succeed conversion */
    char fold4[1024];
    join_path(fold4, sizeof(fold4), tmp, "r4.fold");
    cands[0].dest_path_utf8 = fold4;
    sp.cancel_token = NULL;
    sp.run_id = "run-004";
    sp.recorded_utc = "2026-11-16T05:13:00Z";
    rfc_history_set_fault_for_testing(RFC_HIST_FAULT_WRITE_DISK_FULL,
                                      RFC_ERR_DISK_FULL);
    CHECK(rfc_compare_run(&sp, &r4) == RFC_ERR_DISK_FULL);
    rfc_history_set_fault_for_testing(RFC_HIST_FAULT_NONE, RFC_OK);
    CHECK(r4.run_status == RFC_OK && r4.history_status == RFC_ERR_DISK_FULL &&
          r4.history_recorded == 0 && file_exists_utf8(fold4));
    CHECK(!dir_has_prefix(tmp, ".rfc-hist-tmp."));

    uint8_t *after_fault = NULL;
    size_t after_fault_len = 0u;
    uint8_t after_fault_sha[32];
    read_file_all(hist_path, &after_fault, &after_fault_len);
    rfc_bytes_hash_sha256(after_fault, after_fault_len, after_fault_sha);
    CHECK(after_fault_len == good_len &&
          memcmp(after_fault_sha, good_sha, 32u) == 0);
    free(after_fault);

    /* Repeat with flush failure */
    char fold5[1024];
    join_path(fold5, sizeof(fold5), tmp, "r5.fold");
    cands[0].dest_path_utf8 = fold5;
    sp.run_id = "run-005";
    rfc_history_set_fault_for_testing(RFC_HIST_FAULT_FLUSH_IO, RFC_ERR_IO);
    CHECK(rfc_compare_run(&sp, &r4) == RFC_ERR_IO);
    rfc_history_set_fault_for_testing(RFC_HIST_FAULT_NONE, RFC_OK);
    CHECK(r4.run_status == RFC_OK && r4.history_status == RFC_ERR_IO &&
          r4.history_recorded == 0 && file_exists_utf8(fold5));
    CHECK(!dir_has_prefix(tmp, ".rfc-hist-tmp."));

    /* Forged entry with valid SHA-256 violating unsupported/has_produced_output invariant is rejected */
    const char *nl1 = (const char *)memchr(good_bytes, '\n', good_len);
    CHECK(nl1 != NULL);
    size_t l1_len = (size_t)(nl1 - (const char *)good_bytes) + 1u;
    char *forged = (char *)malloc(l1_len + 16u);
    CHECK(forged != NULL);
    memcpy(forged, good_bytes, l1_len);
    forged[l1_len] = '\0';
    char *webp_tok = strstr(forged, "\"target_format\":\"webp\"");
    CHECK(webp_tok != NULL);
    char *hpo = strstr(webp_tok, "\"has_produced_output\":false");
    CHECK(hpo != NULL);
    /* Replace "false" (5 chars) with "true" (4 chars) by shifting tail left by 1 byte */
    char *false_pos = hpo + strlen("\"has_produced_output\":");
    memcpy(false_pos, "true", 4u);
    memmove(false_pos + 4u, false_pos + 5u, strlen(false_pos + 5u) + 1u);
    char *esha_key = strstr(forged, ",\"entry_sha256\":\"");
    CHECK(esha_key != NULL);
    size_t prefix_pos = (size_t)(esha_key - forged);
    uint8_t *canon = (uint8_t *)malloc(prefix_pos + 1u);
    CHECK(canon != NULL);
    memcpy(canon, forged, prefix_pos);
    canon[prefix_pos] = '}';
    uint8_t forged_sha[32];
    rfc_bytes_hash_sha256(canon, prefix_pos + 1u, forged_sha);
    free(canon);
    static const char k_hex[] = "0123456789abcdef";
    char *hex_dst = esha_key + strlen(",\"entry_sha256\":\"");
    for (size_t hi = 0u; hi < 32u; hi++) {
        hex_dst[hi * 2u] = k_hex[(forged_sha[hi] >> 4) & 0xFu];
        hex_dst[hi * 2u + 1u] = k_hex[forged_sha[hi] & 0xFu];
    }
    CHECK(rfc_history_verify_bytes((const uint8_t *)forged, strlen(forged),
                                   &hlog) == RFC_ERR_VERIFY_MISMATCH);
    free(forged);

    #undef r1
    #undef r2
    #undef r3
    #undef r4
    free(reps);
    free(good_bytes);
    rm_tree(tmp);
    free(png);
    printf("reports: json_and_history_chain ok\n");
}

/* ----------------------------------------------------------------
 * Test 7: Round 1 audit regression suite (all 14 findings)
 * ---------------------------------------------------------------- */

typedef struct r1_conc_arg {
    const char *hist_path;
    const rfc_comparison_report *tmpl_rep;
    int tid;
    int count;
    rfc_status fail_st;
} r1_conc_arg;

static void r1_conc_run(r1_conc_arg *arg) {
    arg->fail_st = RFC_OK;
    rfc_comparison_report *r =
        (rfc_comparison_report *)calloc(1u, sizeof(*r));
    if (!r) {
        arg->fail_st = RFC_ERR_OUT_OF_MEMORY;
        return;
    }
    for (int j = 0; j < arg->count; j++) {
        *r = *arg->tmpl_rep;
        snprintf(r->run_id, sizeof(r->run_id), "conc-t%d-%d", arg->tid, j);
        rfc_status st = rfc_history_append_path(arg->hist_path, r);
        if (st != RFC_OK) {
            arg->fail_st = st;
            free(r);
            return;
        }
    }
    free(r);
}

#ifdef _WIN32
static DWORD WINAPI r1_conc_win_thread(LPVOID p) {
    r1_conc_run((r1_conc_arg *)p);
    return 0u;
}
#else
static void *r1_conc_posix_thread(void *p) {
    r1_conc_run((r1_conc_arg *)p);
    return NULL;
}
#endif

typedef struct r1_overread_rdr_ctx {
    int closed;
} r1_overread_rdr_ctx;

static rfc_status r1_overread_size(void *raw, int *known, uint64_t *sz) {
    (void)raw;
    *known = 1;
    *sz = 70000u;
    return RFC_OK;
}

static rfc_status r1_overread_seek(void *raw, uint64_t off) {
    (void)raw;
    (void)off;
    return RFC_OK;
}

static rfc_status r1_overread_read(void *raw,
                                   uint8_t *buf,
                                   size_t max_bytes,
                                   size_t *out_read) {
    (void)raw;
    (void)buf;
    (void)max_bytes;
    *out_read = 65537u;
    return RFC_OK;
}

static void r1_overread_close(void *raw) {
    r1_overread_rdr_ctx *rc = (r1_overread_rdr_ctx *)raw;
    rc->closed++;
}

static rfc_status r1_overread_open_reader(void *raw_ctx,
                                          rfc_job_id jid,
                                          uint64_t attempt,
                                          rfc_io_reader *out_rdr) {
    (void)jid;
    (void)attempt;
    memset(out_rdr, 0, sizeof(*out_rdr));
    out_rdr->ctx = raw_ctx;
    out_rdr->size_fn = r1_overread_size;
    out_rdr->seek_fn = r1_overread_seek;
    out_rdr->read_fn = r1_overread_read;
    out_rdr->close_fn = r1_overread_close;
    return RFC_OK;
}

typedef struct r1_ctx_tracker {
    uint32_t magic;
    int encode_calls;
    int destroy_calls;
} r1_ctx_tracker;

static rfc_status r1_tracked_encode(rfc_job *job,
                                    const rfc_exec_context *exec,
                                    void *raw_ctx) {
    r1_ctx_tracker *t = (r1_ctx_tracker *)raw_ctx;
    CHECK(t != NULL && t->magic == 0xC0DEC123u && t->destroy_calls == 0);
    t->encode_calls++;
    size_t fcnt = 0u;
    const rfc_metadata *fmeta = NULL;
    const rfc_frame *frames = rfc_job_decoded_frames(job, &fcnt, &fmeta);
    CHECK(frames != NULL && fcnt == 1u);
    rfc_encoded enc;
    memset(&enc, 0, sizeof(enc));
    rfc_status st =
        bmp_sess_encode(NULL, exec, frames, fcnt, fmeta, NULL, 0u, &enc);
    if (st != RFC_OK) {
        return st;
    }
    return rfc_job_set_encoded_output(job, &enc);
}

static void r1_destroy_ctx(void *raw_ctx) {
    r1_ctx_tracker *t = (r1_ctx_tracker *)raw_ctx;
    CHECK(t != NULL && t->magic == 0xC0DEC123u);
    t->destroy_calls++;
}

typedef struct r1_cancel_hook_state {
    rfc_compare_token *tok;
    rfc_compare_hook_phase target_phase;
    size_t target_cand;
} r1_cancel_hook_state;

static void r1_cancel_hook_cb(rfc_compare_hook_phase phase,
                              size_t cand_idx,
                              uint32_t iter_idx,
                              rfc_compare_token *tok,
                              void *user) {
    (void)iter_idx;
    r1_cancel_hook_state *st = (r1_cancel_hook_state *)user;
    if (st && phase == st->target_phase && cand_idx == st->target_cand && tok) {
        rfc_compare_cancel(tok);
    }
}

typedef struct r1_count_pub_ctx {
    cb_pub_ctx base;
    int commit_count;
} r1_count_pub_ctx;

static rfc_status r1_cnt_commit(void *raw_ctx, void *raw_sh, int *out_cleaned) {
    r1_count_pub_ctx *cp = (r1_count_pub_ctx *)raw_ctx;
    cp->commit_count++;
    return cb_pub_commit(&cp->base, raw_sh, out_cleaned);
}

static void test_round1_audit_regressions(void) {
    uint8_t *png = NULL;
    size_t plen = 0u;
    make_test_png_rgb(32u, 32u, 42u, &png, &plen);

    char tmp[1024];
    make_temp_dir(tmp, sizeof(tmp), "r1audit");

    /* #1 + #11: colliding pre-existing file survives failed submit; staging-only
     * completes as measurement_only=1 with has_produced_output=0 & no leftover dirs */
    char collide[1024];
    join_path(collide, sizeof(collide), tmp, "existing.fold");
    const uint8_t sentinel[] = "SENTINEL-KEEP-BYTES";
    write_file_all(collide, sentinel, sizeof(sentinel));

    rfc_compare_candidate_spec c1[2];
    memset(c1, 0, sizeof(c1));
    c1[0].target_format = RFC_FORMAT_RASTERFOLD;
    c1[0].dest_path_utf8 = collide;
    c1[1].target_format = RFC_FORMAT_BMP; /* staging-only */

    rfc_limits custom_lim;
    rfc_limits_default(&custom_lim);
    custom_lim.max_width = 4096u;

    rfc_compare_spec sp1;
    memset(&sp1, 0, sizeof(sp1));
    sp1.source_bytes = png;
    sp1.source_size = plen;
    sp1.source_format = RFC_FORMAT_PNG;
    sp1.staging_dir_utf8 = tmp;
    sp1.requested_threads = 1u;
    sp1.limits = &custom_lim;
    sp1.warmup_iterations = 0u;
    sp1.measured_iterations = 1u;
    sp1.candidates = c1;
    sp1.candidate_count = 2u;
    sp1.run_id = "r1-u-\xC3\xA9-\xE2\x82\xAC-\xF0\x9F\x98\x80";
    sp1.recorded_utc = "2026-11-16T06:00:00Z";

    rfc_comparison_report *rep1_p =
        (rfc_comparison_report *)calloc(1u, sizeof(*rep1_p));
    rfc_comparison_report *rep_bad_p =
        (rfc_comparison_report *)calloc(1u, sizeof(*rep_bad_p));
    CHECK(rep1_p != NULL && rep_bad_p != NULL);
    #define rep1 (*rep1_p)
    #define rep_bad (*rep_bad_p)

    CHECK(rfc_compare_run(&sp1, &rep1) == RFC_OK);
    CHECK(rep1.candidates[0].outcome == RFC_CANDIDATE_FAILED &&
          rep1.candidates[0].status == RFC_ERR_DESTINATION_EXISTS &&
          !rep1.candidates[0].has_produced_output);
    uint8_t *got_col = NULL;
    size_t got_col_len = 0u;
    read_file_all(collide, &got_col, &got_col_len);
    CHECK(got_col_len == sizeof(sentinel) &&
          memcmp(got_col, sentinel, sizeof(sentinel)) == 0);
    free(got_col);

    CHECK(rep1.candidates[1].outcome == RFC_CANDIDATE_COMPLETED &&
          rep1.candidates[1].measurement_only == 1 &&
          rep1.candidates[1].has_produced_output == 0 &&
          rep1.candidates[1].rank == 1u);
    CHECK(rep1.limits.max_width == 4096u);
    CHECK(!dir_has_prefix(tmp, ".rfc-cmp-stage."));
    CHECK(!dir_has_prefix(tmp, ".rfc-cmp."));

    /* #13: limits & measurement_only preserved in JSON and JSONL history */
    char *json1 = NULL;
    size_t json1_len = 0u;
    CHECK(rfc_report_format_json(&rep1, &json1, &json1_len) == RFC_OK);
    CHECK(strstr(json1, "\"limits\":{\"max_width\":4096") != NULL);
    CHECK(strstr(json1, "\"measurement_only\":true") != NULL);
    CHECK(strstr(json1, "\"final_publish_ns\":") != NULL);
    rfc_report_free(json1);

    char lim_hist[1024];
    join_path(lim_hist, sizeof(lim_hist), tmp, "lim.jsonl");
    CHECK(rfc_history_append_path(lim_hist, &rep1) == RFC_OK);
    rfc_history_log llog;
    CHECK(rfc_history_read_path(lim_hist, &llog) == RFC_OK);
    CHECK(llog.entry_count == 1u && llog.entries[0].limits.max_width == 4096u);
    CHECK(llog.entries[0].candidates[1].measurement_only == 1 &&
          llog.entries[0].candidates[1].has_produced_output == 0);
    rfc_history_log_free(&llog);

    /* #8: bounded history max bytes shared between append and read */
    uint8_t *lh_bytes = NULL;
    size_t lh_len = 0u;
    read_file_all(lim_hist, &lh_bytes, &lh_len);
    rfc_history_set_max_bytes_for_testing(lh_len + 32u);
    CHECK(rfc_history_append_path(lim_hist, &rep1) == RFC_ERR_LIMIT_EXCEEDED);
    CHECK(rfc_history_read_path(lim_hist, &llog) == RFC_OK);
    CHECK(llog.entry_count == 1u);
    rfc_history_log_free(&llog);
    uint8_t *lh_after = NULL;
    size_t lh_after_len = 0u;
    read_file_all(lim_hist, &lh_after, &lh_after_len);
    CHECK(lh_after_len == lh_len && memcmp(lh_after, lh_bytes, lh_len) == 0);
    free(lh_after);
    if (lh_len > 1u) {
        rfc_history_set_max_bytes_for_testing(lh_len - 1u);
        CHECK(rfc_history_read_path(lim_hist, &llog) == RFC_ERR_LIMIT_EXCEEDED);
    }
    rfc_history_set_max_bytes_for_testing(0u);
    free(lh_bytes);

    /* #2: concurrent multi-thread rfc_history_append_path (4 threads x 4 entries) */
    char conc_hist[1024];
    join_path(conc_hist, sizeof(conc_hist), tmp, "concurrent.jsonl");
    r1_conc_arg targs[4];
    for (int t = 0; t < 4; t++) {
        targs[t].hist_path = conc_hist;
        targs[t].tmpl_rep = &rep1;
        targs[t].tid = t;
        targs[t].count = 4;
        targs[t].fail_st = RFC_OK;
    }
#ifdef _WIN32
    HANDLE ths[4];
    for (int t = 0; t < 4; t++) {
        ths[t] = CreateThread(NULL, 0u, r1_conc_win_thread, &targs[t], 0u, NULL);
        CHECK(ths[t] != NULL);
    }
    CHECK(WaitForMultipleObjects(4u, ths, TRUE, 30000u) == WAIT_OBJECT_0);
    for (int t = 0; t < 4; t++) {
        CloseHandle(ths[t]);
        CHECK(targs[t].fail_st == RFC_OK);
    }
#else
    pthread_t ths[4];
    for (int t = 0; t < 4; t++) {
        CHECK(pthread_create(&ths[t], NULL, r1_conc_posix_thread, &targs[t]) ==
              0);
    }
    for (int t = 0; t < 4; t++) {
        CHECK(pthread_join(ths[t], NULL) == 0);
        CHECK(targs[t].fail_st == RFC_OK);
    }
#endif
    rfc_history_log clog;
    CHECK(rfc_history_read_path(conc_hist, &clog) == RFC_OK);
    CHECK(clog.entry_count == 16u);
    for (size_t e = 0u; e < clog.entry_count; e++) {
        CHECK(clog.entries[e].sequence == (uint64_t)(e + 1u));
    }
    rfc_history_log_free(&clog);
    CHECK(!dir_has_prefix(tmp, ".rfc-hist-tmp."));

    /* #3: non-existent regular path safely fails without side effects */
    char no_hist[1024];
    join_path(no_hist, sizeof(no_hist), tmp, "does_not_exist.jsonl");
    CHECK(rfc_history_read_path(no_hist, &llog) == RFC_ERR_IO);

    /* #4: callback reader returning got > sizeof(chunk) (65537) fails cleanly */
    r1_overread_rdr_ctx or_ctx = {0};
    rfc_io_publisher or_pub;
    memset(&or_pub, 0, sizeof(or_pub));
    or_pub.ctx = &or_ctx;
    or_pub.open_published_reader = r1_overread_open_reader;
    rfc_export_report or_rep;
    memset(&or_rep, 0, sizeof(or_rep));
    or_rep.valid = 1;
    or_rep.published = 1;
    or_rep.status = RFC_OK;
    or_rep.bytes_written = 70000u;
    rfc_size_metrics or_sm;
    CHECK(rfc_metrics_verify_produced_callback(&or_pub, 1u, 1u, 100u, 300u,
                                               rep1.source_sha256, 1, &or_rep,
                                               &or_sm) == RFC_ERR_IO);
    CHECK(or_ctx.closed == 1);

    /* #5: unterminated fixed char[] buffers rejected by validate and format */
    rfc_provider_build_config cfg_bad;
    CHECK(rfc_report_default_provider_config(RFC_FORMAT_BMP, &cfg_bad) ==
          RFC_OK);
    memset(cfg_bad.provider_name, 'A', sizeof(cfg_bad.provider_name));
    CHECK(rfc_provider_build_config_validate(&cfg_bad) ==
          RFC_ERR_INVALID_ARGUMENT);
    rep_bad = rep1;
    memset(rep_bad.candidates[0].detail, 'Z',
           sizeof(rep_bad.candidates[0].detail));
    CHECK(rfc_report_validate(&rep_bad) == RFC_ERR_INVALID_ARGUMENT);
    CHECK(rfc_report_format_json(&rep_bad, &json1, &json1_len) ==
          RFC_ERR_INVALID_ARGUMENT);

    /* #6: nonzero setting_count with NULL settings rejected */
    rfc_compare_candidate_spec c6;
    memset(&c6, 0, sizeof(c6));
    c6.target_format = RFC_FORMAT_BMP;
    c6.setting_count = 1u;
    c6.settings = NULL;
    rfc_compare_spec sp6 = sp1;
    sp6.candidates = &c6;
    sp6.candidate_count = 1u;
    sp6.limits = NULL;
    sp6.run_id = "r1-null-settings";
    CHECK(rfc_compare_run(&sp6, &rep_bad) == RFC_ERR_INVALID_ARGUMENT);

    rfc_setting_value bad_sv = {"k", NULL};
    c6.settings = &bad_sv;
    CHECK(rfc_compare_run(&sp6, &rep_bad) == RFC_ERR_INVALID_ARGUMENT);

    /* #9: invalid UTF-8 and over-capacity run_id rejected; boundary truncation keeps valid UTF-8 */
    sp6 = sp1;
    sp6.candidates = &c1[1];
    sp6.candidate_count = 1u;
    sp6.limits = NULL;
    sp6.run_id = "bad-\xFF-utf8";
    CHECK(rfc_compare_run(&sp6, &rep_bad) == RFC_ERR_INVALID_ARGUMENT);
    sp6.run_id =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "\xC3\xA9"; /* 63 ASCII + 2-byte UTF-8 = 65 bytes >= 64 */
    CHECK(rfc_compare_run(&sp6, &rep_bad) == RFC_ERR_INVALID_ARGUMENT);
    rep_bad = rep1;
    memcpy(rep_bad.run_id, "abc\xE2\x82", 6u);
    CHECK(rfc_report_validate(&rep_bad) == RFC_ERR_INVALID_ARGUMENT);

    /* #7 + #14: single-ownership user_ctx/release_ctx + homogeneous callback sink */
    r1_count_pub_ctx cpub;
    memset(&cpub, 0, sizeof(cpub));
    rfc_io_publisher pub14 = make_cb_pub(&cpub.base, 1);
    pub14.ctx = &cpub;
    pub14.commit_stage = r1_cnt_commit;

    r1_ctx_tracker tracker = {0xC0DEC123u, 0, 0};
    rfc_compare_candidate_spec c7;
    memset(&c7, 0, sizeof(c7));
    c7.target_format = RFC_FORMAT_BMP;
    c7.publisher = &pub14;
    c7.user_ctx = &tracker;
    c7.has_stage_ops = 1;
    c7.stage_ops.on_encode = r1_tracked_encode;
    c7.stage_ops.destroy_user_ctx = r1_destroy_ctx;

    rfc_compare_spec sp7 = sp1;
    sp7.candidates = &c7;
    sp7.candidate_count = 1u;
    sp7.limits = NULL;
    sp7.warmup_iterations = 1u;
    sp7.measured_iterations = 3u;
    sp7.run_id = "r1-single-owner-cb";
    rfc_comparison_report *rep7 = &rep_bad;
    CHECK(rfc_compare_run(&sp7, rep7) == RFC_OK);
    CHECK(rep7->candidates[0].outcome == RFC_CANDIDATE_COMPLETED);
    CHECK(tracker.encode_calls == 4 && tracker.destroy_calls == 1);
    CHECK(cpub.commit_count == 1 && cpub.base.release_called_count == 1 &&
          cpub.base.closed_before_release == 1);
    CHECK(rep7->candidates[0].measured_completed == 3u);
    for (uint32_t m = 0u; m < 3u; m++) {
        CHECK(rep7->candidates[0].per_iter_timings[m].valid == 1);
    }
    free(cpub.base.stored);

    /* #10a: cancel at BEFORE_FINAL_PUBLISH produces zero destination file */
    char pre_dest[1024];
    join_path(pre_dest, sizeof(pre_dest), tmp, "pre_cancel.bmp");
    rfc_compare_candidate_spec c10;
    memset(&c10, 0, sizeof(c10));
    c10.target_format = RFC_FORMAT_BMP;
    c10.dest_path_utf8 = pre_dest;
    rfc_compare_token *tok10a = NULL;
    CHECK(rfc_compare_token_create(&tok10a) == RFC_OK);
    r1_cancel_hook_state h10a = {tok10a, RFC_COMPARE_HOOK_BEFORE_FINAL_PUBLISH,
                                 0u};
    rfc_compare_set_hook_for_testing(r1_cancel_hook_cb, &h10a);
    rfc_compare_spec sp10 = sp1;
    sp10.candidates = &c10;
    sp10.candidate_count = 1u;
    sp10.limits = NULL;
    sp10.cancel_token = tok10a;
    sp10.run_id = "r1-before-final";
    rfc_comparison_report *rep10a = &rep_bad;
    CHECK(rfc_compare_run(&sp10, rep10a) == RFC_ERR_CANCELLED);
    rfc_compare_set_hook_for_testing(NULL, NULL);
    rfc_compare_token_destroy(tok10a);
    CHECK(rep10a->candidates[0].outcome == RFC_CANDIDATE_CANCELLED &&
          rep10a->candidates[0].has_produced_output == 0 &&
          !file_exists_utf8(pre_dest));
    CHECK(!dir_has_prefix(tmp, ".rfc-cmp-stage."));

    /* #10b: cancel at AFTER_FINAL_PUBLISH preserves completed cand 0 & output */
    char post0[1024], post1[1024];
    join_path(post0, sizeof(post0), tmp, "post0.bmp");
    join_path(post1, sizeof(post1), tmp, "post1.fold");
    rfc_compare_candidate_spec c10b[2];
    memset(c10b, 0, sizeof(c10b));
    c10b[0].target_format = RFC_FORMAT_BMP;
    c10b[0].dest_path_utf8 = post0;
    c10b[1].target_format = RFC_FORMAT_RASTERFOLD;
    c10b[1].dest_path_utf8 = post1;
    rfc_compare_token *tok10b = NULL;
    CHECK(rfc_compare_token_create(&tok10b) == RFC_OK);
    r1_cancel_hook_state h10b = {tok10b, RFC_COMPARE_HOOK_AFTER_FINAL_PUBLISH,
                                 0u};
    rfc_compare_set_hook_for_testing(r1_cancel_hook_cb, &h10b);
    sp10.candidates = c10b;
    sp10.candidate_count = 2u;
    sp10.cancel_token = tok10b;
    sp10.run_id = "r1-after-final";
    rfc_comparison_report *rep10b = &rep_bad;
    CHECK(rfc_compare_run(&sp10, rep10b) == RFC_ERR_CANCELLED);
    rfc_compare_set_hook_for_testing(NULL, NULL);
    rfc_compare_token_destroy(tok10b);
    CHECK(rep10b->cancelled == 1 && rep10b->ranking_complete == 0 &&
          rep10b->completed_count == 1u && rep10b->cancelled_count == 1u);
    CHECK(rep10b->candidates[0].outcome == RFC_CANDIDATE_COMPLETED &&
          rep10b->candidates[0].has_produced_output == 1 &&
          rep10b->candidates[0].rank == 0u && file_exists_utf8(post0));
    CHECK(rep10b->candidates[1].outcome == RFC_CANDIDATE_CANCELLED &&
          rep10b->candidates[1].was_started == 0 &&
          !file_exists_utf8(post1));
    CHECK(rfc_report_validate(rep10b) == RFC_OK);
    char *json10b = NULL;
    size_t json10b_len = 0u;
    CHECK(rfc_report_format_json(rep10b, &json10b, &json10b_len) == RFC_OK);
    rfc_report_free(json10b);

    /* Completion gap #1 validation boundary: only FAILED+STAGE_VERIFY may retain has_produced_output=1 */
    rfc_comparison_report *rep_val =
        (rfc_comparison_report *)calloc(1u, sizeof(*rep_val));
    CHECK(rep_val != NULL);
    *rep_val = *rep10b;
    CHECK(rep_val->candidates[1].outcome == RFC_CANDIDATE_CANCELLED);
    rep_val->candidates[1].has_produced_output = 1;
    CHECK(rfc_report_validate(rep_val) == RFC_ERR_INVALID_ARGUMENT);
    rep_val->candidates[1].outcome = RFC_CANDIDATE_FAILED;
    rep_val->candidates[1].failing_stage = RFC_STAGE_PREFLIGHT;
    rep_val->failed_count = 1u;
    rep_val->cancelled_count = 0u;
    CHECK(rfc_report_validate(rep_val) == RFC_ERR_INVALID_ARGUMENT);
    rep_val->candidates[1].failing_stage = RFC_STAGE_VERIFY;
    CHECK(rfc_report_validate(rep_val) == RFC_OK);
    free(rep_val);

    #undef rep1
    #undef rep_bad
    free(rep1_p);
    free(rep_bad_p);

    /* #12: generated rasterfold provenance + runtime override register/clear */
    rfc_provider_build_config rf_cfg;
    CHECK(rfc_report_default_provider_config(RFC_FORMAT_RASTERFOLD, &rf_cfg) ==
          RFC_OK);
#ifdef RFC_HAVE_GENERATED_PROVENANCE
    CHECK(strcmp(rf_cfg.codec_version, "1.0") != 0);
    CHECK(rf_cfg.codec_version[0] != '\0');
#endif
    rfc_provider_build_config custom_cfg = rf_cfg;
    snprintf(custom_cfg.compiler_id, sizeof(custom_cfg.compiler_id),
             "override-cc 9.9.9");
    CHECK(rfc_report_register_provider_config(RFC_FORMAT_RASTERFOLD,
                                              &custom_cfg) == RFC_OK);
    rfc_provider_build_config got_cfg;
    CHECK(rfc_report_default_provider_config(RFC_FORMAT_RASTERFOLD, &got_cfg) ==
          RFC_OK);
    CHECK(strcmp(got_cfg.compiler_id, "override-cc 9.9.9") == 0);
    rfc_report_clear_provider_configs();
    CHECK(rfc_report_default_provider_config(RFC_FORMAT_RASTERFOLD, &got_cfg) ==
          RFC_OK);
    CHECK(strcmp(got_cfg.compiler_id, rf_cfg.compiler_id) == 0);

    rm_tree(tmp);
    free(png);
    printf("reports: round1_audit_regressions ok\n");
}

/* ----------------------------------------------------------------
 * CLI subcommands invoked by test_reports.py
 * ---------------------------------------------------------------- */

static int cmd_compare_kodim(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr, "usage: compare-kodim <src.png> <out_dir> <threads> "
                        "<json_out> <history.jsonl> <run_id>\n");
        return 2;
    }
    const char *src_png = argv[2];
    const char *out_dir = argv[3];
    uint32_t req_threads = (uint32_t)strtoul(argv[4], NULL, 10);
    const char *json_out = argv[5];
    const char *hist_path = argv[6];
    const char *run_id = argv[7];

    char fold_out[1024], bmp_out[1024];
    join_path(fold_out, sizeof(fold_out), out_dir, "kodim23.fold");
    join_path(bmp_out, sizeof(bmp_out), out_dir, "kodim23.bmp");

    rfc_io_source src_io;
    rfc_io_source_init_path(&src_io, src_png, RFC_IMPORT_DEFAULT);

    rfc_compare_candidate_spec cands[4];
    memset(cands, 0, sizeof(cands));
    cands[0].target_format = RFC_FORMAT_RASTERFOLD;
    cands[0].dest_path_utf8 = fold_out;
    cands[1].target_format = RFC_FORMAT_BMP;
    cands[1].dest_path_utf8 = bmp_out;
    cands[2].target_format = RFC_FORMAT_WEBP;
    cands[3].target_format = RFC_FORMAT_HEIC;

    rfc_compare_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.io_source = &src_io;
    sp.staging_dir_utf8 = out_dir;
    sp.requested_threads = req_threads;
    sp.metadata_policy = RFC_META_POLICY_PRESERVE;
    sp.warmup_iterations = 1u;
    sp.measured_iterations = 2u;
    sp.candidates = cands;
    sp.candidate_count = 4u;
    sp.history_path_utf8 = hist_path;
    sp.run_id = run_id;

    rfc_comparison_report rep;
    rfc_status st = rfc_compare_run(&sp, &rep);
    if (st != RFC_OK) {
        fprintf(stderr, "rfc_compare_run failed: %s\n", rfc_status_name(st));
        return 1;
    }
    st = rfc_report_write_json_path(json_out, &rep);
    if (st != RFC_OK) {
        fprintf(stderr, "rfc_report_write_json_path failed: %s\n",
                rfc_status_name(st));
        return 1;
    }
    printf("kodim ok: fold=%llu bmp=%llu req_threads=%u eff_threads=%u\n",
           (unsigned long long)rep.candidates[0].size.produced_file_bytes,
           (unsigned long long)rep.candidates[1].size.produced_file_bytes,
           (unsigned)rep.requested_threads, (unsigned)rep.effective_threads);
    return 0;
}

static int cmd_verify_history(int argc, char **argv) {
    if (argc != 3) {
        return 2;
    }
    rfc_history_log log;
    rfc_status st = rfc_history_read_path(argv[2], &log);
    if (st != RFC_OK) {
        printf("verify-history: %s\n", rfc_status_name(st));
        return 1;
    }
    printf("verify-history: ok entries=%zu\n", log.entry_count);
    rfc_history_log_free(&log);
    return 0;
}

static int cmd_dump_provider_configs(void) {
    size_t n = rfc_registry_count();
    for (size_t i = 0u; i < n; i++) {
        const rfc_capability *cap = rfc_registry_at(i);
        rfc_provider_build_config cfg;
        CHECK(rfc_report_default_provider_config(cap->id, &cfg) == RFC_OK);
        CHECK(rfc_provider_build_config_validate(&cfg) == RFC_OK);
        printf("%s|%s|%s|%s|%s|%s|%s\n", cap->name, cfg.provider_name,
               cfg.codec_version, cfg.compiler_id, cfg.target_abi,
               cfg.simd_features, cfg.threading_features);
    }
    return 0;
}

int main(int argc, char **argv) {
    register_test_adapters();

    int rc = 0;
    if (argc == 1) {
        test_stage_timings_and_retry();
        test_size_outcomes_and_callback_readback();
        test_sequential_compare_and_ranking();
        test_cancel_and_partial_iterations();
        test_provenance_and_mixed_configs();
        test_json_and_history_chain();
        test_round1_audit_regressions();
        printf("reports: all checks passed\n");
    } else if (strcmp(argv[1], "compare-kodim") == 0) {
        rc = cmd_compare_kodim(argc, argv);
    } else if (strcmp(argv[1], "verify-history") == 0) {
        rc = cmd_verify_history(argc, argv);
    } else if (strcmp(argv[1], "dump-provider-configs") == 0) {
        rc = cmd_dump_provider_configs();
    } else {
        fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
        rc = 2;
    }

    unregister_test_adapters();
    return rc;
}
