---
layout: default
title: Examples
nav_order: 7
source_path: docs/examples.md
---

# Examples

**Status:** Current (`.fold` format version 3 <!-- drift:fact format_version = 3 -->, public C API 1.0 (`RF_API_VERSION_MAJOR = 1` <!-- drift:fact api_major = 1 -->, `RF_API_VERSION_MINOR = 0` <!-- drift:fact api_minor = 0 -->)).

This guide provides working command-line and C library examples for the currently shipped 24-bit RGB8 BMP and 8-bit PNG CLI and in-memory RGB8 C API (`include/rasterfold.h`). The standalone C example programs live in [`examples/encode.c`](../examples/encode.c), [`examples/decode.c`](../examples/decode.c), and [`examples/ppm.h`](../examples/ppm.h).

## Command-Line Examples

`encode` and `decode` refuse an existing output path unless `--overwrite` is given, and produce identical `.fold` bytes across thread counts (`--threads 1` through `--threads 9`).

### Linux or WSL x64

```bash
# Encode a 24-bit BMP image to .fold with timing statistics
./bin/linux-x64/bin/rasterfold encode --time measure/image.bmp build/example.fold

# Inspect the .fold container header, stream sizes, and CRC-32 checksums
./bin/linux-x64/bin/rasterfold inspect --time build/example.fold

# Decode back to a 24-bit BMP image on a single thread
./bin/linux-x64/bin/rasterfold decode --time --threads 1 build/example.fold build/example.bmp
```

### Windows x64 (PowerShell)

```powershell
# Encode, inspect, and decode using the freestanding Windows x64 CLI
.\bin\windows-x64\bin\rasterfold.exe encode --time measure\image.bmp build\example.fold
.\bin\windows-x64\bin\rasterfold.exe inspect --time build\example.fold
.\bin\windows-x64\bin\rasterfold.exe decode --time --threads 1 build\example.fold build\example.bmp
```

## C API Examples (`examples/`)

The repository includes two complete C11 command-line programs that convert binary P6 PPM images (`examples/ppm.h`) to and from `.fold` buffers using `rf_encode_image`, `rf_decode_image`, and `rf_free`:

- [`examples/encode.c`](../examples/encode.c): reads a bounded binary P6 RGB8 PPM file (maxval 255, at most 100000000 pixels) using the header helper in [`examples/ppm.h`](../examples/ppm.h), populates `rf_pixels` (`RF_LAYOUT_RGB8`, stride `width * 3`), calls `rf_encode_image(&pixels, &options, &encoded, &error)`, writes the resulting `.fold` buffer to an exclusive output file (`fopen(..., "wbx")`), and releases `encoded.data` with `rf_free`.
- [`examples/decode.c`](../examples/decode.c): reads a `.fold` file into memory, calls `rf_decode_image(bytes, length, NULL, &image, &error)`, writes the returned packed top-down RGB8 pixels (`image.data`, `image.size`) as a binary P6 PPM file without overwriting an existing destination, and releases `image.data` with `rf_free`.
- [`examples/ppm.h`](../examples/ppm.h): minimal P6 header tokenizer (`token` and `dimension`) that accepts `#` comments, whitespace, and CRLF line endings.

### Compile and Run against the Static Library (Linux or WSL x64)

```bash
tools/llvm-mingw-linux/bin/clang --target=x86_64-linux-gnu -std=c11 \
  -Iinclude examples/encode.c bin/linux-x64/lib/librasterfold.a -pthread -o build/encode-ppm
tools/llvm-mingw-linux/bin/clang --target=x86_64-linux-gnu -std=c11 \
  -Iinclude examples/decode.c bin/linux-x64/lib/librasterfold.a -pthread -o build/decode-ppm
```

### Compile and Run against the Shared Library (Linux or WSL x64)

```bash
tools/llvm-mingw-linux/bin/clang --target=x86_64-linux-gnu -std=c11 \
  -Iinclude examples/encode.c -Lbin/linux-x64/lib -lrasterfold \
  -Wl,-rpath,"$PWD/bin/linux-x64/lib" -o build/encode-ppm-shared
```

### Compile against the Shared DLL or Static Archive (Windows x64)

```powershell
# Link against the Windows DLL import library (define RF_SHARED)
.\tools\llvm-mingw\bin\clang.exe --target=x86_64-w64-mingw32 -std=c11 -DRF_SHARED `
  -Iinclude examples\encode.c bin\windows-x64\lib\librasterfold.dll.a -o build\encode-ppm.exe

# Link against the Windows static archive (hosted caller supplies its C runtime plus kernel32)
.\tools\llvm-mingw\bin\clang.exe --target=x86_64-w64-mingw32 -std=c11 `
  -Iinclude examples\decode.c bin\windows-x64\lib\librasterfold.a -lkernel32 -o build\decode-ppm.exe
```

## Inspecting a `.fold` Buffer in Memory

Call `rf_inspect` to validate both CRC-32 checksums and decode the full bitstream before reading image metadata:

```c
#include "rasterfold.h"

rf_info info = {0};
rf_error error = {0};
int rc = rf_inspect(fold_bytes, fold_size, &info, &error);
if (rc == RF_OK) {
    /* info.width, info.height, info.method, info.payload_size,
       info.payload_crc, info.header_crc, and info.stream_size[0..2] are valid. */
}
```

See the [C API Guide](c-api.md) for full buffer ownership and concurrency rules, and [Troubleshooting](troubleshooting.md) for error codes and refusal diagnostics.
