/* Command line: encode, decode, inspect. Entry point rf_start; no C runtime. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif
#include "rf.h"
#ifdef _WIN32
#define RF_EOL "\r\n"
#else
#define RF_EOL "\n"
#endif

static const char usage_text[] =
    "RasterFold lossless image codec" RF_EOL
    "usage:" RF_EOL
    "  rasterfold encode [--time] [--threads N] input.bmp output.fold" RF_EOL
    "  rasterfold decode [--time] [--threads N] input.fold output.bmp" RF_EOL
    "  rasterfold inspect [--time] input.fold" RF_EOL
    "--threads 1 codes on one thread; the default uses up to 9 when the CPU has them." RF_EOL
    "exit codes: 0 ok, 1 usage, 2 io, 3 unsupported, 4 malformed, 5 limit" RF_EOL;

static int wide_eq(const rf_path_char *a, const char *b) {
    while (*a && *a == (uint8_t)*b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void stat_line(const char *key, uint64_t v) {
    rf_print(0, "rf_stat ");
    rf_print(0, key);
    rf_print(0, "=");
    rf_print_u64(0, v);
    rf_print(0, "" RF_EOL);
}

static void info_line(const char *key, uint64_t v) {
    rf_print(0, key);
    rf_print(0, ": ");
    rf_print_u64(0, v);
    rf_print(0, "" RF_EOL);
}

static void info_hex(const char *key, uint32_t v) {
    rf_print(0, key);
    rf_print(0, ": 0x");
    rf_print_hex32(0, v);
    rf_print(0, "" RF_EOL);
}

static int run(int argc, rf_path_char **argv, rf_error *err) {
    rf_path_char *pos[3] = {0, 0, 0};
    int npos = 0, timing = 0;
    int ncpu = rf_cpu_count();
    int threads = ncpu < 1 ? 1 : (ncpu > 9 ? 9 : ncpu);
    for (int i = 1; i < argc; i++) {
        if (wide_eq(argv[i], "--time")) { timing = 1; continue; }
        if (wide_eq(argv[i], "--threads")) {
            if (i + 1 >= argc) return -1;
            const rf_path_char *v = argv[++i];
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
        return rf_fail(err, RF_ERR_LIMIT, "internal self-test failed");
    if (!inspect && rf_path_blocked(pos[2], err)) return err->code;

    uint64_t t0 = rf_ticks(), t1, t2, t3, t4;
    uint8_t *file = 0, *out = 0;
    size_t file_size = 0, out_size = 0, written = 0;
    rf_image img = {0, 0, 0};
    rf_info info;
    int rc = rf_read_file(pos[1], &file, &file_size, err);
    t1 = t2 = t3 = t4 = rf_ticks();
    if (rc) goto done;
    rf_options options = {sizeof(rf_options), (uint32_t)threads, {0}};
    if (encode) {
        rc = rf_bmp_load(file, file_size, &img, err);
        rf_free(file);
        file = 0;
        t2 = rf_ticks();
        if (rc) goto done;
        rf_pixels pixels = {img.width, img.height, (size_t)img.width * 3, RF_LAYOUT_RGB8, img.rgb, (size_t)img.width * img.height * 3};
        rf_buffer buffer = {0, 0};
        rc = rf_encode_image(&pixels, &options, &buffer, err);
        out = buffer.data; out_size = buffer.size;
        t3 = rf_ticks();
        if (rc) goto done;
        rf_free(img.rgb);
        img.rgb = 0;
        rc = rf_write_atomic(pos[2], out, out_size, err);
        written = out_size;
    } else {
        uint32_t pal_colours = (file_size >= RF_HEADER_SIZE + 2)
            ? (uint32_t)file[RF_HEADER_SIZE] + ((uint32_t)file[RF_HEADER_SIZE + 1] << 8) + 1u
            : 0u;
        uint32_t fold_version = file_size >= 4 ? file[3] : 0;
        if (inspect) rc = rf_inspect(file, file_size, &info, err);
        else rc = rf_decode_bmp(file, file_size, &out, &out_size, threads, err);
        rf_free(file);
        file = 0;
        t3 = rf_ticks();
        if (rc) goto done;
        if (decode) {
            rc = rf_write_atomic(pos[2], out, out_size, err);
            written = out_size;
        } else {
            rf_print(0, fold_version == 3 ? "RasterFold fold version 3" RF_EOL : "RasterFold fold version 2" RF_EOL);
            info_line("width", info.width);
            info_line("height", info.height);
            rf_print(0, info.method == RF_METHOD_CM ? "method: cm" RF_EOL :
                        (info.method == RF_METHOD_PAL || info.method == RF_METHOD_PAL2) ? "method: palette" RF_EOL : "method: raw" RF_EOL);
            info_line("uncompressed_rgb_bytes", (uint64_t)info.width * info.height * 3);
            info_line("payload_size", info.payload_size);
            info_line("file_size", file_size);
            info_hex("payload_crc32", info.payload_crc);
            info_hex("header_crc32", info.header_crc);
            if (info.method == RF_METHOD_CM) {
                info_line("stream_g", info.stream_size[0]);
                info_line("stream_r", info.stream_size[1]);
                info_line("stream_b", info.stream_size[2]);
            } else if (info.method == RF_METHOD_PAL || info.method == RF_METHOD_PAL2) {
                info_line("palette_colours", pal_colours);
            }
            rf_print(0, "status: ok" RF_EOL);
        }
    }
    t4 = rf_ticks();
    if (rc == RF_OK && timing) {
        stat_line("read_us", rf_elapsed_us(t0, t1));
        stat_line("parse_us", rf_elapsed_us(t1, t2));
        stat_line("codec_us", rf_elapsed_us(t2, t3));
        stat_line("write_us", rf_elapsed_us(t3, t4));
        stat_line("total_us", rf_elapsed_us(t0, t4));
        stat_line("alloc_peak", rf_internal_alloc_peak);
        stat_line("peak_working_set", rf_peak_working_set());
        stat_line("output_bytes", written);
    }
done:
    rf_free(file);
    rf_free(out);
    rf_free(img.rgb);
    return rc;
}

#ifdef _WIN32
static int run_win32(void) {
    rf_error error = {0, 0};
    rf_error *err = &error;
    int argc = 0;
    rf_path_char **argv = (rf_path_char **)CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = argv ? run(argc, argv, err) : -1;
    if (rc < 0) {
        rf_print(1, usage_text);
        rc = RF_ERR_USAGE;
    } else if (rc != RF_OK) {
        rf_print(1, "rasterfold: ");
        rf_print(1, err->message ? err->message : "unknown error");
        rf_print(1, "" RF_EOL);
    }
    if (argv) LocalFree(argv);
    return rc;
}

#ifdef RF_PGO_GEN
int main(void) { return run_win32(); }
#else
void rf_start(void) { ExitProcess((UINT)run_win32()); }
#endif

#else
int main(int argc, char **argv) {
    rf_error error = {0, 0};
    int rc = run(argc, argv, &error);
    if (rc < 0) { rf_print(1, usage_text); return RF_ERR_USAGE; }
    if (rc) {
        rf_print(1, "rasterfold: ");
        rf_print(1, error.message ? error.message : "unknown error");
        rf_print(1, "" RF_EOL);
    }
    return rc;
}
#endif
