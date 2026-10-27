---
schema_version: 1
id: format-version-3
domain: 01-compression
status: draft
title: "TODO-03 -- Format Version 3"
depends_on: []
frozen: true
track: C3
---

# TODO-03 -- Format Version 3

> **Goal:** The measured winners of the model experiments and, if its size cost is accepted, horizontal strips for parallel decode become format version 3: new files carry version 3, version 2 files still decode to the same pixels, and every place a format change owes (the C code, the oracle, the spec, the known-answer files, the freeze check) moves in one commit per change.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** The format is version 2 (`RF_VERSION 2u` in `src/rf.h`, magic `RFI\x02`); `rf_decode` rejects any other version as `unsupported fold: version`. The known-answer file `KAT_FOLD` in `tests/test_correctness.py` is a 90-byte version 2 file. No experiment of `D01 T02 §1` to `§6` has run, so nothing is accepted yet.
<!-- claim: count "^#define RF_VERSION 2u$" src/rf.h = 1 -->
<!-- claim: count "^VERSION = 2$" tests/rfi_ref.py = 1 -->
<!-- claim: count "^# RasterFold image format, version 2$" spec/FOLD.md = 1 -->

## Inputs

- [`research/variants/`](../../research/) -- the accepted, combined model change (`version-3.patch`) §1 ports
- [`spec/FOLD.md`](../../spec/FOLD.md), [`tests/rfi_ref.py`](../../tests/rfi_ref.py), [`tests/test_correctness.py`](../../tests/test_correctness.py) -- the spec, oracle, and known-answer files every section here changes
- -> XREF: D01 T02 §1 -- the model experiments whose accepted variants §1 ports
- -> XREF: D99 T01 §3 -- the operator's approval of a new format version, which §1 needs before it starts
- -> XREF: D02 T02 §1 -- the strips measurement whose accepted size cost §2 ports
- -> XREF: D01 T04 §3 -- the match model, which extends version 3 before the 1.0 release
- -> XREF: D01 T05 §2 -- encoder-chosen parameters, which extend version 3 before the 1.0 release
- -> XREF: D01 T06 §2 -- the colour-cache model, which extends version 3 before the 1.0 release

## Outcome

- `bin/windows-x64/bin/rasterfold.exe encode` writes version 3 files; `inspect` prints `RasterFold fold version 3`.
- The version 2 `KAT_FOLD` still decodes to its pixels (kept as a decode-only vector), and a version 3 known-answer file exists.
- `python research/ab.py size photo` shows the combined gain `D01 T02 §7` measured, and `python tests/measure_final.py` still passes.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no new option; strips are chosen from the image height); reporting=applicable @ D01 T03 §1; notifications=not-applicable (the existing exit codes and messages); permissions=not-applicable (file safety is unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T03 §1; reverse=applicable @ D01 T03 §1

**Adjacency rationale:** Exchange is the crux: every version 2 file written before this TODO must keep decoding. Reporting is `inspect` naming the version (and the strip count after §2).

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Version 3: the accepted model changes         | D01 T02 §7, D99 T01 §3 |  [ ]   |
|   2   |   §2    | Version 3: horizontal strips                  | §1, D02 T02 §1 |  [ ]   |

---

## 1. Version 3: The Accepted Model Changes

> **Started:** 2026-10-27T21:58:50Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can export images to smaller version 3 files, import every version 2 and version 3 file back to identical pixels (the reverse of every export), and read the version `inspect` reports.

Ports `research/variants/version-3.patch` (the combination `D01 T02 §7` measured) into method 1 as format version 3, with the version 2 decoder kept for old files. Parts of `version-3.patch` (one part; measured by `D01 T02 §7` on 2026-10-27): the `D01 T02 §1` seven-model change only (a sixth and seventh context model, k5/k6 neighbour contexts, `NMODEL 7`); §2 to §6 of `D01 T02` accepted nothing, so no other part exists. Combined figures: `photo` 30,812,732 (-0.170% against the version 2 baseline, 0.9695 of JPEG XL e7), `tp` 6,641,390 (-0.153%), `synth` 206,229. If `D01 T02 §7` accepted nothing, this section records that, strikes its items with that reason, and stamps the negative result. Version 2 files keep importing: the decoder reverses both versions to their pixels, and `inspect` reports which version a file carries.

**Corrected 2026-10-27:** validation against the tree found four things to fix before building. (1) The file's Current state ("nothing is accepted yet") is stale: `D01 T02 §7` shipped and `research/variants/version-3.patch` exists with the one part listed above; the version claims below it (`RF_VERSION 2u`, oracle `VERSION = 2`, spec title version 2) stay true until this section's port commit flips all three to 3 in the same commit. (2) The Freeze check names `build/research/fold-v2/`, which is empty: the corpus files must be encoded with the pre-change codec before `src/` changes, so that is item 1 below (the old item numbering shifts by one in execution order only; the items keep their text). (3) The parameterization decision for item 1 (now item 2) is made here: the model count becomes a runtime value in `cm.c` (arrays sized for 7, the shipped 5-counter and the new 7-counter `code_bit` kept as two unrolled forms selected per call), the encoder always writes version 3, `rf_cm_decode` takes the file's version (internal header `src/rf.h` only; the public API is unchanged), `src/fold.c` accepts versions 2 and 3 and passes the version through, and the palette and raw payloads are unchanged across versions (the spec will state this). (4) `inspect` prints the file's own version: `src/main.c` currently hardcodes "version 2", so it will print the header byte it validated (no public `rf_info` field is added; the ABI is untouched). Execution order inside the section: freeze corpus, C port, spec, oracle (written from the spec), known-answer files, measurement.

**Corrected 2026-10-27 (plan review `docs/reviews/01-compression/D01-T03-s1.md`):** four gaps folded in. (1) The known-answer migration covers palette and API consumers too: `encode` expectations in `tests/test_correctness.py` and `tests/test_api.py` that name version 2 files (the palette files `KAT_PAL`/`KAT_PAL2` and the API's `KAT_FOLD` encodings) move to new version 3 vectors with identical payloads, and the version 2 vectors stay as decode-only compatibility cases. (2) `tests/test_artifacts.py` generates C that calls `rf_cm_decode` directly; its generated source migrates to the new signature in this section, and version selection is verified through both dispatch paths in `src/fold.c` (packed RGB and BMP decode). (3) The checkpoint inherits the decode-speed acceptance from `D01 T02 §7`: alternating shipped-codec timing against JPEG XL on `measure/image.bmp` with stated thread counts and the below-0.6 minimum-time ratio (not merely `measure_final.py`'s slower-than-JPEG-XL failure line); if the shipped port misses it, that is a defect in this section, not a waiver. (4) The header's two version fields must agree: the spec states it, `src/fold.c` refuses checksum-valid `(2,3)` and `(3,2)` headers as unsupported version, and both the C tests and the oracle carry those refusal cases (no completed output).

- [ ] Create the freeze corpus before touching `src/`: encode every research-corpus image (`photo`, `tp-only`, and `synth` sets, 55 images) with the pre-change codec into `build/research/fold-v2/<name>.fold`. Done when: 55 files exist and each was written by the codec at the commit before this section's first `src/` change.
- [ ] Keep the version 2 model as the decoder for version 2 files: parameterize `src/cm.c` by version (or keep a version 2 copy of the changed functions) so `rf_cm_decode` takes the version from `rf_decode`. Done when: the decoder still imports version 2 files and reverses them to their pixels (`KAT_FOLD` at every step of this section). Cheaper substitute: dropping version 2 support.
- [ ] Apply the variant to the version 3 path of `src/cm.c`, set `RF_VERSION` to 3 in `src/rf.h` (and the magic's fourth byte), and let `rf_decode` accept versions 2 and 3. Done when: `python3 scripts/build.py` (Windows: `python scripts/build.py`) exits 0 with no warning, and `inspect` reports `RasterFold fold version 3` on a new file.
- [ ] Write the version 3 model into `tests/rfi_ref.py` from the spec change of the next item, keeping the version 2 path. Done when: codec and oracle agree byte for byte on every round-trip case.
- [ ] Rewrite `spec/FOLD.md` as version 3, with a section stating how version 2 files differ and that decoders read both. Done when: the oracle item above was written from it.
- [ ] Keep `KAT_FOLD` as a decode-only version 2 vector and add `KAT_FOLD3`, the same image encoded as version 3, to `tests/test_correctness.py`; add version 3 palette vectors (identical payloads, version 3 headers) for the encode expectations in `tests/test_correctness.py` and `tests/test_api.py`, keeping the version 2 palette and API vectors as decode-only cases; migrate the generated C in `tests/test_artifacts.py` to the new `rf_cm_decode` signature; update the version rejection test to reject version 4 and mixed `(2,3)`/`(3,2)` headers. Done when: both known-answer cases pass, the API and artifact tests pass, and every encode expectation names a version 3 file.
- [ ] Measure `python research/ab.py size photo`, `tp`, and `synth` through the new `src/cm.c`, and run `python tests/measure_final.py`. Done when: the totals match `D01 T02 §7`'s combined figures within 0.01%, and the comparison prints PASS.
- [ ] Commit: `"codec: format version 3 carries the measured model changes"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case, both known-answer cases included; `python tests/fuzz_decode.py 2000 11` reports no crash, hang, or mismatch; the `ab.py size photo` line is quoted against 0.9712; the shipped codec's three-thread minimum-time decode ratio against JPEG XL on `measure/image.bmp` (alternating runs, thread counts stated) is quoted against the inherited 0.6 bar; mixed-version headers are refused with no completed output.

**Freeze check:** version 2 files decode unchanged: `KAT_FOLD` and every file under `build/research/fold-v2/` (the research corpus encoded by the commit before this section, kept for this check) decode to pixels identical to their sources; approval quoted from `D99 T01 §3`.

## 2. Version 3: Horizontal Strips

**Needs:** Idle machine (timing)

Three threads leave most of a 12-thread CPU idle. If `D02 T02 §1` measured the size cost of independent horizontal strips as acceptable, version 3 carries a strip count and codes each strip with fresh model state and its own three streams, so decode runs up to three threads per strip. Strips come in the same version as §1 so users see one format change, not two; if `D02 T02 §1` rejected strips, this section records that and strikes its items.

- [ ] Add the strip count to the version 3 payload (`S`, then `3*S` stream sizes, then the streams in strip order), chosen by the encoder from the image height only as `clamp(H / 512, 1, 4)`, never from the thread count. Done when: a 1672x941 image encodes with `S = 1` and a 2048x2048 image with `S = 4`.
- [ ] Run strips in parallel in `run` (`src/cm.c`), three threads per strip up to `rf_cpu_count()`, with the one-thread pipeline as the fallback. Done when: the file bytes are identical for `--threads 1`, `3`, and the default.
- [ ] Write strips into `tests/rfi_ref.py`, `spec/FOLD.md`, and `KAT_FOLD3` (regenerated if its image gets more than one strip; it does not at 16x8). Done when: codec and oracle agree on every case, including a new tall-image case with `S = 4`.
- [ ] Measure decode time with `python research/vsjxl.py -n5` on a 4-strip image and on `measure/image.bmp`. Done when: the decode ratios are quoted and the size cost is within what `D02 T02 §1` accepted.
- [ ] Commit: `"codec: version 3 codes tall images as parallel strips"`

**Test checkpoint:** `python tests/test_correctness.py` passes with the tall-image case; the one-thread and default-thread files of the tall image are byte-identical (hashes quoted); the `vsjxl.py` decode ratio is quoted.

**Freeze check:** version 2 files decode unchanged (`KAT_FOLD` and `build/research/fold-v2/`), and version 3 files written by §1 before strips existed are re-encoded, never read as strip files: §1 and §2 ship before any release, so no version 3 file without strips exists outside the development tree.

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0: build, correctness, fuzz, TODO gates green
- [ ] `python tests/test_correctness.py` exits 0 with both known-answer cases
- [ ] `python tests/measure_final.py` PASS and `python tests/benchmark.py -n 3` rerun, `benchmarks/results.md` and the README measurement table updated
- [ ] `python scripts/todo-graph.py validate` clean
