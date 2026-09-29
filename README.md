# RasterFold

RasterFold is a lossless RGB image codec for Windows x64. It is written in C
with no C runtime (Win32 only) and built with a pinned local Clang toolchain.
The file extension is `.fold`. The format is version 2 and is described in
[spec/FOLD.md](spec/FOLD.md).

On the reference photograph `measure/image.bmp` it is smaller than lossless
JPEG XL at effort 7, decodes faster than JPEG XL does with all threads, encodes
more than six times faster, and uses about a third of the memory. On a corpus
of 39 photographs it is 2.9% smaller than JPEG XL overall. Images with very few
colours, such as checkerboards and pixel art, are still much larger than with
JPEG XL. The measurements and their conditions are below.

## Build

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

The first run executes `tools\bootstrap.ps1`, which downloads and checks the
SHA-256 of three pinned archives into `tools\`: llvm-mingw 20260922 (Clang 23,
LLD, MinGW-w64 import libraries), Ninja 1.13.2, and the libjxl 0.12.0 command
line tools (a reference codec for benchmarks; nothing links against it).
`build.ps1` then runs Ninja on `build.ninja`, which writes objects to
`build\obj\` and the executable to `Bin\rasterfold.exe`. No Visual Studio
installation is needed. The code targets x86-64-v2 (SSE4.2), so it runs on any
x86-64 CPU from the last fifteen years.

## Commands

```
rasterfold encode [--time] [--threads N] input.bmp output.fold
rasterfold decode [--time] [--threads N] input.fold output.bmp
rasterfold inspect [--time] input.fold
```

Exit codes are 0 for success, 1 for usage, 2 for file errors, 3 for an
unsupported BMP or `.fold` version, 4 for a malformed file, and 5 for a size
limit or allocation failure. An existing destination is never overwritten:
output goes to `destination.partial` with `CREATE_NEW`, is flushed, and is
renamed onto the destination only after success.

`--threads 1` codes on one thread. The default uses three threads, one per
colour channel, when the CPU has at least three logical processors. The file
bytes are the same for any thread count.

`--time` prints `rf_stat` lines: `read_us`, `parse_us`, `codec_us`, `write_us`,
`total_us`, `alloc_peak`, `peak_working_set`, and `output_bytes`.

## How it works

Each pixel is coded as three channels: G, then R − G, then B − G. Each channel
has its own models and its own arithmetic-coded stream.

1. **Prediction.** The JPEG XL weighted predictor (four sub-predictors mixed
   by their recent errors), followed by a 12-tap sign-sign LMS stage that
   corrects it from neighbouring errors, the G and R errors at the same pixel,
   and five local gradients.
2. **Contexts.** Five context models look at the neighbours relative to the
   prediction on a scale set by the local error energy, the JPEG-LS gradients,
   four simple alternative predictors, the error energy with the G and R
   residuals of this pixel, and W and N against the prediction.
3. **Coding.** The residual is binarized (zero flag, sign, exponent in unary,
   mantissa). Each binary decision mixes the five models' probabilities with a
   logistic mixer and is coded by an LZMA-style binary range coder. Counters
   adapt at a rate set by their context's visit count.
4. **Threads.** R depends on G only at the same pixel, and B on G and R. The
   three channels therefore run as a pipeline, one thread each, R one row behind
   G and B one row behind R. On one thread the same pipeline runs one pixel
   apart, which lets the processor overlap the three channels.

If the coded payload is not smaller than the raw image, the file stores the raw
pixels instead (method 0), so a `.fold` file is never more than 32 bytes larger
than the raw RGB data.

The accepted input is the uncompressed 24-bit `BITMAPINFOHEADER` BMP subset,
bottom-up or top-down. A decoded BMP is a canonical bottom-up file; lossless
means the width, height and RGB samples match. Limits are 1..32768 per side,
at most 32 megapixels, and at most 256 MiB for an input file.

## Source layout

| file | contents |
|---|---|
| `src/cm.c` | the CM method: predictor, contexts, mixer, range coder, threads |
| `src/fold.c` | the `.fold` container: header, checksums, method choice |
| `src/bmp.c` | BMP reading and writing |
| `src/crc32.c` | zlib CRC-32, slicing by 8 |
| `src/sys.c` | Win32: heap, files, atomic output, timing, threads |
| `src/main.c` | command line |
| `src/mem.c` | `memcpy`, `memset`, `memmove`, `memcmp`, `strlen` |
| `tests/rfi_ref.py` | independent Python implementation of the format, the test oracle |

## Development workflow

Work is planned and tracked in [`todo/`](todo/README.md): each change is a TODO
section with a checklist, a falsifiable test checkpoint, and, for size and
speed ideas, a measured stop condition. [`todo/implementation-plan.md`](todo/implementation-plan.md)
orders the sections in phases, and `python scripts/todo-graph.py query ready`
lists what can run now. The Claude Code skills in `.claude/skills/` run the
lifecycle (`add-todo`, `plan-experiment`, `process-todo-section`,
`review-todo-section`, `process-plan`); [`AGENTS.md`](AGENTS.md) holds the
working rules, and [`docs/research.md`](docs/research.md) the research record and
measurement protocol.

```
pwsh tools\provision.ps1          set up a clone: toolchain, git hooks, Python and Pillow
pwsh scripts\check-all.ps1        every gate: build, correctness, fuzz, plan and tool checks
```

The commit hook (`tools/githooks/pre-commit`) refuses a commit whose staged
plan fails validation, and the `plan-gates` and `build` workflows repeat the
checks on every push.

## Tests

```
python tests\test_correctness.py      round trips and rejection tests
python tests\fuzz_decode.py [n]       mutated files must be rejected cleanly
python tests\measure_final.py         the reference comparison on measure/image.bmp
python tests\benchmark.py             size, speed and memory against PNG, WebP, JPEG XL
```

`test_correctness.py` (33 cases) checks every encoder output byte for byte
against the Python oracle, decodes codec files with the oracle and oracle files
with the codec, compares one-thread and three-thread output, checks a frozen
known-answer file, and feeds truncated, extended, checksum-broken and
out-of-range files, unsupported BMPs, and paths that must not be overwritten.
`fuzz_decode.py` mutates valid files with the checksums recomputed, so the
damage reaches the CM decoder; the decoder must exit 0 or 4, never crash or
hang, and agree with the oracle whenever it accepts a file.

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
