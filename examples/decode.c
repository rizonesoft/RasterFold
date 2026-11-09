/* Usage: decode input.fold output.ppm. Output must not already exist. */
#include "rasterfold.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: decode input.fold output.ppm\n"); return 1; }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror("input"); return 1; }
    if (fseek(in, 0, SEEK_END)) { fclose(in); return 1; }
    long length = ftell(in);
    if (length == 0) { fprintf(stderr, "empty input\n"); fclose(in); return 1; }
    if (length < 0 || length > 512L * 1024 * 1024 || fseek(in, 0, SEEK_SET)) { fclose(in); return 1; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { fclose(in); return 1; }
    int valid = fread(bytes, 1, (size_t)length, in) == (size_t)length; fclose(in);
    if (!valid) { free(bytes); return 1; }
    rf_pixels image = {0}; rf_error error = {0};
    int rc = rf_decode_image(bytes, (size_t)length, NULL, &image, &error);
    free(bytes);
    if (rc) { fprintf(stderr, "%s\n", error.message); return rc; }
    FILE *out = fopen(argv[2], "wbx");
    if (!out) { perror("output"); rf_free(image.data); return 1; }
    valid = fprintf(out, "P6\n%u %u\n255\n", image.width, image.height) > 0;
    valid = valid && fwrite(image.data, 1, image.size, out) == image.size;
    if (fclose(out)) valid = 0;
    rf_free(image.data);
    if (!valid) remove(argv[2]);
    return valid ? 0 : 1;
}
