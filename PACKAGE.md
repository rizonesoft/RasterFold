# RasterFold release package

This directory is one self-contained RasterFold release for one platform: the command-line tool, the shared and static libraries with their header, the documentation, and checksums. Nothing needs to be installed or built. The commands below are run from this directory.

## What is here

| Path | What it is |
| ---- | ---------- |
| `bin/` | The command-line tool: `rasterfold` on Linux, `rasterfold.exe` and `rasterfold.dll` on Windows |
| `lib/` | The libraries: `librasterfold.so`, `librasterfold.so.1` and `librasterfold.a` on Linux; `rasterfold.lib`, `librasterfold.dll.a` and `librasterfold.a` on Windows |
| `include/rasterfold.h` | The C API, five functions, documented in the header itself |
| `symbols/` | Debug symbols (Windows packages only) |
| `docs/` | The documentation, readable offline; start at [docs/index.md](docs/index.md) |
| `build.json` | What built this package: compiler, source revision, and the SHA-256 of every built file |
| `LICENSE`, `CHANGELOG.md` | The licence ([LICENSE](LICENSE)) and what changed ([CHANGELOG.md](CHANGELOG.md)) |
| `SHA256SUMS` | The SHA-256 of every other file in this directory |

## Verify the package

Linux:

```bash
sha256sum -c SHA256SUMS
```

Every line ends in `OK`.

Windows (PowerShell):

```powershell
Get-Content SHA256SUMS | ForEach-Object { $hash, $file = $_ -split '  ', 2; if ((Get-FileHash $file -Algorithm SHA256).Hash.ToLower() -ne $hash) { "FAILED: $file" } }
```

It prints nothing when every file matches.

## First use

Use any image of your own: an 8-bit RGB PNG, a 24-bit BMP, or a P6 PPM. The examples call it `photo.png`.

Linux:

```bash
./bin/rasterfold version
./bin/rasterfold encode photo.png photo.fold
./bin/rasterfold inspect photo.fold
./bin/rasterfold verify photo.fold
./bin/rasterfold decode photo.fold roundtrip.png
```

Windows (PowerShell or Command Prompt):

```powershell
.\bin\rasterfold.exe version
.\bin\rasterfold.exe encode photo.png photo.fold
.\bin\rasterfold.exe inspect photo.fold
.\bin\rasterfold.exe verify photo.fold
.\bin\rasterfold.exe decode photo.fold roundtrip.png
```

`roundtrip.png` holds exactly the pixels of `photo.png`. `encode` and `decode` refuse an existing output file unless `--overwrite` is given. `rasterfold help` lists the commands, and [docs/cli.md](docs/cli.md) is the full command-line reference; [docs/troubleshooting.md](docs/troubleshooting.md) explains every exit code and message.

## Use the library

A smallest program, saved as `app.c` in this directory:

```c
#include <stdio.h>
#include "rasterfold.h"

int main(void) {
    printf("RasterFold API %#x\n", (unsigned)rf_version());
    return 0;
}
```

Linux, linking the static library (the codec is archived into your program; pthreads is the only extra):

```bash
cc -Iinclude app.c lib/librasterfold.a -pthread -o app
./app
```

Linux, linking the shared library:

```bash
cc -Iinclude app.c lib/librasterfold.so -Wl,-rpath,'$ORIGIN/lib' -o app
./app
```

Windows with MinGW or Clang, importing the DLL (keep `rasterfold.dll` beside your program or on its DLL search path):

```powershell
clang -DRF_SHARED -Iinclude app.c lib/librasterfold.dll.a -o app.exe
Copy-Item bin\rasterfold.dll .
.\app.exe
```

Windows with an MSVC-compatible compiler, importing the DLL:

```powershell
cl /DRF_SHARED /Iinclude app.c lib\rasterfold.lib
```

Both print `RasterFold API 0x10000`. [docs/c-api.md](docs/c-api.md) is the guide to buffers, ownership and threads, [docs/api.md](docs/api.md) is the reference for every function and type, and [docs/examples.md](docs/examples.md) has complete encode and decode programs.

## The file format

`.fold` files are specified in [docs/format.md](docs/format.md). An existing output file is never replaced unless you pass `--overwrite`, and no output is left half-written: it goes to a `.partial` file first and is published only when complete.
