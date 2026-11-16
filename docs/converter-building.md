# Building the Windows Converter Bundle

The native image converter targets Windows x64 (`windows-x64`) only and keeps the core freestanding RasterFold CLI, library targets (`scripts/build.py`), and `.fold` format untouched.

## OS Prerequisites vs Bundled Libraries vs Build Tools

| Category | Components | Shipped in Converter | Notes |
| --- | --- | --- | --- |
| Windows OS prerequisites | System DLLs (`kernel32.dll`, `user32.dll`, `advapi32.dll`, `shell32.dll`, `ole32.dll`, `oleaut32.dll`, `bcrypt.dll`, UCRT), Windows Imaging Component (`WindowsCodecs`) | System-provided | HEIF and HEIC import uses the Windows OS WIC codec (`docs/codec-distribution.md`); zero HEVC code or binaries are bundled. |
| Bundled static libraries | All 19 pinned libraries in `converter/vendor.json` (`zlib`, `libdeflate`, `libpng` + `libpng-apng`, `libwebp`, `highway`, `brotli`, `lcms2`, `libjxl`, `libaom`, `libjpeg-turbo`, `libavif`, `libtiff`, `giflib`, `qoi`, `openjpeg`, `imath`, `openjph`, `openexr`, `libraw`) + `librasterfold.a` + static `llvm-mingw` runtimes (`libc++`, `libc++abi`, `libunwind`, `compiler-rt`, `mingw-w64` CRT, `winpthreads`) | Yes (statically linked) | Compiled as static archives against MinGW-w64 UCRT with `-static` C++ and pthread runtimes so deployed binaries require zero unshipped DLLs; all 19 library licenses (including `README.ijg` inside `libjpeg-turbo.txt`) and the 3 toolchain runtime license files (`llvm-runtime.txt`, `mingw-w64-runtime.txt`, `winpthreads.txt`) are tracked in `converter/vendor.json`. |
| Converter build-time tools | `cmake` (`4.5.0`), `nasm` (`3.02`), plus core `llvm-mingw` (`23.1.2`) and `ninja` (`1.13.2`) | Never shipped | `cmake` and `nasm` carry `"scope": "converter"` in `toolchain.json` so core-only `tools/bootstrap.*` and `tools/provision.*` runs stay fast and isolated. |

## Why Meson Is Omitted

Meson is not pinned because every direct and transitive converter dependency builds cleanly with pinned CMake `4.5.0` and Ninja `1.13.2`.

Four upstream packages without native upstream CMake support (`lcms2`, `giflib`, `qoi`, and `libraw`) use small static-library `CMakeLists.txt` wrappers in `converter/cmake/`.

## Staging and Building

1. Stage converter build tools and verify all 19 pinned upstream source archives, patches, and `PINNED.json` tree hashes:
   - Linux/WSL host: `python3 tools/bootstrap.py --converter` and `python3 tools/provision.py --converter --verify`
   - Windows host: `powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1 -Converter` and `powershell -ExecutionPolicy Bypass -File tools\provision.ps1 -Converter -Verify`
2. Build all 19 static archives into `build/converter/windows-x64/deps-prefix`, generate `build/converter/windows-x64/generated/rfc_build_provenance.h` via `scripts/gen_converter_provenance.py` (`VERSION`, `bin/<target>/build.json`, `build/<target>/compile_commands.json`, `converter/vendor.json`, `toolchain.json`, and `deps-build/*/CMakeCache.txt` with `-DRFC_HAVE_GENERATED_PROVENANCE=1`), and link `bin/converter/windows-x64/rfc_bundle_smoke.exe` and converter test executables:
   - `python3 scripts/build-converter.py --target windows-x64`
3. Verify bundled PE imports, registry coverage, offline notices/SBOM, and clean-directory restricted-`PATH` Windows execution:
   - `python3 scripts/check-converter.py --case bundle`

## Language Boundary and Link Model

The converter engine (`rfc_engine` in `converter/src/registry.c`) and bundle smoke harness (`converter/src/bundle_smoke.c`) compile strictly with `-std=c11 -Wall -Wextra -Werror`.

Vendored C++ dependencies (`highway`, `libjxl`, `libaom`, `imath`, `openjph`, `openexr`, `libraw`) stay isolated behind C APIs while linking statically with `libc++`, `libunwind`, and `winpthread`, leaving `CMAKE_CXX_STANDARD 23` reserved for the Windows desktop interface target.

## SIMD, Assembly, and Threading Settings

For fair performance comparisons, every dependency keeps its full standard x86-64 acceleration enabled within the `x86-64-v2` baseline:
- `libjpeg-turbo` (`-DWITH_SIMD=ON -DREQUIRE_SIMD=ON -DWITH_SYSTEM_ZLIB=ON`) and `libaom` (`-DAOM_TARGET_CPU=x86_64`) compile their x86-64 assembly kernels with pinned NASM `3.02`, reusing the pinned static `zlib` `1.4` instead of vendored duplicate zlib objects.
- `highway`, `libjxl`, `libwebp`, `libdeflate`, and `openjph` compile runtime CPUID multi-target SIMD paths (SSE2 through AVX-512 where supported at runtime).
- `libpng` enables `-DPNG_HARDWARE_OPTIMIZATIONS=ON` (SSE2), with the official `libpng-1.6.59-apng.patch.gz` (`-p1`) verified before staging.
- `libraw` builds with `-DLIBRAW_NODLL` and without OpenMP so converter worker pools retain full thread-budget control.

## Notices and SBOM Regeneration

`converter/THIRD-PARTY-NOTICES.txt` and `converter/sbom.spdx.json` are generated deterministically offline from `converter/vendor.json`, `toolchain.json`, and `converter/licenses/*.txt` (covering all 19 codec libraries including `README.ijg` in `libjpeg-turbo.txt`, the 7 compiled embedded subcomponents such as OpenEXR's internal Zstandard `1.5.7` and xxHash `0.8.2`, `libaom`'s `fastfeat`/`vector`/`x86inc`, `libavif`'s `libyuv` subset, and `libjpeg-turbo`'s `libspng`, plus the 3 static `llvm-mingw` runtime entries `llvm-runtime.txt`, `mingw-w64-runtime.txt`, and `winpthreads.txt`):
- Regenerate: `python3 scripts/build-converter.py --write-notices`
- Verify currency: `python3 scripts/build-converter.py --check-notices`
