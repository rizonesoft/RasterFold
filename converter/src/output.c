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

static atomic_uint_fast64_t g_partial_seq = 1u;
static rfc_io_fault_stage g_test_fault_stage = RFC_IO_FAULT_NONE;
static uint64_t g_test_fault_after_bytes = 0u;
static rfc_status g_test_fault_status = RFC_OK;
static void (*g_before_begin_publish_cb)(rfc_job_id job_id, void *user) = NULL;
static void (*g_after_begin_publish_cb)(rfc_job_id job_id, void *user) = NULL;
static void *g_publish_barrier_user = NULL;

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

static rfc_status win_check_parent_reparse_chain(const wchar_t *wpath) {
    wchar_t copy[4096];
    size_t len = wcslen(wpath);
    if (len == 0u || len >= 4096u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memcpy(copy, wpath, (len + 1u) * sizeof(wchar_t));
    for (size_t i = len; i > 0u; i--) {
        size_t idx = i - 1u;
        if (copy[idx] == L'\\' || copy[idx] == L'/') {
            if ((idx == 2u && copy[1] == L':') || idx == 0u) {
                break;
            }
            copy[idx] = L'\0';
            DWORD attrs = GetFileAttributesW(copy);
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
                HANDLE hp = CreateFileW(
                    copy, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                    NULL);
                if (hp != INVALID_HANDLE_VALUE) {
                    int is_link = win_handle_is_symlink_or_junction(hp, attrs);
                    CloseHandle(hp);
                    if (is_link) {
                        return RFC_ERR_PATH_ALIAS;
                    }
                } else {
                    return RFC_ERR_PATH_ALIAS;
                }
            }
        }
    }
    return RFC_OK;
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

static rfc_status posix_check_parent_symlink_chain(const char *path_utf8) {
    char copy[4096];
    size_t len = strlen(path_utf8);
    if (len == 0u || len >= sizeof(copy)) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memcpy(copy, path_utf8, len + 1u);
    for (size_t i = len; i > 0u; i--) {
        size_t idx = i - 1u;
        if (copy[idx] == '/') {
            if (idx == 0u) {
                break;
            }
            copy[idx] = '\0';
            struct stat pst;
            if (lstat(copy, &pst) == 0 && S_ISLNK(pst.st_mode)) {
                return RFC_ERR_PATH_ALIAS;
            }
        }
    }
    return RFC_OK;
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
    st = win_check_parent_reparse_chain(wdest);
    if (st != RFC_OK) {
        return st;
    }
    rfc_file_identity dest_id;
    int exists = 0;
    st = win_query_identity_wide(wdest, &dest_id, &exists);
    if (st != RFC_OK) {
        return st;
    }
    if (exists) {
        if (dest_id.is_symlink_or_reparse) {
            return RFC_ERR_PATH_ALIAS;
        }
        if (source_id && rfc_file_identity_equal(&dest_id, source_id)) {
            return RFC_ERR_PATH_ALIAS;
        }
        if (source_path_utf8 && validate_utf8_path(source_path_utf8, 3800u)) {
            rfc_file_identity src_id;
            if ((rfc_file_identity_query_path(source_path_utf8, &src_id) ==
                     RFC_OK &&
                 rfc_file_identity_equal(&dest_id, &src_id)) ||
                (query_followed_file_identity(source_path_utf8, &src_id) ==
                     RFC_OK &&
                 rfc_file_identity_equal(&dest_id, &src_id))) {
                return RFC_ERR_PATH_ALIAS;
            }
        }
        if (dest_id.link_count > 1u) {
            return RFC_ERR_PATH_ALIAS;
        }
        return RFC_ERR_DESTINATION_EXISTS;
    }
    /* Probe parent directory accessibility / permissions when possible. */
    wchar_t wparent[4096];
    size_t plen = wcslen(wdest);
    memcpy(wparent, wdest, (plen + 1u) * sizeof(wchar_t));
    int found_sep = 0;
    for (size_t i = plen; i > 0u; i--) {
        size_t idx = i - 1u;
        if (wparent[idx] == L'\\' || wparent[idx] == L'/') {
            found_sep = 1;
            if (idx == 2u && wparent[1] == L':') {
                wparent[3] = L'\0';
            } else if (idx == 0u) {
                wparent[1] = L'\0';
            } else {
                wparent[idx] = L'\0';
            }
            break;
        }
    }
    if (!found_sep) {
        if (plen >= 2u && wparent[1] == L':') {
            wparent[2] = L'.';
            wparent[3] = L'\0';
        } else {
            wparent[0] = L'.';
            wparent[1] = L'\0';
        }
    }
    DWORD pattrs = GetFileAttributesW(wparent);
    if (pattrs == INVALID_FILE_ATTRIBUTES) {
        return map_win32_write_error(GetLastError());
    }
    if ((pattrs & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        return RFC_ERR_IO;
    }
    if ((pattrs & FILE_ATTRIBUTE_READONLY) != 0u) {
        /* Directory read-only bit on Windows does not always block files,
         * real ACL enforcement happens at CreateFileW. */
    }
    return RFC_OK;
#else
    size_t dlen = strlen(dest_path_utf8);
    if (dlen == 0u || dest_path_utf8[dlen - 1u] == '/') {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    rfc_status st = posix_check_parent_symlink_chain(dest_path_utf8);
    if (st != RFC_OK) {
        return st;
    }
    struct stat lst;
    if (lstat(dest_path_utf8, &lst) == 0) {
        if (S_ISLNK(lst.st_mode)) {
            return RFC_ERR_PATH_ALIAS;
        }
        rfc_file_identity dest_id;
        memset(&dest_id, 0, sizeof(dest_id));
        dest_id.valid = 1;
        dest_id.volume_id_lo = (uint64_t)lst.st_dev;
        dest_id.file_id_lo = (uint64_t)lst.st_ino;
        dest_id.link_count = (uint64_t)lst.st_nlink;
        if (source_id && rfc_file_identity_equal(&dest_id, source_id)) {
            return RFC_ERR_PATH_ALIAS;
        }
        if (source_path_utf8 && validate_utf8_path(source_path_utf8, 3800u)) {
            rfc_file_identity src_id;
            if ((rfc_file_identity_query_path(source_path_utf8, &src_id) ==
                     RFC_OK &&
                 rfc_file_identity_equal(&dest_id, &src_id)) ||
                (query_followed_file_identity(source_path_utf8, &src_id) ==
                     RFC_OK &&
                 rfc_file_identity_equal(&dest_id, &src_id))) {
                return RFC_ERR_PATH_ALIAS;
            }
        }
        if (lst.st_nlink > 1) {
            return RFC_ERR_PATH_ALIAS;
        }
        return RFC_ERR_DESTINATION_EXISTS;
    } else if (errno != ENOENT) {
        return map_posix_write_error(errno);
    }

    char parent[4096];
    memcpy(parent, dest_path_utf8, dlen + 1u);
    int found_sep = 0;
    for (size_t i = dlen; i > 0u; i--) {
        size_t idx = i - 1u;
        if (parent[idx] == '/') {
            found_sep = 1;
            if (idx == 0u) {
                parent[1] = '\0';
            } else {
                parent[idx] = '\0';
            }
            break;
        }
    }
    if (access(found_sep ? parent : ".", W_OK | X_OK) != 0) {
        return map_posix_write_error(errno);
    }
    return RFC_OK;
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
        publisher->abort_stage(publisher->ctx, stage_handle);
        if (job && tracked_partial > 0) {
            rfc_job_internal_note_partial_bytes(job, -tracked_partial);
        }
        rep.status = st;
        rep.published = 0;
        rep.temp_cleaned_up = 1;
        rep.valid = 1;
        copy_export_detail(rep.detail, sizeof(rep.detail), rfc_status_name(st));
        if (out_report) {
            *out_report = rep;
        }
        return st;
    }

    if (g_before_begin_publish_cb && job) {
        g_before_begin_publish_cb(job_id, g_publish_barrier_user);
    }

    if (job) {
        st = rfc_job_internal_begin_publish(job, job_id);
        if (st != RFC_OK) {
            publisher->abort_stage(publisher->ctx, stage_handle);
            if (tracked_partial > 0) {
                rfc_job_internal_note_partial_bytes(job, -tracked_partial);
            }
            rep.status = st;
            rep.published = 0;
            rep.temp_cleaned_up = 1;
            rep.valid = 1;
            copy_export_detail(rep.detail, sizeof(rep.detail),
                               rfc_status_name(st));
            if (out_report) {
                *out_report = rep;
            }
            return st;
        }
    }

    if (g_after_begin_publish_cb && job) {
        g_after_begin_publish_cb(job_id, g_publish_barrier_user);
    }

    if (g_test_fault_stage == RFC_IO_FAULT_COMMIT_EXISTS) {
        st = RFC_ERR_DESTINATION_EXISTS;
    } else if (g_test_fault_stage == RFC_IO_FAULT_COMMIT_IO) {
        st = g_test_fault_status != RFC_OK ? g_test_fault_status : RFC_ERR_IO;
    } else {
        st = publisher->commit_stage(publisher->ctx, stage_handle);
    }

    if (st != RFC_OK) {
        publisher->abort_stage(publisher->ctx, stage_handle);
        if (job) {
            rfc_job_internal_finish_publish(job, job_id, 0);
            if (tracked_partial > 0) {
                rfc_job_internal_note_partial_bytes(job, -tracked_partial);
            }
        }
        rep.status = st;
        rep.published = 0;
        rep.temp_cleaned_up = 1;
        rep.valid = 1;
        copy_export_detail(rep.detail, sizeof(rep.detail), rfc_status_name(st));
        if (out_report) {
            *out_report = rep;
        }
        return st;
    }

    if (job) {
        rfc_job_internal_finish_publish(job, job_id, 1);
        if (tracked_partial > 0) {
            rfc_job_internal_note_partial_bytes(job, -tracked_partial);
        }
    }
    rep.status = RFC_OK;
    rep.published = 1;
    rep.temp_cleaned_up = 1;
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
#ifdef _WIN32
    HANDLE handle;
    wchar_t wdest[4096];
    wchar_t wtmp[4096];
#else
    int fd;
    char tmp_path[4096];
#endif
    int closed;
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

    rfc_status st = rfc_output_check_destination_path(
        ctx->dest_utf8, ctx->source_utf8, source_id, ctx->export_flags);
    if (st != RFC_OK) {
        return st;
    }

    path_stage_handle *sh = (path_stage_handle *)calloc(1u, sizeof(*sh));
    if (!sh) {
        return RFC_ERR_OUT_OF_MEMORY;
    }
    uint64_t seq = atomic_fetch_add_explicit(&g_partial_seq, 1u,
                                             memory_order_relaxed);
#ifdef _WIN32
    sh->handle = INVALID_HANDLE_VALUE;
    st = utf8_to_wide_out(ctx->dest_utf8, sh->wdest, 4096u);
    if (st != RFC_OK) {
        free(sh);
        return st;
    }
    int n = _snwprintf(sh->wtmp, 4096u, L"%ls.rfc-partial.%lu.%llu.%llu.%llu",
                       sh->wdest, (unsigned long)GetCurrentProcessId(),
                       (unsigned long long)job_id,
                       (unsigned long long)attempt_number,
                       (unsigned long long)seq);
    if (n <= 0 || n >= 4096) {
        free(sh);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    sh->handle = CreateFileW(sh->wtmp, GENERIC_WRITE, 0u, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (sh->handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free(sh);
        return map_win32_write_error(err);
    }
#else
    sh->fd = -1;
    int n = snprintf(sh->tmp_path, sizeof(sh->tmp_path),
                     "%s.rfc-partial.%lu.%llu.%llu.%llu", ctx->dest_utf8,
                     (unsigned long)getpid(), (unsigned long long)job_id,
                     (unsigned long long)attempt_number,
                     (unsigned long long)seq);
    if (n <= 0 || (size_t)n >= sizeof(sh->tmp_path)) {
        free(sh);
        return RFC_ERR_INVALID_ARGUMENT;
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL
#ifdef O_NOFOLLOW
                | O_NOFOLLOW
#endif
        ;
    do {
        sh->fd = open(sh->tmp_path, flags, 0644);
    } while (sh->fd < 0 && errno == EINTR);
    if (sh->fd < 0) {
        int err = errno;
        free(sh);
        return map_posix_write_error(err);
    }
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
    CloseHandle(sh->handle);
    sh->handle = INVALID_HANDLE_VALUE;
    sh->closed = 1;
#else
    if (fsync(sh->fd) != 0 && errno != EINVAL) {
        return map_posix_write_error(errno);
    }
    close(sh->fd);
    sh->fd = -1;
    sh->closed = 1;
#endif
    return RFC_OK;
}

static rfc_status path_pub_commit(void *raw_ctx, void *raw_handle) {
    path_pub_ctx *ctx = (path_pub_ctx *)raw_ctx;
    path_stage_handle *sh = (path_stage_handle *)raw_handle;

    /* Re-verify destination was not replaced by symlink/hardlink/existing file after staging. */
    rfc_status st = rfc_output_check_destination_path(
        ctx->dest_utf8, ctx->source_utf8, ctx->source_id, ctx->export_flags);
    if (st != RFC_OK) {
        return st;
    }

#ifdef _WIN32
    if (!MoveFileExW(sh->wtmp, sh->wdest, 0u)) {
        return map_win32_write_error(GetLastError());
    }
#else
    int rc = renameat2(AT_FDCWD, sh->tmp_path, AT_FDCWD, ctx->dest_utf8,
                       RENAME_NOREPLACE);
    if (rc != 0) {
        if (errno == ENOSYS || errno == EINVAL || errno == ENOTSUP) {
            if (link(sh->tmp_path, ctx->dest_utf8) != 0) {
                return map_posix_write_error(errno);
            }
            unlink(sh->tmp_path);
        } else {
            return map_posix_write_error(errno);
        }
    }
    /* Sync parent directory best-effort after commit. */
    char parent[4096];
    size_t dlen = strlen(ctx->dest_utf8);
    if (dlen < sizeof(parent)) {
        memcpy(parent, ctx->dest_utf8, dlen + 1u);
        int found_sep = 0;
        for (size_t i = dlen; i > 0u; i--) {
            size_t idx = i - 1u;
            if (parent[idx] == '/') {
                found_sep = 1;
                if (idx == 0u) {
                    parent[1] = '\0';
                } else {
                    parent[idx] = '\0';
                }
                break;
            }
        }
        int dfd = open(found_sep ? parent : ".", O_RDONLY);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
    }
#endif
    free(sh);
    return RFC_OK;
}

static void path_pub_abort(void *raw_ctx, void *raw_handle) {
    (void)raw_ctx;
    path_stage_handle *sh = (path_stage_handle *)raw_handle;
    if (!sh) {
        return;
    }
#ifdef _WIN32
    if (!sh->closed && sh->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(sh->handle);
        sh->handle = INVALID_HANDLE_VALUE;
    }
    DeleteFileW(sh->wtmp);
#else
    if (!sh->closed && sh->fd >= 0) {
        close(sh->fd);
        sh->fd = -1;
    }
    unlink(sh->tmp_path);
#endif
    free(sh);
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
