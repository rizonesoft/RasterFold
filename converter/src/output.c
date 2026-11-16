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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "job_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000Cul
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003ul
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define RFC_OUTPUT_CHUNK_BYTES (64u * 1024u)

#ifdef _WIN32
static atomic_uint_fast64_t g_partial_seq = 1u;
static wchar_t g_test_unc_prefix_w[1024];
static wchar_t g_test_unc_backing_w[1024];
#endif
static rfc_io_fault_stage g_test_fault_stage = RFC_IO_FAULT_NONE;
static uint64_t g_test_fault_after_bytes = 0u;
static rfc_status g_test_fault_status = RFC_OK;
static void (*g_before_begin_publish_cb)(rfc_job_id job_id, void *user) = NULL;
static void (*g_after_begin_publish_cb)(rfc_job_id job_id, void *user) = NULL;
static void *g_publish_barrier_user = NULL;
#ifndef _WIN32
static void (*g_after_commit_check_cb)(rfc_job_id job_id, void *user) = NULL;
static void (*g_after_abort_check_cb)(rfc_job_id job_id, void *user) = NULL;
static void *g_posix_post_check_user = NULL;
#endif

void rfc_output_set_write_fault_for_testing(rfc_io_fault_stage fault_stage,
                                            uint64_t fail_after_bytes,
                                            rfc_status fail_status) {
    g_test_fault_stage = fault_stage;
    g_test_fault_after_bytes = fail_after_bytes;
    g_test_fault_status = fail_status;
}

void rfc_output_set_commit_barrier_for_testing(
    void (*before_begin_publish)(rfc_job_id job_id, void *user),
    void (*after_begin_publish)(rfc_job_id job_id, void *user),
    void *user) {
    g_before_begin_publish_cb = before_begin_publish;
    g_after_begin_publish_cb = after_begin_publish;
    g_publish_barrier_user = user;
}

void rfc_output_set_posix_post_check_for_testing(
    void (*after_commit_check)(rfc_job_id job_id, void *user),
    void (*after_abort_check)(rfc_job_id job_id, void *user),
    void *user) {
#ifdef _WIN32
    (void)after_commit_check;
    (void)after_abort_check;
    (void)user;
#else
    g_after_commit_check_cb = after_commit_check;
    g_after_abort_check_cb = after_abort_check;
    g_posix_post_check_user = user;
#endif
}

static void copy_export_detail(char *dst, size_t cap, const char *src) {
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

static int validate_utf8_path(const char *s, size_t max_bytes) {
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

#ifdef _WIN32
static int eq_ascii_ci_len(const char *a, size_t alen, const char *lit) {
    size_t blen = strlen(lit);
    if (alen != blen) {
        return 0;
    }
    for (size_t i = 0u; i < alen; i++) {
        unsigned char ca = (unsigned char)a[i];
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

static int is_win32_unsafe_path(const char *path_utf8) {
    if (!path_utf8 || path_utf8[0] == '\0') {
        return 1;
    }
    unsigned char p0 = (unsigned char)path_utf8[0];
    if (p0 == '\\' || p0 == '/') {
        unsigned char p1 = (unsigned char)path_utf8[1];
        if ((p1 == '\\' || p1 == '/') &&
            (path_utf8[2] == '.' || path_utf8[2] == '?') &&
            (path_utf8[3] == '\\' || path_utf8[3] == '/')) {
            return 1;
        }
        if (p1 == '?' && path_utf8[2] == '?' &&
            (path_utf8[3] == '\\' || path_utf8[3] == '/')) {
            return 1;
        }
    }
    const char *scan = path_utf8;
    if (((scan[0] >= 'A' && scan[0] <= 'Z') ||
         (scan[0] >= 'a' && scan[0] <= 'z')) &&
        scan[1] == ':') {
        scan += 2;
    }
    size_t slen = strlen(scan);
    if (slen == 0u || scan[slen - 1u] == '/' || scan[slen - 1u] == '\\') {
        return 1;
    }
    for (const char *p = scan; *p; p++) {
        if (*p == ':') {
            return 1; /* Reject Alternate Data Streams. */
        }
    }
    static const char *const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM0", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT0", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    const char *p = scan;
    while (*p != '\0') {
        while (*p == '/' || *p == '\\') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *comp = p;
        while (*p != '\0' && *p != '/' && *p != '\\') {
            p++;
        }
        size_t comp_len = (size_t)(p - comp);
        if ((comp_len == 1u && comp[0] == '.') ||
            (comp_len == 2u && comp[0] == '.' && comp[1] == '.')) {
            if (*p == '\0') {
                return 1;
            }
            continue;
        }
        if (comp[comp_len - 1u] == ' ' || comp[comp_len - 1u] == '.') {
            return 1;
        }
        size_t stem_len = 0u;
        while (stem_len < comp_len && comp[stem_len] != '.') {
            stem_len++;
        }
        while (stem_len > 0u && comp[stem_len - 1u] == ' ') {
            stem_len--;
        }
        for (size_t i = 0u; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
            if (eq_ascii_ci_len(comp, stem_len, reserved[i])) {
                return 1;
            }
        }
    }
    return 0;
}

static rfc_status utf8_to_wide_out(const char *utf8,
                                    wchar_t *out_buf,
                                    size_t out_cap) {
    if (!validate_utf8_path(utf8, 3800u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                out_buf, (int)out_cap);
    if (n <= 0) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

static rfc_status map_win32_write_error(DWORD err) {
    if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
        return RFC_ERR_DISK_FULL;
    }
    if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION ||
        err == ERROR_LOCK_VIOLATION || err == ERROR_WRITE_PROTECT) {
        return RFC_ERR_PERMISSION_DENIED;
    }
    if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
        return RFC_ERR_DESTINATION_EXISTS;
    }
    return RFC_ERR_IO;
}

static int win_handle_is_symlink_or_junction(HANDLE h, DWORD attrs) {
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0u) {
        return 0;
    }
    FILE_ATTRIBUTE_TAG_INFO tag_info;
    memset(&tag_info, 0, sizeof(tag_info));
    if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag_info,
                                     sizeof(tag_info))) {
        return tag_info.ReparseTag == IO_REPARSE_TAG_SYMLINK ||
               tag_info.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;
    }
    return 1;
}

static void fill_win_identity_from_handle(HANDLE h,
                                          DWORD attrs,
                                          rfc_file_identity *out_id) {
    out_id->is_symlink_or_reparse = win_handle_is_symlink_or_junction(h, attrs);
    BY_HANDLE_FILE_INFORMATION bhfi;
    memset(&bhfi, 0, sizeof(bhfi));
    if (GetFileInformationByHandle(h, &bhfi)) {
        out_id->valid = 1;
        out_id->volume_id_lo = (uint64_t)bhfi.dwVolumeSerialNumber;
        out_id->file_id_lo = ((uint64_t)bhfi.nFileIndexHigh << 32) |
                             (uint64_t)bhfi.nFileIndexLow;
        out_id->link_count = (uint64_t)bhfi.nNumberOfLinks;
    }

    FILE_ID_INFO fid;
    memset(&fid, 0, sizeof(fid));
    if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof(fid))) {
        out_id->valid = 1;
        out_id->volume_id_lo = fid.VolumeSerialNumber;
        uint64_t lo = 0u, hi = 0u;
        memcpy(&lo, fid.FileId.Identifier, 8u);
        memcpy(&hi, fid.FileId.Identifier + 8u, 8u);
        out_id->file_id_lo = lo;
        out_id->file_id_hi = hi;
    }
}

static rfc_status win_query_identity_wide_opts(const wchar_t *wpath,
                                                int follow_reparse,
                                                rfc_file_identity *out_id,
                                                int *out_exists) {
    memset(out_id, 0, sizeof(*out_id));
    *out_exists = 0;

    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return RFC_OK;
        }
        return map_win32_write_error(err);
    }
    *out_exists = 1;

    DWORD open_flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (!follow_reparse) {
        open_flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    HANDLE h = CreateFileW(
        wpath, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, open_flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(
            wpath, 0u, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, open_flags, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            *out_exists = 0;
            return RFC_OK;
        }
        return map_win32_write_error(err);
    }

    fill_win_identity_from_handle(h, attrs, out_id);
    CloseHandle(h);
    return RFC_OK;
}

static rfc_status win_query_identity_wide(const wchar_t *wpath,
                                          rfc_file_identity *out_id,
                                          int *out_exists) {
    return win_query_identity_wide_opts(wpath, 0, out_id, out_exists);
}
#else
static rfc_status map_posix_write_error(int err) {
    if (err == ENOSPC || err == EDQUOT
#ifdef EFBIG
        || err == EFBIG
#endif
    ) {
        return RFC_ERR_DISK_FULL;
    }
    if (err == EACCES || err == EPERM || err == EROFS) {
        return RFC_ERR_PERMISSION_DENIED;
    }
    if (err == EEXIST) {
        return RFC_ERR_DESTINATION_EXISTS;
    }
    if (err == ELOOP) {
        return RFC_ERR_PATH_ALIAS;
    }
    return RFC_ERR_IO;
}
#endif

rfc_status rfc_file_identity_query_path(const char *path_utf8,
                                        rfc_file_identity *out_identity) {
    if (!path_utf8 || !out_identity) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_identity, 0, sizeof(*out_identity));
#ifdef _WIN32
    wchar_t wpath[4096];
    rfc_status st = utf8_to_wide_out(path_utf8, wpath, 4096u);
    if (st != RFC_OK) {
        return st;
    }
    int exists = 0;
    st = win_query_identity_wide(wpath, out_identity, &exists);
    if (st != RFC_OK) {
        return st;
    }
    return exists ? RFC_OK : RFC_ERR_IO;
#else
    if (!validate_utf8_path(path_utf8, 4096u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    struct stat st;
    if (lstat(path_utf8, &st) != 0) {
        return map_posix_write_error(errno);
    }
    out_identity->valid = 1;
    out_identity->is_symlink_or_reparse = S_ISLNK(st.st_mode) ? 1 : 0;
    out_identity->volume_id_lo = (uint64_t)st.st_dev;
    out_identity->file_id_lo = (uint64_t)st.st_ino;
    out_identity->link_count = (uint64_t)st.st_nlink;
    return RFC_OK;
#endif
}

static rfc_status query_followed_file_identity(const char *path_utf8,
                                               rfc_file_identity *out_identity) {
    if (!path_utf8 || !out_identity) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_identity, 0, sizeof(*out_identity));
#ifdef _WIN32
    wchar_t wpath[4096];
    rfc_status st = utf8_to_wide_out(path_utf8, wpath, 4096u);
    if (st != RFC_OK) {
        return st;
    }
    int exists = 0;
    st = win_query_identity_wide_opts(wpath, 1, out_identity, &exists);
    if (st != RFC_OK) {
        return st;
    }
    return exists ? RFC_OK : RFC_ERR_IO;
#else
    if (!validate_utf8_path(path_utf8, 4096u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    struct stat st;
    if (stat(path_utf8, &st) != 0) {
        return map_posix_write_error(errno);
    }
    out_identity->valid = 1;
    out_identity->is_symlink_or_reparse = 0;
    out_identity->volume_id_lo = (uint64_t)st.st_dev;
    out_identity->file_id_lo = (uint64_t)st.st_ino;
    out_identity->link_count = (uint64_t)st.st_nlink;
    return RFC_OK;
#endif
}

int rfc_file_identity_equal(const rfc_file_identity *a,
                            const rfc_file_identity *b) {
    if (!a || !b || !a->valid || !b->valid) {
        return 0;
    }
    return a->volume_id_hi == b->volume_id_hi &&
           a->volume_id_lo == b->volume_id_lo &&
           a->file_id_hi == b->file_id_hi &&
           a->file_id_lo == b->file_id_lo;
}

static rfc_status check_existing_dest_alias(const rfc_file_identity *dest_id,
                                           int dest_is_link,
                                           const char *source_path_utf8,
                                           const rfc_file_identity *source_id) {
    if (dest_is_link || dest_id->is_symlink_or_reparse) {
        return RFC_ERR_PATH_ALIAS;
    }
    if (source_id && rfc_file_identity_equal(dest_id, source_id)) {
        return RFC_ERR_PATH_ALIAS;
    }
    if (source_path_utf8 && validate_utf8_path(source_path_utf8, 3800u)) {
        rfc_file_identity src_id;
        if ((rfc_file_identity_query_path(source_path_utf8, &src_id) ==
                 RFC_OK &&
             rfc_file_identity_equal(dest_id, &src_id)) ||
            (query_followed_file_identity(source_path_utf8, &src_id) ==
                 RFC_OK &&
             rfc_file_identity_equal(dest_id, &src_id))) {
            return RFC_ERR_PATH_ALIAS;
        }
    }
    if (dest_id->link_count > 1u) {
        return RFC_ERR_PATH_ALIAS;
    }
    return RFC_ERR_DESTINATION_EXISTS;
}

#ifdef _WIN32
static void win_close_dir_handles(HANDLE *handles, size_t count) {
    for (size_t i = 0u; i < count; i++) {
        if (handles[i] != INVALID_HANDLE_VALUE && handles[i] != NULL) {
            CloseHandle(handles[i]);
            handles[i] = INVALID_HANDLE_VALUE;
        }
    }
}

static rfc_status win_verify_dir_handles(const HANDLE *handles, size_t count) {
    for (size_t i = 0u; i < count; i++) {
        HANDLE dh = handles[i];
        if (dh == INVALID_HANDLE_VALUE || dh == NULL) {
            return RFC_ERR_IO;
        }
        FILE_BASIC_INFO fbi;
        memset(&fbi, 0, sizeof(fbi));
        if (!GetFileInformationByHandleEx(dh, FileBasicInfo, &fbi, sizeof(fbi))) {
            return map_win32_write_error(GetLastError());
        }
        if ((fbi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            return RFC_ERR_IO;
        }
        if (win_handle_is_symlink_or_junction(dh, fbi.FileAttributes)) {
            return RFC_ERR_PATH_ALIAS;
        }
    }
    return RFC_OK;
}

static wchar_t win_ascii_lower_w(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

static int win_is_sep_w(wchar_t c) {
    return c == L'\\' || c == L'/';
}

static void win_trim_trailing_seps_w(wchar_t *s) {
    size_t len = wcslen(s);
    while (len > 0u && win_is_sep_w(s[len - 1u])) {
        s[--len] = L'\0';
    }
}

void rfc_output_set_win32_unc_root_for_testing(const char *unc_share_utf8,
                                               const char *backing_dir_utf8) {
    g_test_unc_prefix_w[0] = L'\0';
    g_test_unc_backing_w[0] = L'\0';
    if (!unc_share_utf8 || !backing_dir_utf8 || unc_share_utf8[0] == '\0' ||
        backing_dir_utf8[0] == '\0') {
        return;
    }
    if (utf8_to_wide_out(unc_share_utf8, g_test_unc_prefix_w, 1024u) != RFC_OK ||
        utf8_to_wide_out(backing_dir_utf8, g_test_unc_backing_w, 1024u) !=
            RFC_OK) {
        g_test_unc_prefix_w[0] = L'\0';
        g_test_unc_backing_w[0] = L'\0';
        return;
    }
    win_trim_trailing_seps_w(g_test_unc_prefix_w);
    win_trim_trailing_seps_w(g_test_unc_backing_w);
}

static HANDLE win_open_dir_handle_path(const wchar_t *wpath, DWORD share_mode) {
    const wchar_t *open_path = wpath;
    wchar_t mapped[4096];
    if (g_test_unc_prefix_w[0] != L'\0') {
        size_t plen = wcslen(g_test_unc_prefix_w);
        int match = (plen > 0u);
        for (size_t i = 0u; i < plen && match; i++) {
            wchar_t a = wpath[i];
            wchar_t b = g_test_unc_prefix_w[i];
            if (win_is_sep_w(a) && win_is_sep_w(b)) {
                continue;
            }
            if (win_ascii_lower_w(a) != win_ascii_lower_w(b)) {
                match = 0;
            }
        }
        if (match &&
            (wpath[plen] == L'\0' || win_is_sep_w(wpath[plen]))) {
            size_t blen = wcslen(g_test_unc_backing_w);
            const wchar_t *rest = wpath + plen;
            size_t rlen = wcslen(rest);
            int add_drive_slash =
                (blen == 2u && g_test_unc_backing_w[1] == L':' && rlen == 0u);
            if (blen + (add_drive_slash ? 1u : 0u) + rlen + 1u <= 4096u) {
                memcpy(mapped, g_test_unc_backing_w, blen * sizeof(wchar_t));
                size_t mp = blen;
                if (add_drive_slash) {
                    mapped[mp++] = L'\\';
                }
                memcpy(mapped + mp, rest, (rlen + 1u) * sizeof(wchar_t));
                open_path = mapped;
            }
        }
    }
    return CreateFileW(
        open_path, FILE_READ_ATTRIBUTES, share_mode, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
}

static rfc_status win_open_anchored_ancestors(const wchar_t *wdest,
                                              HANDLE *out_handles,
                                              size_t max_handles,
                                              size_t *out_count,
                                              wchar_t *out_dir_path,
                                              size_t dir_cap,
                                              wchar_t *out_base,
                                              size_t base_cap) {
    *out_count = 0u;
    if (out_dir_path && dir_cap > 0u) {
        out_dir_path[0] = L'\0';
    }
    if (out_base && base_cap > 0u) {
        out_base[0] = L'\0';
    }

    wchar_t wabs[4096];
    wchar_t *file_part = NULL;
    DWORD full_n = GetFullPathNameW(wdest, 4096u, wabs, &file_part);
    if (full_n == 0u || full_n >= 4096u || !file_part || file_part[0] == L'\0') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t blen = wcslen(file_part);
    if (out_base) {
        if (blen + 1u > base_cap) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        memcpy(out_base, file_part, (blen + 1u) * sizeof(wchar_t));
    }

    size_t parent_end = (size_t)(file_part - wabs);
    if (parent_end == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t count = 0u;
    size_t pos = 0u;

    if (((wabs[0] >= L'A' && wabs[0] <= L'Z') ||
         (wabs[0] >= L'a' && wabs[0] <= L'z')) &&
        wabs[1] == L':' && (wabs[2] == L'\\' || wabs[2] == L'/')) {
        wchar_t saved = wabs[3];
        wabs[3] = L'\0';
        HANDLE rh = win_open_dir_handle_path(
            wabs, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        wabs[3] = saved;
        if (rh == INVALID_HANDLE_VALUE) {
            return map_win32_write_error(GetLastError());
        }
        if (count >= max_handles) {
            CloseHandle(rh);
            return RFC_ERR_INVALID_ARGUMENT;
        }
        out_handles[count++] = rh;
        rfc_status vst = win_verify_dir_handles(out_handles + (count - 1u), 1u);
        if (vst != RFC_OK) {
            win_close_dir_handles(out_handles, count);
            *out_count = 0u;
            return vst;
        }
        pos = 3u;
    } else if ((wabs[0] == L'\\' || wabs[0] == L'/') &&
               (wabs[1] == L'\\' || wabs[1] == L'/')) {
        /* UNC share prefix \\server\share */
        size_t s = 2u;
        size_t srv_start = s;
        while (s < parent_end && wabs[s] != L'\\' && wabs[s] != L'/') {
            s++;
        }
        if (s == srv_start || s >= parent_end) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        while (s < parent_end && (wabs[s] == L'\\' || wabs[s] == L'/')) {
            s++;
        }
        if (s >= parent_end) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        size_t shr_start = s;
        while (s < parent_end && wabs[s] != L'\\' && wabs[s] != L'/') {
            s++;
        }
        if (s == shr_start) {
            return RFC_ERR_INVALID_ARGUMENT;
        }
        size_t root_end =
            (s < parent_end && (wabs[s] == L'\\' || wabs[s] == L'/')) ? (s + 1u)
                                                                      : s;
        wchar_t saved = wabs[root_end];
        wabs[root_end] = L'\0';
        HANDLE rh = win_open_dir_handle_path(
            wabs, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        wabs[root_end] = saved;
        if (rh == INVALID_HANDLE_VALUE) {
            return map_win32_write_error(GetLastError());
        }
        if (count >= max_handles) {
            CloseHandle(rh);
            return RFC_ERR_INVALID_ARGUMENT;
        }
        out_handles[count++] = rh;
        rfc_status vst = win_verify_dir_handles(out_handles + (count - 1u), 1u);
        if (vst != RFC_OK) {
            win_close_dir_handles(out_handles, count);
            *out_count = 0u;
            return vst;
        }
        pos = root_end;
    }

    while (pos < parent_end) {
        while (pos < parent_end && (wabs[pos] == L'\\' || wabs[pos] == L'/')) {
            pos++;
        }
        if (pos >= parent_end) {
            break;
        }
        size_t next = pos;
        while (next < parent_end && wabs[next] != L'\\' && wabs[next] != L'/') {
            next++;
        }
        wchar_t saved = wabs[next];
        wabs[next] = L'\0';
        int is_leaf_parent = 1;
        for (size_t k = next; k < parent_end; k++) {
            if (wabs[k] != L'\\' && wabs[k] != L'/' && wabs[k] != L'\0') {
                is_leaf_parent = 0;
                break;
            }
        }
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        HANDLE dh = win_open_dir_handle_path(wabs, share);
        if (dh == INVALID_HANDLE_VALUE &&
            GetLastError() == ERROR_SHARING_VIOLATION && !is_leaf_parent) {
            dh = win_open_dir_handle_path(wabs, share | FILE_SHARE_DELETE);
        }
        if (dh == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            wabs[next] = saved;
            win_close_dir_handles(out_handles, count);
            *out_count = 0u;
            return map_win32_write_error(err);
        }
        if (count >= max_handles) {
            CloseHandle(dh);
            wabs[next] = saved;
            win_close_dir_handles(out_handles, count);
            *out_count = 0u;
            return RFC_ERR_INVALID_ARGUMENT;
        }
        out_handles[count++] = dh;
        rfc_status vst = win_verify_dir_handles(out_handles + (count - 1u), 1u);
        wabs[next] = saved;
        if (vst != RFC_OK) {
            win_close_dir_handles(out_handles, count);
            *out_count = 0u;
            return vst;
        }
        pos = next + 1u;
    }

    if (count == 0u) {
        return RFC_ERR_IO;
    }

    if (out_dir_path && dir_cap > 0u) {
        HANDLE leaf_h = out_handles[count - 1u];
        DWORD fn = GetFinalPathNameByHandleW(
            leaf_h, out_dir_path, (DWORD)dir_cap,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (fn == 0u || fn >= (DWORD)dir_cap) {
            if (parent_end > 3u &&
                (wabs[parent_end - 1u] == L'\\' ||
                 wabs[parent_end - 1u] == L'/')) {
                parent_end--;
            }
            if (parent_end >= dir_cap) {
                win_close_dir_handles(out_handles, count);
                *out_count = 0u;
                return RFC_ERR_INVALID_ARGUMENT;
            }
            memcpy(out_dir_path, wabs, parent_end * sizeof(wchar_t));
            out_dir_path[parent_end] = L'\0';
        }
    }
    *out_count = count;
    return RFC_OK;
}

static rfc_status win_join_dir_and_name(const wchar_t *dir_path,
                                        const wchar_t *name,
                                        wchar_t *out,
                                        size_t out_cap) {
    size_t dlen = wcslen(dir_path);
    size_t nlen = wcslen(name);
    if (dlen == 0u || nlen == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int has_sep = (dir_path[dlen - 1u] == L'\\' || dir_path[dlen - 1u] == L'/');
    size_t need = dlen + (has_sep ? 0u : 1u) + nlen + 1u;
    if (need > out_cap) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memcpy(out, dir_path, dlen * sizeof(wchar_t));
    size_t p = dlen;
    if (!has_sep) {
        out[p++] = L'\\';
    }
    memcpy(out + p, name, (nlen + 1u) * sizeof(wchar_t));
    return RFC_OK;
}
#else
void rfc_output_set_win32_unc_root_for_testing(const char *unc_share_utf8,
                                               const char *backing_dir_utf8) {
    (void)unc_share_utf8;
    (void)backing_dir_utf8;
}

static rfc_status posix_open_anchored_parent(const char *dest_path_utf8,
                                             int *out_dir_fd,
                                             char *out_base,
                                             size_t base_cap) {
    *out_dir_fd = -1;
    if (out_base && base_cap > 0u) {
        out_base[0] = '\0';
    }
    if (!dest_path_utf8 || dest_path_utf8[0] == '\0') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    size_t dlen = strlen(dest_path_utf8);
    if (dlen == 0u || dlen > 3800u || dest_path_utf8[dlen - 1u] == '/') {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    int dfd = -1;
    size_t pos = 0u;
    if (dest_path_utf8[0] == '/') {
        dfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        pos = 1u;
    } else {
        dfd = open(".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (dfd < 0) {
        return map_posix_write_error(errno);
    }

    while (pos < dlen) {
        while (pos < dlen && dest_path_utf8[pos] == '/') {
            pos++;
        }
        if (pos >= dlen) {
            close(dfd);
            return RFC_ERR_INVALID_ARGUMENT;
        }
        const char *slash = strchr(dest_path_utf8 + pos, '/');
        if (!slash) {
            size_t clen = dlen - pos;
            if (clen == 0u || clen >= base_cap ||
                (clen == 1u && dest_path_utf8[pos] == '.') ||
                (clen == 2u && dest_path_utf8[pos] == '.' &&
                 dest_path_utf8[pos + 1u] == '.')) {
                close(dfd);
                return RFC_ERR_INVALID_ARGUMENT;
            }
            if (out_base) {
                memcpy(out_base, dest_path_utf8 + pos, clen + 1u);
            }
            *out_dir_fd = dfd;
            return RFC_OK;
        }

        size_t clen = (size_t)(slash - (dest_path_utf8 + pos));
        if (clen == 0u || clen >= 512u) {
            close(dfd);
            return RFC_ERR_INVALID_ARGUMENT;
        }
        char comp[512];
        memcpy(comp, dest_path_utf8 + pos, clen);
        comp[clen] = '\0';
        pos = (size_t)(slash - dest_path_utf8) + 1u;

        if (strcmp(comp, ".") == 0) {
            continue;
        }
        if (strcmp(comp, "..") != 0) {
            struct stat pst;
            if (fstatat(dfd, comp, &pst, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISLNK(pst.st_mode)) {
                close(dfd);
                return RFC_ERR_PATH_ALIAS;
            }
        }
        int next_fd = openat(dfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dfd);
        if (next_fd < 0) {
            return (errno == ELOOP) ? RFC_ERR_PATH_ALIAS
                                    : map_posix_write_error(errno);
        }
        dfd = next_fd;
    }

    close(dfd);
    return RFC_ERR_INVALID_ARGUMENT;
}

static rfc_status posix_check_dest_in_dir(int dir_fd,
                                          const char *dest_base,
                                          const char *source_path_utf8,
                                          const rfc_file_identity *source_id,
                                          int check_write_access) {
    struct stat lst;
    if (fstatat(dir_fd, dest_base, &lst, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(lst.st_mode)) {
            return RFC_ERR_PATH_ALIAS;
        }
        rfc_file_identity dest_id;
        memset(&dest_id, 0, sizeof(dest_id));
        dest_id.valid = 1;
        dest_id.volume_id_lo = (uint64_t)lst.st_dev;
        dest_id.file_id_lo = (uint64_t)lst.st_ino;
        dest_id.link_count = (uint64_t)lst.st_nlink;
        return check_existing_dest_alias(&dest_id, 0, source_path_utf8,
                                         source_id);
    } else if (errno != ENOENT) {
        return map_posix_write_error(errno);
    }

    if (check_write_access && faccessat(dir_fd, ".", W_OK | X_OK, 0) != 0) {
        return map_posix_write_error(errno);
    }
    return RFC_OK;
}
#endif

rfc_status rfc_output_check_destination_path(const char *dest_path_utf8,
                                             const char *source_path_utf8,
                                             const rfc_file_identity *source_id,
                                             uint32_t export_flags) {
    if (export_flags != RFC_EXPORT_REFUSE_EXISTING) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (!validate_utf8_path(dest_path_utf8, 3800u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    if (is_win32_unsafe_path(dest_path_utf8)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    wchar_t wdest[4096];
    rfc_status st = utf8_to_wide_out(dest_path_utf8, wdest, 4096u);
    if (st != RFC_OK) {
        return st;
    }
    HANDLE handles[64];
    size_t handle_count = 0u;
    wchar_t wdir[4096];
    wchar_t wbase[512];
    st = win_open_anchored_ancestors(wdest, handles, 64u, &handle_count, wdir,
                                     4096u, wbase, 512u);
    if (st != RFC_OK) {
        return st;
    }
    wchar_t wnorm[4096];
    st = win_join_dir_and_name(wdir, wbase, wnorm, 4096u);
    win_close_dir_handles(handles, handle_count);
    if (st != RFC_OK) {
        return st;
    }
    rfc_file_identity dest_id;
    int exists = 0;
    st = win_query_identity_wide(wnorm, &dest_id, &exists);
    if (st != RFC_OK) {
        return st;
    }
    if (exists) {
        return check_existing_dest_alias(&dest_id, dest_id.is_symlink_or_reparse,
                                         source_path_utf8, source_id);
    }
    return RFC_OK;
#else
    int dir_fd = -1;
    char base[512];
    rfc_status st = posix_open_anchored_parent(dest_path_utf8, &dir_fd, base,
                                               sizeof(base));
    if (st != RFC_OK) {
        return st;
    }
    st = posix_check_dest_in_dir(dir_fd, base, source_path_utf8, source_id, 1);
    close(dir_fd);
    return st;
#endif
}

rfc_status rfc_output_publish_staged(const rfc_io_publisher *publisher,
                                     rfc_job *job,
                                     rfc_job_id job_id,
                                     uint64_t attempt_number,
                                     const rfc_file_identity *source_id,
                                     const uint8_t *bytes,
                                     size_t size,
                                     const rfc_exec_context *exec,
                                     rfc_export_report *out_report) {
    rfc_export_report rep;
    memset(&rep, 0, sizeof(rep));

    if (!publisher || !publisher->begin_stage || !publisher->write_stage ||
        !publisher->flush_stage || !publisher->commit_stage ||
        !publisher->abort_stage || (size > 0u && !bytes)) {
        rep.status = RFC_ERR_INVALID_ARGUMENT;
        rep.valid = 1;
        if (out_report) {
            *out_report = rep;
        }
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_bytes_hash_sha256(bytes, size, rep.output_sha256);

    if ((job && rfc_job_is_cancelled(job)) ||
        (exec && rfc_exec_is_cancelled(exec))) {
        rep.status = RFC_ERR_CANCELLED;
        rep.temp_cleaned_up = 1;
        rep.valid = 1;
        copy_export_detail(rep.detail, sizeof(rep.detail),
                           "cancelled before staging");
        if (out_report) {
            *out_report = rep;
        }
        return RFC_ERR_CANCELLED;
    }

    void *stage_handle = NULL;
    rfc_status st = publisher->begin_stage(
        publisher->ctx, job_id, attempt_number, (uint64_t)size, source_id,
        &stage_handle);
    if (st != RFC_OK) {
        rep.status = st;
        rep.temp_cleaned_up = 1;
        rep.valid = 1;
        copy_export_detail(rep.detail, sizeof(rep.detail), rfc_status_name(st));
        if (out_report) {
            *out_report = rep;
        }
        return st;
    }

    size_t offset = 0u;
    int64_t tracked_partial = 0;
    while (offset < size) {
        if ((job && rfc_job_is_cancelled(job)) ||
            (exec && rfc_exec_is_cancelled(exec))) {
            st = RFC_ERR_CANCELLED;
            break;
        }
        size_t chunk = size - offset;
        if (chunk > RFC_OUTPUT_CHUNK_BYTES) {
            chunk = RFC_OUTPUT_CHUNK_BYTES;
        }
        if (g_test_fault_stage == RFC_IO_FAULT_WRITE_DISK_FULL ||
            g_test_fault_stage == RFC_IO_FAULT_WRITE_IO) {
            if ((uint64_t)offset >= g_test_fault_after_bytes) {
                st = g_test_fault_status != RFC_OK ? g_test_fault_status
                                                   : RFC_ERR_DISK_FULL;
                break;
            }
            uint64_t rem_to_fault = g_test_fault_after_bytes - (uint64_t)offset;
            if ((uint64_t)chunk > rem_to_fault) {
                chunk = (size_t)rem_to_fault;
            }
        }
        st = publisher->write_stage(publisher->ctx, stage_handle,
                                    bytes + offset, chunk);
        if (st != RFC_OK) {
            break;
        }
        offset += chunk;
        rep.bytes_written = (uint64_t)offset;
        if (job) {
            rfc_job_internal_note_partial_bytes(job, (int64_t)chunk);
            tracked_partial += (int64_t)chunk;
        }
    }

    if (st == RFC_OK) {
        if ((job && rfc_job_is_cancelled(job)) ||
            (exec && rfc_exec_is_cancelled(exec))) {
            st = RFC_ERR_CANCELLED;
        } else if (g_test_fault_stage == RFC_IO_FAULT_FLUSH_IO) {
            st = g_test_fault_status != RFC_OK ? g_test_fault_status
                                               : RFC_ERR_IO;
        } else {
            st = publisher->flush_stage(publisher->ctx, stage_handle);
        }
    }

    if (st != RFC_OK) {
        rfc_status clean_st = publisher->abort_stage(publisher->ctx, stage_handle);
        int cleaned = (clean_st == RFC_OK) ? 1 : 0;
        if (job && cleaned && tracked_partial > 0) {
            rfc_job_internal_note_partial_bytes(job, -tracked_partial);
        }
        rep.status = st;
        rep.published = 0;
        rep.temp_cleaned_up = cleaned;
        rep.valid = 1;
        copy_export_detail(rep.detail, sizeof(rep.detail), rfc_status_name(st));
        if (out_report) {
            *out_report = rep;
        }
        return st;
    }

    if (g_before_begin_publish_cb) {
        g_before_begin_publish_cb(job_id, g_publish_barrier_user);
    }

    if ((job && rfc_job_is_cancelled(job)) ||
        (exec && rfc_exec_is_cancelled(exec))) {
        st = RFC_ERR_CANCELLED;
    } else if (job) {
        st = rfc_job_internal_begin_publish(job, job_id);
    }
    if (st != RFC_OK) {
        rfc_status clean_st = publisher->abort_stage(publisher->ctx, stage_handle);
        int cleaned = (clean_st == RFC_OK) ? 1 : 0;
        if (job && cleaned && tracked_partial > 0) {
            rfc_job_internal_note_partial_bytes(job, -tracked_partial);
        }
        rep.status = st;
        rep.published = 0;
        rep.temp_cleaned_up = cleaned;
        rep.valid = 1;
        copy_export_detail(rep.detail, sizeof(rep.detail), rfc_status_name(st));
        if (out_report) {
            *out_report = rep;
        }
        return st;
    }

    if (g_after_begin_publish_cb) {
        g_after_begin_publish_cb(job_id, g_publish_barrier_user);
    }

    int commit_cleaned = 1;
    if (g_test_fault_stage == RFC_IO_FAULT_COMMIT_EXISTS) {
        st = RFC_ERR_DESTINATION_EXISTS;
    } else if (g_test_fault_stage == RFC_IO_FAULT_COMMIT_IO) {
        st = g_test_fault_status != RFC_OK ? g_test_fault_status : RFC_ERR_IO;
    } else {
        st = publisher->commit_stage(publisher->ctx, stage_handle,
                                     &commit_cleaned);
    }

    if (st != RFC_OK) {
        rfc_status clean_st = publisher->abort_stage(publisher->ctx, stage_handle);
        int cleaned = (clean_st == RFC_OK) ? 1 : 0;
        if (job) {
            rfc_job_internal_finish_publish(job, job_id, 0);
            if (cleaned && tracked_partial > 0) {
                rfc_job_internal_note_partial_bytes(job, -tracked_partial);
            }
        }
        rep.status = st;
        rep.published = 0;
        rep.temp_cleaned_up = cleaned;
        rep.valid = 1;
        copy_export_detail(rep.detail, sizeof(rep.detail), rfc_status_name(st));
        if (out_report) {
            *out_report = rep;
        }
        return st;
    }

    if (job) {
        rfc_job_internal_finish_publish(job, job_id, 1);
        if (commit_cleaned && tracked_partial > 0) {
            rfc_job_internal_note_partial_bytes(job, -tracked_partial);
        }
    }
    rep.status = RFC_OK;
    rep.published = 1;
    rep.temp_cleaned_up = commit_cleaned ? 1 : 0;
    rep.valid = 1;
    copy_export_detail(rep.detail, sizeof(rep.detail), "published");
    if (out_report) {
        *out_report = rep;
    }
    return RFC_OK;
}

typedef struct path_pub_ctx {
    const char *dest_utf8;
    const char *source_utf8;
    const rfc_file_identity *source_id;
    uint32_t export_flags;
} path_pub_ctx;

typedef struct path_stage_handle {
    rfc_job_id job_id;
#ifdef _WIN32
    HANDLE handle;
    HANDLE dir_handles[64];
    size_t dir_handle_count;
    rfc_file_identity staged_id;
    wchar_t wdir_norm[4096];
    wchar_t wbase[512];
    wchar_t wdest_norm[4096];
    wchar_t wtmp_norm[4096];
#else
    int dir_fd;
    int fd;
    dev_t staged_dev;
    ino_t staged_ino;
    char dest_base[512];
#endif
    int flushed;
} path_stage_handle;

static rfc_status path_pub_begin(void *raw_ctx,
                                 rfc_job_id job_id,
                                 uint64_t attempt_number,
                                 uint64_t expected_bytes,
                                 const rfc_file_identity *source_id,
                                 void **out_handle) {
    (void)expected_bytes;
    path_pub_ctx *ctx = (path_pub_ctx *)raw_ctx;
    *out_handle = NULL;

    if (ctx->export_flags != RFC_EXPORT_REFUSE_EXISTING) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (!validate_utf8_path(ctx->dest_utf8, 3800u)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    path_stage_handle *sh = (path_stage_handle *)calloc(1u, sizeof(*sh));
    if (!sh) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    sh->job_id = job_id;
#ifdef _WIN32
    uint64_t seq = atomic_fetch_add_explicit(&g_partial_seq, 1u,
                                             memory_order_relaxed);
    sh->handle = INVALID_HANDLE_VALUE;
    for (size_t i = 0u; i < 64u; i++) {
        sh->dir_handles[i] = INVALID_HANDLE_VALUE;
    }
    if (is_win32_unsafe_path(ctx->dest_utf8)) {
        free(sh);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    wchar_t wdest_raw[4096];
    rfc_status st = utf8_to_wide_out(ctx->dest_utf8, wdest_raw, 4096u);
    if (st != RFC_OK) {
        free(sh);
        return st;
    }
    st = win_open_anchored_ancestors(wdest_raw, sh->dir_handles, 64u,
                                     &sh->dir_handle_count, sh->wdir_norm,
                                     4096u, sh->wbase, 512u);
    if (st != RFC_OK) {
        free(sh);
        return st;
    }
    st = win_join_dir_and_name(sh->wdir_norm, sh->wbase, sh->wdest_norm, 4096u);
    if (st != RFC_OK) {
        win_close_dir_handles(sh->dir_handles, sh->dir_handle_count);
        free(sh);
        return st;
    }
    rfc_file_identity dest_id;
    int exists = 0;
    st = win_query_identity_wide(sh->wdest_norm, &dest_id, &exists);
    if (st != RFC_OK) {
        win_close_dir_handles(sh->dir_handles, sh->dir_handle_count);
        free(sh);
        return st;
    }
    if (exists) {
        win_close_dir_handles(sh->dir_handles, sh->dir_handle_count);
        free(sh);
        return check_existing_dest_alias(
            &dest_id, dest_id.is_symlink_or_reparse, ctx->source_utf8,
            source_id ? source_id : ctx->source_id);
    }

    int n = _snwprintf(sh->wtmp_norm, 4096u,
                       L"%ls.rfc-partial.%lu.%llu.%llu.%llu",
                       sh->wdest_norm, (unsigned long)GetCurrentProcessId(),
                       (unsigned long long)job_id,
                       (unsigned long long)attempt_number,
                       (unsigned long long)seq);
    if (n <= 0 || n >= 4096) {
        win_close_dir_handles(sh->dir_handles, sh->dir_handle_count);
        free(sh);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    sh->handle = CreateFileW(sh->wtmp_norm, GENERIC_WRITE | DELETE, 0u, NULL,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (sh->handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        win_close_dir_handles(sh->dir_handles, sh->dir_handle_count);
        free(sh);
        return map_win32_write_error(err);
    }
    fill_win_identity_from_handle(sh->handle, FILE_ATTRIBUTE_NORMAL,
                                  &sh->staged_id);
#else
    (void)attempt_number;
    sh->dir_fd = -1;
    sh->fd = -1;
    rfc_status st = posix_open_anchored_parent(
        ctx->dest_utf8, &sh->dir_fd, sh->dest_base, sizeof(sh->dest_base));
    if (st != RFC_OK) {
        free(sh);
        return st;
    }
    st = posix_check_dest_in_dir(sh->dir_fd, sh->dest_base, ctx->source_utf8,
                                 source_id ? source_id : ctx->source_id, 0);
    if (st != RFC_OK) {
        close(sh->dir_fd);
        free(sh);
        return st;
    }
    if (g_test_fault_stage == RFC_IO_FAULT_POSIX_NO_TMPFILE) {
        sh->fd = -1;
        errno = EOPNOTSUPP;
    } else {
#ifdef O_TMPFILE
        do {
            sh->fd = openat(sh->dir_fd, ".", O_TMPFILE | O_WRONLY | O_CLOEXEC,
                            0644);
        } while (sh->fd < 0 && errno == EINTR);
#else
        sh->fd = -1;
        errno = EOPNOTSUPP;
#endif
    }
    if (sh->fd < 0) {
        int err = errno;
        close(sh->dir_fd);
        free(sh);
        return map_posix_write_error(err);
    }
    struct stat sst;
    if (fstat(sh->fd, &sst) != 0) {
        int err = errno;
        close(sh->fd);
        close(sh->dir_fd);
        free(sh);
        return map_posix_write_error(err);
    }
    sh->staged_dev = sst.st_dev;
    sh->staged_ino = sst.st_ino;
#endif
    *out_handle = sh;
    return RFC_OK;
}

static rfc_status path_pub_write(void *raw_ctx,
                                 void *raw_handle,
                                 const uint8_t *data,
                                 size_t len) {
    (void)raw_ctx;
    path_stage_handle *sh = (path_stage_handle *)raw_handle;
    size_t done = 0u;
    while (done < len) {
#ifdef _WIN32
        DWORD want = (DWORD)((len - done) > 0x10000000u ? 0x10000000u
                                                        : (len - done));
        DWORD put = 0u;
        if (!WriteFile(sh->handle, data + done, want, &put, NULL) || put == 0u) {
            return map_win32_write_error(GetLastError());
        }
        done += (size_t)put;
#else
        ssize_t put = write(sh->fd, data + done, len - done);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return map_posix_write_error(errno);
        }
        if (put == 0) {
            return RFC_ERR_IO;
        }
        done += (size_t)put;
#endif
    }
    return RFC_OK;
}

static rfc_status path_pub_flush(void *raw_ctx, void *raw_handle) {
    (void)raw_ctx;
    path_stage_handle *sh = (path_stage_handle *)raw_handle;
#ifdef _WIN32
    if (!FlushFileBuffers(sh->handle)) {
        return map_win32_write_error(GetLastError());
    }
    sh->flushed = 1;
#else
    if (fsync(sh->fd) != 0 && errno != EINVAL) {
        return map_posix_write_error(errno);
    }
    sh->flushed = 1;
#endif
    return RFC_OK;
}

static rfc_status path_pub_commit(void *raw_ctx,
                                  void *raw_handle,
                                  int *out_temp_cleaned_up) {
    path_pub_ctx *ctx = (path_pub_ctx *)raw_ctx;
    path_stage_handle *sh = (path_stage_handle *)raw_handle;
    if (out_temp_cleaned_up) {
        *out_temp_cleaned_up = 1;
    }

#ifdef _WIN32
    rfc_status st = win_verify_dir_handles(sh->dir_handles,
                                           sh->dir_handle_count);
    if (st != RFC_OK) {
        return st;
    }
    if (sh->dir_handle_count == 0u) {
        return RFC_ERR_IO;
    }
    wchar_t cur_dir[4096];
    DWORD fn = GetFinalPathNameByHandleW(
        sh->dir_handles[sh->dir_handle_count - 1u], cur_dir, 4096u,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (fn > 0u && fn < 4096u && wcscmp(cur_dir, sh->wdir_norm) != 0) {
        return RFC_ERR_PATH_ALIAS;
    }

    rfc_file_identity dest_id;
    int exists = 0;
    st = win_query_identity_wide(sh->wdest_norm, &dest_id, &exists);
    if (st != RFC_OK) {
        return st;
    }
    if (exists) {
        return check_existing_dest_alias(&dest_id, dest_id.is_symlink_or_reparse,
                                         ctx->source_utf8, ctx->source_id);
    }

    rfc_file_identity tmp_id;
    int tmp_exists = 0;
    st = win_query_identity_wide(sh->wtmp_norm, &tmp_id, &tmp_exists);
    if (st != RFC_OK || !tmp_exists || tmp_id.is_symlink_or_reparse ||
        !rfc_file_identity_equal(&tmp_id, &sh->staged_id)) {
        return (tmp_exists && tmp_id.is_symlink_or_reparse) ? RFC_ERR_PATH_ALIAS
                                                           : RFC_ERR_IO;
    }

    size_t dbytes = wcslen(sh->wdest_norm) * sizeof(wchar_t);
    size_t fri_sz = sizeof(FILE_RENAME_INFO) + dbytes + sizeof(wchar_t);
    FILE_RENAME_INFO *fri = (FILE_RENAME_INFO *)calloc(1u, fri_sz);
    if (!fri) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    fri->ReplaceIfExists = FALSE;
    fri->RootDirectory = NULL;
    fri->FileNameLength = (DWORD)dbytes;
    memcpy(fri->FileName, sh->wdest_norm, dbytes + sizeof(wchar_t));

    BOOL ren_ok = SetFileInformationByHandle(sh->handle, FileRenameInfo, fri,
                                             (DWORD)fri_sz);
    DWORD ren_err = ren_ok ? 0u : GetLastError();
    free(fri);
    if (!ren_ok) {
        return map_win32_write_error(ren_err);
    }
    CloseHandle(sh->handle);
    sh->handle = INVALID_HANDLE_VALUE;
    win_close_dir_handles(sh->dir_handles, sh->dir_handle_count);
    free(sh);
    return RFC_OK;
#else
    /* Re-verify destination was not replaced by symlink/hardlink/existing file after staging. */
    rfc_status st = posix_check_dest_in_dir(
        sh->dir_fd, sh->dest_base, ctx->source_utf8, ctx->source_id, 0);
    if (st != RFC_OK) {
        return st;
    }

    struct stat cur_fd;
    if (fstat(sh->fd, &cur_fd) != 0 || !S_ISREG(cur_fd.st_mode) ||
        cur_fd.st_dev != sh->staged_dev || cur_fd.st_ino != sh->staged_ino) {
        return RFC_ERR_IO;
    }

    if (g_after_commit_check_cb) {
        g_after_commit_check_cb(sh->job_id, g_posix_post_check_user);
    }

    char proc_fd[64];
    snprintf(proc_fd, sizeof(proc_fd), "/proc/self/fd/%d", sh->fd);
    int lrc = -1;
    if (g_test_fault_stage == RFC_IO_FAULT_POSIX_PROC_LINK ||
        g_test_fault_stage == RFC_IO_FAULT_POSIX_HANDLE_LINK) {
        errno = EOPNOTSUPP;
    } else {
        lrc = linkat(AT_FDCWD, proc_fd, sh->dir_fd, sh->dest_base,
                     AT_SYMLINK_FOLLOW);
    }
    if (lrc != 0) {
        int first_err = errno;
        if (first_err == EEXIST) {
            rfc_status dst_st = posix_check_dest_in_dir(
                sh->dir_fd, sh->dest_base, ctx->source_utf8, ctx->source_id, 0);
            return (dst_st != RFC_OK) ? dst_st : RFC_ERR_DESTINATION_EXISTS;
        }
        if ((first_err == ENOENT || first_err == ENOSYS ||
             first_err == EOPNOTSUPP || first_err == EACCES ||
             first_err == EPERM) &&
            g_test_fault_stage != RFC_IO_FAULT_POSIX_HANDLE_LINK) {
#ifdef AT_EMPTY_PATH
            lrc = linkat(sh->fd, "", sh->dir_fd, sh->dest_base, AT_EMPTY_PATH);
            if (lrc != 0 && errno == EEXIST) {
                rfc_status dst_st = posix_check_dest_in_dir(
                    sh->dir_fd, sh->dest_base, ctx->source_utf8, ctx->source_id,
                    0);
                return (dst_st != RFC_OK) ? dst_st : RFC_ERR_DESTINATION_EXISTS;
            }
#endif
        }
        if (lrc != 0) {
            if ((first_err == EACCES || first_err == EPERM) &&
                faccessat(sh->dir_fd, ".", W_OK | X_OK, 0) == 0) {
                return RFC_ERR_IO;
            }
            return map_posix_write_error(first_err);
        }
    }

    struct stat pub_st;
    if (fstatat(sh->dir_fd, sh->dest_base, &pub_st, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        S_ISLNK(pub_st.st_mode) || pub_st.st_dev != sh->staged_dev ||
        pub_st.st_ino != sh->staged_ino) {
        return RFC_ERR_IO;
    }

    (void)fsync(sh->dir_fd);
    close(sh->fd);
    sh->fd = -1;
    close(sh->dir_fd);
    sh->dir_fd = -1;
    free(sh);
    return RFC_OK;
#endif
}

static rfc_status path_pub_abort(void *raw_ctx, void *raw_handle) {
    (void)raw_ctx;
    path_stage_handle *sh = (path_stage_handle *)raw_handle;
    if (!sh) {
        return RFC_OK;
    }
    rfc_status clean_st = RFC_OK;
#ifdef _WIN32
    if (sh->handle != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO fdi;
        memset(&fdi, 0, sizeof(fdi));
        fdi.DeleteFile = TRUE;
        if (!SetFileInformationByHandle(sh->handle, FileDispositionInfo, &fdi,
                                        sizeof(fdi))) {
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                clean_st = map_win32_write_error(err);
            }
        }
        CloseHandle(sh->handle);
        sh->handle = INVALID_HANDLE_VALUE;
    }
    win_close_dir_handles(sh->dir_handles, sh->dir_handle_count);
#else
    if (sh->fd >= 0) {
        struct stat cur_fd;
        if (fstat(sh->fd, &cur_fd) != 0 || !S_ISREG(cur_fd.st_mode)) {
            clean_st = RFC_ERR_IO;
        }
        if (g_after_abort_check_cb) {
            g_after_abort_check_cb(sh->job_id, g_posix_post_check_user);
        }
        close(sh->fd);
        sh->fd = -1;
    }
    if (sh->dir_fd >= 0) {
        close(sh->dir_fd);
        sh->dir_fd = -1;
    }
#endif
    free(sh);
    return clean_st;
}

rfc_status rfc_output_write_path(const char *dest_path_utf8,
                                 const char *source_path_utf8,
                                 const rfc_file_identity *source_id,
                                 uint32_t export_flags,
                                 rfc_job *job,
                                 rfc_job_id job_id,
                                 const uint8_t *bytes,
                                 size_t size,
                                 const rfc_exec_context *exec,
                                 rfc_export_report *out_report) {
    path_pub_ctx pctx = {dest_path_utf8, source_path_utf8, source_id,
                         export_flags};
    rfc_io_publisher pub;
    memset(&pub, 0, sizeof(pub));
    pub.ctx = &pctx;
    pub.begin_stage = path_pub_begin;
    pub.write_stage = path_pub_write;
    pub.flush_stage = path_pub_flush;
    pub.commit_stage = path_pub_commit;
    pub.abort_stage = path_pub_abort;

    uint64_t attempt = job ? job->attempt_number : 1u;
    return rfc_output_publish_staged(&pub, job, job_id, attempt, source_id,
                                     bytes, size, exec, out_report);
}

rfc_status rfc_output_validate_sink_preflight(
    const rfc_io_sink *io_sink,
    const rfc_io_source *io_source,
    const rfc_file_identity *source_identity) {
    if (!io_sink) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (io_sink->export_flags != RFC_EXPORT_REFUSE_EXISTING) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if ((io_sink->path_utf8 != NULL) == (io_sink->publisher != NULL)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (io_sink->path_utf8) {
        const char *src_path =
            (io_source && io_source->path_utf8) ? io_source->path_utf8 : NULL;
        return rfc_output_check_destination_path(
            io_sink->path_utf8, src_path, source_identity,
            io_sink->export_flags);
    }
    const rfc_io_publisher *p = io_sink->publisher;
    if (!p->begin_stage || !p->write_stage || !p->flush_stage ||
        !p->commit_stage || !p->abort_stage) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    return RFC_OK;
}

rfc_status rfc_output_publish_job(rfc_job *job,
                                  const rfc_exec_context *exec,
                                  const char **out_err_detail) {
    if (out_err_detail) {
        *out_err_detail = NULL;
    }
    if (!job || !exec || !job->inputs || !job->inputs->has_io_sink) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (!job->has_encoded_output ||
        (job->encoded_output.len > 0u && !job->encoded_output.data)) {
        if (out_err_detail) {
            *out_err_detail = "export stage has no encoded output";
        }
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_export_report rep;
    memset(&rep, 0, sizeof(rep));
    rfc_status st = RFC_OK;

    if (job->inputs->io_sink_path) {
        const rfc_file_identity *src_id =
            job->source_identity.valid ? &job->source_identity : NULL;
        st = rfc_output_write_path(
            job->inputs->io_sink_path, job->inputs->io_source_path, src_id,
            job->inputs->io_export_flags, job, job->job_id,
            job->encoded_output.data, job->encoded_output.len, exec, &rep);
    } else if (job->inputs->io_publisher_box) {
        rfc_io_publisher_box *pbox = job->inputs->io_publisher_box;
        const rfc_file_identity *src_id =
            job->source_identity.valid ? &job->source_identity : NULL;
        rfc_mutex_lock(&pbox->lock);
        st = rfc_output_publish_staged(
            &pbox->publisher, job, job->job_id, job->attempt_number, src_id,
            job->encoded_output.data, job->encoded_output.len, exec, &rep);
        rfc_mutex_unlock(&pbox->lock);
    } else {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_mutex_lock(&job->lock);
    job->export_report = rep;
    if (st != RFC_OK && rep.detail[0] != '\0') {
        rfc_job_internal_set_detail(job->detail, rep.detail);
    }
    rfc_mutex_unlock(&job->lock);

    if (st != RFC_OK && out_err_detail) {
        *out_err_detail = rfc_status_name(st);
    }
    return st;
}
