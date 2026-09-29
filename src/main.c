/* Command line: encode, decode, inspect. Entry point rf_start; no C runtime. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "rf.h"

static const char usage_text[] =
    "RasterFold lossless image codec\r\n"
    "usage:\r\n"
    "  rasterfold encode [--time] [--threads N] input.bmp output.fold\r\n"
    "  rasterfold decode [--time] [--threads N] input.fold output.bmp\r\n"
    "  rasterfold inspect [--time] input.fold\r\n"
    "--threads 1 codes on one thread; the default uses three when the CPU has them.\r\n"
    "exit codes: 0 ok, 1 usage, 2 io, 3 unsupported, 4 malformed, 5 limit\r\n";

static int wide_eq(const uint16_t *a, const char *b) {
    while (*a && *a == (uint8_t)*b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void stat_line(const char *key, uint64_t v) {
    rf_print(0, "rf_stat ");
    rf_print(0, key);
    rf_print(0, "=");
    rf_print_u64(0, v);
    rf_print(0, "\r\n");
}

static void info_line(const char *key, uint64_t v) {
    rf_print(0, key);
    rf_print(0, ": ");
    rf_print_u64(0, v);
    rf_print(0, "\r\n");
}

static void info_hex(const char *key, uint32_t v) {
    rf_print(0, key);
    rf_print(0, ": 0x");
    rf_print_hex32(0, v);
    rf_print(0, "\r\n");
}

static int run(int argc, uint16_t **argv) {
    uint16_t *pos[3];
    int npos = 0, timing = 0;
    int threads = rf_cpu_count() >= 3 ? 3 : 1;
    for (int i = 1; i < argc; i++) {
        if (wide_eq(argv[i], "--time")) { timing = 1; continue; }
        if (wide_eq(argv[i], "--threads")) {
            if (i + 1 >= argc) return -1;
            const uint16_t *v = argv[++i];
            if (v[0] < '1' || v[0] > '9' || v[1] != 0) return -1;
            threads = v[0] - '0';
            continue;
        }
        if (npos == 3) return -1;
        pos[npos++] = argv[i];
    }
    if (npos == 0) return -1;
    int encode = wide_eq(pos[0], "encode"), decode = wide_eq(pos[0], "decode"), inspect = wide_eq(pos[0], "inspect");
    if (!(encode || decode || inspect) || npos != (inspect ? 2 : 3)) return -1;

    if (rf_crc32((const uint8_t *)"123456789", 9) != 0xCBF43926u)
        return rf_fail(RF_ERR_LIMIT, "internal self-test failed");
    if (!inspect && rf_path_blocked(pos[2])) return g_err_code;

    uint64_t t0 = rf_ticks(), t1, t2, t3, t4;
    uint8_t *file = 0, *out = 0;
    size_t file_size = 0, out_size = 0, written = 0;
    rf_image img = {0, 0, 0};
    rf_info info;
    int rc = rf_read_file(pos[1], &file, &file_size);
    t1 = t2 = t3 = t4 = rf_ticks();
    if (rc) goto done;
    if (encode) {
        rc = rf_bmp_load(file, file_size, &img);
        rf_free(file);
        file = 0;
        t2 = rf_ticks();
        if (rc) goto done;
        rc = rf_encode(&img, &out, &out_size, threads);
        t3 = rf_ticks();
        if (rc) goto done;
        rf_free(img.rgb);
        img.rgb = 0;
        rc = rf_write_atomic(pos[2], out, out_size);
        written = out_size;
    } else {
        rc = rf_decode(file, file_size, &img, &info, threads);
        rf_free(file);
        file = 0;
        t3 = rf_ticks();
        if (rc) goto done;
        if (decode) {
            rc = rf_bmp_store(&img, &out, &out_size);
            rf_free(img.rgb);
            img.rgb = 0;
            if (!rc) rc = rf_write_atomic(pos[2], out, out_size);
            written = out_size;
        } else {
            rf_print(0, "RasterFold fold version 2\r\n");
            info_line("width", info.width);
            info_line("height", info.height);
            rf_print(0, info.method == RF_METHOD_CM ? "method: cm\r\n" : "method: raw\r\n");
            info_line("uncompressed_rgb_bytes", (uint64_t)info.width * info.height * 3);
            info_line("payload_size", info.payload_size);
            info_line("file_size", file_size);
            info_hex("payload_crc32", info.payload_crc);
            info_hex("header_crc32", info.header_crc);
            if (info.method == RF_METHOD_CM) {
                info_line("stream_g", info.stream_size[0]);
                info_line("stream_r", info.stream_size[1]);
                info_line("stream_b", info.stream_size[2]);
            }
            rf_print(0, "status: ok\r\n");
        }
    }
    t4 = rf_ticks();
    if (rc == RF_OK && timing) {
        stat_line("read_us", rf_elapsed_us(t0, t1));
        stat_line("parse_us", rf_elapsed_us(t1, t2));
        stat_line("codec_us", rf_elapsed_us(t2, t3));
        stat_line("write_us", rf_elapsed_us(t3, t4));
        stat_line("total_us", rf_elapsed_us(t0, t4));
        stat_line("alloc_peak", g_alloc_peak);
        stat_line("peak_working_set", rf_peak_working_set());
        stat_line("output_bytes", written);
    }
done:
    rf_free(file);
    rf_free(out);
    rf_free(img.rgb);
    return rc;
}

void rf_start(void) {
    int argc = 0;
    uint16_t **argv = (uint16_t **)CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = argv ? run(argc, argv) : -1;
    if (rc < 0) {
        rf_print(1, usage_text);
        rc = RF_ERR_USAGE;
    } else if (rc != RF_OK) {
        rf_print(1, "rasterfold: ");
        rf_print(1, g_err_msg ? g_err_msg : "unknown error");
        rf_print(1, "\r\n");
    }
    if (argv) LocalFree(argv);
    ExitProcess((UINT)rc);
}
