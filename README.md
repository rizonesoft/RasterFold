# RasterFold

RasterFold is an experimental lossless image codec for Windows 11 x64. The implementation is x86-64 assembly. The conventional extension is `.rfi`. Version 1 is a measured baseline, not a claim that it beats any existing codec.

The research target is screenshots, application interfaces, diagrams, and pixel art, compared with lossless WebP on size, decode speed, and memory. Correctness comes first. The numbers below are from one machine and one run.

## Build

The build uses Microsoft `ml64`, Microsoft's linker, and the Windows SDK. `build.ps1` locates them with `vswhere`.

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

That writes `build\rasterfold.exe`. The run recorded here used Macro Assembler (x64) 14.51.36256.0 and Windows SDK 10.0.26100.0.

There is no C or C++ in the codec. Python is used only for tests, image generation, and the benchmark driver. PNG, WebP, and JPEG XL in the benchmark come from ffmpeg, not from this tree.

## Commands

```
rasterfold encode [--time] input.bmp output.rfi
rasterfold decode [--time] input.rfi output.bmp
rasterfold inspect [--time] input.rfi
```

Exit codes are 0 for success, 1 for usage, 2 for file errors, 3 for an unsupported BMP or RasterFold version, 4 for a malformed file, and 5 for a size limit or allocation failure.

An existing destination is left untouched. Output is written to `destination.partial` with `CREATE_NEW` and renamed onto the destination only after the bytes are flushed. A failed conversion deletes that temporary file and does not create the destination.

`--time` prints phase counters on stdout. `codec_us` is block encode or block decode. `total_us` adds BMP conversion and file IO, including the flush. `encode_wall_us` in the benchmark is the whole process, startup included.

## Codec

Each image is cut into independent 64×64 blocks, with smaller blocks on the right and bottom edges. A block does not use pixels from its neighbours. Scratch for one block is fixed at 64×64, so a hostile length cannot choose the scratch allocation. The output image is still one buffer.

For every block the encoder tries five modes and stores the smallest payload. A tie keeps the lower mode number, so raw storage wins a tie. That choice is encoder policy. A decoder accepts any mode.

| Mode | Contents |
|---|---|
| 0 | Raw RGB, top to bottom, R then G then B |
| 1 | PackBits of those RGB bytes |
| 2 | Subtract-green, then a left predictor that restarts at 0 on every row. Residuals are stored as R', G', and B' planes and PackBits-coded as one stream |
| 3 | A palette of 1..256 colours in first-seen order, then PackBits of one index byte per pixel |
| 4 | Subtract-green, MED from the left and above samples, then a context Rice coder. The model carries across mode-4 blocks |

Subtract-green and the predictor use 8-bit wraparound, the low 8 bits of an x86 byte add or subtract. PackBits rejects the `0x80` no-op and must consume every input byte while producing exactly the expected count. The file is little-endian. Both checksums are zlib CRC-32: one over the payload, one over the first 28 header bytes (which include the payload checksum). The byte layout, rejection rules, and worked predictor example are in [spec/RFI-v1.md](spec/RFI-v1.md).

PackBits plus a raw fallback is small enough to audit, and it does compress long runs. It is a weak coder for anything that is regular but not a repeated byte. Mode 4 adds a context Rice coder on MED residuals. There is no range coder, no LZ matcher, and no AVX2 path.

The accepted BMP subset is uncompressed 24-bit `BITMAPINFOHEADER` data, bottom-up or top-down, with rows padded to 4 bytes. Other bit depths, compression, and header versions are rejected by name. A decoded BMP is a new canonical bottom-up file. Lossless means the width, height, and RGB samples match. Headers and padding bytes are not preserved.

Limits are 1..32768 on each side, at most 32 megapixels, and at most 256 MiB for an input file.

## Tests

```
python tests\test_correctness.py
```

On 2026-09-28 that suite reported 30 passed, 0 failed. It covers a single pixel, odd widths, padded rows, top-down BMPs, a gap before the pixels, solid colours, gradients, checkers, noise, and UI-like pictures. It also feeds truncated, checksum-broken, and mode-corrupted `.rfi` files, unsupported BMP variants, and paths that must not be overwritten.

Pixels are compared with Pillow and with a separate BMP parser in `tests/rfi_ref.py`. That module is a second implementation of version 1, used only as a test oracle. The assembly encoder's bytes match the oracle, and each decoder accepts files written by the other. The program also checks CRC-32("123456789") = `0xCBF43926`, two PackBits vectors, and the planar predictor example before it will encode anything.

## Measurements

`python tests\benchmark.py` writes `benchmarks/baseline.md` and `benchmarks/baseline.csv`. The images are synthetic pictures drawn for this run (Pillow 12.3.0), split into a development set and a held-out set. The codec was not changed after these numbers were recorded. Every successful decode matched the source pixels. There were no tool failures.

Machine: Windows 11 (10.0.26200), Intel Core i5-11600K at 3.90 GHz, 6 cores and 12 logical processors, 34185392128 bytes of RAM. Every codec ran on one thread. ffmpeg was `8.1.2-full_build-www.gyan.dev` with its bundled libwebp and libjxl; that build does not print those library versions on their own.

Selected sizes in bytes, development set:

| image | raw RGB | RasterFold | PNG | WebP | JPEG XL | blocks used |
|---|---:|---:|---:|---:|---:|---|
| solid 640×480 | 921600 | 5632 | 1602 | 56 | 125 | palette 80 |
| ui 1280×720 | 2764800 | 26468 | 5790 | 1346 | 2316 | palette 239, packbits 1 |
| text 800×600 | 1440000 | 18271 | 4512 | 1198 | 2164 | palette 129, packbits 1 |
| gradient 640×480 | 921600 | 57872 | 3414 | 180 | 2886 | predict 80 |
| noise 256×256 | 196608 | 196736 | 197584 | 196688 | 212784 | raw 16 |

Held-out set, same pattern:

| image | raw RGB | RasterFold | PNG | WebP | JPEG XL | blocks used |
|---|---:|---:|---:|---:|---:|---|
| ui 1024×768 | 2359296 | 24183 | 5646 | 1324 | 2167 | palette 191, packbits 1 |
| text 640×400 | 768000 | 11410 | 3484 | 1136 | 1730 | palette 69, packbits 1 |
| gradient 333×200 | 199800 | 14512 | 852 | 96 | 696 | predict 24 |
| noise 320×180 | 172800 | 172922 | 173623 | 172880 | 187664 | raw 15 |

On the UI, text, diagram, checker, stripe, and gradient images, lossless WebP is the smallest, then JPEG XL, then PNG, then RasterFold. RasterFold is still far below the raw size on those images. On both noise images the raw fallback stays within a few block headers of the raw RGB size and matches WebP; JPEG XL is larger than raw there.

Speed and memory go the other way. For the development UI image, RasterFold's block encoder took 8089 µs and the whole encode process took 39720 µs, with a peak working set of 12206080 bytes and a heap high-water mark of 8358214 bytes. ffmpeg's process wall times on that file were 89746 µs for PNG, 1611033 µs for WebP, and 268271 µs for JPEG XL, with peak working sets of 29102080, 44437504, and 58249216 bytes. ffmpeg's times include startup and muxing. RasterFold's `codec_us` does not. Its process wall time, which does include startup, is still lower on every image in both sets. Decode shows the same split: block decode of that UI image was 1683 µs, while the ffmpeg decodes took 74408 to 131256 µs of wall time.

## Next step

The size gap on the images this project cares about is inside palette blocks and predict blocks, not in the raw fallback. A 1280×720 UI image is 240 blocks. The 6-byte headers are 1440 bytes, and the file is 26468 bytes, against a WebP file of 1346 bytes. A 640×480 gradient is entirely predict mode and is 57872 bytes, against WebP at 180. PackBits only collapses runs of the same byte, so a regular index stream or a planar residual still occupies hundreds of bytes per block.

Mode 4 is that entropy coder for photographic residuals: subtract-green, a MED predictor, and a 64-context Rice code, used only when it is smaller than the other modes. The raw, PackBits, and palette paths stay. On `measure/image.bmp` this is the whole file. The remaining size gap against lossless JPEG XL is the predictor and the context model, not another PackBits pass. Decode has to stay within JPEG XL's wall time; the Rice decoder is already a large share of that budget.

## Limitations

- Input and output are 24-bit BMP only. No alpha, extra bit depths, colour profiles, animation, or shell integration.
- The format is experimental. Version 1 may still change incompatibly.
- The whole image is decoded into one buffer. Block scratch is bounded; the output image is not streamed.
- There is no AVX2 path and no runtime CPU dispatch yet.
- The benchmark pictures are generated. They are not a corpus of captured desktop screenshots.
- ffmpeg wall times are not codec-only times, and this ffmpeg build does not identify the exact libwebp or libjxl version beyond the ffmpeg version string above.
