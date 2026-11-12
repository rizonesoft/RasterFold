---
layout: default
title: Getting Started
nav_order: 2
source_path: docs/getting-started.md
---

# Getting Started

RasterFold builds from source with a repository-local pinned toolchain (Clang/LLVM, LLD and Ninja) defined in `toolchain.json`. No global compiler installation is required on Windows or Linux x64.

This guide describes the following versions. Published releases, when there are any, are on the [Downloads](downloads.md) page.

{% include generated/version-labels.md %}

## 1. Provision and Build

### Linux / WSL x64

```bash
python3 tools/provision.py
source .venv/bin/activate
./build.sh --target all
python3 scripts/check-all.py --all-targets
```

### Windows x64 (PowerShell 7)

```powershell
pwsh tools/provision.ps1
powershell -ExecutionPolicy Bypass -File build.ps1
pwsh scripts/check-all.ps1
```

Build outputs are written to `bin/<target>/`:

- `bin/<target>/bin/`: executable (`rasterfold` or `rasterfold.exe`) and Windows shared library (`rasterfold.dll`)
- `bin/<target>/lib/`: shared/static libraries (`librasterfold.so`, `librasterfold.a`, `rasterfold.lib`, `librasterfold.dll.a`)
- `bin/<target>/include/rasterfold.h`: public C11/C++ header
- `bin/<target>/symbols/`: detached debug symbols (`rasterfold.pdb` on Windows)

## 2. Encode, Inspect, and Decode an Image

Encode a 24-bit uncompressed BMP to `.fold` <!-- drift:fact encoded_ext = .fold -->, validate its header and payload CRCs with `inspect` <!-- drift:fact cmd_inspect = inspect -->, and decode back to `.bmp` <!-- drift:fact input_image_ext = .bmp -->:

```bash
bin/linux-x64/bin/rasterfold encode --time measure/image.bmp build/sample.fold
bin/linux-x64/bin/rasterfold inspect build/sample.fold
bin/linux-x64/bin/rasterfold decode --time build/sample.fold build/restored.bmp
```

On Windows x64, use `bin\windows-x64\bin\rasterfold.exe`. By default the CLI refuses an existing destination file: it writes to an exclusive `.partial` sibling first, flushes it, and publishes atomically without replacement. `--overwrite` on `encode` or `decode` replaces an existing destination instead, through the same `.partial` file. The [Command-Line Reference](cli.md) lists every command and option.

## 3. Exit Codes

Every CLI invocation exits with one of the following codes from `include/rasterfold.h` and `src/main.c`:

| Exit code | Meaning | Constant |
| ---: | --- | --- |
| `0` <!-- drift:fact exit_ok = 0 --> | `ok` <!-- drift:fact exit_ok_meaning = ok --> | `RF_OK` |
| `1` <!-- drift:fact exit_usage = 1 --> | `usage` <!-- drift:fact exit_usage_meaning = usage --> | `RF_ERR_USAGE` |
| `2` <!-- drift:fact exit_io = 2 --> | `io` <!-- drift:fact exit_io_meaning = io --> | `RF_ERR_IO` |
| `3` <!-- drift:fact exit_unsupported = 3 --> | `unsupported` <!-- drift:fact exit_unsupported_meaning = unsupported --> | `RF_ERR_UNSUPPORTED` |
| `4` <!-- drift:fact exit_malformed = 4 --> | `malformed` <!-- drift:fact exit_malformed_meaning = malformed --> | `RF_ERR_FORMAT` |
| `5` <!-- drift:fact exit_limit = 5 --> | `limit` <!-- drift:fact exit_limit_meaning = limit --> | `RF_ERR_LIMIT` |

Continue to [CLI and Library Usage](usage.md) or the [Format Specification](format.md).
