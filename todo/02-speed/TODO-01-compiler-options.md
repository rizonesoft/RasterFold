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
> **Current state (verified 2026-10-25; baseline timings remain 2026-09-29):** `scripts/build.py` compiles with `-O2 -march=x86-64-v2 -mtune=generic -Werror` and links its Windows executable with `-nostdlib` against `kernel32` and `shell32` only; POSIX targets use libc/pthreads; `--pgo-gen`, `--pgo-use` (`-fprofile-instr-generate` / `-fprofile-instr-use`), and `--lto` (`-flto`) are wired as opt-in targets (`D02 T01 §1` and `D02 T01 §2` missed their corpus speedup bars, so default release builds stay unprofiled and non-LTO); there is no instruction-set dispatch yet. Decode of `measure/image.bmp` takes 0.58 of JPEG XL's time with three threads and 0.80 with one thread each (`benchmarks/results.md`, 2026-09-29). The llvm-mingw toolchain carries `llvm-profdata.exe` and the Clang profile runtime.
<!-- claim: count "-march=x86-64-v2" scripts/build.py = 1 -->
<!-- claim: count "flto" scripts/build.py = 3 -->
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
|   1   |   §1    | Profile-guided optimization                   | --         |  [x]   |
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
- [x] Extend `research/vsjxl.py` with `--exe`, `--alt-exe`, `--threads`, `--corpus`, and `--pgo-profile` options and measure alternating runs of the baseline and PGO executables (`research/vsjxl.py -n5` twice, alternating, at `--threads 1` and `--threads 3` on `measure/image.bmp`, plus corpus timing across `photo`, held-out `tp`, and `synth` with geometric means and worst-image reporting via `research/vsjxl.py --corpus all -n 3`). Done when: the one-thread and three-thread decode and encode ratios (minimum and median) are quoted against the 5% one-thread decode bar (`ratio <= 0.950`); if the gain is below 5%, keep release builds unprofiled by default (leaving `--pgo-gen`, `scripts/pgo.ps1`, `scripts/pgo.py`, and `--pgo-use` wired and tested), verify baseline release behavior, and record the negative result in `docs/research.md` section 3.4.
- [x] Commit: `"build: profile-guided optimization from a reproducible profile"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case (`36 passed, 0 failed`, the bytes did not move); `llvm-objdump -p bin\windows-x64\bin\rasterfold.exe` lists only `DLL Name: KERNEL32.dll` and `DLL Name: SHELL32.dll`; profile provenance is `build/pgo/rasterfold.profdata` (`a08f16b63a6f597c6e3f10ffdc8afb38fccd4c3842d533ea4c53bef772e89d70`, `photo:non-tp-kodak-10`, 40 runs), baseline executable `build/research/vs/rasterfold-base.exe` (`4a51a4015af0b9c0b7ea58893b8f99e8095c844e83e310fbfe8a340f48014f67`), and PGO executable `build/research/vs/rasterfold-pgo.exe` (`e1a1b0143a465a72f3934eb37661431a2cc87a5f2ee82b65e5a66dd705d91a24`). On an observed idle machine (no concurrent build or test jobs), alternating `python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-base.exe --alt-exe build/research/vs/rasterfold-pgo.exe` runs on `measure/image.bmp` give `--threads 1` decode ratio `1.079` min (`573.8 / 531.6 ms`) / `1.034` median (`593.2 / 573.6 ms`) and encode ratio `0.962` min (`552.5 / 574.1 ms`) / `0.978` median (`570.9 / 583.9 ms`) on pass 1, and `--threads 1` decode ratio `0.991` min (`502.6 / 507.0 ms`) / `1.005` median (`521.1 / 518.7 ms`) and encode ratio `0.988` min (`507.3 / 513.6 ms`) / `1.021` median (`549.2 / 537.7 ms`) on pass 2; at `--threads 3`, pass 1 gives decode ratio `1.020` min (`195.5 / 191.6 ms`) / `1.055` median (`212.7 / 201.7 ms`) and encode ratio `1.007` min (`191.0 / 189.7 ms`) / `1.214` median (`237.1 / 195.4 ms`), and pass 2 gives decode ratio `1.099` min (`222.2 / 202.1 ms`) / `1.047` median (`238.8 / 228.0 ms`) and encode ratio `1.075` min (`213.4 / 198.5 ms`) / `1.040` median (`259.5 / 249.5 ms`). Across the corpus on an idle machine via `python3 scripts/windows.py research/vsjxl.py --corpus all -n 3 --threads <1|3> --pgo-profile build/pgo/rasterfold.profdata` (aggregating per-image minimum time over 3 alternating baseline/candidate passes per set -- thread-cycle time at `-t1` and wall time at `-t3` per `research/bench.c` -- reporting set sum of per-image minima `sum(min_alt)/sum(min_base)`, geometric mean of per-image minimum ratios, and worst per-image minimum ratio), `photo` (`n=39`) gives `-t1` decode `1.009` (`9295.8 / 9215.6 ms`, geomean `1.020`, worst `kodim19` `1.118`) and encode `0.998` (`9437.3 / 9453.1 ms`, geomean `1.000`, worst `kodim24` `1.082`), `-t3` decode `1.084` (`3497.2 / 3227.1 ms`, geomean `1.073`, worst `kodim13` `1.355`) and encode `1.109` (`3429.6 / 3092.8 ms`, geomean `1.090`, worst `ic_nightshot_iso_100` `1.421`); held-out `tp` (`n=10`) gives `-t1` decode `1.004` (`2029.5 / 2021.1 ms`, geomean `1.003`, worst `tp_spider_web_crop` `1.062`) and encode `0.978` (`2028.3 / 2074.5 ms`, geomean `0.975`, worst `tp_spider_web_crop` `1.047`), `-t3` decode `0.976` (`744.9 / 763.5 ms`, geomean `0.972`, worst `tp_fireworks_crop` `1.122`) and encode `0.994` (`744.4 / 748.9 ms`, geomean `1.004`, worst `tp_spider_web_crop` `1.293`); `synth` (`n=11`) gives `-t1` decode `0.995` (`646.0 / 649.1 ms`, geomean `0.997`, worst `syn_solid` `1.033`) and encode `1.012` (`653.8 / 645.8 ms`, geomean `1.004`, worst `syn_checker` `1.065`), `-t3` decode `1.006` (`203.6 / 202.4 ms`, geomean `0.998`, worst `syn_pixel` `1.125`) and encode `0.995` (`191.0 / 191.9 ms`, geomean `0.992`, worst `syn_solid` `1.200`). Because one-thread decode speedup is below the 5% bar (`ratio <= 0.950`), default release builds remain unprofiled and the negative result is recorded in `docs/research.md` section 3.4.

> **Verified:** 2026-10-25 | §1 | f35ddd32ea7b4f2ace1c0ebf81a43f6593c3c12d
> **Review:** `docs/reviews/d02-t01-s1.json`
## 2. Link-Time Optimization

> **Started:** 2026-10-25T18:48:00Z

**Needs:** Idle machine (timing)

The codec is small, but `cm.c`'s entry points are called through `fold.c`, and LTO can inline across that seam. Bar: one-thread (`--threads 1`) decode minimum-time ratio `<= 0.970` on `measure/image.bmp` across both 5-run alternating passes (`research/vsjxl.py -n5 --threads 1`) and confirmed on `photo` (`min_tot <= 0.970`) and held-out `tp` (`min_tot <= 0.970`) with no encode or 3-thread regression exceeding `1.020`, with the Windows executable and DLL imports and static/shared library consumer compatibility unchanged (and no PGO by default, since §1 recorded a negative result).

- [x] Wire `-flto` (`--lto`) in `scripts/build.py` for linked CLI and shared-library artifacts (`rasterfold.exe`, `rasterfold.dll`, `rasterfold`, `librasterfold.so`) while keeping static archives (`librasterfold.a`) compiled to native machine-code `.o` files so non-LTO C/C++ static consumers link cleanly without LLVM bitcode objects, and verify all four targets (`windows-x64`, `linux-x64`, `android-arm64`, `android-x64`), `llvm-objdump -p` imports on `bin\windows-x64\bin\rasterfold.exe` (`KERNEL32.dll` and `SHELL32.dll` only) and `bin\windows-x64\bin\rasterfold.dll` (`KERNEL32.dll` only), and `tests/test_artifacts.py` (`test_public_exports` and `test_c_cpp_static_and_dynamic_consumers`). Done when: `build.ps1` and `python3 scripts/build.py --target all --lto` exit 0, `llvm-objdump -p` lists only `KERNEL32.dll` and `SHELL32.dll` on the EXE and `KERNEL32.dll` on the DLL, and `tests/test_artifacts.py` passes. Cheaper substitute: trusting LTO without checking that it did not pull in a runtime call or break static library consumers.
- [x] Extend `research/vsjxl.py` with `--lto` corpus mode that compiles separate translation units (`src/cm.c`, `src/fold.c`, `src/bmp.c`, `src/crc32.c`, `research/bench.c`, `research/benchsys.c`) with and without `-flto` (without PGO by default) so cross-TU inlining across `bench.c -> cm.c -> benchsys.c` is exercised, and measure alternating runs on `measure/image.bmp` (`research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-base.exe --alt-exe build/research/vs/rasterfold-lto.exe`, twice each) and across `photo`, held-out `tp`, and `synth` (`research/vsjxl.py --corpus all -n 3 --threads <1|3> --lto`). Done when: the one-thread and three-thread decode and encode ratios, SHA-256 digests, and corpus summaries are quoted against the `0.970` bar; if below the bar, keep default release builds non-LTO (retaining `--lto` as a tested opt-in flag in `scripts/build.py`), rebuild and verify the restored non-LTO default release configuration, and record the negative result in `docs/research.md` sections 3.4 and 5.2.
- [x] Commit: `"build: link-time optimization"`

**Test checkpoint:** Under `--lto` (`python3 scripts/build.py --target all --lto`), `python3 tests/test_correctness.py` (`36 passed, 0 failed`), `python3 scripts/windows.py tests/test_correctness.py` (`36 passed, 0 failed`), `python3 tests/test_api.py` (`9 passed`), and `python3 scripts/windows.py tests/test_api.py` (`9 passed`) pass every case on Linux and Windows (Android `android-arm64` and `android-x64` `--lto` coverage is cross-compilation plus `llvm-nm` exported-symbol and `llvm-readelf -d` `SONAME` validation in `test_public_exports`, without Android device execution), and after restoring the non-LTO default release configuration (`python3 scripts/build.py --target all`), `python3 tests/test_correctness.py` (`36 passed, 0 failed`), `python3 scripts/windows.py tests/test_correctness.py` (`36 passed, 0 failed`), `python3 tests/test_artifacts.py` (`8 passed` on Linux), and `python3 scripts/windows.py tests/test_artifacts.py` (`8 passed, 2 skipped` on Windows, including `test_lto_build_imports_and_consumers` exercising `--lto` exports, C/C++ static/shared consumers, CLI correctness cases `known-answer`/`odd3`/`corrupt`/`truncated-abort`, and atomic publication/no-clobber checks before restoring non-LTO) pass every case; `llvm-objdump -p bin/windows-x64/bin/rasterfold.exe` under `--lto` lists only `DLL Name: KERNEL32.dll` and `DLL Name: SHELL32.dll`, and `llvm-objdump -p bin/windows-x64/bin/rasterfold.dll` lists only `DLL Name: KERNEL32.dll`; baseline executable `build/research/vs/rasterfold-base.exe` (`ad71233aed79d1eaa685888c8d8e0021765328b17eca2aab36ceeef521a433ef`) and LTO executable `build/research/vs/rasterfold-lto.exe` (`414c7df37702a4d8749471787c1e0d271cd641ef3f030407f6930950f25ccd8d`) produce identical `.fold` bytes (`1,409,028` bytes). On an observed idle machine, alternating `python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-base.exe --alt-exe build/research/vs/rasterfold-lto.exe` runs on `measure/image.bmp` give `--threads 1` decode ratio `0.924` min (`554.0 / 599.8 ms`) / `0.985` median (`600.2 / 609.2 ms`) and encode ratio `0.968` min (`576.9 / 596.3 ms`) / `0.907` median (`591.5 / 651.8 ms`) on pass 1, and `--threads 1` decode ratio `0.905` min (`516.9 / 571.1 ms`) / `1.044` median (`634.5 / 607.8 ms`) and encode ratio `0.986` min (`551.1 / 558.8 ms`) / `0.907` median (`579.6 / 639.1 ms`) on pass 2; at `--threads 3`, pass 1 gives decode ratio `1.042` min (`237.3 / 227.7 ms`) / `0.868` median (`243.1 / 280.0 ms`) and encode ratio `1.041` min (`225.3 / 216.3 ms`) / `0.847` median (`243.2 / 287.2 ms`), and pass 2 gives decode ratio `1.033` min (`215.6 / 208.6 ms`) / `1.073` median (`228.1 / 212.5 ms`) and encode ratio `1.098` min (`199.9 / 182.0 ms`) / `0.959` median (`218.7 / 228.1 ms`). Across the corpus on an idle machine via `python3 scripts/windows.py research/vsjxl.py --corpus all -n 3 --threads <1|3> --lto`, `photo` (`n=39`) gives `-t1` decode `min_tot=1.008` (`9732.5 / 9658.4 ms`, geomean `0.981`, worst `ic_nightshot_iso_100` `1.135`) and encode `min_tot=0.996` (`9804.7 / 9844.2 ms`, geomean `0.989`, worst `ic_spider_web` `1.123`), `-t3` decode `min_tot=0.960` (`3355.1 / 3494.3 ms`, geomean `0.986`, worst `kodim07` `1.238`) and encode `min_tot=0.925` (`3083.9 / 3335.7 ms`, geomean `0.936`, worst `kodim20` `1.224`); held-out `tp` (`n=10`) gives `-t1` decode `min_tot=1.019` (`2106.4 / 2068.0 ms`, geomean `0.999`, worst `tp_nightshot_iso_1600_crop` `1.210`) and encode `min_tot=0.991` (`2111.2 / 2131.1 ms`, geomean `0.981`, worst `tp_spider_web_crop` `1.098`), `-t3` decode `min_tot=1.086` (`739.5 / 681.2 ms`, geomean `1.073`, worst `tp_nightshot_iso_1600_crop` `1.179`) and encode `min_tot=1.267` (`771.4 / 608.9 ms`, geomean `1.239`, worst `tp_flower_foveon_crop` `1.467`); `synth` (`n=11`) gives `-t1` decode `min_tot=0.937` (`661.0 / 705.6 ms`, geomean `0.944`, worst `syn_odd` `1.083`) and encode `min_tot=0.939` (`683.0 / 727.7 ms`, geomean `0.950`, worst `syn_odd` `1.077`), `-t3` decode `min_tot=0.899` (`216.5 / 240.7 ms`, geomean `0.949`, worst `syn_gradient` `1.249`) and encode `min_tot=0.762` (`190.0 / 249.3 ms`, geomean `0.847`, worst `syn_pixel` `1.222`). Because `photo` (`1.008`) and held-out `tp` (`1.019`) miss the `<= 0.970` corpus bar (and 3-thread `tp` regresses to `1.086` decode / `1.267` encode), default release builds remain non-LTO and the negative result is recorded in `docs/research.md` sections 3.4 and 5.2.

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
