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
> **Current state (verified 2026-09-29):** The format knows method 0 (raw) and method 1 (CM); `rf_decode` rejects any method above 1 as `unsupported fold: method`. The range coder lives inside `src/cm.c`. On the generated images (`python research/ab.py size synth -v`, 2026-09-29) RasterFold is 18.07 times JPEG XL's size on `syn_checker` (5709 against 316 bytes) and 9.08 times on `syn_pixel` (599 against 66); both images have two colours. Design: `docs/research.md` section 4.1.
<!-- claim: absent src/pal.c -->
<!-- claim: absent src/rc.h -->
<!-- claim: count "^#define RF_METHOD_CM 1u$" src/rf.h = 1 -->
<!-- claim: count "method > RF_METHOD_CM" src/fold.c = 1 -->

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
|   1   |   §1    | One range coder shared between methods        | --         |  [ ]   |
|   2   |   §2    | Method 2: palette encoder, decoder, oracle, spec | §1      |  [ ]   |
|   3   |   §3    | Measure method 2 against JPEG XL and WebP     | §2         |  [ ]   |

---

## 1. One Range Coder Shared between Methods

> **Started:** 2026-10-25T23:36:00Z

Method 2 needs the same binary range coder as method 1. A second copy would drift; one header keeps a single implementation. This section changes no byte: it moves code.

> [!NOTE]
> **Corrected 2026-10-26:** Since `D00 T01 §1` (`scripts/build.py` multi-target Ninja layout), `D03 T01 §1` (`rdec_tail` zero-padded tail refill), `D02 T01 §1` (`PGO_TRACKED_SOURCES` in `scripts/build.py`), and `D02 T01 §4` / `D04 T01 §1` / `D04 T01 §3` (37 cases in `tests/test_correctness.py`), `src/rc.h` also includes `rdec_tail`, is added to `PGO_TRACKED_SOURCES` in `scripts/build.py`, rebuilds `build/<target>/cm.o` under `tools/ninja-linux/ninja -f build/<target>/build.ninja -n` (Windows: `tools/ninja/ninja.exe -f build/windows-x64/build.ninja -n`), updates the `Current state` claim for `src/rc.h` from `absent` to `exists` when `src/rc.h` lands, and `tests/test_correctness.py` prints `37 passed, 0 failed`.

- [ ] Move `renc`, `renc_init`, `renc_shift`, `renc_bit`, `renc_flush`, `rdec`, `rdec_init`, `rdec_tail`, and `rdec_bit` from `src/cm.c` into a new `src/rc.h` unchanged, and include it from `src/cm.c`. Done when: `python3 scripts/build.py --target all` (Windows: `python scripts/build.py --target windows-x64`) and `python3 scripts/tidy.py` exit 0 with no warning. Cheaper substitute: copying the coder into the palette file later.
- [ ] Add `src/rc.h` to `PGO_TRACKED_SOURCES` in `scripts/build.py` and verify the `-MMD` depfiles generated by `scripts/build.py` cover `src/rc.h` (confirm a touch of `src/rc.h` rebuilds `cm.o`). Done when: `tools/ninja-linux/ninja -f build/linux-x64/build.ninja -n` and `tools/ninja/ninja.exe -f build/windows-x64/build.ninja -n` after touching `src/rc.h` list `build/linux-x64/cm.o` and `build/windows-x64/cm.o`.
- [ ] Commit: `"codec: share one range coder header between methods"`

**Test checkpoint:** `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` print `37 passed, 0 failed` with the `known-answer` case passing, so the bytes did not move; a probe that changes `renc_bit`'s rounding makes `known-answer` fail (quoted, then reverted).

**Freeze check:** `KAT_FOLD` is unchanged in `tests/test_correctness.py`, and `git diff --stat` shows no change under `tests/` or `spec/`.

## 2. Method 2: Palette Encoder, Decoder, Oracle, Spec

**Job:** A user can export an image with at most 256 colours to a small `.fold` file, import it back to identical pixels (the reverse of the export), and read the method and colour count that `inspect` reports.

The method itself, in one commit with every place a format change owes. The design is `docs/research.md` section 4.1: a first-seen palette of up to 256 colours, index decisions `I == W`, `I == N`, `I == NE` under a 5-bit equality pattern of the neighbours' indices, and an 8-bit literal tree for the rest, each decision one adaptive counter with method 1's update rule. Exchange and its reverse ship together: the encoder exports a method 2 file only when the decoder of the same commit imports it back to identical pixels, and `inspect` reports the method and the colour count.

- [ ] Add `src/pal.c` with `rf_pal_build` (a 1024-slot open-addressing hash of `R<<16|G<<8|B`, stopping at the 257th colour), `rf_pal_encode`, and `rf_pal_decode`, using `src/rc.h`. Done when: it builds with no warning.
- [ ] Extend `rf_encode` in `src/fold.c` to try method 2 when the image has at most 256 colours and keep the smaller of methods 1 and 2 (ties: method 1), and `rf_decode` to accept method 2 with its checks (colour count, no duplicate palette entry, stream size, every literal index below the count and not an excluded candidate, the stream ending exactly). Done when: a 2-colour BMP encodes to a file whose byte 16 is `2`, and the decoder imports it back to identical pixels (the reverse of the encode).
- [ ] Print `method: palette` and `palette_colours: <n>` in `inspect` (`src/main.c`). Done when: `bin/windows-x64/bin/rasterfold.exe inspect` reports both lines on that file.
- [ ] Write `pal_encode` and `pal_decode` into `tests/rfi_ref.py` from the spec text of the next item, and give `encode_image` the same method choice. Done when: the oracle and the codec produce the same bytes for every round-trip case.
- [ ] Add "Method 2, palette" to `spec/FOLD.md`: payload, index neighbours, contexts, decisions, literal check, rejection rules; and the method choice to "Reference encoder policy". Done when: a reader can decode a method 2 file from the spec alone (the oracle item above was written from it).
- [ ] Add a method 2 known-answer vector `KAT_PAL` beside `KAT_FOLD` in `tests/test_correctness.py`, and rejection cases (colour count beyond the palette bytes, a duplicate colour, a literal index at or above the count, a truncated stream, a trailing byte), each with its checksums recomputed by `recrc()`. Done when: each rejection exits 4 with its message.
- [ ] Commit: `"codec: method 2 codes images with few colours through a palette"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case (the count rises by the new cases), `known-answer` included; `python tests/fuzz_decode.py 2000 7` reports no crash, hang, or mismatch.

**Freeze check:** `KAT_FOLD` still decodes to its pixels and is byte-identical when re-encoded; a method 2 file given to the version 2 decoder built from the commit before this section exits 3 with `unsupported fold: method`, never misread (quoted).

## 3. Measure Method 2 against JPEG XL and WebP

**Needs:** ffmpeg with libjxl (benchmarks)

The method exists to close the synthetic gap without touching photographs. This section proves both and puts the result on the public record.

- [ ] Extend `research/bench.c` and `research/ab.py` to measure through `rf_encode` and `rf_decode` (the container), so method 2 is reachable from `ab.py size`. Done when: `python research/ab.py size synth -v` lists `syn_checker` with its method 2 size.
- [ ] Measure `python research/ab.py size photo`, `tp`, and `synth -v`. Done when: the synthetic images with at most 256 colours are below JPEG XL e7, and the photo and test-pack totals are unchanged. Stop condition: if `syn_pixel` stays above JPEG XL's 66 bytes, add `WW == a` and `NN == b` to the pattern (`docs/research.md` section 4.1, stop condition) in a follow-up section filed through `add-todo`, and ship this measurement as is.
- [ ] Rerun `python tests/benchmark.py synthetic -n 3`, commit `benchmarks/results.md` and `benchmarks/results.csv`, and update the generated-image paragraph of `README.md` and the synthetic row of `docs/research.md` section 1. Done when: the README quotes the new synthetic figures.
- [ ] Commit: `"codec: measure method 2 against JPEG XL and WebP"`

**Test checkpoint:** the three `ab.py size` summary lines before and after are quoted; `syn_checker` and `syn_pixel` are below JPEG XL e7 or the stop condition's follow-up is filed and named.

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0: build, correctness, fuzz, TODO gates green
- [ ] `python tests/test_correctness.py` exits 0 with every case run, the method 2 cases included
- [ ] `python tests/measure_final.py` PASS (the reference image is a photograph and must not change)
- [ ] `python scripts/todo-graph.py validate` clean
