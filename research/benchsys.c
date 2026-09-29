// Minimal platform shim for the benchmark (uses the C runtime and Win32).
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "rf.h"
uint64_t g_alloc_peak;
int g_err_code;
const char *g_err_msg;
int rf_fail(int code, const char *msg) { if (!g_err_code) { g_err_code = code; g_err_msg = msg; } return code; }
void *rf_alloc(size_t size) {
    uint8_t *raw = calloc(1, size + 128);
    if (!raw) return 0;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + 64 + 63) & ~(uintptr_t)63);
    ((void **)p)[-1] = raw;
    return p;
}
void rf_free(void *p) { if (p) free(((void **)p)[-1]); }
typedef struct { void (*fn)(void *); void *arg; } ts_t;
static DWORD WINAPI entry(void *p) { ts_t s = *(ts_t *)p; free(p); s.fn(s.arg); return 0; }
void *rf_thread_create(void (*fn)(void *), void *arg) {
    ts_t *s = malloc(sizeof *s); s->fn = fn; s->arg = arg;
    HANDLE h = CreateThread(0, 0, entry, s, CREATE_SUSPENDED, 0);
    if (!h) free(s);
    return h;
}
void rf_thread_resume(void *t) { ResumeThread((HANDLE)t); }
void rf_thread_join(void *t) { WaitForSingleObject((HANDLE)t, INFINITE); CloseHandle((HANDLE)t); }
void rf_yield(void) { SwitchToThread(); }
int rf_cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
