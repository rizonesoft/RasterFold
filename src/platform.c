#include "rf.h"

static uint64_t alloc_current;
uint64_t rf_internal_alloc_peak;

int rf_fail(rf_error *err, int code, const char *msg) {
    if (err && !err->code) { err->code = code; err->message = msg; }
    return code;
}

/* Accounting is process-wide. Codec errors and working buffers are per-call. */
void rf_account_add(size_t size) {
    uint64_t now = __atomic_add_fetch(&alloc_current, size, __ATOMIC_RELAXED);
    uint64_t peak = __atomic_load_n(&rf_internal_alloc_peak, __ATOMIC_RELAXED);
    while (peak < now && !__atomic_compare_exchange_n(&rf_internal_alloc_peak, &peak, now, 0,
                                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}
void rf_account_remove(size_t size) { __atomic_sub_fetch(&alloc_current, size, __ATOMIC_RELAXED); }

void rf_print_u64(int to_stderr, uint64_t v) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    rf_print(to_stderr, buf + i);
}

void rf_print_hex32(int to_stderr, uint32_t v) {
    char buf[9];
    for (int i = 7; i >= 0; i--, v >>= 4) buf[i] = "0123456789abcdef"[v & 15];
    buf[8] = 0;
    rf_print(to_stderr, buf);
}
