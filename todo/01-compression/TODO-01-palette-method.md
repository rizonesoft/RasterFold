---
schema_version: 1
id: palette-method
domain: 01-compression
status: draft
title: "TODO-01 -- Palette Method for Images with Few Colours"
depends_on: []
frozen: true
track: C1
---

# TODO-01 -- Palette Method for Images with Few Colours

> **Goal:** Images with at most 256 colours (checkerboards, pixel art, flat diagrams, simple screenshots) code through a new method 2 that turns pixels into palette indices with near-certain equality contexts, so they are smaller than lossless JPEG XL e7, while photographs are untouched and every version 2 file still decodes to the same pixels.

> [!IMPORTANT]
> **Current state (verified 2026-10-26):** The format knows method 0 (raw), method 1 (CM), and method 2 (palette, `RF_METHOD_PAL = 2u` in `src/rf.h` and `src/pal.c`); `rf_decode` and `rf_decode_bmp` reject any method above 2 as `unsupported fold: method`. Section 1 moved the shared binary range coder (`renc`, `renc_init`, `renc_shift`, `renc_bit`, `renc_flush`, `rdec`, `rdec_init`, `rdec_tail`, `rdec_bit`) from `src/cm.c` into `src/rc.h`, and section 2 added `src/pal.c`, `pal_encode`/`pal_decode` in `tests/rfi_ref.py`, "Method 2, palette" in `spec/FOLD.md`, `inspect` reporting (`method: palette` and `palette_colours: <n>`), and `KAT_PAL` plus boundary and rejection cases in `tests/test_correctness.py` and `tests/test_api.py`, while corpus measurement (§3) remains open. On the generated images (`python research/ab.py size synth -v`, 2026-09-29, pre-method-2 `cm.c` harness) RasterFold was 18.07 times JPEG XL's size on `syn_checker` (5709 against 316 bytes) and 9.08 times on `syn_pixel` (599 against 66); both images have two colours. Design: `docs/research.md` section 4.1.
<!-- claim: exists src/pal.c -->
<!-- claim: exists src/rc.h -->
<!-- claim: count "^#define RF_METHOD_PAL 2u$" src/rf.h = 1 -->
<!-- claim: count "method > RF_METHOD_PAL" src/fold.c = 1 -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- section 4.1, the method 2 design (payload, contexts, decisions, literal check)
- [`src/cm.c`](../../src/cm.c) -- the range coder §1 moves into a shared header
- [`src/fold.c`](../../src/fold.c) -- the method choice and the decoder's method check §2 extends
- [`tests/rfi_ref.py`](../../tests/rfi_ref.py), [`spec/FOLD.md`](../../spec/FOLD.md), [`tests/test_correctness.py`](../../tests/test_correctness.py) -- the oracle, spec, and tests §2 extends
- -> XREF: D01 T06 §3 -- copy runs, which extend method 2 before the 1.0 release
- -> XREF: D04 T01 §2 -- the fuzz seeds that reach the method 2 decoder, which §2 adds and the long fuzz campaign runs

## Outcome

- `python research/ab.py size synth -v` shows `syn_checker` and `syn_pixel` below their JPEG XL e7 sizes (316 and 66 bytes).
- `python research/ab.py size photo` prints the same total as before this file (0.9712 of JPEG XL on 2026-09-29): no photograph takes method 2.
- The version 2 known-answer file `KAT_FOLD` decodes unchanged, and a method 2 file has its own known-answer vector.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (the encoder chooses the method by size, no option); reporting=applicable @ D01 T01 §2; notifications=not-applicable (errors reach the user through the existing exit codes and messages); permissions=not-applicable (file safety is unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T01 §2; reverse=applicable @ D01 T01 §2

**Adjacency rationale:** Reporting is `inspect` printing `method: palette` and the colour count. Exchange and reverse are the new method's encoder and decoder, which ship together in one section so no file can be written that the decoder does not read.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | One range coder shared between methods        | --         |  [x]   |
|   2   |   §2    | Method 2: palette encoder, decoder, oracle, spec | §1      |  [x]   |
|   3   |   §3    | Measure method 2 against JPEG XL and WebP     | §2         |  [ ]   |

---

## 1. One Range Coder Shared between Methods

> **Started:** 2026-10-25T23:36:00Z

Method 2 needs the same binary range coder as method 1. A second copy would drift; one header keeps a single implementation. This section changes no byte: it moves code.

> [!NOTE]
> **Corrected 2026-10-26:** Since `D00 T01 §1` (`scripts/build.py` multi-target Ninja layout), `D03 T01 §1` (`rdec_tail` zero-padded tail refill), `D02 T01 §1` (`PGO_TRACKED_SOURCES` in `scripts/build.py`), and `D02 T01 §4` / `D04 T01 §1` / `D04 T01 §3` (37 cases in `tests/test_correctness.py`), `src/rc.h` also includes `rdec_tail`, is added to `PGO_TRACKED_SOURCES` in `scripts/build.py`, rebuilds `build/<target>/cm.o` under `tools/ninja-linux/ninja -f build/<target>/build.ninja -n` (Windows: `tools/ninja/ninja.exe -f build/windows-x64/build.ninja -n`), updates the `Current state` claim for `src/rc.h` from `absent` to `exists` when `src/rc.h` lands, and `tests/test_correctness.py` prints `37 passed, 0 failed`.

- [x] Move `renc`, `renc_init`, `renc_shift`, `renc_bit`, `renc_flush`, `rdec`, `rdec_init`, `rdec_tail`, and `rdec_bit` from `src/cm.c` into a new `src/rc.h` unchanged, and include it from `src/cm.c`. Done when: `python3 scripts/build.py --target all` (Windows: `python scripts/build.py --target windows-x64`) and `python3 scripts/tidy.py` exit 0 with no warning. Cheaper substitute: copying the coder into the palette file later.
- [x] Add `src/rc.h` to `PGO_TRACKED_SOURCES` in `scripts/build.py` and verify the `-MMD` depfiles generated by `scripts/build.py` cover `src/rc.h` (confirm a touch of `src/rc.h` rebuilds `cm.o`). Done when: `tools/ninja-linux/ninja -f build/linux-x64/build.ninja -n` and `tools/ninja/ninja.exe -f build/windows-x64/build.ninja -n` after touching `src/rc.h` list `build/linux-x64/cm.o` and `build/windows-x64/cm.o`.
- [x] Commit: `"codec: share one range coder header between methods"`

**Test checkpoint:** `python3 scripts/build.py --target all` builds `linux-x64`, `windows-x64`, `android-arm64`, and `android-x64` with 0 warnings; `python3 scripts/tidy.py` exits 0 (`tidy: 11/11 unique warning(s) across 6 target databases`); after `touch src/rc.h`, `tools/ninja-linux/ninja -f build/linux-x64/build.ninja -n -v` prints `[1/4] ... -c .../src/cm.c -o .../build/linux-x64/cm.o` and `tools\ninja\ninja.exe -f build\windows-x64\build.ninja -n -v` prints `[1/4] ... -c .../src/cm.c -o .../build/windows-x64/cm.o`; `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` print `37 passed, 0 failed` with `OK   known-answer`; and a deliberate probe changing `renc_bit`'s bound in `src/rc.h` from `(e->range >> 12) * p1` to `((e->range >> 12) * p1) + 1` makes `python3 tests/test_correctness.py -k known-answer` exit 1 (`FAIL known-answer: codec bytes differ from the known-answer file`, `0 passed, 1 failed, 36 skipped by -k`) and reverting restores `OK   known-answer`.

**Freeze check:** `KAT_FOLD` is unchanged in `tests/test_correctness.py` (`sha256=b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32`), and `git diff --stat -- tests/ spec/` prints nothing (no change under `tests/` or `spec/`).

> **Verified:** 2026-10-26 | §1 | 6f76629d1e64d81ee585d02d72e23a23268556f9
> **Review:** `docs/reviews/d01-t01-s1.json`
## 2. Method 2: Palette Encoder, Decoder, Oracle, Spec

> **Started:** 2026-10-26T00:06:00Z

**Job:** A user can export an image with at most 256 colours to a small `.fold` file, import it back to identical pixels (the reverse of the export), and read the method and colour count that `inspect` reports.

The method itself, in one commit with every place a format change owes. The design is `docs/research.md` section 4.1: a first-seen palette of up to 256 colours, index decisions `I == W`, `I == N`, `I == NE` under a 5-bit equality pattern of the neighbours' indices, and an 8-bit literal tree for the rest, each decision one adaptive counter with method 1's update rule. Exchange and its reverse ship together: the encoder exports a method 2 file only when the decoder of the same commit imports it back to identical pixels, and `inspect` reports the method and the colour count.

> [!NOTE]
> **Corrected 2026-10-26:** Adding `src/pal.c` and method 2 (`RF_METHOD_PAL = 2u`) also updates `COMMON` and `PGO_TRACKED_SOURCES` in `scripts/build.py`, `COMMON` in `scripts/tidy.py`, `units` in `research/vsjxl.py`, `docs/format.md` via `python3 scripts/build-docs.py --write`, and the `Current state` claims in this file (`src/pal.c` from `absent` to `exists`, `RF_METHOD_PAL 2u` in `src/rf.h`, and `method > RF_METHOD_PAL` in `src/fold.c`). In `src/pal.c`, `tests/rfi_ref.py`, and `spec/FOLD.md`, each counter's 12-bit coding probability is `max(1, p >> 4)` so `p < 16` after repeated zero bits can never pass `p1 = 0` (`bound = 0`) into the binary range coder. Per the `--kind plan` and Round 1 reviews (`P1`, `P2`): (1) both `rf_decode` (`stride == 0` packed RGB) and `rf_decode_bmp` (`stride != 0` bottom-up BGR with failure cleanup) in `src/fold.c` dispatch `RF_METHOD_PAL` to `rf_pal_decode`, and both CLI (`tests/test_correctness.py`) and library (`tests/test_api.py`, including guard-page testing) verify method 2 round trips; (2) `rf_info` keeps its frozen public ABI (`sizeof(rf_info)` unchanged in `include/rasterfold.h`, `stream_size[0..2] == 0` for `RF_METHOD_PAL`), while `src/main.c` reads `pal_colours = (uint32_t)file[RF_HEADER_SIZE] + ((uint32_t)file[RF_HEADER_SIZE + 1] << 8) + 1u` before `rf_free(file)` and prints `method: palette` and `palette_colours: <n>` only after `rf_inspect` validates the complete file; (3) `rf_encode` and `encode_image` select among raw (`0`), CM (`1`, eligible when each channel stream fits in `w * h + 64` bytes per `D03 T01 §3` and total payload `< raw`), and palette (`2`, eligible when `rf_pal_build` returns `1..256` colours and payload `< best_size`) so ties keep raw over CM/palette and CM over palette; (4) `tests/fuzz_decode.py` adds method 2 palette seeds and method-aware mutations so `python3 tests/fuzz_decode.py 2000 7` fuzzes method 2 files; and (5) `tests/test_correctness.py` and `tests/test_artifacts.py` cover `KAT_PAL` (exercising D1, D2, D3, and literal branches), 256 colours (`method == 2`, `palette_colours: 256`), 257th-colour fallback (`method == 1`), tiny 1-colour raw fallback (`method == 0`), per-channel CM stream overflow (`> w * h + 64` falling back to `method == 2` for `<= 256` colours and `method == 0` for `> 256` colours), a >= 300-pixel run exercising the `max(1, p >> 4)` clamp, and rejections for `count > 256` with full palette bytes, `stream_size < 5`, colour count beyond palette bytes, duplicate colour, literal `>= count`, excluded-candidate literals (`lit == a`, `lit == b`, `lit == d`), truncated stream, and trailing byte.

- [x] Add `src/pal.c` with `rf_pal_build` (a 1024-slot open-addressing hash of `R<<16|G<<8|B`, stopping at the 257th colour), `rf_pal_encode`, and `rf_pal_decode` (handling both `stride == 0` packed RGB and `stride != 0` bottom-up BGR BMP output), using `src/rc.h`, and wire `pal` into `scripts/build.py`, `scripts/tidy.py`, and `research/vsjxl.py`. Done when: it builds with no warning across all targets and `scripts/tidy.py` passes.
- [x] Extend `rf_encode` in `src/fold.c` to try method 2 when the image has at most 256 colours and keep the smallest eligible candidate of raw (`0`), CM (`1`, where each channel stream is `<= w * h + 64` bytes per `D03 T01 §3`), and palette (`2`) (with `rf_cm_encode` capped at `raw - 1` and `rf_pal_encode` capped at `best_size - 1`, so ties keep raw over CM/palette and CM over palette), and extend both `rf_decode` and `rf_decode_bmp` to accept method 2 (`RF_METHOD_PAL = 2u`) with its checks (colour count in `1..256`, no duplicate palette entry, stream size `>= 5` matching payload length, every literal index below the count and not an excluded candidate `a`, `b`, or `d`, and the stream ending exactly, with failure cleanup freeing allocated output). Done when: a 2-colour BMP encodes to a file whose byte 16 is `2`, and both CLI and library decoders import it back to identical pixels (the reverse of the encode).
- [x] Print `method: palette` and `palette_colours: <n>` in `inspect` (`src/main.c`, reading the validated `count` from the payload header before freeing `file` while preserving `rf_info` ABI). Done when: `bin/windows-x64/bin/rasterfold.exe inspect` reports both lines on that file.
- [x] Write `pal_encode` and `pal_decode` into `tests/rfi_ref.py` from the spec text of the next item, give `encode_image` the same raw/CM/palette method choice (including the `width * height + 64` per-channel stream bound for CM), and extend `tests/fuzz_decode.py` with method 2 seeds and method-aware stream mutations. Done when: the oracle and the codec produce the same bytes for every round-trip case and `tests/fuzz_decode.py` mutates method 2 files.
- [x] Add "Method 2, palette" to `spec/FOLD.md`: payload, index neighbours, contexts, decisions (with `max(1, p >> 4)` coding probability), literal check, rejection rules; and the raw/CM/palette method choice (with the per-channel `width * height + 64` stream bound for method 1) to "Reference encoder policy", then sync `docs/format.md` via `python3 scripts/build-docs.py --write`. Done when: a reader can decode a method 2 file from the spec alone (the oracle item above was written from it).
- [x] Add a method 2 known-answer vector `KAT_PAL` beside `KAT_FOLD` in `tests/test_correctness.py` (plus guard-page and inspect coverage in `tests/test_api.py` and in-place overflow fallback coverage in `tests/test_artifacts.py`), boundary cases (palette winning on `KAT_PAL`, CM winning on 68-colour `KAT_FOLD`, raw winning on 1x1 `px1`, 256 colours choosing method 2 with `palette_colours: 256`, 257 colours falling back to method 1, per-channel CM stream overflow `> w * h + 64` selecting method 2, and a >= 300-run image exercising the `max(1, p >> 4)` counter clamp), and rejection cases (`count > 256` with full palette bytes, `stream_size < 5`, colour count beyond the palette bytes, a duplicate colour, a literal index at or above the count, excluded-candidate literals `lit == a`, `lit == b`, and `lit == d`, a truncated stream, and a trailing byte), each with its checksums recomputed by `recrc()`. Done when: each rejection exits 4 with its message and `tests/test_correctness.py`, `tests/test_api.py`, and `tests/test_artifacts.py` pass.
- [x] Commit: `"codec: method 2 codes images with few colours through a palette"`

**Test checkpoint:** `python3 scripts/build.py --target all` builds `linux-x64`, `windows-x64`, `android-arm64`, and `android-x64` with 0 warnings; `python3 scripts/tidy.py` exits 0 (`tidy: 11/11 unique warning(s) across 6 target databases`); `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` pass every case (`38 passed, 0 failed`, including `OK   oracle`, `OK   known-answer`, `OK   corrupt`, `OK   tail-threshold`, and `OK   palette-method`); `python3 tests/test_api.py` and `python3 scripts/windows.py tests/test_api.py` pass (`Ran 9 tests ... OK`, including `pal_short_tail` and `pal_long_tail` guard-page cases); `python3 tests/test_artifacts.py` and `python3 scripts/windows.py tests/test_artifacts.py` pass (`Ran 8 tests ... OK`); `bin/windows-x64/bin/rasterfold.exe inspect` on `KAT_PAL` prints `method: palette` and `palette_colours: 4`; and `python3 tests/fuzz_decode.py 2000 7` reports `2000 mutated files: 1939 rejected, 61 still valid, no crash, hang or mismatch`.

**Freeze check:** `KAT_FOLD` decodes to its pixels and is byte-identical when re-encoded (`OK   known-answer`, `sha256=b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32`); and giving `KAT_PAL` (`tests/work/kat/pal_out.fold`) to the version 2 decoder built from the commit before this section (`4635a04f1042f3983d8f56222e1521745a191a6d`) exits 3 with `rasterfold: unsupported fold: method` and leaves no output file (`build/pre_s2/rasterfold exit: 3 stderr: rasterfold: unsupported fold: method exists: False`; `build/pre_s2/rasterfold.exe exit: 3 stderr: rasterfold: unsupported fold: method exists: False`).

> **Verified:** 2026-10-26 | §2 | 0a033aa744264d076716c3f78ea5796e5edda0ae
> **Review:** `docs/reviews/d01-t01-s2.json`
## 3. Measure Method 2 against JPEG XL and WebP

> **Started:** 2026-10-26T01:08:30Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A reader can inspect the measured `.fold` sizes of method 2 across `photo`, `tp`, and `synth` against JPEG XL (`e7` and `e9`) and lossless WebP in `benchmarks/corpus.csv`, `benchmarks/results.csv`, `benchmarks/results.md`, `docs/benchmarks.md`, and `docs/research.md`, with photographs unchanged and the `syn_pixel` stop-condition follow-up filed as `D01 T01 §4`.

The method exists to close the synthetic gap without touching photographs. This section proves both and puts the result on the public record.

> [!NOTE]
> **Corrected 2026-10-26:** `research/bench.c` calls `rf_encode` and `rf_decode` (cleaning up `fold` and `dec_im.rgb` via `rf_free`, using `fold_sz` directly since `rf_encode` includes the 32-byte `.fold` header, and printing `"RAW"`, `"PAL"`, or `"OK"` from `info.method`), and `research/ab.py` links `src/fold.c`, `src/pal.c` (or `src/cm.c` when `src` is a `pal.c` variant), `src/bmp.c`, and `src/crc32.c`, accepts `"PAL"` in `parse_bench_rows`, and checks `("src", "include", "tests/test_correctness.py")` in `codec_provenance`'s `git status --porcelain` check (keeping `PROVENANCE_PATHS` for `_provenance_digests` mid-run snapshot hashing) so `python3 research/ab.py size all --csv benchmarks/corpus.csv` runs cleanly in the same commit that updates `research/bench.c`. Because live benchmark tables moved from `README.md` to `docs/benchmarks.md` in `D08 T01`, item 3 updates `benchmarks/corpus.csv`, `benchmarks/results.csv`, `benchmarks/results.md`, `tests/test_research_refs.py` (`test_corpus_csv_reconstructs_all_five_research_summaries`), `docs/research.md` (sections 1 and 3), `research/README.md`, and `docs/benchmarks.md` + `docs/facts.json` + `docs/_includes/generated/{charts,tables}/*` via `python3 scripts/build-docs.py --write`. On `synth`, `syn_checker` drops from `5,709` to `297` bytes (below JPEG XL e7's `316` bytes) while `syn_pixel` drops from `599` to `90` bytes (still above JPEG XL e7's `66` bytes), triggering item 2's stop condition to file follow-up section `D01 T01 §4` through `add-todo` and ship this measurement as is.

- [ ] Extend `research/bench.c` and `research/ab.py` to measure through `rf_encode` and `rf_decode` (the container), so method 2 is reachable from `ab.py size`. Done when: `python research/ab.py size synth -v` lists `syn_checker` with its method 2 size.
- [ ] Measure `python research/ab.py size photo`, `tp`, and `synth -v`. Done when: the synthetic images with at most 256 colours are below JPEG XL e7, and the photo and test-pack totals are unchanged. Stop condition: if `syn_pixel` stays above JPEG XL's 66 bytes, add `WW == a` and `NN == b` to the pattern (`docs/research.md` section 4.1, stop condition) in a follow-up section filed through `add-todo`, and ship this measurement as is.
- [ ] Rerun `python tests/benchmark.py synthetic -n 3` and `python3 research/ab.py size all --csv benchmarks/corpus.csv`, commit `benchmarks/results.md`, `benchmarks/results.csv`, and `benchmarks/corpus.csv`, and update `docs/benchmarks.md` (which replaced the generated-image benchmark section of `README.md` in `D08 T01`, regenerated via `python3 scripts/build-docs.py --write`) and the synthetic row of `docs/research.md` section 1 (plus section 3). Done when: `docs/benchmarks.md` and `docs/research.md` quote the new synthetic figures and `python3 scripts/drift-check.py` exits 0.
- [ ] Commit: `"codec: measure method 2 against JPEG XL and WebP"`

**Test checkpoint:** the three `ab.py size` summary lines before and after are quoted; `syn_checker` and `syn_pixel` are below JPEG XL e7 or the stop condition's follow-up (`D01 T01 §4`) is filed and named.

**Freeze check:** `KAT_FOLD` (`sha256=b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32`) and `KAT_PAL` decode to their exact pixels and re-encode byte-identically (`python3 tests/test_correctness.py`: `38 passed, 0 failed`), and `photo` (`30,865,211` bytes) and `tp` (`6,651,583` bytes) corpus totals are unchanged.

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0: build, correctness, fuzz, TODO gates green
- [ ] `python tests/test_correctness.py` exits 0 with every case run, the method 2 cases included
- [ ] `python tests/measure_final.py` PASS (the reference image is a photograph and must not change)
- [ ] `python scripts/todo-graph.py validate` clean
