---
layout: default
title: Licenses and Provenance
nav_order: 14
source_path: docs/licenses.md
---

# Licenses and Provenance

**Status:** Current (`.fold` format version 3 <!-- drift:fact format_version = 3 -->, public C API 1.0 (`RF_API_VERSION_MAJOR = 1` <!-- drift:fact api_major = 1 -->, `RF_API_VERSION_MINOR = 0` <!-- drift:fact api_minor = 0 -->)).

This page records the license of the RasterFold source code and documentation, the project code of conduct, the provenance and redistribution terms of the committed test photographs, and the pinned build/test toolchain components.

## RasterFold License and Code of Conduct

- **Project license:** RasterFold is licensed under the **GNU General Public License v3.0** (`GPL-3.0`). The full license text is in [`LICENSE`](../LICENSE) at the repository root and is included in every generated target archive (`build/packages/rasterfold-<target>.zip`).
- **Code of conduct:** contributor and maintainer conduct expectations and private reporting instructions (`derick@rizonetech.com`) are in [`CODE_OF_CONDUCT.md`](../CODE_OF_CONDUCT.md).

## Test Image Provenance (`testdata/photos/` and `measure/`)

The repository includes committed reference and test photographs used by `tests/test_correctness.py`, `tests/measure_final.py`, and `tests/benchmark.py`. Their exact SHA-256 pixel hashes, dimensions, and crop rectangles are recorded in [`testdata/photos/manifest.json`](../testdata/photos/manifest.json) and documented in [`testdata/photos/README.md`](../testdata/photos/README.md):

| Path | Count | Source and Redistribution Terms |
| --- | --- | --- |
| `testdata/photos/kodim01.png`, `kodim03.png`, `kodim05.png`, `kodim13.png`, `kodim23.png` | 5 | **Kodak Lossless True Color Image Suite** (`768 x 512`), released by the Eastman Kodak Company for unrestricted usage. |
| `testdata/photos/deer_crop.png`, `fireworks_crop.png`, `flower_foveon_crop.png`, `nightshot_iso_1600_crop.png`, `spider_web_crop.png` | 5 | **imagecompression.info** ("The New Image Compression Test Set - Jan 2008", 8-bit RGB set). Each file is an altered `1024 x 768` center crop redistributed under the terms in [`testdata/photos/NOTICE-imagecompression-info.txt`](../testdata/photos/NOTICE-imagecompression-info.txt), which is included unmodified. These files may not be sold. |
| `measure/image.bmp` | 1 | Committed `1672` <!-- drift:fact measurement_reference_width = 1672 --> x `941` <!-- drift:fact measurement_reference_height = 941 --> 24-bit RGB8 reference image used by `tests/measure_final.py` and the `measure` set in `benchmarks/results.csv`. |

## Pinned Toolchain and Reference Binaries (`toolchain.json`)

Downloaded compilers, build drivers, and benchmark reference binaries are pinned by URL, byte length, and SHA-256 in [`toolchain.json`](../toolchain.json) and installed into ignored `tools/` directories by `tools/bootstrap.py` and `tools/bootstrap.ps1`. None of these external tools are linked into the freestanding Windows codec or distributed inside the repository's Git history:

| Component | Pinned Version | Host | Role |
| --- | --- | --- | --- |
| `llvm-mingw` / `llvm-mingw-linux` | `20260922` (Clang `23.1.2`) | Windows x64, Linux x64 | C11 compiler, LLD linker, `llvm-ar`, and MinGW-w64 import libraries for `windows-x64` and `linux-x64`. |
| `android-ndk` | `r30` (`30.0.16248370`, min API `21`) | Linux x64 | Clang/LLD cross-toolchain and sysroot for `android-arm64` (`arm64-v8a`) and `android-x64` (`x86_64`). |
| `ninja` / `ninja-linux` | `1.13.2` | Windows x64, Linux x64 | Build driver used by `scripts/build.py`. |
| `libjxl` | `0.12.0` | Windows x64 | Reference `cjxl` / `djxl` binaries for comparative benchmark scripts only; nothing in RasterFold links against `libjxl`. |
| `Pillow` (`requirements-dev.txt`) | `12.2.0` | Linux `.venv`, Windows Python | Python image library used only by the test and benchmark scripts (`tests/`). |
| `basedpyright` (`requirements-dev.txt`) | `1.40.2` | Linux `.venv`, Windows `.venv` | Python language server for editors over the tooling; an editor aid, not a gate (D00 T05 s1). |
