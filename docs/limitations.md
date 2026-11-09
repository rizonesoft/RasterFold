---
layout: default
title: Limitations
nav_order: 10
source_path: docs/limitations.md
---

# Limitations

**Status:** Current (`.fold` format version 3 <!-- drift:fact format_version = 3 -->, public C API 1.0 (`RF_API_VERSION_MAJOR = 1` <!-- drift:fact api_major = 1 -->, `RF_API_VERSION_MINOR = 0` <!-- drift:fact api_minor = 0 -->)).

This page documents the technical boundaries of the currently shipped RasterFold CLI and C libraries, and separates current behavior from historical baseline data and planned features.

## Input and Pixel Format Boundaries

- **CLI image format (`rasterfold` / `rasterfold.exe`):** the command-line tool currently reads and writes uncompressed 24-bit RGB8 BMP files and 8-bit RGB or fully opaque RGBA PNG files (`BITMAPINFOHEADER` for BMP, `.fold` header size `32` bytes <!-- drift:fact header_bytes = 32 -->, 1 plane, 24 bits per pixel, top-down or bottom-up rows). BMP files with palettes, bitfields, RLE compression, 16/32-bit samples, or non-40-byte DIB headers are refused with exit code `3` <!-- drift:fact exit_unsupported = 3 --> (`unsupported`).
- **C API pixel layout (`include/rasterfold.h`):** `rf_encode_image` and `rf_decode_image` currently support `RF_LAYOUT_RGB8` (`1`, packed or row-padded 8-bit RGB, 3 bytes per pixel). Alpha channels (RGBA), grayscale, 16-bit integer channels, HDR/floating-point samples, ICC profiles, and EXIF/XMP metadata are not part of `.fold` format version 3 <!-- drift:fact format_version = 3 -->.

## Dimension, Pixel, and File Size Caps

Enforced identically by the CLI, C API, and container parser (`src/rf.h`, `src/fold.c`, `src/bmp.c`, `src/api.c`):

- **Maximum dimension:** `32768` pixels <!-- drift:fact max_dimension = 32768 --> per axis (`RF_MAX_DIM`).
- **Maximum image:** `33554432` pixels <!-- drift:fact max_pixels = 33554432 --> total (`RF_MAX_PIXELS`, e.g. `8192 x 4096`).
- **Maximum file size:** `268435456` bytes <!-- drift:fact max_file_bytes = 268435456 --> (`RF_MAX_FILE`, 256 MiB) for input files and encoded `.fold` buffers.
- Inputs exceeding these bounds are rejected before allocation with exit/error code `5` <!-- drift:fact exit_limit = 5 --> (`limit`).

## Memory and Threading Model

- **Whole-image buffer model:** `rf_encode_image`, `rf_decode_image`, and `rf_inspect` operate on complete in-memory buffers rather than incremental row-by-row streaming I/O.
- **Full verification on `inspect`:** `rf_inspect` (and `rasterfold inspect` <!-- drift:fact cmd_inspect = inspect -->) verifies the header CRC-32, payload CRC-32, and decodes the complete payload to confirm that the bitstream is valid before reporting `status: ok`.
- **Channel and strip parallelism:** method 1 (CM) splits the image into 3 decorrelated channels (`G`, `R - G`, `B - G`) and up to 4 horizontal strips per channel (`S = min(4, max(1, height / 256))`), running up to `3 * S` strips in parallel across `--threads 1..9` (default `clamp(rf_cpu_count(), 1, 9)`). All thread counts produce byte-identical `.fold` output.

## Platform and Target Scope

- **CPU instruction set on x64:** `windows-x64`, `linux-x64`, and `android-x64` builds target `x86-64-v2` (SSE4.2 / POPCNT). Hosts without `x86-64-v2` support cannot execute the default x64 binaries; `--portable` builds a separate portable C arithmetic test package (`bin/<target>-portable/`).
- **Android cross-compilation vs. device verification:** the WSL/Linux build cross-compiles `android-arm64` (`arm64-v8a`) and `android-x64` (`x86_64`) against the pinned Android NDK (`r30`, minimum API 21) and verifies ELF headers, symbols, and 16 KiB segment alignment. Cross-compilation and ELF inspection do not substitute for running on an Android device or emulator.
- **Destination safety on older filesystems:** output publication uses `SetFileInformationByHandle` (`FileRenameInfoEx` without replacement) on Windows and `renameat2(..., RENAME_NOREPLACE)` with an exclusive `linkat` fallback on Linux/Android. Filesystems that support neither non-replacing rename nor hard links are refused without modifying the destination path.

## Current vs. Historical vs. Planned Scope

- **Current:** `.fold` format version 3 <!-- drift:fact format_version = 3 -->, C API 1.0 (`RF_API_VERSION_MAJOR = 1` <!-- drift:fact api_major = 1 -->, `RF_API_VERSION_MINOR = 0` <!-- drift:fact api_minor = 0 -->), `rasterfold` CLI (`encode` <!-- drift:fact cmd_encode = encode -->, `decode` <!-- drift:fact cmd_decode = decode -->, `inspect` <!-- drift:fact cmd_inspect = inspect -->, `verify`, `help`, `version`), and development archives for `windows-x64`, `linux-x64`, `android-arm64`, and `android-x64`.
- **Historical:** the `photos` and `synthetic` benchmark sets in `benchmarks/results.csv` are retained from the `2026-09-29` baseline (`0159404532cf944c4265ecb8ea46c54f637bdfc2`, where `jxl-e9` was not timed), and `docs/measurements.md` preserves the pre-migration Windows record. See [Benchmarks](benchmarks.md) and [Research Summary](research-summary.md).
- **Planned (not yet shipped):** the native desktop and mobile converter application (with bundled multi-format input/output decoding and downloadable installer packages) and script-generated CLI/API reference tables are planned in [the product vision](../docs/vision.md) and are not currently available as downloads.
