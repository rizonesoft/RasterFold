---
schema_version: 1
id: dll-and-api
domain: 06-library
status: active
title: "TODO-01 -- rasterfold.dll and the C API"
depends_on: []
frozen: true
track: L1
---

# TODO-01 -- rasterfold.dll and the C API

> **Goal:** Other programs can encode, decode, and inspect `.fold` files through Windows/Linux/Android shared or static libraries and one header, `rasterfold.h`, with a versioned, documented C API that never allocates behind the caller's back without a way to free it, never touches the file system unless asked, and gives byte-identical results to the command line.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** The migration implementation adds a public C API, per-call errors, concurrent table initialization, CLI integration and shared/static builds for Windows, Linux and both requested Android ABIs. `tests/test_api.py` exercises native calls and `tests/test_artifacts.py` checks exports and C/C++ consumers. This implementation is committed, but has no verified section stamp; target-specific runtime evidence is recorded in `docs/wsl-migration-audit.md`.
<!-- claim: exists include/rasterfold.h -->
<!-- claim: count "g_err_msg" src/rf.h = 0 -->

## Inputs

- [`src/rf.h`](../../src/rf.h), [`src/fold.c`](../../src/fold.c), [`src/sys.c`](../../src/sys.c) -- the internal interface the API wraps
- [`scripts/build.py`](../../scripts/build.py) -- the build §2 extends with the DLL and import libraries
- -> XREF: D07 T01 §1 -- the command line, which §3 rebuilds on the library
- -> XREF: D08 T03 §2 -- the API reference generated from the header
- -> XREF: D09 T01 §2 -- the release package that ships the DLL, header, and import libraries

- -> XREF: D00 T04 §1 -- local Windows, Linux and Android toolchains.

- -> XREF: D10 T01 §1 -- shared implementation or verification boundary.

## Outcome

- `include/rasterfold.h` declares the API with its version macros; `bin/windows-x64/bin/rasterfold.dll` exports exactly the functions listed in `src/rasterfold.def`, importing only `KERNEL32.dll`.
- A C program and a Python `ctypes` test encode and decode through the DLL with byte-identical results to `rasterfold.exe`.

**Adjacency:** list=not-applicable (no records to browse); document=not-applicable (no printed output); settings=applicable @ D06 T01 §1; reporting=applicable @ D06 T01 §1; notifications=applicable @ D06 T01 §1; permissions=not-applicable (the library writes no file unless the caller asks, and then through the same atomic path); audit=not-applicable (git history is the audit); exchange=applicable @ D06 T01 §2; reverse=applicable @ D06 T01 §4

**Adjacency rationale:** Settings are the options structure (thread count, later effort); reporting is `rf_inspect`; notifications are the error codes and messages the caller receives; exchange is the exported interface; reverse is the decode that undoes every encode, proven in the API tests.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | The public header | -- |  [x]   |
|   2   |   §2    | Shared/static libraries and Windows import libraries | §1 |  [ ]   |
|   3   |   §3    | The command line on the library | §2 |  [ ]   |
|   4   |   §4    | API tests and the ABI list | §2 |  [ ]   |
|   5   |   §5    | Example programs | §4 |  [ ]   |

---

## 1. The Public Header

> **Started:** 2026-11-09T01:59:26Z

**Job:** A developer can choose the settings (the thread count) through an options structure, read a file's inspect report, and receive a notification of every failure as an error code with a message.

The API is a contract that outlives releases, so it is designed before code: plain C, no global state, caller-visible ownership, and a version check.

- [x] Write `include/rasterfold.h`: `RF_API_VERSION` macros and `rf_version()`; an options structure with a size field for forward compatibility (`threads`, reserved fields); `rf_encode_image(const rf_pixels *, const rf_options *, rf_buffer *out, rf_error *err)`, `rf_decode_image(const void *data, size_t size, const rf_options *, rf_pixels *out, rf_error *err)`, `rf_inspect(const void *, size_t, rf_info *, rf_error *)`, and `rf_free(void *)` for everything the library allocates; `rf_pixels` naming width, height, stride, and layout (RGB8 for 1.0). Done when: the header compiles as C11 and C++ with `-Wall -Wextra -Werror`. Cheaper substitute: exporting the internal functions with their globals.
- [x] Document every function, parameter, ownership rule, and error code in the header's comments in the form `D08 T03 §2` generates the reference from. Done when: every declaration carries its comment.
- [x] Commit: `"api: document rasterfold.h contracts and aliasing rules"`

**Test checkpoint:** `clang -std=c11 -Wall -Wextra -Werror -fsyntax-only include/rasterfold.h` and `clang++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only -x c++ include/rasterfold.h` exit 0; separately, the freeze check below is the fidelity evidence that no encoded byte changed.

**Freeze check:** the header adds an interface and changes no `.fold` byte: `KAT_FOLD` and every oracle comparison are unchanged.

> **Verified:** 2026-11-09 | §1 | 105d86eada94be9e459ec83009433737202cc35e
> **Review:** `docs/reviews/d06-t01-s1.json`
## 2. The DLL and Its Import Libraries

> **Started:** 2026-11-09T02:10:26Z

- [x] Add Windows DLL/static and Linux/Android shared/static targets through `scripts/build.py` (same objects without `main.c`, Windows entry `DllMain`-free with no C runtime, exports from `src/rasterfold.def`), plus a MinGW import library (`librasterfold.dll.a`) and an MSVC one (`rasterfold.lib`, from `llvm-dlltool`). Done when: `tools/llvm-mingw/bin/llvm-objdump.exe -p bin/windows-x64/bin/rasterfold.dll` lists exactly the `.def` exports and imports only `KERNEL32.dll`. Cheaper substitute: a DLL that exports every internal symbol.
- [x] Replace the error globals with the `rf_error` the caller passes (the executable keeps printing it). Done when: two threads of one process can encode concurrently with separate errors, shown by a test that runs concurrent encode calls with distinct expected failures beside a successful encode and checks each caller's retained code and message after all finish.
- [x] Commit: `"api: verify rasterfold.dll import libraries and error isolation"`

**Test checkpoint:** `tools/llvm-mingw-linux/bin/llvm-objdump -p bin/windows-x64/bin/rasterfold.dll` lists exactly the five `src/rasterfold.def` exports (`rf_version`, `rf_encode_image`, `rf_decode_image`, `rf_inspect`, `rf_free`) and imports only `KERNEL32.dll`. The C consumer evidence (scratch sources `build/consumer.c` and `build/consumer_entry.c`, the entry TU renaming `main` to `rf_consumer_main` and calling `ExitProcess`), run from the repository root: `tools/llvm-mingw-linux/bin/clang --target=x86_64-w64-mingw32 -Iinclude build/consumer.c -Lbin/windows-x64/lib -lrasterfold -o build/consumer/consumer_mingw.exe`; `tools/llvm-mingw-linux/bin/clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -Dmain=rf_consumer_main -Iinclude -c build/consumer.c -o build/consumer/consumer.obj`; `tools/llvm-mingw-linux/bin/clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -c build/consumer_entry.c -o build/consumer/entry.obj`; `tools/llvm-mingw-linux/bin/lld-link build/consumer/consumer.obj build/consumer/entry.obj bin/windows-x64/lib/rasterfold.lib tools/llvm-mingw-linux/x86_64-w64-mingw32/lib/libkernel32.a /out:build/consumer/consumer_msvc.exe /entry:mainCRTStartup /subsystem:console`; `python3 scripts/windows.py --exe build/consumer/consumer_mingw.exe` and `python3 scripts/windows.py --exe build/consumer/consumer_msvc.exe`, each exit code 0. `python3 tests/test_api.py` passes 10 tests including `test_concurrent_encode_error_isolation` (2026-11-09).

**Freeze check:** `KAT_FOLD`, `KAT_FOLD3`, and every oracle comparison are unchanged, proven by `python3 tests/test_correctness.py` on the candidate.

## 3. The Command Line on the Library

The executable uses the same code path as the library (linked statically, so `rasterfold.exe` stays one file), which guarantees the two cannot drift apart.

- [ ] Rebuild `src/main.c` on the public API calls. Done when: `python tests/test_correctness.py` passes unchanged. Cheaper substitute: two code paths kept in step by hand.
- [ ] Commit: `"cli: build the command line on the public API"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case; `KAT_FOLD` unchanged.

## 4. API Tests and the ABI List

**Job:** A developer can rely on every decode through the DLL reversing its encode, byte for byte.

- [ ] Add `tests/test_api.py`, which drives the DLL through `ctypes`: round trips byte-identical to `rasterfold.exe` on every correctness image, every error code reachable, options with an older `size` accepted. Done when: `python tests/test_api.py` passes and `scripts/check-all.ps1` runs it.
- [ ] Freeze `src/rasterfold.def` as the ABI list and add a check that fails when an export disappears or changes ordinal. Done when: removing an export from a build makes the check fail.
- [ ] Inspect exported symbol/version sets for Windows, Linux and both Android ABIs in `tests/test_artifacts.py`. Done when: a leaked internal symbol or missing public API fails on any target, with no Windows-only ABI assumption.
- [ ] Commit: `"test: API round trips and the frozen export list"`

**Test checkpoint:** `python tests/test_api.py` output quoted; the ABI check's failure on a removed export quoted, then restored.

## 5. Example Programs

- [ ] Add `examples/encode.c` and `examples/decode.c` (PPM in and out, error handling shown), built and run by `tests/test_artifacts.py` against native shared and static libraries. Done when: both build and round-trip an image locally and in the configured native CI jobs; remote CI proof is recorded separately. Cheaper substitute: code snippets in the docs that nothing compiles.
- [ ] Commit: `"api: example programs built in CI"`

**Test checkpoint:** `python3 tests/test_artifacts.py` proves native examples; the `build` workflow runs the same commands, with remote results reported only after observation.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0 with the API tests
- [ ] `python scripts/todo-graph.py validate` clean
