// bench.c - time .fold encode/decode (src/fold.c, src/cm.c, src/pal.c) in thread cycles (best of N); verifies round trip.
// Optional -p: sample the thread's RIP ~1 kHz into prof_rips.txt (see prof.py).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "rf.h"

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
    int reps = 3, prof = 0, threads = 1;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (int a = 1; a < argc; a++) { if (!strcmp(argv[a], "-p")) prof = 1; if (!strncmp(argv[a], "-t", 2) && atoi(argv[a] + 2) > 1) g_wall = 1; }
    if (prof) {
        g_rip = malloc(sizeof(DWORD64) * MAXRIP);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
        HANDLE t = CreateThread(0, 0, sampler, 0, 0, 0);
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    }
    double te_all = 0, td_all = 0, px_all = 0; size_t tot = 0;
    for (int a = 1; a < argc; a++) {
        if (!strncmp(argv[a], "-r", 2)) { reps = atoi(argv[a] + 2); if (reps < 1) reps = 1; continue; }
        if (!strcmp(argv[a], "-p")) continue;
        if (!strncmp(argv[a], "-t", 2)) { threads = atoi(argv[a] + 2); continue; }
        Image im; if (!load_ppm(argv[a], &im)) { fprintf(stderr, "bad %s\n", argv[a]); return 1; }
        size_t raw = (size_t)im.w * im.h * 3;
        rf_image rfi = { (uint32_t)im.w, (uint32_t)im.h, im.rgb };
        size_t fold_sz = 0; double be = 1e30, bd = 1e30; int ok = 1;
        uint32_t method = RF_METHOD_CM;
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
            rf_free(fold);
            if (method == RF_METHOD_RAW) { be = bd = t1 - t0; break; }   /* container stores raw */
            if (t1 - t0 < be) be = t1 - t0;
            if (t3 - t2 < bd) bd = t3 - t2;
        }
        const char *b = strrchr(argv[a], '/'); b = b ? b + 1 : argv[a];
        const char *status = !ok ? "MISMATCH" : (method == RF_METHOD_RAW ? "RAW" : (method == RF_METHOD_PAL ? "PAL" : "OK"));
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
    return 0;
}
