/* The compiler emits calls to these for struct copies, large moves, and loops
   it recognizes as library idioms. rep movsb / rep stosb are fast on every
   x86-64 core with ERMSB. no_builtin keeps the loops below from being turned
   back into calls to themselves. */
#include "rf.h"

void *memcpy(void *dst, const void *src, size_t n) {
    void *d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    void *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    /* Overlapping, destination above source: copy backwards. */
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

__attribute__((no_builtin)) int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

__attribute__((no_builtin)) size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
