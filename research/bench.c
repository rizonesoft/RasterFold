// bench.c - time .fold encode/decode (src/fold.c, src/cm.c, src/pal.c) in thread cycles (best of N); verifies round trip.
// Optional -p: sample the thread's RIP ~1 kHz into prof_rips.txt (see prof.py).
// Optional --breakdown: attribute CM bytes per channel (G, R-G, B-G) and decision kind (zero, sign, exp, mant).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "rf.h"

#if !defined(RF_NO_BREAKDOWN)
#include "rc.h"

#ifndef RF_BENCH_CM_PATH
#define RF_BENCH_CM_PATH "../src/cm.c"
#endif

typedef struct {
    const renc *rc;
    int c;
} bd_rc_map_t;

static bd_rc_map_t g_bd_map[16];
static int g_bd_nmap;
static double g_bd_bits[3][4];
static const int node = 16;

static inline void bd_renc_init_hook(renc *rc, uint8_t *buf, size_t cap, int c) {
    renc_init(rc, buf, cap);
    if (g_bd_nmap < 16) {
        g_bd_map[g_bd_nmap].rc = rc;
        g_bd_map[g_bd_nmap].c = (c >= 0 && c < 3) ? c : 0;
        g_bd_nmap++;
    }
}

static inline void bd_renc_bit_hook(renc *rc, int bit, uint32_t p1, int nd) {
    uint32_t bound = (rc->range >> 12) * p1;
    uint32_t nr = bit ? bound : (rc->range - bound);
    double bits = -log2((double)nr / (double)rc->range);
    int c = 0;
    for (int i = 0; i < g_bd_nmap; i++) {
        if (g_bd_map[i].rc == rc) {
            c = g_bd_map[i].c;
            break;
        }
    }
    int k = (nd == 1) ? 0 : (nd == 2) ? 1 : (nd <= 8) ? 2 : 3;
    g_bd_bits[c][k] += bits;
    renc_bit(rc, bit, p1);
}

#define RF_BENCH_BREAKDOWN_PASS 1
#define renc_init(rc_ptr, buf, cap) bd_renc_init_hook((rc_ptr), (buf), (cap), (c))
#define renc_bit(rc_ptr, bit_v, p1_v) bd_renc_bit_hook((rc_ptr), (bit_v), (p1_v), (node))
#define rf_cm_encode bd_cm_encode_inner
#define rf_cm_encode_orig bd_cm_encode_orig_inner
#define rf_cm_decode bd_cm_decode_unused
#include RF_BENCH_CM_PATH
#undef RF_BENCH_BREAKDOWN_PASS
#undef renc_init
#undef renc_bit
#undef rf_cm_encode
#undef rf_cm_encode_orig
#undef rf_cm_decode

static int run_breakdown(const char *b, int w, int h, const uint8_t *rgb, const uint8_t *fold, size_t fold_sz, uint32_t method) {
    if (method != RF_METHOD_CM) {
        const char *mname = (method == RF_METHOD_RAW) ? "RAW" : "PAL";
        printf("  breakdown %-20s non-CM method=%u (%s) fold=%zu\n", b, (unsigned)method, mname, fold_sz);
        return 1;
    }
    if (fold_sz < RF_HEADER_SIZE) {
        fprintf(stderr, "breakdown %s: short fold header (%zu)\n", b, fold_sz);
        return 0;
    }
    uint8_t version = fold[3];
    const uint8_t *payload = fold + RF_HEADER_SIZE;
    size_t payload_sz = fold_sz - RF_HEADER_SIZE;
    size_t raw = (size_t)w * (size_t)h * 3u;
    if (raw == 0) {
        fprintf(stderr, "breakdown %s: empty image (%dx%d)\n", b, w, h);
        return 0;
    }
    size_t cap = raw + 256u;
    uint8_t *bd_buf = (uint8_t *)malloc(cap);
    if (!bd_buf) {
        fprintf(stderr, "breakdown %s: out of memory (%zu)\n", b, cap);
        return 0;
    }
    memset(g_bd_bits, 0, sizeof(g_bd_bits));
    g_bd_nmap = 0;
    size_t bd_written = 0;
    rf_error err = {0};
    int rc = bd_cm_encode_inner(rgb, (uint32_t)w, (uint32_t)h, bd_buf, raw - 1u, &bd_written, 1, &err);
    if (rc != RF_OK || bd_written != payload_sz || memcmp(bd_buf, payload, payload_sz) != 0) {
        fprintf(stderr, "breakdown %s: instrumented payload mismatch (rc=%d written=%zu expected=%zu)\n",
                b, rc, bd_written, payload_sz);
        free(bd_buf);
        return 0;
    }
    free(bd_buf);
    int S = (version == 3) ? (int)payload[0] : 1;
    size_t dir_off = (version == 3) ? 1u : 0u;
    size_t payload_hdr = dir_off + (size_t)S * 12u;
    if (S < 1 || S > 4 || payload_sz < payload_hdr) {
        fprintf(stderr, "breakdown %s: invalid strip header (S=%d payload=%zu)\n", b, S, payload_sz);
        return 0;
    }
    size_t ch_stream[3] = {0, 0, 0};
    for (int s = 0; s < S; s++) {
        for (int c = 0; c < 3; c++) {
            const uint8_t *q = payload + dir_off + (size_t)(s * 3 + c) * 4u;
            uint32_t sz_sc = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
            ch_stream[c] += sz_sc;
        }
    }
    size_t stream_all = ch_stream[0] + ch_stream[1] + ch_stream[2];
    if (payload_hdr + stream_all != payload_sz) {
        fprintf(stderr, "breakdown %s: stream sum mismatch (hdr=%zu streams=%zu payload=%zu)\n",
                b, payload_hdr, stream_all, payload_sz);
        return 0;
    }
    static const char *const ch_names[3] = {"G", "R-G", "B-G"};
    double kind_all[4] = {0.0, 0.0, 0.0, 0.0};
    double dec_all = 0.0;
    for (int c = 0; c < 3; c++) {
        double z = g_bd_bits[c][0] / 8.0;
        double sg = g_bd_bits[c][1] / 8.0;
        double ex = g_bd_bits[c][2] / 8.0;
        double mn = g_bd_bits[c][3] / 8.0;
        double dt = z + sg + ex + mn;
        double fl = (double)ch_stream[c] - dt;
        double denom = dt > 0.0 ? dt : 1.0;
        double s_pct = stream_all > 0 ? (100.0 * (double)ch_stream[c] / (double)stream_all) : 0.0;
        printf("  breakdown %-20s %-4s stream=%8zu (%5.1f%%)  dec=%10.1f  zero=%9.1f (%4.1f%%)  sign=%9.1f (%4.1f%%)  exp=%9.1f (%4.1f%%)  mant=%9.1f (%4.1f%%)  flush=%+6.1f\n",
               b, ch_names[c], ch_stream[c], s_pct, dt,
               z, 100.0 * z / denom, sg, 100.0 * sg / denom,
               ex, 100.0 * ex / denom, mn, 100.0 * mn / denom, fl);
        kind_all[0] += z;
        kind_all[1] += sg;
        kind_all[2] += ex;
        kind_all[3] += mn;
        dec_all += dt;
    }
    double denom_all = dec_all > 0.0 ? dec_all : 1.0;
    double fl_all = (double)stream_all - dec_all;
    size_t hdr_total = RF_HEADER_SIZE + payload_hdr;
    printf("  breakdown %-20s ALL  stream=%8zu (100.0%%)  dec=%10.1f  zero=%9.1f (%4.1f%%)  sign=%9.1f (%4.1f%%)  exp=%9.1f (%4.1f%%)  mant=%9.1f (%4.1f%%)  flush=%+6.1f  hdr=%zu  fold=%zu\n",
           b, stream_all, dec_all,
           kind_all[0], 100.0 * kind_all[0] / denom_all,
           kind_all[1], 100.0 * kind_all[1] / denom_all,
           kind_all[2], 100.0 * kind_all[2] / denom_all,
           kind_all[3], 100.0 * kind_all[3] / denom_all,
           fl_all, hdr_total, fold_sz);
    return 1;
}
#endif

typedef struct { int w, h; uint8_t *rgb; } Image;
static int load_ppm(const char *path, Image *im) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    char magic[3] = {0}; int w, h, mx;
    if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &mx) != 4 || strcmp(magic, "P6") || mx != 255) { fclose(f); return 0; }
    fgetc(f); im->w = w; im->h = h; im->rgb = malloc((size_t)w * h * 3);
    if (fread(im->rgb, 1, (size_t)w * h * 3, f) != (size_t)w * h * 3) { fclose(f); return 0; }
    fclose(f); return 1;
}
static int g_wall;
static double cyc_ms(void) { if (g_wall) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; } ULONG64 c; QueryThreadCycleTime(GetCurrentThread(), &c); return c / 3.9e6; }

static HANDLE g_main; static volatile LONG g_stop; static DWORD64 *g_rip; static volatile LONG g_nrip;
#define MAXRIP 2000000
static DWORD WINAPI sampler(void *arg) {
    timeBeginPeriod(1);
    while (!g_stop) {
        Sleep(1);
        if (SuspendThread(g_main) == (DWORD)-1) continue;
        CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_main, &ctx) && g_nrip < MAXRIP) g_rip[g_nrip++] = ctx.Rip;
        ResumeThread(g_main);
    }
    return 0;
}

int main(int argc, char **argv) {
    int reps = 3, prof = 0, threads = 1, breakdown = 0;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-p")) prof = 1;
        if (!strcmp(argv[a], "--breakdown")) breakdown = 1;
        if (!strncmp(argv[a], "-t", 2) && atoi(argv[a] + 2) > 1) g_wall = 1;
    }
#if defined(RF_NO_BREAKDOWN)
    if (breakdown) {
        fprintf(stderr, "--breakdown is not supported for historical baseline builds\n");
        return 2;
    }
#endif
    if (prof && breakdown) {
        fprintf(stderr, "-p and --breakdown cannot be combined\n");
        return 2;
    }
    if (prof) {
        g_rip = malloc(sizeof(DWORD64) * MAXRIP);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
        HANDLE t = CreateThread(0, 0, sampler, 0, 0, 0);
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    }
    double te_all = 0, td_all = 0, px_all = 0; size_t tot = 0;
    int exit_code = 0;
    for (int a = 1; a < argc; a++) {
        if (!strncmp(argv[a], "-r", 2)) { reps = atoi(argv[a] + 2); if (reps < 1) reps = 1; continue; }
        if (!strcmp(argv[a], "-p")) continue;
        if (!strcmp(argv[a], "--breakdown")) continue;
        if (!strncmp(argv[a], "-t", 2)) { threads = atoi(argv[a] + 2); continue; }
        Image im; if (!load_ppm(argv[a], &im)) { fprintf(stderr, "bad %s\n", argv[a]); return 1; }
        size_t raw = (size_t)im.w * im.h * 3;
        rf_image rfi = { (uint32_t)im.w, (uint32_t)im.h, im.rgb };
        size_t fold_sz = 0; double be = 1e30, bd = 1e30; int ok = 1;
        uint32_t method = RF_METHOD_CM;
        uint8_t *last_fold = 0;
        for (int r = 0; r < reps; r++) {
            uint8_t *fold = 0;
            size_t sz = 0;
            double t0 = cyc_ms();
            if (rf_encode(&rfi, &fold, &sz, threads, 0)) { printf("encode failed %s\n", argv[a]); ok = 0; break; }
            double t1 = cyc_ms();
            fold_sz = sz;
            rf_image dec_im = {0};
            rf_info info = {0};
            double t2 = cyc_ms();
            int rc = rf_decode(fold, sz, &dec_im, &info, threads, 0);
            double t3 = cyc_ms();
            method = info.method;
            ok &= rc == 0 && dec_im.rgb != 0 && dec_im.width == (uint32_t)im.w && dec_im.height == (uint32_t)im.h && memcmp(dec_im.rgb, im.rgb, raw) == 0;
            rf_free(dec_im.rgb);
            if (breakdown && r == reps - 1) last_fold = fold;
            else rf_free(fold);
            if (t1 - t0 < be) be = t1 - t0;
            if (t3 - t2 < bd) bd = t3 - t2;
        }
        const char *b = strrchr(argv[a], '/'); b = b ? b + 1 : argv[a];
#if !defined(RF_NO_BREAKDOWN)
        if (breakdown && ok && last_fold) {
            if (!run_breakdown(b, im.w, im.h, im.rgb, last_fold, fold_sz, method)) ok = 0;
        }
#endif
        if (last_fold) rf_free(last_fold);
        if (!ok) exit_code = 1;
        const char *status = !ok ? "MISMATCH" : (method == RF_METHOD_RAW ? "RAW" : (method >= RF_METHOD_PAL ? "PAL" : "OK"));
        double px = (double)im.w * im.h;
        printf("%-24s %9zu  bpp=%.4f  enc=%7.1fms dec=%7.1fms  (%.0f/%.0f cyc/sample) %s\n", b, fold_sz, fold_sz * 8.0 / px, be, bd,
               be * 3.9e6 / (px * 3), bd * 3.9e6 / (px * 3), status);
        te_all += be; td_all += bd; tot += fold_sz; px_all += px;
        free(im.rgb);
    }
    printf("TOTAL %zu  enc=%.1fms dec=%.1fms  (%.0f/%.0f cyc/sample)\n", tot, te_all, td_all, te_all * 3.9e6 / (px_all * 3), td_all * 3.9e6 / (px_all * 3));
    if (prof) {
        g_stop = 1; Sleep(20);
        DWORD64 base = (DWORD64)GetModuleHandleA(0);
        FILE *f = fopen("prof_rips.txt", "w");
        for (LONG i = 0; i < g_nrip; i++) fprintf(f, "0x%llx\n", (unsigned long long)(g_rip[i] - base + 0x140000000ull));
        fclose(f);
        printf("samples %ld written to prof_rips.txt\n", g_nrip);
    }
    return exit_code;
}
