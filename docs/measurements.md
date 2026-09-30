# Historical Windows measurements

These are the pre-migration Windows results recorded in the original README at commit `97e5ca5` (2026-09-29). They are retained as a baseline, not a new cross-platform performance claim. The 2026-09-30 migration checks verify correctness, ABI and builds; they do not remeasure compression speed or the JPEG XL comparison. Reproduce comparisons on a quiet machine before relying on these timings.

## Measurements

Machine: Windows 11 (10.0.26200), Intel Core i5-11600K at 3.90 GHz, 6 cores and
12 logical processors. The machine was running other work during these runs,
so times vary by about ±10% between runs. ffmpeg 8.1.2 (gyan.dev full build)
supplied PNG, WebP, JPEG XL and AVIF.

`python tests/measure_final.py` on `measure/image.bmp` (1672×941), unchanged
from its original form:

| codec | bytes | decode wall | decode peak working set |
|---|---:|---:|---:|
| RasterFold | **1,409,028** | **261 ms** | **17.3 MB** |
| JPEG XL, effort 7 | 1,470,703 | 434 ms | 53.6 MB |
| WebP, lossless, level 6 | 1,588,708 | 195 ms | 44.6 MB |
| AVIF, lossless | 1,776,841 | 345 ms | 49.1 MB |

That script encodes JPEG XL with `-threads 1` but decodes it with ffmpeg's
default thread count, so JPEG XL decodes on several threads there. Encoding
this image takes RasterFold about 0.25 s and JPEG XL about 1.7 s. With one
thread each, RasterFold decodes it in about 0.58 s and JPEG XL in about 0.72 s.

`python tests/benchmark.py` writes `benchmarks/results.md` for this image, the
ten photographs in `testdata/photos/`, and eleven generated images. Every value
below is RasterFold divided by JPEG XL effort 7, so lower is better. Values are
geometric means of the per-image ratios, except the total-size column. Times
are process wall times (median of 3 runs) and memory is the process peak
working set.

| set | total size | size | encode | encode, 1 thread each | decode | decode, 1 thread each | encode memory | decode memory |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `measure/image.bmp` | 0.958 | 0.958 | 0.15 | 0.37 | 0.58 | 0.80 | 0.19 | 0.33 |
| 10 photographs | 0.974 | 0.972 | 0.15 | 0.30 | 0.52 | 0.71 | 0.22 | 0.33 |
| 11 generated images | 0.948 | 1.199 | 0.34 | 0.55 | 0.42 | 0.63 | 0.29 | 0.32 |

The "encode" and "decode" columns compare RasterFold's default three threads
with JPEG XL encoding on one thread and decoding with ffmpeg's default threads.
The "1 thread each" columns run both codecs on one thread. All ten test
photographs are smaller than JPEG XL. The generated set is mixed. Stripes,
solid fills, the UI, text and diagram images, and noise are smaller. The
checkerboard is 18 times larger, the one-pixel checker "pixel art" 9 times, the
smooth gradients 1.2 to 1.6 times. JPEG XL has a palette transform for such
content, and lossless WebP is smaller than both codecs on most of these images.
The palette method is designed in `docs/research.md` (section 4.1) and planned as `D01 T01`.

On a wider research corpus (the 24 Kodak photographs, 14 center crops of the
imagecompression.info 8-bit RGB set, and `measure/image.bmp`), RasterFold's
files total 2.9% less than JPEG XL effort 7's, and 37 of the 39 are smaller;
kodim17 and kodim09 are 0.4% and 0.2% larger. `research/README.md` explains
how to reproduce these numbers.

## Limitations

- Input and output are 24-bit BMP only. No alpha, 16-bit samples, colour
  profiles, or animation.
- The whole image is held in memory; decoding does not stream.
- Encoder and decoder are symmetric: decoding costs about as much as encoding.
- Palette-like synthetic content compresses worse than with JPEG XL or WebP.
