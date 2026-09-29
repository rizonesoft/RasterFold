---
schema_version: 1
id: compiler-options
domain: 02-speed
status: draft
title: "TODO-01 -- Compiler Options: PGO, LTO, AVX2"
depends_on: []
track: S1
---

# TODO-01 -- Compiler Options: PGO, LTO, AVX2

> **Goal:** The release build uses every compiler option that makes it measurably faster without changing a byte it writes: profile-guided optimization from a reproducible profile, link-time optimization if it helps, and an AVX2 path chosen at run time only if building for AVX2 is worth at least 5%.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** `build.ninja` compiles with `-O2 -march=x86-64-v2 -mtune=generic -Werror` and links with `-nostdlib` against `kernel32` and `shell32` only; there is no profile, no `-flto`, and no instruction-set dispatch. Decode of `measure/image.bmp` takes 0.58 of JPEG XL's time with three threads and 0.80 with one thread each (`benchmarks/results.md`, 2026-09-29). The llvm-mingw toolchain carries `llvm-profdata.exe` and the Clang profile runtime.
<!-- claim: count "-march=x86-64-v2" build.ninja = 1 -->
<!-- claim: count "flto" build.ninja = 0 -->
<!-- claim: count "fprofile" build.ninja = 0 -->
<!-- claim: count "llvm-mingw-20260922-ucrt-x86_64.zip" toolchain.json = 1 -->

## Inputs

- [`build.ninja`](../../build.ninja) -- the flags every section here changes
- [`docs/research.md`](../../docs/research.md) -- sections 5.1, 5.2, and 5.6, the procedures
- [`research/vsjxl.py`](../../research/vsjxl.py) -- process wall times against JPEG XL, the measurement for all three sections

## Outcome

- `Bin/rasterfold.exe` is built with a committed or regenerated profile, and decodes `measure/image.bmp` at least 5% faster on one thread than before this file, or the file records that profile-guided optimization did not reach 5%.
- The executable still imports only `KERNEL32.dll` and `SHELL32.dll`, and `KAT_FOLD` is unchanged.

**Adjacency:** all=not-applicable (build flags; no byte, option, or output changes)

**Adjacency rationale:** Every section proves the bytes did not move; none adds an option or an output.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Profile-guided optimization                   | --         |  [ ]   |
|   2   |   §2    | Link-time optimization                        | §1         |  [ ]   |
|   3   |   §3    | An AVX2 build, measured before dispatching    | §2         |  [ ]   |

---

## 1. Profile-Guided Optimization

**Needs:** Idle machine (timing)

The hot paths (`code_bit`, `code_residual`, `cm_model`) are branchy; a profile tells Clang which way the branches go. The instrumented build needs the profile runtime and a C runtime, which the release build does not have, so it is a separate target. Bar: a one-thread decode at least 5% faster (`research/vsjxl.py`), bytes identical; otherwise the negative result ships and nothing changes in the release build.

- [ ] Add a `pgo-gen` target to `build.ninja` that compiles with `-fprofile-instr-generate` and links `Bin\rasterfold-pgo.exe` with the normal C runtime entry and `tools\llvm-mingw\lib\clang\23\lib\windows\libclang_rt.profile-x86_64.a`. Done when: `tools\ninja\ninja.exe pgo-gen` builds it.
- [ ] Add `scripts/pgo.ps1`, which runs the instrumented build over `measure/image.bmp` and the ten test-pack photos, encode and decode, `--threads 1` and default, with `LLVM_PROFILE_FILE=build\pgo\%p.profraw`, then merges them with `llvm-profdata.exe merge -o build\pgo\rasterfold.profdata`. Done when: the merged profile exists. Cheaper substitute: profiling one image, which fits the profile to it.
- [ ] Build the release with `-fprofile-instr-use=build\pgo\rasterfold.profdata` when the profile exists, and make `build.ps1` regenerate it when it is missing, so a clean checkout reproduces the build. Done when: a clean `build.ps1` run produces the optimized binary.
- [ ] Measure with alternating runs of the old and new executable (`research/vsjxl.py -n5` for each, twice, alternating). Done when: the decode and encode ratios at one and three threads are quoted.
- [ ] Commit: `"build: profile-guided optimization from a reproducible profile"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case (the bytes did not move); `llvm-objdump -p Bin\rasterfold.exe` lists only `KERNEL32.dll` and `SHELL32.dll`; the alternating timings are quoted against the 5% bar.

## 2. Link-Time Optimization

**Needs:** Idle machine (timing)

The codec is small, but `cm.c`'s entry points are called through `fold.c`, and LTO can inline across that seam. Bar: any measurable gain (a minimum-time ratio below 0.97) with the imports unchanged.

- [ ] Add `-flto` to `cflags` and `ldflags` in `build.ninja` (with the §1 profile when it shipped). Done when: `build.ps1` exits 0 and `llvm-objdump -p Bin\rasterfold.exe` lists only `KERNEL32.dll` and `SHELL32.dll`. Cheaper substitute: trusting LTO without checking that it did not pull in a runtime call.
- [ ] Measure with alternating runs as in §1. Done when: the ratios are quoted; below the bar, `-flto` is removed again and the result recorded in `docs/research.md` section 3.
- [ ] Commit: `"build: link-time optimization"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case; the imports listing and the alternating timings are quoted.

## 3. An AVX2 Build, Measured before Dispatching

**Needs:** Idle machine (timing)

Building for `x86-64-v3` might speed the LMS stage and the quantizers. Dispatch costs a second copy of `cm.c` and a CPUID check, so it is built only if the whole-codec `-march=x86-64-v3` build is at least 5% faster.

- [ ] Build the codec once with `-march=x86-64-v3` into a scratch executable and measure it against the release with alternating runs. Done when: the ratios are quoted. Below 5%, record the result in `docs/research.md` section 3, strike the next item with that reason, and ship the measurement.
- [ ] Compile `src/cm.c` a second time with `-march=x86-64-v3` and renamed entry points, check CPUID and `XGETBV` in `src/main.c`, and call the v3 entry points when the CPU and the operating system support AVX2. Done when: the output bytes are identical to the v2 path's on every correctness case, and `--threads 1` decode is at least 5% faster on an AVX2 machine.
- [ ] Commit: `"build: measure an AVX2 build and dispatch to it when it pays"`

**Test checkpoint:** `python tests/test_correctness.py` passes; the scratch build's alternating timings are quoted against the 5% bar.

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0
- [ ] `python tests/benchmark.py -n 3` rerun, `benchmarks/results.md` and the README measurement table updated with the new times
- [ ] `python scripts/todo-graph.py validate` clean
