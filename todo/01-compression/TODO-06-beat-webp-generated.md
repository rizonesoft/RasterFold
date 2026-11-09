---
schema_version: 1
id: beat-webp-generated
domain: 01-compression
status: active
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
- -> XREF: D01 T01 §2 -- the palette methods, whose encoder-selected form (method 3) §3 extends (corrected 2026-11-09, see §3)
- -> XREF: D01 T03 §1 -- format version 3, which §2 extends before the 1.0 release

## Outcome

- The generated set's total and geometric mean are below lossless WebP's, and every generated image is within 1.1 times WebP's size.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no option); reporting=not-applicable (no new report); notifications=not-applicable (no new message); permissions=not-applicable (file safety unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T06 §2; reverse=applicable @ D01 T06 §2

**Adjacency rationale:** Both model changes alter what the encoder exports and the decoder imports.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Where WebP still wins | -- |  [x]   |
|   2   |   §2    | A colour-cache model in version 3 | §1, D01 T03 §1 |  [x]   |
|   3   |   §3    | Copy runs for palette images | §1, D01 T01 §2 |  [x]   |
|   4   |   §4    | Coding the palette itself for palette images | §3 |  [x]   |
|   5   |   §5    | Above-copy runs for palette images | §3 |  [x]   |
|   6   |   §6    | A neighbour-candidate colour cache in version 3 | §2 |  [ ]   |

---

## 1. Where WebP Still Wins

> **Started:** 2026-11-09T00:13:20Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Corrected 2026-11-09:** The `heldout` set this section cites does not exist yet (`research/ab.py` has no `heldout` set; it is authored by `D00 T03 §1`, whose own claim records that absence). The heldout screenshot measurement is therefore owned by `D00 T03 §2` and the Milestone 9 gate (`D05 T01 §9`), which re-measure against WebP; this section measures the `synth` generated set, which is the set §2 and §3 are designed against.
**Corrected 2026-11-09:** `docs/research.md` section 7 is now Robustness and format scope; the compression findings belong in section 4 (Compression designs) as a new subsection, next to §4.7 (exact-repeat context for screenshots).

- [x] Measure `python research/ab.py size synth -v` against WebP at the current codec, recording the codec revision, command, date, thread count, and the full `synth` total, geometric-mean, and worst-image WebP ratios. Done when: every image above 1.1 times WebP is listed, with the encoder-selected method recorded per image.
- [x] For each listed image, attribute RasterFold's bytes at the level its selected method supports: for method 1 (CM) images, the `--breakdown` mode of `D01 T05 §1` (per channel and decision kind, with the instrumented payload verified byte-identical); for palette-method images, structural attribution from the container (header, palette bytes at 3 bytes per colour, and the index stream, per `spec/FOLD.md`), because `--breakdown` does not instrument the palette coder. State that limit in the table; a forced-CM measurement is not evidence about a palette file. Done when: the table gives each listed image's RasterFold attribution beside its whole-file WebP size and difference, and any claim about which WebP mechanism (colour cache, backward references) saves the bytes is labelled a hypothesis unless the attribution itself proves it. Cheaper substitute: assuming the cause without the breakdown.
- [x] Record the findings in `docs/research.md` section 4 as a new subsection. Done when: the findings name which of §2 and §3 targets which images, and the stop condition (complete diagnostic coverage of every listed image, with supported or explicitly hypothesised causes) is met; this section ships no codec change and no compression improvement is its bar.
- [x] Commit: `"research: explain where WebP still wins"`

**Test checkpoint:** the breakdown table for every image above 1.1 times WebP: `docs/research.md` section 4.13 lists `syn_checker` 138 vs WebP 82 (1.6829, method 3, index stream 94), `syn_text` 1447 vs 1198 (1.2078, method 3, palette 753, index stream 656), `syn_diagram` 1452 vs 1228 (1.1824, method 3, palette 753, index stream 661), and `syn_odd` 75 vs 60 (1.2500, method 1, header 45, stream 30), from `python3 scripts/windows.py research/ab.py size src/cm.c synth -v` (`total=201640 webp=201248 webp_ratio=1.0019 webp_geomean=1.0007`) at codec revision `b22dec2` on 2026-11-09.

> **Verified:** 2026-11-09 | §1 | b10b5d4f990041a367fc7fd289fb91d7c23ac85d
> **Review:** `docs/reviews/d01-t06-s1.json`
## 2. A Colour-Cache Model in Version 3

> **Started:** 2026-11-09T00:27:02Z

**Needs:** ffmpeg with libjxl (benchmarks)

A context model over recently seen colours: a small hash of the last colours per position class, whose hit (the current pixel equals a cached colour) becomes a strong mixer input for G, R, and B together. Bar: the generated geometric mean at least 10% lower with the photo total no more than 0.05% worse; otherwise the negative result ships.

**Prototype form (2026-11-09, from the plan review):** the true-colour hit is not causal (the decoder cannot know the current colour before coding it), so the prototype tests the causal narrowing of the idea: per channel and per strip, a 32-sample recent-value cache over coded byte values (timestamp per byte value, refreshed on each coded sample, a value older than 32 samples is evicted, state resets with the strip's zeroed `cm_t`). The hit queried before coding is whether the context prediction byte `pred` (the pre-gate prediction the other contexts read) carries a timestamp within the last 32 samples of this channel; the hit replaces the low bit of the version 3 `k6` context (`k6 = (k6 & ~1) | hit`, row count unchanged). The cache updates in `cm_code` with the coded value, identically on encode and decode. A miss of the bar rejects this causal variant only, not colour caching in general; the stronger neighbour-candidate form the plan review named is a separate experiment if this one shows promise. Acceptance is `GM(candidate/baseline) <= 0.90` over the encoder-selected `synth` sizes and `photo` candidate total / baseline total `<= 1.0005`, both from `research/ab.py size <source> <set> -v -t1`, with per-image baseline/candidate sizes and selected methods recorded for `syn_ui`, `syn_text`, and `syn_diagram` (the many-colour screen images §1 named).

- [x] Prototype the model in `build/research/try.c` and measure `synth`, `photo`, and `tp`. Done when: the summary lines are quoted against the bar.
- [x] ~~If accepted, port it into the version 3 path of `src/cm.c` with `tests/rfi_ref.py`, `spec/FOLD.md`, and `KAT_FOLD3`.~~ Not accepted: the prototype missed the `synth` bar (candidate/baseline GM `1.0000` against `<= 0.90`; `photo` `0.99997` within its `<= 1.0005` bar), so nothing was ported, per the negative-result rule. Measured 2026-11-09, recorded in `docs/research.md` section 3.2.
- [x] Commit: `"codec: a colour-cache model for screen content"`

**Test checkpoint:** the generated geometric mean candidate/baseline is `1.0000` (`synth` total `201640` -> `201640`, all 11 encoder-selected sizes identical) against the `GM <= 0.90` bar, and `photo` is `30634873` -> `30634088` (`0.99997`, bar `<= 1.0005`), from `python3 scripts/windows.py research/ab.py size <source> <set> -v -t1` on 2026-11-09; negative result, no port, so `python tests/test_correctness.py` owes no new case.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending the unreleased version 3.

> **Verified:** 2026-11-09 | §2 | 1d765a439f387835444b230eac0e2a5fdd84f51c
> **Review:** `docs/reviews/d01-t06-s2.json`
## 3. Copy Runs for Palette Images

> **Started:** 2026-11-09T00:38:30Z

For method 3 (extended palette, the palette method the encoder actually selects; `RF_METHOD_PAL2` in `src/pal.c`), a run token that copies a span of indices from the row above (at offsets -2 to 2) or from the left, coded before the per-pixel decisions. Bar: palette images at least 20% smaller, no photo affected.

**Corrected 2026-11-09:** this section originally named method 2, but the encoder never selects method 2 (`src/fold.c` selects `RF_METHOD_PAL2`, file method 3, when the palette wins, and every palette file measured in §1 is method 3). The run token therefore belongs in the method 3 path of `src/pal.c`; method 2 files remain decode-only legacy. This changes what the encoder writes for method 3, so the four format duties in item 2 apply to method 3.

**Corrected 2026-11-09:** the `heldout` set does not exist yet (authored by `D00 T03 §1`); its screenshots are owned by `D00 T03 §2`. This section measures the palette images of `synth`.

**Prototype form (2026-11-09, from the plan review):** the prototype is `build/research/try_pal.c`, a copy of `src/pal.c` with the token always on for method 3 (measured through `research/ab.py`, which verifies round trips). It tests left-copy runs only, in this exact grammar: after a D1 hit (`idx == a`) in a flat pattern (`(pat & 31) == 31`, all five neighbour-equality bits set), a run flag (adaptive counter per pattern) and, for a same-colour run of total length at least 8, a unary code of `runlen - 8` ones plus a terminating zero (adaptive counters by unary position, index capped at 31), so seven skipped pixels are implicit in a taken run. Length means the current pixel plus the explicitly counted skipped pixels; the decoder checks the remaining row width before filling, does tail/exhaustion checks inside the unary loop, and keeps exact stream-consumption validation. Every eligible D1 hit routes through the token: the encoder's `prev_key0` fast-skip is removed so no path bypasses it, `count == 1` is exempt on both sides (its loop already codes one repeated index), and the two-colour D1-miss shortcut is preserved. Skipped pixels fill every entry of the three-row neighbour buffer, do not update D1 counters, and leave `hit` set. A miss of the bar rejects this token, threshold, and length code only, not above-copy runs (offsets -2 to 2) or copy runs in general. Acceptance is the six palette-coded `synth` images of §1 (`syn_solid` 61, `syn_checker` 138, `syn_pixel` 52, `syn_ui` 1410, `syn_text` 1447, `syn_diagram` 1452; total 4560) totalling at most 3648 bytes (20% smaller) with per-image methods recorded, and no `photo` image changed in size (any palette-coded photo is quoted per image).

- [x] Prototype the run token as above and measure the palette images of `synth` and the `photo` set. Done when: the sizes are quoted against the bar.
- [x] ~~If accepted, make it part of method 3 with `tests/rfi_ref.py`, `spec/FOLD.md`, and `KAT_PAL2_3` (the version 3, method 3 fixture in `tests/test_correctness.py`), including version-aware palette dispatch in `src/fold.c` (the decoder currently receives the method but not the format version, and version 2 method 3 files exist: `KAT_PAL2` must keep decoding through the old grammar in both the C and the oracle, on both the RGB and strided BMP paths), a fixture that actually exercises a run, targeted malformed-run rejection tests (overlong, unterminated, and truncated tokens with valid container checksums), and seeded fuzzing.~~ Not accepted: the prototype measured 7220 bytes against the 4560-byte baseline (acceptance at most 3648), so nothing was ported, per the negative-result rule. Measured 2026-11-09, recorded in `docs/research.md` section 3.2.
- [x] Commit: `"codec: copy runs for palette images"`

**Test checkpoint:** the six palette-coded `synth` images total `7220` candidate bytes against the `4560`-byte baseline and the at-most-`3648` acceptance total (`syn_checker` `138 -> 506`, `syn_ui` `1410 -> 2175`, `syn_text` `1447 -> 2204`, `syn_diagram` `1452 -> 2222`), and `photo` per-image sizes are identical to baseline on all 39 images (candidate and baseline `photo -v` logs compared image by image; no palette-coded photo changed size; every palette `synth` image is method 3, from the §1 `.fold` parse (`syn_checker` 2 colours, `syn_text`/`syn_diagram` 251 colours, `syn_ui` 240 colours, `syn_solid` 1 colour (exempt), `syn_pixel` method 3 at 52 bytes, each encoded file's header byte 16 read directly)), from `python3 scripts/windows.py research/ab.py size build/research/try_pal.c synth -v -t1` and `photo -t1` on 2026-11-09; negative result, no port, so `python tests/test_correctness.py` owes no new case.

**Freeze check:** method 3 is part of the unreleased format version 3, so its files may change; `KAT_FOLD` (version 2, method 1) and the method 2 fixtures decode unchanged.

> **Verified:** 2026-11-09 | §3 | 9644892a45c33501af8a9905e3e32ac8ad5fff2f
> **Review:** `docs/reviews/d01-t06-s3.json`
## 4. Coding the Palette Itself for Palette Images

> **Started:** 2026-11-09T08:25:00Z

**Origin:** discovered run=f64ec873f271 2026-11-09 -- Milestone 8 gate (D05 T01 §8) missed its target on 2026-11-09; this is follow-up 1 of up to three under the missed-milestone policy (D99 T01 §4), required by that gate's acceptance bar.

**Needs:** ffmpeg with libjxl (benchmarks)

The palette field is the largest single component on the many-colour generated images: `syn_text` and `syn_diagram` each pay 753 raw palette bytes of 1447 and 1452 total (52%), and `syn_ui` pays 720 of 1410 (`docs/research.md` section 4.13). Section 4.13 named this follow-up in advance: if §2 and §3 both landed and the palette bytes still dominate, coding the palette itself (delta or entropy coding of the triples) is the next experiment. Both landed as negative results and the palette bytes still dominate. Hypothesis: palette triples in first-seen order are predictable (sorted-ish screens, repeated channels), so coding each channel byte with the method 3 range coder under neighbour-palette contexts (previous entry's same channel, channel differences) costs materially less than 3 raw bytes per colour. Non-goal: changing the index stream (that is §5) or the CM coder (that is §6). Prior art: lossless WebP codes its palette with a colour cache and delta coding of the palette entries; JPEG XL modular mode entropy-codes the palette channel. Baseline measured 2026-11-09 at `8160619`: the six palette-coded `synth` images total 4560 (`syn_solid` 61, `syn_checker` 138, `syn_pixel` 52, `syn_ui` 1410, `syn_text` 1447, `syn_diagram` 1452) and `photo` totals 30634873, from `python3 scripts/windows.py research/ab.py size src/cm.c synth -v -t1`.

**Prototype form:** in `build/research/try_pal.c` (a copy of `src/pal.c`), method 3 writes the palette as range-coded channel deltas instead of raw triples: entry 0 raw, each later entry coded as signed differences from the previous entry per channel under adaptive counters shared with the existing palette coder's update rule, in the version 3 palette field only; the decoder reconstructs entries in order and rejects a count or stream that overruns the field. Exactly this one grammar is tested; sorted-palette remapping and per-channel plane coding are different experiments. Acceptance is the six palette-coded `synth` images totalling at most 4104 bytes (10% below the 4560 baseline) with `syn_text` at most 1318 and `syn_diagram` at most 1351 (each within 1.1 times its WebP size), and no `photo` image changed in size beyond +0.05% on the total (any palette-coded photo is quoted per image).

**Corrected 2026-11-09 (plan review `docs/reviews/01-compression/D01-T06-s4.md`):** two P2 gaps folded in: (1) before any port is accepted, the measurement is confirmed on `tp` and quoted as full-set totals, geometric means, and worst images, with alternating baseline/candidate timing at `-t1` and `-t3` for an accepted candidate (an early size-bar miss still terminates as the negative result below); (2) the version-aware dispatch wiring names `src/rf.h` (the `rf_pal_decode` declaration, which today takes only the method), its definition in `src/pal.c`, and both callers in `src/fold.c` (RGB and strided BMP paths), so version 2 and version 3 method 3 select different palette grammars consistently.

- [x] Prototype the palette coding as above in `build/research/try_pal.c` and measure the palette images of `synth` and the `photo` set. Done when: the sizes are quoted against the bar.
- [x] ~~If accepted, make it part of method 3 in version 3 with `tests/rfi_ref.py` (written from the spec text), `spec/FOLD.md`, and `KAT_PAL2_3`, including version-aware palette dispatch in `src/fold.c` (version 2 method 3 files keep the old grammar: `KAT_PAL2` decodes unchanged through C and oracle on the RGB and strided BMP paths), a fixture whose palette coding actually engages, targeted malformed-palette rejection tests (overlong count, truncated delta stream, valid container checksums), and seeded fuzzing.~~ Not accepted: the prototype measured 4858 bytes against the 4560-byte baseline (acceptance at most 4104), so nothing was ported, per the negative-result rule. Measured 2026-11-09, recorded in `docs/research.md` section 3.
- [x] Commit: `"codec: code the palette itself for palette images"`

**Test checkpoint:** the six palette-coded `synth` images total `4858` candidate bytes against the `4560`-byte baseline and the at-most-`4104` acceptance total (`syn_solid` `61 -> 70`, `syn_checker` `138 -> 146`, `syn_pixel` `52 -> 60`, `syn_ui` `1410 -> 1497`, `syn_text` `1447 -> 1544` against its `1318` bar, `syn_diagram` `1452 -> 1541` against its `1351` bar), and `photo` per-image sizes are identical to baseline on all 39 images (totals `30634873` = `30634873`), from `python3 scripts/windows.py research/ab.py size build/research/try_pal.c synth -v -t1` and `photo -v -t1` on 2026-11-09; negative result, no port, so `python tests/test_correctness.py` owes no new case.

**Freeze check:** method 3 is part of the unreleased format version 3, so its files may change; `KAT_FOLD` (version 2, method 1) and the method 2 fixtures decode unchanged.

> **Verified:** 2026-11-09 | §4 | 593ebf79ce0e2ce0b388dbf9bae0b7111aec9f08
> **Review:** `docs/reviews/d01-t06-s4.json`
## 5. Above-Copy Runs for Palette Images

> **Started:** 2026-11-09T08:38:00Z

**Origin:** discovered run=f64ec873f271 2026-11-09 -- Milestone 8 gate (D05 T01 §8) missed its target on 2026-11-09; this is follow-up 2 of up to three under the missed-milestone policy (D99 T01 §4), required by that gate's acceptance bar.

**Needs:** ffmpeg with libjxl (benchmarks)

§3 measured a left-copy run token and rejected that variant only: "above-copy tokens and other length codes are untested" (`docs/research.md` section 3). The index streams §3 targeted repeat from above as often as from the left (`syn_text` above repeat 97.1%, `syn_diagram` 97.1%, `syn_checker` left/above 93.8%, section 4.13), and `syn_checker` (138 vs WebP 82) cannot be reached by palette coding because its palette field is only 12 bytes. Hypothesis: a run token copying indices from the row above at offsets -2 to 2 captures the vertical structure the left-copy token could not. Non-goal: re-testing §3's left-copy grammar unchanged, or changing the palette field (that is §4). Baseline measured 2026-11-09 at `8160619`: the six palette-coded `synth` images total 4560 and `photo` totals 30634873, from `python3 scripts/windows.py research/ab.py size src/cm.c synth -v -t1`.

**Prototype form:** in `build/research/try_pal.c`, method 3 gains an above-copy token in the index stream: where the coming indices equal the row above's indices at a fixed offset in -2..2 for at least 8 pixels, one flag decision (adaptive counter per neighbour pattern, as in §3) plus a coded offset (5 adaptive counters) and the §3 unary length (`runlen - 8`, counters by position capped at 31) replace the per-pixel decisions; the decoder checks the remaining row width and the row-above bounds before filling, does tail and exhaustion checks inside the unary loop, and keeps exact stream-consumption validation. Every D1-hit path routes through the token exactly as §3 specified (no `prev_key0` bypass, `count == 1` exempt, two-colour shortcut preserved); skipped pixels fill the three-row neighbour buffer and do not update D1 counters. Acceptance is the six palette-coded `synth` images totalling at most 4104 bytes with `syn_checker` at most 114 (within 1.1 times WebP or 32 bytes larger, whichever allows more), and no `photo` image changed in size (any palette-coded photo is quoted per image).

**Corrected 2026-11-09 (plan review `docs/reviews/01-compression/D01-T06-s5.md`):** three P2 gaps folded in: (1) before any port is accepted, the measurement is confirmed on `tp` and quoted as full-set totals, geometric means, and worst images, with alternating baseline/candidate timing at `-t1` and `-t3` for an accepted candidate (an early size-bar miss still terminates as the negative result below); (2) the exact grammar is fixed here: the token is offered only after a D1 hit on rows below the first; its flag is coded on the D1 run counter `pat * 2 + 1` (the counter section 3's prototype used); the offset in -2..2 is coded by successive elimination in the order -2, -1, 0, 1, 2 under four adaptive counters, the last offset being the terminal case when the first four eliminate out; the unary length codes `runlen - 8` where `runlen` includes the already-coded D1 pixel, so seven or more pixels are skipped; skipped pixels fill the three-row neighbour buffer, update no D1 counters, and leave `hit` at 1 for the next coded pixel; a miss rejects this routing, shared counter, threshold, and length code only; (3) the port item's tests cover all five offsets, a run of length exactly 8, source-row left and right bounds, first-row refusal (no token is offered on row 0), a copied span containing colour transitions followed by ordinary coding, and both the RGB and strided BMP output paths.

- [x] Prototype the above-copy token as above in `build/research/try_pal.c` and measure the palette images of `synth` and the `photo` set. Done when: the sizes are quoted against the bar.
- [x] ~~If accepted, make it part of method 3 in version 3 with `tests/rfi_ref.py` (written from the spec text), `spec/FOLD.md`, and `KAT_PAL2_3`, including version-aware palette dispatch in `src/fold.c` (`KAT_PAL2` decodes unchanged through C and oracle on both output paths), a fixture that actually exercises an above-copy run, targeted malformed-run rejection tests (overlong, unterminated, and truncated tokens with valid container checksums), and seeded fuzzing. If not accepted, strike this item with a pointer to the stamp. Done when: the four format places agree or the strike is recorded.~~ Not accepted: the prototype measured 9973 bytes against the 4560-byte baseline (acceptance at most 4104), so nothing was ported, per the negative-result rule. Measured 2026-11-09, recorded in `docs/research.md` section 3.
- [x] Commit: `"codec: above-copy runs for palette images"`

**Test checkpoint:** the six palette-coded `synth` images total `9973` candidate bytes against the `4560`-byte baseline and the at-most-`4104` acceptance total (`syn_solid` `61 -> 61`, `syn_checker` `138 -> 3259` against its `114` bar, `syn_pixel` `52 -> 52`, `syn_ui` `1410 -> 2175`, `syn_text` `1447 -> 2204`, `syn_diagram` `1452 -> 2222`), and `photo` per-image sizes are identical to baseline on all 39 images (totals `30634873` = `30634873`), from `python3 scripts/windows.py research/ab.py size build/research/try_pal.c synth -v -t1` and `photo -v -t1` on 2026-11-09; negative result, no port, so `python tests/test_correctness.py` owes no new case.

**Freeze check:** method 3 is part of the unreleased format version 3, so its files may change; `KAT_FOLD` (version 2, method 1) and the method 2 fixtures decode unchanged.

> **Verified:** 2026-11-09 | §5 | 6c8beb941ac03fcc7f73b7744f7f8219ff866183
> **Review:** `docs/reviews/d01-t06-s5.json`
## 6. A Neighbour-Candidate Colour Cache in Version 3

> **Started:** 2026-11-09T08:55:00Z

**Origin:** discovered run=f64ec873f271 2026-11-09 -- Milestone 8 gate (D05 T01 §8) missed its target on 2026-11-09; this is follow-up 3 of up to three under the missed-milestone policy (D99 T01 §4), required by that gate's acceptance bar.

**Needs:** ffmpeg with libjxl (benchmarks)

§2 tested a causal predicted-byte colour cache and rejected that variant only: "the neighbour-candidate form is a separate experiment" (`docs/research.md` section 3). The generated set's CM-coded images are the ones §4 and §5 cannot reach, and the gate's geometric mean (1.0007) needs every class moving. Hypothesis: the current colour often equals a neighbour's actual colour (W, N, NE, or the W+N-NE gradient candidate), so a hit decision naming which candidate matches, coded before the channel residuals, is stronger evidence than §2's predicted-byte timestamp. Non-goal: re-testing §2's timestamp cache, or touching the palette methods (those are §4 and §5). Prior art: lossless WebP's colour cache indexes recently used colours; CALIC and JPEG-LS use the gradient candidate as a predictor. Baseline measured 2026-11-09 at `8160619`: `synth` total 201640 (WebP 201248, geomean ratio 1.0007), `photo` 30634873, from `python3 scripts/windows.py research/ab.py size src/cm.c synth -v -t1`. This differs from §2 in the hit definition (actual neighbour colours and the gradient candidate, not the prediction byte's cache timestamp) and in coding all three channels from one hit.

**Prototype form:** in `build/research/try.c` (a copy of `src/cm.c`), before the G residual at each pixel after the first row and column, one hit decision under an adaptive counter per strip: hit when the pixel's colour equals W's, N's, NE's, or the clamped W+N-NE candidate's colour, followed on a hit by a 2-bit candidate index under adaptive counters; on a hit all three channel residuals are skipped and the models see the colour exactly as if it had been coded. State resets per strip exactly as the version 3 models do. Acceptance is the encoder-selected `synth` candidate/baseline geometric mean at most 0.90 with the `photo` total at most 1.0005 times baseline, per-image sizes and selected methods recorded for the CM-coded `synth` images.

**Corrected 2026-11-09 (plan review `docs/reviews/01-compression/D01-T06-s6.md`, recorded late after round 1 found it missing):** the measured grammar is the narrowed above-row form, not the W-inclusive form in the prototype paragraph above. Candidates, in order: N, NE, NW, and the above-row gradient candidate (per channel `clamp(2 * N - NE, 0, 255)`); the first matching candidate wins. The token is offered only where `y >= 1`, `x >= 1`, and `x + 1 < w` within the strip. The hit flag and two candidate-index bits sit on the G channel's stream under dedicated adaptive counters (initialised at 32768, palette-style minimum-step update, count capped at 60), reset at each strip's start and never inherited across strips. On a hit each channel sample runs its full model update with the candidate colour (context counters and mixer weights update as if coded) while the range coder emits and consumes no residual bits (a ghost pass). A miss rejects this above-row candidate set, offer region, and counter scheme only. The prototype implements the one-thread pipelined driver, the driver the `-t1` measurement uses.

- [x] Prototype the neighbour-candidate cache as above in `build/research/try.c` and measure `synth`, `photo`, and `tp`. Done when: the summary lines are quoted against the bar.
- [x] ~~If accepted, port it into the version 3 path of `src/cm.c` with `tests/rfi_ref.py` (written from the spec text), `spec/FOLD.md`, and `KAT_FOLD3`, plus: the cross-channel scheduling step for the threaded channel driver (candidate availability from the completed row above, publication of hit decisions and candidate colours to the R and B workers, abort propagation so no worker waits forever, and the `spec/FOLD.md` scheduling contract updated to match), byte identity and identical decoding across thread counts and strip boundaries, fixtures exercising all four candidate hits, a miss, duplicate candidates, gradient clamping at 0 and 255, first and last columns, strip-local first rows, and a hit followed by ordinary residual coding, oracle byte identity on those fixtures, truncated hit-flag and candidate-index refusal, threaded abort completion, and seeded fuzzing. If not accepted, strike this item with a pointer to the stamp. Done when: the four format places agree or the strike is recorded.~~ Not accepted: the prototype measured a `synth` geometric mean of 1.0298 against the at-most-0.90 bar and a `photo` ratio of 1.0110 against at most 1.0005, so nothing was ported, per the negative-result rule. Measured 2026-11-09, recorded in `docs/research.md` section 3.
- [x] Commit: `"codec: a neighbour-candidate colour cache for screen content"`

**Test checkpoint:** the generated geometric mean candidate/baseline is `1.0298` (`synth` total `201640 -> 201682`, candidate `webp_ratio=1.0022 webp_geomean=1.0305 webp_worst=1.6829 (syn_checker)` against the baseline's `1.0019 / 1.0007 / 1.6829`; `syn_stripes` `113 -> 123`, `syn_gradient` `119 -> 133`, `syn_wide` `133 -> 151`, the other eight encoder-selected sizes identical) against the at-most-`0.90` bar; `photo` totals `30634873 -> 30972555` (ratio `1.0110` against at most `1.0005`; candidate `webp_geomean=0.8999`, worst vs JPEG XL e7 `1.2207 (ic_artificial)` against the baseline's `webp_geomean=0.8834`, `0.9934 (kodim17)`); and `tp` totals `6598954 -> 6681672` (candidate `webp_geomean=0.8867`, worst `1.0323 (tp_flower_foveon_crop)` against the baseline's `0.8723` and `0.9897`), from `python3 scripts/windows.py research/ab.py size build/research/try.c <set> -v -t1` on 2026-11-09; negative result, no port, so `python tests/test_correctness.py` owes no new case.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending the unreleased version 3.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
