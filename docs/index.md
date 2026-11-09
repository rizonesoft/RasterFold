---
layout: default
title: Overview
nav_order: 1
source_path: docs/index.md
---

# RasterFold Documentation

<p><img src="../assets/brand/cover.svg" alt="RasterFold. Every pixel, preserved. Lossless RGB image compression for Windows, Linux and Android." width="960"></p>

RasterFold is a lossless RGB image codec written in C11 for Windows x64, Linux x64, Android ARM64, and Android x64. It supplies a command-line tool (`rasterfold` / `rasterfold.exe`) and shared and static C libraries through `include/rasterfold.h`.

## Current Version and Limits

- **Current encoded format:** `.fold` format version 3 <!-- drift:fact format_version = 3 --> (`32`-byte header <!-- drift:fact header_bytes = 32 -->, CRC-32 verified header and payload).
- **Current product version:** 1.0.0 <!-- drift:fact product_version = 1.0.0 --> (pre-1.0 development; the release is D09 T01 §4).
- **Current public C API:** version 1.0 (`RF_API_VERSION_MAJOR = 1` <!-- drift:fact api_major = 1 -->, `RF_API_VERSION_MINOR = 0` <!-- drift:fact api_minor = 0 -->).
- **Current image limits:** maximum dimension `32768` pixels <!-- drift:fact max_dimension = 32768 -->; maximum image `33554432` pixels <!-- drift:fact max_pixels = 33554432 -->; maximum file size `268435456` bytes <!-- drift:fact max_file_bytes = 268435456 -->.
- **Current vs. historical vs. planned scope:**
  - **Current:** the CLI (`encode` <!-- drift:fact cmd_encode = encode -->, `decode` <!-- drift:fact cmd_decode = decode -->, `inspect` <!-- drift:fact cmd_inspect = inspect -->) and C library (`include/rasterfold.h`) for 24-bit RGB8 BMP, 8-bit PNG, and in-memory RGB8 buffers are shipped in source and built for `windows-x64`, `linux-x64`, `android-arm64`, and `android-x64`.
  - **Historical:** the `photos` and `synthetic` sets in `benchmarks/results.csv` are retained from `2026-09-29` (`0159404532cf944c4265ecb8ea46c54f637bdfc2`), and pre-migration Windows measurements are preserved in [`docs/measurements.md`](measurements.md).
  - **Planned (not downloadable yet):** the native desktop/mobile converter application and downloadable release packages are planned in [the product vision](vision.md) and are not yet offered as downloads.

## Complete Documentation Map

- [Getting Started](getting-started.md): provision the pinned toolchain, build all four targets, and run the first encode, decode, and inspect commands.
- [Installation and Build](installation.md): four-target host prerequisites, Linux/WSL and Windows build commands, artifact layout, Android NDK cross-builds, and development archives mirrored from `docs/building.md`.
- [CLI and Library Usage](usage.md): target artifacts, CLI commands and flags, exit codes, and C API embedding overview mirrored from `README.md`.
- [C API Guide](c-api.md): buffer ownership, concurrency, options validation, static/shared linking on Linux/Windows/Android, and return codes mirrored from `docs/library.md`.
- [API Reference](api.md): every function, type, constant, and error code in `include/rasterfold.h` with its ownership rule, generated from the header.
- [Format Specification](format.md): the normative `.fold` version 3 container and context-mixing (CM) bitstream specification mirrored from `spec/FOLD.md`.
- [Examples](examples.md): CLI round-trip workflows and complete C11 PPM encode/decode consumer examples (`examples/encode.c`, `examples/decode.c`, `examples/ppm.h`).
- [Benchmarks](benchmarks.md): corpus and reference-image measurements, static SVG comparison charts, and data tables comparing RasterFold with JPEG XL (`e7` and `e9`), lossless WebP, and PNG.
- [Research Summary](research-summary.md): where the codec stands, the measurement protocol, experiments already tried, and prior art mirrored from `docs/research.md`.
- [Limitations](limitations.md): current format, pixel layout, dimension/file caps, memory model, and platform boundaries.
- [Troubleshooting](troubleshooting.md): exit codes `0..5`, exact BMP/container/API refusal messages, atomic output publication recovery, and toolchain repair.
- [Contributing](contributing.md): repository workflow, format/safety contracts, and verification gates mirrored from `CONTRIBUTING.md`.
- [Security Policy](security.md): private vulnerability reporting and scope mirrored from `SECURITY.md`.
- [Licenses and Provenance](licenses.md): GPL-3.0 license, code of conduct, Kodak and `imagecompression.info` test image provenance, and pinned toolchain components.
