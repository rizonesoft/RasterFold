/* Command line: encode, decode, inspect. Entry point rf_start; no C runtime. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif
#include "rf.h"
#include "product_version.h"
#ifdef _WIN32
#define RF_EOL "\r\n"
#else
#define RF_EOL "\n"
#endif

static const char usage_text[] =
    "RasterFold lossless image codec" RF_EOL
    "usage:" RF_EOL
    "  rasterfold encode [--time] [--threads N] input.bmp|input.png|input.ppm output.fold" RF_EOL
    "  rasterfold decode [--time] [--threads N] input.fold output.bmp|output.png|output.ppm" RF_EOL
    "  rasterfold inspect [--time] [--json] input.fold" RF_EOL
    "  rasterfold verify [--threads N] input.fold" RF_EOL
    "  rasterfold help [command]" RF_EOL
    "  rasterfold version" RF_EOL
    "commands:" RF_EOL
    "  encode   encode a BMP, PNG, or PPM image into a .fold file" RF_EOL
    "  decode   decode a .fold file into a BMP, PNG, or PPM image" RF_EOL
    "  inspect  print what a .fold file holds, as text or JSON" RF_EOL
    "  verify   decode a .fold file in memory and print its pixel hash; writes nothing" RF_EOL
    "  help     print this summary, or one command's usage with `help <command>`" RF_EOL
    "  version  print the format and API versions" RF_EOL
    "--threads 1 codes on one thread; the default uses up to 9 when the CPU has them." RF_EOL
    "exit codes: 0 ok, 1 usage, 2 io, 3 unsupported, 4 malformed, 5 limit" RF_EOL;

static const char help_encode[] =
    "usage: rasterfold encode [--time] [--threads N] input.bmp|input.png|input.ppm output.fold" RF_EOL
    "Encode a 24-bit BMP, 8-bit PNG, or P6 PPM (maxval 255) image into a .fold file. The output must not already exist." RF_EOL
    "options: --time prints timing statistics; --threads N codes on N threads (1 to 9)." RF_EOL
    "exit codes: 0 ok, 1 usage, 2 io, 3 unsupported, 4 malformed, 5 limit" RF_EOL
    "example: rasterfold encode photo.bmp photo.fold" RF_EOL;

static const char help_decode[] =
    "usage: rasterfold decode [--time] [--threads N] input.fold output.bmp|output.png|output.ppm" RF_EOL
    "Decode a .fold file into a 24-bit BMP, 8-bit PNG, or P6 PPM (maxval 255) image. The output must not already exist." RF_EOL
    "options: --time prints timing statistics; --threads N codes on N threads (1 to 9)." RF_EOL
    "exit codes: 0 ok, 1 usage, 2 io, 3 unsupported, 4 malformed, 5 limit" RF_EOL
    "example: rasterfold decode photo.fold photo.bmp" RF_EOL;

static const char help_inspect[] =
    "usage: rasterfold inspect [--time] [--json] input.fold" RF_EOL
    "Print what a .fold file holds, fully validating its payload. --json prints one JSON object and refuses --time." RF_EOL
    "options: --time prints timing statistics (text form only); --json selects the JSON form." RF_EOL
    "exit codes: 0 ok, 1 usage, 2 io, 3 unsupported, 4 malformed, 5 limit" RF_EOL
    "example: rasterfold inspect --json photo.fold" RF_EOL;

static const char help_verify[] =
    "usage: rasterfold verify [--threads N] input.fold" RF_EOL
    "Decode a .fold file in memory, checking every checksum and stream end, and print the CRC32 of the decoded pixels. Writes nothing." RF_EOL
    "options: --threads N decodes on N threads (1 to 9)." RF_EOL
    "exit codes: 0 ok, 1 usage, 2 io, 3 unsupported, 4 malformed, 5 limit" RF_EOL
    "example: rasterfold verify photo.fold" RF_EOL;

static const char help_help[] =
    "usage: rasterfold help [command]" RF_EOL
    "Print the command summary, or one command's usage, options, exit codes, and example." RF_EOL
    "exit codes: 0 ok, 1 usage" RF_EOL
    "example: rasterfold help encode" RF_EOL;

static const char help_version[] =
    "usage: rasterfold version" RF_EOL
    "Print the .fold format versions this build reads and writes and the C API version. --version is the same command." RF_EOL
    "exit codes: 0 ok, 1 usage" RF_EOL
    "example: rasterfold version" RF_EOL;

static void print_fold_version(uint32_t fold_version) {
    rf_print(0, fold_version == 3 ? "RasterFold fold version 3" RF_EOL : "RasterFold fold version 2" RF_EOL);
}

static void print_version(void) {
    rf_print(0, "RasterFold codec" RF_EOL);
    rf_print(0, "product version: " RF_PRODUCT_VERSION RF_EOL);
    rf_print(0, "format versions read: 2, 3" RF_EOL);
    rf_print(0, "format version written: 3" RF_EOL);
    rf_print(0, "api version: 1.0 (0x00010000)" RF_EOL);
}

enum { EXT_NONE, EXT_UNKNOWN, EXT_BMP, EXT_PNG, EXT_PPM, EXT_FOLD };

static int ext_of(const rf_path_char *path) {
    if (!path) return EXT_NONE;
    const rf_path_char *base = path;
    for (const rf_path_char *p = path; *p; p++) {
        if (*p == '/'
#ifdef _WIN32
            || *p == '\\'
#endif
        ) base = p + 1;
    }
    const rf_path_char *dot = 0;
    for (const rf_path_char *p = base; *p; p++)
        if (*p == '.') dot = p;
    if (!dot || dot == base || !dot[1]) return EXT_NONE;
    char ext[8];
    size_t n = 0;
    for (const rf_path_char *p = dot + 1; *p; p++) {
        if (n + 1 >= sizeof ext) return EXT_UNKNOWN;
        rf_path_char c = *p;
        uint32_t v = (uint32_t)c;
        if (v > 127u) return EXT_UNKNOWN;
        if (v >= (uint32_t)'A' && v <= (uint32_t)'Z') c = (rf_path_char)(v - (uint32_t)'A' + (uint32_t)'a');
        ext[n++] = (char)c;
    }
    ext[n] = 0;
    {
        static const char *const names[] = {"bmp", "png", "ppm", "fold"};
        static const int kinds[] = {EXT_BMP, EXT_PNG, EXT_PPM, EXT_FOLD};
        for (int k = 0; k < 4; k++) {
            size_t i = 0;
            while (names[k][i] && ext[i] == names[k][i]) i++;
            if (!names[k][i] && !ext[i]) return kinds[k];
        }
    }
    return EXT_UNKNOWN;
}

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
    int npos = 0, timing = 0, json = 0, want_help = 0, want_version = 0;
    int ncpu = rf_cpu_count();
    int threads = ncpu < 1 ? 1 : (ncpu > 9 ? 9 : ncpu);
    for (int i = 1; i < argc; i++) {
        if (wide_eq(argv[i], "--time")) { timing = 1; continue; }
        if (wide_eq(argv[i], "--json")) { json = 1; continue; }
        if (wide_eq(argv[i], "--help")) { want_help = 1; continue; }
        if (wide_eq(argv[i], "--version")) { want_version = 1; continue; }
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
    if (want_help || want_version) {
        if (npos != 0 || timing || json) return -1;
        if (want_help && want_version) return -1;
        if (want_help) rf_print(0, usage_text);
        else print_version();
        return RF_OK;
    }
    if (npos == 0) return -1;
    if (wide_eq(pos[0], "help")) {
        if (json || timing || npos > 2) return -1;
        if (npos == 1) { rf_print(0, usage_text); return RF_OK; }
        if (wide_eq(pos[1], "encode")) rf_print(0, help_encode);
        else if (wide_eq(pos[1], "decode")) rf_print(0, help_decode);
        else if (wide_eq(pos[1], "inspect")) rf_print(0, help_inspect);
        else if (wide_eq(pos[1], "verify")) rf_print(0, help_verify);
        else if (wide_eq(pos[1], "help")) rf_print(0, help_help);
        else if (wide_eq(pos[1], "version")) rf_print(0, help_version);
        else return -1;
        return RF_OK;
    }
    if (wide_eq(pos[0], "version")) {
        if (json || timing || npos != 1) return -1;
        print_version();
        return RF_OK;
    }
    int encode = wide_eq(pos[0], "encode"), decode = wide_eq(pos[0], "decode"), inspect = wide_eq(pos[0], "inspect"), verify = wide_eq(pos[0], "verify");
    if (!(encode || decode || inspect || verify) || npos != ((inspect || verify) ? 2 : 3)) return -1;
    if (json && !inspect) return -1;
    if (json && timing) return -1;
    if (verify && timing) return -1;

    if (rf_crc32((const uint8_t *)"123456789", 9) != 0xCBF43926u)
        return rf_fail(err, RF_ERR_LIMIT, "internal self-test failed");
    int in_ext = EXT_NONE, out_ext = EXT_NONE;
    {
        in_ext = ext_of(pos[1]);
        int want_in = encode ? EXT_BMP : EXT_FOLD;
        if (in_ext == EXT_UNKNOWN) return -1;
        if (encode && in_ext == EXT_PNG) want_in = EXT_PNG;
        if (encode && in_ext == EXT_PPM) want_in = EXT_PPM;
        if (in_ext != EXT_NONE && in_ext != want_in)
            return rf_fail(err, RF_ERR_UNSUPPORTED, encode ? "unsupported input format: encode reads .bmp, .png, or .ppm" : "unsupported input format: this command reads .fold");
        if ((encode || decode) && rf_path_blocked(pos[2], err)) return err->code;
        if (encode || decode) {
            out_ext = ext_of(pos[2]);
            int want_out = encode ? EXT_FOLD : EXT_BMP;
            if (decode && out_ext == EXT_PNG) want_out = EXT_PNG;
            if (decode && out_ext == EXT_PPM) want_out = EXT_PPM;
            if (out_ext == EXT_UNKNOWN) return -1;
            if (out_ext != EXT_NONE && out_ext != want_out)
                return rf_fail(err, RF_ERR_UNSUPPORTED, encode ? "unsupported output format: encode writes .fold" : "unsupported output format: decode writes .bmp, .png, or .ppm");
        }
    }

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
        rc = in_ext == EXT_PNG ? rf_png_load(file, file_size, &img, err)
             : in_ext == EXT_PPM ? rf_ppm_load(file, file_size, &img, err)
                               : rf_bmp_load(file, file_size, &img, err);
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
        if (verify) {
            rf_pixels pixels = {0, 0, 0, 0, 0, 0};
            rc = rf_decode_image(file, file_size, &options, &pixels, err);
            rf_free(file);
            file = 0;
            t3 = rf_ticks();
            if (rc) goto done;
            print_fold_version(fold_version);
            info_line("width", pixels.width);
            info_line("height", pixels.height);
            info_hex("pixel_crc32", rf_crc32((const uint8_t *)pixels.data, pixels.size));
            rf_print(0, "status: ok" RF_EOL);
            rf_free(pixels.data);
            goto timing_out;
        }
        if (inspect) rc = rf_inspect(file, file_size, &info, err);
        else if (decode && (out_ext == EXT_PNG || out_ext == EXT_PPM)) {
            rf_image decoded = {0, 0, 0};
            rc = rf_decode(file, file_size, &decoded, &info, threads, err);
            if (!rc) {
                rc = out_ext == EXT_PNG ? rf_png_store(&decoded, &out, &out_size, err)
                                        : rf_ppm_store(&decoded, &out, &out_size, err);
                rf_free(decoded.rgb);
            }
        }
        else rc = rf_decode_bmp(file, file_size, &out, &out_size, threads, err);
        rf_free(file);
        file = 0;
        t3 = rf_ticks();
        if (rc) goto done;
        if (decode) {
            rc = rf_write_atomic(pos[2], out, out_size, err);
            written = out_size;
        } else if (json) {
            const char *method = info.method == RF_METHOD_CM ? "cm" :
                (info.method == RF_METHOD_PAL || info.method == RF_METHOD_PAL2) ? "palette" : "raw";
            rf_print(0, "{\"version\": ");
            rf_print_u64(0, fold_version);
            rf_print(0, ", \"width\": ");
            rf_print_u64(0, info.width);
            rf_print(0, ", \"height\": ");
            rf_print_u64(0, info.height);
            rf_print(0, ", \"method\": \"");
            rf_print(0, method);
            rf_print(0, "\", \"uncompressed_rgb_bytes\": ");
            rf_print_u64(0, (uint64_t)info.width * info.height * 3);
            rf_print(0, ", \"payload_size\": ");
            rf_print_u64(0, info.payload_size);
            rf_print(0, ", \"file_size\": ");
            rf_print_u64(0, file_size);
            rf_print(0, ", \"payload_crc32\": \"0x");
            rf_print_hex32(0, info.payload_crc);
            rf_print(0, "\", \"header_crc32\": \"0x");
            rf_print_hex32(0, info.header_crc);
            rf_print(0, "\"");
            if (info.method == RF_METHOD_CM) {
                rf_print(0, ", \"stream_g\": ");
                rf_print_u64(0, info.stream_size[0]);
                rf_print(0, ", \"stream_r\": ");
                rf_print_u64(0, info.stream_size[1]);
                rf_print(0, ", \"stream_b\": ");
                rf_print_u64(0, info.stream_size[2]);
            } else if (info.method == RF_METHOD_PAL || info.method == RF_METHOD_PAL2) {
                rf_print(0, ", \"palette_colours\": ");
                rf_print_u64(0, pal_colours);
            }
            rf_print(0, ", \"status\": \"ok\"}" RF_EOL);
        } else {
            print_fold_version(fold_version);
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
timing_out:
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
