---
schema_version: 1
id: beat-webp-generated
domain: 01-compression
status: draft
title: "TODO-06 -- Beat WebP on Generated Images"
depends_on: []
frozen: true
track: C6
---

# TODO-06 -- Beat WebP on Generated Images

> **Goal:** On generated and screen content (flat fills, gradients, UI, text, diagrams, pixel art) RasterFold is smaller than lossless WebP in total and in geometric mean, and on every image within 1.1 times WebP or at most 32 bytes larger, whichever allows more (Milestone 8's target as revised by the operator on 2026-10-30, `D99 T01 §4`).

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** On the generated set RasterFold's geometric mean is 3.66 times WebP's (2026-09-29, before the palette method and the match model). WebP's advantages are its colour cache (recently used colours coded as a short index) and its 2D backward references; milestones 3 and 5 close part of the gap first, so §1 re-measures before anything is designed.
<!-- claim: count "colou?r[ _-]?cache" src/cm.c = 0 -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- section 8 (lossless WebP), section 7 item 4
- -> XREF: D01 T01 §2 -- method 2, which §3 extends
- -> XREF: D01 T03 §1 -- format version 3, which §2 extends before the 1.0 release

## Outcome

- The generated set's total and geometric mean are below lossless WebP's, and every generated image is within 1.1 times WebP's size.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no option); reporting=not-applicable (no new report); notifications=not-applicable (no new message); permissions=not-applicable (file safety unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T06 §2; reverse=applicable @ D01 T06 §2

**Adjacency rationale:** Both model changes alter what the encoder exports and the decoder imports.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Where WebP still wins | -- |  [ ]   |
|   2   |   §2    | A colour-cache model in version 3 | §1, D01 T03 §1 |  [ ]   |
|   3   |   §3    | Copy runs for palette images | §1, D01 T01 §2 |  [ ]   |

---

## 1. Where WebP Still Wins

> **Started:** 2026-11-09T00:13:20Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Corrected 2026-11-09:** The `heldout` set this section cites does not exist yet (`research/ab.py` has no `heldout` set; it is authored by `D00 T03 §1`, whose own claim records that absence). The heldout screenshot measurement is therefore owned by `D00 T03 §2` and the Milestone 9 gate (`D05 T01 §9`), which re-measure against WebP; this section measures the `synth` generated set, which is the set §2 and §3 are designed against.
**Corrected 2026-11-09:** `docs/research.md` section 7 is now Robustness and format scope; the compression findings belong in section 4 (Compression designs) as a new subsection, next to §4.7 (exact-repeat context for screenshots).

- [ ] Measure `python research/ab.py size synth -v` against WebP at the current codec, recording the codec revision, command, date, thread count, and the full `synth` total, geometric-mean, and worst-image WebP ratios. Done when: every image above 1.1 times WebP is listed, with the encoder-selected method recorded per image.
- [ ] For each listed image, attribute RasterFold's bytes at the level its selected method supports: for method 1 (CM) images, the `--breakdown` mode of `D01 T05 §1` (per channel and decision kind, with the instrumented payload verified byte-identical); for palette-method images, structural attribution from the container (header, palette bytes at 3 bytes per colour, and the index stream, per `spec/FOLD.md`), because `--breakdown` does not instrument the palette coder. State that limit in the table; a forced-CM measurement is not evidence about a palette file. Done when: the table gives each listed image's RasterFold attribution beside its whole-file WebP size and difference, and any claim about which WebP mechanism (colour cache, backward references) saves the bytes is labelled a hypothesis unless the attribution itself proves it. Cheaper substitute: assuming the cause without the breakdown.
- [ ] Record the findings in `docs/research.md` section 4 as a new subsection. Done when: the findings name which of §2 and §3 targets which images, and the stop condition (complete diagnostic coverage of every listed image, with supported or explicitly hypothesised causes) is met; this section ships no codec change and no compression improvement is its bar.
- [ ] Commit: `"research: explain where WebP still wins"`

**Test checkpoint:** the breakdown table for every image above 1.1 times WebP.

## 2. A Colour-Cache Model in Version 3

**Needs:** ffmpeg with libjxl (benchmarks)

A context model over recently seen colours: a small hash of the last colours per position class, whose hit (the current pixel equals a cached colour) becomes a strong mixer input for G, R, and B together. Bar: the generated geometric mean at least 10% lower with the photo total no more than 0.05% worse; otherwise the negative result ships.

- [ ] Prototype the model in `build/research/try.c` and measure `synth`, `photo`, and `tp`. Done when: the summary lines are quoted against the bar.
- [ ] If accepted, port it into the version 3 path of `src/cm.c` with `tests/rfi_ref.py`, `spec/FOLD.md`, and `KAT_FOLD3`. Done when: codec and oracle agree on every case and the decoder imports every file back to identical pixels (the reverse of the export).
- [ ] Commit: `"codec: a colour-cache model for screen content"`

**Test checkpoint:** `python tests/test_correctness.py` passes; the generated geometric mean against WebP quoted before and after.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending the unreleased version 3.

## 3. Copy Runs for Palette Images

For method 2, a run token that copies a span of indices from the row above (at offsets -2 to 2) or from the left, coded before the per-pixel decisions. Bar: palette images at least 20% smaller, no photo affected.

**Corrected 2026-11-09:** the `heldout` set does not exist yet (authored by `D00 T03 §1`); its screenshots are owned by `D00 T03 §2`. This section measures the palette images of `synth`.

- [ ] Prototype the run token in the method 2 encoder and decoder (`src/pal.c`) behind a research switch, and measure the palette images of `synth`. Done when: the sizes are quoted against the bar.
- [ ] If accepted, make it part of method 2 with `tests/rfi_ref.py`, `spec/FOLD.md`, and `KAT_PAL`. Done when: codec and oracle agree.
- [ ] Commit: `"codec: copy runs for palette images"`

**Test checkpoint:** `python tests/test_correctness.py` passes; the palette images' sizes against WebP quoted before and after.

**Freeze check:** method 2 is unreleased until 1.0, so its files may change; `KAT_FOLD` (version 2, method 1) decodes unchanged.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
