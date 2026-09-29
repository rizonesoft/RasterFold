/* zlib / PNG CRC-32 (reflected polynomial 0xEDB88320), slicing by 8.
   CRC-32("123456789") = 0xCBF43926. */
#include "rf.h"

static uint32_t T[8][256];
static int ready;

static void crc_tables(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        T[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int s = 1; s < 8; s++) T[s][i] = (T[s - 1][i] >> 8) ^ T[0][T[s - 1][i] & 0xFF];
    ready = 1;
}

uint32_t rf_crc32(const uint8_t *p, size_t n) {
    if (!ready) crc_tables();
    uint32_t c = 0xFFFFFFFFu;
    while (n && ((uintptr_t)p & 7)) { c = T[0][(c ^ *p++) & 0xFF] ^ (c >> 8); n--; }
    while (n >= 8) {
        uint32_t lo = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        lo ^= c;
        c = T[7][lo & 0xFF] ^ T[6][(lo >> 8) & 0xFF] ^ T[5][(lo >> 16) & 0xFF] ^ T[4][lo >> 24] ^
            T[3][hi & 0xFF] ^ T[2][(hi >> 8) & 0xFF] ^ T[1][(hi >> 16) & 0xFF] ^ T[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) c = T[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
