/* Usage: encode input.ppm output.fold. Output must not already exist. */
#include "rasterfold.h"
#include "ppm.h"

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: encode input.ppm output.fold\n"); return 1; }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror("input"); return 1; }
    char text[64]; unsigned w = 0, h = 0;
    int valid = token(in, text, sizeof text) && !strcmp(text, "P6");
    if (valid && token(in, text, sizeof text)) w = dimension(text);
    if (valid && token(in, text, sizeof text)) h = dimension(text);
    valid = valid && w && h && (uint64_t)w * h <= 100000000 && token(in, text, sizeof text) && !strcmp(text, "255");
    if (!valid) { fprintf(stderr, "expected a bounded P6 RGB8 image\n"); fclose(in); return 1; }
    size_t size = (size_t)w * h * 3;
    unsigned char *rgb = malloc(size);
    if (!rgb) { fclose(in); return 1; }
    valid = fread(rgb, 1, size, in) == size; fclose(in);
    if (!valid) { free(rgb); fprintf(stderr, "truncated PPM\n"); return 1; }
    rf_pixels pixels = {w, h, (size_t)w * 3, RF_LAYOUT_RGB8, rgb, size};
    rf_options options = {sizeof options, 1, {0}};
    rf_buffer encoded = {0}; rf_error error = {0};
    int rc = rf_encode_image(&pixels, &options, &encoded, &error);
    free(rgb);
    if (rc) { fprintf(stderr, "%s\n", error.message); return rc; }
    FILE *out = fopen(argv[2], "wbx");
    if (!out) { perror("output"); rf_free(encoded.data); return 1; }
    valid = fwrite(encoded.data, 1, encoded.size, out) == encoded.size;
    if (fclose(out)) valid = 0;
    rf_free(encoded.data);
    if (!valid) { remove(argv[2]); fprintf(stderr, "write failed\n"); }
    return valid ? 0 : 1;
}
