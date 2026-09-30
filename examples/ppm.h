#ifndef RF_EXAMPLE_PPM_H
#define RF_EXAMPLE_PPM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* Small P6 example reader: comments, spaces and CRLF headers are accepted. */
static int token(FILE *f, char *out, size_t capacity) {
    int c;
    do {
        c = fgetc(f);
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); }
    } while (c != EOF && isspace((unsigned char)c));
    size_t n = 0;
    while (c != EOF && !isspace((unsigned char)c)) {
        if (n + 1 >= capacity) return 0;
        out[n++] = (char)c; c = fgetc(f);
    }
    if (c == '\r') { c = fgetc(f); if (c != '\n' && c != EOF) ungetc(c, f); }
    out[n] = 0;
    return n != 0;
}

static unsigned dimension(const char *s) {
    char *end;
    unsigned long value = strtoul(s, &end, 10);
    return *s && !*end && value > 0 && value <= 65535 ? (unsigned)value : 0;
}
#endif
