# Build and verify RasterFold

RasterFold builds a command-line codec, shared library and static library for Windows x64, Linux x64, Android ARM64 (`arm64-v8a`) and Android x64 (`x86_64`). x64 binaries require the x86-64-v2 instruction set. The format stays at version 2; the public C API starts at 1.0.

## Linux and WSL

Use an x86-64 Linux host. Install Python 3.12 or later with venv support, Git, native C development headers and libraries (Ubuntu's `build-essential`), and PowerShell 7 for the existing Claude Stop-hook tests. These host prerequisites are distinct from the downloaded compiler. The codec itself needs no Python or PowerShell at runtime.

```bash
python3 tools/provision.py
source .venv/bin/activate
./build.sh --target all
python3 scripts/check-all.py --all-targets
```

Provisioning installs checked archives under `tools/`, creates a local `.venv` for the pinned Pillow test dependency, and sets this clone's Git hook path when no custom path exists. It preserves a different existing hook path and reports it. `python3 tools/provision.py --verify` checks without installing or changing configuration.

For just one target, choose `linux-x64`, `windows-x64`, `android-arm64` or `android-x64`. With no target argument, the native host target is built. `--jobs N` sets build parallelism. `--portable` exercises the portable integer SIMD implementation on x64; it writes a separate `-portable` output directory and does not claim an ARM runtime test.

```bash
./build.sh --target linux-x64
./build.sh --target windows-x64
./build.sh --portable
RF_TEST_EXE="$PWD/bin/linux-x64-portable/bin/rasterfold" python3 tests/test_correctness.py
```

The Linux-host compiler, cross-compiler, linker, archiver and Ninja are pinned in `toolchain.json`. The native Linux build uses the host's libc development SDK and system runtime libraries. This is a repository-local compiler toolchain, not a hermetic Linux sysroot. Build release Linux artifacts on the oldest supported distribution and verify them on the intended systems; a WSL build alone does not establish a distribution support matrix.

## Windows

A native Windows x64 host builds the Windows artifacts with the pinned Windows LLVM package. Linux and Android cross-builds use Linux or WSL.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/provision.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
python scripts/check-all.py
```

The Windows codec remains freestanding. Its executable imports only `KERNEL32.dll` and `SHELL32.dll`; the DLL imports only `KERNEL32.dll`. The library's caller may use a normal C or C++ runtime. PowerShell 7 must also be installed to run the repository's current hook definitions; `powershell.exe` remains supported for the build/provision wrappers.

From WSL, `python3 scripts/windows.py tests/test_correctness.py` runs the Windows Python installation against the Windows codec. Windows Python and Pillow are separate from the Linux venv. WSL interop must be enabled. These are Windows runtime tests, not Linux execution of a PE binary.

The original process-memory benchmarks and sampled profiler remain Windows tools. Run them through the same bridge so their Win32 memory measurements keep their meaning. The Windows compiler and ffmpeg with libjxl must be installed for these commands. Native Linux codec tests are separate from this Windows research path.

```bash
python3 scripts/windows.py research/ab.py bench -r1 syn_odd -t1
python3 scripts/windows.py tests/benchmark.py -n 3
python3 scripts/windows.py tests/measure_final.py
```

The last two commands write their recorded benchmark results; run them on a quiet machine under the research protocol. The migration audit only smoke-tests the measurement helpers and one research harness input, without replacing historical comparison results. Codex lifecycle and Linux installer tests run only in the WSL/Linux full gate suite; the native Windows suite covers Windows tooling.

## Outputs and Android

Each `bin/<target>/` is a self-contained development package. The outer `bin/` is the repository's generated output root; the inner `bin/` is the package's runtime directory. Artifacts follow [CMake's conventional runtime, library and header destinations](https://cmake.org/cmake/help/latest/command/install.html). The compiler remains LLVM/Ninja; adopting this layout does not require CMake.

```text
bin/
  windows-x64/
    bin/rasterfold.exe
    bin/rasterfold.dll
    lib/librasterfold.a
    lib/librasterfold.dll.a
    lib/rasterfold.lib
    include/rasterfold.h
    symbols/rasterfold.pdb
    symbols/rasterfold-dll.pdb
    build.json
  linux-x64/
    bin/rasterfold
    lib/librasterfold.a
    lib/librasterfold.so -> librasterfold.so.1
    lib/librasterfold.so.1
    include/rasterfold.h
    build.json
  android-arm64/
  android-x64/
```

| Artifact | Purpose |
| --- | --- |
| `bin/rasterfold[.exe]` | Standalone command-line codec |
| `bin/rasterfold.dll` | Windows runtime library; deploy beside the application that uses it |
| `lib/librasterfold.a` | Static library linked into a C/C++ application |
| `lib/librasterfold.dll.a` | Windows DLL import library for MinGW/Clang |
| `lib/rasterfold.lib` | Windows DLL import library for an MSVC-compatible linker; this is not a static codec library |
| `lib/librasterfold.so[.1]` | Linux/Android shared runtime library; the versioned name is Linux-only |
| `include/rasterfold.h` | Public C/C++ interface |
| `symbols/*.pdb` | Matching Windows debugging information, unnecessary for normal execution |
| `build.json` | Compiler identity, target triple and SHA-256 hashes keyed by package-relative artifact paths |

Android uses the same `bin/`, `lib/` and `include/` structure, with an unversioned `librasterfold.so`. Linux and Android currently retain DWARF debugging information inside their ELF artifacts and static archives. The separate Windows `symbols/` directory is a project convention, not a universal install-directory standard. Generated Ninja graphs and object files stay in `build/<target>/`; there are no duplicated executables or libraries directly under the output root. Portable test variants use a separate `<target>-portable/` package.

Android uses the pinned NDK r30 and API 21 minimum. Its ELF segments use 16 KiB alignment. The ARM64 build uses the portable C arithmetic path; Android x64 uses the x86-64-v2 path. A successful cross-build and ELF inspection do not prove device behavior. Run the CLI and a library consumer on a device or emulator for each ABI before a release. No Android runtime claim is made by the WSL audit.

The Android CLI can be copied into an executable app-private or shell location such as `/data/local/tmp` for testing. Shared storage often forbids execution. Output publication uses no-replace rename, with an exclusive hard-link fallback on older kernels; unsupported filesystems are refused without replacing a destination. Library callers control their own file I/O. On filesystems without directory-fsync support, file data is flushed but persistence of the directory entry is best effort. Exit code 2 can also report a completed publication whose temporary cleanup or directory flush failed; the diagnostic says `output published`, and callers should inspect that destination instead of deleting or blindly retrying it.

## Pinning and packaging

Downloads are cached in ignored `tools/downloads/`. The bootstrap checks archive size and SHA-256 before extraction, checks installed probe hashes and tool versions, and refuses an incompatible existing installation. Review a manifest change as a dependency decision. To replace an old pin, remove only its corresponding ignored installation and rerun bootstrap; do not delete the whole checkout or system toolchain.

```bash
python3 tools/bootstrap.py --verify
python3 scripts/package.py --target all
```

Packages and SHA-256 sidecars land in `build/packages/`. They preserve the target's `bin/`, `lib/`, `include/` and optional `symbols/` structure beneath `rasterfold-<target>/`, alongside `build.json`, `LICENSE` and `docs/` notes. The packager verifies every artifact, including the copied header and PDBs, against the build manifest. They are development packages, not published releases. Publishing remains a separate release action.

The full gate runner retains logs under `build/logs/` and reports every failed command. `--quick` and `--skip-build` are iteration shortcuts, not full verification. Comparative benchmarks additionally require ffmpeg with libjxl and follow the quiet-machine protocol in [research.md](research.md).

<!-- drift:begin build-capabilities -->
The sources currently declare format 2, API 1.0, a 32768-pixel dimension limit and a 33554432-pixel image limit. See [the document drift policy](document-drift.md) for validation and repair.
<!-- drift:end build-capabilities -->
