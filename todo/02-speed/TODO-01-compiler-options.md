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
> **Current state (verified 2026-10-25; baseline timings remain 2026-09-29):** `scripts/build.py` compiles with `-O2 -march=x86-64-v2 -mtune=generic -Werror` and links its Windows executable with `-nostdlib` against `kernel32` and `shell32` only; POSIX targets use libc/pthreads; `--pgo-gen` and `--pgo-use` (`-fprofile-instr-generate` / `-fprofile-instr-use`) are wired as opt-in targets (`D02 T01 §1` measured < 5% one-thread decode speedup, so default release builds stay unprofiled); there is no `-flto` and no instruction-set dispatch yet. Decode of `measure/image.bmp` takes 0.58 of JPEG XL's time with three threads and 0.80 with one thread each (`benchmarks/results.md`, 2026-09-29). The llvm-mingw toolchain carries `llvm-profdata.exe` and the Clang profile runtime.
<!-- claim: count "-march=x86-64-v2" scripts/build.py = 1 -->
<!-- claim: count "flto" scripts/build.py = 0 -->
<!-- claim: count "fprofile" scripts/build.py = 4 -->
<!-- claim: count "llvm-mingw-20260922-ucrt-x86_64.zip" toolchain.json = 1 -->

## Inputs

- [`scripts/build.py`](../../scripts/build.py) -- the flags every section here changes
- [`docs/research.md`](../../docs/research.md) -- sections 5.1, 5.2, and 5.6, the procedures
- [`research/vsjxl.py`](../../research/vsjxl.py) -- process wall times against JPEG XL, the measurement for all three sections

## Outcome

- `bin/windows-x64/bin/rasterfold.exe` is built with a committed or regenerated profile, and decodes `measure/image.bmp` at least 5% faster on one thread than before this file, or the file records that profile-guided optimization did not reach 5%.
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

> **Started:** 2026-10-25T17:31:30Z

**Needs:** Idle machine (timing)

The hot paths (`code_bit`, `code_residual`, `cm_model`) are branchy; a profile tells Clang which way the branches go. The instrumented build needs the profile runtime and a C runtime, which the release build does not have, so it is a separate target. Bar: a one-thread decode at least 5% faster (`research/vsjxl.py`), bytes identical; otherwise the negative result ships and nothing changes in the release build.

- [x] Add a `pgo-gen` target to `scripts/build.py` (`--pgo-gen`) and root `build.ninja` that compiles with `-fprofile-instr-generate -DRF_PGO_GEN` (with a hosted Windows `int main(void)` in `src/main.c` under `#ifdef RF_PGO_GEN` that preserves `CommandLineToArgvW` parsing and returns `rc` instead of calling `ExitProcess` so the profile runtime flushes on exit) and links `bin/windows-x64-pgo/bin/rasterfold.exe` with the normal C runtime entry and `libclang_rt.profile-x86_64.a` (`tools/llvm-mingw` on Windows, `tools/llvm-mingw-linux` on Linux/WSL). Done when: `tools\ninja\ninja.exe pgo-gen` (and `python3 scripts/build.py --pgo-gen`) builds `bin/windows-x64-pgo/bin/rasterfold.exe` and running it emits a non-empty `.profraw` file.
- [x] Add `scripts/pgo.ps1` and `scripts/pgo.py`, which clear stale raw profiles in an isolated staging directory, run the instrumented build (`encode` and `decode`, `--threads 1` and `--threads 3`) over the `photo` tuning set (or deterministic non-held-out training BMPs when `build/research/ppm` is absent, keeping held-out `tp` and `measure/image.bmp` out of training per `docs/research.md` section 2.1), verify every command exit code and non-empty `.profraw` emission with `LLVM_PROFILE_FILE`, merge them into a staged profile with `llvm-profdata merge`, validate with `llvm-profdata show`, and atomically publish `build/pgo/rasterfold.profdata` alongside provenance metadata `build/pgo/rasterfold.profdata.json`. Done when: the merged profile and provenance JSON exist and unit/integration checks cover repeated generation, failed training, and corrupt-profile refusal. Cheaper substitute: profiling one image or training on the held-out `tp` confirmation set.
- [x] Wire `--pgo-use` (and automatic profile consumption when enabled) in `scripts/build.py` across `build.ps1` and `build.sh`, adding the profile file as an implicit Ninja dependency of compiled objects so profile updates rebuild objects, and verifying that all four targets (`windows-x64`, `linux-x64`, `android-arm64`, `android-x64`) and all artifacts (CLI, DLL/SO, static library) build cleanly. Done when: `build.ps1` and `scripts/build.py` produce the profile-optimized binary and rebuild objects when the profile changes.
- [x] Extend `research/vsjxl.py` with `--exe`, `--alt-exe`, and `--threads` options and measure alternating runs of the baseline and PGO executables (`research/vsjxl.py -n5` twice, alternating, at `--threads 1` and `--threads 3` on `measure/image.bmp`, plus corpus timing across `photo`, held-out `tp`, and `synth` with geometric means and worst-image reporting). Done when: the one-thread and three-thread decode and encode ratios (minimum and median) are quoted against the 5% one-thread decode bar (`ratio <= 0.950`); if the gain is below 5%, keep release builds unprofiled by default (leaving `--pgo-gen`, `scripts/pgo.ps1`, `scripts/pgo.py`, and `--pgo-use` wired and tested), verify baseline release behavior, and record the negative result in `docs/research.md` section 3.4.
- [x] Commit: `"build: profile-guided optimization from a reproducible profile"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case (`36 passed, 0 failed`, the bytes did not move); `llvm-objdump -p bin\windows-x64\bin\rasterfold.exe` lists only `DLL Name: KERNEL32.dll` and `DLL Name: SHELL32.dll`; alternating `research/vsjxl.py -n5` runs on `measure/image.bmp` give `--threads 1` decode ratio `1.079` min (`573.8 / 531.6 ms`) / `1.034` median (`593.2 / 573.6 ms`) and encode ratio `0.962` min (`552.5 / 574.1 ms`) / `0.978` median (`570.9 / 583.9 ms`) on pass 1, and `--threads 1` decode ratio `0.991` min (`502.6 / 507.0 ms`) / `1.005` median (`521.1 / 518.7 ms`) and encode ratio `0.988` min (`507.3 / 513.6 ms`) / `1.021` median (`549.2 / 537.7 ms`) on pass 2; at `--threads 3`, pass 1 gives decode ratio `1.020` min (`195.5 / 191.6 ms`) / `1.055` median (`212.7 / 201.7 ms`) and encode ratio `1.007` min (`191.0 / 189.7 ms`) / `1.214` median (`237.1 / 195.4 ms`), and pass 2 gives decode ratio `1.099` min (`222.2 / 202.1 ms`) / `1.047` median (`238.8 / 228.0 ms`) and encode ratio `1.075` min (`213.4 / 198.5 ms`) / `1.040` median (`259.5 / 249.5 ms`). Across the corpus (3 alternating passes per set), `photo` (`n=39`) gives `-t1` decode `0.999` (`10041.1 / 10054.7 ms`, geomean `1.000`, worst `ic_artificial` `1.150`) and encode `1.000` (`10369.5 / 10364.4 ms`, geomean `1.003`, worst `kodim12` `1.148`), `-t3` decode `1.046` (`5016.4 / 4794.1 ms`, geomean `0.971`, worst `ic_cathedral` `1.278`) and encode `1.057` (`4264.1 / 4033.7 ms`, geomean `0.986`, worst `ic_flower_foveon` `1.504`); held-out `tp` (`n=10`) gives `-t1` decode `0.988` (`2064.0 / 2089.2 ms`, geomean `0.980`, worst `tp_nightshot_iso_1600_crop` `1.054`) and encode `0.977` (`2059.8 / 2108.4 ms`, geomean `0.990`, worst `tp_spider_web_crop` `1.068`), `-t3` decode `0.924` (`680.5 / 736.3 ms`, geomean `0.973`, worst `kodim03` `1.057`) and encode `0.940` (`681.9 / 725.4 ms`, geomean `0.935`, worst `tp_spider_web_crop` `1.076`); `synth` (`n=11`) gives `-t1` decode `0.982` (`625.7 / 637.2 ms`, geomean `0.985`, worst `syn_text` `1.027`) and encode `0.967` (`627.0 / 648.4 ms`, geomean `0.992`, worst `syn_ui` `1.020`), `-t3` decode `1.012` (`247.9 / 244.9 ms`, geomean `1.090`, worst `syn_stripes` `1.360`) and encode `1.094` (`247.7 / 226.5 ms`, geomean `1.178`, worst `syn_checker` `1.700`). Because one-thread decode speedup is below the 5% bar (`ratio <= 0.950`), default release builds remain unprofiled and the negative result is recorded in `docs/research.md` section 3.4.

## 2. Link-Time Optimization

**Needs:** Idle machine (timing)

The codec is small, but `cm.c`'s entry points are called through `fold.c`, and LTO can inline across that seam. Bar: any measurable gain (a minimum-time ratio below 0.97) with the imports unchanged.

- [ ] Add `-flto` to `cflags` and `ldflags` in `scripts/build.py` (with the §1 profile when it shipped). Done when: `build.ps1` exits 0 and `llvm-objdump -p bin\windows-x64\bin\rasterfold.exe` lists only `KERNEL32.dll` and `SHELL32.dll`. Cheaper substitute: trusting LTO without checking that it did not pull in a runtime call.
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
