---
layout: default
title: Troubleshooting
nav_order: 11
source_path: docs/troubleshooting.md
---

# Troubleshooting

**Status:** Current (`.fold` format version 3 <!-- drift:fact format_version = 3 -->, public C API 1.0 (`RF_API_VERSION_MAJOR = 1` <!-- drift:fact api_major = 1 -->, `RF_API_VERSION_MINOR = 0` <!-- drift:fact api_minor = 0 -->)).

This guide lists every CLI exit code, C API return code, exact diagnostic string from `src/main.c`, `src/fold.c`, `src/cm.c`, `src/pal.c`, `src/bmp.c`, `src/api.c`, and `src/sys_posix.c`, and the recovery steps for build and runtime failures.

## Exit and Error Codes (`0..5`)

Both the `rasterfold` CLI and the C API (`include/rasterfold.h`) use the same six status codes:

| Code | Constant | CLI Meaning | When Returned |
| --- | --- | --- | --- |
| `0` <!-- drift:fact exit_ok = 0 --> | `RF_OK` | `ok` <!-- drift:fact exit_ok_meaning = ok --> | Command or API call completed and verified all checksums. |
| `1` <!-- drift:fact exit_usage = 1 --> | `RF_ERR_USAGE` | `usage` <!-- drift:fact exit_usage_meaning = usage --> | Invalid CLI flags/arguments or invalid C API pointers, stride, or options alignment/thread count. |
| `2` <!-- drift:fact exit_io = 2 --> | `RF_ERR_IO` | `io` <!-- drift:fact exit_io_meaning = io --> | Input file cannot be read, output path already exists, or atomic output publication/cleanup failed. |
| `3` <!-- drift:fact exit_unsupported = 3 --> | `RF_ERR_UNSUPPORTED` | `unsupported` <!-- drift:fact exit_unsupported_meaning = unsupported --> | Unsupported `.fold` version/header/method/pixel format, non-24-bit or compressed BMP, or nonzero reserved API option. |
| `4` <!-- drift:fact exit_malformed = 4 --> | `RF_ERR_FORMAT` | `malformed` <!-- drift:fact exit_malformed_meaning = malformed --> | Truncated file, bad magic/signature, zero dimensions, header/payload CRC-32 mismatch, or corrupt CM/palette stream. |
| `5` <!-- drift:fact exit_limit = 5 --> | `RF_ERR_LIMIT` | `limit` <!-- drift:fact exit_limit_meaning = limit --> | Dimension above `32768` <!-- drift:fact max_dimension = 32768 -->, pixel count above `33554432` <!-- drift:fact max_pixels = 33554432 -->, file size above `268435456` bytes <!-- drift:fact max_file_bytes = 268435456 -->, or allocation failure. |

## File I/O and Output Publication Refusals (Exit Code `2`)

RasterFold writes to an exclusive sibling `<destination>.partial` file, flushes file data, and publishes without replacing an existing file (`src/sys.c`, `src/sys_posix.c`):

- `rasterfold: output file already exists`: RasterFold never overwrites an existing destination file. Choose a new output path or remove the old output file explicitly before re-running.
- `rasterfold: temporary output already exists`: another active conversion is writing `<destination>.partial`, or an earlier interrupted process left `<destination>.partial` behind (`src/sys.c`, `src/sys_posix.c`). **Recovery:** first confirm that no running `rasterfold` process or caller still owns `<destination>.partial` (removing an active writer's temporary file disrupts its final rename/link publication); if another writer is active, choose a different output path, otherwise remove the stale `<destination>.partial` file and retry.
- `rasterfold: output published but temporary cleanup failed` or `rasterfold: output published but directory flush failed`: the destination file was already published with flushed contents, but unlinking the hard-linked `.partial` file or flushing the parent directory descriptor failed (`src/sys_posix.c`). **Recovery:** verify the published destination file before deleting or overwriting it -- for an encoded `.fold` destination, run `rasterfold inspect <destination.fold>`, and for a decoded BMP destination, verify the published `.bmp` header/dimensions or compare against the source `.fold` (since `rasterfold inspect` only validates `.fold` containers) -- then remove any remaining `<destination>.partial` link once no writer owns it.
- `rasterfold: cannot read input` or `rasterfold: cannot access output path` or `rasterfold: cannot write output` or `rasterfold: path too long`: verify that the input path is a regular file, the parent directory exists and is writable, and the path is under 1,024 bytes on POSIX.

## Format and BMP Refusals (Exit Codes `3`, `4`, `5`)

### BMP Diagnostics (`src/bmp.c`)

- `unsupported BMP: header`, `unsupported BMP: planes`, `unsupported BMP: bit count`, `unsupported BMP: compression` (exit code `3` <!-- drift:fact exit_unsupported = 3 -->): the CLI only accepts uncompressed 24-bit RGB8 BMP files with a 40-byte `BITMAPINFOHEADER`. Convert paletted, 32-bit RGBA, or compressed BMPs to 24-bit RGB BMP first.
- `invalid BMP: truncated`, `invalid BMP: signature`, `invalid BMP: dimensions`, `invalid BMP: file size`, `invalid BMP: image size` (exit code `4` <!-- drift:fact exit_malformed = 4 -->): the BMP header or row data is incomplete or inconsistent with the file length.

### `.fold` Container, CM Stream, and Palette Stream Diagnostics (`src/fold.c`, `src/cm.c`, `src/pal.c`)

- `unsupported fold: version`, `unsupported fold: header size`, `unsupported fold: pixel format`, `unsupported fold: method` (exit code `3` <!-- drift:fact exit_unsupported = 3 -->): the file was written by an incompatible `.fold` version or uses an unknown coding method. Current decoders accept `.fold` format version 3 <!-- drift:fact format_version = 3 --> and version 2 files (`32`-byte header <!-- drift:fact header_bytes = 32 -->, `RGB8`, method `0` raw, method `1` CM, method `2` palette, method `3` extended palette, or, in version 3 files only, method `4` compact palette).
- `invalid fold: truncated`, `invalid fold: magic`, `invalid fold: header checksum`, `invalid fold: reserved byte`, `invalid fold: dimensions`, `invalid fold: payload length`, `invalid fold: payload checksum`, `invalid fold: raw length`, `invalid fold: cm strips`, `invalid fold: cm streams`, `invalid fold: cm stream`, `invalid fold: palette`, `invalid fold: duplicate palette colour`, `invalid fold: palette stream` (exit code `4` <!-- drift:fact exit_malformed = 4 -->): the `.fold` file is corrupted or truncated. Both the 28-byte header prefix and the compressed payload are protected by CRC-32 (`spec/FOLD.md`).
- `image exceeds size limit` or `input exceeds size limit` (exit code `5` <!-- drift:fact exit_limit = 5 -->): dimensions exceed `32768` pixels <!-- drift:fact max_dimension = 32768 -->, total pixels exceed `33554432` <!-- drift:fact max_pixels = 33554432 -->, or file size exceeds `268435456` bytes <!-- drift:fact max_file_bytes = 268435456 -->.

## C API Argument and Option Diagnostics (`src/api.c`)

- `output is required`, `pixels are required`, `input is required`, `info is required`, `invalid pixel stride or buffer size` (code `1` <!-- drift:fact exit_usage = 1 -->): verify that all required pointers are non-null and that `pixels->stride >= width * 3` and `pixels->size >= (height - 1) * stride + width * 3`.
- `options: size too small`, `options: invalid size alignment`, `options: threads must be 0..9` (code `1` <!-- drift:fact exit_usage = 1 -->): initialize `rf_options options = { sizeof options, threads, {0} };` where `threads` is `0` (auto) or `1..9`.
- `options: size not supported`, `options: reserved field`, `unsupported pixel layout` (code `3` <!-- drift:fact exit_unsupported = 3 -->): zero-initialize `options.reserved` and set `pixels->layout = RF_LAYOUT_RGB8`.

## Toolchain and Build Recovery

If a build or gate fails because a pinned tool under `tools/` is missing or corrupted:

### Linux or WSL x64

```bash
# Verify the pinned Linux toolchain and Python .venv without modifying files
python3 tools/provision.py --verify

# Re-provision missing archives from toolchain.json and rebuild all targets
python3 tools/provision.py
./build.sh --target all
```

### Windows x64 (PowerShell)

```powershell
# Verify or re-provision the pinned Windows toolchain
powershell -NoProfile -ExecutionPolicy Bypass -File tools/provision.ps1 -Verify
powershell -NoProfile -ExecutionPolicy Bypass -File tools/provision.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

See [Installation and Build](installation.md) for host prerequisites and [Security Policy](security.md) to report a suspected vulnerability or crash on malformed input.
