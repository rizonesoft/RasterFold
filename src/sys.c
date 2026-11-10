/* Win32 platform layer: heap, files, console, timing. No C runtime. */
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include "rf.h"

/* Blocks are 64-byte aligned. The 64 bytes in front of the block hold the
   raw HeapAlloc pointer and the requested size. */
void *rf_alloc(size_t size) {
    if (size == 0 || size > RF_MAX_FILE * 2) return 0;
    uint8_t *raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 128);
    if (!raw) return 0;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + 64 + 63) & ~(uintptr_t)63);
    ((void **)p)[-1] = raw;
    ((size_t *)p)[-2] = size;
    rf_account_add(size);
    return p;
}

void rf_free(void *p) {
    if (!p) return;
    rf_account_remove(((size_t *)p)[-2]);
    HeapFree(GetProcessHeap(), 0, ((void **)p)[-1]);
}

int rf_read_file(const rf_path_char *path, uint8_t **data, size_t *size, rf_error *err) {
    *data = 0;
    *size = 0;
    HANDLE f = CreateFileW((LPCWSTR)path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return rf_fail(err, RF_ERR_IO, "cannot read input");
    LARGE_INTEGER len;
    int rc = RF_OK;
    uint8_t *buf = 0;
    if (!GetFileSizeEx(f, &len)) rc = rf_fail(err, RF_ERR_IO, "cannot read input");
    else if (len.QuadPart <= 0) rc = rf_fail(err, RF_ERR_FORMAT, "invalid input: empty file");
    else if ((uint64_t)len.QuadPart > RF_MAX_FILE) rc = rf_fail(err, RF_ERR_LIMIT, "input exceeds size limit");
    else if (!(buf = rf_alloc((size_t)len.QuadPart + 8))) rc = rf_fail(err, RF_ERR_LIMIT, "out of memory");
    else {
        size_t done = 0, want = (size_t)len.QuadPart;
        while (done < want) {
            DWORD got = 0;
            DWORD chunk = (DWORD)((want - done) > 0x10000000u ? 0x10000000u : (want - done));
            if (!ReadFile(f, buf + done, chunk, &got, 0) || got == 0) { rc = rf_fail(err, RF_ERR_IO, "cannot read input"); break; }
            done += got;
        }
    }
    CloseHandle(f);
    if (rc != RF_OK) { rf_free(buf); return rc; }
    *data = buf;
    *size = (size_t)len.QuadPart;
    return RF_OK;
}

/* 0 when nothing exists at path and the path can be created. */
int rf_path_blocked(const rf_path_char *path, rf_error *err) {
    if (GetFileAttributesW((LPCWSTR)path) != INVALID_FILE_ATTRIBUTES)
        return rf_fail(err, RF_ERR_IO, "output file already exists");
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return RF_OK;
    return rf_fail(err, RF_ERR_IO, "cannot access output path");
}

/* Write to path + ".partial" with CREATE_NEW, flush, then rename onto path.
   An existing path or temporary is never touched; a failure deletes only the
   temporary this call created. */
static int rf_write_impl(const rf_path_char *path, const uint8_t *data, size_t size, rf_error *err, int replace) {
    int rc = replace ? RF_OK : rf_path_blocked(path, err);
    if (rc) return rc;
    static const uint16_t suffix[] = {'.', 'p', 'a', 'r', 't', 'i', 'a', 'l', 0};
    uint16_t tmp[1024 + 9];
    size_t n = 0;
    while (path[n]) {
        if (n >= 1024) return rf_fail(err, RF_ERR_IO, "path too long");
        tmp[n] = path[n];
        n++;
    }
    if (n == 0) return rf_fail(err, RF_ERR_IO, "cannot write output");
    for (size_t i = 0; i < 9; i++) tmp[n + i] = suffix[i];
    if (GetFileAttributesW((LPCWSTR)tmp) != INVALID_FILE_ATTRIBUTES)
        return rf_fail(err, RF_ERR_IO, "temporary output already exists");
    HANDLE f = CreateFileW((LPCWSTR)tmp, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return rf_fail(err, RF_ERR_IO, "cannot write output");
    size_t done = 0;
    int ok = 1;
    while (ok && done < size) {
        DWORD put = 0;
        DWORD chunk = (DWORD)((size - done) > 0x10000000u ? 0x10000000u : (size - done));
        ok = WriteFile(f, data + done, chunk, &put, 0) && put != 0;
        done += put;
    }
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    ok = ok && (replace ? MoveFileExW((LPCWSTR)tmp, (LPCWSTR)path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                        : MoveFileW((LPCWSTR)tmp, (LPCWSTR)path));
    if (!ok) {
        DeleteFileW((LPCWSTR)tmp);
        return rf_fail(err, RF_ERR_IO, "cannot write output");
    }
    return RF_OK;
}

typedef struct { void (*fn)(void *); void *arg; } thread_start_t;

static DWORD WINAPI thread_entry(void *p) {
    thread_start_t s = *(thread_start_t *)p;
    rf_free(p);
    s.fn(s.arg);
    return 0;
}

void *rf_thread_create(void (*fn)(void *), void *arg) {
    thread_start_t *s = rf_alloc(sizeof *s);
    if (!s) return 0;
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread(0, 0, thread_entry, s, CREATE_SUSPENDED, 0);
    if (!h) rf_free(s);
    return h;
}

void rf_thread_resume(void *thread) { ResumeThread((HANDLE)thread); }

void rf_thread_join(void *thread) {
    WaitForSingleObject((HANDLE)thread, INFINITE);
    CloseHandle((HANDLE)thread);
}

void rf_yield(void) { SwitchToThread(); }

int rf_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

uint64_t rf_ticks(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

uint64_t rf_elapsed_us(uint64_t start, uint64_t end) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    uint64_t d = end - start;
    return (d / (uint64_t)f.QuadPart) * 1000000u + (d % (uint64_t)f.QuadPart) * 1000000u / (uint64_t)f.QuadPart;
}

uint64_t rf_peak_working_set(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    pmc.cb = sizeof pmc;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
    return pmc.PeakWorkingSetSize;
}

void rf_print(int to_stderr, const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    HANDLE h = GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD put;
    if (h && h != INVALID_HANDLE_VALUE && n) WriteFile(h, s, (DWORD)n, &put, 0);
}

int rf_write_atomic(const rf_path_char *path, const uint8_t *data, size_t size, rf_error *err) {
    return rf_write_impl(path, data, size, err, 0);
}

int rf_write_atomic_replace(const rf_path_char *path, const uint8_t *data, size_t size, rf_error *err) {
    return rf_write_impl(path, data, size, err, 1);
}
